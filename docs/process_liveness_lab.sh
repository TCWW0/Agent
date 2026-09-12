#!/usr/bin/env bash
# 进程存活排查实验场 —— 配合 PROCESS_LIVENESS_NOTES.md 使用
#
# 用法:
#   ./process_liveness_lab.sh            # 依次跑全部场景（自动清理）
#   ./process_liveness_lab.sh hold A     # 只跑场景A，保持存活直到 Ctrl+C，方便手动把玩
#   ./process_liveness_lab.sh A C        # 只跑指定场景
#
# 场景对照（NOTES.md 的五层阶梯各有活体标本）:
#   A  健康等待    —— 网络程序的常态：state=S，阻塞在 socket 读
#   B  繁忙计算    —— state=R，CPU ticks 持续增长
#   C  SYN-SENT楔死 —— 服务端 accept 队列满不收，客户端连接困在 SYN-SENT
#   D  僵尸        —— state=Z，父进程不收尸
#   E  CLOSE-WAIT堆积 —— 对端已关、本端不关：fd 泄漏的形态
#
# 每个场景自造演员、自打观测、自报预期读数，退出自动清理。

set -u

PY=python3
PORT_A=47301   # 场景A 健康服务器
PORT_C=47303   # 场景C 楔死服务器（listen 但不 accept）
PORT_E=47305   # 场景E close-wait 服务器
LAB_PIDS=()

cleanup() {
    for p in ${LAB_PIDS[@]+"${LAB_PIDS[@]}"}; do kill -9 "$p" 2>/dev/null; done
}
trap cleanup EXIT

hdr()  { printf '\n\033[1;36m══ %s ══\033[0m\n' "$*"; }
note() { printf '\033[0;33m%s\033[0m\n' "$*"; }
run()  { printf '\033[0;90m$\033[0m %s\n' "$*"; eval "$*"; }

# 第1层量具：进程状态字母
state_of() {
    local pid=$1
    awk '{printf "  state=%s (ppid=%s) comm=%s\n", $3, $4, $2}' "/proc/$pid/stat" 2>/dev/null \
        || echo "  (pid $pid 已不存在)"
    printf "  wchan="; cat "/proc/$pid/wchan" 2>/dev/null; echo
}

# 第2层量具：CPU ticks 两次采样取差值（100 ticks/s 制）
cpu_delta() {
    local pid=$1 secs=$2 a b
    a=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null) || return
    sleep "$secs"
    b=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null) || return
    echo "  ${secs}s 内 CPU tick 差: $((b-a))"
}

# ── 场景A：健康等待（网络程序的常态，不是病）──────────────────────────────
scene_A() {
    hdr "场景A 健康等待：客户端阻塞在 socket 读 —— state=S 是常态"
    $PY -c 'import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",'"$PORT_A"')); s.listen(5)
c,_=s.accept(); time.sleep(600)' &
    LAB_PIDS+=($!)
    sleep 1   # 等服务端 bind+listen 就位，再起客户端（否则 Connection refused）
    $PY -c 'import socket,time
c=socket.create_connection(("127.0.0.1",'"$PORT_A"'))
c.recv(1024)  # 阻塞读：对端永远不发 → 健康地等下去' &
    LAB_PIDS+=($!)
    sleep 1
    local cpid=${LAB_PIDS[-1]}
    note "预期读数：state=S（可中断睡眠）；wchan 是 socket 等待点（sk_wait_data 一类）；"
    note "连接形态 ESTAB；CPU tick 差=0 —— 零 CPU 不是病，是『在等网络』。"
    run "state_of $cpid"
    run "ss -tnp | grep :$PORT_A"
    run "cpu_delta $cpid 3"
}

# ── 场景B：繁忙计算（state=R，CPU 持续增长）───────────────────────────────
scene_B() {
    hdr "场景B 繁忙计算：纯 CPU 循环 —— 与场景A 对照"
    $PY -c 'x=0
while True: x+=1' &
    LAB_PIDS+=($!)
    sleep 1
    local pid=${LAB_PIDS[-1]}
    note "预期读数：state=R（或高频出现在 R）；CPU tick 差显著非零。"
    note "『在干活』与『在挂起』的分界就在这一层：两采样差值。"
    run "state_of $pid"
    run "cpu_delta $pid 3"
}

