#include "my_agent/tool/effects.hpp"

#include <gtest/gtest.h>

TEST(EffectSetTest, CombinedSetReportsOnlyDeclaredEffects)
{
    const my_agent::tool::EffectSet effects{
        my_agent::tool::Effect::ReadFs,
        my_agent::tool::Effect::Net,
    };

    EXPECT_TRUE(effects.has(my_agent::tool::Effect::ReadFs));
    EXPECT_FALSE(effects.has(my_agent::tool::Effect::WriteFs));
    EXPECT_TRUE(effects.has(my_agent::tool::Effect::Net));
    EXPECT_FALSE(effects.has(my_agent::tool::Effect::Exec));
}
