#include "calculator.hpp"
#include <cmath>
#include <format>
#include <string>
#include <nlohmann/json.hpp>

namespace my_agent::tool::detail {

namespace {
    ExecResult execute_calculator(const nlohmann::json& args)
    {
        if (!args.is_object()
            || !args.contains("operation")
            || !args["operation"].is_string()
            || !args.contains("left")
            || !args["left"].is_number()
            || !args.contains("right")
            || !args["right"].is_number()) {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "calculator requires operation, left, and right",
            });
        }

        const std::string operation = args["operation"].get<std::string>();
        if (operation != "multiply") {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "unsupported calculator operation: " + operation,
            });
        }

        const double value =
            args["left"].get<double>() * args["right"].get<double>();
        if (!std::isfinite(value)) {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "calculator result is not finite",
            });
        }

        return ToolOutput{.text = std::format("{}", value)};
    }
}

    RegisteredTool make_calculator_tool()
    {
        return RegisteredTool{
            .descriptor = ToolDescriptor{
                .name = ToolName{.value = "calculator"},
                .description = "Multiply two numbers.",
                .input_schema = nlohmann::json{
                {"type", "object"},
                {"properties", nlohmann::json{
                    {"operation", nlohmann::json{
                        {"type", "string"},
                        {"enum", nlohmann::json::array({"multiply"})},
                    }},
                    {"left", nlohmann::json{{"type", "number"}}},
                    {"right", nlohmann::json{{"type", "number"}}},
                }},
                {"required", nlohmann::json::array({
                    "operation",
                    "left",
                    "right",
                })},
                {"additionalProperties", false},
            },
                .effects = {},
                .replay = {},
            },
            .handler = [](
                           const nlohmann::json& args,
                           const ToolExecutionContext&) {
                           return execute_calculator(args);
                       },
        };
    }
}
