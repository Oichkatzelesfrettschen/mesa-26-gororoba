/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_PLAN_H
#define R300_R2VB_PLAN_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "amd/r300/common/r300_r2vb_source_contract.h"
#include "compiler/shader_enums.h"
#include "compiler/radeon_code.h"
#include "r300_context.h"
#include "r300_fs.h"
#include "r300_nir_ssa_cut.h"
#include "r300_r2vb.h"
#include "r300_r2vb_clip.h"

#ifdef __cplusplus
extern "C" {
#endif

struct r300_context;
struct nir_shader;
struct nir_intrinsic_instr;

/* Producer input ceiling: the split pass-B draw feeds every model attribute
 * plus one carry input.  The register authority is
 * src/gallium/drivers/r300/r300_reg.h for the VAP stream and RS register
 * definitions, and r300_context.h exposes eight VAP_PROG_STREAM_CNTL and
 * VAP_PROG_STREAM_CNTL_EXT entries plus eight IP and INST entries; state
 * derivation limits VAP_OUTPUT_VTX_FMT_1 and rasterizer texcoord allocation
 * to eight slots (r300_state_derived.c).  Symbol discovery uses
 * (rg --fixed-strings R300_VAP_PROG_STREAM_CNTL_0 src/gallium/drivers/r300/;
 * rg --fixed-strings R300_VAP_OUTPUT_VTX_FMT_1 src/gallium/drivers/r300/;
 * rg --fixed-strings R300_RS_IP_0 src/gallium/drivers/r300/;
 * rg --fixed-strings R300_R2VB_MAX_PRODUCER_INPUTS src/gallium/drivers/r300/).
 * The RS482 register-table notes in
 * docs/hardware/rs482-hybrid-vertex-tcl-design.md and the retained VAP/RS
 * captures are calibration evidence for that 0..7 surface, not primary
 * hardware authority.  Pass-B admission requires num_in + 1 <= 8.  A
 * nine-input RS482 pass-B capture that executes without truncation or decline
 * falsifies this bound; the calibrated software witness is reproducible with
 * `meson test -C build r300-r2vb-plan-oracle`. */
#define R300_R2VB_MAX_PRODUCER_INPUTS 8

/* The R2VB producer plan: one classification record per (vertex shader, plan
 * key) describing how the fragment-ALU producer would deliver the shader --
 * one pass, a carry-BO split, or a decline -- with every failure class
 * observed across the ranked cut candidates retained.  The plan is the
 * classification authority the pre-draw host mirror only predicts; route
 * reachability stays with the existing float-route admission until the typed
 * diagnostic gate consumes the plan directly. */

/* Plan validity is separate from rejection cause: READY and
 * SEMANTIC_REJECT are cacheable, and a TRANSIENT_FAILURE (allocation) is
 * retried on the next request instead of pinning a permanent gallivm
 * fallback.  Numeric values retain the archived census encoding. */
enum r300_r2vb_plan_status {
    R300_R2VB_PLAN_READY = 0,
    R300_R2VB_PLAN_SEMANTIC_REJECT = 1, /* a property of the workload */
    R300_R2VB_PLAN_TRANSIENT_FAILURE = 3, /* infrastructure; never cached */
};

enum r300_r2vb_plan_action {
    R300_R2VB_PLAN_REJECT = 0,
    R300_R2VB_PLAN_SINGLE = 1, /* position pass (and varying pass, when
                                * admitted) compiles under the 64-slot emit
                                * ceiling */
    R300_R2VB_PLAN_SPLIT = 3, /* over-budget position pass with an admitted
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
 * The vertex shader itself is the cache owner (the key lives per-VS), so it is
 * not a field. */
struct r300_r2vb_plan_key {
    bool allow_computed_varying;
    enum r300_r2vb_position_space space;
    enum r300_fs_input_semantics input_semantics;
    uint32_t viewport_scale[3];
    uint32_t viewport_translate[3];
};

/* The application source identity of the plan's position input, measured
 * on the bound VS at plan-build time: the surviving input variable's
 * driver location and its semantic rank among the VS inputs in ascending
 * location order.  The producer mapping contract consumes these measured
 * values; a caller passing literals asserts an identity the plan never
 * proved.  Re-ingest maps passthrough outputs by driver location because
 * component-packed variables share one physical vertex element. */
struct r300_r2vb_position_source {
    uint8_t app_driver_location;
    uint8_t location_rank;
    bool valid;
};

/* External constant-source correspondence for the retained position
 * producer.  AUTO_SINGLE mirrors only byte-addressed UBO block 0 through
 * the first 64 bytes; every other source domain stays unsupported. */
enum r300_r2vb_constant_source_contract {
    R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED = 0,
    R300_R2VB_CONSTANT_SOURCE_NONE,
    R300_R2VB_CONSTANT_SOURCE_UBO0_PREFIX64,
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
    struct r300_r2vb_position_source position_source;
    enum r300_r2vb_constant_source_contract constant_source;
    uint32_t constant_bytes;

    /* Computed-varying admission record (allow_computed_varying cells only):
     * the varying's slot, the single application input feeding it (same
     * rank/driver-location identity as position_source), and the UBO0 prefix
     * the varying producer reads.  varying_slot stays -1 on cells whose VS
     * carries no computed varying. */
    int varying_slot;
    struct r300_r2vb_position_source varying_source;
    enum r300_r2vb_constant_source_contract varying_constant_source;
    uint32_t varying_constant_bytes;

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
enum r300_r2vb_constant_source_contract
r300_r2vb_constant_source_scan(struct nir_shader *producer,
                               uint32_t *required_bytes);

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

/* Shadow-parity divergence accounting: assertion builds stop at a mismatch.
 * Assertion-disabled builds keep the admission memo authoritative, increment
 * this process-wide counter for the planner test and telemetry, and leave
 * rendering unchanged. */
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

/* Identify the output location carried by a lowered store intrinsic.  A
 * constant store_output offset within its declared slot range becomes part of
 * the target identity; indirect offsets and out-of-range stores return false,
 * keeping planner and live-restager admission on one delivery record. */
bool r300_r2vb_output_store_location(const struct nir_intrinsic_instr *intr,
                                     gl_varying_slot *location);

/* Remove output stores outside target from a caller-owned clone.  The planner
 * and live restager call this same target reduction before DCE and emission. */
void r300_r2vb_prune_output_stores(struct nir_shader *nir,
                                   gl_varying_slot target);

/* Reduce a caller-owned clone to the position producer before structural
 * admission.  Non-position stores and the dead dependencies that feed them
 * stay outside the cv=0 cell. */
void r300_r2vb_prune_position_only(struct nir_shader *nir);

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
/* Test calibration injects one failed position-input clone.  Normal driver
 * execution leaves this flag clear. */
void r300_r2vb_test_fail_position_input_clone_once(void);
/* Test calibration lets the first count succeed and the following clone fail,
 * matching a transient allocation failure after route preflight. */
void r300_r2vb_test_fail_position_input_clone_after_one(void);
/* Test calibration injects one transient failure in the source-identity scan
 * after the position-input count succeeds. */
void r300_r2vb_test_fail_position_source_clone_once(void);
/* Test calibration injects one transient failure at the shadow recount. */
void r300_r2vb_test_fail_shadow_recount_once(void);
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
    R300_R2VB_AUTO_SINGLE_PASSTHROUGH_ROUTE,
    R300_R2VB_AUTO_SINGLE_HARDWARE_TCL,
    R300_R2VB_AUTO_SINGLE_INDEXED,
    R300_R2VB_AUTO_SINGLE_INSTANCED,
    R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE,
    R300_R2VB_AUTO_SINGLE_COUNT_CEILING,
    R300_R2VB_AUTO_SINGLE_SLOT_LAYOUT,
    R300_R2VB_AUTO_SINGLE_SLOT_FETCH,
    R300_R2VB_AUTO_SINGLE_RAW_SUBMIT,
    R300_R2VB_AUTO_SINGLE_BO_MODE,
    R300_R2VB_AUTO_SINGLE_ORDERING_MODE,
    R300_R2VB_AUTO_SINGLE_MODE_CONFLICT,
    R300_R2VB_AUTO_SINGLE_QUERY_ACTIVE,
    R300_R2VB_AUTO_SINGLE_INPUT_UNCHECKED,
    R300_R2VB_AUTO_SINGLE_INPUT_CONSTANTS,
    R300_R2VB_AUTO_SINGLE_INPUT_CONSTANT_SOURCE,
    R300_R2VB_AUTO_SINGLE_INPUT_POSITION_SOURCE,
    R300_R2VB_AUTO_SINGLE_INPUT_INSTANCE_RATE,
    R300_R2VB_AUTO_SINGLE_INPUT_BUFFER_BINDING,
    R300_R2VB_AUTO_SINGLE_INPUT_SOURCE_CLASS,
    R300_R2VB_AUTO_SINGLE_INPUT_CPU_ACCESS,
    R300_R2VB_AUTO_SINGLE_INPUT_UPLOADER,
    R300_R2VB_AUTO_SINGLE_INPUT_FORMAT,
    R300_R2VB_AUTO_SINGLE_INPUT_STRIDE,
    R300_R2VB_AUTO_SINGLE_INPUT_SPAN,
    R300_R2VB_AUTO_SINGLE_FRONTFACE,
    R300_R2VB_AUTO_SINGLE_CLIP_PLANES,
    R300_R2VB_AUTO_SINGLE_FS_EXTERNAL_CONSTANTS,
    R300_R2VB_AUTO_SINGLE_PLAN_NOT_READY,
    R300_R2VB_AUTO_SINGLE_PLAN_NOT_SINGLE,
    R300_R2VB_AUTO_SINGLE_TYPED_SOURCE,
    R300_R2VB_AUTO_SINGLE_INPUT_SHAPE,
    R300_R2VB_AUTO_SINGLE_OUTPUT_STREAMS,
    R300_R2VB_AUTO_SINGLE_DELIVERY_CELL,
    R300_R2VB_AUTO_SINGLE_BELOW_VERTEX_FLOOR,
    R300_R2VB_AUTO_SINGLE_POINT_SIZE_WRITER,
    R300_R2VB_AUTO_SINGLE_POINT_COORD_STATE,
    R300_R2VB_AUTO_SINGLE_POINT_QUAD_RASTERIZATION,
    R300_R2VB_AUTO_SINGLE_POINT_VERTEX_SIZE,
    R300_R2VB_AUTO_SINGLE_REASON_COUNT,
};

/* Explicit slot-layout representation.  The policy names the shape rather
 * than deriving it from count alone, so the two representations of one
 * count (4096 as a legacy row versus 2048x2) stay separately addressable
 * for the layout-boundary silicon comparison.  LEGACY_ROW is the proven
 * one-row shape (width == count, height == 1) up to the 4096 storage
 * ceiling and rounds its physical pitch to an even pixel count.
 * GRID_2048 always uses width == pitch == 2048 with
 * height == ceil(count / 2048), the common rendered-and-sampled axis from
 * the RS482 virtualization matrix (2560 is the color-render axis; the
 * one-row 2559/2560/2561 boundary cells probe it under LEGACY_ROW). */
enum r300_r2vb_slot_layout_policy {
    R300_R2VB_LAYOUT_LEGACY_ROW,
    R300_R2VB_LAYOUT_GRID_2048,
};

/* Producer slot-grid layout: the slot-pixel stream maps slot s to raster
 * center ((s % width) + 0.5, (s / width) + 0.5) and the color target maps
 * that pixel to (y * pitch_pixels + x) * 16.  Every valid slot maps to
 * s * 16, so the re-ingest VAP reads the first count FP32x4 records as one
 * flat vertex array.  A one-row layout may carry an even-pitch tail after
 * its valid records.  A multirow layout requires pitch_pixels == width,
 * which keeps row transitions contiguous. */
struct r300_r2vb_slot_layout {
    enum r300_r2vb_slot_layout_policy policy;
    uint32_t count;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
    uint64_t storage_slots;
    uint64_t storage_bytes;
};

/* Pure over (count, policy); fails on count == 0, count >= 65536 (the
 * 16-bit re-ingest index ceiling), and -- LEGACY_ROW -- count > 4096. */
bool r300_r2vb_slot_layout_init_policy(uint32_t count,
                                       enum r300_r2vb_slot_layout_policy policy,
                                       struct r300_r2vb_slot_layout *out);

/* Count-derived wrapper: LEGACY_ROW through 4096, GRID_2048 above it when
 * grid_enabled, decline otherwise.  Production callers keep this form. */
bool r300_r2vb_slot_layout_init(uint32_t count, bool grid_enabled,
                                struct r300_r2vb_slot_layout *out);

/* Pure runtime selection.  An absent selector uses the count-derived policy;
 * "legacy_row" and "grid_2048" select an explicit representation.  The
 * grid representation additionally requires its exact-value gate.  Empty
 * and unrecognized values fail closed. */
bool r300_r2vb_slot_layout_select(uint32_t count, const char *selector,
                                  bool grid_enabled,
                                  struct r300_r2vb_slot_layout *out);

/* Resolve one valid logical slot in a canonical layout to its raster
 * coordinates and physical FP32x4 byte offset.  Accepted mappings equal the
 * flat re-ingest mapping slot * 16. */
bool r300_r2vb_slot_layout_address(
    const struct r300_r2vb_slot_layout *layout, uint32_t slot,
    uint32_t *x, uint32_t *y, uint64_t *byte_offset);

/* The count-scaled 3D_DRAW_IMMD producer remains bounded to the retained
 * one-input 1024-vertex payload.  The product covers one slot vector plus
 * every model input; larger draws use the fixed-size BO producer. */
bool r300_r2vb_immediate_producer_shape_ok(uint32_t count,
                                           unsigned num_model_inputs);

/* Rebase an application vertex buffer for a delivery draw whose producer
 * outputs begin at slot zero.  The element offset stays unchanged. */
bool r300_r2vb_rebased_buffer_offset(uint32_t buffer_offset,
                                     uint32_t stride_bytes, uint32_t start,
                                     uint32_t *rebased_offset);

/* Slot-grid gate value (R300_R2VB_SLOT_GRID, exact "1"); pure parser. */
bool r300_r2vb_slot_grid_gate_value(const char *value);

/* Production BO delivery selector (R300_R2VB_BO_DRAW, exact
 * "producer_deliver"); pure parser. */
bool r300_r2vb_bo_draw_delivery_mode_value(const char *value);

/* BO-fetched producer vertex streams: stream 0 is the slot-position BO
 * (FP32x4, stride 16, offset 0) and stream 1 is the application model
 * attribute fetched from its own buffer, so the producer draw carries no
 * count-scaled immediate payload.  The load-bearing invariant is the VAP
 * tuple: the summed fetch dwords equal the producer VAP_VTX_SIZE and the
 * declared PSC input tuple.  A GA front-end stall is the failure hypothesis
 * for a mismatch; the tuple contract itself remains the admission invariant.
 * The first contract is deliberately narrow: one R32G32B32A32_FLOAT model
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

/* Pure over the model element facts; fails on a format outside the admitted
 * source contract, a stride that is zero or not a dword multiple, or offset
 * arithmetic that overflows uint32.  start is the draw's first vertex:
 * the model stream begins at buffer_offset + src_offset + start * stride
 * while the slot stream always begins at slot zero.  The unsuffixed name is
 * the production entry and keeps the F32_2 gate closed; the _gated variant
 * takes the explicit flag and serves only the no-submit capture fixture. */
bool r300_r2vb_producer_streams_init(uint32_t buffer_offset,
                                     uint32_t src_offset,
                                     uint32_t src_stride_bytes,
                                     enum pipe_format format, uint32_t start,
                                     struct r300_r2vb_producer_streams *out);

bool r300_r2vb_producer_streams_init_gated(
    uint32_t buffer_offset, uint32_t src_offset, uint32_t src_stride_bytes,
    enum pipe_format format, uint32_t start, bool float2_enabled,
    struct r300_r2vb_producer_streams *out);

/* Slot-fetch gate value (R300_R2VB_SLOT_FETCH, exact "1"); pure parser. */
bool r300_r2vb_slot_fetch_gate_value(const char *value);

/* One materialized model fetch: an owned resource reference (released by
 * the caller after the CS takes its own) and the descriptor offset --
 * the upload subrange begins at the first fetched record, so the offset
 * is the uploader's, not the application's. */
struct r300_r2vb_model_fetch {
    enum r300_r2vb_model_source_kind kind;
    struct pipe_resource *resource;
    uint32_t gpu_offset;
    /* The exact contract this transaction materialized: the uploader is
     * a suballocator, so only [gpu_offset, gpu_offset + span_bytes)
     * belongs to this draw -- validating against the backing BO's full
     * width would let a count drift fetch a neighboring allocation. */
    uint32_t count;
    uint32_t stride_dwords;
    uint32_t record_dwords;
    uint64_t span_bytes;
    uint64_t uploaded_bytes;
};

struct pipe_vertex_buffer;
struct pipe_vertex_element;

/* Side-effect-free BO-fetch input admission.  Each status names one
 * deterministic boundary that the route proves before its decision token.
 * Allocation and command submission remain later runtime operations. */
enum r300_r2vb_producer_input_status {
    R300_R2VB_PRODUCER_INPUT_UNCHECKED = 0,
    R300_R2VB_PRODUCER_INPUT_OK,
    R300_R2VB_PRODUCER_INPUT_CONSTANTS,
    R300_R2VB_PRODUCER_INPUT_CONSTANT_SOURCE,
    R300_R2VB_PRODUCER_INPUT_POSITION_SOURCE,
    R300_R2VB_PRODUCER_INPUT_INSTANCE_RATE,
    R300_R2VB_PRODUCER_INPUT_BUFFER_BINDING,
    R300_R2VB_PRODUCER_INPUT_SOURCE_CLASS,
    R300_R2VB_PRODUCER_INPUT_CPU_ACCESS,
    R300_R2VB_PRODUCER_INPUT_UPLOADER,
    R300_R2VB_PRODUCER_INPUT_FORMAT,
    R300_R2VB_PRODUCER_INPUT_STRIDE,
    R300_R2VB_PRODUCER_INPUT_SPAN,
    R300_R2VB_PRODUCER_INPUT_STATUS_COUNT,
};

enum r300_r2vb_producer_input_status
r300_r2vb_producer_input_preflight(
    const struct r300_r2vb_producer_plan *plan,
    const struct pipe_vertex_element *velems, unsigned velem_count,
    const struct pipe_vertex_buffer *vertex_buffers,
    unsigned nr_vertex_buffers, uint32_t start, uint32_t count);

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

/* The 3D_LOAD_VBPNTR format word packs the stride into an 8-bit dword
 * field; a host-accepted stride past it would truncate in the emitter. */
#define R300_R2VB_VBPNTR_STRIDE_DWORDS_MAX \
    R300_R2VB_SOURCE_STRIDE_DWORDS_MAX

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

/* The unsuffixed entry keeps the F32_2 gate closed; the _gated variant
 * serves only the no-submit capture fixture. */
bool r300_r2vb_producer_fetch_init_gated(
    const struct r300_r2vb_producer_streams *s, uint32_t count,
    uint64_t slot_bo_bytes, uint64_t model_bo_bytes, bool float2_enabled,
    struct r300_r2vb_producer_fetch *out);

/* Gate and floor parsers, pure over the string so the calibration test
 * drives every arm without process environment state. */
bool r300_r2vb_auto_single_gate_value(const char *value);
bool r300_r2vb_standing_gate_value(const char *value);
bool r300_r2vb_auto_single_floor_value(const char *value, uint32_t *floor);
const char *
r300_r2vb_auto_single_reason_str(enum r300_r2vb_auto_single_reason reason);
enum r300_r2vb_auto_single_reason
r300_r2vb_auto_single_route_reason(enum r300_r2vb_verdict verdict);

/* Draw-shape facts the policy consumes, gathered by the route at the draw
 * and constructed directly by the host oracle. */
struct r300_r2vb_auto_single_draw {
    /* The shared route classifier owns the outer path class.  OK means the
     * producer candidate reaches the detailed AUTO_SINGLE contract. */
    enum r300_r2vb_auto_single_reason route_reason;
    unsigned mode;               /* enum mesa_prim */
    uint32_t count;
    uint32_t instance_count;
    uint32_t index_size;
    bool fs_reads_face;
    bool clip_planes_enabled;
    bool fs_reads_external_constants;
    bool slot_layout_available;
    bool uses_grid_layout;
    bool slot_fetch_enabled;
    bool raw_submit_accepted;
    bool bo_draw_mode_compatible;
    bool bo_delivery_ordering_compatible;
    bool route_mode_compatible;
    bool query_active;
    /* Fixed-size point contract: the re-ingest transports position (plus an
     * admitted computed varying) only, so a per-vertex point size (VS PSIZ
     * writer or rasterizer point_size_per_vertex) or a sprite-coordinate /
     * point-quad request would be silently dropped by delivery. The effective
     * rasterized-point classification includes polygon-mode POINT triangles.
     * Each names its own decline. */
    bool rasterizer_emits_points;
    bool vs_writes_point_size;
    bool sprite_coord_requested;
    bool point_quad_rasterization;
    bool point_size_per_vertex;
    enum r300_r2vb_delivery_stream_status delivery_stream_status;
    enum r300_r2vb_producer_input_status producer_input_status;
};

/* Process-mode values that can replace, discard, split, or diagnose the
 * production AUTO_SINGLE path.  Presence-sensitive modes retain their raw
 * pointer so even an empty value conflicts when the consuming path treats
 * getenv() presence as enabled. */
struct r300_r2vb_auto_single_mode_values {
    const char *diagnostic;
    const char *barrier;
    const char *inspect;
    const char *clip_classify;
    const char *clip_edge;
    const char *budget_escape;
    const char *typed_split;
    const char *force_split;
    const char *varying;
    const char *delivery_capture;
};

bool r300_r2vb_auto_single_mode_values_compatible(
    const struct r300_r2vb_auto_single_mode_values *values);

/* The pure admission policy: route-support shape first (plain TRIANGLES with
 * whole triangles, or POINTS under the fixed-size point contract,
 * non-indexed, single instance, below the 16-bit re-ingest
 * ceiling, no face/clip-plane/FS-external dependency -- the clip-route
 * delivery contract), then both delivery cells (READY SINGLE untyped
 * one-input plans for cv=0 clip and cv=0 window), then the vertex floor.
 * Returns OK only when every gate holds. */
enum r300_r2vb_auto_single_reason
r300_r2vb_auto_single_policy(const struct r300_r2vb_producer_plan *clip_plan,
                             const struct r300_r2vb_producer_plan *window_plan,
                             const struct r300_r2vb_auto_single_draw *d,
                             uint32_t floor);


/* Position-input mapping for the BO-fetch producer: the plan's single
 * position input reads application input location 0, and the gallium
 * vertex-element convention feeds VS input i from element i, so the
 * position source is exactly velem[0].  The classifier proves the rest
 * of that identity -- the element exists, its buffer binding is in
 * range and bound, and its format is one of the admitted families --
 * and everything else declines to gallivm as position_input_mapping.
 * Pure over the element facts so the calibration drives every arm. */
bool r300_r2vb_position_input_mapping_ok(unsigned num_position_inputs,
                                         unsigned app_driver_location,
                                         unsigned location_rank,
                                         unsigned velem_count,
                                         unsigned vertex_buffer_index,
                                         unsigned nr_vertex_buffers,
                                         bool buffer_bound,
                                         enum pipe_format format);

/* The producer's programmable-stream interface, derived mechanically
 * from the validated stream contract: both elements pack into the first
 * PROG_STREAM_CNTL/EXT register pair (two 16-bit elements per dword,
 * the r300_swtcl_vertex_psc packing), the hardware data type and the
 * non-default swizzle come from the shared pipe_format translators --
 * FLOAT_3 receives its W from the swizzle's constant-one select -- the
 * model element carries LAST_VEC, and registers 1..7 are zeroed so no
 * stale multi-stream state survives into the producer draw.
 * VAP_VTX_SIZE is the physical record sum; the assembly registers
 * (VTX_STATE_CNTL, VSM_VTX_ASSM, OUTPUT_VTX_FMT) calibrate against the
 * working immediate producer's draw-adjacent decode rather than being
 * derived here. */
struct r300_r2vb_producer_interface {
    uint32_t prog_stream_cntl[8];
    uint32_t prog_stream_cntl_ext[8];
    uint32_t vap_vtx_size;
};

bool r300_r2vb_producer_interface_init(
    const struct r300_r2vb_producer_fetch *fetch,
    unsigned slot_dst_vec_loc, unsigned model_dst_vec_loc,
    struct r300_r2vb_producer_interface *out);

/* The unsuffixed entry keeps the F32_2 gate closed; the _gated variant
 * serves the no-submit capture fixture. */
bool r300_r2vb_producer_interface_init_gated(
    const struct r300_r2vb_producer_fetch *fetch,
    unsigned slot_dst_vec_loc, unsigned model_dst_vec_loc,
    bool float2_enabled, struct r300_r2vb_producer_interface *out);

/* Rebind the application stream contract onto the materialized GPU
 * objects: the model offset becomes the transaction's gpu_offset (for
 * an upload, the uploader's suballocation, not the application offset),
 * and the emission-ready fetch object revalidates against the ACTUAL
 * resource widths.  This second validation is the object the PM4 arm
 * consumes; the pre-materialization stream must never reach the
 * emitter. */
bool
r300_r2vb_producer_streams_rebind(const struct r300_r2vb_producer_streams *orig,
                                  const struct r300_r2vb_model_fetch *model,
                                  uint64_t slot_bo_bytes, uint32_t count,
                                  struct r300_r2vb_producer_fetch *out);

struct nir_shader;
bool r300_r2vb_position_source_scan(struct nir_shader *vs_nir,
                                    struct r300_r2vb_position_source *out);
bool r300_r2vb_position_source_scan_status(
    struct nir_shader *vs_nir, struct r300_r2vb_position_source *out,
    bool *transient_failure);

/* Source identity of the one application input feeding the computed varying
 * at `slot`: strip every store except that varying's, DCE, and require
 * exactly one surviving input.  Same rank/driver-location record as the
 * position scan, so the BO-fetch route feeds the varying pass through the
 * identical single-model-stream contract. */
bool r300_r2vb_varying_source_scan(struct nir_shader *vs_nir, int slot,
                                   struct r300_r2vb_position_source *out);

/* The element mapper the delivery route uses (rank among VS inputs in
 * ascending location order), exported so the rank oracle proves the
 * scan and the mapper share one convention. */
struct nir_variable;
int r300_r2vb_input_velem_index_for_test(struct nir_shader *vs,
                                         const struct nir_variable *in);

/* The producer FS input contract: exactly one generic input and nothing
 * else -- no colors, no FACE, no fog, no WPOS -- verified against the
 * compiled variant's retained semantics.  The hardware input register
 * replays the compiler's allocation order (colors, face, generics, fog,
 * WPOS), so the single generic lands in register 0; the helper returns
 * that derived register rather than assuming it. */
struct r300_shader_semantics;
bool r300_r2vb_producer_fs_input_hwreg(
    const struct r300_shader_semantics *inputs, unsigned *out_hwreg);

/* One authoritative record binding the three producer index namespaces:
 * the application input location/rank feeding the model bytes, the
 * VAP destination vector locations the fetched streams land in, and the
 * FS hardware input register the rasterizer routes the model vector to.
 * The constructor fails unless the selected measured source, the compiled
 * producer FS semantics, and the derived RS block all describe the same
 * binding. */
struct r300_r2vb_producer_logical_binding {
    uint8_t app_driver_location;
    uint8_t location_rank;
    uint8_t velem_index;
    uint8_t slot_dst_vec_loc;
    uint8_t model_dst_vec_loc;
    uint8_t fs_hw_input_reg;
};

/* Calibrated producer destination vectors, from the retained
 * immediate-producer decode: PROG_STREAM_CNTL_0 = 0x26030003 places the
 * slot position at VAP destination vector 0 and the model attribute at
 * destination vector 6 with LAST_VEC.  The runtime constructor requires
 * the live derived stream state to decode to these values; a derived
 * state naming other vectors is a route regression, not a new binding. */
#define R300_R2VB_CAL_SLOT_DST_VEC_LOC 0
#define R300_R2VB_CAL_MODEL_DST_VEC_LOC 6

struct r300_rs_block;
bool r300_r2vb_producer_logical_binding_init(
    const struct r300_r2vb_position_source *source,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    unsigned slot_dst_vec_loc, unsigned model_dst_vec_loc,
    struct r300_r2vb_producer_logical_binding *out);

/* Complete pre-emission contract check over a prospective transaction:
 * physical fetch extent, decoded PSC swizzle meaning, LAST_VEC
 * placement, VAP assembly, output tuple, RS component routing, the FS
 * input register, and the zeroed stream tail.  Returns a bitmask so a
 * violating transaction reports every broken layer, not the first. */
enum r300_r2vb_producer_binding_violation {
    R300_R2VB_BINDING_FETCH_SIZE = 1u << 0,
    R300_R2VB_BINDING_SWIZZLE = 1u << 1,
    R300_R2VB_BINDING_LAST_VEC = 1u << 2,
    R300_R2VB_BINDING_VAP_ASSEMBLY = 1u << 3,
    R300_R2VB_BINDING_OUTPUT_FMT = 1u << 4,
    R300_R2VB_BINDING_RS_COMPONENTS = 1u << 5,
    R300_R2VB_BINDING_FS_REGISTER = 1u << 6,
    R300_R2VB_BINDING_TAIL_STATE = 1u << 7,
    R300_R2VB_BINDING_GB_STATE = 1u << 8,
};

/* Runtime binding constructor: derives the destination vectors from
 * the DERIVED producer stream state (never caller literals), requires
 * them to match the calibrated locations, and builds the binding
 * against the selected model source, the compiled producer FS
 * semantics, and the exact RS contract.  A null model source selects
 * the plan's position source.  This is the only constructor a runtime
 * path calls; the literal-argument form stays for the calibration oracle. */
struct r300_vertex_stream_state;
bool r300_r2vb_producer_logical_binding_from_state(
    const struct r300_r2vb_producer_plan *plan,
    const struct r300_r2vb_position_source *model_source,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    const struct r300_vertex_stream_state *psc,
    struct r300_r2vb_producer_logical_binding *out);

unsigned r300_r2vb_producer_binding_check(
    const struct r300_r2vb_producer_fetch *fetch,
    const struct r300_r2vb_producer_interface *psc,
    const struct r300_r2vb_producer_logical_binding *binding,
    const struct r300_rs_block *rs);

/* The all-fallible-work-before-emission producer transaction: every
 * operation that can fail -- source identity, element eligibility,
 * stream construction, model materialization, span rebind, PSC and
 * logical-binding derivation, the complete violation check, and
 * command-stream capacity with relocation indices -- completes before
 * the emitter writes a register.  The validate phase performs the
 * fallible host work and takes the referenced resources; the CS phase
 * orders capacity before buffer addition before relocation lookup and
 * rechecks the snapshot identities; fini releases what validate took. */
/* Transaction lifecycle: init yields EMPTY, validate advances EMPTY to
 * VALIDATED, CS staging advances VALIDATED to READY, emission advances
 * READY to EMITTED, and fini returns any state to EMPTY.  Each operation
 * requires its exact predecessor state and declines otherwise, so an
 * owned transaction is never overwritten and no phase runs twice. */
enum r300_r2vb_producer_bo_draw_state {
    R300_R2VB_BO_DRAW_EMPTY = 0,
    R300_R2VB_BO_DRAW_VALIDATED,
    R300_R2VB_BO_DRAW_READY,
    R300_R2VB_BO_DRAW_EMITTED,
};

struct r300_r2vb_producer_bo_draw {
    /* Snapshot identity: the authorities the transaction was built
     * against, compared by pointer at the CS phase. */
    const struct r300_r2vb_producer_plan *plan;
    const struct r300_shader_semantics *fs_inputs;
    const struct r300_rs_block *rs;
    const struct r300_vertex_stream_state *psc_state;
    uint32_t draw_start;
    uint32_t count;
    enum r300_r2vb_position_space space;

    /* Validated storage; validate references the slot, model, and
     * output resources and fini releases all three. */
    struct r300_r2vb_slot_layout layout;
    struct pipe_resource *slot_resource;
    struct r300_r2vb_model_fetch model;

    /* Output authority: the producer render target, proven identical to
     * the bound framebuffer color target.  Its relocation rides the
     * dirty framebuffer state atom, so the fixed custom command size
     * carries only the two input-array relocations and the recorded
     * output relocation index is diagnostic. */
    struct pipe_resource *output_resource;
    uint64_t output_required_bytes;
    uint64_t output_valid_bytes;
    uint32_t output_offset;
    uint32_t output_pitch_pixels;

    /* GPU-facing contract. */
    struct r300_r2vb_producer_fetch fetch;
    struct r300_r2vb_producer_interface psc;
    struct r300_r2vb_producer_logical_binding logical;

    /* CS bookkeeping; relocation indices belong to the CS that was
     * current when the CS phase ran. */
    unsigned required_cs_dwords;
    int slot_reloc_index;
    int model_reloc_index;
    int output_reloc_index;
    enum r300_r2vb_producer_bo_draw_state state;

    /* By-value snapshots of the mutable derived authorities.  The RS
     * block and PSC words are context-derived state updated in place, so
     * pointer identity does not freeze them; the emitter consumes these
     * copies, and CS staging value-compares them against the live state
     * before and after the final CS validation. */
    struct r300_vertex_stream_state psc_snapshot;
    struct r300_rs_block rs_snapshot;
    struct r300_viewport_state viewport_snapshot;
};

/* Sets the transaction to EMPTY.  Required before the first validate;
 * validate declines an owned or uninitialized transaction. */
void r300_r2vb_producer_bo_draw_init(struct r300_r2vb_producer_bo_draw *txn);

/* Fixed command size of the BO-fetch producer draw: every register the
 * emitter writes, the two-array LOAD_VBPNTR with both relocation
 * records, and the VBUF draw.  Count-independent, so equal packet
 * length across 3..4096 is a compile-time fact the capture ladder
 * re-proves by cursor delta. */
unsigned r300_r2vb_producer_bo_draw_cs_dwords(void);

struct pipe_vertex_buffer;
struct pipe_vertex_element;
bool r300_r2vb_producer_bo_draw_validate(
    struct r300_context *r300,
    const struct r300_r2vb_producer_plan *plan,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    const struct r300_vertex_stream_state *psc_state,
    const struct pipe_vertex_buffer *vb, const struct pipe_vertex_element *ve,
    unsigned velem_count, unsigned nr_vertex_buffers,
    const struct r300_r2vb_slot_layout *layout,
    struct pipe_resource *slot_resource,
    struct pipe_resource *output_resource, uint32_t start, uint32_t count,
    enum r300_r2vb_position_space space,
    const struct r300_r2vb_position_source *model_source,
    struct r300_r2vb_producer_bo_draw *out);

bool r300_r2vb_producer_bo_draw_stage_cs(
    struct r300_context *r300, struct r300_r2vb_producer_bo_draw *txn,
    const struct r300_r2vb_producer_plan *plan,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    const struct r300_vertex_stream_state *psc_state);

/* Mechanical emission of the fixed 64-dword producer draw from the
 * transaction's snapshot words alone.  Requires READY (the complete
 * buffer list passed cs_validate at staging).  The caller lands dirty
 * application atoms before the raw producer prologue; this function emits
 * only the calibrated stream/RS/draw range.  Its cursor delta equals
 * cs_dwords() by construction. */
bool r300_r2vb_producer_bo_draw_emit(struct r300_context *r300,
                                     struct r300_r2vb_producer_bo_draw *txn);

void r300_r2vb_producer_bo_draw_fini(struct r300_r2vb_producer_bo_draw *txn);

bool
r300_r2vb_materialize_model_fetch_for_test(
    struct r300_context *r300, const struct pipe_vertex_buffer *vb,
    const struct pipe_vertex_element *ve, uint32_t start, uint32_t count,
    const struct r300_r2vb_producer_stream *model_stream,
    struct r300_r2vb_model_fetch *out);

#ifdef __cplusplus
}
#endif

#endif /* R300_R2VB_PLAN_H */
