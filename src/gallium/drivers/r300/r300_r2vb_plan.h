/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_PLAN_H
#define R300_R2VB_PLAN_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "compiler/shader_enums.h"
#include "compiler/radeon_code.h"
#include "r300_fs.h"
#include "r300_nir_ssa_cut.h"
#include "r300_r2vb_clip.h"

#ifdef __cplusplus
extern "C" {
#endif

struct r300_context;
struct nir_shader;

/* Producer input ceiling: the split pass-B draw feeds every model attribute
 * plus the carry, and the producer VAP_OUT_VTX_FMT / PSC packing, the
 * per-input passthrough varyings, and the R300 RS texcoord-unit count (8, the
 * binding limit) all scale with the input count.  The CD-4 sedenion product
 * (two 16-component sedenions = 8 FP32x4 velems) is HW-confirmed at this
 * width on RS482. */
#define R300_R2VB_MAX_PRODUCER_INPUTS 8

/* The R2VB producer plan: one classification record per (vertex shader, plan
 * key) describing how the fragment-ALU producer would deliver the shader --
 * one pass, a carry-BO split, or a decline -- with every failure class
 * observed across the ranked cut candidates retained.  The plan is the
 * classification authority the pre-draw host mirror only predicts; route
 * reachability stays with the existing float-route admission until the typed
 * diagnostic gate consumes the plan directly. */

/* Plan validity is separate from rejection cause: only stable outcomes
 * (READY and the two reject classes) are cacheable, and a TRANSIENT_FAILURE
 * (allocation) is retried on the next request instead of pinning a permanent
 * gallivm fallback. */
enum r300_r2vb_plan_status {
    R300_R2VB_PLAN_READY = 0,
    R300_R2VB_PLAN_SEMANTIC_REJECT,   /* a property of the workload */
    R300_R2VB_PLAN_POLICY_REJECT,     /* a gate held the route closed; the
                                       * typed diagnostic gate produces this */
    R300_R2VB_PLAN_TRANSIENT_FAILURE, /* infrastructure; never cached */
};

enum r300_r2vb_plan_action {
    R300_R2VB_PLAN_REJECT = 0,
    R300_R2VB_PLAN_SINGLE,    /* position pass (and varying pass, when
                               * admitted) compiles under the 64-slot emit
                               * ceiling */
    R300_R2VB_PLAN_COMPACTED, /* reserved for the algebraic-compaction pass:
                               * a rewritten candidate that fits or splits
                               * where the baseline did not */
    R300_R2VB_PLAN_SPLIT,     /* over-budget position pass with an admitted
                               * single-vec4 carry-BO cut: both halves
                               * compile */
};

/* Failure classes, one bit per reason in observed_reason_mask.  Declaration
 * order is precedence order: the primary reason of a rejected plan is the
 * lowest-numbered observed reason, so a candidate walk that saw both a range
 * decline and a half-compile failure reports the range decline.  The typed
 * diagnostic route adds its shape/witness/gate reasons when it lands; the
 * shadow planner produces the classes below. */
enum r300_r2vb_plan_reason {
    R300_R2VB_PLAN_OK = 0,
    R300_R2VB_PLAN_OUT_OF_MEMORY,          /* infrastructure, never cached */
    R300_R2VB_PLAN_CONTROL_FLOW,           /* if/loop/jump: fragment ALU is
                                            * straight-line on R300 */
    R300_R2VB_PLAN_INTRINSIC,              /* intrinsic outside plain I/O and
                                            * uniform/UBO loads, or texturing */
    R300_R2VB_PLAN_IO_SHAPE,               /* missing gl_Position output,
                                            * varying passthrough violation,
                                            * or position inputs beyond the
                                            * producer ceiling */
    R300_R2VB_PLAN_TYPED_SINGLE_PASS_UNPROVEN, /* under-budget typed producer:
                                            * a single pass would bypass the
                                            * carry range/signedness checks,
                                            * so it declines until the
                                            * single-pass domain is proven */
    R300_R2VB_PLAN_MIXED_SIGNEDNESS,       /* carried integer with conflicting
                                            * signed/unsigned/bool consumers */
    R300_R2VB_PLAN_SIGNED_RANGE,           /* proven bounds leave the FP24
                                            * exact window (+-2^17) */
    R300_R2VB_PLAN_UNSIGNED_RANGE,
    R300_R2VB_PLAN_CARRY_WIDTH,            /* crossing set > one vec4 */
    R300_R2VB_PLAN_PASS_A,                 /* carry-pass build or compile fail */
    R300_R2VB_PLAN_PASS_B,                 /* position-pass build/compile fail */
    R300_R2VB_PLAN_BACKEND,                /* unsplit producer compile rejected
                                            * for a reason other than the ALU
                                            * ceiling */
    R300_R2VB_PLAN_NO_EXACT_CUT,           /* no admissible cut candidate */
    R300_R2VB_PLAN_OVER_ALU_NO_SPLIT,      /* computed-varying pass over the
                                            * ceiling: that pass keeps the
                                            * single-pass rule */
    R300_R2VB_PLAN_REASON_COUNT,
};

/* The mask holds one bit per reason. */
static_assert(R300_R2VB_PLAN_REASON_COUNT <= 64,
              "r300_r2vb_plan_reason must index a 64-bit mask");

/* Whole-program typed-source shape of the producer, independent of the carry:
 * a shader can compute a typed value entirely before the cut and carry only a
 * float, so this scans every op, not the selected crossing set.  When both
 * signed and unsigned markers appear the class reads SINT, the stricter
 * admission constraint. */
enum r300_r2vb_typed_source_class {
    R300_R2VB_TYPED_SOURCE_NONE = 0,
    R300_R2VB_TYPED_SOURCE_BOOL,
    R300_R2VB_TYPED_SOURCE_SINT,
    R300_R2VB_TYPED_SOURCE_UINT,
};

/* Every NIR-specializing input of the producer build, explicit and
 * bit-compared: the window producer bakes the viewport scale/translate as
 * immediates, so those travel as float bit patterns, never numeric compares.
 * clip_halfz feeds the CPU clip classifier, not the restaged producer NIR,
 * so it joins the key when the clip-route plan integration makes it
 * specializing.  The vertex shader itself is the cache owner (the key lives
 * per-VS), so it is not a field. */
struct r300_r2vb_plan_key {
    bool allow_computed_varying;
    enum r300_r2vb_position_space space;
    enum r300_fs_input_semantics input_semantics;
    uint32_t viewport_scale[3];
    uint32_t viewport_translate[3];
};

struct r300_r2vb_producer_plan {
    enum r300_r2vb_plan_status status;
    enum r300_r2vb_plan_action action;
    enum r300_r2vb_plan_reason primary_reason;
    uint64_t observed_reason_mask; /* bit (1ull << reason) per class */

    struct r300_r2vb_plan_key key;
    bool has_typed_source;
    enum r300_r2vb_typed_source_class typed_source_class;

    unsigned num_position_inputs;

    /* Backend resource vectors from the admission oracle: the unsplit
     * position producer when it emitted, and the two admitted halves on
     * SPLIT. */
    struct r300_fs_admission_cost baseline;
    struct r300_fs_admission_cost pass_a_cost;
    struct r300_fs_admission_cost pass_b_cost;

    /* Selected cut on SPLIT: the full partition, whose base pointers refer
     * into the owned candidate NIR below and stay valid for the plan's
     * lifetime. */
    struct r300_mp_partition partition;

    /* Canonical optimized restaged position-pass FS NIR the verdict was
     * measured on.  The plan owns it; every later compile or state creation
     * clones it, so a consuming backend helper cannot invalidate the plan. */
    struct nir_shader *candidate;
};

/* Compute a producer plan for vs_nir.  Fills *plan and returns true; returns
 * false only on TRANSIENT_FAILURE (allocation), which the caller must not
 * cache.  Pure with respect to vs_nir (works on clones); reads r300 for the
 * screen, the admission compile state, and -- window space only -- the bound
 * viewport the restaged producer bakes as immediates. */
bool r300_r2vb_plan_producer(struct r300_context *r300,
                             struct nir_shader *vs_nir,
                             bool allow_computed_varying,
                             enum r300_r2vb_position_space space,
                             struct r300_r2vb_producer_plan *plan);

/* Free the plan's owned candidate NIR and clear the record. */
void r300_r2vb_plan_release(struct r300_r2vb_producer_plan *plan);

/* Cached plan for the bound vertex shader, computed on first use and keyed by
 * r300_r2vb_plan_key.  A viewport change re-plans the window-space slots.
 * Returns NULL only on TRANSIENT_FAILURE; that result is never cached, so a
 * later call retries. */
const struct r300_r2vb_producer_plan *
r300_r2vb_producer_plan_get(struct r300_context *r300,
                            bool allow_computed_varying,
                            enum r300_r2vb_position_space space);

/* Release every cached plan slot of a vertex shader (delete_vs_state). */
struct r300_vertex_shader;
void r300_r2vb_plan_cache_release(struct r300_vertex_shader *vs);

/* Shadow-parity divergence accounting: the admission memo stays
 * authoritative, a divergence increments this process-wide counter for the
 * planner test and telemetry, and rendering never changes. */
void r300_r2vb_plan_note_shadow_divergence(void);
uint32_t r300_r2vb_plan_shadow_divergences(void);

const char *r300_r2vb_plan_action_str(enum r300_r2vb_plan_action action);
const char *r300_r2vb_plan_reason_str(enum r300_r2vb_plan_reason reason);

/* R2VB producer-lane internals the planner and the route-chain host oracle
 * consume; defined in r300_r2vb.c. */
struct nir_shader *r300_r2vb_build_restaged_fs_nir(struct r300_context *r300,
                                                   struct nir_shader *vs_nir,
                                                   gl_varying_slot target,
                                                   enum r300_r2vb_position_space space);
unsigned r300_r2vb_count_position_inputs(struct nir_shader *vs_nir);
int r300_r2vb_first_computed_varying(struct nir_shader *vs_nir);

#ifdef __cplusplus
}
#endif

#endif /* R300_R2VB_PLAN_H */
