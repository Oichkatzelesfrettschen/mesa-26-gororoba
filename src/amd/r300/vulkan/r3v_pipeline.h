/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_PIPELINE_H
#define R3V_PIPELINE_H

#include "r3v_private.h"

#include "pipe/p_state.h"
#include "vk_object.h"

#include "r300/r300_public.h"
#include "r300/r300_compute_admission.h"

#include "amd/r300/common/r300_fragment_binary.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
   R3V_INPUT_ATTACHMENT_SAMPLER_UNIT = 0,
};

/* r3v_pipeline stores Gallium CSO handles compiled from SPIR-V through
 * vk_spirv_to_nir() -> r300g's internal nir_to_rc path.
 * The ICD does NOT call nir_to_tgsi; r300g handles the NIR lowering
 * internally when create_vs_state / create_fs_state receive
 * PIPE_SHADER_IR_NIR shader state.
 *
 * vs_hw / fs_hw: pre-extracted HW code descriptors filled at pipeline-create
 * time via r300_vs_get_hw_code() / r300_fs_get_hw_code().  A cs-direct
 * emitter consumes these descriptors directly; the pipe_context replay path
 * uses the opaque CSO handles below.
 * vs_hw_valid / fs_hw_valid: false if extraction failed (SW-TCL VS, dummy
 * shader, or empty cb_code); a cs-direct emitter must not submit when either
 * flag is false. */
struct r3v_pipeline {
   struct vk_object_base   base;

   struct r300_vs_hw_code  vs_hw;
   struct r300_fs_hw_code  fs_hw;
   bool                    vs_hw_valid;
   bool                    fs_hw_valid;

   /* R3V-owned deep copy of fs_hw: every consumed dword is copied out of the
    * Gallium CSO at extraction time, so the descriptor outlives
    * delete_fs_state.  A native queue consumes only this copy; the replay
    * path keeps reading fs_hw through the live CSO. */
   struct r300_fragment_binary fs_owned;
   bool                    fs_owned_valid;

   /* The UBO descriptor (set, binding) each stage selects, if any.  r300 has
    * separate vertex and fragment constant files, so each stage binds its own
    * selected uniform buffer at CONST[0].  The replay binds this exact (set,
    * binding) rather than the first UBO in layout order, which is the wrong
    * buffer when a set declares several UBOs and the shader reads a later one.
    * Tracking it per stage lets the vertex and fragment shader read different
    * bindings -- including two bindings of one buffer, as
    * dEQP-VK.ubo.link_by_binding does.  r3v_compile_shader records the slot
    * for the stage it compiles. */
   bool                    vs_has_ubo;
   uint32_t                vs_ubo_set;
   uint32_t                vs_ubo_binding;
   uint32_t                vs_ubo_size;
   bool                    fs_has_ubo;
   uint32_t                fs_ubo_set;
   uint32_t                fs_ubo_binding;
   uint32_t                fs_ubo_size;

   /* The fragment shader reads a subpass input attachment (subpassLoad), lowered
    * to a normalized texture() at gl_FragCoord*inv_extent.  The replay binds the
    * input image from this descriptor (set, binding); r300's Gallium callbacks
    * require sampler updates to start at unit zero, so the lowered texture uses
    * R3V_INPUT_ATTACHMENT_SAMPLER_UNIT.  The inv_extent vec2 is bound at
    * fragment CONST[0]. */
   bool                    fs_has_input_attachment;
   uint32_t                fs_input_attachment_set;
   uint32_t                fs_input_attachment_binding;

