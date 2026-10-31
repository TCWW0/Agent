#include "read.hpp"
#include "my_agent/tool/effects.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace my_agent::tool::detail {
namespace {
namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxFileSize = 1024U * 1024U;
constexpr std::size_t kAutoOutlineSize = 32U * 1024U;
constexpr std::size_t kDefaultLineLimit = 2000;
constexpr std::size_t kMaxOutlineEntries = 250;
constexpr std::size_t kOutlineFallbackBytes = 1024;

struct ReadRequest
{
    std::string path;
    std::size_t start_line{1};
    std::size_t limit{kDefaultLineLimit};
    bool explicit_range{false};
};

[[nodiscard]] ToolError invalid_args(std::string message)
{
    return ToolError{
        .kind = ErrorKind::InvalidArgs,
        .message = std::move(message),
    };
}

[[nodiscard]] std::expected<std::optional<std::size_t>, ToolError>
positive_integer_arg(const nlohmann::json& args, std::string_view name)
{
    const std::string key{name};
    if (!args.contains(key)) {
        return std::optional<std::size_t>{};
    }

    const nlohmann::json& value = args.at(key);
    std::uint64_t parsed = 0;
    if (value.is_number_unsigned()) {
        parsed = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value <= 0) {
            return std::unexpected(invalid_args(key + " must be a positive integer"));
        }
        parsed = static_cast<std::uint64_t>(signed_value);
    } else {
        return std::unexpected(invalid_args(key + " must be a positive integer"));
    }

    if (parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(invalid_args(key + " must be a positive integer"));
    }
    return std::optional<std::size_t>{static_cast<std::size_t>(parsed)};
}

[[nodiscard]] std::expected<ReadRequest, ToolError> parse_read_request(const nlohmann::json& args)
{
    if (!args.is_object() || !args.contains("path") || !args["path"].is_string()) {
        return std::unexpected(invalid_args("read requires a string path"));
    }

    std::string path = args["path"].get<std::string>();
    if (path.empty()) {
        return std::unexpected(invalid_args("read path must not be empty"));
    }

    const auto offset = positive_integer_arg(args, "offset");
    if (!offset) {
        return std::unexpected(offset.error());
    }
    const auto limit = positive_integer_arg(args, "limit");
    if (!limit) {
        return std::unexpected(limit.error());
    }
    const auto start_line = positive_integer_arg(args, "start_line");
    if (!start_line) {
        return std::unexpected(start_line.error());
    }
    const auto end_line = positive_integer_arg(args, "end_line");
    if (!end_line) {
        return std::unexpected(end_line.error());
    }

    if (*offset && *start_line && **offset != **start_line) {
        return std::unexpected(invalid_args("offset conflicts with start_line"));
    }

    const std::size_t start = offset->value_or(start_line->value_or(1));
    std::size_t count = limit->value_or(kDefaultLineLimit);

    if (*end_line) {
        if (**end_line < start) {
            return std::unexpected(invalid_args("end_line must not precede the start line"));
        }
        const std::size_t implied_count = **end_line - start + 1;
        if (*limit && **limit != implied_count) {
            return std::unexpected(invalid_args("limit conflicts with end_line"));
        }
        count = implied_count;
    }

    if (count > std::numeric_limits<std::size_t>::max() - start + 1) {
        return std::unexpected(invalid_args("requested line range is too large"));
    }

    return ReadRequest{
        .path = std::move(path),
        .start_line = start,
        .limit = count,
        .explicit_range = args.contains("offset") || args.contains("limit") ||
                          args.contains("start_line") || args.contains("end_line"),
    };
}

[[nodiscard]] std::string render_line_range(std::string_view content, std::size_t start_line,
                                            std::size_t limit)
{
    std::string output;
    output.reserve(content.size());

    std::size_t total_lines = 0;
    std::size_t shown = 0;
    std::size_t line_start = 0;
    for (std::size_t index = 0; index < content.size(); ++index) {
        if (content[index] != '\n') {
            continue;
        }

        ++total_lines;
        if (total_lines >= start_line && shown < limit) {
            output.append(content.data() + line_start, index + 1 - line_start);
            ++shown;
        }
        line_start = index + 1;
    }

    if (line_start < content.size()) {
        ++total_lines;
        if (total_lines >= start_line && shown < limit) {
            output.append(content.data() + line_start, content.size() - line_start);
            ++shown;
        }
    }

    if (start_line > 1 || shown < total_lines) {
        output += "\n[";
        if (shown == 0) {
            output += "no lines shown from requested offset " + std::to_string(start_line);
        } else {
            const std::size_t last_line = start_line + shown - 1;
            output +=
                "showing lines " + std::to_string(start_line) + "-" + std::to_string(last_line);
        }
        output += " of " + std::to_string(total_lines) + "]";
    }

    return output;
}

