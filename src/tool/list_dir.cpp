#include "list_dir.hpp"

#include "my_agent/tool/effects.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "read_roots.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace my_agent::tool::detail {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kDefaultMaxDepth = 3;
constexpr std::size_t kMaximumMaxDepth = 16;
constexpr std::size_t kMaximumEntries = 1000;
constexpr std::size_t kMaximumOutputBytes = 64U * 1024U;
constexpr std::string_view kEntryLimitMarker =
    "[listing truncated after 1000 entries; use a narrower path or glob]\n";
constexpr std::string_view kByteLimitMarker =
    "[listing truncated at 64 KiB; use a narrower path or glob]\n";
constexpr std::size_t kMarkerReserve =
    kEntryLimitMarker.size() > kByteLimitMarker.size()
        ? kEntryLimitMarker.size()
        : kByteLimitMarker.size();

struct ListDirRequest
{
    std::string path{"."};
    bool recursive{false};
    std::size_t max_depth{kDefaultMaxDepth};
};

enum class EntryType : std::uint8_t {
    Directory,
    RegularFile,
    Symlink,
    Other,
};

struct ListedEntry
{
    fs::directory_entry entry;
    EntryType type{EntryType::Other};
    std::optional<std::string> metadata_error;
};

struct DirectoryContents
{
    std::vector<ListedEntry> entries;
    std::optional<std::string> traversal_error;
};

enum class Truncation : std::uint8_t {
    None,
    EntryLimit,
    ByteLimit,
};

struct RenderState
{
    std::string output;
    std::size_t entries{0};
    Truncation truncation{Truncation::None};

    [[nodiscard]] bool append_entry(std::string line)
    {
        if (truncation != Truncation::None) {
            return false;
        }
        if (entries >= kMaximumEntries) {
            truncation = Truncation::EntryLimit;
            return false;
        }
        if (output.size() + line.size() + kMarkerReserve > kMaximumOutputBytes) {
            truncation = Truncation::ByteLimit;
            return false;
        }
        output += std::move(line);
        ++entries;
        return true;
    }

    void append_notice(std::string line)
    {
        if (truncation != Truncation::None) {
            return;
        }
        if (output.size() + line.size() + kMarkerReserve > kMaximumOutputBytes) {
            truncation = Truncation::ByteLimit;
            return;
        }
        output += std::move(line);
    }

    void finish()
    {
        if (truncation == Truncation::EntryLimit) {
            output += kEntryLimitMarker;
        } else if (truncation == Truncation::ByteLimit) {
            output += kByteLimitMarker;
        }
    }
};

[[nodiscard]] ToolError invalid_args(std::string message)
{
    return ToolError{
        .kind = ErrorKind::InvalidArgs,
        .message = std::move(message),
    };
}

[[nodiscard]] std::expected<ListDirRequest, ToolError>
parse_list_dir_request(const nlohmann::json& args)
{
    if (!args.is_object()) {
        return std::unexpected(invalid_args("list_dir arguments must be an object"));
    }

    ListDirRequest request;
    if (args.contains("path")) {
        if (!args.at("path").is_string()) {
            return std::unexpected(invalid_args("path must be a non-empty string"));
        }
        request.path = args.at("path").get<std::string>();
        if (request.path.empty()) {
            return std::unexpected(invalid_args("path must be a non-empty string"));
        }
    }

    if (args.contains("recursive")) {
        if (!args.at("recursive").is_boolean()) {
            return std::unexpected(invalid_args("recursive must be a boolean"));
        }
        request.recursive = args.at("recursive").get<bool>();
    }

    if (args.contains("max_depth")) {
        if (!args.contains("recursive") || !request.recursive) {
            return std::unexpected(invalid_args("max_depth requires recursive=true"));
        }
        const nlohmann::json& value = args.at("max_depth");
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return std::unexpected(invalid_args("max_depth must be an integer from 1 to 16"));
        }
        const std::int64_t parsed = value.get<std::int64_t>();
        if (parsed < 1 || parsed > static_cast<std::int64_t>(kMaximumMaxDepth)) {
            return std::unexpected(invalid_args("max_depth must be an integer from 1 to 16"));
        }
        request.max_depth = static_cast<std::size_t>(parsed);
    }
    return request;
}

[[nodiscard]] std::string format_size(std::uintmax_t bytes)
{
    if (bytes < 1024U) {
        return std::to_string(bytes) + " B";
    }

    constexpr std::uintmax_t kKiB = 1024U;
    constexpr std::uintmax_t kMiB = kKiB * 1024U;
    constexpr std::uintmax_t kGiB = kMiB * 1024U;
    char buffer[32];
    if (bytes < kMiB) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB", bytes / static_cast<double>(kKiB));
    } else if (bytes < kGiB) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MiB", bytes / static_cast<double>(kMiB));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f GiB", bytes / static_cast<double>(kGiB));
    }
    return buffer;
}

