/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Edge-case tests for the format-helper predicates in u_format.c and u_format.h.
 *
 * PIPE_FORMAT_COUNT safety note:
 *   util_format_table.py line 600 generates:
 *     assert(format >= 0 && format < PIPE_FORMAT_COUNT);
 *   followed by a direct array index into util_format_descriptions[].
 *   Passing PIPE_FORMAT_COUNT fires the assert in debug builds and
 *   is an out-of-bounds access (UB) in release builds.  It is therefore
 *   not tested here.  PIPE_FORMAT_NONE (value 0) is the correct boundary
 *   probe: it is a valid table entry, all four channels are VOID, and
 *   every predicate that starts from get_first_non_void_channel reaches
 *   its i==-1 early-return path via NONE.
 */

#include <gtest/gtest.h>

#include "util/format/u_format.h"

/* ------------------------------------------------------------------ */
/* util_format_is_pure_uint                                            */
/* ------------------------------------------------------------------ */

TEST(util_format_is_pure_uint, none_returns_false)
{
   EXPECT_FALSE(util_format_is_pure_uint(PIPE_FORMAT_NONE));
}

TEST(util_format_is_pure_uint, uint_format_returns_true)
{
   /* R32G32B32A32_UINT has type UNSIGNED + pure_integer on all channels. */
   EXPECT_TRUE(util_format_is_pure_uint(PIPE_FORMAT_R32G32B32A32_UINT));
}

TEST(util_format_is_pure_uint, sint_format_returns_false)
{
   /* SINT is signed integer, not unsigned integer. */
   EXPECT_FALSE(util_format_is_pure_uint(PIPE_FORMAT_R32G32B32A32_SINT));
}

TEST(util_format_is_pure_uint, unorm_format_returns_false)
{
   /* UNORM channels are not pure_integer. */
   EXPECT_FALSE(util_format_is_pure_uint(PIPE_FORMAT_R8G8B8A8_UNORM));
}

/* ------------------------------------------------------------------ */
/* util_format_is_snorm                                                */
/* ------------------------------------------------------------------ */

TEST(util_format_is_snorm, none_returns_false)
{
   EXPECT_FALSE(util_format_is_snorm(PIPE_FORMAT_NONE));
}

TEST(util_format_is_snorm, snorm_format_returns_true)
{
   EXPECT_TRUE(util_format_is_snorm(PIPE_FORMAT_R8G8B8A8_SNORM));
}

TEST(util_format_is_snorm, unorm_format_returns_false)
{
   EXPECT_FALSE(util_format_is_snorm(PIPE_FORMAT_R8G8B8A8_UNORM));
}

TEST(util_format_is_snorm, uint_format_returns_false)
{
   EXPECT_FALSE(util_format_is_snorm(PIPE_FORMAT_R32G32B32A32_UINT));
}

/* ------------------------------------------------------------------ */
/* util_format_is_snorm8                                               */
/* ------------------------------------------------------------------ */

TEST(util_format_is_snorm8, none_returns_false)
{
   EXPECT_FALSE(util_format_is_snorm8(PIPE_FORMAT_NONE));
}

TEST(util_format_is_snorm8, snorm8_format_returns_true)
{
   /* R8G8B8A8_SNORM: signed, normalized, 8-bit channels, not mixed. */
   EXPECT_TRUE(util_format_is_snorm8(PIPE_FORMAT_R8G8B8A8_SNORM));
}

TEST(util_format_is_snorm8, snorm16_format_returns_false)
{
   /* R16G16B16A16_SNORM has channel size 16, not 8. */
   EXPECT_FALSE(util_format_is_snorm8(PIPE_FORMAT_R16G16B16A16_SNORM));
}

TEST(util_format_is_snorm8, unorm8_format_returns_false)
{
   /* UNORM is not signed-normalized. */
   EXPECT_FALSE(util_format_is_snorm8(PIPE_FORMAT_R8G8B8A8_UNORM));
}

/* ------------------------------------------------------------------ */
/* util_format_is_unorm8                                               */
/* ------------------------------------------------------------------ */

TEST(util_format_is_unorm8, none_returns_false)
{
   /* PIPE_FORMAT_NONE has no non-void channel; get_first_non_void_channel
    * returns -1, so the predicate returns false. */
   const struct util_format_description *desc =
      util_format_description(PIPE_FORMAT_NONE);
   EXPECT_FALSE(util_format_is_unorm8(desc));
}

TEST(util_format_is_unorm8, unorm8_format_returns_true)
{
   const struct util_format_description *desc =
      util_format_description(PIPE_FORMAT_R8G8B8A8_UNORM);
   EXPECT_TRUE(util_format_is_unorm8(desc));
}

TEST(util_format_is_unorm8, unorm16_format_returns_false)
{
   /* R16G16B16A16_UNORM has channel size 16, not 8. */
   const struct util_format_description *desc =
      util_format_description(PIPE_FORMAT_R16G16B16A16_UNORM);
   EXPECT_FALSE(util_format_is_unorm8(desc));
}

TEST(util_format_is_unorm8, snorm8_format_returns_false)
{
   /* SNORM is not unorm. */
   const struct util_format_description *desc =
      util_format_description(PIPE_FORMAT_R8G8B8A8_SNORM);
   EXPECT_FALSE(util_format_is_unorm8(desc));
}
