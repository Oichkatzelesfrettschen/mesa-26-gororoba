/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_COMPUTE_ADMISSION_H
#define R300_COMPUTE_ADMISSION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nir_shader;

/* Why a compute kernel cannot lower to the RS482 compute-as-raster substrate.
 * Each reason names a hardware constraint: the substrate has texture-LD load,
 * FP24 ALU compute, RB3D export store, the blend ADD/MIN/MAX/SUB + stencil +
 * ZPASS reduction forms, ROP bitwise, and per-pixel predicates -- but no LDS,
 * no workgroup barrier, no general atomic on an arbitrary address, no
 * arbitrary read-write storage, and no FP64. */
enum r300_compute_reject {
   R300_COMPUTE_ADMIT = 0,
   R300_COMPUTE_REJECT_SHARED_MEMORY,  /* no LDS on R3xx */
   R300_COMPUTE_REJECT_BARRIER,        /* fragments are not a synchronized workgroup */
   R300_COMPUTE_REJECT_GENERAL_ATOMIC, /* only blend-add/min/max/sub, stencil, ZPASS exist */
   R300_COMPUTE_REJECT_RW_STORAGE,     /* only texture-load + RT-export, no scatter */
   R300_COMPUTE_REJECT_FP64,           /* ALU is FP24; no double precision */
};

/* Result of classifying a compute nir_shader.  classify-only: the analysis
 * never mutates the shader and never lowers or executes it. */
struct r300_compute_admission {
   bool admissible;
   enum r300_compute_reject reason;
   const char *detail; /* static string naming the offending construct */
};

/* Classify a MESA_SHADER_COMPUTE nir_shader against the RS482 substrate.
 * Pure read-only analysis: walks the shader once, fills *out, mutates nothing.
 * out->admissible is true with reason R300_COMPUTE_ADMIT when no unsupported
 * construct is present; otherwise the first unsupported construct sets the
 * reason and a static detail string. */
void r300_nir_classify_compute(const struct nir_shader *s,
                               struct r300_compute_admission *out);

/* Human-readable name of a rejection reason (static string). */
const char *r300_compute_reject_name(enum r300_compute_reject reason);

/* Enum-keyed reject-reason registry.  The detail field on struct
 * r300_compute_admission names the SPECIFIC offending construct (an intrinsic or
 * opcode name) found while classifying; this registry names the CATEGORY-level
 * reason keyed by the enum: a stable machine key and the absent hardware
 * capability that makes the construct unlowerable.  One row exists per enum
 * value; a build-time assert in r300_compute_admission.c keeps the table and the
 * enum from diverging. */
struct r300_compute_reject_row {
   enum r300_compute_reject reason;
   const char *key;               /* stable machine-readable key */
   const char *substrate_absence; /* the absent hardware capability */
};

/* Registry row for a reason; never NULL for a valid enum value. */
const struct r300_compute_reject_row *
r300_compute_reject_lookup(enum r300_compute_reject reason);

/* The absent hardware capability behind a rejection (static string). */
const char *r300_compute_reject_substrate_absence(enum r300_compute_reject reason);

/* Identity-map pattern recognized at compute-pipeline-create time so the
 * dispatch-replay can lower the kernel to a fullscreen-quad fragment draw that
 * samples in_tex (NEAREST) and writes the RB3D color export.  The pattern is
 * `out_buffer[gid] = in_buffer[gid]`: one store_ssbo of a value loaded by one
 * load_ssbo, with the two ssbo bindings recorded so the dispatch can resolve
 * them through the bound descriptor sets to a pipe_sampler_view (input) and a
 * pipe_surface (output).
 *
 * The index-equivalence between load and store is not asserted by the
 * detection (a deep ssa-chain trace would prove it); a non-identity index
 * relationship is caught empirically by the read-back oracle. */
struct r300_compute_identity_pattern {
   bool       is_identity_map;
   uint32_t   input_ssbo_binding;   /* binding index of the load_ssbo source */
   uint32_t   output_ssbo_binding;  /* binding index of the store_ssbo dest */
   uint8_t    value_components;     /* stored value vector width */
   uint8_t    value_bit_size;       /* stored value component width */
   bool       value_is_float;       /* store_ssbo src_type base == nir_type_float */
};

