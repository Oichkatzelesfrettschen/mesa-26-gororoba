/* SPDX-License-Identifier: MIT */

#include "r3v_crossover_delivery.h"

#include <stdio.h>

static const char *const stage_names[R3V_CROSSOVER_STAGE_COUNT] = {
   [R3V_CROSSOVER_STAGE_INITIALIZE] = "initialize",
   [R3V_CROSSOVER_STAGE_RECORD] = "record",
   [R3V_CROSSOVER_STAGE_CLOCK_START] = "clock_start",
   [R3V_CROSSOVER_STAGE_DELAY] = "delay",
   [R3V_CROSSOVER_STAGE_SUBMIT] = "submit",
   [R3V_CROSSOVER_STAGE_AWAIT] = "await_completion",
   [R3V_CROSSOVER_STAGE_CLOCK_COMPLETION] = "clock_completion",
   [R3V_CROSSOVER_STAGE_VISIBLE] = "make_visible",
   [R3V_CROSSOVER_STAGE_CLOCK_VISIBLE] = "clock_visible",
   [R3V_CROSSOVER_STAGE_VERIFY] = "verify",
};

const char *
r3v_crossover_delivery_stage_name(enum r3v_crossover_delivery_stage stage)
{
   if (stage >= R3V_CROSSOVER_STAGE_COUNT)
      return "unknown";
   return stage_names[stage];
}

/* Runs one operation, recording the stage it belongs to before the call
 * so a refusal reports where it stopped. */
static bool
step(bool (*op)(void *, char *, size_t), void *ctx,
     enum r3v_crossover_delivery_stage stage,
     enum r3v_crossover_delivery_stage *failed_stage, char *why,
     size_t why_size)
{
   *failed_stage = stage;
   return op(ctx, why, why_size);
}

static bool
read_clock(const struct r3v_crossover_delivery_ops *ops, void *ctx,
           uint64_t *ns, enum r3v_crossover_delivery_stage stage,
           enum r3v_crossover_delivery_stage *failed_stage, char *why,
           size_t why_size)
{
   *failed_stage = stage;
   return ops->read_clock(ctx, ns, why, why_size);
}

bool
r3v_crossover_deliver(const struct r3v_crossover_delivery_ops *ops, void *ctx,
                      struct r3v_crossover_delivery_result *out,
                      enum r3v_crossover_delivery_stage *failed_stage,
                      char *why, size_t why_size)
{
   /* Every output the sequence writes through is held before it is
    * written: reporting a refused stage through the pointer whose
    * absence is the refusal would dereference it first. */
   if (failed_stage == NULL)
      return false;
   *failed_stage = R3V_CROSSOVER_STAGE_INITIALIZE;
   if (ops == NULL || out == NULL || ops->read_clock == NULL ||
       ops->initialize == NULL || ops->record == NULL ||
       ops->delay == NULL || ops->submit == NULL ||
       ops->await_completion == NULL || ops->make_visible == NULL ||
       ops->verify == NULL) {
      snprintf(why, why_size,
               "the delivery sequence needs a complete operation set");
      return false;
   }

   if (!step(ops->initialize, ctx, R3V_CROSSOVER_STAGE_INITIALIZE,
             failed_stage, why, why_size))
      return false;
   if (!step(ops->record, ctx, R3V_CROSSOVER_STAGE_RECORD, failed_stage, why,
             why_size))
      return false;

   uint64_t start = 0;
   if (!read_clock(ops, ctx, &start, R3V_CROSSOVER_STAGE_CLOCK_START,
                   failed_stage, why, why_size))
      return false;
   if (!step(ops->delay, ctx, R3V_CROSSOVER_STAGE_DELAY, failed_stage, why,
             why_size))
      return false;
   if (!step(ops->submit, ctx, R3V_CROSSOVER_STAGE_SUBMIT, failed_stage, why,
             why_size))
      return false;
   if (!step(ops->await_completion, ctx, R3V_CROSSOVER_STAGE_AWAIT,
             failed_stage, why, why_size))
      return false;
   uint64_t completion = 0;
   if (!read_clock(ops, ctx, &completion,
                   R3V_CROSSOVER_STAGE_CLOCK_COMPLETION, failed_stage, why,
                   why_size))
      return false;
   if (completion < start) {
      *failed_stage = R3V_CROSSOVER_STAGE_CLOCK_COMPLETION;
      snprintf(why, why_size,
               "the clock ran backward between the start and completion "
               "reads");
      return false;
   }
   if (!step(ops->make_visible, ctx, R3V_CROSSOVER_STAGE_VISIBLE,
             failed_stage, why, why_size))
      return false;
   uint64_t visible = 0;
   if (!read_clock(ops, ctx, &visible, R3V_CROSSOVER_STAGE_CLOCK_VISIBLE,
                   failed_stage, why, why_size))
      return false;
   if (visible < completion) {
      *failed_stage = R3V_CROSSOVER_STAGE_CLOCK_VISIBLE;
      snprintf(why, why_size,
               "the clock ran backward between the completion and visible "
               "reads");
      return false;
   }

   if (!step(ops->verify, ctx, R3V_CROSSOVER_STAGE_VERIFY, failed_stage, why,
             why_size))
      return false;

   out->delivery_ns = visible - start;
   out->transport_ns = completion - start;
   return true;
}