   /* Flattens every combined-image-sampler in the pipeline layout into r300's one
    * fragment texture-unit space.  nir_lower_samplers keys the Gallium unit on the
    * sampler variable's binding (ignoring the descriptor set), and the replay binds
    * each descriptor to a unit; without a shared map a sampler in set 1 and one in
    * set 0 at the same binding would alias.  Units are assigned in (set, binding)
    * order across the whole layout, so samplers in any descriptor set -- not just
    * set 0 -- resolve to a distinct unit on both the compile and replay sides.
    * r3v_compile_shader rewrites each FS sampler's binding to fs_sampler_map[].unit
    * before nir_lower_samplers; r3v_bind_descriptor_textures looks up the same
    * unit per (set, binding). */
   struct {
      uint16_t             set;
      uint16_t             binding;
      uint16_t             unit;
   }                       fs_sampler_map[R3V_MAX_FS_SAMPLER_UNITS];
   uint16_t                fs_sampler_map_count;

   /* Set when the experimental NEAREST tile-stitch gate is on and the fragment
    * shader samples: each combined-image-sampler then reserves a 2x2 tile-unit
    * grid (so its four per-tile fetches get distinct units), the replay binds the
    * tile views and uploads the per-image affine/split geometry to CONST[0], and
    * an app UBO / push constant / subpass input is rejected (the geometry needs
    * CONST[0]). */
   bool                    fs_nearest_stitch;

   /* A compute pipeline created under the experimental hybrid-compute gate.
    * The no-op kernel carries no graphics CSOs; lowering the kernel onto the
    * compute-as-raster substrate is a later stage. */
   bool                    is_compute;

   /* Identity-map kernel detected at pipeline-create time.  When set, the
    * dispatch replay lowers the kernel onto the compute-as-raster substrate as
    * a fullscreen-quad fragment draw that samples in_tex (NEAREST) and writes
    * via RB3D color export, then copies the RT to the output ssbo via the
    * existing R3V_CMD_COPY_IMAGE_TO_BUFFER path.  The bindings are the ssbo
    * indices the kernel reads/writes; the dispatch resolves them through the
    * bound descriptor sets. */
   struct r300_compute_identity_pattern identity_map;

   /* Texture-pair binary-map kernel detected at pipeline-create time.  Same
    * orchestrator skeleton as identity_map but with two sampler stages (in_a
    * + in_b) and a synthesized FS containing two TEX + one ALU + one MOV
    * out per element. */
   struct r300_compute_binary_map_pattern binary_map;

   /* Single-input affine unary-map kernel detected at pipeline-create time:
    * out[gid] = in[gid]*c0 + c1.  Reuses the identity_map 1-in/1-out dispatch
    * replay -- r3v_unary_map_synthesize_shaders mirrors this pattern's
    * input/output bindings + value format into identity_map and binds a MAD
    * fragment program (tex*c0 + c1) as pl->fs_cso instead of the copy FS. */
   struct r300_compute_unary_map_pattern unary_map;

   /* Single-input transcendental map out[gid] = f(in[gid]) detected at
    * pipeline-create time, f a native US scalar transcendental
    * (rcp/rsq/sqrt/exp2/log2/sin/cos/fract/floor/round).  Reuses the scalar
    * unary_map carrier; pl->fs_cso is the 1-TEX-1-scalar transcendental FS.
    * Precision is FP16-RT-carrier bounded (~10-bit), not bit-exact. */
   struct r300_compute_unary_transcendental_pattern unary_transcendental;

   /* Two-input transcendental map out[gid] = f(a[gid], b[gid]) detected at
    * pipeline-create time, f a non-commutative binary (pow or a/b).  Reuses the
    * vec4 two-in/one-out carrier the binary_map float path uses; pl->fs_cso is
    * the 2-TEX componentwise FS.  Precision is FP16-RT-carrier bounded. */
   struct r300_compute_binary_transcendental_pattern binary_transcendental;

   /* Two-input bitwise map out[gid] = a[gid] OP b[gid] (iand/ior/ixor) detected
    * at pipeline-create time.  The FP24 ALU has no bitwise op, so the dispatch
    * rides the RB3D ROP logic-op output stage: uint32 packed as RGBA8, two draws
    * into one RT (b, then a with the logic op).  Bit-exact, no FP.  pl->vs_cso /
    * pl->fs_cso are the identity-map passthrough VS + copy FS. */
   struct r300_compute_bitwise_logicop_pattern bitwise_logicop;

