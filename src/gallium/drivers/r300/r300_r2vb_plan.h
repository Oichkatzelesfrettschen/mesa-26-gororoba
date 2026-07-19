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
                                            * position inputs beyond the
                                            * producer ceiling, or -- on the
                                            * cv=1 varying plan -- a varying
                                            * discipline violation (a
                                            * non-input-fed passthrough, or a
                                            * computing non-first input
                                            * alongside a computed varying) */
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

/* Typed-source shape of the cell's restaged position producer, independent
 * of the carry: a producer can compute a typed value entirely before the cut
 * and carry only a float, so this scans every op of the position candidate,
 * not the selected crossing set.  A typed computation feeding only a
 * non-position output stays outside the cell (the restage's dead-code
 * elimination removes it from the candidate).  When both signed and unsigned
 * markers appear the class reads SINT, the stricter admission constraint. */
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

/* Diagnostic typed-split gate value: exactly "1" opens; NULL, empty, and
 * every other value keep the route closed.  Pure over the string so the
 * calibration test drives every arm. */
bool r300_r2vb_typed_split_gate_value(const char *value);

/* Per-VS admission memo byte, keyed by (allow_computed_varying, position
 * space).  Route reachability reads this byte; the plan cache is the shadow
 * authority the memo is audited against. */
enum r300_r2vb_admission_memo {
    R300_R2VB_ADMIT_UNMEASURED = 0,
    R300_R2VB_ADMIT_FITS,
    R300_R2VB_ADMIT_REJECT,
    R300_R2VB_ADMIT_SPLIT,
};

/* The meaning of a memo cell depends on the writer that populated it: the
 * legacy float route records a SPLIT only under the spill1 escape gate, and
 * the typed diagnostic route records exactly what its contract admits.  The
 * classify structure fixes the writer per cell -- a VS that passes the
 * fragment-aluable scan takes the float writer, a structurally rejected VS
 * reaches the typed writer -- so the effective mapping below is
 * deterministic per cell, independent of call order and of the other gate. */
enum r300_r2vb_memo_writer {
    R300_R2VB_MEMO_WRITER_LEGACY_FLOAT = 0,
    R300_R2VB_MEMO_WRITER_TYPED_DIAGNOSTIC,
};

/* Effective admission a plan cell predicts for the memo, per writer policy.
 * Pure over the explicit gate value so the calibration test drives the full
 * writer x gate matrix without process-cached environment state. */
enum r300_r2vb_admission_memo
r300_r2vb_plan_effective_admission(const struct r300_r2vb_producer_plan *plan,
                                   enum r300_r2vb_memo_writer writer,
                                   bool budget_escape_enabled,
                                   bool allow_computed_varying,
                                   enum r300_r2vb_position_space space,
                                   unsigned num_position_inputs);

/* Diagnostic typed-split route contract over a cached plan cell: NULL means
 * the cell executes through the plan-driven split; otherwise the returned
 * stable name is the decline reason the diagnostic token line prints.  The
 * contract admits only the position cell (cv=0) of a READY SPLIT plan whose
 * key matches the requested cell, with a typed source, at least one typed
 * transport in the selected carry, flat-vertex input semantics, a one-vec4
 * carry, the planned model-attribute arity, and an owned candidate; SPLIT
 * itself certifies both halves compiled under budget. */
const char *
r300_r2vb_typed_split_contract(const struct r300_r2vb_producer_plan *plan,
                               bool allow_computed_varying,
                               enum r300_r2vb_position_space space,
                               unsigned num_position_inputs);

/* Format the typed-route diagnostic token line into buf.  Pure over the
 * inputs; the runtime note prints exactly this string, so the calibration
 * test pins the token vocabulary (gate, plan status/action, space, typed
 * source, carry transport letters, cut, pass costs, decision, decline
 * reason) against the silicon engagement oracle. */
void
r300_r2vb_typed_split_note_format(const struct r300_r2vb_producer_plan *plan,
                                  enum r300_r2vb_position_space space,
                                  const char *decline, char *buf, size_t len);
unsigned r300_r2vb_count_position_inputs(struct nir_shader *vs_nir);
int r300_r2vb_first_computed_varying(struct nir_shader *vs_nir);

/* AUTO_SINGLE canary: automatic route selection for the untyped fitting
 * producer class under two exact-value research gates.  The route opens only
 * when R300_R2VB_AUTO_SINGLE is exactly "1" AND
 * R300_R2VB_AUTO_SINGLE_MIN_VERTICES parses as a strict positive decimal
 * uint32 (bare digits: sign, whitespace, trailing characters, overflow, and
 * zero all keep the route closed).  Admission composes route support first,
 * then the two delivery cells of the chosen path -- the plain route reads
 * cv=0 clip for classification and cv=0 window for delivery -- then the
 * vertex floor.  Everything outside the contract declines to gallivm. */
