/* SPDX-License-Identifier: MIT
 *
 * The ordered sequence one crossover repetition performs, over an
 * injected clock and an injected operation set.
 *
 * The campaign supplies real Vulkan calls and CLOCK_MONOTONIC_RAW; the
 * harness supplies counters and a clock that advances a known amount per
 * stage.  Both drive this one function, so a mutation that moves a stage
 * across a timestamp changes the interval the harness measures and the
 * interval the campaign reports together.  A harness holding its own copy
 * of the order would keep passing while the campaign's bracket moved.
 *
 * Two intervals leave the sequence, both opening at the same instant:
 *
 *   delivery  = visible timestamp   - start timestamp
 *   transport = completion timestamp - start timestamp
 *
 * delivery is the public result -- what a caller waits through before the
 * fill's bytes are readable.  transport is the nested diagnostic that
 * stops at completion and excludes the visibility operation; it carries
 * host submission and waiting cost, so it is not a measure of GPU time.
 */

#ifndef R3V_CROSSOVER_DELIVERY_H
#define R3V_CROSSOVER_DELIVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The stages in the order the sequence runs them.  A failure reports the
 * stage it stopped at, so a caller separates a refused submission from a
 * failed oracle without parsing the message. */
enum r3v_crossover_delivery_stage {
   R3V_CROSSOVER_STAGE_INITIALIZE = 0,
   R3V_CROSSOVER_STAGE_RECORD,
   R3V_CROSSOVER_STAGE_CLOCK_START,
   R3V_CROSSOVER_STAGE_DELAY,
   R3V_CROSSOVER_STAGE_SUBMIT,
   R3V_CROSSOVER_STAGE_AWAIT,
   R3V_CROSSOVER_STAGE_CLOCK_COMPLETION,
   R3V_CROSSOVER_STAGE_VISIBLE,
   R3V_CROSSOVER_STAGE_CLOCK_VISIBLE,
   R3V_CROSSOVER_STAGE_VERIFY,
   R3V_CROSSOVER_STAGE_COUNT,
};

const char *
r3v_crossover_delivery_stage_name(enum r3v_crossover_delivery_stage stage);

/* Each operation returns true on success and fills why on failure.  A
 * null read_clock refuses the sequence; every other member is required
 * as well, so an incomplete set is a refusal rather than a skipped
 * stage that silently shortens the interval. */
struct r3v_crossover_delivery_ops {
   bool (*read_clock)(void *ctx, uint64_t *ns, char *why, size_t why_size);
   /* Conditions the destination.  Outside both intervals: the workload
    * class this campaign measures initializes the whole allocation, and
    * that cost belongs to the conditioning rather than to the fill. */
   bool (*initialize)(void *ctx, char *why, size_t why_size);
   /* Records the fill.  Outside both intervals because either route
    * defers the work to the submission. */
   bool (*record)(void *ctx, char *why, size_t why_size);
   /* A known interval the caller asks to be spent inside the brackets,
    * so a sweep run with it and without it shows whether the timestamps
    * enclose what they name. */
   bool (*delay)(void *ctx, char *why, size_t why_size);
   bool (*submit)(void *ctx, char *why, size_t why_size);
   bool (*await_completion)(void *ctx, char *why, size_t why_size);
   /* The invalidation a noncoherent mapping requires before a host read
    * of the result.  Inside delivery and outside transport, which is the
    * whole difference between them. */
   bool (*make_visible)(void *ctx, char *why, size_t why_size);
   /* The byte oracle, outside both intervals. */
   bool (*verify)(void *ctx, char *why, size_t why_size);
};

struct r3v_crossover_delivery_result {
   uint64_t delivery_ns;
   uint64_t transport_ns;
};

/* Runs one repetition.  Returns false with *failed_stage set to the
 * stage that refused and why naming it.  A clock that runs backward
 * between two of its own reads refuses at the later read rather than
 * publishing an interval that underflowed. */
bool r3v_crossover_deliver(const struct r3v_crossover_delivery_ops *ops,
                           void *ctx,
                           struct r3v_crossover_delivery_result *out,
                           enum r3v_crossover_delivery_stage *failed_stage,
                           char *why, size_t why_size);

#endif