   /* Logical shift out[gid] = a[gid] << k or >> k (constant k) detected at
    * pipeline-create time.  The FP24 ALU has no shift, so the dispatch reuses
    * the UNORM8 1-in/1-out carrier with a byte-recombination FS (uint32 as RGBA8,
    * each output byte gathers bits from its source byte and neighbour).  Bit-
    * exact.  pl->fs_cso is the synthesized shift FS. */
   struct r300_compute_shift_logical_pattern shift_logical;

   /* Variable-amount shift out[gid] = a[gid] << b[gid] or >> b[gid] (per-element
    * b) detected at pipeline-create time.  Two passes: a gather FS reads 2^M from
    * the device 2^j lookup (M = b for left, 31-b for right) into a transient
    * buffer, then the multilimb convolution multiplies a by 2^M and the readback
    * keeps the 32-bit window (low 32 for left, bits[31,62] for right).  Reuses
    * multilimb_fs[] for the convolution; shift_variable_gather_fs is the lookup
    * shader.  Bit-exact.  pl->fs_cso aliases multilimb_fs[0] for the prologue. */
   struct r300_compute_shift_variable_pattern shift_variable;
   void *shift_variable_gather_fs;
   /* Sign-extension fill FS for variable ishr: after gather+convolve produce the
    * logical ushr result in a transient, this pass adds sign(a)*fill[b].  NULL
    * for ishl/ushr (no sign extension). */
   void *shift_variable_signfill_fs;

   /* Quantized dot-product (DP4) kernel detected at pipeline-create time:
    * out[gid] = dot(in_a[gid], in_b[gid]).  Lowered to a fullscreen draw whose
    * pure-NIR FS samples in_a + in_b and writes their FP24 DP4 to the RT. */
   struct r300_compute_dp4_pattern dp4;

   /* Quaternion Hamilton-product (QMUL) kernel detected at pipeline-create time:
    * out[gid] = q1[gid] * q2[gid], the Cayley-Dickson dim-4 multiply.  Lowered to
    * a fullscreen draw whose synthesized FS (r3v_build_qmul_fs_nir) emits the
    * product as four sign-permuted DP4s to an FP16 render target, unpacked into
    * the kernel's vec4 FP32 output buffer. */
   struct r300_compute_qmul_pattern qmul;

   /* Quaternion division (QDIV) kernel detected at pipeline-create time:
    * out[gid] = a[gid] / b[gid] = a * inv(b), inv(b) = conj(b)*rcp(dot(b,b)).  The
    * dim-4 sibling of ODIV, but one pass: the synthesized FS
    * (r3v_build_qdiv_fs_nir) emits a single Hamilton product over the scaled
    * conjugate (four DP4s plus the US RCP) to an FP16 render target, unpacked into
    * the kernel's vec4 FP32 output buffer.  Same two-in/one-out dispatch as QMUL. */
   struct r300_compute_qdiv_pattern qdiv;

   /* General 4x4 vertex transform (MAT4VEC) detected at pipeline-create time:
    * out[gid] = M * v[gid], four DP4s of v against the broadcast matrix rows --
    * the absent vertex FPU's transform on the FP24 fragment ALU.  The synthesized
    * FS reads the broadcast matrix from the fragment constant file (CONST[0..3])
    * and the per-element vertex from sampler stage 0; the dispatch uploads the
    * matrix as the fragment constant buffer. */
   struct r300_compute_mat4vec_pattern mat4vec;

   /* Quaternion scalar-broadcast multiply (QFMUL) detected at pipeline-create
    * time: out[gid] = q[gid] * s, a per-element quaternion times one uniform
    * scalar broadcast across all four lanes.  The same broadcast-operand lever
    * as MAT4VEC: the uniform scalar rides the fragment constant file (CONST[0].x)
    * instead of a per-fragment texture fetch, so the synthesized FS is one TEX
    * (the quaternion) and one MUL against the constant -- no second sampler. */
   struct r300_compute_qfmul_pattern qfmul;

