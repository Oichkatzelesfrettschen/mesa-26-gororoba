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

#ifdef __cplusplus
}
#endif

#endif /* R300_COMPUTE_ADMISSION_H */
