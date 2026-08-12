/*
 * SPDX-License-Identifier: MIT
 *
 * Identity-map compute-as-raster lowering primitives.  The classifier
 * (r300_compute_admission.c r300_nir_detect_identity_map) recognizes
 * `out_buffer[gid] = in_buffer[gid]` at pipeline-create time; the helpers
 * here lower one such admitted kernel onto the RS482 fragment pipeline as a
 * fullscreen-quad draw that samples the input buffer wrapped as a
 * PIPE_TEXTURE_2D and writes via RB3D color export.  All r3v Vulkan-API
 * concepts (buffer, descriptor set, dispatch group count) are resolved to
 * pipe_context primitives so the gallium-mediated submit path replays the
 * kernel as a graphics draw.
 *
 * Three helpers:
 *   r3v_identity_map_wrap_input_as_sampler_view   wraps a pipe_resource
 *       buffer as a PIPE_TEXTURE_2D sampler view for fragment-draw input.
 *   r3v_identity_map_dispatch_replay              lowers the admitted
 *       kernel onto the compute-as-raster substrate as a fullscreen-quad draw.
 *   bit-exact readback oracle validates the dispatched draw result.
 */

#ifndef R3V_IDENTITY_MAP_H
#define R3V_IDENTITY_MAP_H

#include "r3v_private.h"

#include "pipe/p_state.h"
#include "util/format/u_formats.h"

#ifdef __cplusplus
extern "C" {
#endif

struct r3v_device;
struct r3v_pipeline;
struct r3v_cmd_dispatch;
struct r3v_cmd_bind_descriptor_sets;

/* Dispatch-time index-exactness gate.  Maps the pipeline's classified
 * invocation-index consumption onto the grid-fold guard
 * (r300_grid_index_exact): position-addressed kernels are bounded by the
 * 2048x2048 raster fold, kernels that materialize stride * gid + offset in
 * the FP24 fragment ALU are bounded by the 2^17 exact-integer ceiling, and
 * kernels whose index reaches a stored value non-affinely have no derivable
 * bound and never pass.  Returns true when the dispatch's invocation count
 * is honest for the kernel's consumption class; a false return means the
 * replay must not draw (index identity would corrupt silently).  out_reason
 * may be NULL; when non-NULL it receives a stable rejection string. */
bool
r3v_dispatch_index_exact(const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const char **out_reason);

/* Wrap the contents of a PIPE_BUFFER pipe_resource as a transient
 * PIPE_TEXTURE_2D sampler view. The caller selects sampler state; resource
 * creation selects the texture layout. The helper populates the texture with a
 * bounded CPU map/copy from the buffer, and the returned sampler view retains
 * it. The caller releases the view with pipe_sampler_view_reference(&view,
 * NULL); dropping the view's last reference also drops the texture's last
 * reference, so the transient resource is freed automatically.
 *
 * total_elements * util_format_get_blocksize(format) MUST equal the byte
 * count the caller intends to read out of src_buf.  width * height is the
 * raster extent and may include padding texels introduced by grid folding.
 *
 * Returns NULL when the requested source span is invalid or resource creation
 * fails. */
struct pipe_sampler_view *
r3v_identity_map_wrap_input_as_sampler_view(struct r3v_device *device,
                                               struct pipe_resource *src_buf,
                                               unsigned byte_offset,
                                               unsigned width,
                                               unsigned height,
                                               uint64_t total_elements,
                                               enum pipe_format format);

/* The dispatch-replay orchestrator: lowers one R3V_CMD_DISPATCH of an
 * identity-map kernel into the equivalent fullscreen-quad draw.  Walks the
 * bound descriptor set to resolve the input + output ssbo bindings to the
 * underlying pipe_resource handles, wraps the input via the helper above,
 * allocates a transient output RT, sets up the framebuffer + viewport +
 * scissor, lazily creates the fullscreen VBO + vertex elements, binds the
 * device-cached state CSOs + the pipeline's vs_cso / fs_cso, issues the
 * draw, flushes, and copies the RT contents back to the output buffer.
 *
 * Returns false on any unrecoverable failure (resource_create, descriptor
 * walk miss, dimensions exceeding the 2048-per-axis texture cap).  A
 * failed replay leaves the pipe_context in an indeterminate state; the
 * caller's submit-time flush + fence still signals the Vulkan fence so the
 * dispatch object lifecycle still completes. */
bool
r3v_identity_map_dispatch_replay(struct r3v_device *device,
                                    const struct r3v_pipeline *pl,
                                    const struct r3v_cmd_dispatch *dispatch,
                                    const struct r3v_cmd_bind_descriptor_sets *binds);

/* Binary-map dispatch replay: lowers an admitted compute kernel of the
 * shape out[i] = f(a[i], b[i]) onto a fullscreen-quad fragment draw that
 * samples in_a (sampler 0) + in_b (sampler 1), applies the synthesised
 * per-op ALU in the PFS, and writes to the RT.  Same orchestrator skeleton
 * as identity-map, the only structural changes are: 3-binding layout
 * (input_a / input_b / output), two input wraps, two sampler views bound.
 *
 * Returns false on resource_create failure, descriptor walk miss, or any
 * post-explicit_io binding inconsistency; the queue's caller then falls
 * through to the no-op compute lifecycle and the dispatch still signals
 * the fence so the object lifecycle completes. */
bool
r3v_binary_map_dispatch_replay(struct r3v_device *device,
                                  const struct r3v_pipeline *pl,
                                  const struct r3v_cmd_dispatch *dispatch,
                                  const struct r3v_cmd_bind_descriptor_sets *binds);

/* Binary-transcendental dispatch replay: out[i] = f(a[i], b[i]) for f in
 * {fpow, fdiv}, vec4 componentwise.  Shares the two-in/one-out vec4 carrier with
 * binary_map's float path; pl->fs_cso holds the componentwise transcendental FS. */
bool
r3v_binary_transcendental_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds);

