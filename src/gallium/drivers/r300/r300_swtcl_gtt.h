/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_SWTCL_GTT_H
#define R300_SWTCL_GTT_H

#include <stdbool.h>
#include <stdint.h>

struct r300_swtcl_gtt_backpressure {
   uint64_t drain_mark;
   unsigned drain_streak;
   bool initialized;
   bool budget_exceeded;
};

static inline void
r300_swtcl_gtt_begin_draw(struct r300_swtcl_gtt_backpressure *state)
{
   state->drain_streak = 0;
   state->budget_exceeded = false;
}

static inline bool
r300_swtcl_gtt_needs_drain(struct r300_swtcl_gtt_backpressure *state,
                           uint64_t usage, uint64_t budget)
{
   if (!state->initialized) {
      state->drain_mark = usage;
      state->initialized = true;
      return false;
   }

   if (usage > state->drain_mark && usage - state->drain_mark > budget)
      return true;

   if (usage < state->drain_mark) {
      state->drain_mark = usage;
      state->drain_streak = 0;
   }

   return false;
}

static inline void
r300_swtcl_gtt_record_drain(struct r300_swtcl_gtt_backpressure *state,
                            uint64_t usage_after_drain,
                            unsigned drain_streak_cap)
{
   if (usage_after_drain < state->drain_mark)
      state->drain_streak = 0;
   else
      state->drain_streak++;

   state->drain_mark = usage_after_drain;
   state->initialized = true;
   if (state->drain_streak >= drain_streak_cap)
      state->budget_exceeded = true;
}

#endif
