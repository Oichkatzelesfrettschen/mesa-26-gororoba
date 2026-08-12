/*
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdio.h>

#include "r300_r2vb.h"

static unsigned failures;

#define CHECK(condition, name)                                               \
   do {                                                                      \
      if (!(condition)) {                                                    \
         fprintf(stderr, "FAIL %s: %s\n", name, #condition);               \
         failures++;                                                         \
      }                                                                       \
   } while (0)

static void
check_available_candidate(void)
{
   unsigned state = R300_R2VB_SUBMIT3_AVAILABLE;

   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_EMIT,
         "available state emits one candidate");
   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_EMIT,
         "a pre-submit decline keeps the candidate available");
}

static void
check_submitted_repeated_draws(void)
{
   unsigned state = R300_R2VB_SUBMIT3_AVAILABLE;
   r300_r2vb_submit3_mark_submitted(&state);

   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_CONSUME,
         "submitted state consumes the next qualifying draw");
   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_CONSUME,
         "submitted state consumes repeated qualifying draws");
}

static void
check_invalid_state_fails_closed(void)
{
   CHECK(r300_r2vb_submit3_action_for_state(UINT_MAX) ==
            R300_R2VB_SUBMIT3_CONSUME,
         "unknown state consumes instead of emitting");
}

int
main(void)
{
   check_available_candidate();
   check_submitted_repeated_draws();
   check_invalid_state_fails_closed();

   if (failures) {
      fprintf(stderr, "r300_r2vb_submit3_state_test: %u failure(s)\n",
              failures);
      return 1;
   }

   printf("r300_r2vb_submit3_state_test: all checks passed\n");
   return 0;
}
