# 进程存活排查：从「在不在」到「在不在干活」—— 五层阶梯与实战复盘

> 产自 2026-09-12 rag_bench #42 验收 run 2 的现场排查（WSL 转发饿死复发）。
> 配套实验场：`docs/process_liveness_lab.sh`（每个场景一个活体标本，可复现、可把玩）。

## 0. 核心问题

排查网络程序时，正确的问题不是「进程还活着吗」，而是 **「它在不在干活」**。
存在但不干活的进程（活着、却不产出、也不报错）才是排查的难点。

整条排查阶梯就是在回答这个问题的细分：

```
进程存在吗？          kill -0 / ps
  ↓ 活
在干活吗？            CPU ticks 两采样差值
  ↓ 不涨（state=S）
卡在哪儿？            wchan / strace 采样
  ↓ poll/socket 等网络
网络通吗？            ss -tnp 看连接形态
  ├─ ESTAB            → 健康，只是慢
  ├─ SYN-SENT 固化    → 对照组 curl 新进程：
  │                      新也挂 = 服务端/端口问题
  │                      新能通 = 转发层饿死存量进程（WSL 这坑）→ 杀重跑
  └─ CLOSE-WAIT 堆积  → fd 泄漏 → lsof / /proc/<pid>/fd
```

两条经验法则先立在这里：

1. **单次快照几乎不能定论，两次采样的差值才是信息**——CPU 趋势、
   SYN-SENT 消散还是固化、fd 数量走势，全是差值问题。
2. **state=S 对网络程序是常态不是病症**。健康的服务器/客户端大部分时间
   都在睡（等事件）。要问的不是「它在睡」，而是 wchan/strace 给出的
   「它在等什么」。

---

## 1. 第0层：存在性

```bash
ps aux | grep rag_bench | grep -v grep   # -v grep：否则 grep 命中自己（经典乌龙）
kill -0 <pid> && echo alive              # 脚本判活：只探测不真发信号
```

ps 里值得读的三个字段：**%CPU**（在烧 CPU 吗）、**TIME**（累计 CPU 时间，
第2层的主角）、**STAT**（状态字母，第1层）。

## 2. 第1层：进程状态字母（`ps` STAT / `/proc/<pid>/stat` 第3字段）

| 字母 | 含义 | 网络程序里的解读 |
|---|---|---|
| `R` | 正在 CPU 上跑 | 健康 |
| `S` | 可中断睡眠，等事件 | **正常**——等 socket 可读就是这个态 |
| `D` | 不可中断睡眠 | **危险**——通常卡在磁盘/NFS，连 kill 都杀不动 |
| `Z` | 僵尸 | 已死等收尸；排查方向转向父进程（为何不 wait） |

实操：

```bash
awk '{print "state:", $3, "ppid:", $4}' /proc/<pid>/stat
cat /proc/<pid>/wchan        # 隐藏宝石：睡眠时给出内核里的等待函数名
```

wchan 把「在睡」细化成「在等什么」：`do_sys_poll` = poll 循环等 socket
（httplib 系的常态）；`hrtimer_nanosleep` = sleep 定时器；`sk_wait_data`
= 阻塞 recv。零 CPU + 有意义的 wchan = 健康等待；零 CPU + 空洞的 wchan
才可疑。

## 3. 第2层：CPU 时间趋势（两采样法）

单次采样只能说明「过去干过活」。隔几秒采两次 `/proc/<pid>/stat` 的
`utime+stime`（第14+15字段，100 ticks/s 制）看差值：

```bash
a=$(awk '{print $14+$15}' /proc/<pid>/stat); sleep 5
b=$(awk '{print $14+$15}' /proc/<pid>/stat); echo $((b-a))
```

- 差值显著非零 → 在干活（CPU 型工作）
- 差值为零 → **不能直接定罪**：可能在健康地等网络（配合第3/4层判），
  也可能挂死。2026-09-12 现场：CPU 差 0 + wchan=do_sys_poll，两种解释
  （等慢回包 vs 连接已死傻等超时）无法在这一层分开，往下走。

## 4. 第3层：连接状态词汇表（`ss -tnp`，WSL 案例的主战场）

`-p` 才能看到「哪条连接属于哪个进程」。四个状态覆盖 90% 场景：

- `ESTAB`：连接活着——正常
- `SYN-SENT`：**发出握手没人理**。回环上持续超过一秒就是异常；
  正常回环握手这个态只存在微秒级
- `CLOSE-WAIT`：**对方已关、我方没关**——少量是时序残尾，
  堆积 = fd 泄漏
- `CLOSING`：双方同时关，正常几秒内消散，堆积说明残尾排不出去

**SYN-SENT 的关键细分**：瞬时出现 ≠ 必死。SYN 重传按 1s→3s→7s 退避，
转发层「跛行」时连接会靠重传挤过去（源端口在变 = 在换连接重试 = 进程
仍在前进）；**同一连接固化不动**才是死透。多采几次看消散还是固化。

## 5. 第4层：strace —— 「此刻到底在干嘛」的终极答案

wchan 只给一个函数名，strace 直接吐系统调用流。对一个怀疑卡死的进程，
采样三秒看它停在哪儿：

```bash
timeout 3 strace -p <pid>
```

2026-09-12 现场抓到的一段（rag_bench 的一次健康 embed 往返，逐行可对回
我们的代码）：

```
setsockopt(SO_RCVTIMEO=60s, SO_SNDTIMEO=5s)        ← HttpClient 的 socket 级超时
poll(POLLOUT) → sendto("POST /api/embed")          ← 连接建立、请求发出
poll(POLLIN, 60000) → recvfrom("HTTP/1.1 200 OK")  ← 等回包，60s 兜底
recvfrom × 2                                        ← 分块读完 body
shutdown → close                                    ← 一次性连接，用完即弃
```

