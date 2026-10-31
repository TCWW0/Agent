#include "my_agent/tool/skills.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace my_agent::tool::skills {
namespace {

constexpr std::string_view kWhitespace = " \t\r\n";

[[nodiscard]]
std::string_view trim(std::string_view text)
{
    const std::size_t begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(kWhitespace) - begin + 1);
}

// 去掉 value 两侧成对的引号。YAML 允许 `description: "..."`，但引号是语法不是内容。
[[nodiscard]]
std::string_view unquote(std::string_view value)
{
    if (value.size() >= 2 && value.front() == value.back()
        && (value.front() == '"' || value.front() == '\'')) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

[[nodiscard]]
std::string_view first_non_blank_line(std::string_view body)
{
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        const std::size_t end = body.find('\n', cursor);
        const std::string_view line = trim(
            body.substr(cursor, end == std::string_view::npos ? end : end - cursor)
        );
        if (!line.empty()) {
            return line;
        }
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1;
    }
    return {};
}

[[nodiscard]]
std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

}  // namespace

Skill parse_skill(
    std::string_view contents,
    std::string_view slug,
    std::filesystem::path dir
)
{
    Skill skill{.dir = std::move(dir)};

    std::string_view body = contents;
    std::string_view frontmatter;

    // 只有以 `---` 起首才算有 frontmatter。手写的 skill 常常整份都是正文，
    // 那也是一份可用的 skill，不是损坏的文件。
    if (contents.starts_with("---\n")) {
        const std::size_t close = contents.find("\n---", 3);
        if (close != std::string_view::npos) {
            frontmatter = contents.substr(4, close - 4);
            const std::size_t after = contents.find('\n', close + 1);
            body = after == std::string_view::npos
                ? std::string_view{}
                : contents.substr(after + 1);
        }
    }

    std::size_t cursor = 0;
    while (cursor < frontmatter.size()) {
        const std::size_t end = frontmatter.find('\n', cursor);
        const std::string_view line = frontmatter.substr(
            cursor,
            end == std::string_view::npos ? end : end - cursor
        );
        cursor = end == std::string_view::npos ? frontmatter.size() : end + 1;

        // 按**第一个**冒号切：`description: Use when: handling PDFs` 是真实
        // skill 里的常见写法，别的客户端也接受。严格 YAML 会在这里失败，而我们
        // 要能读别人已经装好的 skill。
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }

        const std::string_view key = trim(line.substr(0, colon));
        const std::string_view value = unquote(trim(line.substr(colon + 1)));

        if (key == "name") {
            skill.name = value;
        } else if (key == "description") {
            skill.description = value;
        }
        // 其余键宽容跳过：allowed-tools / license 等是别的客户端写的，
        // 一个陌生键不该让整份 skill 消失。
    }

    skill.body = body;

    // 目录名是 skill 的天然标识 —— activate 用它寻址，装 skill 的人已经用目录名
    // 表达了意图。
    if (skill.name.empty()) {
        skill.name = slug;
    }

    // Tier1 目录每行都要有描述，否则模型看到一个光名字无从判断何时用它。
    if (skill.description.empty()) {
        skill.description = first_non_blank_line(skill.body);
    }

    return skill;
}

SkillEngine::SkillEngine(
    std::vector<std::filesystem::path> roots,
    std::function<bool(const std::filesystem::path&)> register_read_root)
    : roots_(std::move(roots))
    , register_read_root_(std::move(register_read_root))
{
}

std::vector<Skill> SkillEngine::all() const
{
    namespace fs = std::filesystem;

    std::vector<Skill> found;

    for (const fs::path& root : roots_) {
        std::error_code error;
        // 四个发现根里通常只有一两个真的存在。不存在的根跳过，不是错误。
        fs::directory_iterator entries{root, error};
        if (error) {
            continue;
        }

        for (const fs::directory_entry& entry : entries) {
            if (!entry.is_directory(error) || error) {
                continue;
            }

            const fs::path manifest = entry.path() / "SKILL.md";
            if (!fs::is_regular_file(manifest, error) || error) {
                continue;
            }

            const std::string contents = read_file(manifest);
            if (contents.empty()) {
                continue;
            }

            Skill skill = parse_skill(
                contents,
                entry.path().filename().string(),
                entry.path()
            );

            // 首个命中的根胜出。合并两份同名 skill 会产生自相矛盾的指令，
            // 所以是覆盖而不是合并 —— 项目里的特化版本完整替换全局那份。
            const bool already = std::ranges::any_of(
                found,
                [&skill](const Skill& seen) { return seen.name == skill.name; }
            );
            if (already) {
                continue;
            }

            if (register_read_root_ && !register_read_root_(skill.dir)) {
                continue;
            }

            found.push_back(std::move(skill));
        }
    }

    // 按名字排序：目录顺序稳定，系统提示才不会因为文件系统的遍历顺序每轮抖动
    // （提示内容一变就打掉 provider 端的前缀缓存）。
    std::ranges::sort(found, {}, &Skill::name);
    return found;
}

