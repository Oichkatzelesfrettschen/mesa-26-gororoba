/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_instance.h"

#include "r300vk_entrypoints.h"
#include "r300vk_physical_device.h"
#include "r300vk_private.h"

#include "util/u_debug.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static const struct debug_control r300vk_debug_options[] = {
   {"startup", R300VK_DEBUG_STARTUP},
   {NULL, 0},
};

/* KHR_get_physical_device_properties2 is honoured by the Mesa Vulkan
 * runtime through vk_common_GetPhysicalDeviceProperties2 (generated
 * from vk_physical_device_properties_gen.py).  No driver-side hook is
 * required for the instance-level alias vkGetPhysicalDeviceProperties2KHR
 * because the entrypoints generator (vk_entrypoints_gen.py) emits the
 * alias automatically and the runtime fills it from
 * pdev->properties.
 *
 * KHR_device_group_creation, debug-utils, external-memory-capabilities,
 * and the WSI surface family are not advertised because the
 * corresponding vkEnumeratePhysicalDeviceGroupsKHR /
 * vkCreateDebugUtilsMessengerEXT /
 * vkGetPhysicalDeviceExternalBufferPropertiesKHR /
 * vkCreate*SurfaceKHR entrypoints are not implemented.  Promising an
 * extension without its entrypoints violates Vulkan 1.4 spec ch. 36
 * "Extensions" and produces VK_ERROR_FEATURE_NOT_PRESENT crashes in
 * application code paths that take the advertised feature flag as a
 * green light. */
static const struct vk_instance_extension_table r300vk_instance_extensions_supported = {
   .KHR_get_physical_device_properties2 = true,
};

VKAPI_ATTR VkResult VKAPI_CALL
r300vk_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                            uint32_t *pPropertyCount,
                                            VkExtensionProperties *pProperties)
{
   if (pLayerName != NULL)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(&r300vk_instance_extensions_supported,
                                                     pPropertyCount, pProperties);
}

/* The driver exposes no instance layers.  The Vulkan 1.4 spec
 * (ch. 36 "Layers", vkEnumerateInstanceLayerProperties) requires
 * returning VK_SUCCESS with *pPropertyCount=0 whether or not the
 * caller passed a pProperties array; VK_ERROR_LAYER_NOT_PRESENT is
 * reserved for vkCreateInstance receiving a ppEnabledLayerNames entry
 * the implementation cannot satisfy. */
VKAPI_ATTR VkResult VKAPI_CALL
r300vk_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                        VkLayerProperties *pProperties)
{
   (void)pProperties;
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r300vk_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   *pApiVersion = R300VK_API_VERSION;
   return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r300vk_GetInstanceProcAddr(VkInstance instanceHandle, const char *pName)
{
   const struct vk_instance *const instance = vk_instance_from_handle(instanceHandle);
   return vk_instance_get_proc_addr(instance, &r300vk_instance_entrypoints, pName);
}

VkResult
r300vk_instance_init(struct r300vk_instance *const instance,
                     const VkInstanceCreateInfo *const create_info,
                     const VkAllocationCallbacks *const allocator)
{
   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table,
                                               &r300vk_instance_entrypoints, true);

   VkResult result = vk_instance_init(&instance->vk, &r300vk_instance_extensions_supported,
                                      &dispatch_table, create_info, allocator);
   if (result != VK_SUCCESS)
      return vk_error(NULL, result);

   instance->vk.physical_devices.try_create_for_drm = r300vk_physical_device_try_create_for_drm;
   instance->vk.physical_devices.destroy = r300vk_physical_device_destroy;

   instance->debug_flags = parse_debug_string(getenv("R300VK_DEBUG"), r300vk_debug_options);

   if (instance->debug_flags & R300VK_DEBUG_STARTUP)
      fputs("r300vk: info: Created an instance.\n", stderr);

   return VK_SUCCESS;
}

void
r300vk_instance_finish(struct r300vk_instance *const instance)
{
   vk_instance_finish(&instance->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
r300vk_CreateInstance(const VkInstanceCreateInfo *const pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkInstance *const pInstance)
{
   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   struct r300vk_instance *const instance =
      vk_alloc(pAllocator, sizeof(*instance), alignof(struct r300vk_instance),
               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (instance == NULL)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = r300vk_instance_init(instance, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return result;
   }

   *pInstance = r300vk_instance_to_handle(instance);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r300vk_DestroyInstance(VkInstance instanceHandle,
                       const VkAllocationCallbacks *pAllocator)
{
   struct r300vk_instance *const instance = r300vk_instance_from_handle(instanceHandle);
   if (instance == NULL)
      return;

   /* vk_instance_init copies the allocator into instance->vk.alloc; free
    * the instance through that copy so the lifetime matches the one used
    * to allocate it.  The pAllocator argument is accepted for spec
    * conformance but the stored allocator is the authoritative one, as in
    * terakan_DestroyInstance. */
   (void)pAllocator;
   VkAllocationCallbacks alloc = instance->vk.alloc;
   r300vk_instance_finish(instance);
   vk_free(&alloc, instance);
}

/* The Vulkan ICD loader requires vk_icdGetInstanceProcAddr to be
 * exported from the shared library so it can dispatch entrypoints
 * before any instance exists.  The companion negotiation entry point
 * vk_icdNegotiateLoaderICDInterfaceVersion is provided by
 * src/vulkan/runtime/vk_instance.c and is already exported through
 * vulkan.sym, so this file must not define it. */

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instanceHandle, const char *pName)
{
   return r300vk_GetInstanceProcAddr(instanceHandle, pName);
}