/* Detect the identity-map pattern in a classify-admitted kernel.  Pure
 * read-only analysis.  Sets out->is_identity_map = true when exactly one
 * store_ssbo's value is the result of exactly one load_ssbo; the bindings are
 * the canonical (binding=0) form r300_nir_classify_compute already enforces
 * for store_ssbo, but load_ssbo's binding is read off the load's src[0]
 * descriptor. */
void r300_nir_detect_identity_map(const struct nir_shader *s,
                                  struct r300_compute_identity_pattern *out);

/* Texture-pair binary-map pattern: exactly one store_ssbo whose value is the
 * result of a single ALU op whose two sources are exactly the SSA defs of
 * two distinct load_ssbo intrinsics.  The recognized ALU op set is bounded
 * by what the FP24 fragment ALU reproduces exactly: iadd / isub / imul /
 * imin / imax / umin / umax / fadd / fsub / fmul / fmin / fmax for the first
 * cut; richer arithmetic is a later extension.
 *
 * alu_op carries the NIR opcode value so the orchestrator's FS synthesis
 * picks the right PFS instruction; the bindings are 0 when the post-
 * explicit_io load_ssbo / store_ssbo binding sources are not constants
 * (the orchestrator's descriptor-set layout fallback recovers them then). */
struct r300_compute_binary_map_pattern {
   bool       is_binary_map;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
   uint16_t   alu_op;     /* nir_op enum value, only valid if is_binary_map */
   uint8_t    value_components; /* store_ssbo value vector width */
   uint8_t    value_bit_size;   /* store_ssbo value component width */
   bool       value_is_float;   /* result type, not a promise of FP32 exactness */
};

void r300_nir_detect_binary_map(const struct nir_shader *s,
                                struct r300_compute_binary_map_pattern *out);

/* Blend-add-reduction kernel pattern: a kernel whose store value is an
 * atomicAdd of a load_ssbo result, where the atomic's target buffer is a
 * small output histogram and the atomic's offset folds the dispatch grid into
 * a smaller bin range -- the canonical shape:
 *
 *     uint gid = gl_GlobalInvocationID.x;
 *     uint bin = gid & BIN_MASK;
 *     atomicAdd(out_data[bin], in_data[gid]);
 *
 * On RS482 this lowers to a blend-add accumulation: the output buffer binds
 * as a 1xM RT, the blend equation is RB3D_CBLEND.COMB_FCN_ADD with
 * blend_func = (ONE, ONE), the orchestrator draws one fragment per gid at
 * position (bin, 0), and the RB3D blend hardware accumulates the per-gid
 * value into the bin cell.  The mechanism is hardware-confirmed at the
 * substrate verb level.
 *
 * value_ssbo_binding is the binding of the load_ssbo feeding the atomic
 * value; output_ssbo_binding is the binding of the atomic's target buffer
 * (the histogram).  When the post-explicit_io binding sources are not
 * constants, both stay 0 and the orchestrator's descriptor-set layout
 * fallback recovers them (same policy as the binary-map detector).  alu_op
 * holds nir_op_iadd for the first cut; fadd will land alongside in a future
 * extension when the FP24 envelope analysis confirms the per-bin sum stays
 * exact. */
struct r300_compute_blend_acc_reduction_pattern {
   bool       is_blend_acc_reduction;
   uint32_t   value_ssbo_binding;
   uint32_t   output_ssbo_binding;
   uint16_t   alu_op;     /* nir_op enum value, only valid if true */
};

void r300_nir_detect_blend_acc_reduction(const struct nir_shader *s,
                                         struct r300_compute_blend_acc_reduction_pattern *out);

