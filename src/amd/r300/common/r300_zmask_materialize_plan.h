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
