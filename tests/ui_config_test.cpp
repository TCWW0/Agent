#include "my_agent/ui/ui_config.hpp"

#include <gtest/gtest.h>

namespace {

TEST(UiConfigTest, UsesThreeRowsWhenTheEnvironmentValueIsMissingOrInvalid)
{
    const char* values[] = {
        nullptr, "", "0", "21", "-1", "abc", " 3", "3 "};
    for (const char* value : values) {
        EXPECT_EQ(3, my_agent::ui::UiConfig::from_environment_value(value)
                          .mouse_scroll_rows)
            << (value == nullptr ? "<missing>" : value);
    }
}

TEST(UiConfigTest, AcceptsEveryMouseScrollRowsValueInTheDocumentedRange)
{
    EXPECT_EQ(1, my_agent::ui::UiConfig::from_environment_value("1")
                    .mouse_scroll_rows);
    EXPECT_EQ(20, my_agent::ui::UiConfig::from_environment_value("20")
                     .mouse_scroll_rows);
    EXPECT_EQ(7, my_agent::ui::UiConfig::from_environment_value("7")
                    .mouse_scroll_rows);
}

}  // namespace