std::string SkillEngine::catalog_block() const
{
    const std::vector<Skill> found = all();
    // 空块不输出：与 <memory> 同一个约定。空的 <skills></skills> 是噪声，
    // 还可能被读成「确实没有可用 skill」的强信号。
    if (found.empty()) {
        return {};
    }

    // 明确写出「用 skill 工具整份加载，不要猜」—— 只给一句描述时模型倾向于
    // 照着描述编造正文内容，而正文才是真正的指令。
    std::string block =
        "<skills>\n"
        "On-demand skills are available. Each is a focused instruction document "
        "you can load in full with the skill tool when its task comes up; load "
        "it rather than guessing what it says. A skill may bundle resource "
        "files, which the activation result lists — read the specific file when "
        "the instructions call for it.\n";

    for (const Skill& skill : found) {
        block += "- " + skill.name + " — " + skill.description + "\n";
    }

    block += "</skills>\n";
    return block;
}

std::optional<std::string> SkillEngine::activate(std::string_view name) const
{
    namespace fs = std::filesystem;

    const std::vector<Skill> found = all();
    const auto match = std::ranges::find_if(
        found,
        [name](const Skill& skill) { return skill.name == name; }
    );
    // 找不到返回 nullopt：由工具层渲染成模型能自我纠正的错误，而不是给一份空正文
    // 让它以为这份 skill 是空的。
    if (match == found.end()) {
        return std::nullopt;
    }

    std::string payload =
        "<skill_content name=\"" + match->name + "\">\n" + match->body;

    if (!payload.ends_with('\n')) {
        payload += '\n';
    }

    // 绝对路径必须给：正文里的相对路径是相对 skill 目录的，而工具的 cwd 是工作区。
    // 不给绝对路径，模型就拼不出 read 能用的路径。
    payload += "\nSkill directory: " + match->dir.string() + "\n";
    payload +=
        "Relative paths in this skill resolve against that directory; pass "
        "absolute paths to the read tool.\n";

    std::vector<std::string> resources;
    std::error_code error;
    const fs::recursive_directory_iterator end;
    for (fs::recursive_directory_iterator entry{match->dir, error};
         !error && entry != end;
         entry.increment(error)) {
        if (!entry->is_regular_file(error) || error) {
            continue;
        }
        if (entry->path().filename() == "SKILL.md") {
            continue;
        }
        resources.push_back(
            entry->path().lexically_relative(match->dir).generic_string()
        );
    }

    if (!resources.empty()) {
        std::ranges::sort(resources);
        payload += "\n<skill_resources>\n";
        for (const std::string& resource : resources) {
            payload += "  " + resource + "\n";
        }
        payload += "</skill_resources>\n";
        // 只列不读：正文往往引用了这些文件，但引用不等于每次都要，而一份 skill
        // 可能带上几十 KB 的参考文档。这句话是为了防止模型假设内容已经在手上。
        payload +=
            "These files are NOT loaded. Read the specific one when the "
            "instructions above call for it.\n";
    }

    payload += "</skill_content>\n";
    return payload;
}

std::vector<std::filesystem::path> discover_roots()
{
    namespace fs = std::filesystem;

    std::vector<fs::path> roots;

    std::error_code error;
    const fs::path cwd = fs::current_path(error);
    // project 的根在前，所以同名 skill 的项目版本覆盖全局版本。
    if (!error && !cwd.empty()) {
        roots.push_back(cwd / ".my_agent" / "skills");
        // 跨客户端可移植：已经为 Claude Code 装好的 skill 不需要再复制一份。
        roots.push_back(cwd / ".claude" / "skills");
    }

    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path{home} / ".my_agent" / "skills");
        roots.emplace_back(fs::path{home} / ".claude" / "skills");
    }

    return roots;
}

std::vector<std::filesystem::path> SkillEngine::directories() const
{
    std::vector<std::filesystem::path> dirs;
    for (const Skill& skill : all()) {
        dirs.push_back(skill.dir);
    }
    return dirs;
}

}  // namespace my_agent::tool::skills
