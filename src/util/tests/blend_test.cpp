/*
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include "util/blend.h"

/* ------------------------------------------------------------------ */
/* util_blendfactor_is_inverted                                        */
/* ------------------------------------------------------------------ */

TEST(util_blendfactor_is_inverted, non_inverted_factors)
{
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_ONE));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_SRC_COLOR));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_SRC_ALPHA));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_DST_ALPHA));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_DST_COLOR));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_CONST_COLOR));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_CONST_ALPHA));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_SRC1_COLOR));
   EXPECT_FALSE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_SRC1_ALPHA));
}

TEST(util_blendfactor_is_inverted, inverted_factors)
{
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_ZERO));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_SRC_COLOR));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_SRC_ALPHA));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_DST_ALPHA));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_DST_COLOR));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_CONST_COLOR));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_CONST_ALPHA));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_SRC1_COLOR));
   EXPECT_TRUE(util_blendfactor_is_inverted(PIPE_BLENDFACTOR_INV_SRC1_ALPHA));
}

/* ------------------------------------------------------------------ */
/* util_blendfactor_without_invert                                     */
/* ------------------------------------------------------------------ */

TEST(util_blendfactor_without_invert, inverted_maps_to_base)
{
   /* Each INV_ form must strip PIPE_BLENDFACTOR_INVERT_BIT and yield
    * the corresponding non-inverted factor. */
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_ZERO),
             PIPE_BLENDFACTOR_ONE);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_SRC_COLOR),
             PIPE_BLENDFACTOR_SRC_COLOR);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_SRC_ALPHA),
             PIPE_BLENDFACTOR_SRC_ALPHA);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_DST_ALPHA),
             PIPE_BLENDFACTOR_DST_ALPHA);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_DST_COLOR),
             PIPE_BLENDFACTOR_DST_COLOR);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_CONST_COLOR),
             PIPE_BLENDFACTOR_CONST_COLOR);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_CONST_ALPHA),
             PIPE_BLENDFACTOR_CONST_ALPHA);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_SRC1_COLOR),
             PIPE_BLENDFACTOR_SRC1_COLOR);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_INV_SRC1_ALPHA),
             PIPE_BLENDFACTOR_SRC1_ALPHA);
}

TEST(util_blendfactor_without_invert, non_inverted_unchanged)
{
   /* Factors without INVERT_BIT are returned as-is. */
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_ONE),
             PIPE_BLENDFACTOR_ONE);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_SRC_COLOR),
             PIPE_BLENDFACTOR_SRC_COLOR);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_SRC_ALPHA),
             PIPE_BLENDFACTOR_SRC_ALPHA);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_DST_ALPHA),
             PIPE_BLENDFACTOR_DST_ALPHA);
   EXPECT_EQ(util_blendfactor_without_invert(PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE),
             PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE);
}

/* ------------------------------------------------------------------ */
/* util_blendfactor_to_alpha                                           */
/* ------------------------------------------------------------------ */

TEST(util_blendfactor_to_alpha, color_maps_to_alpha)
{
   /* COLOR factors convert to their ALPHA counterpart. */
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_SRC_COLOR),
             PIPE_BLENDFACTOR_SRC_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_DST_COLOR),
             PIPE_BLENDFACTOR_DST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_CONST_COLOR),
             PIPE_BLENDFACTOR_CONST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_SRC1_COLOR),
             PIPE_BLENDFACTOR_SRC1_ALPHA);
}

TEST(util_blendfactor_to_alpha, inv_color_maps_to_inv_alpha)
{
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_SRC_COLOR),
             PIPE_BLENDFACTOR_INV_SRC_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_DST_COLOR),
             PIPE_BLENDFACTOR_INV_DST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_CONST_COLOR),
             PIPE_BLENDFACTOR_INV_CONST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_SRC1_COLOR),
             PIPE_BLENDFACTOR_INV_SRC1_ALPHA);
}

TEST(util_blendfactor_to_alpha, passthrough_factors)
{
   /* Alpha factors and other non-COLOR factors return themselves. */
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_ONE),
             PIPE_BLENDFACTOR_ONE);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_ZERO),
             PIPE_BLENDFACTOR_ZERO);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_SRC_ALPHA),
             PIPE_BLENDFACTOR_SRC_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_DST_ALPHA),
             PIPE_BLENDFACTOR_DST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE),
             PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_CONST_ALPHA),
             PIPE_BLENDFACTOR_CONST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_SRC1_ALPHA),
             PIPE_BLENDFACTOR_SRC1_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_SRC_ALPHA),
             PIPE_BLENDFACTOR_INV_SRC_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_DST_ALPHA),
             PIPE_BLENDFACTOR_INV_DST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_CONST_ALPHA),
             PIPE_BLENDFACTOR_INV_CONST_ALPHA);
   EXPECT_EQ(util_blendfactor_to_alpha(PIPE_BLENDFACTOR_INV_SRC1_ALPHA),
             PIPE_BLENDFACTOR_INV_SRC1_ALPHA);
}

/* ------------------------------------------------------------------ */
/* util_blend_dst_alpha_to_one                                         */
/* ------------------------------------------------------------------ */

TEST(util_blend_dst_alpha_to_one, dst_alpha_becomes_one)
{
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_DST_ALPHA),
             PIPE_BLENDFACTOR_ONE);
}

TEST(util_blend_dst_alpha_to_one, inv_dst_alpha_becomes_zero)
{
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_INV_DST_ALPHA),
             PIPE_BLENDFACTOR_ZERO);
}

TEST(util_blend_dst_alpha_to_one, other_factors_pass_through)
{
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_ONE),
             PIPE_BLENDFACTOR_ONE);
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_ZERO),
             PIPE_BLENDFACTOR_ZERO);
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_SRC_ALPHA),
             PIPE_BLENDFACTOR_SRC_ALPHA);
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_INV_SRC_ALPHA),
             PIPE_BLENDFACTOR_INV_SRC_ALPHA);
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_SRC_COLOR),
             PIPE_BLENDFACTOR_SRC_COLOR);
   EXPECT_EQ(util_blend_dst_alpha_to_one(PIPE_BLENDFACTOR_CONST_ALPHA),
             PIPE_BLENDFACTOR_CONST_ALPHA);
}
