/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_TELEMETRY_H
#define R300_R2VB_TELEMETRY_H

#include <stdint.h>

#include "r300_r2vb_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

struct r300_context;

/* Standing-route telemetry over the R2VB producer classification: one record
 * per (vertex shader, computed-varying mode, position space) cell, taken from
 * the cached producer plan at the admission-memo decision point, so the
 * counts describe the real bound workload rather than a synthetic corpus.
 *
 * Counters run unconditionally, like the shadow-divergence counter.  The
 * per-event line prints under R300_R2VB_TELEMETRY=1 (exact value).  Producer
 * retention writes the application VS NIR of each plan whose split, range,
 * or budget machinery engaged into the directory named by
 * R300_R2VB_TELEMETRY_RETAIN, one nir_serialize blob per content hash
 * (r2vb-vs-<blake3-prefix>.nir), deduplicated by filename, so recurring
 * over-budget shapes accumulate once and feed the compaction-rule mining in
 * docs/hardware/rs482-producer-alu-compaction-design.md. */
struct r300_r2vb_telemetry_counters {
    uint32_t by_action[4];  /* indexed by enum r300_r2vb_plan_action */
    uint32_t by_reason[R300_R2VB_PLAN_REASON_COUNT]; /* primary_reason */
    uint32_t typed[4];      /* indexed by enum r300_r2vb_typed_source_class,
                             * counted when has_typed_source is set */
    uint32_t retained;      /* retention files written */
    uint32_t retain_failures;
};

/* Record one plan classification.  Called at the admission-memo decision
 * point, once per cell; counts always, prints and retains under the gates
 * above. */
void r300_r2vb_telemetry_note(struct r300_context *r300,
                              const struct r300_r2vb_producer_plan *plan);

const struct r300_r2vb_telemetry_counters *r300_r2vb_telemetry_get(void);

/* Print the process-wide totals to stderr under R300_R2VB_TELEMETRY=1;
 * context destruction calls this so a run's summary lands at teardown. */
void r300_r2vb_telemetry_print_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* R300_R2VB_TELEMETRY_H */