# ── 场景C：SYN-SENT 楔死（accept 队列满，SYN 被静默丢弃）──────────────────
scene_C() {
    hdr "场景C SYN-SENT 楔死：服务端 listen(1) 却永不 accept"
    $PY -c 'import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",'"$PORT_C"')); s.listen(1)   # 队列容量1，收下第一个就满
time.sleep(600)' &
    LAB_PIDS+=($!)
    sleep 1
    note "起 1 个持有人 + 5 个倒霉蛋：持有人挤进 accept 队列并保持（ESTAB），"
    note "倒霉蛋的 SYN 被内核丢弃，按 1s→3s→7s… 退避重传 —— 固化在 SYN-SENT。"
    note "彩蛋读数：Linux 的 accept 队列实际容量比 listen(1) 的字面值大一点，"
    note "总有一两个倒霉蛋挤了进去 —— 连接成功、脚本立即退出、socket 随之关闭，"
    note "留下一对 FIN-WAIT-2/CLOSE-WAIT 残影。这不是 bug，是内核排队的现实。"
    $PY -c 'import socket,time
c=socket.create_connection(("127.0.0.1",'"$PORT_C"'), timeout=60)
time.sleep(600)   # 持有人：连接已成功（进了 accept 队列），攥着不放' &
    LAB_PIDS+=($!)
    sleep 1
    for i in 1 2 3 4 5; do
        $PY -c 'import socket
c=socket.create_connection(("127.0.0.1",'"$PORT_C"'), timeout=60)' &
        LAB_PIDS+=($!)
    done
    sleep 3
    note "预期读数：若干条 SYN-SENT 固化（多采几次都不消散才叫固化）；"
    note "客户端 CPU 几乎不涨 —— 与场景A 同样零 CPU，但连接形态暴露了死因。"
    run "ss -tnp | grep :$PORT_C"
    note "对照组（诊断链的关键一步）：新进程 curl 打另一个健康端口，秒通 ——"
    note "说明『服务端地形没问题，问题在这条通路上』。"
    run "timeout 3 curl -s -o /dev/null -w 'curl->健康端口A: %{time_connect}s %{http_code}\n' http://127.0.0.1:$PORT_A/" \
        || note "（健康端口A没起 —— 单跑场景C时可先 ./process_liveness_lab.sh hold A 开着）"
    note "诚实标注：本场景模拟的是『队列满 → 对所有来者一视同仁地饿死』；"
    note "真实的 WSL 转发饿死更刁钻 —— 只饿存量进程、新进程畅通，"
    note "那种选择性无法在本机复现，见 NOTES.md 实战复盘一节。"
}

# ── 场景D：僵尸（state=Z，父进程不收尸）───────────────────────────────────
scene_D() {
    hdr "场景D 僵尸：子进程已退出，父进程不 wait —— state=Z"
    # 关键细节：Popen 的返回值必须保活（局部变量 p 在 frame 里活到 sleep 结束）。
    # 丢弃返回值会触发 GC 里的 __del__ → 顺手 waitpid → 僵尸造不出来。
    $PY -c 'import subprocess,time
p=subprocess.Popen(["true"])   # 子进程立即退出；p 保引用 → 无人收尸
time.sleep(600)' &
    LAB_PIDS+=($!)
    sleep 1
    local ppid=${LAB_PIDS[-1]}
    note "预期读数：state=Z 的 (true) 进程，ppid 指向 python 父进程。"
    note "僵尸不再消耗 CPU/内存，只占进程表一项；排查方向转向父进程：为什么不收尸。"
    note "杀掉父进程后由 init 接手收尸 —— 可以现场验证。"
    run "ps -o pid,ppid,state,comm --ppid $ppid"
}

# ── 场景E：CLOSE-WAIT 堆积（对端已关、本端不关 —— fd 泄漏形态）────────────
scene_E() {
    hdr "场景E CLOSE-WAIT 堆积：服务端 accept 后立刻 close，客户端死攥 socket 不放"
    $PY -c 'import socket
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",'"$PORT_E"')); s.listen(16)
while True:
    c,_=s.accept(); c.close()   # 收一个关一个：FIN 发往客户端' &
    LAB_PIDS+=($!)
    sleep 1
    $PY -c 'import socket,time
socks=[socket.create_connection(("127.0.0.1",'"$PORT_E"')) for _ in range(5)]
time.sleep(600)   # 客户端不 close：5 条 CLOSE-WAIT 固化' &
    LAB_PIDS+=($!)
    sleep 2
    local cpid=${LAB_PIDS[-1]}
    note "预期读数：客户端名下 5 条 CLOSE-WAIT；fd 数量随泄漏只增不减。"
    note "健康的程序 fd 数量应恒定波动（开 +1、close -1）；"
    note "CLOSE-WAIT 堆积 + fd 只涨不降 = 泄漏，最终撞 ulimit -n。"
    run "ss -tnp | grep CLOSE-WAIT | grep pid=$cpid"
    run "ls /proc/$cpid/fd | wc -l"
}

# ── 分发 ──────────────────────────────────────────────────────────────────
if [ $# -eq 0 ]; then
    for s in A B C D E; do "scene_$s"; sleep 1; done
elif [ "$1" = "hold" ]; then
    "scene_$2"
    note "保持存活，随便把玩（state_of / ss / strace -p …）。Ctrl+C 退出并清理。"
    wait
else
    for s in "$@"; do "scene_$s"; sleep 1; done
fi
