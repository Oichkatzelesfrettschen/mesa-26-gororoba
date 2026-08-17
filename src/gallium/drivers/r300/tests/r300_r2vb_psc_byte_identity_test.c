/*
 * SPDX-License-Identifier: MIT
 *
 * Byte-identity pin between the legacy pipe-format PSC word construction
 * and the neutral vertex-format identity.  The R2VB producer interface
 * routes through the neutral table, and this test holds every word it
 * emits bit-equal to the r300_translate_vertex_data_type/swizzle
 * construction it replaced, for each admitted F32 width.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "amd/r300/common/r300_r2vb_source_contract.h"
#include "r300_defines.h"
#include "r300_reg.h"
#include "r300_state_inlines.h"

static uint32_t
legacy_element(enum pipe_format format, unsigned dst_vec_loc, bool last)
{
   uint16_t type = r300_translate_vertex_data_type(format);
   assert(type != R300_INVALID_FORMAT);
   return (uint32_t)type | (dst_vec_loc << R300_DST_VEC_LOC_SHIFT) |
          (last ? R300_LAST_VEC : 0);
}

static uint32_t
neutral_element(enum r300_vertex_format_id id, unsigned dst_vec_loc,
                bool last)
{
   const struct r300_vertex_format_semantics *semantics =
      r300_vertex_format_semantics(id);
   assert(semantics != NULL);
   return (uint32_t)semantics->data_type |
          (dst_vec_loc << R300_DST_VEC_LOC_SHIFT) |
          (last ? R300_LAST_VEC : 0);
}

static void
check_width(enum pipe_format format, enum r300_vertex_format_id id)
{
   /* Control word: legacy and neutral constructions agree at every
    * destination location and LAST_VEC placement the producer uses.
    */
   assert(legacy_element(format, 0, false) == neutral_element(id, 0, false));
   assert(legacy_element(format, 6, true) == neutral_element(id, 6, true));

   /* Swizzle word: the synthesized-lane selectors agree with the legacy
    * FP_ZERO-then-FP_ONE fill.
    */
   uint16_t legacy_swizzle =
      (uint16_t)r300_translate_vertex_data_swizzle(format);
   uint16_t neutral_swizzle =
      r300_vertex_format_psc_swizzle(r300_vertex_format_semantics(id));
   assert(legacy_swizzle == neutral_swizzle);
}

static void
test_admitted_widths_are_byte_identical(void)
{
   check_width(PIPE_FORMAT_R32G32B32_FLOAT, R300_VERTEX_FORMAT_F32_3);
   check_width(PIPE_FORMAT_R32G32B32A32_FLOAT, R300_VERTEX_FORMAT_F32_4);
   /* The gated F32_2 source keeps the same identity for the day its route
    * opens; pinning it now keeps the capture fixture honest.
    */
   check_width(PIPE_FORMAT_R32G32_FLOAT, R300_VERTEX_FORMAT_F32_2);
}

static void
test_known_bad_words_differ(void)
{
   /* A flipped LAST_VEC, a wrong destination, and a wrong W selector each
    * change the word, so the identity check cannot pass on a corrupted
    * construction.
    */
   assert(neutral_element(R300_VERTEX_FORMAT_F32_3, 6, true) !=
          neutral_element(R300_VERTEX_FORMAT_F32_3, 6, false));
   assert(neutral_element(R300_VERTEX_FORMAT_F32_4, 0, false) !=
          neutral_element(R300_VERTEX_FORMAT_F32_4, 1, false));
   assert(r300_vertex_format_psc_swizzle(
             r300_vertex_format_semantics(R300_VERTEX_FORMAT_F32_3)) !=
          r300_vertex_format_psc_swizzle(
             r300_vertex_format_semantics(R300_VERTEX_FORMAT_F32_4)));
   /* The packed producer pair puts LAST_VEC only on the model element. */
   uint32_t pair =
      neutral_element(R300_VERTEX_FORMAT_F32_4, 0, false) |
      (neutral_element(R300_VERTEX_FORMAT_F32_3, 6, true) << 16);
   assert(!(pair & R300_LAST_VEC));
   assert(pair & (R300_LAST_VEC << 16));
}

int
main(void)
{
   test_admitted_widths_are_byte_identical();
   test_known_bad_words_differ();
   printf("r300_r2vb_psc_byte_identity_test: all checks passed\n");
   return 0;
}