/* Bitwise-logicop dispatch replay: out[i] = a[i] OP b[i] for OP in
 * {iand, ior, ixor}, uint32 packed as RGBA8.  Two draws into one RGBA8 RT (b,
 * then a with the RB3D ROP logic op enabled); bit-exact, no FP. */
bool
r3v_bitwise_logicop_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds);

/* Unary affine-map dispatch replay: out[i] = in[i]*c0 + c1.  Reuses the
 * identity 1-in/1-out replay core -- r3v_unary_map_synthesize_shaders
 * mirrored the bindings + value format into pl->identity_map and bound the MAD
 * fragment program as pl->fs_cso, so the same fullscreen draw scales+biases the
 * sampled texel instead of copying it.  push_data is the queue walk's running
 * 128-byte push-constant window; a pattern with a push-derived c0/c1 binds it
 * at FS CONST[0] so the synthesized constant-file reads see the pushed
 * values. */
bool
r3v_unary_map_dispatch_replay(struct r3v_device *device,
                                 const struct r3v_pipeline *pl,
                                 const struct r3v_cmd_dispatch *dispatch,
                                 const struct r3v_cmd_bind_descriptor_sets *binds,
                                 const uint8_t *push_data);

/* Unary-transcendental dispatch replay: out[i] = f(in[i]) for a single native
 * US scalar transcendental.  Same scalar carrier as the unary_map scalar path;
 * pl->fs_cso holds the 1-TEX-1-scalar transcendental FS.  No push window.  The
 * FP16 RT carrier bounds the result to ~10-bit mantissa (approximate, not
 * exact). */
bool
r3v_unary_transcendental_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds);

/* Logical-shift dispatch replay: out[i] = a[i] << k or >> k (logical, constant
 * k).  Reuses the 1-in/1-out replay core over the UNORM8 carrier with the
 * byte-recombination FS; bit-exact. */
bool
r3v_shift_logical_dispatch_replay(
   struct r3v_device *device,
   const struct r3v_pipeline *pl,
   const struct r3v_cmd_dispatch *dispatch,
   const struct r3v_cmd_bind_descriptor_sets *binds);

/* DP4 (quantized-dot) orchestrator entry: shares the 2-in / 1-out replay core
 * with binary-map; pl->fs_cso holds the pure-NIR DP4 FS. */
bool
r3v_dp4_dispatch_replay(struct r3v_device *device,
                           const struct r3v_pipeline *pl,
                           const struct r3v_cmd_dispatch *dispatch,
                           const struct r3v_cmd_bind_descriptor_sets *binds);

/* QMUL (quaternion Hamilton product) orchestrator entry: shares the 2-in / 1-out
 * replay core with DP4 but renders to an FP16 target and unpacks it into the
 * kernel's vec4 FP32 output buffer; pl->fs_cso holds the synthesized Hamilton
 * FS (r3v_build_qmul_fs_nir). */
