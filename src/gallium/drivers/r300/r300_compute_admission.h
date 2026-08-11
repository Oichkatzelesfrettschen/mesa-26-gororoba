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
 *   RW_STORAGE       -- store_ssbo with a non-canonical address or store_deref global
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
   R300_COMPUTE_REJECT_RW_STORAGE,        /* unsupported store_ssbo address or global deref */
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

/* Binding provenance states used by detector/replay handoff.  A complete
 * capture includes one validity bit for every role; an all-unknown capture is
 * eligible for a descriptor-layout fallback, while a partial capture cannot
 * identify the remaining roles safely. */
enum r300_compute_binding_capture_state {
   R300_COMPUTE_BINDINGS_ALL_UNKNOWN,
   R300_COMPUTE_BINDINGS_COMPLETE,
   R300_COMPUTE_BINDINGS_PARTIAL,
};

enum r300_compute_binding_capture_state
r300_compute_binding_capture_classify(unsigned valid_mask, unsigned role_count);

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
   bool       input_ssbo_binding_valid;
   bool       output_ssbo_binding_valid;
   uint8_t    value_components;     /* stored value vector width */
   uint8_t    value_bit_size;       /* stored value component width */
   bool       value_is_float;       /* store_ssbo src_type base == nir_type_float */
};

/* Detect the identity-map pattern in a classify-admitted kernel.  Pure
 * read-only analysis.  Sets out->is_identity_map = true when exactly one
 * store_ssbo's value is the result of exactly one load_ssbo.  Constant
 * bindings are recorded here with per-role validity bits; binding zero is
 * therefore distinct from an opaque source.  Descriptor-lowered Vulkan
 * kernels carry opaque handles that dispatch resolves through the
 * descriptor-set layout. */
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
 * picks the right PFS instruction.  Binding values have a separate validity
 * flag because binding 0 is a valid Vulkan binding; a false flag means the
 * post-explicit_io source was opaque and dispatch must resolve the role from
 * its descriptor-set contract. */
