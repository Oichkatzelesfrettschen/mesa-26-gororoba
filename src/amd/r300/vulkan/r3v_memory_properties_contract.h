/*
 * SPDX-License-Identifier: MIT
 *
 * RS48x platform ceilings and the memory-property invariants a reported
 * VkPhysicalDeviceMemoryProperties table satisfies.
 */

#ifndef R3V_MEMORY_PROPERTIES_CONTRACT_H
#define R3V_MEMORY_PROPERTIES_CONTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

/* RS480-family UMA ceilings.  The BIOS carves 128 MiB of system memory for
 * the shared-VRAM partition, and the RS482 memory controller translates a
 * 1 GiB GTT aperture; the K8 host GART stays disabled, so every GPU-visible
 * page arrives through one of those two windows.  DRM_RADEON_GEM_INFO
 * reports the sizes the running kernel actually manages, and those numbers
 * bound the report from below -- the ceilings bound it from above, so a
 * kernel that reports a larger pool cannot inflate an advertised heap past
 * the memory the platform can reach.
 */
#define R3V_RS48X_VRAM_CARVEOUT_BYTES (128ULL * 1024 * 1024)
#define R3V_RS48X_GTT_APERTURE_BYTES  (1024ULL * 1024 * 1024)
#define R3V_RS48X_MEMORY_CEILING_BYTES \
   (R3V_RS48X_VRAM_CARVEOUT_BYTES + R3V_RS48X_GTT_APERTURE_BYTES)

enum r3v_memory_properties_verdict {
   R3V_MEMORY_PROPERTIES_OK,
   R3V_MEMORY_PROPERTIES_HEAP_COUNT_OUT_OF_RANGE,
   R3V_MEMORY_PROPERTIES_TYPE_COUNT_OUT_OF_RANGE,
   R3V_MEMORY_PROPERTIES_HEAP_SIZE_EMPTY,
   R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CEILING,
   R3V_MEMORY_PROPERTIES_HEAP_INDEX_OUT_OF_RANGE,
   R3V_MEMORY_PROPERTIES_DEVICE_LOCAL_TYPE_ON_HOST_HEAP,
   R3V_MEMORY_PROPERTIES_HOST_PROPERTY_WITHOUT_HOST_VISIBLE,
   R3V_MEMORY_PROPERTIES_NO_HOST_VISIBLE_TYPE,
};

/* Vulkan binds a memory type to a heap by index, requires a DEVICE_LOCAL
 * type to live in a DEVICE_LOCAL heap, and derives HOST_COHERENT and
 * HOST_CACHED from a host mapping, so each of those rides HOST_VISIBLE.
 * The ceiling argument carries the platform bound the caller enforces, and
 * every heap plus the total stay under it.  A device with no host-visible
 * type admits no allocation the host can write, so the table also proves one
 * exists.
 */
static inline enum r3v_memory_properties_verdict
r3v_memory_properties_check(const VkPhysicalDeviceMemoryProperties *m,
                            uint64_t ceiling_bytes)
{
   if (m->memoryHeapCount == 0 || m->memoryHeapCount > VK_MAX_MEMORY_HEAPS)
      return R3V_MEMORY_PROPERTIES_HEAP_COUNT_OUT_OF_RANGE;
   if (m->memoryTypeCount == 0 || m->memoryTypeCount > VK_MAX_MEMORY_TYPES)
      return R3V_MEMORY_PROPERTIES_TYPE_COUNT_OUT_OF_RANGE;

   uint64_t total = 0;
   for (uint32_t i = 0; i < m->memoryHeapCount; i++) {
      if (m->memoryHeaps[i].size == 0)
         return R3V_MEMORY_PROPERTIES_HEAP_SIZE_EMPTY;
      if (m->memoryHeaps[i].size > ceiling_bytes)
         return R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CEILING;
      total += m->memoryHeaps[i].size;
   }
   if (total > ceiling_bytes)
      return R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CEILING;

   bool host_visible_present = false;
   for (uint32_t i = 0; i < m->memoryTypeCount; i++) {
      const VkMemoryPropertyFlags flags = m->memoryTypes[i].propertyFlags;
      const uint32_t heap = m->memoryTypes[i].heapIndex;
      if (heap >= m->memoryHeapCount)
         return R3V_MEMORY_PROPERTIES_HEAP_INDEX_OUT_OF_RANGE;
      if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
          !(m->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
         return R3V_MEMORY_PROPERTIES_DEVICE_LOCAL_TYPE_ON_HOST_HEAP;
      if ((flags & (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) &&
          !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         return R3V_MEMORY_PROPERTIES_HOST_PROPERTY_WITHOUT_HOST_VISIBLE;
      if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         host_visible_present = true;
   }
   if (!host_visible_present)
      return R3V_MEMORY_PROPERTIES_NO_HOST_VISIBLE_TYPE;

   return R3V_MEMORY_PROPERTIES_OK;
}

/* The native lane reports one budget: RS480-family UMA draws the GTT
 * aperture and the shared-VRAM carve-out from the same system memory, so a
 * single DEVICE_LOCAL heap carries both kernel pools.  Type 0 is the
 * GTT|CPU_ACCESS placement r3v_native_memory_type_policy allocates and type
 * 1 the VRAM|GTT NO_CPU_ACCESS placement, which reaches the carve-out and
 * stays off the host side because no CPU mapping of that range exists.  The
 * GTT mapping is cached and the aperture unsnooped: radeon_bo_create strips
 * RADEON_GEM_GTT_WC and RADEON_GEM_GTT_UC on every non-PCIE device, so the
 * GTT mmap is always ttm_cached, and rs400_gart_enable programs
 * RS480_AGP_MODE_CNTL with REQ_TYPE_SNOOP_DIS.  HOST_CACHED reports that
 * mapping attribute, and HOST_COHERENT holds because the driver publishes
 * with radeon_drm_vk_bo_cache_sync at every device-access window the
 * synchronous submit model has -- at the vertex write and over live
 * mappings before the submission ioctl, and invalidating after completion
 * and at map establishment.
 * A kernel pool sum of zero leaves the caller's fallback in place.
 */
static inline void
r3v_native_memory_properties_fill(VkPhysicalDeviceMemoryProperties *m,
                                  uint64_t heap_bytes)
{
   if (heap_bytes > R3V_RS48X_MEMORY_CEILING_BYTES)
      heap_bytes = R3V_RS48X_MEMORY_CEILING_BYTES;

   m->memoryHeapCount = 1;
   m->memoryHeaps[0] = (VkMemoryHeap){
      .size = heap_bytes,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
   };
   m->memoryTypeCount = 2;
   m->memoryTypes[0] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      .heapIndex = 0,
   };
   m->memoryTypes[1] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      .heapIndex = 0,
   };
}

#endif /* R3V_MEMORY_PROPERTIES_CONTRACT_H */
