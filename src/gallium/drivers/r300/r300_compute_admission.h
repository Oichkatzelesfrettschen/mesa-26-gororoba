/*
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
 * The substrate has texture-LD load, FP24 ALU compute, RB3D export store,
 * blend ADD/MIN/MAX/SUB, stencil and ZPASS reductions, ROP bitwise ops, and
 * per-pixel predicates.  It has no LDS, no workgroup barrier, no general atomic
 * on an arbitrary address, no arbitrary read-write storage, and no FP64.
 *
 * Three storage-class rejects capture distinct surface areas:
 *   RW_STORAGE       -- store_deref into global/pointer memory (not an indexed SSBO)
 *   ARBITRARY_SCATTER -- store_global / store_global_2x32 (raw pointer scatter)
 *   IMAGE_STORE       -- image_store / image_deref_store / bindless_image_store
 *
 * UNKNOWN_SHAPE fires at dispatch time for a kernel that admitted classification
 * but matched no raster-verb pattern; the dispatch is a silent no-op (the output
 * buffer is not written) so the queue is not poisoned. */
enum r300_compute_reject {
   R300_COMPUTE_ADMIT = 0,
   R300_COMPUTE_REJECT_SHARED_MEMORY,     /* no LDS on R3xx */
   R300_COMPUTE_REJECT_BARRIER,           /* fragments are not a synchronized workgroup */
   R300_COMPUTE_REJECT_GENERAL_ATOMIC,    /* only blend-add/min/max/sub, stencil, ZPASS exist */
   R300_COMPUTE_REJECT_RW_STORAGE,        /* store_deref to global pointer; no scatter store */
   R300_COMPUTE_REJECT_FP64,              /* ALU is FP24; no double precision */
   R300_COMPUTE_REJECT_FP16,              /* no native FP16 RT or arithmetic; virtual-FP16 only */
   R300_COMPUTE_REJECT_ARBITRARY_SCATTER, /* store_global / store_global_2x32 */
   R300_COMPUTE_REJECT_IMAGE_STORE,       /* image_store / image_deref_store / bindless_image_store */
   R300_COMPUTE_REJECT_UNKNOWN_SHAPE,     /* admitted but no raster verb matched at dispatch */
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
 * imin / imax / umin / umax / fadd / fsub / fmul / fmin / fmax for the bounded
 * FP24 map; richer arithmetic needs its own carrier and exactness audit.
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

/* Single-input affine unary-map pattern: out[gid] = in[gid] * c0 + c1, one
 * store_ssbo whose value is an affine function of exactly one load_ssbo def with
 * constant scale and bias.  The recognized NIR shapes are the three forms an
 * `in*c0 + c1` GLSL kernel lowers to: nir_op_ffma(load, c0, c1),
 * nir_op_fadd(nir_op_fmul(load, c0), c1), and the degenerate nir_op_fmul(load,
 * c0) (c1 = 0) / nir_op_fadd(load, c1) (c0 = 1).  This is the gap between
 * identity_map (store value IS the load def, no ALU) and binary_map (store value
 * is an ALU of TWO load defs): a one-input elementwise scale-and-bias.  On RS482
 * it lowers to the identity-map substrate with the fragment program multiplying
 * the sampled texel by c0 and adding c1 before the RB3D color export -- one
 * extra FP24 MAD over the identity copy.  c0/c1 run in FP24 and carry to the
 * substrate's float render target; exact for operands inside the FP24 window.
 * Bindings stay 0 when the post-explicit_io sources are not constants (the
 * orchestrator's positional fallback recovers input=0, output=1; an in-place
 * kernel like d[i]=d[i]*c0+c1 binds the same buffer to both). */
struct r300_compute_unary_map_pattern {
   bool       is_unary_map;
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
   bool       input_ssbo_binding_valid;
   bool       output_ssbo_binding_valid;
   float      mul_const;          /* c0 scale; 1.0 for a pure bias */
   float      add_const;          /* c1 bias; 0.0 for a pure scale */
   /* Each affine constant is either a literal (load_const; value in
    * mul_const/add_const above) or a push-constant scalar whose value is only
    * known at command-record time.  For a push-derived constant the detector
    * records the byte offset into the push window instead; the synthesized
    * fragment program reads it from the constant file (the dispatch replay
    * binds the push window at FS CONST[0], so byte offset N maps to
    * CONST[N/16] component (N%16)/4) rather than baking an immediate. */
   bool       mul_const_from_push;
   bool       add_const_from_push;
   uint16_t   mul_const_push_offset; /* byte offset of c0 in the push window */
   uint16_t   add_const_push_offset; /* byte offset of c1 in the push window */
   uint8_t    value_components;   /* store_ssbo value vector width */
   uint8_t    value_bit_size;     /* store_ssbo value component width */
   bool       value_is_float;     /* result base type is float */
};

void r300_nir_detect_unary_map(const struct nir_shader *s,
                               struct r300_compute_unary_map_pattern *out);

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

/* Quantized dot-product (DP4) pattern: out[gid] = dot(in_a[gid], in_b[gid]).
 * One store_ssbo whose value is the def of an nir_op_fdot{2,3,4} of two distinct
 * load_ssbo defs, with no atomic / loop / if.  On RS482 this lowers to the US
 * fragment ALU DP4 instruction.  The dot runs in FP24, so it is BYTE-EXACT when
 * the operands are quantized to <= 7-bit magnitude (4*127^2 = 64516 < 2^17,
 * hardware-confirmed by the surfaceless-EGL dp4 probe) and the ordinary
 * FP24-precise float dot otherwise -- the detector admits the SHAPE; the
 * quantized-exactness contract is on the runtime operand range, not visible at
 * classify time.  components is the dot width (2, 3, or 4) and dot_op carries the
 * nir_op so the orchestrator picks the FS dot form.  Bindings stay 0 when the
 * post-explicit_io binding sources are not constants (the orchestrator's
 * positional fallback recovers them: binding 0 = a, 1 = b, 2 = output). */
struct r300_compute_dp4_pattern {
   bool       is_dp4;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
   uint16_t   dot_op;       /* nir_op_fdot{2,3,4}, only valid if is_dp4 */
   uint8_t    components;   /* dot width: 2, 3, or 4 */
};

void r300_nir_detect_dp4_pattern(const struct nir_shader *s,
                                 struct r300_compute_dp4_pattern *out);

/* Quaternion Hamilton-product (QMUL) pattern: out[gid] = q1[gid] * q2[gid],
 * the Cayley-Dickson dim-4 multiply (QMUL_HAMILTON in r300_virtual_op_catalog).
 * The admissible shape is the canonical four-dot form: one whole-vec4 store_ssbo
 * whose value is nir_vec4(d0,d1,d2,d3), each di an nir_op_fdot4 of one input
 * load_ssbo (q1, used identity-swizzled) against a vec4 sign-permutation of the
 * other load_ssbo (q2).  The four permutations must be exactly the Hamilton
 * matrix rows in (w,x,y,z) layout -- the detector verifies the (channel, sign)
 * of all sixteen permuted operands, so it admits only a true quaternion product,
 * never a structurally-similar kernel the substrate's Hamilton FS would silently
 * recompute differently.  A natural component-wise quat_mul that the compiler
 * lowers to an fmul/fadd tree is NOT this shape; the kernel must be written as
 * the four sign-permuted dots (the form r300vk_build_qmul_fs_nir emits).  The
 * dot runs in FP24 and is exact for operands inside the FP24 window; the result
 * carries to the substrate's FP16 quaternion render target.  Bindings stay 0
 * when the post-explicit_io sources are not constants (the orchestrator's
 * positional fallback recovers q1=0, q2=1, output=2). */
struct r300_compute_qmul_pattern {
   bool       is_qmul;
   uint32_t   input_a_ssbo_binding;   /* q1 */
   uint32_t   input_b_ssbo_binding;   /* q2 */
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qmul_pattern(const struct nir_shader *s,
                                  struct r300_compute_qmul_pattern *out);

/* Quaternion division (QDIV): out = a / b = a * inv(b), inv(b) = conj(b)/|b|^2.
 * Two input quaternions a,b and one output; the divisor b is inverted via the
 * scalar reciprocal r = 1/dot(b,b) (the US RCP) scaling conj(b), then the Hamilton
 * product a*inv(b) (four DP4s).  Bindings stay 0 when the sources are not
 * constants (positional fallback a=0, b=1, output=2). */
struct r300_compute_qdiv_pattern {
   bool       is_qdiv;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qdiv_pattern(const struct nir_shader *s,
                                  struct r300_compute_qdiv_pattern *out);

/* General 4x4 vertex transform (MAT4VEC): out[gid] = M * v[gid], component i =
 * dot(row_i, v).  The matrix M is four contiguous vec4 rows read at constant
 * offsets 0/16/32/48 from one binding (broadcast across all elements); v is the
 * per-element load that appears in all four dots; the store is the vec4 of the
 * four row-dots.  The absent vertex FPU's transform expressed as four DP4s on
 * the fragment ALU. */
struct r300_compute_mat4vec_pattern {
   bool       is_mat4vec;
   uint32_t   matrix_ssbo_binding;
   uint32_t   vertex_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_mat4vec_pattern(const struct nir_shader *s,
                                     struct r300_compute_mat4vec_pattern *out);

/* Quaternion-scalar product (QFMUL) pattern: out[gid] = a[gid] * s, where a is a
 * per-element vec4 quaternion and s is a BROADCAST scalar (a 1-component load at a
 * fixed offset, the same value for every element).  Recognized by the asymmetry:
 * the store value is an fmul whose operands are one 4-component identity-swizzled
 * load (the quaternion) and one 1-component splat-swizzled load (the scalar).
 * Like MAT4VEC's broadcast matrix, the broadcast scalar belongs in the fragment
 * constant file rather than a per-element sampler. */
struct r300_compute_qfmul_pattern {
   bool       is_qfmul;
   uint32_t   scalar_ssbo_binding;
   uint32_t   quat_ssbo_binding;
   uint32_t   output_ssbo_binding;
   bool       scalar_ssbo_binding_valid;
   bool       quat_ssbo_binding_valid;
   bool       output_ssbo_binding_valid;
};

void r300_nir_detect_qfmul_pattern(const struct nir_shader *s,
                                   struct r300_compute_qfmul_pattern *out);

/* Quaternion rotation (QROTATE) pattern: out[gid] = q[gid] * embed(v[gid]) *
 * conj(q[gid]), the sandwich that rotates the vec3 v by the unit quaternion q.
 * The admissible shape is two nested canonical Hamilton products: the store is
 * the outer product of the inner product t = q*embed(v) against conj(q), where
 * embed(v) = (0, vx, vy, vz) and conj(q) = (qw, -qx, -qy, -qz).  The matcher
 * recovers each product's operands and verifies conj(q) and embed(v) against
 * the two load_ssbos, so it admits only a true rotation sandwich -- eight DP4s
 * the substrate's two-Hamilton FS reproduces.  Bindings stay 0 when the sources
 * are not constants (the orchestrator's positional fallback: q=0, v=1, out=2). */
struct r300_compute_qrotate_pattern {
   bool       is_qrotate;
   uint32_t   input_q_ssbo_binding;   /* unit quaternion */
   uint32_t   input_v_ssbo_binding;   /* vector to rotate (vec3 in a vec4) */
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qrotate_pattern(const struct nir_shader *s,
                                     struct r300_compute_qrotate_pattern *out);

/* Quaternion conjugate (QCONJ) pattern: out[gid] = (a.x, -a.y, -a.z, -a.w), the
 * conjugate that negates the three vector lanes of a[gid] and keeps the scalar
 * lane.  Zero DP4 -- a sign flip exact in FP24 -- but carried through the FP16
 * quaternion render target like the other single-lane ops.  Binding stays 0 when
 * the post-explicit_io sources are not constants (the orchestrator's positional
 * fallback: input=0, output=1). */
struct r300_compute_qconj_pattern {
   bool       is_qconj;
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qconj_pattern(const struct nir_shader *s,
                                   struct r300_compute_qconj_pattern *out);

/* Quaternion squared-norm (QNORM) pattern: out[gid] = vec4(dot(a[gid], a[gid])),
 * the squared norm broadcast across four lanes so the substrate's vec4 FP16
 * readback carries it (the kernel reads lane 0).  One DP4 -- a self-dot.  Binding
 * stays 0 when the sources are not constants (positional fallback: input=0,
 * output=1). */
struct r300_compute_qnorm_pattern {
   bool       is_qnorm;
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qnorm_pattern(const struct nir_shader *s,
                                   struct r300_compute_qnorm_pattern *out);

/* Quaternion normalize (QNORMALIZE) pattern: out[gid] = a * rsqrt(dot(a,a)) =
 * a / |a|, the unit quaternion in a's direction.  One DP4 for the squared norm,
 * the US RSQ for the reciprocal length, one vec4 scale.  ROCQ ground:
 * quat_normalize_unit (QuatNormalize.v) proves |normalize(a)|^2 = 1.  Binding
 * stays 0 when the sources are not constants (positional fallback: input=0,
 * output=1). */
struct r300_compute_qnormalize_pattern {
   bool       is_qnormalize;
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qnormalize_pattern(const struct nir_shader *s,
                                        struct r300_compute_qnormalize_pattern *out);

/* Octonion product (OMUL) pattern.  An octonion (a,b) is two quaternions; the
 * Cayley-Dickson product (a,b)*(c,d) = (a*c - conj(d)*b, d*a + b*conj(c)) is four
 * Hamilton products = sixteen DP4s.  The admissible kernel presents the four
 * quaternion halves a,b,c,d as four input SSBOs (read in that order) and the two
 * output halves o_lo, o_hi as two output SSBOs, so the substrate keeps its 1:1
 * element-to-texel raster: the lower store is the folded a*c - conj(d)*b and the
 * upper store the folded d*a + b*conj(c), each verified against the Hamilton rows
 * (and, for the second conjugate, the conj-composed rotation rows).  Bindings
 * stay 0 when the sources are not constants (positional fallback: a=0..d=3,
 * o_lo=4, o_hi=5). */
struct r300_compute_omul_pattern {
   bool       is_omul;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   input_c_ssbo_binding;
   uint32_t   input_d_ssbo_binding;
   uint32_t   output_lo_ssbo_binding;
   uint32_t   output_hi_ssbo_binding;
};

void r300_nir_detect_omul_pattern(const struct nir_shader *s,
                                  struct r300_compute_omul_pattern *out);

/* Octonion componentwise add/sub (OADD/OSUB): out = (a,b) (+|-) (c,d), the lower
 * half a (+|-) c and the upper b (+|-) d combined by the same op.  Four input
 * SSBOs a,b,c,d (read in declaration order) and two output halves; is_sub picks
 * the operator.  Bindings stay 0 when the sources are not constants (positional
 * fallback a=0..d=3, o_lo=4, o_hi=5). */
struct r300_compute_oaddsub_pattern {
   bool       is_oaddsub;
   bool       is_sub;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   input_c_ssbo_binding;
   uint32_t   input_d_ssbo_binding;
   uint32_t   output_lo_ssbo_binding;
   uint32_t   output_hi_ssbo_binding;
};

void r300_nir_detect_oaddsub_pattern(const struct nir_shader *s,
                                     struct r300_compute_oaddsub_pattern *out);

/* Octonion conjugate (OCONJ): conj((a,b)) = (conj(a), -b) -- the lower half is
 * the quaternion conjugate of a (scalar lane kept, vector lanes negated) and the
 * upper half is the full negation of b.  Two input SSBOs a,b and two output
 * halves.  Bindings stay 0 when not constant (positional: a=0, b=1, o_lo=2,
 * o_hi=3). */
struct r300_compute_oconj_pattern {
   bool       is_oconj;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_lo_ssbo_binding;
   uint32_t   output_hi_ssbo_binding;
};

void r300_nir_detect_oconj_pattern(const struct nir_shader *s,
                                   struct r300_compute_oconj_pattern *out);

/* Octonion squared norm (ONORM): |(a,b)|^2 = dot(a,a) + dot(b,b), broadcast to
 * four lanes (the kernel reads lane 0).  Two input SSBOs a,b and one output.  Two
 * DP4s.  Bindings stay 0 when not constant (positional: a=0, b=1, out=2). */
struct r300_compute_onorm_pattern {
   bool       is_onorm;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_onorm_pattern(const struct nir_shader *s,
                                   struct r300_compute_onorm_pattern *out);

/* Octonion division (ODIV): out = x * inv(y) (right, x/y) OR inv(y) * x (left,
 * y\x), with inv(y) = conj(y)/|y|^2, valid for every nonzero octonion (dim 8 is a
 * division algebra; |y|^2 = dot(ylo,ylo)+dot(yhi,yhi) > 0 unless y = 0).  Right
 * and left differ because octonions are non-commutative AND non-associative, but
 * each is parenthesis-safe (Artin: x and y generate an associative subalgebra
 * containing inv(y)).  The kernel reads x = (xlo,xhi) and y = (ylo,yhi) and writes
 * the two product halves; the scalar reciprocal r = 1/|y|^2 scales conj(y) into
 * inv(y), then the eight-wide product is the OMUL fold -- x*inv(y) (is_left false)
 * or inv(y)*x (is_left true).  Four input SSBOs (xlo,xhi,ylo,yhi), two output
 * halves; bindings stay 0 when the sources are not constants (positional fallback
 * xlo=0..yhi=3, o_lo=4, o_hi=5).  Division stays a dim-8-only op: at dim 16 conj/N
 * is only a pseudo-inverse (sedenion zero divisors). */
struct r300_compute_odiv_pattern {
   bool       is_odiv;
   bool       is_left;   /* false: right division x*inv(y); true: left inv(y)*x */
   uint32_t   input_xlo_ssbo_binding;
   uint32_t   input_xhi_ssbo_binding;
   uint32_t   input_ylo_ssbo_binding;
   uint32_t   input_yhi_ssbo_binding;
   uint32_t   output_lo_ssbo_binding;
   uint32_t   output_hi_ssbo_binding;
};

void r300_nir_detect_odiv_pattern(const struct nir_shader *s,
                                  struct r300_compute_odiv_pattern *out);

/* Octonion sandwich transform (OTRANS): out = x * v * conj(x), the octonion analog
 * of the quaternion rotation sandwich.  Two chained octonion products -- t = x*v,
 * then out = t*conj(x) -- so 32 DP4s materializing the intermediate octonion t.
 * Parenthesis-safe by the flexible law (conj(x) in R[x]).  The kernel reads
 * x = (xlo,xhi) and v = (vlo,vhi) and writes the two halves; the detector finds
 * the two OMUL(x,v) halves (tl,th), then verifies the stores are OMUL((tl,th),
 * conj(x)) where conj(x) = (conj(xlo), -xhi) folds into the permutation rows.  The
 * dispatch runs t = x*v to a scratch buffer, then out = t*conj(x).  Four input
 * SSBOs (xlo,xhi,vlo,vhi), two output halves; bindings stay 0 when the sources are
 * not constants (positional fallback xlo=0..vhi=3, o_lo=4, o_hi=5). */
struct r300_compute_otrans_pattern {
   bool       is_otrans;
   uint32_t   input_xlo_ssbo_binding;
   uint32_t   input_xhi_ssbo_binding;
   uint32_t   input_vlo_ssbo_binding;
   uint32_t   input_vhi_ssbo_binding;
   uint32_t   output_lo_ssbo_binding;
   uint32_t   output_hi_ssbo_binding;
};

void r300_nir_detect_otrans_pattern(const struct nir_shader *s,
                                    struct r300_compute_otrans_pattern *out);

/* Quaternion fused multiply-add (QFMADD): out = a*b + c, the Hamilton product of
 * a and b plus the quaternion c.  Four DP4s plus a vec4 add.  Three input SSBOs
 * (a,b,c in declaration order) and one output; bindings stay 0 when the sources
 * are not constants (positional fallback: a=0, b=1, c=2, out=3). */
struct r300_compute_qfmadd_pattern {
   bool       is_qfmadd;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   input_c_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qfmadd_pattern(const struct nir_shader *s,
                                    struct r300_compute_qfmadd_pattern *out);

/* Quaternion fused triple product (QFMMUL): out = a*b*c = (a*b)*c, two chained
 * Hamilton products (eight DP4s) in one pass -- associativity in H makes the
 * parenthesization irrelevant.  Three input SSBOs (a,b,c) and one output. */
struct r300_compute_qfmmul_pattern {
   bool       is_qfmmul;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   input_c_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_qfmmul_pattern(const struct nir_shader *s,
                                    struct r300_compute_qfmmul_pattern *out);

/* Constant-fill (CONSTFILL) pattern: out[gid] = C for every element, where C is a
 * compile-time constant with zero loads.  The degenerate SSBO-store is the limiting
 * case of the identity-map where the stored value is independent of gid and memory --
 * a framebuffer CLEAR, not a fragment program.  On RS482 this lowers to an RB3D
 * color-buffer clear using the constant bytes as the RGBA8 clear color, followed by
 * the standard RT-to-buffer copy that the identity-map readback path already provides.
 * No per-element fragment draw is needed: the clear fills the entire render target with
 * the same four bytes per texel, so every element of the output SSBO receives C.
 *
 * Theorem: "degenerate store is a clear" -- out[gid] = C for all gid.
 *
 * Discriminator from every other admitted shape:
 *   load_count == 0 (identity-map, unary-map, binary-map all require >= 1 load_ssbo).
 *   The stored value's producing instruction type is nir_instr_type_load_const.
 *   No atomics (rules out blend-acc and ZPASS), no loop (rules out multipass),
 *   no if (rules out predicated-store).
 *
 * const_value[0..3] holds the four bytes of the RGBA8 clear color in little-endian
 * component order (R=byte 0, G=byte 1, B=byte 2, A=byte 3).  For a uint32_t value V
 * the bytes are the four uint8_t components of V as written by the shader.  For a
 * vec4 float constant the bytes come from the RB3D RGBA8 encoding the substrate uses
 * for the RT carrier; the caller converts from the NIR constant representation.
 *
 * output_ssbo_binding is 0 when the post-explicit_io store_ssbo binding source is not
 * a constant (the orchestrator's positional fallback recovers it: binding 0 = output).
 * value_components carries the stored vector width (1 for uint32_t fill, 4 for vec4).
 * value_bit_size is the per-component width in bits. */
struct r300_compute_const_fill_pattern {
   bool       is_const_fill;
   uint32_t   output_ssbo_binding;
   bool       output_ssbo_binding_valid;
   uint8_t    const_value[4];    /* RGBA8 bytes of the constant, R=byte 0 */
   uint8_t    value_components;
   uint8_t    value_bit_size;
};

void r300_nir_detect_const_fill_pattern(const struct nir_shader *s,
                                        struct r300_compute_const_fill_pattern *out);

/* Virtual IEEE FP16 classification pattern (IEEE16_CLASSIFY_LUT):
 * out[gid] = classify(in[gid]). Samples 1-component raw bits, writes vec4 color carrier. */
struct r300_compute_ieee16_classify_pattern {
   bool       is_ieee16_classify;
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_ieee16_classify(const struct nir_shader *s,
                                     struct r300_compute_ieee16_classify_pattern *out);

/* Virtual IEEE FP16 significand multiplication (IEEE16_MUL_RNE):
 * out[gid] = normal_mul(ua[gid], ub[gid]). Samples 2-component significands, writes 2-limb carry. */
struct r300_compute_ieee16_mul_pattern {
   bool       is_ieee16_mul;
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_ieee16_mul(const struct nir_shader *s,
                                struct r300_compute_ieee16_mul_pattern *out);

/* How the kernel consumes the work-item invocation index.  The FP24 fragment
 * ALU represents every integer only up to 2^17, so the index-exactness bound
 * a dispatch must satisfy depends on whether the index is ever materialized
 * as an FP24 VALUE, not on whether the kernel reads the index at all:
 *
 *   NONE          -- no invocation-index intrinsic; output addressed purely by
 *                    texel position (CONSTFILL shape).
 *   ADDRESS_ONLY  -- index defs feed only load_ssbo / store_ssbo offset
 *                    chains.  The raster replay carries addressing as texel
 *                    position (each axis <= 2048, exact), so the full
 *                    2048x2048 fold is honest.
 *   VALUE_AFFINE  -- an index def reaches a store_ssbo VALUE source through
 *                    an affine chain a * gid + b with constant a, b.  The
 *                    replay must materialize a * gid + b in FP24; the
 *                    dispatch-time guard bounds a * (total - 1) + b by 2^17
 *                    (r300_grid_strided_index_exact).
 *   VALUE_GENERAL -- an index def reaches a stored value through a non-affine
 *                    chain (index-squared, control flow, an unrecognized
 *                    intrinsic).  No exactness bound is derivable; the
 *                    dispatch replay must not claim index identity.
 */
enum r300_compute_index_consumption {
   R300_COMPUTE_INDEX_NONE = 0,
   R300_COMPUTE_INDEX_ADDRESS_ONLY,
   R300_COMPUTE_INDEX_VALUE_AFFINE,
   R300_COMPUTE_INDEX_VALUE_GENERAL,
};

/* Result of classifying invocation-index consumption.  stride / offset carry
 * the affine coefficients (value = stride * gid + offset) and are valid only
 * for VALUE_AFFINE.  uses_component_y / uses_component_z report whether any
 * vec3 index channel beyond .x is read, which selects the COORD fold bound
 * (per-axis) over the linear-total bound at dispatch time. */
struct r300_compute_index_pattern {
   enum r300_compute_index_consumption consumption;
   bool       stride_valid;
   uint32_t   stride;
   uint32_t   offset;
   bool       uses_component_y;
   bool       uses_component_z;
};

/* Classify how a compute kernel consumes its invocation index.  Pure
 * read-only analysis: walks the use chains of every invocation-index
 * intrinsic (global/local invocation id and index, workgroup id) and reports
 * whether any chain escapes the addressing path into a stored value.
 * Conservative: an unrecognized consumer or a use-graph larger than the
 * internal traversal bound classifies as VALUE_GENERAL, never as a weaker
 * class. */
void r300_nir_classify_index_consumption(const struct nir_shader *s,
                                         struct r300_compute_index_pattern *out);

#ifdef __cplusplus
}
#endif

#endif /* R300_COMPUTE_ADMISSION_H */
