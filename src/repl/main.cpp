#include "my_agent/http/http_client.hpp"
#include "my_agent/prompt/system_prompt.hpp"
#include "my_agent/provider/ollama.hpp"
#include "my_agent/runtime/async_host.hpp"
#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/skills.hpp"
#include "my_agent/ui/terminal.hpp"
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
        if (model.thread.messages.empty()) {
            return;
        }

        const my_agent::Message& latest = model.thread.messages.back();
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
bool ask_approval(const my_agent::PendingPermission& pending, const my_agent::Model& model)
{
    std::string tool_name{"(unknown)"};
    for (const my_agent::Message& message : model.thread.messages) {
        for (const my_agent::ToolCall& call : message.tool_calls) {
            if (call.id == pending.id) {
                tool_name = call.name;
            }
        }
    }

    std::cout << "\n[permission] allow tool '" << tool_name << "'? [y/N] "
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

// 两个 scope 的记录都注入，project 在后 —— 更贴近当前工作的事实离指令更近。
// 渲染带上 id，模型才能在用户说「忘掉那条」时精确调 forget。
std::vector<std::string> load_memories()
{
    namespace memory = my_agent::tool::memory;

    const memory::MemoryStore store{memory::discover_roots()};

    std::vector<std::string> lines;
    for (const memory::Scope scope : {memory::Scope::User, memory::Scope::Project}) {
        for (const memory::Record& record : store.load_all(scope)) {
            lines.push_back(memory::render_for_prompt(record));
        }
    }
    return lines;
}

// Tier1：只有名字和一句描述进提示。正文等模型调 skill 工具才加载，资源文件更是
// 只列不读 —— 全部塞进提示，token 会随 skill 数量线性增长，而一个回合真正用得上
// 的通常只有一份。
std::string load_skills_catalog()
{
    namespace skills = my_agent::tool::skills;
    return skills::SkillEngine{skills::discover_roots()}.catalog_block();
}

void report_errors(const my_agent::Model& model)
{
    if (model.thread.messages.empty()) {
        return;
    }

    const my_agent::Message& latest = model.thread.messages.back();
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

            if (!current.pending_permission) {
                break;
            }

            const my_agent::PendingPermission pending = *current.pending_permission;
            if (ask_approval(pending, current)) {
                host_runtime.dispatch(my_agent::Msg{
                    my_agent::PermissionApprove{.id = pending.id},
                });
            } else {
                host_runtime.dispatch(my_agent::Msg{
                    my_agent::PermissionReject{.id = pending.id},
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

    my_agent::AsyncHost host_runtime{
        my_agent::provider::ollama::make_stream(
            host,
            port,
            model,
            std::make_shared<my_agent::http::HttpClient>(),
            stream_stats
        ),
    };

    // 每轮重新构建：memory 与 skill 目录会在会话过程中变化。remember 工具刚写下
    // 的事实，下一轮就必须出现在提示里 —— 这正是 provider 是函数而不是字符串的理由。
    host_runtime.set_system_prompt_provider([] {
        my_agent::prompt::Context context = my_agent::prompt::capture_environment();
        context.memories = load_memories();
        context.skills_catalog = load_skills_catalog();
        return my_agent::prompt::build(context);
    });

    host_runtime.dispatch(my_agent::Msg{
        my_agent::SetProfile{parse_profile(profile_name)},
    });

    {
        // 驱动的作用域必须比 run_ui 大一点：析构负责退出备用屏并还原 termios，
        // 而下面那句给行式回退路径的提示得等还原之后才打，否则它会连同备用屏一起
        // 被丢掉，用户看到的是一个没有任何说明的空白屏。
        my_agent::ui::TerminalDriver terminal{STDIN_FILENO, STDOUT_FILENO};
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
                    status.context_used = snapshot.prompt_eval_count;
                    status.tokens_per_second = snapshot.tokens_per_second;
                    status.elapsed_seconds = snapshot.eval_duration_seconds;
                }
                return status;
            };

        if (my_agent::ui::run_ui(
                host_runtime,
                terminal,
                status_provider
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
