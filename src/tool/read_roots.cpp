#include "read_roots.hpp"

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace my_agent::tool::detail {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool is_within_root(const fs::path& normalized_root,
                                  const fs::path& normalized_target)
{
    auto root_part = normalized_root.begin();
    auto target_part = normalized_target.begin();
    for (; root_part != normalized_root.end() && target_part != normalized_target.end();
         ++root_part, ++target_part) {
        if (*root_part != *target_part) {
            return false;
        }
    }
    return root_part == normalized_root.end();
}

} // namespace

ReadRoots::ReadRoots(
    const fs::path& workspace_root,
    bool allow_temporary_root)
{
    std::error_code error;
    workspace_root_ = fs::canonical(workspace_root, error);
    const bool is_directory = !error && fs::is_directory(workspace_root_, error);
    if (error || !is_directory) {
        throw std::invalid_argument{
            "readable workspace root must be an existing directory: "
            + workspace_root.string()};
    }
    if (allow_temporary_root) {
        auto _ = add(fs::path{"/tmp"});
    }
}

const fs::path& ReadRoots::workspace_root() const noexcept { return workspace_root_; }

std::expected<fs::path, ToolError> ReadRoots::resolve(
    const fs::path& requested_path,
    std::string_view operation) const
{
    fs::path requested = requested_path;
    if (requested.is_relative()) {
        requested = workspace_root_ / requested;
    }

    std::error_code error;
    fs::path target = fs::weakly_canonical(requested, error);
    const std::string prefix{operation};
    if (error || target.empty()) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::OutOfWorkspace,
            .message = prefix + " could not prove path is within readable roots: "
                       + requested.string() + " (workspace: " + workspace_root_.string() + ")",
        });
    }
    if (allows(target)) {
        return target;
    }
    return std::unexpected(ToolError{
        .kind = ErrorKind::OutOfWorkspace,
        .message = prefix + " refused path outside readable roots: " + target.string()
                   + " (workspace: " + workspace_root_.string() + ")",
    });
}

std::expected<void, std::string> ReadRoots::add(const fs::path& root)
{
    std::error_code error;
    fs::path canonical = fs::canonical(root, error);
    const bool is_directory = !error && fs::is_directory(canonical, error);
    if (error || !is_directory) {
        return std::unexpected{
            "readable root must be an existing directory: " + root.string()};
    }
    std::lock_guard lock{mutex_};
    for (const fs::path& existing : allowed_roots_) {
        if (existing == canonical) {
            return {};
        }
    }
    allowed_roots_.push_back(std::move(canonical));
    return {};
}

bool ReadRoots::allows(const fs::path& canonical_target) const
{
    if (is_within_root(workspace_root_, canonical_target)) {
        return true;
    }
    std::lock_guard lock{mutex_};
    for (const fs::path& root : allowed_roots_) {
        if (is_within_root(root, canonical_target)) {
            return true;
        }
    }
    return false;
}

} // namespace my_agent::tool::detail