   /* Quaternion rotation (QROTATE) kernel detected at pipeline-create time:
    * out[gid] = q[gid] * embed(v[gid]) * conj(q[gid]), the sandwich = two Hamilton
    * products = eight DP4s.  Lowered to a fullscreen draw whose synthesized FS
    * (r3v_build_qrotate_fs_nir) emits the sandwich to an FP16 render target,
    * unpacked into the kernel's vec4 FP32 output buffer. */
   struct r300_compute_qrotate_pattern qrotate;

   /* Quaternion conjugate (QCONJ) kernel detected at pipeline-create time:
    * out[gid] = (a.x, -a.y, -a.z, -a.w).  Lowered to a fullscreen draw whose
    * synthesized FS (r3v_build_qconj_fs_nir) sign-flips the sampled quaternion
    * to an FP16 render target, unpacked into the kernel's vec4 FP32 output. */
   struct r300_compute_qconj_pattern qconj;

   /* Quaternion squared-norm (QNORM) kernel detected at pipeline-create time:
    * out[gid] = vec4(dot(a[gid], a[gid])).  Lowered to a fullscreen draw whose
    * synthesized FS (r3v_build_qnorm_fs_nir) writes the broadcast self-dot to
    * an FP16 render target, unpacked into the kernel's vec4 FP32 output. */
   struct r300_compute_qnorm_pattern qnorm;

   /* Quaternion normalize (QNORMALIZE) kernel: out[gid] = a * rsqrt(dot(a,a)).
    * Same one-in/one-out skeleton as QNORM; the synthesized FS
    * (r3v_build_qnormalize_fs_nir) scales the quaternion by the US RSQ of its
    * squared norm. */
   struct r300_compute_qnormalize_pattern qnormalize;

   /* Quaternion fused multiply-add (QFMADD): out = a*b + c, and fused triple
    * product (QFMMUL): out = a*b*c.  Both three-in/one-out, one pass (fs_cso). */
   struct r300_compute_qfmadd_pattern qfmadd;
   struct r300_compute_qfmmul_pattern qfmmul;
   struct r300_compute_ieee16_classify_pattern ieee16_classify;
   struct r300_compute_ieee16_mul_pattern ieee16_mul;

   /* Constant-fill (CONSTFILL) kernel detected at pipeline-create time:
    * out[gid] = C for every element, where C is a compile-time constant.
    * Lowers to a direct CPU buffer fill: no GPU draw is needed since the
    * constant is known at pipeline-create time.  The four bytes of the
    * constant are in const_fill.const_value[0..3] (R=byte 0). */
   struct r300_compute_const_fill_pattern const_fill;

   /* Invocation-index consumption classified at pipeline-create time.
    * Address-only consumption rides raster texel position (the full
    * 2048x2048 fold is honest); value consumption must materialize
    * stride * gid + offset in FP24, so the dispatch-time guard bounds it by
    * the 2^17 exact-integer ceiling (r300_grid_index_exact). */
   struct r300_compute_index_pattern index_consumption;

   /* Affine-iota kernel detected at pipeline-create time: out[gid] =
    * stride * gid + offset with zero loads.  The replay carries the index as
    * a texel-unit varying, evaluates the affine in the FP24 fragment ALU,
    * and byte-decomposes the integer result into an RGBA8 render target. */
   struct r300_compute_affine_iota_pattern affine_iota;

   /* Multilimb u32 multiply: out[gid] = a[gid] * b[gid], exact for ALL u32
    * operands via 7-bit-limb convolution columns on the FP24 ALU
    * (MULTILIMB7_U32_MUL, HW-confirmed).  Takes precedence over the
    * FP24-bounded binary-map imul lowering for this shape.  multilimb_fs
    * holds one specialized column program per convolution column k. */
   struct r300_compute_multilimb_mul_pattern multilimb_mul;
   void *multilimb_fs[9];

