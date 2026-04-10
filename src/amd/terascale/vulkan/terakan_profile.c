/*
 * terakan_profile.c — dump-and-reset for hot-path profiling counters
 */

#include "terakan_profile.h"

#include <stdio.h>
#include <string.h>

void
terakan_profile_dump_and_reset(struct terakan_profile_counters *c)
{
   if (c->submit_count == 0)
      return;

   uint64_t draw_avg_us = c->draw_count ? (c->draw_ns / c->draw_count / 1000) : 0;
   uint64_t dispatch_avg_us = c->dispatch_count ? (c->dispatch_ns / c->dispatch_count / 1000) : 0;
   uint64_t submit_avg_us = c->submit_count ? (c->submit_ns / c->submit_count / 1000) : 0;
   uint64_t compile_avg_us = c->compile_count ? (c->compile_ns / c->compile_count / 1000) : 0;
   uint64_t state_avg_us = c->state_emit_count ? (c->state_emit_ns / c->state_emit_count / 1000) : 0;

   fprintf(stderr,
      "TERAKAN_PROFILE[%lu]: "
      "draw=%lu(%luus) dispatch=%lu(%luus) submit=%lu(%luus,ib=%ludw,bo=%lu) "
      "compile=%lu(hit=%lu,%luus) state=%lu(%luus)\n",
      (unsigned long)c->frame_count,
      (unsigned long)c->draw_count, (unsigned long)draw_avg_us,
      (unsigned long)c->dispatch_count, (unsigned long)dispatch_avg_us,
      (unsigned long)c->submit_count, (unsigned long)submit_avg_us,
      (unsigned long)(c->submit_count ? c->submit_ib_dwords / c->submit_count : 0),
      (unsigned long)(c->submit_count ? c->submit_bo_refs / c->submit_count : 0),
      (unsigned long)c->compile_count, (unsigned long)c->compile_cache_hit,
      (unsigned long)compile_avg_us,
      (unsigned long)c->state_emit_count, (unsigned long)state_avg_us);

   uint64_t frame = c->frame_count + 1;
   memset(c, 0, sizeof(*c));
   c->frame_count = frame;
}
