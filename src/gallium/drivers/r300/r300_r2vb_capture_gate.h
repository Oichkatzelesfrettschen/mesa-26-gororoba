/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_CAPTURE_GATE_H
#define R300_R2VB_CAPTURE_GATE_H

#include <stdbool.h>
#include <string.h>

enum r300_r2vb_selftest_action {
   R300_R2VB_SELFTEST_DECLINE = 0,
   R300_R2VB_SELFTEST_CAPTURE,
   R300_R2VB_SELFTEST_SUBMIT,
};

static inline bool
r300_r2vb_option_is(const char *value, const char *expected)
{
   return value && strcmp(value, expected) == 0;
}

/* Select the transport before allocation, command emission, or query
 * finalization.  RADEON_FLUSH_NOOP discards the capture IB before
 * DRM_RADEON_CS, so capture declines an active query rather than advancing
 * r300_emit_query_end's CPU bookkeeping without the matching GPU write.  An
 * active query is admitted only for submit, which carries the query-end packet
 * and additionally requires exact raw submission consent. */
static inline enum r300_r2vb_selftest_action
r300_r2vb_select_selftest_action(const char *hb_tcl, const char *timing,
                                 const char *raw_submit_accepted,
                                 bool from_flush, bool already_fired,
                                 bool query_active)
{
   if (!from_flush || already_fired || !r300_r2vb_option_is(hb_tcl, "1"))
      return R300_R2VB_SELFTEST_DECLINE;

   if (r300_r2vb_option_is(timing, "capture"))
      return query_active ? R300_R2VB_SELFTEST_DECLINE
                     : R300_R2VB_SELFTEST_CAPTURE;

   if (r300_r2vb_option_is(timing, "submit") &&
      r300_r2vb_option_is(raw_submit_accepted, "1"))
      return R300_R2VB_SELFTEST_SUBMIT;

   return R300_R2VB_SELFTEST_DECLINE;
}

#endif /* R300_R2VB_CAPTURE_GATE_H */