/* ZPASS coverage-count reduction kernel pattern.  Recognises the
 * predicate-gated counter shape:
 *
 *     uint gid = gl_GlobalInvocationID.x;
 *     if (in_data[gid] >= THRESHOLD)
 *         atomicAdd(count_out, 1u);
 *
 * On RS482 this lowers to the depth/stencil unit's ZPASS coverage-count verb:
 * the orchestrator binds a 1xN RT, draws N point primitives at
 * (gid_norm_x, 0), each fragment KILL-discards when the per-vertex-baked
 * predicate is false; the ZB_ZPASS_DATA / ZB_ZPASS_ADDR counter pair
 * accumulates the per-pipe surviving-fragment count.  Mesa's existing
 * r300_query.c chain (r300_create_query + begin_query + end_query +
 * get_query_result) wraps this register pair as PIPE_QUERY_OCCLUSION_COUNTER
 * returning a u64 fragment sum; the orchestrator writes that sum to
 * count_out[0].  The mechanism is hardware-confirmed at the substrate verb
 * level (surviving-fragment count read back through the occlusion query).
 *
 * Discriminator from the blend-acc reduction:
 *
 *   blend-acc: 1 ssbo_atomic-iadd + 1 load_ssbo, the atomic's value
 *              source SSA == load's def (load's RESULT is the
 *              accumulated value).
 *   zpass:     1 ssbo_atomic-iadd + 1 load_ssbo, the atomic's value
 *              source is the CONSTANT 1 (NOT the load's def); the load
 *              feeds an nir_if condition that GATES the atomic.
 *
 * value_ssbo_binding is the binding of the predicate load_ssbo;
 * output_ssbo_binding is the binding of the single-element counter
 * buffer.  Bindings stay 0 when the post-explicit_io binding sources
 * are not constants (the orchestrator's positional fallback recovers them). */
struct r300_compute_zpass_reduction_pattern {
   bool       is_zpass_reduction;
   uint32_t   value_ssbo_binding;     /* binding of the predicate-source load_ssbo */
   uint32_t   output_ssbo_binding;    /* binding of the single-element counter */
   uint16_t   alu_op;                 /* nir_op_iadd for the first cut */
};

void r300_nir_detect_zpass_reduction(const struct nir_shader *s,
                                     struct r300_compute_zpass_reduction_pattern *out);

/* Multipass FBO ping-pong scan kernel pattern.  Recognises the per-element
 * self-iterated shape:
 *
 *     uint x = in_data[gid];
 *     for (uint k = 0u; k < pass_count; k++) x = x * 2u;
 *     out_data[gid] = x;
 *
 * where pass_count is a runtime value loaded from a params storage buffer
 * (binding 2) so the loop does NOT constant-fold.  On RS482 this lowers to a
 * multipass FBO ping-pong: the orchestrator runs pass_count dependent fragment
 * passes binding the prior pass's render target as the next pass's sampler,
 * each pass applying the per-iteration step (the doubling) to the texel.  The
 * mechanism is hardware-confirmed at the EGL/GBM render-ladder level.
 *
 * The kernel NIR is classified-then-discarded (ralloc_free in
 * r300vk_classify_compute_kernel), never compiled to the R300 fragment
 * program, so the runtime loop is a detection-time signal only -- the
 * recognised shape is realized entirely by the orchestrator's pass ladder,
 * not by executing the kernel's own loop.
 *
 * Discriminator from every other admitted shape: the presence of a nir_loop.
 * Identity-map, binary-map, blend-acc, and ZPASS are all loop-free; this is
 * the only shape carrying a loop whose body is a self-only arithmetic step
 * (the loop-carried value is not a cross-element gather, which would need a
 * workgroup barrier the substrate lacks).
 *
 * step_op holds the per-iteration nir_op (nir_op_imul for the doubling first
 * cut).  Bindings stay 0 when the post-explicit_io binding sources are not
 * constants (the orchestrator's positional fallback: binding 0 = input,
 * 1 = output, 2 = params). */
struct r300_compute_multipass_scan_pattern {
   bool       is_multipass_scan;
   uint32_t   input_ssbo_binding;     /* binding of the per-element data load */
   uint32_t   output_ssbo_binding;    /* binding of the store dest */
   uint16_t   step_op;                /* per-iteration nir_op (imul for doubling) */
};

void r300_nir_detect_multipass_scan_pattern(const struct nir_shader *s,
                                            struct r300_compute_multipass_scan_pattern *out);

