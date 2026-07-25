/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests for mesa_log_level_to_string.
 *
 * level_from_str is a static function in log.c and is not reachable from
 * test code without source modification.  It is exercised indirectly
 * through mesa_log_level_to_string and is not separately tested here.
 */

#include <gtest/gtest.h>
#include <string.h>

#include "util/log.h"

TEST(mesa_log_level_to_string, error_level)
{
   EXPECT_STREQ(mesa_log_level_to_string(MESA_LOG_ERROR), "error");
}

TEST(mesa_log_level_to_string, warn_level)
{
   EXPECT_STREQ(mesa_log_level_to_string(MESA_LOG_WARN), "warning");
}

TEST(mesa_log_level_to_string, info_level)
{
   EXPECT_STREQ(mesa_log_level_to_string(MESA_LOG_INFO), "info");
}

TEST(mesa_log_level_to_string, debug_level)
{
   EXPECT_STREQ(mesa_log_level_to_string(MESA_LOG_DEBUG), "debug");
}
