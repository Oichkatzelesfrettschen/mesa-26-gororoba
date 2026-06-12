/*
 * SPDX-License-Identifier: MIT
 *
 * Identity-map compute-as-raster lowering primitives.  The classifier
 * (r300_compute_admission.c r300_nir_detect_identity_map) recognizes
 * `out_buffer[gid] = in_buffer[gid]` at pipeline-create time; the helpers
 * here lower one such admitted kernel onto the RS482 fragment pipeline as a
 * fullscreen-quad draw that samples the input buffer wrapped as a
 * PIPE_TEXTURE_2D and writes via RB3D color export.  All r300vk Vulkan-API
 * concepts (buffer, descriptor set, dispatch group count) are resolved to
 * pipe_context primitives so the gallium-mediated submit path replays the
 * kernel as a graphics draw.
 *
 * Three helpers:
 *   r300vk_identity_map_wrap_input_as_sampler_view   wraps a pipe_resource
 *       buffer as a PIPE_TEXTURE_2D sampler view for fragment-draw input.
 *   r300vk_identity_map_dispatch_replay              lowers the admitted
 *       kernel onto the compute-as-raster substrate as a fullscreen-quad draw.
 *   bit-exact readback oracle validates the dispatched draw result.
 */

#ifndef R300VK_IDENTITY_MAP_H
#define R300VK_IDENTITY_MAP_H

#include "r300vk_private.h"

#include "pipe/p_state.h"
#include "util/format/u_formats.h"