   /* Constant-operand compare-and-swap (CAS ROUTE A):
    * old = atomicCompSwap(guard[gid], C_expect, C_new); result[gid] = old.
    * The replay copies the guard pre-image to the result buffer, draws the
    * bytewise-SEQ select into a guard render target, and copies it back. */
   struct r300_compute_cas_pattern cas;

   /* log4 2x2 average pool: one LINEAR corner-tap pass; runtime range
    * admission (elements >= 256 refuse -- the RGBA8 R channel is the
    * filter's carrier). */
   struct r300_compute_log4_pool_pattern log4_pool;

   /* Octonion-product (OMUL) kernel detected at pipeline-create time:
    * (a,b)*(c,d) = (a*c - conj(d)*b, d*a + b*conj(c)) = sixteen DP4s.  Lowered to
    * two fullscreen draws (the lower-half FS in fs_cso, the upper-half FS in
    * fs_cso2), each sampling the four quaternion inputs and writing one output
    * half to an FP16 render target, unpacked into the kernel's vec4 FP32 output. */
   struct r300_compute_omul_pattern omul;

   /* Octonion elementwise-algebra kernels detected at pipeline-create time.
    * OADD/OSUB: out = (a,b) (+|-) (c,d), four inputs / two output halves.  OCONJ:
    * conj((a,b)) = (conj(a), -b), two inputs / two halves.  ONORM: |(a,b)|^2 =
    * dot(a,a)+dot(b,b) broadcast, two inputs / one output.  OADD/OSUB and OCONJ
    * fill both halves in one MRT pass (fs_cso_mrt); ONORM rides the 2-in/1-out
    * core (fs_cso). */
   struct r300_compute_oaddsub_pattern oaddsub;
   struct r300_compute_oconj_pattern oconj;
   struct r300_compute_onorm_pattern onorm;

   /* Octonion right division (ODIV): out = x*inv(y), inv(y) = conj(y)/|y|^2.
    * Four inputs (xlo,xhi,ylo,yhi), two output halves; one MRT pass forms the
    * reciprocal-scaled conjugate and the eight-wide product.  Division stays a
    * dim-8-only op (no zero divisors). */
   struct r300_compute_odiv_pattern odiv;

   /* Octonion sandwich transform (OTRANS): out = x*v*conj(x), two chained
    * octonion products.  Four inputs (xlo,xhi,vlo,vhi), two output halves; the
    * dispatch runs t = x*v to a scratch buffer (fs_cso/fs_cso2), then
    * out = t*conj(x) (fs_cso3/fs_cso4). */
   struct r300_compute_otrans_pattern otrans;

   /* Blend-add reduction kernel detected at pipeline-create time.  Recognized
    * shape:
    *   atomicAdd(out_data[gid & MASK], in_data[gid])
    * lowers to a draw of N point fragments at position (bin, 0), with the FS
    * sampling in_data via a 1D texture coordinate and writing the value to
    * COLOR.  The blend equation `RB3D_CBLEND.COMB_FCN_ADD` with
    * blend_func = (ONE, ONE) accumulates the per-fragment value into the bin
    * cell of the 1xM output RT.  The orchestrator parallels the identity-map
    * orchestrator at one level of indirection -- different RT extent,
    * different VBO, blend state enabled. */
   struct r300_compute_blend_acc_reduction_pattern blend_acc_reduction;

   /* ZPASS coverage-count reduction kernel detected at pipeline-create time.
    * Recognized shape:
    *   if (in_data[gid] >= THRESHOLD) atomicAdd(count_out, 1u);
    * Orchestrator lowers to N point-primitive draws into a 1xN RT with the
    * per-vertex-baked predicate gating fragment KILL; the ZB ZPASS counter
    * (ZB_ZPASS_DATA / ZB_ZPASS_ADDR) accumulates
    * the per-pipe surviving-fragment count, exposed through pipe_query
    * (PIPE_QUERY_OCCLUSION_COUNTER) via r300_query.c. */
   struct r300_compute_zpass_reduction_pattern zpass_reduction;