[[nodiscard]] std::string quote(std::string_view value)
{
    std::string output{"\""};
    const auto append_hex = [&output](unsigned char byte) {
        char escaped[5];
        std::snprintf(escaped, sizeof(escaped), "\\x%02X", byte);
        output += escaped;
    };
    const auto continuation = [&value](std::size_t index) {
        return index < value.size()
               && (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U;
    };

    for (std::size_t index = 0; index < value.size();) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte >= 0x80U) {
            std::size_t length = 0;
            if (byte >= 0xC2U && byte <= 0xDFU && continuation(index + 1)) {
                length = 2;
            } else if (byte >= 0xE0U && byte <= 0xEFU && continuation(index + 1)
                       && continuation(index + 2)) {
                const unsigned char second = static_cast<unsigned char>(value[index + 1]);
                if ((byte != 0xE0U || second >= 0xA0U)
                    && (byte != 0xEDU || second <= 0x9FU)) {
                    length = 3;
                }
            } else if (byte >= 0xF0U && byte <= 0xF4U && continuation(index + 1)
                       && continuation(index + 2) && continuation(index + 3)) {
                const unsigned char second = static_cast<unsigned char>(value[index + 1]);
                if ((byte != 0xF0U || second >= 0x90U)
                    && (byte != 0xF4U || second <= 0x8FU)) {
                    length = 4;
                }
            }
            if (length > 0) {
                output.append(value.substr(index, length));
                index += length;
                continue;
            }
            append_hex(byte);
            ++index;
            continue;
        }

        switch (byte) {
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (byte < 0x20U || byte == 0x7FU) {
                    append_hex(byte);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
                break;
        }
        ++index;
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] bool should_skip_directory(std::string_view name)
{
    static constexpr std::array<std::string_view, 20> kSkippedDirectories{
        ".git",          "node_modules",       "build",       "target",
        "__pycache__",   ".cache",             "vendor",      "dist",
        "out",           ".next",              ".venv",       "cmake-build-debug",
        "cmake-build-release", ".idea",        ".vscode",     "_deps",
        "third_party",   "thirdparty",         "3rdparty",    "external",
    };
    return std::find(kSkippedDirectories.begin(), kSkippedDirectories.end(), name)
           != kSkippedDirectories.end();
}

[[nodiscard]] DirectoryContents read_entries(const fs::path& directory)
{
    DirectoryContents contents;
    std::error_code error;
    for (fs::directory_iterator iterator{directory, error}, end; !error && iterator != end;
         iterator.increment(error)) {
        std::error_code status_error;
        const fs::file_status status = iterator->symlink_status(status_error);
        EntryType type = EntryType::Other;
        if (!status_error) {
            if (fs::is_symlink(status)) {
                type = EntryType::Symlink;
            } else if (fs::is_directory(status)) {
                type = EntryType::Directory;
            } else if (fs::is_regular_file(status)) {
                type = EntryType::RegularFile;
            }
        }
        contents.entries.push_back(ListedEntry{
            .entry = *iterator,
            .type = type,
            .metadata_error = status_error
                ? std::optional<std::string>{status_error.message()}
                : std::nullopt,
        });
    }
    if (error) {
        contents.traversal_error = error.message();
    }

    std::sort(
        contents.entries.begin(),
        contents.entries.end(),
        [](const ListedEntry& left, const ListedEntry& right) {
            if (left.type != right.type) {
                return left.type < right.type;
            }
            return left.entry.path().filename().string()
                   < right.entry.path().filename().string();
        });
    return contents;
}

void render_contents(const DirectoryContents& contents, const ListDirRequest& request,
                     std::size_t depth, RenderState& state)
{
    if (state.truncation != Truncation::None) {
        return;
    }
    const std::string indent((depth - 1) * 2, ' ');
    for (const ListedEntry& listed : contents.entries) {
        if (state.truncation != Truncation::None) {
            return;
        }
        const fs::path& path = listed.entry.path();
        const std::string name = path.filename().string();
        std::string line = indent + quote(name);

        if (listed.metadata_error) {
            line += "  [metadata unavailable: " + *listed.metadata_error + "]\n";
            if (!state.append_entry(std::move(line))) {
                return;
            }
            continue;
        }

        switch (listed.type) {
            case EntryType::Directory: {
                line += "/";
                const bool can_recurse = request.recursive && depth < request.max_depth;
                const bool skipped = can_recurse && should_skip_directory(name);
                if (skipped) {
                    line += "  [not expanded]";
                }
                line += "\n";
                if (!state.append_entry(std::move(line))) {
                    return;
                }
                if (can_recurse && !skipped) {
                    const DirectoryContents children = read_entries(path);
                    render_contents(children, request, depth + 1, state);
                }
                break;
            }
            case EntryType::RegularFile: {
                std::error_code size_error;
                const std::uintmax_t size = listed.entry.file_size(size_error);
                line += size_error ? "  ?\n" : "  " + format_size(size) + "\n";
                if (!state.append_entry(std::move(line))) {
                    return;
                }
                break;
            }
            case EntryType::Symlink: {
                std::error_code link_error;
                const fs::path target = fs::read_symlink(path, link_error);
                if (link_error) {
                    line += " -> [unreadable: " + link_error.message() + "]\n";
                } else {
                    line += " -> " + quote(target.string()) + "\n";
                }
                if (!state.append_entry(std::move(line))) {
                    return;
                }
                break;
            }
            case EntryType::Other:
                line += "  [other]\n";
                if (!state.append_entry(std::move(line))) {
                    return;
                }
                break;
        }
    }

    if (contents.traversal_error && state.truncation == Truncation::None) {
        state.append_notice(indent + "[unable to continue listing: "
                            + *contents.traversal_error + "]\n");
    }
}

ExecResult execute_list_dir(const ReadRoots& roots, const nlohmann::json& args)
{
    const auto request = parse_list_dir_request(args);
    if (!request) {
        return std::unexpected(request.error());
    }

    const auto resolved = roots.resolve(fs::path{request->path}, "list_dir");
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    const fs::path& path = *resolved;

    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to inspect path: " + path.string() + " (" + error.message() + ")",
        });
    }
    if (!exists) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::NotFound,
            .message = "directory does not exist: " + path.string(),
        });
    }

    error.clear();
    const bool is_directory = fs::is_directory(path, error);
    if (error) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::IOError,
            .message =
                "failed to inspect file type: " + path.string() + " (" + error.message() + ")",
        });
    }
    if (!is_directory) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::NotADirectory,
            .message = "not a directory: " + path.string(),
        });
    }

    const DirectoryContents root = read_entries(path);
    if (root.entries.empty() && root.traversal_error) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::IOError,
            .message = "failed to list directory: " + path.string() + " ("
                       + *root.traversal_error + ")",
        });
    }
    RenderState state;
    render_contents(root, *request, 1, state);
    if (state.output.empty() && state.truncation == Truncation::None) {
        state.output = "empty directory";
    } else {
        state.finish();
    }
    return ToolOutput{.text = std::move(state.output)};
}

} // namespace