/* Predicated masked-store pattern recognized at compute-pipeline-create time.
 * The shape is the per-element conditional store
 *
 *     if (in_pred[gid] != 0u) out_data[gid] = in_val[gid];
 *
 * glslang emits this as a real control-flow branch (OpBranchConditional ->
 * nir_if) with the single store_ssbo INSIDE the conditional and two load_ssbo
 * (the predicate and the value); out_data is never loaded.  A side-effecting
 * store cannot be speculatively if-converted to a bcsel (that would need an
 * unsafe load of out_data), so the conditional store survives to the detector.
 * On RS482 this lowers to a per-pixel predicate discard: the orchestrator seeds
 * a render target from out_data's pre-existing contents, draws a fullscreen
 * quad whose fragment program KILL_IFs the masked fragments and writes the
 * sampled value for the covered ones, then copies the RT back to out_data --
 * killed fragments perform no ROP write, so the masked cells keep the seeded
 * baseline.  The per-pixel-predicate verb is the M-H realization (stream-
 * compaction precursor).
 *
 * Discriminator from every prior admitted shape: a store that is CONDITIONAL
 * (inside a nir_if) with load_count == 2.  Identity-map needs load_count == 1;
 * binary-map needs the store value to be a binary ALU op (this store value is a
 * plain load_ssbo def); blend-acc and ZPASS need an atomic; multipass needs a
 * loop.  Bindings stay 0 when the post-explicit_io binding sources are not
 * constants (the orchestrator's positional fallback: binding 0 = predicate,
 * 1 = value, 2 = output). */
struct r300_compute_predicated_store_pattern {
   bool       is_predicated_store;
   uint32_t   pred_ssbo_binding;      /* binding feeding the nir_if condition */
   uint32_t   value_ssbo_binding;     /* binding of the stored value load */
   uint32_t   output_ssbo_binding;    /* binding of the conditional store dest */
};

void r300_nir_detect_predicated_store_pattern(const struct nir_shader *s,
                                              struct r300_compute_predicated_store_pattern *out);

/* Multi-tap neighborhood gather (convolution) pattern.  Recognises the
 * unweighted N-tap shape
 *
 *     out_data[gid] = in_data[gid+o0] + in_data[gid+o1] + ... ;   // N >= 3
 *
 * one store_ssbo whose value is an integer add-reduction tree (nested
 * nir_op_iadd) whose every leaf is a load_ssbo def, with at least three taps,
 * no atomic, and no loop.  On RS482 this lowers to a multi-TEX fragment draw:
 * the input SSBO binds as a PIPE_TEXTURE_2D (the identity-map substrate), the
 * synthesized fragment program samples it at N neighbourhood taps in one
 * texture clause, sums them in the FP24 ALU, and writes the RB3D color export
 * -- the texture-pair binary-map generalised to N taps of one sampler plus a
 * sum.
 *
 * Discriminator from every prior admitted shape: an add-reduction of >= 3
 * load_ssbo leaves.  identity-map needs load_count == 1; binary-map needs the
 * store value to be an ALU op with EXACTLY two inputs, both load_ssbo defs (an
 * N>=3 nested iadd has an iadd, not a load, as its first input, so the
 * binary-map two-input test rejects it); blend-acc and ZPASS need an atomic;
 * multipass needs a loop.
 *
 * The detector recognises the SHAPE, not per-tap offsets or weights: the
 * orchestrator applies a canonical box kernel and the probe uses the same
 * kernel (the shared probe/orchestrator contract, as for the ZPASS KILL_IF
 * threshold).  Per-byte exactness holds while the unweighted tap sum stays
 * <= 255 (no UNORM8 clamp, no inter-byte carry, no division): a box-3 on input
 * bytes <= 85 sums to <= 255.  Bindings stay 0 when the post-explicit_io
 * binding sources are not constants; the orchestrator's positional fallback
 * recovers them (binding 0 = input, 1 = output).  tap_count carries the leaf
 * count for diagnostics. */
struct r300_compute_multitap_gather_pattern {
   bool       is_multitap_gather;
   uint32_t   input_ssbo_binding;     /* binding of the gathered input */
   uint32_t   output_ssbo_binding;    /* binding of the store dest */
   uint16_t   tap_count;              /* load_ssbo leaves in the add-reduction (>= 3) */
};

void r300_nir_detect_multitap_gather_pattern(const struct nir_shader *s,
                                             struct r300_compute_multitap_gather_pattern *out);

#ifdef __cplusplus
}
#endif

#endif /* R300_COMPUTE_ADMISSION_H */
