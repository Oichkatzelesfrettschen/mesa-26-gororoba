/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for R3V partial-submit resource lifetime ordering.
 */

#include <stdbool.h>
#include <stdio.h>

#include "../r3v_submit_lifetime.h"

static unsigned failures;

#define CHECK(condition, name)           \
   do {                                  \
      if (condition) {                   \
         printf("  ok   - %s\n", name); \
      } else {                           \
         printf("  FAIL - %s\n", name); \
         failures++;                     \
      }                                  \
   } while (0)

enum lifetime_event {
   LIFETIME_DRAIN,
   LIFETIME_RELEASE,
};

struct lifetime_trace {
   enum lifetime_event events[2];
   unsigned event_count;
   bool gpu_pending;
   bool fence_signaled;
   bool resource_live;
   bool released_before_fence;
};

static void
trace_event(struct lifetime_trace *trace, enum lifetime_event event)
{
   if (trace->event_count < 2)
      trace->events[trace->event_count++] = event;
}

static void
fake_drain(void *data)
{
   struct lifetime_trace *trace = data;
   trace_event(trace, LIFETIME_DRAIN);
   trace->fence_signaled = true;
   trace->gpu_pending = false;
}

static void
fake_release(void *data)
{
   struct lifetime_trace *trace = data;
   if (trace->gpu_pending && !trace->fence_signaled)
      trace->released_before_fence = true;
   trace_event(trace, LIFETIME_RELEASE);
   trace->resource_live = false;
}

static const struct r3v_submit_lifetime_ops fake_ops = {
   .drain = fake_drain,
   .release = fake_release,
};

static void
check_partial_submit_failure(void)
{
   struct lifetime_trace trace = {
      .gpu_pending = true,
      .resource_live = true,
   };

   r3v_submit_lifetime_finish(true, &fake_ops, &trace);

   CHECK(trace.event_count == 2 && trace.events[0] == LIFETIME_DRAIN &&
         trace.events[1] == LIFETIME_RELEASE,
         "pending replay drains before transient release");
   CHECK(!trace.released_before_fence && !trace.resource_live,
         "partial-submit failure retires GPU references before release");
}

static void
check_no_pending_failure(void)
{
   struct lifetime_trace trace = {
      .resource_live = true,
   };

   r3v_submit_lifetime_finish(false, &fake_ops, &trace);

   CHECK(trace.event_count == 1 && trace.events[0] == LIFETIME_RELEASE,
         "failure without queued GPU work releases directly");
   CHECK(!trace.released_before_fence && !trace.resource_live &&
         !trace.fence_signaled,
         "no-pending release has no outstanding GPU reference");
}

static void
check_depth_only_render_pass_clear(void)
{
   struct lifetime_trace trace = {
      .resource_live = true,
   };
   const bool pending =
      r3v_submit_lifetime_render_pass_has_clear(true, false);

   r3v_submit_lifetime_finish(pending, &fake_ops, &trace);

   CHECK(pending && trace.fence_signaled && !trace.released_before_fence,
         "depth-only load clear enters the fenced submit path");
}

static void
check_known_bad_order(void)
{
   struct lifetime_trace trace = {
      .gpu_pending = true,
      .resource_live = true,
   };

   fake_release(&trace);
   fake_drain(&trace);

   CHECK(trace.released_before_fence,
         "known-bad release-before-fence order is detected");
}

int
main(void)
{
   check_partial_submit_failure();
   check_no_pending_failure();
   check_depth_only_render_pass_clear();
   check_known_bad_order();

   if (failures) {
      printf("FAILED: %u check(s)\n", failures);
      return 1;
   }

   printf("OK\n");
   return 0;
}