enum r300_r2vb_auto_single_reason {
    R300_R2VB_AUTO_SINGLE_OK = 0,
    R300_R2VB_AUTO_SINGLE_INDEXED,
    R300_R2VB_AUTO_SINGLE_INSTANCED,
    R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE,
    R300_R2VB_AUTO_SINGLE_COUNT_CEILING,
    R300_R2VB_AUTO_SINGLE_FRONTFACE,
    R300_R2VB_AUTO_SINGLE_CLIP_PLANES,
    R300_R2VB_AUTO_SINGLE_FS_EXTERNAL_CONSTANTS,
    R300_R2VB_AUTO_SINGLE_PLAN_NOT_READY,
    R300_R2VB_AUTO_SINGLE_PLAN_NOT_SINGLE,
    R300_R2VB_AUTO_SINGLE_TYPED_SOURCE,
    R300_R2VB_AUTO_SINGLE_INPUT_SHAPE,
    R300_R2VB_AUTO_SINGLE_DELIVERY_CELL,
    R300_R2VB_AUTO_SINGLE_BELOW_VERTEX_FLOOR,
    R300_R2VB_AUTO_SINGLE_REASON_COUNT,
};

/* Producer slot-grid layout: the slot-pixel stream maps slot s to raster
 * center ((s % width) + 0.5, (s / width) + 0.5) and to linear BO byte
 * (s * 16).  pitch_pixels == width is the load-bearing invariant: a
 * pitch-tight row makes the two mappings agree, so the producer rasterizes
 * in two dimensions while the re-ingest VAP reads the first count FP32x4
 * records as a flat vertex array with no gather or copy.  Grid disabled or
 * count <= 4096 keeps the proven one-row layout byte-for-byte; above 4096
 * the grid uses width = pitch = 2048 (inside the RS482 tiled render-width
 * ceiling) and height = ceil(count / 2048). */
struct r300_r2vb_slot_layout {
    uint32_t count;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
    uint64_t storage_slots;
    uint64_t storage_bytes;
};

/* Pure over (count, grid_enabled); fails on count == 0, count >= 65536
 * (the 16-bit re-ingest index ceiling), and -- grid off -- count > 4096. */
bool r300_r2vb_slot_layout_init(uint32_t count, bool grid_enabled,
                                struct r300_r2vb_slot_layout *out);

/* Slot-grid gate value (R300_R2VB_SLOT_GRID, exact "1"); pure parser. */
bool r300_r2vb_slot_grid_gate_value(const char *value);

/* BO-fetched producer vertex streams: stream 0 is the slot-position BO
 * (FP32x4, stride 16, offset 0) and stream 1 is the application model
 * attribute fetched from its own buffer, so the producer draw carries no
 * count-scaled immediate payload.  The load-bearing invariant is the VAP
 * tuple: the summed fetch dwords equal the producer VAP_VTX_SIZE and the
 * declared PSC input tuple (the GA front-end stalls on a mismatch).  The
 * first contract is deliberately narrow: one R32G32B32A32_FLOAT model
 * input with a dword-multiple stride; widening runs one format family at
 * a time against silicon. */
struct r300_r2vb_producer_stream {
    uint32_t offset_bytes;      /* within the stream's BO */
    uint32_t stride_dwords;
    uint32_t size_dwords;       /* physical dwords fetched per vertex */
    uint32_t logical_components; /* post-PSC vector width: a FLOAT_3
                                  * stream fetches 3 dwords and the PSC
                                  * swizzle synthesizes W from FP_ONE, so
                                  * the producer FS still reads a vec4 --
                                  * the physical-fetch invariant
                                  * (fetch_dwords == VAP_VTX_SIZE) and
                                  * the logical interface are separate
                                  * facts */
};

struct r300_r2vb_producer_streams {
    unsigned num;
    struct r300_r2vb_producer_stream stream[2];
    uint32_t fetch_dwords;   /* == producer VAP_VTX_SIZE */
};

/* Pure over the model element facts; fails on a format outside the first
 * contract, a stride that is zero or not a dword multiple, or offset
 * arithmetic that overflows uint32.  start is the draw's first vertex:
 * the model stream begins at buffer_offset + src_offset + start * stride
 * while the slot stream always begins at slot zero. */
bool r300_r2vb_producer_streams_init(uint32_t buffer_offset,
                                     uint32_t src_offset,
                                     uint32_t src_stride_bytes,
                                     enum pipe_format format, uint32_t start,
                                     struct r300_r2vb_producer_streams *out);

/* Slot-fetch gate value (R300_R2VB_SLOT_FETCH, exact "1"); pure parser. */
bool r300_r2vb_slot_fetch_gate_value(const char *value);