#ifdef __cplusplus
extern "C" {
#endif

struct r300vk_device;
struct r300vk_pipeline;
struct r300vk_cmd_dispatch;
struct r300vk_cmd_bind_descriptor_sets;

/* Wrap the contents of a PIPE_BUFFER pipe_resource as a transient
 * PIPE_TEXTURE_2D + a pipe_sampler_view configured for NEAREST sampling.
 * The texture is allocated linear-tiled, populated by
 * pipe->resource_copy_region (which falls through to util_resource_copy_region
 * for the buffer-to-texture target combination per r300_blit.c:593), and
 * referenced by the returned sampler view.  The caller releases the view
 * with pipe_sampler_view_reference(&view, NULL); dropping the view's last
 * reference also drops the texture's last reference, so the transient
 * resource is freed automatically.
 *
 * width * height * util_format_get_blocksize(format) MUST equal the byte
 * count the caller intends to read out of src_buf; the helper does not
 * verify the buffer's actual size.
 *
 * Returns NULL on resource_create / create_sampler_view failure. */
struct pipe_sampler_view *
r300vk_identity_map_wrap_input_as_sampler_view(struct r300vk_device *device,
                                               struct pipe_resource *src_buf,
                                               unsigned byte_offset,
                                               unsigned width,
                                               unsigned height,
                                               enum pipe_format format);

/* The dispatch-replay orchestrator: lowers one R300VK_CMD_DISPATCH of an
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
r300vk_identity_map_dispatch_replay(struct r300vk_device *device,
                                    const struct r300vk_pipeline *pl,
                                    const struct r300vk_cmd_dispatch *dispatch,
                                    const struct r300vk_cmd_bind_descriptor_sets *binds);

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
r300vk_binary_map_dispatch_replay(struct r300vk_device *device,
                                  const struct r300vk_pipeline *pl,
                                  const struct r300vk_cmd_dispatch *dispatch,
                                  const struct r300vk_cmd_bind_descriptor_sets *binds);

/* Unary affine-map dispatch replay: out[i] = in[i]*c0 + c1.  Reuses the
 * identity 1-in/1-out replay core -- r300vk_unary_map_synthesize_shaders
 * mirrored the bindings + value format into pl->identity_map and bound the MAD
 * fragment program as pl->fs_cso, so the same fullscreen draw scales+biases the
 * sampled texel instead of copying it.  push_data is the queue walk's running
 * 128-byte push-constant window; a pattern with a push-derived c0/c1 binds it
 * at FS CONST[0] so the synthesized constant-file reads see the pushed
 * values. */
bool
r300vk_unary_map_dispatch_replay(struct r300vk_device *device,
                                 const struct r300vk_pipeline *pl,
                                 const struct r300vk_cmd_dispatch *dispatch,
                                 const struct r300vk_cmd_bind_descriptor_sets *binds,
                                 const uint8_t *push_data);

/* DP4 (quantized-dot) orchestrator entry: shares the 2-in / 1-out replay core
 * with binary-map; pl->fs_cso holds the pure-NIR DP4 FS. */
bool
r300vk_dp4_dispatch_replay(struct r300vk_device *device,
                           const struct r300vk_pipeline *pl,
                           const struct r300vk_cmd_dispatch *dispatch,
                           const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QMUL (quaternion Hamilton product) orchestrator entry: shares the 2-in / 1-out
 * replay core with DP4 but renders to an FP16 target and unpacks it into the
 * kernel's vec4 FP32 output buffer; pl->fs_cso holds the synthesized Hamilton
 * FS (r300vk_build_qmul_fs_nir). */
bool
r300vk_qmul_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QDIV (quaternion division) orchestrator entry: the same 2-in / 1-out FP16-RT /
 * FP32-readback core as QMUL, with the dividend a and divisor b as the two inputs;
 * pl->fs_cso holds the synthesized division FS (r300vk_build_qdiv_fs_nir). */
bool
r300vk_qdiv_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds);

/* MAT4VEC (general 4x4 vertex transform) orchestrator entry: the broadcast matrix
 * is uploaded into the fragment constant file (CONST[0..3] = the four rows), the
 * vertices are the only sampler (per-element, at the dispatch extent); pl->fs_cso
 * holds the synthesized 1-TEX + 4-DP4 transform FS, run through the QMUL
 * FP16-RT/FP32-readback core. */
bool
r300vk_mat4vec_dispatch_replay(struct r300vk_device *device,
                               const struct r300vk_pipeline *pl,
                               const struct r300vk_cmd_dispatch *dispatch,
                               const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QFMUL (quaternion scalar-broadcast multiply) orchestrator entry: the uniform
 * scalar is uploaded into the fragment constant file (CONST[0].x), the
 * per-element quaternion is the only sampler; pl->fs_cso holds the synthesized
 * 1-TEX + 1-MUL FS, run through the QMUL FP16-RT / FP32-readback core.  The same
 * broadcast-operand-to-constant-file lever as MAT4VEC, dim-1 scalar instead of a
 * 4x4 matrix. */
bool
r300vk_qfmul_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QROTATE (quaternion rotation sandwich) orchestrator entry: same FP16-RT /
 * FP32-readback 2-in / 1-out core as QMUL, with q and v as the two inputs;
 * pl->fs_cso holds the synthesized sandwich FS (r300vk_build_qrotate_fs_nir). */
bool
r300vk_qrotate_dispatch_replay(struct r300vk_device *device,
                               const struct r300vk_pipeline *pl,
                               const struct r300vk_cmd_dispatch *dispatch,
                               const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QCONJ (quaternion conjugate) orchestrator entry: shares a 1-in / 1-out replay
 * core with QNORM, sampling one FP32 quaternion and unpacking the FP16 sign-flip
 * result into the kernel's vec4 FP32 output; pl->fs_cso holds the synthesized
 * conjugate FS (r300vk_build_qconj_fs_nir). */
bool
r300vk_qconj_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QNORM (quaternion squared norm) orchestrator entry: same 1-in / 1-out FP16-RT
 * core as QCONJ; pl->fs_cso holds the synthesized self-dot FS
 * (r300vk_build_qnorm_fs_nir), which broadcasts dot(a,a) across the vec4. */
bool
r300vk_qnorm_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QNORMALIZE (quaternion normalize) orchestrator entry: same 1-in / 1-out FP16-RT
 * core as QNORM; pl->fs_cso holds the synthesized normalize FS
 * (r300vk_build_qnormalize_fs_nir), a * rsqrt(dot(a,a)). */
bool
r300vk_qnormalize_dispatch_replay(struct r300vk_device *device,
                                  const struct r300vk_pipeline *pl,
                                  const struct r300vk_cmd_dispatch *dispatch,
                                  const struct r300vk_cmd_bind_descriptor_sets *binds);

/* QFM fused orchestrator entries (three inputs a,b,c bound straight, one output):
 * QFMADD out = a*b + c; QFMMUL out = a*b*c.  Both one single-output pass. */
bool
r300vk_qfmadd_dispatch_replay(struct r300vk_device *device,
                              const struct r300vk_pipeline *pl,
                              const struct r300vk_cmd_dispatch *dispatch,
                              const struct r300vk_cmd_bind_descriptor_sets *binds);
bool
r300vk_qfmmul_dispatch_replay(struct r300vk_device *device,
                              const struct r300vk_pipeline *pl,
                              const struct r300vk_cmd_dispatch *dispatch,
                              const struct r300vk_cmd_bind_descriptor_sets *binds);

/* OMUL (octonion product) orchestrator entry: the eight-wide Cayley-Dickson
 * product runs as two passes -- the lower-half FS (pl->fs_cso) and the upper-half
 * FS (pl->fs_cso2), each sampling the four quaternion inputs a,b,c,d and writing
 * one output half to an FP16 render target unpacked into the kernel's vec4 FP32
 * output.  Both passes share four sampler views and the fullscreen quad. */
bool
r300vk_omul_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds);

/* Octonion elementwise-algebra orchestrator entries.  ONORM rides the 2-in/1-out
 * core (pl->fs_cso = the self-dot-sum FS).  OCONJ and OADD/OSUB write both output
 * halves in one MRT pass (pl->fs_cso_mrt): OCONJ samples two inputs, OADD/OSUB
 * four (bound a,c,b,d so each half reads a contiguous sampler pair). */
bool
r300vk_onorm_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds);
bool
r300vk_oconj_dispatch_replay(struct r300vk_device *device,
                             const struct r300vk_pipeline *pl,
                             const struct r300vk_cmd_dispatch *dispatch,
                             const struct r300vk_cmd_bind_descriptor_sets *binds);
bool
r300vk_oaddsub_dispatch_replay(struct r300vk_device *device,
                               const struct r300vk_pipeline *pl,
                               const struct r300vk_cmd_dispatch *dispatch,
                               const struct r300vk_cmd_bind_descriptor_sets *binds);

/* ODIV (octonion division) orchestrator entry: out = x*inv(y) (right) or
 * inv(y)*x (left), inv(y) = conj(y)/|y|^2, in two single-output passes (the
 * synthesized half-shaders fs_cso/fs_cso2 each form the reciprocal-scaled
 * conjugate and emit one half).  Four inputs bound straight (xlo,xhi,ylo,yhi),
 * two output halves. */
bool
r300vk_odiv_dispatch_replay(struct r300vk_device *device,
                            const struct r300vk_pipeline *pl,
                            const struct r300vk_cmd_dispatch *dispatch,
                            const struct r300vk_cmd_bind_descriptor_sets *binds);

/* OTRANS (octonion sandwich x*v*conj(x)) orchestrator entry: two octonion
 * products through a scratch intermediate t = x*v.  Four inputs bound straight
 * (xlo,xhi,vlo,vhi), two output halves; four single-output passes (pass 1 OMUL
 * halves to scratch, pass 2 t*conj(x) halves to the outputs). */
bool
r300vk_otrans_dispatch_replay(struct r300vk_device *device,
                              const struct r300vk_pipeline *pl,
                              const struct r300vk_cmd_dispatch *dispatch,
                              const struct r300vk_cmd_bind_descriptor_sets *binds);

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
r300vk_blend_acc_reduction_dispatch_replay(struct r300vk_device *device,
                                           const struct r300vk_pipeline *pl,
                                           const struct r300vk_cmd_dispatch *dispatch,
                                           const struct r300vk_cmd_bind_descriptor_sets *binds);

/* ZPASS coverage-count reduction orchestrator entry: resolves the
 * (predicate-source, single-element counter) buffer pair, stages a
 * per-point VBO carrying (pos, predicate-float) per gid, draws N point
 * primitives into a 1xN RT with PIPE_QUERY_OCCLUSION_COUNTER bracketed
 * around the draw.  The fragment program KILL_IF discards every fragment
 * whose baked predicate is 0; surviving fragments increment the depth/
 * stencil unit's ZPASS counter pair (ZB_ZPASS_DATA / ZB_ZPASS_ADDR);
 * pipe_query exposes the sum as a uint64 that the orchestrator truncates to
 * uint32 and writes to count_out[0].  The
 * kernel-shape pattern is `if (in_data[gid] != 0u) atomicAdd(count_out, 1u)`
 * (the orchestrator and probe share a compare-to-zero predicate contract;
 * the detector recognizes the shape but does not extract the comparison RHS,
 * so the contract is enforced on the orchestrator side).  Returns false on
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
r300vk_multipass_scan_dispatch_replay(struct r300vk_device *device,
                                      const struct r300vk_pipeline *pl,
                                      const struct r300vk_cmd_dispatch *dispatch,
                                      const struct r300vk_cmd_bind_descriptor_sets *binds);

bool
r300vk_zpass_reduction_dispatch_replay(struct r300vk_device *device,
                                       const struct r300vk_pipeline *pl,
                                       const struct r300vk_cmd_dispatch *dispatch,
                                       const struct r300vk_cmd_bind_descriptor_sets *binds);

/* M-H predicated masked-store orchestrator: resolves the (predicate, value,
 * output) buffer triple, seeds a render target from the output buffer's
 * pre-existing contents, draws a fullscreen quad whose fragment program
 * KILL_IFs the masked (predicate-false) fragments and writes the sampled value
 * for the covered ones, then copies the RT back to the output buffer.  Killed
 * fragments perform no ROP write, so masked cells keep the seeded baseline --
 * the masked-store semantics.  Returns false on resource creation failure,
 * descriptor walk miss, or a seed/copy map failure; the queue's caller then
 * falls through to the no-op compute lifecycle. */
bool
r300vk_predicated_store_dispatch_replay(struct r300vk_device *device,
                                        const struct r300vk_pipeline *pl,
                                        const struct r300vk_cmd_dispatch *dispatch,
                                        const struct r300vk_cmd_bind_descriptor_sets *binds);

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
r300vk_multitap_gather_dispatch_replay(struct r300vk_device *device,
                                       const struct r300vk_pipeline *pl,
                                       const struct r300vk_cmd_dispatch *dispatch,
                                       const struct r300vk_cmd_bind_descriptor_sets *binds);

#ifdef __cplusplus
}
#endif

