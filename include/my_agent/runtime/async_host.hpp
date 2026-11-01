#pragma once

#include "my_agent/provider/provider.hpp"
#include "my_agent/runtime/agent.hpp"
#include "my_agent/runtime/model.hpp"
#include "my_agent/runtime/wake_signal.hpp"
#include "my_agent/runtime/work_pool.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace my_agent {

// 每轮请求发出前重新构建系统提示。是函数而不是字符串，因为 memory 和 skill 目录
// 会在会话过程中变化（remember 工具就在改它），每轮都得看到最新的。
using SystemPromptProvider = std::function<std::string()>;

class AsyncHost{
public:
    AsyncHost(StreamEffect stream,
              std::shared_ptr<const tool::ToolRuntime> runtime);
    ~AsyncHost();

    // Owner-thread only，通常在开跑前设置一次。update() 是纯的、不能读文件，
    // 所以系统提示由宿主在 effect 侧注入 —— 这是 M7 的一行接入点。
    void set_system_prompt_provider(SystemPromptProvider provider);

    AsyncHost(const AsyncHost&) = delete;
    AsyncHost& operator=(const AsyncHost&) = delete;
    AsyncHost(AsyncHost&&) = delete;
    AsyncHost& operator=(AsyncHost&&) = delete;

    // Owner-thread only. 用于接受用户输入、审批等 owner 侧操作
    void dispatch(Msg msg);

    // 交换出当前 Inbox 消息批次，并按 FIFO 顺序交给 Core。返回本次是否有
    // 消息，供 UI 调度器把领域变化登记为一次重绘请求。
    [[nodiscard]]
    bool drain_inbox();

    // Owner-thread only. 阻塞事件循环：drain → 判定静止 → 等唤醒。
    // 在 Core 业务静止时返回：Idle、AwaitingPermission 或 Blocked。
    // 逻辑取消后的旧 worker 可能仍在收尾；这不是等待后台任务全部退出。
    void run_until_quiescent();

    // Owner-thread only. 测试用的唤醒观测 seam：等一次后台唤醒信号。
    [[nodiscard]]
    bool wait_wake(std::chrono::milliseconds timeout);

    // 后台唤醒的可 poll 读端。前端把它和 stdin、SIGWINCH 放进同一个 poll，
    // 从而在流式输出进行中也能读键盘 —— 这是 run_until_quiescent 做不到的事，
    // 那个函数在 phase 是 Streaming 时不返回。
    // 返回 -1 表示唤醒机制降级成惰性哨兵，调用方应改用超时轮询。
    [[nodiscard]]
    int wake_fd() const noexcept;

    // May be called from another thread. Signals shared stop and wakes the owner.
    void shutdown();

    [[nodiscard]]
    const Model& model() const noexcept;

private:
    struct InboxState;

    void process_msg(Msg msg);
    void execute_cmd(NoCommand cmd);
    void execute_cmd(PrepareRequest cmd);
    void execute_cmd(StartCompaction cmd);
    void execute_cmd(StartStream cmd);
    void execute_cmd(RunTool cmd);
    void execute_cmd(CancelOperation cmd);

    struct OperationState;
    struct CurrentOperation {
        OperationRef ref;
        std::shared_ptr<OperationState> state;
    };
    [[nodiscard]]
    std::shared_ptr<OperationState> begin_operation(OperationRef ref);
    void invalidate_current_operation() noexcept;

    [[nodiscard]]
    bool is_quiescent() const noexcept;

private:
    Model current_model_{};

    StreamEffect stream_;
    SystemPromptProvider system_prompt_provider_;
    const std::shared_ptr<const tool::ToolRuntime> tool_runtime_;

    // shared_ptr 而非成员对象：隔离通道的线程是 detach 的，可能活得比 host 久。
    // 共享所有权让它们不会对已析构的 WakeSignal / Inbox 动手。
    std::shared_ptr<WakeSignal> wake_;
    std::shared_ptr<InboxState> inbox_;

    // Owner-thread-only logical cancellation slot. Workers retain only the
    // shared OperationState and therefore never access host-owned state.
    std::optional<CurrentOperation> current_operation_;

    // Provider 流走共享池，工具执行走隔离通道。声明顺序即销毁顺序的逆序：
    // pool_ 最先析构（join 掉 worker），此时 inbox_/wake_ 仍然有效。
    WorkPool pool_;
};

}  // namespace my_agent
