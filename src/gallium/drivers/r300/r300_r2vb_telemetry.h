/*
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
 * (r2vb-vs-<full-blake3>.nir).  Publication is atomic -- a same-directory
 * temp file renamed into place -- so the final name only ever holds a
 * complete blob, and an existing file verifies byte-for-byte against the
 * fresh serialization before it deduplicates.  Recurring over-budget shapes
 * therefore accumulate once and feed the compaction-rule mining in
 * docs/hardware/rs482-producer-alu-compaction-design.md. */
struct r300_r2vb_telemetry_counters {
    uint32_t by_action[4];  /* indexed by enum r300_r2vb_plan_action */
    uint32_t by_reason[R300_R2VB_PLAN_REASON_COUNT]; /* primary_reason */
    uint32_t typed[4];      /* indexed by enum r300_r2vb_typed_source_class,
                             * counted when has_typed_source is set */
    uint32_t retained;      /* retention files written */
    uint32_t retain_failures;
};

/* The fixed array lengths track their indexing enums. */
static_assert(sizeof(((struct r300_r2vb_telemetry_counters *)0)->by_action) /
                  sizeof(uint32_t) ==
              R300_R2VB_PLAN_SPLIT + 1,
              "by_action covers enum r300_r2vb_plan_action");
static_assert(sizeof(((struct r300_r2vb_telemetry_counters *)0)->typed) /
                  sizeof(uint32_t) ==
              R300_R2VB_TYPED_SOURCE_UINT + 1,
              "typed covers enum r300_r2vb_typed_source_class");

/* Retention scope (R300_R2VB_TELEMETRY_RETAIN_SCOPE, exact values): the
 * unset, empty, and every unrecognized value keep the established
 * budget-only policy, so a typo can never widen retention.
 *   budget      plans whose split, range, or budget machinery engaged
 *   single      fitting SINGLE plans (the standing-route policy corpus)
 *   structural  rejects whose cause is structural (control flow,
 *               intrinsic set, I/O shape, backend)
 *   all         every plan with an application NIR VS (research reads) */
enum r300_r2vb_telemetry_retain_scope {
    R300_R2VB_TELEMETRY_RETAIN_BUDGET = 0,
    R300_R2VB_TELEMETRY_RETAIN_SINGLE,
    R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL,
    R300_R2VB_TELEMETRY_RETAIN_ALL,
};

/* Pure over the string so the calibration test drives every arm. */
enum r300_r2vb_telemetry_retain_scope
r300_r2vb_telemetry_retain_scope_value(const char *value);

/* Pure eligibility of one plan under one scope. */
bool r300_r2vb_telemetry_retain_eligible_in_scope(
    const struct r300_r2vb_producer_plan *plan,
    enum r300_r2vb_telemetry_retain_scope scope);

/* Record one plan classification.  Called at the admission-memo decision
 * point, once per cell; counts always, prints and retains under the gates
 * above.  The event line carries the application VS content hash
 * (vs_blake3=<64 hex>), computed once per shader and cached on the VS, so
 * the census separates observation prevalence from shape diversity even
 * when the shader is outside the retention scope. */
/* Observation is armed when the per-event print gate or the retain
 * directory is set; the route consults this to classify draws for
 * telemetry when the route gate itself stays closed. */
bool r300_r2vb_telemetry_observation_enabled(void);

/* BLAKE3 content hex of the bound application VS, computed once and cached
 * on the VS; "-" when the VS carries no serializable NIR.  The AUTO_SINGLE
 * decision token prints this so a decision joins the retained corpus and the
 * workload weights by the same key. */
const char *r300_r2vb_telemetry_vs_content_hex(struct r300_context *r300);

void r300_r2vb_telemetry_note(struct r300_context *r300,
                              const struct r300_r2vb_producer_plan *plan);

/* Dynamic workload weight, accumulated per candidate draw after the plan is
 * cached -- no compiles, no serialization past the first hash: draws,
 * vertices, instances, draw-size extrema, a topology bit mask, and the
 * indexed-draw count, keyed by the VS content hash and plan action.  The
 * teardown summary prints one workload line per (hash, action) key, turning
 * cell incidence into the evidence a route-on policy needs (a six-slot
 * producer amortizes its fixed route cost only above a measured vertex-count
 * crossover). */
struct r300_r2vb_workload_stats {
    char action;             /* R=reject, N=single, P=split */
    uint64_t draws;
    uint64_t vertices;
    uint64_t instances;
    uint32_t draw_min;
    uint32_t draw_max;
    uint32_t topology_mask;  /* bit per pipe primitive type */
    uint64_t indexed_draws;
};

struct pipe_draw_info;
struct pipe_draw_start_count_bias;
void r300_r2vb_telemetry_draw(struct r300_context *r300,
                              const struct r300_r2vb_producer_plan *plan,
                              const struct pipe_draw_info *info,
                              const struct pipe_draw_start_count_bias *draw);

/* Stats snapshot by VS content hash and plan action for the calibration test;
 * single-threaded reads only.  Returns false for an unseen pair. */
bool r300_r2vb_telemetry_workload_stats(
   const char *hex, enum r300_r2vb_plan_action action,
   struct r300_r2vb_workload_stats *out);

/* Counter snapshot for the calibration test; single-threaded reads only.
 * Concurrent readers snapshot through r300_r2vb_telemetry_print_summary,
 * which loads each counter atomically. */
const struct r300_r2vb_telemetry_counters *r300_r2vb_telemetry_get(void);

/* Context-epoch accounting: create registers, destroy prints the cumulative
 * summary when the last live context goes away, so a multi-context run
 * reports one total instead of one partial summary per context. */
void r300_r2vb_telemetry_context_created(void);
void r300_r2vb_telemetry_context_destroyed(void);

/* Print the process-wide totals to stderr under R300_R2VB_TELEMETRY=1. */
void r300_r2vb_telemetry_print_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* R300_R2VB_TELEMETRY_H */
