/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_physical_device.h"

#include "r300vk_entrypoints.h"
#include "r300vk_instance.h"
#include "r300vk_private.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

static const char *
r300vk_chip_name_from_pci_device_id(uint32_t pci_device_id)
{
   switch (pci_device_id) {
   case R300VK_PCI_DEVICE_ID_RS482:
      return "ATI RS480 (RS482)";
   case R300VK_PCI_DEVICE_ID_RS485:
      return "ATI RS480 (RS485)";
   default:
      return "ATI RS480";
   }
}

static void
r300vk_physical_device_init_properties(struct vk_properties *const props,
                                       uint32_t const pci_vendor_id,
                                       uint32_t const pci_device_id)
{
   memset(props, 0, sizeof(*props));

   props->apiVersion = R300VK_API_VERSION;

   /* R3xx generation does not carry a driver version distinct from
    * Mesa's r300g.  Report the Mesa fork's primary version number.
    * The Vulkan loader does not parse this field; conformance suites
    * only require it to be a monotonic integer. */
   props->driverVersion = VK_MAKE_VERSION(0, 1, 0);

   props->vendorID = pci_vendor_id;
   props->deviceID = pci_device_id;

   /* RS482/RS485 are integrated graphics in the Radeon Xpress 200M /
    * Xpress 1100/1150 mobile chipsets.  Vulkan treats this as
    * VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU. */
   props->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

   const char *const chip_name = r300vk_chip_name_from_pci_device_id(pci_device_id);
   snprintf(props->deviceName, sizeof(props->deviceName), "%s", chip_name);

   /* Pipeline-cache UUID seeds the disk_cache key.  Fold the API version
    * and PCI ID into the bytes so a header version bump or chip switch
    * invalidates stale entries.
    *
    * FIXME: missing work --
    *           replace the hand-rolled byte layout with a BLAKE3 hash that
    *           ingests disk_cache_get_function_identifier() and
    *           MESA_GIT_SHA1 from src/util/disk_cache.h, matching the
    *           construction in terakan_physical_device.c's pipelineCacheUUID
    *           block.
    *       reason --
    *           BLAKE3 hashing requires the shader cache to be wired and the
    *           sha1_h custom_target from src/meson.build to be plumbed into
    *           the r300vk shared library; neither lands until the device
    *           layer brings in the disk_cache dependency.
    *       tracking-artifact --
    *           disk_cache_get_function_identifier (src/util/disk_cache.h)
    *           and the equivalent block in terakan_physical_device.c near
    *           line 600 of terakan_physical_device_get_capabilities.
    */
   memset(props->pipelineCacheUUID, 0, sizeof(props->pipelineCacheUUID));
   props->pipelineCacheUUID[0] = 'r';
   props->pipelineCacheUUID[1] = '3';
   props->pipelineCacheUUID[2] = '0';
   props->pipelineCacheUUID[3] = '0';
   props->pipelineCacheUUID[4] = 'v';
   props->pipelineCacheUUID[5] = 'k';
   props->pipelineCacheUUID[6] = (uint8_t)(pci_device_id >> 8);
   props->pipelineCacheUUID[7] = (uint8_t)(pci_device_id & 0xff);
   props->pipelineCacheUUID[8] = (uint8_t)(props->apiVersion >> 24);
   props->pipelineCacheUUID[9] = (uint8_t)(props->apiVersion >> 16);
   props->pipelineCacheUUID[10] = (uint8_t)(props->apiVersion >> 8);
   props->pipelineCacheUUID[11] = (uint8_t)(props->apiVersion & 0xff);

   /* VK_KHR_driver_properties identity.
    *
    * FIXME: missing work --
    *           populate driverID with a Khronos-registered VkDriverId for
    *           r300vk so VkPhysicalDeviceDriverProperties (Vulkan
    *           1.2 ch. 4.1.3) carries an accurate driver fingerprint.
    *       reason --
    *           Khronos has not allocated a VkDriverId for this driver;
    *           reusing VK_DRIVER_ID_MESA_RADV would misattribute every
    *           conformance result.  Reporting (VkDriverId)0 keeps the
    *           driverName/driverInfo strings as the load-bearing
    *           attribution surface until a real ID lands.
    *       tracking-artifact --
    *           VkDriverId registry at
    *           https://gitlab.khronos.org/vulkan/vulkan/-/issues and the
    *           VkPhysicalDeviceDriverProperties chapter of the Vulkan
    *           specification (Vulkan 1.2, 4.1.3).
    */
   props->driverID = (VkDriverId)0;
   snprintf(props->driverName, sizeof(props->driverName), "%s", "r300vk");
   snprintf(props->driverInfo, sizeof(props->driverInfo), "%s", "Mesa r300vk");
   props->conformanceVersion = (VkConformanceVersion){0, 0, 0, 0};
}

static const struct vk_device_extension_table r300vk_device_extensions_supported = {
   /* Empty: the loader-visible skeleton advertises no device extensions
    * until the device layer and WSI bring-up wire VK_KHR_swapchain and
    * the external-memory family. */
   0
};

