#pragma once

#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/msg.hpp"

#include <functional>
#include <variant>
namespace my_agent{

    using ProviderEvent = std::variant<
        StreamTextDelta,
        StreamToolCall,
        StreamFinished,
        RequestUsage,
        StreamError
    >;

    using ProviderEventSink = std::function<void(ProviderEvent)>;
    using StreamEffect = std::function<void(Request,ProviderEventSink)>;
}// namespace my_agent