bool
r3v_qmul_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds);

/* QDIV (quaternion division) orchestrator entry: the same 2-in / 1-out FP16-RT /
 * FP32-readback core as QMUL, with the dividend a and divisor b as the two inputs;
 * pl->fs_cso holds the synthesized division FS (r3v_build_qdiv_fs_nir). */
bool
r3v_qdiv_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds);

/* MAT4VEC (general 4x4 vertex transform) orchestrator entry: the broadcast matrix
 * is uploaded into the fragment constant file (CONST[0..3] = the four rows), the
 * vertices are the only sampler (per-element, at the dispatch extent); pl->fs_cso
 * holds the synthesized 1-TEX + 4-DP4 transform FS, run through the QMUL
 * FP16-RT/FP32-readback core. */
bool
r3v_mat4vec_dispatch_replay(struct r3v_device *device,
                               const struct r3v_pipeline *pl,
                               const struct r3v_cmd_dispatch *dispatch,
                               const struct r3v_cmd_bind_descriptor_sets *binds);

/* QFMUL (quaternion scalar-broadcast multiply) orchestrator entry: the uniform
 * scalar is uploaded into the fragment constant file (CONST[0].x), the
 * per-element quaternion is the only sampler; pl->fs_cso holds the synthesized
 * 1-TEX + 1-MUL FS, run through the QMUL FP16-RT / FP32-readback core.  The same
 * broadcast-operand-to-constant-file lever as MAT4VEC, dim-1 scalar instead of a
 * 4x4 matrix. */
bool
r3v_qfmul_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds);

/* QROTATE (quaternion rotation sandwich) orchestrator entry: same FP16-RT /
 * FP32-readback 2-in / 1-out core as QMUL, with q and v as the two inputs;
 * pl->fs_cso holds the synthesized sandwich FS (r3v_build_qrotate_fs_nir). */
bool
r3v_qrotate_dispatch_replay(struct r3v_device *device,
                               const struct r3v_pipeline *pl,
                               const struct r3v_cmd_dispatch *dispatch,
                               const struct r3v_cmd_bind_descriptor_sets *binds);

/* QCONJ (quaternion conjugate) orchestrator entry: shares a 1-in / 1-out replay
 * core with QNORM, sampling one FP32 quaternion and unpacking the FP16 sign-flip
 * result into the kernel's vec4 FP32 output; pl->fs_cso holds the synthesized
 * conjugate FS (r3v_build_qconj_fs_nir). */
bool
r3v_qconj_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds);

/* QNORM (quaternion squared norm) orchestrator entry: same 1-in / 1-out FP16-RT
 * core as QCONJ; pl->fs_cso holds the synthesized self-dot FS
 * (r3v_build_qnorm_fs_nir), which broadcasts dot(a,a) across the vec4. */
bool
r3v_qnorm_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds);

/* QNORMALIZE (quaternion normalize) orchestrator entry: same 1-in / 1-out FP16-RT
 * core as QNORM; pl->fs_cso holds the synthesized normalize FS
 * (r3v_build_qnormalize_fs_nir), a * rsqrt(dot(a,a)). */
bool
r3v_qnormalize_dispatch_replay(struct r3v_device *device,
                                  const struct r3v_pipeline *pl,
                                  const struct r3v_cmd_dispatch *dispatch,
                                  const struct r3v_cmd_bind_descriptor_sets *binds);

/* QFM fused orchestrator entries (three inputs a,b,c bound straight, one output):
 * QFMADD out = a*b + c; QFMMUL out = a*b*c.  Both one single-output pass. */
bool
r3v_qfmadd_dispatch_replay(struct r3v_device *device,
                              const struct r3v_pipeline *pl,
                              const struct r3v_cmd_dispatch *dispatch,
                              const struct r3v_cmd_bind_descriptor_sets *binds);
bool
r3v_qfmmul_dispatch_replay(struct r3v_device *device,
                              const struct r3v_pipeline *pl,
                              const struct r3v_cmd_dispatch *dispatch,
                              const struct r3v_cmd_bind_descriptor_sets *binds);

