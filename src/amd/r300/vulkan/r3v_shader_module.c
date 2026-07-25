/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_shader_module.h"
#include "r3v_device.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_object.h"

#include <string.h>

VkResult
r3v_CreateShaderModule(VkDevice _device,
                           const VkShaderModuleCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkShaderModule *pShaderModule)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   struct r3v_shader_module *mod;

   mod = vk_alloc2(&device->vk.alloc, pAllocator,
                   sizeof(*mod) + pCreateInfo->codeSize, 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mod)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &mod->base,
                       VK_OBJECT_TYPE_SHADER_MODULE);
   mod->code_size = pCreateInfo->codeSize;
   memcpy(mod->code, pCreateInfo->pCode, pCreateInfo->codeSize);

   *pShaderModule = r3v_shader_module_to_handle(mod);
   return VK_SUCCESS;
}

void
r3v_DestroyShaderModule(VkDevice _device,
                            VkShaderModule _module,
                            const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_shader_module, mod, _module);
   if (!mod)
      return;

   vk_object_base_finish(&mod->base);
   vk_free2(&device->vk.alloc, pAllocator, mod);
}
