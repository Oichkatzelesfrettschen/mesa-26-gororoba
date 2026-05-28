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
 * Each reason names a hardware constraint from the substrate finding
 * (2026-05-27-rs482-compute-substrate-expanded-and-numeric-envelope): the
 * substrate has texture-LD load, FP24 ALU compute, RB3D export store, the
 * blend ADD/MIN/MAX/SUB + stencil + ZPASS reduction forms, ROP bitwise, and
 * per-pixel predicates -- but no LDS, no workgroup barrier, no general atomic
 * on an arbitrary address, no arbitrary read-write storage, and no FP64. */
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
 * by the FP24-budget table in
 * src/re/r300/docs/rs482-r300vk-compute-texture-pair-binary-map-derivation.md
 * (iadd / isub / imul / imin / imax / umin / umax / fadd / fsub / fmul /
 * fmin / fmax for the first cut; richer arithmetic is M-G territory).
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
};

void r300_nir_detect_binary_map(const struct nir_shader *s,
                                struct r300_compute_binary_map_pattern *out);

/* M-G blend-add-reduction kernel pattern (Conjecture M-G Entry 4): a kernel
 * whose store value is an atomicAdd of a load_ssbo result, where the atomic's
 * target buffer is a small output histogram and the atomic's offset folds the
 * dispatch grid into a smaller bin range -- the canonical shape:
 *
 *     uint gid = gl_GlobalInvocationID.x;
 *     uint bin = gid & BIN_MASK;
 *     atomicAdd(out_data[bin], in_data[gid]);
 *
 * On RS482 this lowers to a blend-add accumulation: the output buffer binds
 * as a 1xM RT, the blend equation is `RB3D_CBLEND.COMB_FCN_ADD` with
 * `blend_func = (ONE, ONE)`, the orchestrator draws one fragment per gid at
 * position (bin, 0), and the RB3D blend hardware accumulates the per-gid
 * value into the bin cell.  The mechanism is hardware-confirmed at the
 * substrate verb level (bundle blendacc_20260527T045725Z); M-G.1 lifts the
 * pattern from kernel NIR to driver detection.
 *
 * value_ssbo_binding is the binding of the load_ssbo feeding the atomic
 * value; output_ssbo_binding is the binding of the atomic's target buffer
 * (the histogram).  When the post-explicit_io binding sources are not
 * constants, both stay 0 and the orchestrator's descriptor-set layout
 * fallback recovers them (same policy as binary-map M-F.3).  alu_op holds
 * nir_op_iadd for the first cut; fadd will land alongside in a future
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

/* M-G Entry 5 ZPASS coverage-count reduction kernel pattern.  Recognises
 * the predicate-gated counter shape:
 *
 *     uint gid = gl_GlobalInvocationID.x;
 *     if (in_data[gid] >= THRESHOLD)
 *         atomicAdd(count_out, 1u);
 *
 * On RS482 this lowers to the substrate's ZPASS coverage-count verb:
 * the orchestrator binds a 1xN RT, draws N point primitives at
 * (gid_norm_x, 0), each fragment KILL-discards when the per-vertex-baked
 * predicate is false; the depth/stencil unit's `ZB_ZPASS_DATA` /
 * `ZB_ZPASS_ADDR` counter pair (per umr-gororoba RS482 register decode
 * mmR300_ZB_ZPASS_DATA = DWORD 0x13d6 / byte 0x4f58 and
 * mmR300_ZB_ZPASS_ADDR = DWORD 0x13d7 / byte 0x4f5c) accumulates the
 * per-pipe surviving-fragment count.  Mesa's existing
 * src/gallium/drivers/r300/r300_query.c r300_create_query +
 * begin_query + end_query + get_query_result chain (lines 15-188)
 * wraps this register pair as PIPE_QUERY_OCCLUSION_COUNTER returning
 * a u64 fragment sum; the orchestrator writes that sum to count_out[0].
 *
 * The mechanism is hardware-confirmed at the substrate verb level by
 * bundle stencil_zpass_20260527T052208Z (zpass_samples=1682, per
 * 2026-05-26-rs482-compute-as-raster-functional-unit-substrate.md).
 *
 * Discriminator from M-G Entry 4 (blend-acc reduction):
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
 * are not constants (same convention as M-F.3 / M-G.3 fallback). */
struct r300_compute_zpass_reduction_pattern {
   bool       is_zpass_reduction;
   uint32_t   value_ssbo_binding;     /* binding of the predicate-source load_ssbo */
   uint32_t   output_ssbo_binding;    /* binding of the single-element counter */
   uint16_t   alu_op;                 /* nir_op_iadd for the first cut */
};

void r300_nir_detect_zpass_reduction(const struct nir_shader *s,
                                     struct r300_compute_zpass_reduction_pattern *out);

/* M-G Entry 6 multipass FBO ping-pong scan kernel pattern.  Recognises the
 * per-element self-iterated shape:
 *
 *     uint x = in_data[gid];
 *     for (uint k = 0u; k < pass_count; k++) x = x * 2u;
 *     out_data[gid] = x;
 *
 * where pass_count is a runtime value loaded from a params storage buffer
 * (binding 2) so the loop does NOT constant-fold.  On RS482 this lowers to
 * the substrate's multipass FBO ping-pong verb (substrate finding
 * 2026-05-26-rs482-compute-as-raster-functional-unit-substrate.md, frontier
 * ping_pong_fbo_iter4): the orchestrator runs pass_count dependent fragment
 * passes binding the prior pass's render target as the next pass's sampler,
 * each pass applying the per-iteration step (the doubling) to the texel.
 * The mechanism is hardware-confirmed at the EGL/GBM render-ladder level by
 * bundle glamor_compute_surface_20260522T023537Z.
 *
 * Design + admit-path linchpin in
 * src/re/r300/findings/active/2026-05-28-rs482-multipass-pingpong-scan-design.md:
 * the kernel NIR is classified-then-discarded (ralloc_free in
 * r300vk_classify_compute_kernel), never compiled to the R300 fragment
 * program, so the runtime loop is a detection-time signal only -- which is
 * what keeps Entry 6 independent of the M-K dispatch-barrier contract.
 *
 * Discriminator from every prior entry: the presence of a nir_loop.  M-E
 * identity-map, M-F binary-map, M-G.4 blend-acc, and M-G.5 ZPASS are all
 * loop-free; M-G.6 is the only shape carrying a loop whose body is a
 * self-only arithmetic step (the loop-carried value is not a cross-element
 * gather, which would need a workgroup barrier the substrate lacks).
 *
 * step_op holds the per-iteration nir_op (nir_op_imul for the doubling
 * first cut).  Bindings stay 0 when the post-explicit_io binding sources
 * are not constants (same positional-fallback convention as M-F.3 / M-G.3:
 * binding 0 = input, 1 = output, 2 = params). */
struct r300_compute_multipass_scan_pattern {
   bool       is_multipass_scan;
   uint32_t   input_ssbo_binding;     /* binding of the per-element data load */
   uint32_t   output_ssbo_binding;    /* binding of the store dest */
   uint16_t   step_op;                /* per-iteration nir_op (imul for doubling) */
};

void r300_nir_detect_multipass_scan_pattern(const struct nir_shader *s,
                                            struct r300_compute_multipass_scan_pattern *out);

#ifdef __cplusplus
}
#endif

#endif /* R300_COMPUTE_ADMISSION_H */
