/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_instance.h"

#include "r3v_entrypoints.h"
#include "r3v_physical_device.h"
#include "r3v_private.h"

#include "util/u_debug.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "wsi_common.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* R3V_DEBUG is a comma-separated option list. Instance creation consumes
 * startup. Device creation consumes identity_map for replay diagnostics,
 * classify_nir for lowered compute NIR, log_draws and log_pixels for replay
 * logging, and no_overlay and no_topo for draw-path isolation. */
static const struct debug_control r3v_debug_options[] = {
   {"startup", R3V_DEBUG_STARTUP},
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
 * KHR_device_group_creation, debug-utils, and
 * external-memory-capabilities are not advertised because the
 * corresponding vkEnumeratePhysicalDeviceGroupsKHR /
 * vkCreateDebugUtilsMessengerEXT /
 * vkGetPhysicalDeviceExternalBufferPropertiesKHR entrypoints are not
 * implemented.  Promising an extension without its entrypoints violates
 * Vulkan 1.4 spec ch. 36 "Extensions" and produces
 * VK_ERROR_FEATURE_NOT_PRESENT crashes in application code paths that
 * take the advertised feature flag as a green light. */
static const struct vk_instance_extension_table r3v_instance_extensions_supported = {
   .KHR_get_physical_device_properties2 = true,
   /* The VK_KHR_surface family backs presentation through Mesa's common WSI
    * in software mode (wsi_device_init with sw_device on the physical
    * device): the vkCreate*SurfaceKHR entrypoints and surface queries come
    * from wsi_instance_entrypoints, layered into the instance dispatch in
    * r3v_instance_init.  X11 surfaces only; presentation runs the xcb-shm
    * CPU path, no DRM modifiers or dma-buf involved. */
   .KHR_surface = true,
   .KHR_xcb_surface = true,
   .KHR_xlib_surface = true,
   /* Required by the device-level external-memory family; the physical-device
    * query entry points come from the generated dispatch and
    * r3v_GetPhysicalDeviceImageFormatProperties2's external handling. */
   .KHR_external_memory_capabilities = true,
};

VKAPI_ATTR VkResult VKAPI_CALL
r3v_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                            uint32_t *pPropertyCount,
                                            VkExtensionProperties *pProperties)
{
   if (pLayerName != NULL)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(&r3v_instance_extensions_supported,
                                                     pPropertyCount, pProperties);
}

/* The driver exposes no instance layers.  The Vulkan 1.4 spec
 * (ch. 36 "Layers", vkEnumerateInstanceLayerProperties) requires
 * returning VK_SUCCESS with *pPropertyCount=0 whether or not the
 * caller passed a pProperties array; VK_ERROR_LAYER_NOT_PRESENT is
 * reserved for vkCreateInstance receiving a ppEnabledLayerNames entry
 * the implementation cannot satisfy. */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                        VkLayerProperties *pProperties)
{
   (void)pProperties;
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   *pApiVersion = R3V_API_VERSION;
   return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r3v_GetInstanceProcAddr(VkInstance instanceHandle, const char *pName)
{
   const struct vk_instance *const instance = vk_instance_from_handle(instanceHandle);
   return vk_instance_get_proc_addr(instance, &r3v_instance_entrypoints, pName);
}

VkResult
r3v_instance_init(struct r3v_instance *const instance,
                     const VkInstanceCreateInfo *const create_info,
                     const VkAllocationCallbacks *const allocator)
{
   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table,
                                               &r3v_instance_entrypoints, true);
   /* The common WSI's vkCreate*SurfaceKHR / vkGetPhysicalDeviceSurface* fill
    * every slot the driver leaves empty; without this overlay a GLX/EGL client
    * calls a NULL instance entrypoint at surface creation. */
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table,
                                               &wsi_instance_entrypoints, false);

   VkResult result = vk_instance_init(&instance->vk, &r3v_instance_extensions_supported,
                                      &dispatch_table, create_info, allocator);
   if (result != VK_SUCCESS)
      return vk_error(NULL, result);

   instance->vk.physical_devices.try_create_for_drm = r3v_physical_device_try_create_for_drm;
   instance->vk.physical_devices.destroy = r3v_physical_device_destroy;

   instance->debug_flags = parse_debug_string(getenv("R3V_DEBUG"), r3v_debug_options);

   if (instance->debug_flags & R3V_DEBUG_STARTUP)
      fputs("r3v: info: Created an instance.\n", stderr);

   return VK_SUCCESS;
}

void
r3v_instance_finish(struct r3v_instance *const instance)
{
   vk_instance_finish(&instance->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateInstance(const VkInstanceCreateInfo *const pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkInstance *const pInstance)
{
   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   struct r3v_instance *const instance =
      vk_alloc(pAllocator, sizeof(*instance), alignof(struct r3v_instance),
               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (instance == NULL)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = r3v_instance_init(instance, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return result;
   }

   *pInstance = r3v_instance_to_handle(instance);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyInstance(VkInstance instanceHandle,
                       const VkAllocationCallbacks *pAllocator)
{
   struct r3v_instance *const instance = r3v_instance_from_handle(instanceHandle);
   if (instance == NULL)
      return;

   /* vk_instance_init copies the allocator into instance->vk.alloc; free
    * the instance through that copy so the lifetime matches the one used
    * to allocate it.  The pAllocator argument is accepted for spec
    * conformance but the stored allocator is the authoritative one, as in
    * terakan_DestroyInstance. */
   (void)pAllocator;
   VkAllocationCallbacks alloc = instance->vk.alloc;
   r3v_instance_finish(instance);
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
   return r3v_GetInstanceProcAddr(instanceHandle, pName);
}
