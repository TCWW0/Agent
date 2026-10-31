#pragma once

#include "my_agent/tool/effects.hpp"
#include <cstdint>
#include <expected>
#include <functional>
#include <string>

namespace my_agent::tool {

struct ToolOutput {
    std::string text;
};

enum class ErrorKind {
    InvalidArgs,
    NotFound,
    OutOfWorkspace,
    Binary,
    NotAFile,
    NotADirectory,
    TooLarge,
    IOError,
    ExecutionFailed,
};

struct ToolError {
    ErrorKind kind;
    std::string message;

    [[nodiscard]]
    std::string render() const;
};

using ExecResult = std::expected<ToolOutput, ToolError>;

struct ToolName {
    std::string value;
    friend bool operator==(const ToolName&, const ToolName&) = default;
};

// --- 会话内身份 ---------------------------------------------
struct ToolRef {
    std::uint64_t value{0};
    friend bool operator==(const ToolRef& , const ToolRef&) = default;
};

struct ToolBinding {
    ToolRef ref{};
    EffectSet effects{};
};

struct ToolResolutionError {
    std::string name;
    std::string message;
};

struct ToolExecutionContext {
    std::function<bool()> cancelled;

    [[nodiscard]]
    bool cancellation_requested() const
    {
        return cancelled && cancelled();
    }
};

} // namespace my_agent::tool