   /* Multipass FBO ping-pong scan kernel detected at pipeline-create time.
    * Recognized shape:
    *   uint x = in_data[gid];
    *   for (uint k = 0; k < pass_count; k++) x = x * 2u;
    *   out_data[gid] = x;
    * with pass_count a runtime params-buffer load.  Lowers to the
    * compute-as-raster multipass FBO ping-pong substrate verb: the
    * orchestrator runs pass_count dependent fragment passes binding the prior
    * pass's RT as the next pass's sampler.  The unique discriminator from
    * the single-pass kernel classes (identity-map, binary-map,
    * blend-acc-reduction, ZPASS-reduction) is the presence of a nir_loop
    * in the admitted NIR shader. */
   struct r300_compute_multipass_scan_pattern multipass_scan;

   /* Predicated masked-store pattern detected at pipeline-create time.
    * Recognized shape:
    *   if (in_pred[gid] != 0u) out_data[gid] = in_val[gid];
    * a conditional store_ssbo inside a nir_if with two load_ssbo (predicate +
    * value) and no atomic / no loop.  Lowers to a per-pixel KILL_IF discard:
    * the orchestrator seeds the render target from out_data, draws a fullscreen
    * quad whose FS discards the masked fragments and writes the sampled value
    * for the covered ones, and copies the RT back -- killed fragments keep the
    * seeded baseline.  Discriminated from identity-map (load_count == 1),
    * binary-map (store value is a binary ALU op), blend-acc / ZPASS (atomic),
    * and multipass (loop) by the conditional store with two loads. */
   struct r300_compute_predicated_store_pattern predicated_store;

   /* Multi-tap gather (N-tap neighborhood convolution) detected at
    * pipeline-create time.  Recognized shape:
    *   out_data[gid] = in_data[gid+o0] + in_data[gid+o1] + ... (N >= 3 taps)
    * an unconditional store of an iadd-reduction over N >= 3 load_ssbo leaves
    * from one input buffer, no atomic / no loop.  Lowers to a single
    * fullscreen-quad draw that samples the input (wrapped PIPE_TEXTURE_2D,
    * NEAREST) at N neighborhood taps and sums them in the FP24 ALU.  The
    * detector recognizes the shape but does not extract per-tap offsets; the
    * orchestrator applies a fixed canonical box-3 kernel (taps at gid-1, gid,
    * gid+1) and the probe kernel matches it -- the shared canonical-kernel
    * contract.  The neighbor texel displacement (1/width in normalized
    * texcoord X, the single texture row derive_raster_extent produces for
    * <= 2048 elements) is a dispatch-time quantity the orchestrator uploads as
    * a fragment constant; the FS adds it to the interpolated texcoord.
    * Discriminated from binary-map (load_count == 2; gather has >= 3) and from
    * every loop/atomic/conditional class by the unconditional iadd-tree
    * store. */
   struct r300_compute_multitap_gather_pattern multitap_gather;

   struct r300_compute_admission admission;
   uint32_t local_size_x;
   uint32_t local_size_y;
   uint32_t local_size_z;

   void                   *vs_cso;
   void                   *fs_cso;
   /* Second fragment CSO for the two-pass octonion product (OMUL): fs_cso holds
    * the lower-half FS, fs_cso2 the upper-half FS.  NULL for single-pass ops. */
   void                   *fs_cso2;
   /* MRT fragment CSO for the single-pass OMUL route: both halves in one draw.
    * Non-NULL only when the screen supports two simultaneous FP16 render targets,
    * which is exactly the gate the dispatch uses to prefer route B over A. */
   void                   *fs_cso_mrt;
   /* Third and fourth fragment CSOs for the OTRANS second pass (out = t*conj(x)):
    * fs_cso3 the lower-half FS, fs_cso4 the upper-half FS.  fs_cso/fs_cso2 hold
    * the first-pass OMUL(x,v) half-shaders.  NULL for non-OTRANS ops. */
   void                   *fs_cso3;
   void                   *fs_cso4;
   void                   *blend_cso;
   void                   *rasterizer_cso;
   void                   *dsa_cso;
   void                   *velems_cso;
   /* The element array velems_cso was created from, kept so the replay can
    * rebuild a transient CSO with vkCmdBindVertexBuffers2 strides patched in
    * (VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE makes the bind-time
    * strides authoritative over the vertex-input description's). */
   struct pipe_vertex_element velems_template[PIPE_MAX_ATTRIBS];
   uint32_t                velems_count;
   VkPrimitiveTopology     topology;

