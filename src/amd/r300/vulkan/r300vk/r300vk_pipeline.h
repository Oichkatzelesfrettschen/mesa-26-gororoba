/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_PIPELINE_H
#define R300VK_PIPELINE_H

#include "r300vk_private.h"

#include "vk_object.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_pipeline stores Gallium CSO handles compiled from SPIR-V through
 * vk_spirv_to_nir() -> r300g's internal r300_nir_to_rc_direct path.
 * The ICD does NOT call nir_to_tgsi; r300g handles the NIR lowering
 * internally when create_vs_state / create_fs_state receive
 * PIPE_SHADER_IR_NIR shader state. */
struct r300vk_pipeline {
   struct vk_object_base   base;
   void                   *vs_cso;
   void                   *fs_cso;
   void                   *blend_cso;
   void                   *rasterizer_cso;
   void                   *dsa_cso;
   void                   *velems_cso;
   VkPrimitiveTopology     topology;
   uint32_t                vertex_stride[16];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_pipeline, base, VkPipeline,
                                VK_OBJECT_TYPE_PIPELINE)

VkResult r300vk_CreateGraphicsPipelines(VkDevice device,
                                         VkPipelineCache pipelineCache,
                                         uint32_t createInfoCount,
                                         const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                         const VkAllocationCallbacks *pAllocator,
                                         VkPipeline *pPipelines);

void r300vk_DestroyPipeline(VkDevice device,
                             VkPipeline pipeline,
                             const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_PIPELINE_H */
