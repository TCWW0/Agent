#pragma once

#include "my_agent/runtime/agent.hpp"
#include "my_agent/runtime/request_preparation.hpp"
#include <string>
#include <utility>
#include <variant>

namespace my_agent_test {

inline my_agent::Step prepare_request(my_agent::Step step)
{
    const auto* command_ptr = std::get_if<my_agent::PrepareRequest>(&step.cmd);
    if (command_ptr == nullptr) {
        return step;
    }

    const my_agent::PrepareRequest command = *command_ptr;
    my_agent::PreparedRequestSnapshot snapshot =
        my_agent::prepare_request_snapshot(
            command.request,
            {},
            std::nullopt,
                        command.policy,
            command.ledger);

    return my_agent::update(
        std::move(step.model),
        my_agent::Msg{my_agent::RequestPrepared{
            .preparation_id = command.preparation_id,
            .snapshot = std::move(snapshot),
        }});
}

}  // namespace my_agent_test