   /* The pipe-state templates the fixed CSOs above were created from, kept so
    * the replay can rebuild a transient CSO with dynamic fields overlaid.
    * dyn_mask names the R3V_DYN_* states the pipeline declared dynamic:
    * a vkCmdSet* value applies to a draw only when the bound pipeline made
    * that state dynamic (Vulkan's static-else-dynamic rule), so the replay
    * masks its merged shadow with this before overlaying. */
   struct pipe_rasterizer_state          rs_template;
   struct pipe_depth_stencil_alpha_state dsa_template;
   uint32_t                dyn_mask;
   float                   static_blend_const[4];
   uint32_t                static_stencil_ref_front;
   uint32_t                static_stencil_ref_back;
   uint32_t                vertex_stride[R3V_MAX_VERTEX_BINDINGS];
   uint32_t                vertex_binding_extent[R3V_MAX_VERTEX_BINDINGS];
   uint32_t                vertex_binding_mask;
   /* Subset of vertex_binding_mask whose Vulkan binding inputRate advances per
    * instance; replay bounds these against firstInstance/instanceCount. */
   uint32_t                vertex_instance_binding_mask;

   /* Pipeline-static viewport/scissor, captured at create time when the
    * matching VK_DYNAMIC_STATE_VIEWPORT/SCISSOR is not enabled.  Static
    * viewport state lives in VkPipelineViewportStateCreateInfo, so no
    * CmdSetViewport/CmdSetScissor command records a replay entry.  Draw replay
    * applies the captured state with the live tile origin before
    * rasterization; has_static_* false means replay-supplied dynamic state
    * remains active. */
   VkViewport              static_viewport;
   VkRect2D                static_scissor;
   bool                    has_static_viewport;
   bool                    has_static_scissor;

   /* Synthetic VS-system-value stream.  The RC vertex path has no
    * system-value input slot, so R3V supplies VertexIndex / InstanceIndex
    * as driver-generated vertex attributes at reserved driver_location and
    * vertex-buffer binding pairs. */
   bool                    needs_vertex_id_stream;
   bool                    needs_instance_id_stream;
   /* The shader reads push constants, lowered onto CONST[0]; replay binds the
    * running push-constant window there instead of the descriptor UBO (the two
    * are mutually exclusive -- a pipeline using both is rejected at compile). */
   bool                    uses_push_constants;
   /* One bit per 4-byte word of the 128-byte push-constant window: set when the
    * shader loads that word as an integer.  r300's constant file is float-only
    * and nir_lower_int_to_float represents an integer by its float value, but
    * leaves external push-constant VALUES as raw int bits; the replay converts
    * the marked words int->float before binding CONST[0] so the FP24 ALU sees
    * the value the lowered integer ops expect. */
   uint32_t                push_const_int_word_mask;
   uint8_t                 vertex_id_slot;
   uint8_t                 instance_id_slot;
   uint8_t                 vertex_id_vb_binding;
   uint8_t                 instance_id_vb_binding;
};

/* True when one of the compute-as-raster verb detectors fired, i.e. a dispatch
 * of this pipeline reaches a real replay path instead of the unknown-shape
 * no-op.  This OR MUST list exactly the verbs r3v_replay_dispatch routes;
 * adding a verb means updating both.  index_consumption is deliberately absent
 * -- it is a classify-time guard signal, not a dispatch verb, so a kernel that
 * only sets it still no-ops.  Used at pipeline-create to warn for an
 * admitted-but-unmatched kernel (the UNKNOWN_SHAPE reject that the classifier's
 * reject-reason path cannot see, because the kernel passed admission). */
