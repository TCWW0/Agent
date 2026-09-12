#include "my_agent/prompt/system_prompt.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

namespace my_agent::prompt {
namespace {

constexpr const char* kIdentity =
    "You are my_agent, a command-line coding assistant.\n"
    "\n"
    "You help with software engineering tasks: reading code, running "
    "calculations, and answering questions about the project you are in.\n"
    "\n"
    "Guidelines:\n"
    "- Use the provided tools when they can answer a question more reliably "
    "than guessing. Prefer reading a file over speculating about it.\n"
    "- Keep answers short and concrete. Skip preamble.\n"
    "- If a tool call is rejected, respect the decision and continue without "
    "retrying the same call.\n"
    // 没有这一条，模型会在散文里"答应"记住然后什么也不做 —— 回合结束，事实丢失。
    // 上面那条指引只覆盖"工具能更可靠地回答问题"的情况，而 remember 不回答任何
    // 问题，所以要单独说明。
    "- When the user states a durable preference or fact about themselves or "
    "this project, call the remember tool to store it. Acknowledging it in prose "
    "is not enough; that is forgotten when the conversation ends.\n"
    // 同 remember 的理由：泛用条款覆盖不到「文档问题先检索」这个习惯。行号
    // 出处 + read 深读是配套动作；mode 行是量纲提示（BM25-only 时该知道
    // 结果只来自词法匹配，换个措辞再试可能更好）。
    "- For questions about this project's documentation, call search_docs "
    "before answering. Each passage carries a file path and line range; use "
    "read on that path when you need the surrounding context. The trailing "
    "mode: line says whether matching was hybrid or BM25-only.\n";

std::string detect_os()
{
#if defined(__linux__)
    utsname info{};
    if (uname(&info) == 0) {
        return std::string{info.sysname} + " " + info.release;
    }
    return "Linux";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "unknown";
#endif
}

}  // namespace

std::string build(const Context& context)
{
    std::string prompt{kIdentity};

    prompt += "\n<environment>\n";
    prompt += "working_directory: " + context.working_directory + "\n";
    prompt += "operating_system: " + context.operating_system + "\n";
    prompt += "</environment>\n";

    // 空块不输出：给模型一个空的 <memory></memory> 是噪声，还可能被当成"确实
    // 没有任何记忆"的强信号。
    if (!context.memories.empty()) {
        prompt += "\n<memory>\n";
        for (const std::string& memory : context.memories) {
            prompt += "- " + memory + "\n";
        }
        prompt += "</memory>\n";
    }

    // 目录整块由 SkillEngine 生成（Tier1 渐进披露：只有名字和一句描述，正文等
    // 模型 activate 才加载），这里只负责放进去。
    if (!context.skills_catalog.empty()) {
        prompt += "\n" + context.skills_catalog + "\n";
    }

    return prompt;
}

Context capture_environment()
{
    std::error_code error;
    std::filesystem::path cwd = std::filesystem::current_path(error);

    return Context{
        .working_directory = error ? std::string{"unknown"} : cwd.string(),
        .operating_system = detect_os(),
    };
}

}  // namespace my_agent::prompt
