/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_PIPELINE_H
#define R300VK_PIPELINE_H

#include "r300vk_private.h"

#include "vk_object.h"

#include "r300/r300_public.h"
#include "r300/r300_compute_admission.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_pipeline stores Gallium CSO handles compiled from SPIR-V through
 * vk_spirv_to_nir() -> r300g's internal r300_nir_to_rc_direct path.
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
struct r300vk_pipeline {
   struct vk_object_base   base;

   struct r300_vs_hw_code  vs_hw;
   struct r300_fs_hw_code  fs_hw;
   bool                    vs_hw_valid;
   bool                    fs_hw_valid;

   /* A compute pipeline created under the experimental hybrid-compute gate.
    * The no-op kernel carries no graphics CSOs; lowering the kernel onto the
    * compute-as-raster substrate is a later stage. */
   bool                    is_compute;

   /* Identity-map kernel detected at pipeline-create time.  When set, the
    * dispatch replay lowers the kernel onto the compute-as-raster substrate as
    * a fullscreen-quad fragment draw that samples in_tex (NEAREST) and writes
    * via RB3D color export, then copies the RT to the output ssbo via the
    * existing R300VK_CMD_COPY_IMAGE_TO_BUFFER path.  The bindings are the ssbo
    * indices the kernel reads/writes; the dispatch resolves them through the
    * bound descriptor sets. */
   struct r300_compute_identity_pattern identity_map;

   /* Texture-pair binary-map kernel detected at pipeline-create time.  Same
    * orchestrator skeleton as identity_map but with two sampler stages (in_a
    * + in_b) and a synthesized FS containing two TEX + one ALU + one MOV
    * out per element. */
   struct r300_compute_binary_map_pattern binary_map;

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

   /* M-H per-pixel predicate + masked store detected at pipeline-create time.
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

   void                   *vs_cso;
   void                   *fs_cso;
   void                   *blend_cso;
   void                   *rasterizer_cso;
   void                   *dsa_cso;
   void                   *velems_cso;
   VkPrimitiveTopology     topology;
   uint32_t                vertex_stride[R300VK_MAX_VERTEX_BINDINGS];
   uint32_t                vertex_binding_extent[R300VK_MAX_VERTEX_BINDINGS];
   uint32_t                vertex_binding_mask;

   /* Synthetic VS-system-value stream.  RS480-family parts have no PVS, so a VS
    * that reads gl_VertexIndex / gl_InstanceIndex has the value supplied as a
    * driver-generated vertex attribute (filled per draw with firstVertex + i /
    * firstInstance + i) at a reserved driver_location / vertex-buffer binding;
    * r300_nir_lower_vs_system_values_to_inputs rewrote the intrinsic to read it. */
   bool                    needs_vertex_id_stream;
   bool                    needs_instance_id_stream;
   uint8_t                 vertex_id_slot;          /* velem index == driver_location */
   uint8_t                 instance_id_slot;
   uint8_t                 vertex_id_vb_binding;    /* synthetic vertex_buffer_index */
   uint8_t                 instance_id_vb_binding;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_pipeline, base, VkPipeline,
                                VK_OBJECT_TYPE_PIPELINE)

VkResult r300vk_CreateGraphicsPipelines(VkDevice device,
                                         VkPipelineCache pipelineCache,
                                         uint32_t createInfoCount,
                                         const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                         const VkAllocationCallbacks *pAllocator,
                                         VkPipeline *pPipelines);

VkResult r300vk_CreateComputePipelines(VkDevice device,
                                        VkPipelineCache pipelineCache,
                                        uint32_t createInfoCount,
                                        const VkComputePipelineCreateInfo *pCreateInfos,
                                        const VkAllocationCallbacks *pAllocator,
                                        VkPipeline *pPipelines);

void r300vk_DestroyPipeline(VkDevice device,
                             VkPipeline pipeline,
                             const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_PIPELINE_H */