/* OMUL (octonion product) orchestrator entry: the eight-wide Cayley-Dickson
 * product runs as two passes -- the lower-half FS (pl->fs_cso) and the upper-half
 * FS (pl->fs_cso2), each sampling the four quaternion inputs a,b,c,d and writing
 * one output half to an FP16 render target unpacked into the kernel's vec4 FP32
 * output.  Both passes share four sampler views and the fullscreen quad. */
bool
r3v_omul_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds);

/* Octonion elementwise-algebra orchestrator entries.  ONORM rides the 2-in/1-out
 * core (pl->fs_cso = the self-dot-sum FS).  OCONJ and OADD/OSUB write both output
 * halves in one MRT pass (pl->fs_cso_mrt): OCONJ samples two inputs, OADD/OSUB
 * four (bound a,c,b,d so each half reads a contiguous sampler pair). */
bool
r3v_onorm_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds);
bool
r3v_oconj_dispatch_replay(struct r3v_device *device,
                             const struct r3v_pipeline *pl,
                             const struct r3v_cmd_dispatch *dispatch,
                             const struct r3v_cmd_bind_descriptor_sets *binds);
bool
r3v_oaddsub_dispatch_replay(struct r3v_device *device,
                               const struct r3v_pipeline *pl,
                               const struct r3v_cmd_dispatch *dispatch,
                               const struct r3v_cmd_bind_descriptor_sets *binds);

/* ODIV (octonion division) orchestrator entry: out = x*inv(y) (right) or
 * inv(y)*x (left), inv(y) = conj(y)/|y|^2, in two single-output passes (the
 * synthesized half-shaders fs_cso/fs_cso2 each form the reciprocal-scaled
 * conjugate and emit one half).  Four inputs bound straight (xlo,xhi,ylo,yhi),
 * two output halves. */
bool
r3v_odiv_dispatch_replay(struct r3v_device *device,
                            const struct r3v_pipeline *pl,
                            const struct r3v_cmd_dispatch *dispatch,
                            const struct r3v_cmd_bind_descriptor_sets *binds);

/* OTRANS (octonion sandwich x*v*conj(x)) orchestrator entry: two octonion
 * products through a scratch intermediate t = x*v.  Four inputs bound straight
 * (xlo,xhi,vlo,vhi), two output halves; four single-output passes (pass 1 OMUL
 * halves to scratch, pass 2 t*conj(x) halves to the outputs). */
bool
r3v_otrans_dispatch_replay(struct r3v_device *device,
                              const struct r3v_pipeline *pl,
                              const struct r3v_cmd_dispatch *dispatch,
                              const struct r3v_cmd_bind_descriptor_sets *binds);

/* Blend-acc-reduction orchestrator entry: descriptor walk to resolve
 * the (value-input, histogram-output) buffer pair, stage a per-point VBO
 * carrying (pos, packed-RGBA8-value) per gid, bind the blend-enabled
 * `COMB_FCN_ADD` / blend_func (ONE, ONE) state CSO, draw N point primitives
 * into a 1xM RT, copy the accumulated bin cells back to the output buffer.
 * The kernel-shape pattern recognised by r300_nir_detect_blend_acc_reduction
 * is `atomicAdd(out_data[gid & MASK], in_data[gid])` (the histogram /
 * accumulator shape).
 *
 * Returns false on resource_create failure, descriptor walk miss, or any
 * post-explicit_io binding inconsistency; the queue's caller then falls
 * through to the no-op compute lifecycle and the dispatch still signals
 * the fence so the object lifecycle completes. */
bool
r3v_blend_acc_reduction_dispatch_replay(struct r3v_device *device,
                                           const struct r3v_pipeline *pl,
                                           const struct r3v_cmd_dispatch *dispatch,
                                           const struct r3v_cmd_bind_descriptor_sets *binds);

/* ZPASS coverage-count reduction orchestrator entry: resolves the
 * (predicate-source, single-element counter) buffer pair, stages a
 * per-point VBO carrying (pos, predicate-float) per gid, draws N point
 * primitives into a 1xN RT with PIPE_QUERY_OCCLUSION_COUNTER bracketed
 * around the draw.  The fragment program KILL_IF discards every fragment
 * whose baked predicate is 0; surviving fragments increment the depth/
 * stencil unit's ZPASS counter pair (ZB_ZPASS_DATA / ZB_ZPASS_ADDR);
 * pipe_query exposes the sum as a uint64 that the orchestrator truncates to
 * uint32 and writes to count_out[0].  The
 * r300_compute_admission.h owns the kernel-shape and dispatch contract.  The
 * detector validates the zero RHS and the orchestrator applies the
 * dispatch-shape guard for a gl_GlobalInvocationID.x source.  Returns false on
 * resource creation failure, descriptor walk miss, or query-result wait
 * failure; the queue's caller then falls through to the no-op compute
 * lifecycle and the dispatch still signals the fence so the object lifecycle
 * completes. */
