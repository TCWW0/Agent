#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace my_agent::tool::skills {

// Skill = 一份聚焦的指令文档（SKILL.md），按需加载进对话。
//
// 三层渐进披露，动机是上下文预算：把所有 skill 正文都塞进系统提示，token 会随
// skill 数量线性增长，而任一回合真正用得上的通常只有一份。所以分层：
//
//   Tier1  目录  —— 名字 + 一句描述，每轮都在系统提示里。便宜，模型据此知道
//                    有什么可用。
//   Tier2  正文  —— 模型调 skill 工具才加载完整 SKILL.md。一次一份，用时才付。
//   Tier3  资源  —— 附带的脚本/参考文件**只列不读**，靠既有 read 工具按需取用。
//                    正文往往引用了它们，但引用不等于每次都要。
struct Skill {
    std::string name;         // frontmatter name，缺失时回退目录名
    std::string description;  // frontmatter description，缺失时回退正文首个非空行
    std::string body;         // frontmatter 之后的正文
    std::filesystem::path dir;
};

class SkillEngine {
public:
    // roots 按优先级从高到低。构造时注入而不是内部读环境 —— 与 MemoryStore、
    // make_read_tool 同一个约定，纯逻辑才能喂临时目录测。
    explicit SkillEngine(
        std::vector<std::filesystem::path> roots,
        std::function<bool(const std::filesystem::path&)> register_read_root = {});

    // 同名 skill 首个命中的根胜出（project 覆盖 user）。
    //
    // 每次调用都重扫，不缓存：目录扫描对这个量级可以忽略，而缓存要正确就得跟
    // mtime，那是一整套失效逻辑。手工新增一个 skill 下一轮就该出现在目录里。
    [[nodiscard]]
    std::vector<Skill> all() const;

    // Tier1：注入系统提示的目录块。没有 skill 时返回空串 —— 空的 <skills></skills>
    // 只是噪声，还可能被读成「确实没有可用 skill」的强信号。
    [[nodiscard]]
    std::string catalog_block() const;

    // Tier2：按名字加载。找不到返回 nullopt，由工具层渲染成模型能自我纠正的错误。
    [[nodiscard]]
    std::optional<std::string> activate(std::string_view name) const;

    // Tier3 的前提：这些目录要进 read 工具的允许列表，否则 skill 装在 $HOME 下时
    // 正文引用的脚本一读就被 workspace 硬边界拒掉。
    [[nodiscard]]
    std::vector<std::filesystem::path> directories() const;

private:
    std::vector<std::filesystem::path> roots_;
    std::function<bool(const std::filesystem::path&)> register_read_root_;
};

// 唯一碰环境的函数。优先级：project 的两个根在前，user 的两个根在后，所以
// 同名 skill 的项目版本覆盖全局版本。
//
// 带上 .claude/skills 是为了跨客户端可移植 —— 已经为 Claude Code 装好的 skill
// 不需要再复制一份。
[[nodiscard]]
std::vector<std::filesystem::path> discover_roots();

// 把一份 SKILL.md 的内容解析成 Skill。slug 是目录名，用于 name 缺失时回退。
// 纯函数，不碰文件系统 —— frontmatter 的解析规则可以直接断言。
[[nodiscard]]
Skill parse_skill(
    std::string_view contents,
    std::string_view slug,
    std::filesystem::path dir
);

}  // namespace my_agent::tool::skills