排卡死时的判读：反复停在 `poll/recvfrom` 等一个 fd = 在等网络（往第3层
走）；停在 `futex` = 等线程锁（死锁方向）；停在 `read` 某个普通文件 fd =
等磁盘/NFS（D state 风险）；完全无系统调用输出 = 纯 CPU 循环或已死。

## 6. 第5层：fd 视角（lsof / `/proc/<pid>/fd`）—— 泄漏诊断

当 ss 里 CLOSE-WAIT 开始堆积时：

```bash
ls /proc/<pid>/fd | wc -l    # 一次性连接的程序应稳定在小数值波动
lsof -p <pid>                # fd → 文件/socket 的映射
```

健康的模式是 fd 数量**恒定波动**（开一条连接 +1，close 后 -1）。
只涨不降 = 泄漏，最终撞 `ulimit -n`，症状突变为 "Too many open files"。

---

## 7. 实战复盘：2026-09-12 rag_bench run 2（WSL 转发饿死复发）

背景：#42 验收第二跑。bench 每查询一次一次性 embed 连接（约 4500 次/跑）；
Ollama 在 Windows 宿主侧，WSL 内 localhost:11434 经转发层抵达。完整诊断链
按层走了一遍，每一层都留下了真实读数：

| 层 | 命令 | 现场读数 | 判读 |
|---|---|---|---|
| 0 | `ps aux` | 进程在，%CPU 24% | 活 |
| 1 | `/proc/pid/stat` | state=S, Threads=1 | 睡——常态 |
| 1 | `cat /proc/pid/wchan` | `do_sys_poll` | 在 poll 等 socket |
| 2 | CPU 两采样 | 5s 差 0 → 10s 差 23 | 时零时有——跛行 |
| 3 | `ss -tnp` | `fd=3` 卡 SYN-SENT，**源端口三次采样在变** | 换连接重试中，非单连接固化 |
| 3 | 对照组 | 新进程 curl 0.4ms 秒连 + 200 | **转发层只饿存量进程**（确诊） |
| 4 | `strace -p` | 完整 200 OK 往返可观测 | 部分查询在正常完成 |

处置决策也值得复盘：**没有杀进程，而是让它跑完**。理由——指标本身就是
判别器：阶梯 dense/hybrid 每查询一次 embed，任何一次连接真失败都会记
miss、recall 应声下跌。run 2 指标与 run 1 逐位相同 = 缓存没坏 + 连接全
健康，一次验收两个结论；不同 = 才需要杀掉重跑。

对照 2026-09-10 首例（见 memory: reference-wsl-localhost-forwarding-wedge）
的差异：首例是 CPU **零**增量 + SYN-SENT 固化（硬饿死，杀重跑）；本次是
低增量 + SYN-SENT 流动（软跛行，重传能挤过，可以等）。同一个病，
**严重度看「固化 vs 流动」**，处置随严重度分级——这就是经验法则 1
（差值才是信息）的具象。

预防方向（未实施，首例复盘时记录）：批量 embed 把连接数砍 64×、
http 层长连接复用、或 Ollama 移入 WSL 内跑。

---

## 8. 实验场使用说明

```bash
cd my_agent/docs
./process_liveness_lab.sh          # 全部场景连跑，每场自动清理
./process_liveness_lab.sh hold A   # 场景A保持存活，手动把玩（ss/strace/…）
./process_liveness_lab.sh C E      # 挑着跑
```

| 场景 | 制造的形态 | 预期读数（验证你看到的和讲义一致） |
|---|---|---|
| A 健康等待 | 客户端阻塞在 `recv` | state=S、wchan=sk_wait_data 类、ESTAB、CPU 差 0 |
| B 繁忙计算 | 纯 CPU 死循环 | state=R、CPU 差显著非零 |
| C SYN-SENT 楔死 | `listen(1)` 永不 accept，4 客户端排队 | 前 1 个 ESTAB、其余 SYN-SENT 固化、客户端 CPU 近零 |
| D 僵尸 | 子退出、父 sleep 不 wait | `ps --ppid` 见 state=Z 的 (true) |
| E CLOSE-WAIT 堆积 | 服务端 accept 即 close，客户端攥 5 条 socket | 客户端名下 5 条 CLOSE-WAIT、fd 只增不减 |

场景C的诚实标注：本机模拟的是「accept 队列满 → 对所有来者一视同仁饿死」；
真实 WSL 案例更刁钻（只饿存量进程、新进程畅通），那种**选择性**无法在
本机复现——它的确诊靠的是对照组 curl，这正是对照组之所以是诊断链关键
一步的原因。

场景D的实现细节本身是个坑的注脚：Popen 返回值必须保引用，否则 GC 的
`__del__` 会顺手 waitpid 把尸体收走——僵尸造不出来。「谁负责收尸」
在正常路径里被运行时藏得多好，异常路径里它就得你自己看。

## 9. 速查卡

```bash
# 存在
kill -0 <pid> && echo alive
ps aux | grep <name> | grep -v grep
# 状态 + 等待点
awk '{print $3}' /proc/<pid>/stat; cat /proc/<pid>/wchan
# 活着吗（差值）
a=$(awk '{print $14+$15}' /proc/<pid>/stat); sleep 5; \
  b=$(awk '{print $14+$15}' /proc/<pid>/stat); echo $((b-a))
# 网络
ss -tnp | grep <pid>
# 终极答案
timeout 3 strace -p <pid>
# 泄漏
ls /proc/<pid>/fd | wc -l; lsof -p <pid>
```
