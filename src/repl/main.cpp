#include "my_agent/http/http_client.hpp"
#include "my_agent/prompt/system_prompt.hpp"
#include "my_agent/provider/ollama.hpp"
#include "my_agent/runtime/async_host.hpp"
#include "my_agent/runtime/permission_projection.hpp"
#include "my_agent/tool/session_services.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include "my_agent/ui/painter.hpp"
#include "my_agent/ui/terminal_session.hpp"
#include "my_agent/ui/ui_loop.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <unistd.h>

namespace {

std::string env_or(const char* name, std::string fallback)
{
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string{value}
                                                : std::move(fallback);
}

// 前端只碰 dispatch / run_until_quiescent / model 三个公开 seam，与循环核心解耦。
// 流式 token 要在到达时立刻可见，但 Msg 类型里没有渲染回调 —— 所以这里记住已
// 打印的长度，每次 drain 之后只输出增量。未来的 TUI 从同一个位置接入。
class Printer {
public:
    void render(const my_agent::Model& model)
    {
        if (model.domain.thread.messages.empty()) {
            return;
        }

        const my_agent::Message& latest = model.domain.thread.messages.back();
        if (latest.role != my_agent::Role::Assistant) {
            return;
        }

        if (latest.text.size() > printed_) {
            std::cout << std::string_view{latest.text}.substr(printed_)
                      << std::flush;
            printed_ = latest.text.size();
        }
    }

    void begin_turn() { printed_ = 0; }

private:
    std::size_t printed_{0};
};

// 权限审批：停在 AwaitingPermission 时问一次 y/n。这是 M4 权限闭环的终端出口。
bool ask_approval(const my_agent::ToolActivityProjection& activity)
{
    std::cout << "\n[permission] allow tool '" << activity.tool_name
              << "'? effect=" << activity.effects
              << " args: " << activity.arguments << " [y/N] "
              << std::flush;

    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }

    return answer == "y" || answer == "Y";
}

my_agent::Profile parse_profile(std::string_view name)
{
    if (name == "minimal") {
        return my_agent::Profile::Minimal;
    }
    if (name == "ask") {
        return my_agent::Profile::Ask;
    }
    return my_agent::Profile::Write;
}

void report_errors(const my_agent::Model& model)
{
    if (model.domain.thread.messages.empty()) {
        return;
    }

    const my_agent::Message& latest = model.domain.thread.messages.back();
    if (latest.error) {
        std::cout << "\n[error] " << *latest.error << "\n";
    }
}

// 行式回退路径。非 tty（管道、CI、重定向、`| tee`）时走这条 —— 往文件里吐转义
// 序列毫无意义，而这条路径本身还是阶段一的验收出口，保留着它就保留了一个不依赖
// 终端的端到端通道。
void run_line_repl(my_agent::AsyncHost& host_runtime)
{
    Printer printer;
    std::string line;

    while (true) {
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;
        }

        if (line.empty()) {
            continue;
        }

        printer.begin_turn();
        host_runtime.dispatch(my_agent::Msg{my_agent::Submit{.text = line}});

        // 一个回合可能要多次进出循环：每次工具审批都是一个静止点。
        while (true) {
            host_runtime.run_until_quiescent();
            printer.render(host_runtime.model());

            const my_agent::Model& current = host_runtime.model();
            if (!std::holds_alternative<my_agent::AwaitingPermission>(current.phase)) {
                break;
            }

            const my_agent::ToolActivityProjection activity =
                my_agent::tool_activity_projection(current);
            if (activity.kind
                != my_agent::ToolActivityProjection::Kind::AwaitingPermission) {
                std::cout << "\n[permission] "
                          << (activity.diagnostic.empty()
                                  ? "permission state invalid"
                                  : activity.diagnostic)
                          << "; cancelling turn\n";
                host_runtime.dispatch(my_agent::Msg{my_agent::CancelActive{}});
                continue;
            }
            if (ask_approval(activity)) {
                host_runtime.dispatch(my_agent::Msg{
                    my_agent::PermissionApprove{
                        .call = activity.call},
                });
            } else {
                host_runtime.dispatch(my_agent::Msg{
                    my_agent::PermissionReject{
                        .call = activity.call},
                });
            }
        }

        report_errors(host_runtime.model());
        std::cout << "\n";
    }
}

}  // namespace

