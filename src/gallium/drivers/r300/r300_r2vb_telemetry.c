/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_telemetry.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nir.h"
#include "nir_serialize.h"
#include "util/blob.h"
#include "util/mesa-blake3.h"
#include "util/u_atomic.h"

#include "r300_context.h"
#include "r300_vs.h"

static struct r300_r2vb_telemetry_counters counters;

/* The per-event print gate takes the exact value 1; unset, empty, and every
 * other value keep it closed. */
static bool
telemetry_print_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("R300_R2VB_TELEMETRY");
        enabled = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return enabled == 1;
}

/* The retain directory reads per event: retention runs only on the cold
 * once-per-cell classification path, and an uncached read lets the
 * calibration test exercise closed and open states in one process. */
static const char *
telemetry_retain_dir(void)
{
    const char *dir = getenv("R300_R2VB_TELEMETRY_RETAIN");
    return (dir && dir[0]) ? dir : NULL;
}

/* Retention selects the plans whose budget, split, or typed-range machinery
 * engaged -- the shapes the compaction mining needs.  A SINGLE plan fits as
 * is, and a reject whose cause is structural (control flow, intrinsic set,
 * I/O shape, backend, allocation) carries no budget signal. */
static bool
telemetry_retain_eligible(const struct r300_r2vb_producer_plan *plan)
{
    if (plan->action == R300_R2VB_PLAN_SPLIT)
        return true;
    if (plan->action != R300_R2VB_PLAN_REJECT)
        return false;
    switch (plan->primary_reason) {
    case R300_R2VB_PLAN_OK:
    case R300_R2VB_PLAN_OUT_OF_MEMORY:
    case R300_R2VB_PLAN_CONTROL_FLOW:
    case R300_R2VB_PLAN_INTRINSIC:
    case R300_R2VB_PLAN_IO_SHAPE:
    case R300_R2VB_PLAN_BACKEND:
        return false;
    default:
        return true;
    }
}

/* Serialize the application VS into <dir>/r2vb-vs-<blake3-prefix>.nir.  The
 * filename is the content hash, so a shape that recurs across draws,
 * contexts, and processes lands once and an existing file is the
 * deduplication check. */
static void
telemetry_retain(const char *dir, const struct nir_shader *vs_nir)
{
    struct blob blob;
    blob_init(&blob);
    nir_serialize(&blob, (nir_shader *)vs_nir, false);
    if (blob.out_of_memory) {
        blob_finish(&blob);
        p_atomic_inc(&counters.retain_failures);
        return;
    }

    blake3_hash hash;
    char hex[BLAKE3_HEX_LEN];
    _mesa_blake3_compute(blob.data, blob.size, hash);
    _mesa_blake3_format(hex, hash);

    char path[1024];
    int need = snprintf(path, sizeof(path), "%s/r2vb-vs-%.16s.nir", dir, hex);
    if (need < 0 || (size_t)need >= sizeof(path)) {
        blob_finish(&blob);
        p_atomic_inc(&counters.retain_failures);
        fprintf(stderr, "r2vb_telemetry retain path too long, dropped\n");
        return;
    }

    if (access(path, F_OK) == 0) {
        blob_finish(&blob);
        return;
    }

    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(blob.data, 1, blob.size, f) == blob.size;
    if (f)
        ok = (fclose(f) == 0) && ok;
    blob_finish(&blob);

    if (ok) {
        p_atomic_inc(&counters.retained);
        if (telemetry_print_enabled())
            fprintf(stderr, "r2vb_telemetry retained=%s\n", path);
    } else {
        p_atomic_inc(&counters.retain_failures);
        fprintf(stderr, "r2vb_telemetry retain failed: %s\n", path);
    }
}

void
r300_r2vb_telemetry_note(struct r300_context *r300,
                         const struct r300_r2vb_producer_plan *plan)
{
    if (plan->action < ARRAY_SIZE(counters.by_action))
        p_atomic_inc(&counters.by_action[plan->action]);
    if (plan->primary_reason < R300_R2VB_PLAN_REASON_COUNT)
        p_atomic_inc(&counters.by_reason[plan->primary_reason]);
    if (plan->has_typed_source &&
        plan->typed_source_class < ARRAY_SIZE(counters.typed))
        p_atomic_inc(&counters.typed[plan->typed_source_class]);

    if (telemetry_print_enabled()) {
        fprintf(stderr,
                "r2vb_telemetry action=%s primary=%s mask=0x%" PRIx64
                " typed=%d inputs=%u space=%s cv=%d baseline=%u/%u/%u "
                "passA=%u/%u/%u passB=%u/%u/%u\n",
                r300_r2vb_plan_action_str(plan->action),
                r300_r2vb_plan_reason_str(plan->primary_reason),
                plan->observed_reason_mask, plan->has_typed_source,
                plan->num_position_inputs,
                plan->key.space == R300_R2VB_POSITION_WINDOW ? "window"
                                                             : "clip",
                plan->key.allow_computed_varying,
                plan->baseline.alu, plan->baseline.temps,
                plan->baseline.consts, plan->pass_a_cost.alu,
                plan->pass_a_cost.temps, plan->pass_a_cost.consts,
                plan->pass_b_cost.alu, plan->pass_b_cost.temps,
                plan->pass_b_cost.consts);
    }

    const char *dir = telemetry_retain_dir();
    if (dir && telemetry_retain_eligible(plan)) {
        struct r300_vertex_shader *vs = r300_vs(r300);
        if (vs && vs->state.type == PIPE_SHADER_IR_NIR && vs->state.ir.nir)
            telemetry_retain(dir, vs->state.ir.nir);
    }
}

const struct r300_r2vb_telemetry_counters *
r300_r2vb_telemetry_get(void)
{
    return &counters;
}

void
r300_r2vb_telemetry_print_summary(void)
{
    if (!telemetry_print_enabled())
        return;
    uint32_t total = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(counters.by_action); i++)
        total += counters.by_action[i];
    if (!total)
        return;
    fprintf(stderr, "r2vb_telemetry summary cells=%u", total);
    for (unsigned i = 0; i < ARRAY_SIZE(counters.by_action); i++) {
        if (counters.by_action[i])
            fprintf(stderr, " %s=%u",
                    r300_r2vb_plan_action_str((enum r300_r2vb_plan_action)i),
                    counters.by_action[i]);
    }
    for (unsigned i = 0; i < R300_R2VB_PLAN_REASON_COUNT; i++) {
        if (counters.by_reason[i])
            fprintf(stderr, " reason:%s=%u",
                    r300_r2vb_plan_reason_str((enum r300_r2vb_plan_reason)i),
                    counters.by_reason[i]);
    }
    static const char *const typed_names[4] = { "none", "bool", "sint",
                                                "uint" };
    for (unsigned i = 0; i < ARRAY_SIZE(counters.typed); i++) {
        if (counters.typed[i])
            fprintf(stderr, " typed:%s=%u", typed_names[i],
                    counters.typed[i]);
    }
    if (counters.retained || counters.retain_failures)
        fprintf(stderr, " retained=%u retain_failures=%u",
                counters.retained, counters.retain_failures);
    fprintf(stderr, "\n");
}