[[nodiscard]] std::string_view trim_outline_line(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

[[nodiscard]] bool is_outline_line(std::string_view line)
{
    const std::string candidate{trim_outline_line(line)};
    if (candidate.empty() || candidate.size() > 4096) {
        return false;
    }

    static const std::regex heading{R"(#{1,6}\s+\S.*)",
                                    std::regex::ECMAScript | std::regex::optimize};
    static const std::regex type_declaration{
        R"((?:(?:export|public|private|protected|static|final|abstract)\s+)*(?:class|struct|enum(?:\s+class)?|namespace|interface|trait|module)\s+\S.*)",
        std::regex::ECMAScript | std::regex::optimize};
    static const std::regex named_function{
        R"((?:(?:pub|public|private|protected|static|inline|virtual|async|export|extern)\s+)*(?:fn|def|function)\s+\w+\s*\([^)]*\).*)",
        std::regex::ECMAScript | std::regex::optimize};
    static const std::regex c_like_function{
        R"((?:(?:pub|public|private|protected|static|inline|virtual|constexpr|consteval|extern)\s+)*(?:[\w:~<>\[\]&*,]+\s+)+[\w:~]+\s*\([^)]*\)\s*(?:const\s*)?(?:[;{].*)?)",
        std::regex::ECMAScript | std::regex::optimize};
    static const std::regex variable_declaration{
        R"((?:const|let|var)\s+(?:[\w:<>\[\]&*]+\s+)?\w+\s*(?:=|:).*)",
        std::regex::ECMAScript | std::regex::optimize};

    return std::regex_match(candidate, heading) || std::regex_match(candidate, type_declaration) ||
           std::regex_match(candidate, named_function) ||
           std::regex_match(candidate, c_like_function) ||
           std::regex_match(candidate, variable_declaration);
}

[[nodiscard]] std::size_t count_lines(std::string_view content)
{
    const std::size_t newlines =
        static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n'));
    return newlines + static_cast<std::size_t>(!content.empty() && content.back() != '\n');
}

[[nodiscard]] std::string render_outline(std::string_view content, std::string_view display_path)
{
    std::string entries;
    std::size_t emitted = 0;
    std::size_t line_number = 0;
    std::size_t line_start = 0;

    const auto inspect_line = [&](std::string_view line) {
        if (emitted >= kMaxOutlineEntries || !is_outline_line(line)) {
            return;
        }
        const std::string_view trimmed = trim_outline_line(line);
        entries += "[L" + std::to_string(line_number) + "] ";
        entries.append(trimmed.data(), trimmed.size());
        entries.push_back('\n');
        ++emitted;
    };

    for (std::size_t index = 0; index < content.size(); ++index) {
        if (content[index] != '\n') {
            continue;
        }
        ++line_number;
        inspect_line(content.substr(line_start, index - line_start));
        line_start = index + 1;
    }
    if (line_start < content.size()) {
        ++line_number;
        inspect_line(content.substr(line_start));
    }

    const std::string range_hint = "Use offset/limit or start_line/end_line to read specific "
                                   "regions.";
    if (!entries.empty()) {
        std::string output = "File is larger than 32 KiB, so a structural overview "
                             "was returned instead of full content.\n\n# Outline of ";
        output.append(display_path.data(), display_path.size());
        output += "\n\n" + entries;
        if (emitted == kMaxOutlineEntries) {
            output +=
                "\n[outline truncated at " + std::to_string(kMaxOutlineEntries) + " entries]\n";
        }
        output += "\n" + range_hint;
        return output;
    }

    std::size_t preview_size = std::min(kOutlineFallbackBytes, content.size());
    while (preview_size > 0 && preview_size < content.size() &&
           (static_cast<unsigned char>(content[preview_size]) & 0xC0) == 0x80) {
        --preview_size;
    }
    const std::size_t last_newline = content.rfind('\n', preview_size == 0 ? 0 : preview_size - 1);
    if (last_newline != std::string_view::npos && last_newline > 0) {
        preview_size = last_newline + 1;
    }

    std::string output = "File is larger than 32 KiB and has no recognizable "
                         "structure. Returning a leading preview instead of full "
                         "content.\nThe file has " +
                         std::to_string(count_lines(content)) + " lines.\n\n# First 1 KiB of ";
    output.append(display_path.data(), display_path.size());
    output += "\n\n";
    output.append(content.data(), preview_size);
    output += "\n" + range_hint;
    return output;
}

ExecResult execute_read(const ReadRoots& roots, const nlohmann::json& args)
{
    const auto request = parse_read_request(args);
    if (!request) {
        return std::unexpected(request.error());
    }

    const auto resolved = roots.resolve(fs::path{request->path}, "read");
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    const fs::path& path = *resolved;

    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to inspect path: " + path.string() + " (" + ec.message() + ")",
        });
    }
    if (!exists) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::NotFound,
            .message = "path does not exist: " + path.string(),
        });
    }
    ec.clear();
    const bool is_regular_file = fs::is_regular_file(path, ec);
    if (ec) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to inspect file type: " + path.string() + " (" + ec.message() + ")",
        });
    }
    if (!is_regular_file) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::NotAFile,
            .message = path.string() + " not a file",
        });
    }

    ec.clear();
    const std::uintmax_t file_size = fs::file_size(path, ec);
    if (ec) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to inspect file size: " + path.string() + " (" + ec.message() + ")",
        });
    }
    if (file_size > kMaxFileSize) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::TooLarge,
            .message = "file exceeds the 1 MiB read limit: " + path.string(),
        });
    }

    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected{ToolError{
            .kind = ErrorKind::IOError,
            .message = "unable to open file: " + path.string(),
        }};
    }

    std::array<char, 512> probe{};
    try {
        input.read(probe.data(), probe.size());
    } catch (const std::ios_base::failure& error) {
        return std::unexpected{ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to inspect file for binary content: " + path.string() + " (" +
                       error.what() + ")",
        }};
    }
    const std::streamsize count = input.gcount();
    if (input.bad()) {
        return std::unexpected{ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to inspect file for binary content: " + path.string(),
        }};
    }
    const auto probe_end = probe.begin() + count;
    const bool has_nul = std::find(probe.begin(), probe_end, '\0') != probe_end;

    if (has_nul) {
        return std::unexpected{ToolError{
            .kind = ErrorKind::Binary,
            .message = "error to read a binary file: " + path.string(),
        }};
    }

    try {
        input.clear();
        input.seekg(0, std::ios::beg);
    } catch (const std::ios_base::failure& error) {
        return std::unexpected{ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to rewind file after binary probe: " + path.string() + " (" +
                       error.what() + ")",
        }};
    }
    if (!input) {
        return std::unexpected{ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to rewind file after binary probe: " + path.string(),
        }};
    }

    std::string content(static_cast<std::size_t>(file_size), '\0');
    if (!content.empty()) {
        try {
            input.read(content.data(), static_cast<std::streamsize>(content.size()));
        } catch (const std::ios_base::failure& error) {
            return std::unexpected{ToolError{
                .kind = ErrorKind::IOError,
                .message = "failed to read file: " + path.string() + " (" + error.what() + ")",
            }};
        }
        if (input.bad() || input.gcount() != static_cast<std::streamsize>(content.size())) {
            return std::unexpected{ToolError{
                .kind = ErrorKind::IOError,
                .message = "failed to read complete file: " + path.string(),
            }};
        }
    }
    if (!request->explicit_range && content.size() > kAutoOutlineSize) {
        return ToolOutput{.text = render_outline(content, request->path)};
    }

    return ToolOutput{.text = render_line_range(content, request->start_line, request->limit)};
}
} // namespace