int main()
{
    const std::string host = env_or("MY_AGENT_OLLAMA_HOST", "localhost");
    const int port = std::stoi(env_or("MY_AGENT_OLLAMA_PORT", "11434"));
    const std::string model = env_or("MY_AGENT_MODEL", "qwen3.5:latest");
    const std::string profile_name = env_or("MY_AGENT_PROFILE", "write");
    const std::size_t context_limit = static_cast<std::size_t>(
        std::stoull(env_or("MY_AGENT_CONTEXT_LIMIT", "8192"))
    );
    const auto stream_stats =
        std::make_shared<my_agent::provider::ollama::StreamStats>();

    const my_agent::tool::SessionServices services{
        my_agent::tool::discover_session_facts()
    };

    const auto tool_runtime = std::make_shared<const my_agent::tool::ToolRuntime>(
        services.registrations());
    
    my_agent::AsyncHost host_runtime{
        my_agent::provider::ollama::make_stream(
            host,
            port,
            model,
            std::make_shared<my_agent::http::HttpClient>(),
            stream_stats,
            context_limit
        ),
        tool_runtime,
    };
    host_runtime.dispatch(my_agent::Msg{my_agent::SetRequestPolicy{
        .policy = my_agent::RequestPolicy{my_agent::ContextManagementConfig{
                                              .configured_limit = context_limit,
                                          },
            "ollama",
            model}}});

    // 每轮重新构建**提示**，但不再重建**服务**。
    //
    // 旧注释说「memory 与 skill 目录会在会话过程中变化，所以每轮重建」——
    // 前提对（remember 刚写下的事实，下一轮必须出现在提示里），结论错。
    // 答案之所以新鲜，是因为 MemoryStore::load_all() 每次读盘、
    // SkillEngine::all() 每次重扫，不是因为环境发现了两次。
    // 实例稳定，答案新鲜。
    host_runtime.set_system_prompt_provider([&services] {
        return my_agent::prompt::build(my_agent::prompt::Context{
            .working_directory = services.workspace_root().string(),
            .operating_system = my_agent::prompt::host_operating_system(),
            .memories = services.memory_lines(),
            .skills_catalog = services.skill_catalog(),
        });
    });

    host_runtime.dispatch(my_agent::Msg{
        my_agent::SetProfile{parse_profile(profile_name)},
    });

    {
        // 驱动的作用域必须比 run_ui 大一点：析构负责退出备用屏并还原 termios，
        // 而下面那句给行式回退路径的提示得等还原之后才打，否则它会连同备用屏一起
        // 被丢掉，用户看到的是一个没有任何说明的空白屏。
        my_agent::ui::TerminalSession terminal{STDIN_FILENO, STDOUT_FILENO};
        my_agent::ui::Painter painter;
        // 崩溃时析构不会执行，而 raw mode 没还原意味着用户的 shell 从此不回显 ——
        // 只能敲 reset 才救得回来。所以这条路径不能依赖 RAII。放在真实入口而不是
        // 驱动构造函数里：挂信号是进程级副作用，测试要能构造驱动而不动它。
        terminal.install_crash_handler();

        // 非 tty 时 run_ui 立刻返回 false。判断放在 run_ui 里而不是这里，是因为
        // 「能不能跑」是那个循环自己的前置条件，调用方只需要知道它没跑。
        const my_agent::ui::StatusProvider status_provider =
            [model, context_limit, stream_stats] {
                my_agent::ui::StatusBarInput status{
                    .model_name = model,
                    .context_limit = context_limit,
                };
                const auto snapshot = stream_stats->snapshot();
                if (snapshot.available) {
                    status.tokens_per_second = snapshot.tokens_per_second;
                    status.tokens_generated = snapshot.eval_count;
                    status.telemetry_generation = snapshot.generation;
                    status.elapsed_seconds = snapshot.eval_duration_seconds;
                }
                return status;
            };
        const my_agent::ui::UiConfig ui_config =
            my_agent::ui::UiConfig::from_environment_value(
                std::getenv("MY_AGENT_MOUSE_SCROLL_ROWS"));

        if (my_agent::ui::run_ui(
                host_runtime,
                terminal,
                painter,
                status_provider,
                ui_config
            )) {
            host_runtime.shutdown();
            return 0;
        }
    }

    std::cout << "my_agent REPL — model " << model << " at " << host << ':'
              << port << " (profile: " << profile_name << ")"
              << "\nNot a terminal, falling back to line mode."
              << "\nType your message, or Ctrl-D to exit.\n";

    run_line_repl(host_runtime);

    host_runtime.shutdown();
    return 0;
}
