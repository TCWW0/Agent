#include "my_agent/provider/provider.hpp"
#include "my_agent/runtime/async_host.hpp"
#include "my_agent/runtime/agent.hpp"
#include "my_agent/runtime/msg.hpp"
#include "my_agent/tool/tool.hpp"
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

namespace my_agent{
    struct AsyncHost::InboxState{
        explicit InboxState(std::shared_ptr<WakeSignal> wake):wake(std::move(wake))
        {
        }

        // 生产者(Worker Thread) 推送数据的接口，只在数据由0->1时进行通知
        void post(Msg msg)
        {
            bool should_wake = false;
            {
                std::lock_guard<std::mutex> lock{mutex};

                should_wake = messages.empty();
                messages.push_back(std::move(msg));
            }

            if (should_wake&&wake){
                wake->signal();
            }
        }

        // 消费者"消费(取出)"数据的接口
        std::vector<Msg> drain()
        {
            std::vector<Msg> batch;
            {
                std::lock_guard<std::mutex> lock(mutex);
                batch.swap(messages);
            }
            return batch;
        }

        std::shared_ptr<WakeSignal> wake;   // 生产者通知消费者的方式
        std::mutex mutex;
        std::vector<Msg> messages;

    };

    AsyncHost::AsyncHost(StreamEffect stream)
        :AsyncHost(std::move(stream),ToolExecEffect{tool::execute})
    {}

    AsyncHost::AsyncHost(StreamEffect stream,ToolExecEffect execute_tool)
        :stream_(std::move(stream)),execute_tool_(execute_tool),
         wake_(std::make_shared<WakeSignal>()),
         inbox_(std::make_shared<InboxState>(wake_))
        {}

    void AsyncHost::shutdown()
    {
        // Owner-thread only，且是终态：stop 之后不再重置。重置会让仍持有旧
        // token 的 worker 变成孤儿，永远看不到停止请求。
        //
        // 不需要 signal 唤醒阻塞中的 owner：shutdown 与 run_until_quiescent
        // 同属 owner thread，二者不可能同时在跑，所以 owner 绝不会在 stop 之后
        // 还阻塞在 wait() 上。循环入口的 stop 检查已经足够。
        pool_.request_stop();
    }

    AsyncHost::~AsyncHost()
    {
        shutdown();
    }

    const Model& AsyncHost::model() const noexcept
    {
        return current_model_;
    }

    void AsyncHost::dispatch(Msg msg)
    {
        process_msg(std::move(msg));
    }

    void AsyncHost::drain_inbox()
    {
        std::vector<Msg> batch = inbox_->drain();
        for(Msg& msg:batch){
            process_msg(std::move(msg));
        }
    }

    bool AsyncHost::is_quiescent() const noexcept
    {
        return std::holds_alternative<Idle>(current_model_.phase)
            || std::holds_alternative<AwaitingPermission>(current_model_.phase);
    }

    void AsyncHost::run_until_quiescent()
    {
        while (true) {
            if (pool_.stop_token().stop_requested()) {
                return;
            }

            drain_inbox();

            if (is_quiescent()) {
                return;
            }

            wake_->wait();
        }
    }

    bool AsyncHost::wait_wake(std::chrono::milliseconds timeout)
    {
        return wake_->wait_for(timeout);
    }

    int AsyncHost::wake_fd() const noexcept
    {
        return wake_->fd();
    }

    void AsyncHost::process_msg(Msg msg)
    {
        Step step = update(std::move(current_model_),std::move(msg));
        current_model_ = std::move(step.model);
        std::visit([this](auto&& command){execute_cmd(std::move(command));},std::move(step.cmd));
    }

    void AsyncHost::execute_cmd(NoCommand)
    {
    }

    void AsyncHost::set_system_prompt_provider(SystemPromptProvider provider)
    {
        system_prompt_provider_ = std::move(provider);
    }

    void AsyncHost::execute_cmd(StartStream command)
    {
        // update() 是纯的，make_request() 只能把 system_prompt 留空。在这里补上：
        // 构建它要读 memory / skill 文件，IO 属于 effect 侧。每轮重新构建，因为
        // remember 工具会在会话过程中改动这些内容。
        if (system_prompt_provider_) {
            command.request.system_prompt = system_prompt_provider_();
        }

        StreamEffect stream = stream_;
        std::shared_ptr<InboxState> inbox = inbox_;
        std::stop_token token = pool_.stop_token();

        // 共享池：Provider 流是长时任务，但它必须与短工具调用共享一个有界的池，
        // 否则一条流就能把线程数无界地拉起来。
        pool_.task(
            [
                stream = std::move(stream),
                token = token,
                inbox = std::move(inbox),
                request = std::move(command.request)
            ]() mutable {
                // 事件循环把 phase 当作 in-flight 计数器，这依赖一条边界保证：
                // 每次 stream 调用都必须投出恰好一个终态事件。Provider 抛异常
                // 或静默返回时若不补上，owner 会永久停在 Streaming 上等一个
                // 永不到来的消息。用超时兜会让测试 flaky，所以在这里强制。
                bool terminal_posted = false;

                EventSink sink = [inbox,token,&terminal_posted] (Msg msg){
                    if (std::holds_alternative<StreamFinished>(msg)
                        || std::holds_alternative<StreamError>(msg)) {
                        terminal_posted = true;
                    }

                    if (token.stop_requested()) return;
                    inbox -> post(std::move(msg));
                };

                try {
                    stream(std::move(request),sink);
                } catch (const std::exception& error) {
                    sink(Msg{StreamError{.message = error.what()}});
                } catch (...) {
                    sink(Msg{StreamError{.message = "unknown stream failure"}});
                }

                if (!terminal_posted) {
                    sink(Msg{StreamError{
                        .message = "stream returned without a terminal event",
                    }});
                }
            }
        );
    }

    void AsyncHost::execute_cmd(RunTool command)
    {
        ToolExecEffect execute_tool = execute_tool_;
        std::shared_ptr<InboxState> inbox = inbox_;
        std::stop_token token = pool_.stop_token();

        // 隔离通道：工具可能永久卡在阻塞 syscall 上（死掉的挂载点、僵住的子
        // 进程）。放在共享池里会永久占掉一个槽位并饿死后续任务。捕获的 inbox
        // 是 shared_ptr，所以即使 host 已析构，detached 线程也能安全收尾。
        pool_.task_isolated(
            [
                execute_tool = std::move(execute_tool),
                inbox = std::move(inbox),
                token = token,
                command = std::move(command)
            ]()mutable {
                // 同样的边界保证：ExecutingTool 也靠 phase 当 in-flight 计数
                // 器，工具抛异常必须收敛成一次失败结果投递回去。
                tool::ExecResult result = [&]() -> tool::ExecResult {
                    try {
                        return execute_tool(command.name,command.args);
                    } catch (const std::exception& error) {
                        return std::unexpected(tool::ToolError{
                            .kind = tool::ErrorKind::ExecutionFailed,
                            .message = error.what(),
                        });
                    } catch (...) {
                        return std::unexpected(tool::ToolError{
                            .kind = tool::ErrorKind::ExecutionFailed,
                            .message = "unknown tool failure",
                        });
                    }
                }();

                if (token.stop_requested()) return;
                inbox->post(Msg{
                    ToolExecOutput{
                        .id = std::move(command.id),
                        .result = std::move(result),
                    }
                });
            }
        );
    }
}
