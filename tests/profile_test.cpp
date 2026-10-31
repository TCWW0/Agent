#include "my_agent/domain/profile.hpp"
#include "my_agent/runtime/agent.hpp"

#include <utility>
#include <variant>

#include <gtest/gtest.h>

TEST(ProfileTest, SetProfileImmediatelyUpdatesCurrentSession)
{
    my_agent::Model model{};
    EXPECT_EQ(my_agent::Profile::Write, model.domain.profile);

    const my_agent::Step step = my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::SetProfile{
                .profile = my_agent::Profile::Ask,
            },
        }
    );

    EXPECT_EQ(my_agent::Profile::Ask, step.model.domain.profile);
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(step.cmd));
}
