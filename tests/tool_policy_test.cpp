#include "my_agent/domain/profile.hpp"
#include "my_agent/tool/effects.hpp"
#include "my_agent/tool/policy.hpp"
#include "../src/tool/calculator.hpp"

#include <gtest/gtest.h>

TEST(ToolPolicyTest, ExecEffectIsAllowedOnlyByWriteProfile)
{
    const my_agent::tool::EffectSet exec_effects{
        my_agent::tool::Effect::Exec,
    };

    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            exec_effects,
            my_agent::Profile::Write
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            exec_effects,
            my_agent::Profile::Ask
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            exec_effects,
            my_agent::Profile::Minimal
        )
    );
}

TEST(ToolPolicyTest, ReadFsEffectIsPromptedOnlyByMinimalProfile)
{
    const my_agent::tool::EffectSet read_effects{
        my_agent::tool::Effect::ReadFs,
    };

    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            read_effects,
            my_agent::Profile::Write
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            read_effects,
            my_agent::Profile::Ask
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            read_effects,
            my_agent::Profile::Minimal
        )
    );
}

TEST(ToolPolicyTest, WriteFsEffectIsAllowedOnlyByWriteProfile)
{
    const my_agent::tool::EffectSet write_effects{
        my_agent::tool::Effect::WriteFs,
    };

    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            write_effects,
            my_agent::Profile::Write
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            write_effects,
            my_agent::Profile::Ask
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            write_effects,
            my_agent::Profile::Minimal
        )
    );
}

TEST(ToolPolicyTest, NetEffectIsAllowedOnlyByWriteProfile)
{
    const my_agent::tool::EffectSet net_effects{
        my_agent::tool::Effect::Net,
    };

    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            net_effects,
            my_agent::Profile::Write
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            net_effects,
            my_agent::Profile::Ask
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            net_effects,
            my_agent::Profile::Minimal
        )
    );
}

TEST(ToolPolicyTest, CalculatorIsPureAndAllowedByEveryProfile)
{
    const my_agent::tool::RegisteredTool calculator =
        my_agent::tool::detail::make_calculator_tool();
    EXPECT_EQ(my_agent::tool::EffectSet{}, calculator.descriptor.effects);

    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            calculator.descriptor.effects,
            my_agent::Profile::Write
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            calculator.descriptor.effects,
            my_agent::Profile::Ask
        )
    );
    EXPECT_EQ(
        my_agent::tool::policy::Decision::Allow,
        my_agent::tool::policy::permission(
            calculator.descriptor.effects,
            my_agent::Profile::Minimal
        )
    );
}

TEST(ToolPolicyTest, AnyPromptingEffectMakesCombinedEffectSetPrompt)
{
    const my_agent::tool::EffectSet read_and_net_effects{
        my_agent::tool::Effect::ReadFs,
        my_agent::tool::Effect::Net,
    };

    EXPECT_EQ(
        my_agent::tool::policy::Decision::Prompt,
        my_agent::tool::policy::permission(
            read_and_net_effects,
            my_agent::Profile::Ask
        )
    );
}
