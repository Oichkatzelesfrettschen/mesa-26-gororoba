/*
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdio.h>

#include "c11/threads.h"
#include "r300_r2vb.h"

static unsigned failures;

#define CHECK(condition, name)                                               \
   do {                                                                      \
      if (!(condition)) {                                                    \
         fprintf(stderr, "FAIL %s: %s\n", name, #condition);               \
         failures++;                                                         \
      }                                                                       \
   } while (0)

struct reserve_thread_args {
   unsigned *state;
};

static int
reserve_thread(void *data)
{
   struct reserve_thread_args *args = data;
   return r300_r2vb_submit3_try_reserve(args->state) ? 1 : 0;
}

static void
check_atomic_reservation(void)
{
   enum { reserve_thread_count = 8 };
   thrd_t threads[reserve_thread_count];
   struct reserve_thread_args args;
   unsigned state = R300_R2VB_SUBMIT3_AVAILABLE;
   unsigned created = 0;
   unsigned winners = 0;
   args.state = &state;

   for (; created < reserve_thread_count; created++) {
      if (thrd_create(&threads[created], reserve_thread, &args) !=
          thrd_success) {
         CHECK(false, "reservation threads start");
         break;
      }
   }
   for (unsigned i = 0; i < created; i++) {
      int result = 0;
      CHECK(thrd_join(threads[i], &result) == thrd_success,
            "reservation threads join");
      if (result == 1)
         winners++;
   }
   CHECK(winners == 1, "concurrent reservation has one winner");
   CHECK(state == R300_R2VB_SUBMIT3_IN_PROGRESS,
         "reservation leaves the in-progress state");
   CHECK(!r300_r2vb_submit3_try_reserve(&state),
         "second reservation observes the occupied state");
   CHECK(r300_r2vb_submit3_rollback(&state),
         "owner rollback returns the slot to available");
   CHECK(state == R300_R2VB_SUBMIT3_AVAILABLE,
         "rollback restores the available state");
}

static void
check_submit3_state_transitions(void)
{
   unsigned state = R300_R2VB_SUBMIT3_AVAILABLE;
   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_EMIT,
         "available state emits");
   CHECK(r300_r2vb_submit3_try_reserve(&state),
         "available state reserves atomically");
   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_WAIT,
         "in-progress state waits for the owner");
   CHECK(r300_r2vb_submit3_mark_submitted(&state),
         "in-progress state commits after emission");
   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_CONSUME,
         "submitted state consumes repeated draws");
   CHECK(!r300_r2vb_submit3_mark_submitted(&state),
         "submitted state rejects a second commit");
   CHECK(!r300_r2vb_submit3_rollback(&state),
         "submitted state rejects rollback");
   CHECK(r300_r2vb_submit3_action_for_state(UINT_MAX) ==
            R300_R2VB_SUBMIT3_FALLBACK,
         "unknown state falls back fail closed");

   state = R300_R2VB_SUBMIT3_AVAILABLE;
   CHECK(r300_r2vb_submit3_try_reserve(&state),
         "retry reserves after a declined candidate");
   CHECK(r300_r2vb_submit3_rollback(&state),
         "declined candidate rolls back its reservation");
   CHECK(r300_r2vb_submit3_action_for_state(state) ==
            R300_R2VB_SUBMIT3_EMIT,
         "rollback reopens emission for the next candidate");
}

static void
check_owner_failure_loser_retry(void)
{
   unsigned state = R300_R2VB_SUBMIT3_AVAILABLE;
   CHECK(r300_r2vb_submit3_try_reserve(&state),
         "owner reserves before the loser arrives");
   CHECK(r300_r2vb_submit3_action_for_atomic_state(&state) ==
            R300_R2VB_SUBMIT3_WAIT,
         "loser waits while owner validates");
   CHECK(r300_r2vb_submit3_rollback(&state),
         "owner failure publishes rollback");
   CHECK(r300_r2vb_submit3_action_for_atomic_state(&state) ==
            R300_R2VB_SUBMIT3_EMIT,
         "loser retries after owner rollback");
   CHECK(r300_r2vb_submit3_try_reserve(&state),
         "loser claims the reopened reservation");
   CHECK(r300_r2vb_submit3_action_for_atomic_state(&state) ==
            R300_R2VB_SUBMIT3_WAIT,
         "retry owner remains the sole PM4 candidate");
   CHECK(r300_r2vb_submit3_rollback(&state),
         "retry owner failure restores availability");
}

int
main(void)
{
   check_atomic_reservation();
   check_submit3_state_transitions();
   check_owner_failure_loser_retry();

   if (failures) {
      fprintf(stderr, "r300_r2vb_submit3_state_test: %u failure(s)\n",
              failures);
      return 1;
   }

   printf("r300_r2vb_submit3_state_test: all checks passed\n");
   return 0;
}