RegisteredTool make_list_dir_tool(std::shared_ptr<ReadRoots> roots)
{
    return RegisteredTool{
        .descriptor =
            ToolDescriptor{
                .name = ToolName{.value = "list_dir"},
                .description =
                    "List a directory within the current session's readable roots. Path "
                    "defaults to the workspace. Set recursive=true to recurse up to 3 levels "
                    "by default; max_depth requires recursive=true and must be from 1 to 16. "
                    "Directory symlinks are shown but never followed.",
                .input_schema =
                    nlohmann::json{
                        {"type", "object"},
                        {"properties",
                         nlohmann::json{
                             {"path",
                              nlohmann::json{
                                  {"type", "string"},
                                  {"minLength", 1},
                                  {"description",
                                   "Directory to list. Defaults to '.' relative to the session "
                                   "workspace and must remain within the session's readable "
                                   "roots."},
                              }},
                             {"recursive",
                              nlohmann::json{
                                  {"type", "boolean"},
                                  {"description",
                                   "Recurse into real subdirectories when true. Defaults to "
                                   "false; directory symlinks are never followed."},
                              }},
                             {"max_depth",
                              nlohmann::json{
                                  {"type", "integer"},
                                  {"minimum", 1},
                                  {"maximum", 16},
                                  {"description",
                                   "Maximum levels below the requested directory. Requires "
                                   "recursive=true; defaults to 3 when recursive."},
                              }},
                         }},
                        {"additionalProperties", false},
                    },
                .effects = {Effect::ReadFs},
                .replay = {},
            },
        .handler = [roots = std::move(roots)](const nlohmann::json& args,
                                               const ToolExecutionContext&) {
            return execute_list_dir(*roots, args);
        },
    };
}

} // namespace my_agent::tool::detail
