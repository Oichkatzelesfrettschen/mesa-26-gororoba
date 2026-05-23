/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_SHADER_MODULE_H
#define R300VK_SHADER_MODULE_H

#include "r300vk_private.h"

#include "vk_object.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_shader_module stores the raw SPIR-V words verbatim.
 * CreateGraphicsPipelines reads code + code_size to drive
 * vk_spirv_to_nir(), which produces the NIR shader that r300g
 * then lowers through r300_nir_to_rc_direct internally. */
struct r300vk_shader_module {
   struct vk_object_base  base;
   size_t                 code_size;
   uint32_t               code[];  /* flexible array; allocated as sizeof + code_size */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r300vk_shader_module, base, VkShaderModule,
                                VK_OBJECT_TYPE_SHADER_MODULE)

VkResult r300vk_CreateShaderModule(VkDevice device,
                                    const VkShaderModuleCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    VkShaderModule *pShaderModule);

void r300vk_DestroyShaderModule(VkDevice device,
                                 VkShaderModule shaderModule,
                                 const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_SHADER_MODULE_H */
