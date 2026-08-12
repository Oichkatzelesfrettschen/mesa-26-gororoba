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

static inline bool
r300_r2vb_selftest_armed(const char *hb_tcl, bool rs48x_capable,
                         bool from_flush, bool already_fired)
{
   return rs48x_capable && from_flush && !already_fired &&
          r300_r2vb_option_is(hb_tcl, "1");
}

/* A nested pipe flush belongs to the active probe's cleanup and submission
 * path.  It uses the normal flush implementation without dispatching another
 * probe, so a no-submit capture cannot reset the active probe's command stream.
 * The call graph is reproducible with `(rg --fixed-strings
 * r300_r2vb_probe_dispatch_active src/gallium/drivers/r300/)`.
 */
static inline bool
r300_r2vb_probe_dispatch_allowed(bool probe_dispatch_active)
{
   return !probe_dispatch_active;
}

/* Readback maps the output BO with synchronized PIPE_MAP_READ semantics.  The
 * explicit fence gate therefore keeps the transform oracle away from a failed
 * submit before the mapping path can wait on the resource. */
static inline bool
r300_r2vb_transform_verify_allowed(bool xform, bool do_submit,
                                   bool submit_signalled)
{
   return xform && do_submit && submit_signalled;
}

static inline bool
r300_r2vb_diagnostic_allowed(bool *reported)
{
   if (*reported)
      return false;
   *reported = true;
   return true;
}

/* Select the transport before allocation, command emission, or query
 * finalization.  RADEON_FLUSH_NOOP discards the capture IB before
 * DRM_RADEON_CS, so capture declines an active query rather than advancing
 * r300_emit_query_end's CPU bookkeeping without the matching GPU write.  An
 * active query is admitted only for submit, which carries the query-end packet
 * and additionally requires exact raw submission consent.  The RS48x family
 * predicate is independent from caps.has_tcl and num_vert_fpus because the
 * R300_HB_TCL opt-in sets both fields before this self-test runs. */
static inline enum r300_r2vb_selftest_action
r300_r2vb_select_selftest_action(const char *hb_tcl, const char *timing,
                                 const char *raw_submit_accepted,
                                 bool rs48x_capable,
                                 bool from_flush, bool already_fired,
                                 bool query_active)
{
   if (!r300_r2vb_selftest_armed(hb_tcl, rs48x_capable, from_flush,
                                 already_fired))
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
