/*
 * terakan_profile.h — Lightweight hot-path profiling counters
 *
 * Activated by TERAKAN_DEBUG=profile. Zero overhead when disabled.
 *
 * Usage: each hot path calls terakan_profile_begin/end around the
 * measured section. On queue submit, counters are dumped to stderr
 * and reset.
 */

#ifndef TERAKAN_PROFILE_H
#define TERAKAN_PROFILE_H

#include <stdbool.h>
#include <time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_profile_counters {
   /* Draw / dispatch */
   uint64_t draw_count;
   uint64_t draw_ns;
   uint64_t dispatch_count;
   uint64_t dispatch_ns;

   /* Queue submit */
   uint64_t submit_count;
   uint64_t submit_ns;
   uint64_t submit_ib_dwords;

   /* Shader compile */
   uint64_t compile_count;
   uint64_t compile_cache_hit;
   uint64_t compile_ns;

   /* State emission */
   uint64_t state_emit_count;
   uint64_t state_emit_ns;

   /* Frame boundary */
   uint64_t frame_count;
};

/* Returns monotonic nanoseconds — inline for zero-overhead gating. */
static inline uint64_t
terakan_profile_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void terakan_profile_dump_and_reset(struct terakan_profile_counters *counters);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PROFILE_H */