bool
r300vk_ieee16_classify_dispatch_replay(struct r300vk_device *device,
                                       const struct r300vk_pipeline *pl,
                                       const struct r300vk_cmd_dispatch *dispatch,
                                       const struct r300vk_cmd_bind_descriptor_sets *binds);

bool
r300vk_ieee16_mul_dispatch_replay(struct r300vk_device *device,
                                  const struct r300vk_pipeline *pl,
                                  const struct r300vk_cmd_dispatch *dispatch,
                                  const struct r300vk_cmd_bind_descriptor_sets *binds);

/* Constant-fill dispatch replay: fills the output SSBO with the compile-time
 * constant bytes stored in pl->const_fill.const_value at pipeline-create time.
 * No GPU work is issued; the output buffer is mapped and written directly by
 * the CPU.  Supported for value_components == 1 and value_bit_size == 32
 * (scalar u32 fill, 4 bytes per invocation).  Returns false for shapes the
 * CPU fill cannot reconstruct faithfully (multi-component fills where
 * const_value cannot encode all component bytes); the caller then treats the
 * dispatch as a no-op per R300_COMPUTE_REJECT_UNKNOWN_SHAPE. */
bool
r300vk_const_fill_dispatch_replay(struct r300vk_device *device,
                                   const struct r300vk_pipeline *pl,
                                   const struct r300vk_cmd_dispatch *dispatch,
                                   const struct r300vk_cmd_bind_descriptor_sets *binds);

#endif /* R300VK_IDENTITY_MAP_H */