/* Multipass FBO ping-pong scan orchestrator: resolves the (input, output,
 * params) buffer triple, reads pass_count from the params buffer, seeds a
 * 1xN RGBA8 texture from the input, runs pass_count dependent fragment
 * passes alternating two textures as sampler source / render target (each
 * pass doubling the texel via the synthesized FS), and copies the final
 * texture to the output buffer.  The unique discriminator from the
 * single-pass identity-map, binary-map, blend-acc-reduction, and
 * ZPASS-reduction kernels is the presence of a nir_loop in the admitted
 * NIR shader.  Returns false on resource creation failure, descriptor walk
 * miss, or a pass_count above the per-byte UNORM8 envelope; the queue's
 * caller then falls through to the no-op compute lifecycle. */
bool
r3v_multipass_scan_dispatch_replay(struct r3v_device *device,
                                      const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const struct r3v_cmd_bind_descriptor_sets *binds);

bool
r3v_zpass_reduction_dispatch_replay(struct r3v_device *device,
                                       const struct r3v_pipeline *pl,
                                       const struct r3v_cmd_dispatch *dispatch,
                                       const struct r3v_cmd_bind_descriptor_sets *binds);

/* Predicated masked-store orchestrator: resolves the (predicate, value,
 * output) buffer triple, seeds a render target from the output buffer's
 * pre-existing contents, draws a fullscreen quad whose fragment program
 * KILL_IFs the masked (predicate-false) fragments and writes the sampled value
 * for the covered ones, then copies the RT back to the output buffer.  Killed
 * fragments perform no ROP write, so masked cells keep the seeded baseline --
 * the masked-store semantics.  Returns false on resource creation failure,
 * descriptor walk miss, or a seed/copy map failure; the queue's caller then
 * falls through to the no-op compute lifecycle. */
bool
r3v_predicated_store_dispatch_replay(struct r3v_device *device,
                                        const struct r3v_pipeline *pl,
                                        const struct r3v_cmd_dispatch *dispatch,
                                        const struct r3v_cmd_bind_descriptor_sets *binds);

/* Multi-tap gather (box-3 convolution) orchestrator: resolves the (input,
 * output) buffer pair, wraps the input as a single PIPE_TEXTURE_2D sampler
 * view (like identity-map), uploads the neighbor texel displacement
 * CONST[0] = (1/width, 0, 0, 0) as a fragment constant, draws a fullscreen
 * quad whose synthesized FS samples the input at three neighborhood offsets
 * (gid-1, gid, gid+1) and sums them in the FP24 ALU, then copies the RT back
 * to the output buffer.  The displacement is dispatch-time (width = the grid
 * size), so the orchestrator computes and uploads it here rather than baking
 * it into the FS at pipeline-create.  Returns false on resource creation
 * failure, descriptor walk miss, or a copy-back map failure; the queue's
 * caller then falls through to the no-op compute lifecycle. */
bool
r3v_multitap_gather_dispatch_replay(struct r3v_device *device,
                                       const struct r3v_pipeline *pl,
                                       const struct r3v_cmd_dispatch *dispatch,
                                       const struct r3v_cmd_bind_descriptor_sets *binds);

#ifdef __cplusplus
}
#endif

bool
r3v_ieee16_classify_dispatch_replay(struct r3v_device *device,
                                       const struct r3v_pipeline *pl,
                                       const struct r3v_cmd_dispatch *dispatch,
                                       const struct r3v_cmd_bind_descriptor_sets *binds);

bool
r3v_ieee16_mul_dispatch_replay(struct r3v_device *device,
                                  const struct r3v_pipeline *pl,
                                  const struct r3v_cmd_dispatch *dispatch,
                                  const struct r3v_cmd_bind_descriptor_sets *binds);

