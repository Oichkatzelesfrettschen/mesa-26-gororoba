/* SPDX-License-Identifier: MIT */

#ifndef R300_ZMASK_MATERIALIZE_PLAN_H
#define R300_ZMASK_MATERIALIZE_PLAN_H

#include "r300_zmask_layout.h"
#include "r300_zb_depth_surface.h"

#include <stdint.h>

#define R300_ZMASK_MATERIALIZE_PLAN_MAX_DWORDS 16u

struct r300_zmask_materialize_plan {
   uint32_t words[R300_ZMASK_MATERIALIZE_PLAN_MAX_DWORDS];
   uint32_t begin_dword_count;
   uint32_t dword_count;
   uint32_t clear_word;
};

int r300_zmask_materialize_prefix(
   const struct r300_zb_depth_surface *surface,
   const struct r300_zmask_layout *layout, uint32_t depth_code,
   uint32_t stencil, struct r300_zmask_materialize_plan *out);

int r300_zmask_materialize_suffix(struct r300_zmask_materialize_plan *out);

/* The shared bind closed by a caller-chosen ZB_BW_CNTL group drawn from
 * FAST_FILL_ENABLE, RD_COMP_ENABLE and WR_COMP_ENABLE, for a discovery
 * cell that holds the bind fixed and moves the group alone.  A bit outside
 * the three, or the rule-8 combination r300_zmask_clear_bw_cntl_check
 * refuses, is -EINVAL.  The suffix is r300_zmask_materialize_suffix.
 */
int r300_zmask_read_prefix_at_group(
   const struct r300_zb_depth_surface *surface,
   const struct r300_zmask_layout *layout, uint32_t depth_code,
   uint32_t stencil, uint32_t zb_bw_cntl,
   struct r300_zmask_materialize_plan *out);

/* Builds the complete register wrapper for a depth read that consumes a
 * ZMASK fast-clear value directly.  The prefix binds the metadata at the
 * layout's own block and enables FAST_FILL with read and write
 * compression, where r300_zmask_materialize_prefix stops at FAST_FILL
 * plus read compression for the write-enabled materialize draw; the
 * suffix flushes the Z cache and restores the compression-disabled
 * state, where 4x4 plane equations are what the R5xx acceleration guide
 * requires.  The compressed metadata representation has a distinct
 * lifecycle status and never reaches this plan.
 */
int r300_zmask_fast_clear_read_plan(
   const struct r300_zb_depth_surface *surface,
   const struct r300_zmask_layout *layout, uint32_t depth_code,
   uint32_t stencil, struct r300_zmask_materialize_plan *out);

static inline const uint32_t *
r300_zmask_materialize_end_words(
   const struct r300_zmask_materialize_plan *plan)
{
   return plan->words + plan->begin_dword_count;
}

static inline uint32_t
r300_zmask_materialize_end_dword_count(
   const struct r300_zmask_materialize_plan *plan)
{
   return plan->dword_count - plan->begin_dword_count;
}

#endif
