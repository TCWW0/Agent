#include "my_agent/ui/terminal_session.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

TEST(TerminalSessionTest, ReportsNonTtyAndExposesItsInputFd)
{
    const int devnull = ::open("/dev/null", O_WRONLY);
    ASSERT_GE(devnull, 0);

    {
        my_agent::ui::TerminalSession session{devnull, devnull};
        EXPECT_FALSE(session.is_tty());
        EXPECT_EQ(session.input_fd(), devnull);
    }

    ::close(devnull);
}
