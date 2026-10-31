#include "my_agent/tool/tool_protocol.hpp"

namespace my_agent::tool {

std::string ToolError::render() const
{
    switch (kind) {
        case ErrorKind::InvalidArgs:
            return "[invalid args] " + message;
        case ErrorKind::NotFound:
            return "[not found] " + message;
        case ErrorKind::OutOfWorkspace:
            return "[out of workspace] " + message;
        case ErrorKind::ExecutionFailed:
            return "[execution failed] " + message;
        case ErrorKind::NotAFile:
            return "[not a file] " + message;
        case ErrorKind::NotADirectory:
            return "[not a directory] " + message;
        case ErrorKind::IOError:
            return "[error in IO] " + message;
        case ErrorKind::Binary:
            return "[binary] " + message;
        case ErrorKind::TooLarge:
            return "[too large] " + message;
    }
    return message;
}

}  // namespace my_agent::tool
