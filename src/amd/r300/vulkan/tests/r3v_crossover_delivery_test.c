/* SPDX-License-Identifier: MIT
 *
 * Holds the delivery sequence's stage order and its two intervals.
 *
 * Every operation advances one virtual clock by its own distinct amount,
 * so the intervals the sequence publishes are exact sums of the stages
 * they enclose rather than a range a noisy timer happens to fall in.
 * delivery therefore equals DELAY + SUBMIT + AWAIT + VISIBLE and
 * transport equals DELAY + SUBMIT + AWAIT, and moving any one of those
 * stages across a timestamp changes a sum this test states literally.
 *
 * The verdicts stand outside assert(), so a release build with NDEBUG
 * set judges the same predicates as a debug build.
 */

#include "r3v_crossover_delivery.h"

#include <stdio.h>
#include <string.h>

/* One stage's advance.  The values are distinct and share no sum with
 * any other subset, so an interval identifies exactly which stages it
 * enclosed: 1 + 2 + 4 + ... admits one decomposition. */
#define ADVANCE_INITIALIZE 1000u
#define ADVANCE_RECORD 2000u
/* A clock read costs nothing in the model, so each interval is the sum
 * of the operation stages it encloses and nothing else. */
#define ADVANCE_CLOCK 0u
#define ADVANCE_DELAY 8000u
#define ADVANCE_SUBMIT 16000u
#define ADVANCE_AWAIT 32000u
#define ADVANCE_VISIBLE 64000u
#define ADVANCE_VERIFY 128000u

#define MAX_EVENTS 32

struct harness {
   uint64_t now;
   enum r3v_crossover_delivery_stage events[MAX_EVENTS];
   uint32_t event_count;
   /* The stage this run refuses at, or COUNT for a run that completes. */
   enum r3v_crossover_delivery_stage refuse_at;
   /* A clock that steps backward on this read index, for the monotonicity
    * predicates.  UINT32_MAX leaves the clock rising. */
   uint32_t reverse_on_read;
   uint32_t clock_reads;
};

static int failures;

static void
check(bool condition, const char *what)
{
   if (condition)
      return;
   fprintf(stderr, "FAIL: %s\n", what);
   failures++;
}

static void
record_event(struct harness *h, enum r3v_crossover_delivery_stage stage)
{
   if (h->event_count < MAX_EVENTS)
      h->events[h->event_count++] = stage;
}

static bool
refused(struct harness *h, enum r3v_crossover_delivery_stage stage, char *why,
        size_t why_size)
{
   if (h->refuse_at != stage)
      return false;
   snprintf(why, why_size, "the harness refuses %s",
            r3v_crossover_delivery_stage_name(stage));
   return true;
}

static bool
harness_clock(void *ctx, uint64_t *ns, char *why, size_t why_size)
{
   struct harness *h = ctx;
   (void)why;
   (void)why_size;
   if (h->clock_reads == h->reverse_on_read) {
      /* A read below every earlier one, which the sequence must refuse
       * rather than publish as an underflowed interval.  The clock starts
       * well above zero, so zero is unambiguously backward. */
      *ns = 0u;
      h->clock_reads++;
      return true;
   }
   *ns = h->now;
   h->now += ADVANCE_CLOCK;
   h->clock_reads++;
   return true;
}

#define HARNESS_OP(lower, STAGE, advance)                                    \
   static bool harness_##lower(void *ctx, char *why, size_t why_size)        \
   {                                                                        \
      struct harness *h = ctx;                                              \
      record_event(h, STAGE);                                               \
      if (refused(h, STAGE, why, why_size))                                 \
         return false;                                                      \
      h->now += (advance);                                                  \
      return true;                                                          \
   }

HARNESS_OP(initialize, R3V_CROSSOVER_STAGE_INITIALIZE, ADVANCE_INITIALIZE)
HARNESS_OP(record, R3V_CROSSOVER_STAGE_RECORD, ADVANCE_RECORD)
HARNESS_OP(delay, R3V_CROSSOVER_STAGE_DELAY, ADVANCE_DELAY)
HARNESS_OP(submit, R3V_CROSSOVER_STAGE_SUBMIT, ADVANCE_SUBMIT)
HARNESS_OP(await_completion, R3V_CROSSOVER_STAGE_AWAIT, ADVANCE_AWAIT)
HARNESS_OP(make_visible, R3V_CROSSOVER_STAGE_VISIBLE, ADVANCE_VISIBLE)
HARNESS_OP(verify, R3V_CROSSOVER_STAGE_VERIFY, ADVANCE_VERIFY)

static const struct r3v_crossover_delivery_ops harness_ops = {
   .read_clock = harness_clock,
   .initialize = harness_initialize,
   .record = harness_record,
   .delay = harness_delay,
   .submit = harness_submit,
   .await_completion = harness_await_completion,
   .make_visible = harness_make_visible,
   .verify = harness_verify,
};

static void
harness_init(struct harness *h)
{
   memset(h, 0, sizeof(*h));
   h->now = 1000000u;
   h->refuse_at = R3V_CROSSOVER_STAGE_COUNT;
   h->reverse_on_read = UINT32_MAX;
}

/* The stages the sequence runs, in order.  The clock reads are absent
 * because they carry no operation event; their positions show in the
 * intervals instead. */