/* Model-source classification for the BO-fetch producer: a real winsys BO
 * fetches in place; a CPU-shadow resource (the no-TCL allocation policy
 * keeps non-custom vertex buffers in malloced_buffer with no winsys BO)
 * uploads its exact fetched subrange through the context uploader; a user
 * pointer stays outside the first contract because it carries no
 * resource-width authority.  Pure over the three source facts so the
 * calibration drives every arm. */
enum r300_r2vb_model_source_kind {
    /* Zero is the invalid state so a cleared record is fail-closed. */
    R300_R2VB_MODEL_UNSUPPORTED = 0,
    R300_R2VB_MODEL_REAL_BO,
    R300_R2VB_MODEL_CPU_SHADOW_UPLOAD,
};

enum r300_r2vb_model_source_kind
r300_r2vb_model_source_classify(bool is_user_buffer, bool has_winsys_bo,
                                bool has_cpu_shadow);

/* One materialized model fetch: an owned resource reference (released by
 * the caller after the CS takes its own) and the descriptor offset --
 * the upload subrange begins at the first fetched record, so the offset
 * is the uploader's, not the application's. */
struct r300_r2vb_model_fetch {
    enum r300_r2vb_model_source_kind kind;
    struct pipe_resource *resource;
    uint32_t gpu_offset;
    uint64_t uploaded_bytes;
};

struct pipe_vertex_buffer;
struct pipe_vertex_element;

/* Lifetime: init once, materialize into an empty record, fini on every
 * exit.  fini releases the owned reference and returns the record to
 * the fail-closed empty state; init alone never inspects the resource
 * pointer, so a fresh stack record must pass through init before any
 * other call. */
static inline void
r300_r2vb_model_fetch_init(struct r300_r2vb_model_fetch *m)
{
    memset(m, 0, sizeof(*m));
    m->kind = R300_R2VB_MODEL_UNSUPPORTED;
}

void r300_r2vb_model_fetch_fini(struct r300_r2vb_model_fetch *m);

bool
r300_r2vb_materialize_model_fetch_for_test(
    struct r300_context *r300, const struct pipe_vertex_buffer *vb,
    const struct pipe_vertex_element *ve, uint32_t start, uint32_t count,
    const struct r300_r2vb_producer_stream *model_stream,
    struct r300_r2vb_model_fetch *out);

/* The 3D_LOAD_VBPNTR format word packs the stride into an 8-bit dword
 * field; a host-accepted stride past it would truncate in the emitter. */
#define R300_R2VB_VBPNTR_STRIDE_DWORDS_MAX 255u

/* Emission-ready producer fetch: the stream contract plus the properties
 * only the draw and the bound resources can prove -- the final fetched
 * byte of every stream lies inside its BO (64-bit arithmetic), offsets
 * are dword-aligned, the stride covers a full FP32x4 record and fits the
 * packet field, and the VAP tuple carries through to VAP_VTX_SIZE. */
struct r300_r2vb_producer_fetch {
    struct r300_r2vb_producer_streams streams;
    uint32_t vap_vtx_size;
    uint32_t vf_min;
    uint32_t vf_max;
    uint64_t slot_required_bytes;
    uint64_t model_required_bytes;
};

bool r300_r2vb_producer_fetch_init(const struct r300_r2vb_producer_streams *s,
                                   uint32_t count, uint64_t slot_bo_bytes,
                                   uint64_t model_bo_bytes,
                                   struct r300_r2vb_producer_fetch *out);

/* Gate and floor parsers, pure over the string so the calibration test
 * drives every arm without process environment state. */
bool r300_r2vb_auto_single_gate_value(const char *value);
bool r300_r2vb_auto_single_floor_value(const char *value, uint32_t *floor);
const char *
r300_r2vb_auto_single_reason_str(enum r300_r2vb_auto_single_reason reason);

/* Draw-shape facts the policy consumes, gathered by the route at the draw
 * and constructed directly by the host oracle. */
struct r300_r2vb_auto_single_draw {
    unsigned mode;               /* enum mesa_prim */
    uint32_t count;
    uint32_t instance_count;
    uint32_t index_size;
    bool fs_reads_face;
    bool clip_planes_enabled;
    bool fs_reads_external_constants;
};

/* The pure admission policy: route-support shape first (plain TRIANGLES,
 * whole triangles, non-indexed, single instance, below the 16-bit re-ingest
 * ceiling, no face/clip-plane/FS-external dependency -- the clip-route
 * delivery contract), then both delivery cells (READY SINGLE untyped
 * one-input plans for cv=0 clip and cv=0 window), then the vertex floor.
 * Returns OK only when every gate holds. */
enum r300_r2vb_auto_single_reason
r300_r2vb_auto_single_policy(const struct r300_r2vb_producer_plan *clip_plan,
                             const struct r300_r2vb_producer_plan *window_plan,
                             const struct r300_r2vb_auto_single_draw *d,
                             uint32_t floor);

#ifdef __cplusplus
}
#endif

#endif /* R300_R2VB_PLAN_H */
