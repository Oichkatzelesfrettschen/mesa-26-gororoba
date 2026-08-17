/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "r300_swtcl_gtt.h"

static unsigned failures;

#define CHECK(condition, name)            \
   do {                                   \
      if (condition) {                    \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         failures++;                      \
      }                                   \
   } while (0)

int
main(void)
{
   const uint64_t budget = 32ull * 1024 * 1024;
   const unsigned streak_cap = 4;
   struct r300_swtcl_gtt_backpressure state = {0};

   r300_swtcl_gtt_begin_draw(&state);
   CHECK(!r300_swtcl_gtt_needs_drain(&state, 0, budget) &&
            state.initialized && state.drain_mark == 0,
         "zero GTT usage initializes the baseline");
   CHECK(r300_swtcl_gtt_needs_drain(&state, budget + 1, budget),
         "growth after a zero baseline crosses the budget");

   r300_swtcl_gtt_record_drain(&state, budget + 1, streak_cap);
   CHECK(state.drain_streak == 1 && !state.budget_exceeded,
         "an ineffective drain starts the streak");
   CHECK(!r300_swtcl_gtt_needs_drain(&state, budget + budget / 2, budget) &&
            state.drain_streak == 1,
         "under-budget growth preserves the drain streak");

   CHECK(!r300_swtcl_gtt_needs_drain(&state, budget, budget) &&
            state.drain_streak == 0 && state.drain_mark == budget,
         "usage below the post-drain mark clears the streak");

   state.drain_mark = 1024;
   state.drain_streak = 2;
   r300_swtcl_gtt_record_drain(&state, 512, streak_cap);
   CHECK(state.drain_streak == 0 && state.drain_mark == 512,
         "a reclaiming drain clears the streak immediately");

   state.drain_mark = 0;
   state.drain_streak = 0;
   for (unsigned drain = 0; drain < streak_cap; drain++) {
      uint64_t usage = state.drain_mark + budget + 1;
      CHECK(r300_swtcl_gtt_needs_drain(&state, usage, budget),
            "unbounded growth requests another drain");
      r300_swtcl_gtt_record_drain(&state, usage, streak_cap);
   }
   CHECK(state.budget_exceeded && state.drain_streak == streak_cap,
         "the ineffective-drain cap rejects the draw");

   r300_swtcl_gtt_record_drain(&state, state.drain_mark - 1, streak_cap);
   CHECK(state.budget_exceeded && state.drain_streak == 0,
         "rejection remains latched through the rest of the draw");

   uint64_t retained_mark = state.drain_mark;
   r300_swtcl_gtt_begin_draw(&state);
   CHECK(!state.budget_exceeded && state.drain_streak == 0 &&
            state.initialized && state.drain_mark == retained_mark,
         "the API draw boundary resets rejection and retains the baseline");

   printf("%s\n", failures ? "FAILED" : "PASSED");
   return failures ? 1 : 0;
}