static const enum r3v_crossover_delivery_stage expected_order[] = {
   R3V_CROSSOVER_STAGE_INITIALIZE, R3V_CROSSOVER_STAGE_RECORD,
   R3V_CROSSOVER_STAGE_DELAY,      R3V_CROSSOVER_STAGE_SUBMIT,
   R3V_CROSSOVER_STAGE_AWAIT,      R3V_CROSSOVER_STAGE_VISIBLE,
   R3V_CROSSOVER_STAGE_VERIFY,
};

int
main(void)
{
   char why[256];
   enum r3v_crossover_delivery_stage failed;

   /* A complete repetition: the order the sequence runs, and the two
    * intervals as exact sums of the stages each encloses. */
   {
      struct harness h;
      harness_init(&h);
      struct r3v_crossover_delivery_result result;
      const bool ok = r3v_crossover_deliver(&harness_ops, &h, &result,
                                            &failed, why, sizeof(why));
      check(ok, "a complete repetition delivers");
      check(h.event_count ==
               sizeof(expected_order) / sizeof(expected_order[0]),
            "the sequence runs exactly the seven operation stages");
      bool order_holds = h.event_count ==
                         sizeof(expected_order) / sizeof(expected_order[0]);
      for (uint32_t i = 0; order_holds && i < h.event_count; i++)
         order_holds = h.events[i] == expected_order[i];
      check(order_holds, "the stages run in the declared order");

      /* delivery opens before the delay and closes after the visibility
       * operation, so it encloses delay, submit, await, and visible plus
       * the two clock reads that fall inside it. */
      const uint64_t expected_delivery =
         ADVANCE_DELAY + ADVANCE_SUBMIT + ADVANCE_AWAIT + ADVANCE_VISIBLE;
      /* transport closes at the completion read, so it excludes the
       * visibility operation and the clock read that follows it. */
      const uint64_t expected_transport =
         ADVANCE_DELAY + ADVANCE_SUBMIT + ADVANCE_AWAIT;
      check(result.delivery_ns == expected_delivery,
            "delivery encloses the delay, the submission, the completion "
            "wait, and the visibility operation");
      check(result.transport_ns == expected_transport,
            "transport encloses the delay, the submission, and the "
            "completion wait alone");
      check(result.transport_ns < result.delivery_ns,
            "transport is nested inside delivery");
      /* Stated as its own predicate: a submission moved outside the
       * bracket leaves delivery short by exactly this much, which is the
       * mutation that calibrates this test. */
      check(result.delivery_ns - expected_transport == ADVANCE_VISIBLE,
            "the visibility operation is the whole difference between the "
            "two intervals");
      check(result.delivery_ns >= ADVANCE_SUBMIT &&
               result.transport_ns >= ADVANCE_SUBMIT,
            "both intervals contain the submission");
   }

   /* Each stage refuses on its own, reporting itself and publishing no
    * interval. */
   for (uint32_t s = 0; s < R3V_CROSSOVER_STAGE_COUNT; s++) {
      const enum r3v_crossover_delivery_stage stage = s;
      if (stage == R3V_CROSSOVER_STAGE_CLOCK_START ||
          stage == R3V_CROSSOVER_STAGE_CLOCK_COMPLETION ||
          stage == R3V_CROSSOVER_STAGE_CLOCK_VISIBLE)
         continue;
      struct harness h;
      harness_init(&h);
      h.refuse_at = stage;
      struct r3v_crossover_delivery_result result = { .delivery_ns = 7u,
                                                      .transport_ns = 7u };
      const bool ok = r3v_crossover_deliver(&harness_ops, &h, &result,
                                            &failed, why, sizeof(why));
      check(!ok, "a refused stage refuses the repetition");
      check(failed == stage, "the refusal names the stage that refused");
      check(result.delivery_ns == 7u && result.transport_ns == 7u,
            "a refused repetition publishes no interval");
   }

   /* A clock that steps backward refuses at the read that observed it,
    * rather than publishing an interval that wrapped. */
   {
      struct harness h;
      harness_init(&h);
      h.reverse_on_read = 1u;
      struct r3v_crossover_delivery_result result;
      const bool ok = r3v_crossover_deliver(&harness_ops, &h, &result,
                                            &failed, why, sizeof(why));
      check(!ok, "a backward clock refuses the repetition");
      check(failed == R3V_CROSSOVER_STAGE_CLOCK_COMPLETION,
            "the backward completion read names itself");
   }
   {
      struct harness h;
      harness_init(&h);
      h.reverse_on_read = 2u;
      struct r3v_crossover_delivery_result result;
      const bool ok = r3v_crossover_deliver(&harness_ops, &h, &result,
                                            &failed, why, sizeof(why));
      check(!ok, "a backward visible read refuses the repetition");
      check(failed == R3V_CROSSOVER_STAGE_CLOCK_VISIBLE,
            "the backward visible read names itself");
   }

   /* An incomplete operation set refuses rather than skipping a stage
    * and shortening the interval it would have carried. */
   {
      struct harness h;
      harness_init(&h);
      struct r3v_crossover_delivery_ops partial = harness_ops;
      partial.make_visible = NULL;
      struct r3v_crossover_delivery_result result;
      const bool ok = r3v_crossover_deliver(&partial, &h, &result, &failed,
                                            why, sizeof(why));
      check(!ok, "an incomplete operation set refuses the repetition");
      check(h.event_count == 0,
            "an incomplete operation set runs no stage");
   }

   if (failures != 0) {
      fprintf(stderr, "%d predicate(s) failed\n", failures);
      return 1;
   }
   printf("crossover delivery sequence: every predicate holds\n");
   return 0;
}
