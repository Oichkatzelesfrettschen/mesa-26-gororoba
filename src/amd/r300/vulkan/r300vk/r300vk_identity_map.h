/*
 * Copyright (c) 2026 Terascale Functionalists
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

#ifdef __cplusplus
}
#endif

#endif /* R300VK_IDENTITY_MAP_H */
