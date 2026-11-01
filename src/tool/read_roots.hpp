#pragma once

#include "my_agent/tool/tool_protocol.hpp"

#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace my_agent::tool::detail {

// The readable filesystem boundary shared by every read-only tool in one
// session. Expanding these roots never grants write access.
class ReadRoots {
public:
    explicit ReadRoots(
        const std::filesystem::path& workspace_root,
        bool allow_temporary_root = true);

    [[nodiscard]] const std::filesystem::path& workspace_root() const noexcept;
    [[nodiscard]] std::expected<void, std::string> add(
        const std::filesystem::path& root);
    [[nodiscard]] std::expected<std::filesystem::path, ToolError> resolve(
        const std::filesystem::path& requested,
        std::string_view operation) const;

private:
    [[nodiscard]] bool allows(
        const std::filesystem::path& canonical_target) const;

    std::filesystem::path workspace_root_;
    mutable std::mutex mutex_;
    std::vector<std::filesystem::path> allowed_roots_;
};

} // namespace my_agent::tool::detail