RegisteredTool make_read_tool(std::filesystem::path workspace_root,
                              std::vector<std::filesystem::path> allowed_roots)
{
    auto roots = std::make_shared<ReadRoots>(workspace_root);
    for (const fs::path& allowed : allowed_roots) {
        const auto added = roots->add(allowed);
        if (!added) {
            throw std::invalid_argument{added.error()};
        }
    }
    return make_read_tool(std::move(roots));
}

RegisteredTool make_read_tool(std::shared_ptr<ReadRoots> roots)
{
    return RegisteredTool{
        .descriptor =
            ToolDescriptor{
                .name = ToolName{.value = "read"},
                .description = "Read up to 2000 lines from a text file in the current "
                               "workspace, /tmp, or a directory allowed for this session. "
                               "Use offset/limit or start_line/end_line for ranges. Files "
                               "over 32 KiB return an outline unless a range is explicit; "
                               "files over 1 MiB are refused.",
                .input_schema =
                    nlohmann::json{
                        {"type", "object"},
                        {"properties",
                         nlohmann::json{
                             {"path",
                              nlohmann::json{
                                  {"type", "string"},
                                  {"minLength", 1},
                              }},
                             {"offset",
                              nlohmann::json{
                                  {"type", "integer"},
                                  {"minimum", 1},
                              }},
                             {"limit",
                              nlohmann::json{
                                  {"type", "integer"},
                                  {"minimum", 1},
                              }},
                             {"start_line",
                              nlohmann::json{
                                  {"type", "integer"},
                                  {"minimum", 1},
                              }},
                             {"end_line",
                              nlohmann::json{
                                  {"type", "integer"},
                                  {"minimum", 1},
                              }},
                         }},
                        {"required", nlohmann::json::array({"path"})},
                        {"additionalProperties", false},
                    },
                .effects = {Effect::ReadFs},
                .replay = {},
            },
        .handler =
            [roots = std::move(roots)](const nlohmann::json& args, const ToolExecutionContext&) {
                return execute_read(*roots, args);
            },
    };
}
} // namespace my_agent::tool::detail