static inline bool
r3v_pipeline_matched_raster_verb(const struct r3v_pipeline *pl)
{
   return pl->identity_map.is_identity_map ||
          pl->const_fill.is_const_fill ||
          pl->affine_iota.is_affine_iota ||
          pl->multilimb_mul.is_multilimb_mul ||
          pl->cas.is_cas ||
          pl->log4_pool.is_log4_pool ||
          pl->binary_map.is_binary_map ||
          pl->unary_map.is_unary_map ||
          pl->unary_transcendental.is_unary_transcendental ||
          pl->binary_transcendental.is_binary_transcendental ||
          pl->bitwise_logicop.is_bitwise_logicop ||
          pl->shift_logical.is_shift_logical ||
          pl->dp4.is_dp4 ||
          pl->qmul.is_qmul ||
          pl->qdiv.is_qdiv ||
          pl->mat4vec.is_mat4vec ||
          pl->qfmul.is_qfmul ||
          pl->qrotate.is_qrotate ||
          pl->qconj.is_qconj ||
          pl->qnorm.is_qnorm ||
          pl->qnormalize.is_qnormalize ||
          pl->omul.is_omul ||
          pl->oaddsub.is_oaddsub ||
          pl->oconj.is_oconj ||
          pl->onorm.is_onorm ||
          pl->odiv.is_odiv ||
          pl->otrans.is_otrans ||
          pl->qfmadd.is_qfmadd ||
          pl->qfmmul.is_qfmmul ||
          pl->blend_acc_reduction.is_blend_acc_reduction ||
          pl->zpass_reduction.is_zpass_reduction ||
          pl->multipass_scan.is_multipass_scan ||
          pl->predicated_store.is_predicated_store ||
          pl->multitap_gather.is_multitap_gather ||
          pl->ieee16_classify.is_ieee16_classify ||
          pl->ieee16_mul.is_ieee16_mul;
}

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_pipeline, base, VkPipeline,
                                VK_OBJECT_TYPE_PIPELINE)

/* Vulkan-to-gallium fixed-function enum converters, shared by the pipeline's
 * static CSO translation and the replay's dynamic-state overlay so both build
 * identical pipe state from the same Vulkan values. */
unsigned r3v_cull_mode_to_pipe(VkCullModeFlags cull);
unsigned r3v_compare_op_to_pipe(VkCompareOp op);
unsigned r3v_stencil_op_to_pipe(VkStencilOp op);

VkResult r3v_CreateGraphicsPipelines(VkDevice device,
                                         VkPipelineCache pipelineCache,
                                         uint32_t createInfoCount,
                                         const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                         const VkAllocationCallbacks *pAllocator,
                                         VkPipeline *pPipelines);

VkResult r3v_CreateComputePipelines(VkDevice device,
                                        VkPipelineCache pipelineCache,
                                        uint32_t createInfoCount,
                                        const VkComputePipelineCreateInfo *pCreateInfos,
                                        const VkAllocationCallbacks *pAllocator,
                                        VkPipeline *pPipelines);

void r3v_DestroyPipeline(VkDevice device,
                             VkPipeline pipeline,
                             const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

/* SPIR-V-to-NIR preparation the pipeline compiles through, exported for the
 * R2VB route-chain host oracle.  pl/vi may be NULL for a vertex-stage host
 * harness; see the definition for the contract. */
struct r3v_device;
struct r3v_pipeline;
VkResult r3v_prepare_shader_nir(struct r3v_device *device,
                                const VkPipelineShaderStageCreateInfo *stage_info,
                                struct r3v_pipeline *pl,
                                const VkPipelineVertexInputStateCreateInfo *vi,
                                struct nir_shader **out_nir);

#endif /* R3V_PIPELINE_H */