/* Constant-fill dispatch replay: fills the output SSBO with the compile-time
 * constant bytes stored in pl->const_fill.const_value at pipeline-create time.
 * No GPU work is issued; the output buffer is mapped and written directly by
 * the CPU.  Supported for value_components == 1 and value_bit_size == 32
 * (scalar u32 fill, 4 bytes per invocation).  Returns false for shapes the
 * CPU fill cannot reconstruct faithfully (multi-component fills where
 * const_value cannot encode all component bytes); the caller then treats the
 * dispatch as a no-op per R300_COMPUTE_REJECT_UNKNOWN_SHAPE. */
bool
r3v_const_fill_dispatch_replay(struct r3v_device *device,
                                   const struct r3v_pipeline *pl,
                                   const struct r3v_cmd_dispatch *dispatch,
                                   const struct r3v_cmd_bind_descriptor_sets *binds);

/* AFFINE_IOTA dispatch replay: out[gid] = stride * gid + offset, the first
 * verb that materializes the work-item index as an FP24 value.  Draws a quad
 * whose texcoord varying is in TEXEL units (vertex corners 0..width and
 * 0..height) so the interpolated value at each fragment center is
 * (x + 0.5, y + 0.5) without any FP24 division; the FS evaluates the affine
 * and byte-decomposes the integer result into an RGBA8 render target whose
 * raw little-endian bytes are copied to the kernel's u32 output SSBO.  The
 * detector proves the store destination is out[gid], and the dispatch
 * index-exactness gate has already bounded stride * (total - 1) + offset by
 * 2^17 before this runs. */
bool
r3v_affine_iota_dispatch_replay(struct r3v_device *device,
                                   const struct r3v_pipeline *pl,
                                   const struct r3v_cmd_dispatch *dispatch,
                                   const struct r3v_cmd_bind_descriptor_sets *binds);

/* MULTILIMB u32-multiply dispatch replay: out[gid] = a[gid] * b[gid], exact
 * for every u32 pair.  Nine fullscreen passes (one specialized column FS
 * each) compute the 7-bit-limb convolution columns on the FP24 ALU into
 * RGBA8 targets; the host reads the columns back, propagates the carries in
 * 64-bit integers, and writes the low 32 bits of the exact product to the
 * output SSBO. */
bool
r3v_multilimb_mul_dispatch_replay(struct r3v_device *device,
                                     const struct r3v_pipeline *pl,
                                     const struct r3v_cmd_dispatch *dispatch,
                                     const struct r3v_cmd_bind_descriptor_sets *binds);

/* Variable-amount shift dispatch replay: out[gid] = a[gid] << b[gid] (left) or
 * a[gid] >> b[gid] (right, unsigned), per-element amount b.  A gather pass reads
 * 2^M (M = b for left, 31-b for right) from the device 2^j lookup into a
 * transient buffer, then the multilimb convolution multiplies a by 2^M and the
 * readback keeps the 32-bit window of the exact product (low 32 for left,
 * bits[31,62] for right).  Bit-exact for every amount in [0,31]. */
bool
r3v_shift_variable_dispatch_replay(struct r3v_device *device,
                                      const struct r3v_pipeline *pl,
                                      const struct r3v_cmd_dispatch *dispatch,
                                      const struct r3v_cmd_bind_descriptor_sets *binds);

/* CAS dispatch replay: old = atomicCompSwap(guard[gid], C_expect, C_new);
 * result[gid] = old.  Copies the guard pre-image to the result buffer (the
 * returned old values), draws the bytewise-SEQ select into a guard render
 * target, and copies the post-image back into the guard buffer. */
bool
r3v_cas_dispatch_replay(struct r3v_device *device,
                           const struct r3v_pipeline *pl,
                           const struct r3v_cmd_dispatch *dispatch,
                           const struct r3v_cmd_bind_descriptor_sets *binds);

/* log4 2x2 average-pool dispatch replay: one LINEAR corner-tap fullscreen
 * pass at half extent.  Runtime range admission: the input is range-scanned
 * during its host-side map and any element >= 256 refuses the dispatch (the
 * RGBA8 R channel is the filter's carrier; the bound is data-dependent). */
bool
r3v_log4_pool_dispatch_replay(struct r3v_device *device,
                                 const struct r3v_pipeline *pl,
                                 const struct r3v_cmd_dispatch *dispatch,
                                 const struct r3v_cmd_bind_descriptor_sets *binds);

#endif /* R3V_IDENTITY_MAP_H */
