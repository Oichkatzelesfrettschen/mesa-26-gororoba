/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_PIPELINE_H
#define R300VK_PIPELINE_H

#include "r300vk_private.h"

#include "vk_object.h"

#include "r300/r300_public.h"

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
