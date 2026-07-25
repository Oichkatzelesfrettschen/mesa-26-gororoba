/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_SHADER_MODULE_H
#define R3V_SHADER_MODULE_H

#include "r3v_private.h"

#include "vk_object.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* r3v_shader_module stores the raw SPIR-V words verbatim.
 * CreateGraphicsPipelines reads code + code_size to drive
 * vk_spirv_to_nir(), which produces the NIR shader that r300g
 * then lowers through nir_to_rc internally. */
struct r3v_shader_module {
   struct vk_object_base  base;
   size_t                 code_size;
   uint32_t               code[];  /* flexible array; allocated as sizeof + code_size */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_shader_module, base, VkShaderModule,
                                VK_OBJECT_TYPE_SHADER_MODULE)

VkResult r3v_CreateShaderModule(VkDevice device,
                                    const VkShaderModuleCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    VkShaderModule *pShaderModule);

void r3v_DestroyShaderModule(VkDevice device,
                                 VkShaderModule shaderModule,
                                 const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif /* R3V_SHADER_MODULE_H */