struct r300_compute_binary_map_pattern {
   bool       is_binary_map;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
   bool       input_a_ssbo_binding_valid;
   bool       input_b_ssbo_binding_valid;
   bool       output_ssbo_binding_valid;
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

/* Admissible storage-image RT-export pattern.  Exactly one class of
 * image_deref_store has fragment-shader-writes-a-color-target semantics and can
 * lower to the RB3D color export: one image_deref_store into a single 2D,
 * non-array, non-multisample R8G8B8A8_UNORM storage image (four 32-bit
 * components, full write mask, LOD 0), at a 2-component coordinate that
 * depends on gl_GlobalInvocationID (and only constants plus that id -- no
 * descriptor loads), with no image load, no atomic, and no competing storage
 * write.  is_rt_exportable is false for every shape outside that subset, which
 * stays rejected under R300_COMPUTE_REJECT_IMAGE_STORE.  image_binding holds
 * the image variable's descriptor binding when is_rt_exportable is true.
 * Pure read-only NIR analysis; gates the lowering, never mutates. */
struct r300_image_store_rt_export_pattern {
   bool       is_rt_exportable;
   uint32_t   image_binding;         /* storage-image descriptor binding */
   bool       image_binding_valid;   /* true when is_rt_exportable */
};

void r300_nir_detect_image_store_rt_export(
   const struct nir_shader *s,
   struct r300_image_store_rt_export_pattern *out);

/* Single-input transcendental map: out[gid] = f(in[gid]) where f is one native
 * r300 US scalar transcendental -- the store value is a single-source ALU op of
 * exactly one load_ssbo def, and the op is one nir_to_rc.c lowers to an r300 ALU
 * verb: frcp(RCP), frsq(RSQ), fsqrt, fexp2(EX2), flog2(LG2), fsin(SIN),
 * fcos(COS), ffract(FRC), ffloor, fround_even(ROUND).  This is the gap above
 * unary_map (affine a*x+b via MAD): a non-affine single-input map the fragment
 * ALU computes natively but no verb exposed.  Distinct from unary_map by the op
 * set (a transcendental, never fmul/fadd/ffma) so the two detectors never both
 * fire.  PRECISION TIER: the result is FP24-approximate (~15-16 bit ALU) and is
 * carried through an FP16 render target (~10-bit mantissa), so the realized
 * output is FP16-bounded -- never bit-exact.  rsq/rcp/sqrt are Newton-refinable;
 * sin/cos/exp2/log2 are sampled/bounded against a host oracle. */
struct r300_compute_unary_transcendental_pattern {
   bool       is_unary_transcendental;
   uint16_t   alu_op;             /* nir_op of the transcendental */
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
   bool       input_ssbo_binding_valid;
   bool       output_ssbo_binding_valid;
   uint8_t    value_components;   /* store_ssbo value vector width (1 supported) */
   uint8_t    value_bit_size;     /* store_ssbo value component width (32) */
};

void r300_nir_detect_unary_transcendental(
   const struct nir_shader *s,
   struct r300_compute_unary_transcendental_pattern *out);

/* True when op is one of the single-input transcendentals the unary-transcendental
 * verb covers -- shared by the detector and the r3v FS synthesis so both agree
 * on the admitted op set. */
bool r300_nir_is_unary_transcendental_op(uint16_t nir_op);

/* Two-input transcendental map: out[gid] = f(a[gid], b[gid]) where f is fpow or
 * fdiv -- the gap above binary_map (commutative fadd/fmul/fmin/fmax) for the two
 * non-commutative transcendental binaries the fragment ALU computes natively:
 * pow as EX2(LG2(a)*b), and a/b as a*RCP(b).  Operand order is preserved
 * (input_a feeds the op's first source, input_b the second), since neither op is
 * commutative.  The op set keeps it disjoint from binary_map; a unit-numerator
 * fdiv(1.0, x) is the unary reciprocal arm, not this verb (its numerator is a
 * load, not a constant).  Vec4 componentwise only -- the proven R32G32B32A32 ->
 * FP16 RT carrier the binary_map float path uses.  PRECISION TIER: FP24-ALU +
 * FP16-carrier bounded, never bit-exact. */
struct r300_compute_binary_transcendental_pattern {
   bool       is_binary_transcendental;
   uint16_t   alu_op;                 /* nir_op: fpow or fdiv */
   uint32_t   input_a_ssbo_binding;   /* feeds the op's first source */
   uint32_t   input_b_ssbo_binding;   /* feeds the op's second source */
   uint32_t   output_ssbo_binding;
   uint8_t    value_components;       /* store_ssbo value vector width (4 supported) */
   uint8_t    value_bit_size;         /* store_ssbo value component width (32) */
};

void r300_nir_detect_binary_transcendental(
   const struct nir_shader *s,
   struct r300_compute_binary_transcendental_pattern *out);

/* Two-input bitwise map: out[gid] = a[gid] OP b[gid] for OP in {iand, ior, ixor}
 * -- the bitwise binaries the FP24 fragment ALU genuinely cannot compute, lowered
 * instead onto the RB3D ROP output stage (R300_RB3D_ROPCNTL logic op).  Each
 * uint32 element is packed as RGBA8 and the logic op combines source against
 * destination per bit, so byte boundaries are irrelevant and the result is
 * bit-exact -- no FP, no precision tier.  AND/OR/XOR commute, so operand order is
 * not tracked.  Disjoint from binary_map (iadd/imul/imin/...) and binary_
 * transcendental (fpow/fdiv) by op set.  Logical shifts ride a separate byte
 * carrier (see r300_compute_shift_logical_pattern); only signed ishr and
 * variable shift amounts stay genuinely beyond the substrate. */
struct r300_compute_bitwise_logicop_pattern {
   bool       is_bitwise_logicop;
   uint16_t   alu_op;                 /* nir_op: iand, ior, or ixor */
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
   uint8_t    value_components;       /* scalar uint32 (1) */
   uint8_t    value_bit_size;         /* 32 */
};

void r300_nir_detect_bitwise_logicop(
   const struct nir_shader *s,
   struct r300_compute_bitwise_logicop_pattern *out);

/* Constant-amount integer shift: out[gid] = a[gid] << k (ishl), >> k unsigned
 * (ushr), or >> k signed (ishr), for a constant k in [1, 31].  The FP24 ALU has
 * no shift, but a shift is a byte re-pack: each uint32 packs as RGBA8, and a
 * single fragment pass recombines the bytes -- out_byte[j] gathers the low 8-r
 * bits of one source byte and the high r bits of its neighbour, where k = 8*q+r,
 * q = byte distance, r = within-byte bit distance.  Byte values are 0..255 and
 * byte*2^r < 2^17, so every intermediate is an exact FP24 integer and the result
 * is bit-exact (the same UNORM8 round-trip the identity-map verb relies on).
 * Arithmetic ishr adds the sign-extension fill ushr left at zero -- those bits
 * are disjoint from the logical result, so it is an exact add.  The struct name
 * keeps the original logical-shift scope; is_arithmetic distinguishes ishr.
 * SCOPE: constant shift amounts only.  Variable shift amounts (needing an exact
 * per-element 2^b) stay UNKNOWN_SHAPE rather than produce a wrong result. */
struct r300_compute_shift_logical_pattern {
   bool       is_shift_logical;
   bool       is_left;                /* ishl (true) vs right shift (false) */
   bool       is_arithmetic;          /* ishr (sign-extend) vs ushr (zero-fill) */
   uint8_t    shift_amount;           /* k in [1, 31] */
   uint32_t   input_ssbo_binding;
   uint32_t   output_ssbo_binding;
   uint8_t    value_components;       /* scalar uint32 (1) */
   uint8_t    value_bit_size;         /* 32 */
};

void r300_nir_detect_shift_logical(
   const struct nir_shader *s,
   struct r300_compute_shift_logical_pattern *out);

/* Variable-amount shift: out[gid] = a[gid] << b[gid] (ishl), >> b[gid] unsigned
 * (ushr), or >> b[gid] signed (ishr), where the shift amount b is a per-element
 * load, not a constant -- the shape the constant-shift detector cannot serve.
 * The exact realisation is a per-element power-of-two multiply: a << b = low 32
 * bits of a * 2^b, and a >> b = bits [31,62] of a * 2^(31-b).  Both 2^M (M = b or
 * 31-b) are <= 2^31, so they fit a uint32 and there is no 2^32 corner; the
 * dispatch looks up 2^M per element from a 2^j texture, then runs the
 * multilimb-multiply convolution and reads back the windowed 32 bits.  Signed
 * ishr adds the sign extension: ishr = ushr + sign(a) * (0xFFFFFFFF << (32-b)),
 * disjoint from the logical bits so an exact per-byte add, with the fill looked
 * up per element from a second texture.  Bit-exact for every amount in [0,31]. */
struct r300_compute_shift_variable_pattern {
   bool       is_shift_variable;
   bool       is_left;                /* ishl (true) vs ushr/ishr (false) */
   bool       is_arithmetic;          /* ishr: sign-extend the right shift */
   uint32_t   input_a_ssbo_binding;   /* the value being shifted */
   uint32_t   input_b_ssbo_binding;   /* the per-element shift amount */
   uint32_t   output_ssbo_binding;
   uint8_t    value_components;       /* scalar uint32 (1) */
   uint8_t    value_bit_size;         /* 32 */
};

void r300_nir_detect_shift_variable(
   const struct nir_shader *s,
   struct r300_compute_shift_variable_pattern *out);

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
 * (the histogram).  Each binding has a validity flag because binding 0 is a
 * valid Vulkan binding.  When both flags are false, dispatch uses the
 * descriptor-set positional contract; a partial capture is rejected.  alu_op
 * holds nir_op_iadd for the first cut; fadd will land alongside in a future
 * extension when the FP24 envelope analysis confirms the per-bin sum stays
 * exact. */
struct r300_compute_blend_acc_reduction_pattern {
   bool       is_blend_acc_reduction;
   uint32_t   value_ssbo_binding;
   uint32_t   output_ssbo_binding;
   bool       value_ssbo_binding_valid;
   bool       output_ssbo_binding_valid;
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
 * buffer.  Each binding has a validity flag because binding 0 is a valid
 * Vulkan binding.  When both flags are false, dispatch uses the
 * descriptor-set positional contract; a partial capture is rejected. */
struct r300_compute_zpass_reduction_pattern {
   bool       is_zpass_reduction;
   uint32_t   value_ssbo_binding;     /* binding of the predicate-source load_ssbo */
   uint32_t   output_ssbo_binding;    /* binding of the single-element counter */
   bool       value_ssbo_binding_valid;
   bool       output_ssbo_binding_valid;
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
 * r3v_classify_compute_kernel), never compiled to the R300 fragment
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
 * baseline. The per-pixel-predicate verb realizes the masked store.
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

/* Quantized dot-product (DP4) pattern:
 * out_uint[gid] = f2u32(dot(in_a[gid], in_b[gid])).  One store_ssbo whose
 * value is f2u32(nir_op_fdot{2,3,4}) of two distinct load_ssbo defs, with no
 * atomic / loop / if.  The detector also proves stride-normalized offset
 * equivalence: fdot2 inputs advance by 8 bytes, fdot3/fdot4 inputs advance by
 * the 16-byte FP32x4 replay carrier, and the uint output advances by 4 bytes,
 * all on the same zero-biased invocation stream.  On RS482 this lowers to the
 * US fragment ALU DP4 instruction.  The dot runs in FP24, so it is byte-exact
 * when the operands are quantized to <= 7-bit magnitude (4*127^2 = 64516 <
 * 2^17, hardware-confirmed by the surfaceless-EGL dp4 probe) and the ordinary
 * FP24-precise float dot otherwise.  components is the dot width (2, 3, or 4)
 * and dot_op carries the nir_op so the orchestrator picks the FS dot form.
 * Bindings stay 0 when the post-explicit_io binding sources are not constants
 * (the orchestrator's positional fallback recovers them: binding 0 = a,
 * 1 = b, 2 = output). */
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
 * the four sign-permuted dots (the form r3v_build_qmul_fs_nir emits).  The
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
   bool       is_sub;   /* true when the final vec4 combine is fsub (QFMSUB) */
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

/* Q16.16 fixed-point addition (Q16_16_ADD): out = a + b where a, b, out are
 * u32 values encoding Q16.16.  The canonical 2-limb carry shape:
 *   lo_sum  = iadd(iand(a, 0xFFFF), iand(b, 0xFFFF))
 *   carry   = ushr(lo_sum, 16)
 *   hi_sum  = iadd(iadd(ushr(a, 16), ushr(b, 16)), carry)
 *   out     = ior(ishl(hi_sum, 16), iand(lo_sum, 0xFFFF))
 * The max sum is (2^16-1)+(2^16-1)+1 = 2^17-1 < 2^17, so every intermediate
 * stays within the FP24 exact-integer window. */
struct r300_compute_q16_16_add_pattern {
   bool       is_q16_16_add;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_q16_16_add_pattern(const struct nir_shader *s,
                                        struct r300_compute_q16_16_add_pattern *out);

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
 * const_value[0..3] holds the four bytes of the scalar uint32_t clear value in
 * little-endian component order (R=byte 0, G=byte 1, B=byte 2, A=byte 3).
 * Vector and sub-32-bit fills are rejected until a replay path can represent
 * every stored lane and byte stride explicitly.
 *
 * output_ssbo_binding is 0 when the post-explicit_io store_ssbo binding source is not
 * a constant (the orchestrator's positional fallback recovers it: binding 0 = output).
 * value_components is 1 and value_bit_size is 32 for admitted fills. */
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
   /* Per-component affine: ax * id.x + ay * id.y + az * id.z + b with at
    * least one of ay, az nonzero -- the kernel-side 3D flatten.  The
    * dispatch gate bounds ax(tx-1) + ay(ty-1) + az(tz-1) + b by 2^17, and
    * the replay accepts only the canonical flatten (ay == ax * dim_x,
    * az == ax * dim_x * dim_y), under which that bound equals the 1D
    * strided bound on the flattened total exactly. */
   R300_COMPUTE_INDEX_VALUE_AFFINE_3D,
   R300_COMPUTE_INDEX_VALUE_GENERAL,
};

/* Result of classifying invocation-index consumption.  stride / offset carry
 * the affine coefficients (value = stride * gid + offset) and are valid only
 * for VALUE_AFFINE.  uses_component_y / uses_component_z report whether any
 * vec3 index channel beyond .x is read, which selects the COORD fold bound
 * (per-axis) over the linear-total bound at dispatch time.
 * affine_global_invocation_only reports whether the materialized value comes
 * only from global invocation ID/index plus zero dispatch-base intrinsics.
 * AFFINE_IOTA needs that stronger source identity because its replay cannot
 * materialize local-invocation or workgroup IDs from the flat raster index.
 *
 * store_offset_* reports a common store_ssbo byte-offset affine when every
 * tracked store offset has one.  AFFINE_IOTA uses it to prove the destination
 * is out[gid] instead of a constant or unrelated output slot. */
struct r300_compute_index_pattern {
   enum r300_compute_index_consumption consumption;
   bool       stride_valid;
   uint32_t   stride;     /* ax: the id.x coefficient */
   uint32_t   stride_y;   /* ay: the id.y coefficient (VALUE_AFFINE_3D) */
   uint32_t   stride_z;   /* az: the id.z coefficient (VALUE_AFFINE_3D) */
   uint32_t   offset;
   bool       uses_component_y;
   bool       uses_component_z;
   bool       affine_global_invocation_only;
   bool       store_offset_valid;
   bool       store_offset_global_invocation_only;
   uint32_t   store_offset_stride;
   uint32_t   store_offset_stride_y;
   uint32_t   store_offset_stride_z;
   uint32_t   store_offset_offset;
};

/* Affine-iota pattern: out[gid] = stride * gid + offset, a pure function of
 * the invocation index -- one store_ssbo, ZERO loads, the stored value an
 * affine chain of the index with constant coefficients.  The first verb that
 * materializes the work-item index as an FP24 VALUE: the replay carries the
 * index as a texel-unit vertex varying, evaluates stride * gid + offset in
 * the fragment ALU, and byte-decomposes the result into an RGBA8 carrier
 * (exact while the maximum value stays at or below 2^17 -- the dispatch gate
 * enforces r300_grid_strided_index_exact).  Integer 32-bit single-component
 * stores only: the byte decomposition writes the little-endian integer, not
 * an IEEE-754 bit pattern, so a float store is not this shape. */
struct r300_compute_affine_iota_pattern {
   bool       is_affine_iota;
   uint32_t   output_ssbo_binding;  /* binding of the store_ssbo dest */
   bool       output_ssbo_binding_valid;
   uint32_t   stride;               /* ax in ax * id.x + ay * id.y + az * id.z + b */
   uint32_t   stride_y;             /* ay; 0 for the 1D linear form */
   uint32_t   stride_z;             /* az; 0 for the 1D linear form */
   uint32_t   offset;               /* b */
   uint32_t   output_offset_stride;  /* store byte offset ax */
   uint32_t   output_offset_stride_y;
   uint32_t   output_offset_stride_z;
   uint32_t   output_offset_offset;
};

void r300_nir_detect_affine_iota_pattern(const struct nir_shader *s,
                                         struct r300_compute_affine_iota_pattern *out);

/* Constant-operand compare-and-swap pattern (CAS ROUTE A):
 * old = atomicCompSwap(guard[gid], C_expect, C_new); result[gid] = old.
 * Per-element guards addressed by the invocation index need no bus
 * atomicity (one fragment per cell); the replay samples the guard,
 * compares the four bytes against C_expect with per-channel SEQ
 * (silicon-confirmed exact for all u32 -- the byteseq calibration), and
 * selects C_new or the original bytes into the guard render target; the
 * pre-image is copied to the result buffer first (the returned old).
 * Only the constant-operand comp_swap is admitted; every other atomic
 * stays R300_COMPUTE_REJECT_GENERAL_ATOMIC. */
struct r300_compute_cas_pattern {
   bool       is_cas;
   uint32_t   guard_ssbo_binding;
   bool       guard_binding_valid;
   uint32_t   result_ssbo_binding;
   bool       result_binding_valid;
   uint32_t   expect;
   uint32_t   value_new;
};

void r300_nir_detect_cas_pattern(const struct nir_shader *s,
                                 struct r300_compute_cas_pattern *out);

/* log4 2x2 average-pool pattern (one tree level):
 * out[y * W2 + x] = (in[2y*W + 2x] + in[2y*W + 2x + 1] + in[(2y+1)*W + 2x]
 *                  + in[(2y+1)*W + 2x + 1] + 2) >> 2
 * -- exactly the TX bilinear corner tap's measured half-up conversion
 * (log4round calibration, 27/27).  The detector captures the row constant
 * W off the load-offset affines; the replay validates W against the
 * dispatch grid, RANGE-SCANS the input (elements >= 256 spill out of the
 * RGBA8 R channel the filter averages -- a data-dependent bound no static
 * admission can prove; out-of-range dispatches refuse with the explicit
 * no-op), and runs one LINEAR corner-tap pass. */
struct r300_compute_log4_pool_pattern {
   bool       is_log4_pool;
   uint32_t   input_ssbo_binding;
   bool       input_binding_valid;
   uint32_t   output_ssbo_binding;
   bool       output_binding_valid;
   uint32_t   row_w;   /* input row width W in elements */
};

void r300_nir_detect_log4_pool_pattern(const struct nir_shader *s,
                                       struct r300_compute_log4_pool_pattern *out);

/* Multilimb u32 multiply pattern: out[gid] = a[gid] * b[gid] for
 * single-component 32-bit integers -- the binary-map imul shape, routed to
 * the exact multilimb orchestrator instead of the FP24-bounded elementwise
 * map.  Each factor splits into five 7-bit limbs extracted byte-locally
 * from the RGBA8 carrier; the product's nine convolution columns
 * c_k = sum_{i+j=k} a_i * b_j each stay <= 5 * 127^2 = 80645 < 2^17, an
 * exact FP24 integer (MULTILIMB7_U32_MUL in the virtual-op catalog,
 * HW-confirmed bit-exact through 0xFFFFFFFF^2).  The dispatch computes one
 * column per fragment pass and the host assembles the carries, writing the
 * low 32 bits of the exact 64-bit product. */
struct r300_compute_multilimb_mul_pattern {
   bool       is_multilimb_mul;
   uint32_t   input_a_ssbo_binding;
   uint32_t   input_b_ssbo_binding;
   uint32_t   output_ssbo_binding;
};

void r300_nir_detect_multilimb_mul_pattern(const struct nir_shader *s,
                                           struct r300_compute_multilimb_mul_pattern *out);

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