static void
r300vk_physical_device_init_features(struct vk_features *features)
{
   /* Zero optional features.  vk_physical_device_init stores this table
    * so vk_common_GetPhysicalDeviceFeatures2 can answer queries with the
    * exact set the driver supports. */
   memset(features, 0, sizeof(*features));
}

void
r300vk_physical_device_destroy(struct vk_physical_device *const device_base)
{
   struct r300vk_physical_device *const device =
      container_of(device_base, struct r300vk_physical_device, vk);

   if (device->render_node_fd >= 0)
      close(device->render_node_fd);

   vk_physical_device_finish(&device->vk);
   vk_free(&device->vk.instance->alloc, device);
}

VkResult
r300vk_physical_device_try_create_for_drm(struct vk_instance *const instance_base,
                                          struct _drmDevice *const drm_device,
                                          struct vk_physical_device **const device_out)
{
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PCI ||
       drm_device->deviceinfo.pci->vendor_id != R300VK_VENDOR_ID_ATI ||
       !r300vk_pci_device_id_is_supported(drm_device->deviceinfo.pci->device_id)) {
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct r300vk_instance *const instance =
      container_of(instance_base, struct r300vk_instance, vk);

   const char *const render_node_path = drm_device->nodes[DRM_NODE_RENDER];
   int render_node_fd = open(render_node_path, O_RDWR | O_CLOEXEC);
   if (render_node_fd < 0) {
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Failed to open the DRM render node '%s'", render_node_path);
   }

   drmVersionPtr const drm_version = drmGetVersion(render_node_fd);
   if (drm_version == NULL) {
      close(render_node_fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Failed to get DRM version for '%s'", render_node_path);
   }
   const bool is_radeon = strcmp(drm_version->name, "radeon") == 0;
   drmFreeVersion(drm_version);
   if (!is_radeon) {
      close(render_node_fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct r300vk_physical_device *const device =
      vk_alloc(&instance->vk.alloc, sizeof(*device), alignof(struct r300vk_physical_device),
               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      close(render_node_fd);
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   device->pci_vendor_id = drm_device->deviceinfo.pci->vendor_id;
   device->pci_device_id = drm_device->deviceinfo.pci->device_id;
   device->render_node_fd = render_node_fd;

   struct vk_features features;
   r300vk_physical_device_init_features(&features);

   /* vk_physical_device_init copies *properties into pdevice->properties
    * by value (src/vulkan/runtime/vk_physical_device.c:48-49), so the
    * struct must be fully populated before the call.  Mirror the order
    * used by terakan_physical_device_init at
    * src/amd/terascale/vulkan/terakan_physical_device.c around line 1640.
    */
   struct vk_properties properties;
   r300vk_physical_device_init_properties(&properties, device->pci_vendor_id,
                                          device->pci_device_id);

   /* Driver entrypoints only; vk_physical_device_init merges
    * vk_common_physical_device_entrypoints itself at
    * src/vulkan/runtime/vk_physical_device.c:53-55. */
   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &r300vk_physical_device_entrypoints, true);

   VkResult result = vk_physical_device_init(&device->vk, &instance->vk,
                                             &r300vk_device_extensions_supported,
                                             &features, &properties, &dispatch_table);
   if (result != VK_SUCCESS) {
      /* terakan_physical_device_init does not call vk_physical_device_finish
       * on init failure (terakan_physical_device.c fail_isa label); the
       * runtime helper only requires finish after a successful init. */
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   if (instance->debug_flags & R300VK_DEBUG_STARTUP) {
      fprintf(stderr,
              "r300vk: info: Found compatible DRM device '%s' (%04x:%04x).\n",
              render_node_path, device->pci_vendor_id, device->pci_device_id);
   }

   *device_out = &device->vk;
   return VK_SUCCESS;
}

/* Queue family enumeration.  Advertises one graphics+transfer queue
 * family with one queue.  RS482/RS485 has no native compute dispatch
 * surface, so VK_QUEUE_COMPUTE_BIT is intentionally absent. */
VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                               uint32_t *pCount,
                                               VkQueueFamilyProperties2 *pProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pProperties, pCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties = (VkQueueFamilyProperties){
         .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 1,
         .timestampValidBits = 0,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

/* Memory model.  Two heaps placeholder backed by the radeon GTT and
 * shared-VRAM partitions.  Heap sizes are nominal until the device
 * layer queries DRM_RADEON_GEM_INFO. */
VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VkPhysicalDeviceMemoryProperties *const m = &pMemoryProperties->memoryProperties;

   m->memoryHeapCount = 2;
   m->memoryHeaps[0] = (VkMemoryHeap){
      .size = 128 * 1024 * 1024,
      .flags = 0,
   };
   m->memoryHeaps[1] = (VkMemoryHeap){
      .size = 64 * 1024 * 1024,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
   };

   m->memoryTypeCount = 2;
   m->memoryTypes[0] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      .heapIndex = 0,
   };
   m->memoryTypes[1] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      .heapIndex = 1,
   };
}
