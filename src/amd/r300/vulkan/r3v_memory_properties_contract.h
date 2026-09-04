/*
 * SPDX-License-Identifier: MIT
 *
 * RS48x memory-property invariants and the runtime capacity reported by
 * Radeon DRM.
 */

#ifndef R3V_MEMORY_PROPERTIES_CONTRACT_H
#define R3V_MEMORY_PROPERTIES_CONTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

/* The kernel's radeon_gem_info_ioctl() is the authority for the runtime
 * capacity input.  Its `vram_size` field reads the TTM VRAM manager and its
 * `gart_size` field reads the effective rdev->mc.gtt_size after pinned GART
 * space is debited.  The source location is
 * drivers/gpu/drm/radeon/radeon_gem.c:407-423; discover the symbol with
 * `(rg --fixed-strings "radeon_gem_info_ioctl"
 * drivers/gpu/drm/radeon/radeon_gem.c)`.
 *
 * The RS400 GART programming path accepts 32, 64, 128, 256, 512, 1024, and
 * 2048 MiB selectors in rs400_gart_init() and rs400_gart_enable(), at
 * drivers/gpu/drm/radeon/rs400.c:93-105 and :123-148; discover both symbols
 * with `(rg --fixed-strings "rs400_gart_enable"
 * drivers/gpu/drm/radeon/rs400.c)`.  The IGP VRAM interval comes from the
 * firmware-provided NB_TOM range in r100_vram_init_sizes(), at
 * drivers/gpu/drm/radeon/r100.c:2801-2839; discover it with
 * `(rg --fixed-strings "r100_vram_init_sizes"
 * drivers/gpu/drm/radeon/r100.c)`.
 *
 * Firmware and the module's gartsize parameter therefore select the
 * per-device capacities.  The native report preserves the positive,
 * overflow-safe sum of those two DRM values instead of imposing a universal
 * RS485M-family ceiling that the kernel sources do not define.
 */

enum r3v_memory_properties_verdict {
   R3V_MEMORY_PROPERTIES_OK,
   R3V_MEMORY_PROPERTIES_HEAP_COUNT_OUT_OF_RANGE,
   R3V_MEMORY_PROPERTIES_TYPE_COUNT_OUT_OF_RANGE,
   R3V_MEMORY_PROPERTIES_HEAP_SIZE_EMPTY,
   R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CAPACITY,
   R3V_MEMORY_PROPERTIES_HEAP_INDEX_OUT_OF_RANGE,
   R3V_MEMORY_PROPERTIES_DEVICE_LOCAL_TYPE_ON_HOST_HEAP,
   R3V_MEMORY_PROPERTIES_HOST_PROPERTY_WITHOUT_HOST_VISIBLE,
   R3V_MEMORY_PROPERTIES_NO_HOST_VISIBLE_TYPE,
};

/* Vulkan binds a memory type to a heap by index, requires a DEVICE_LOCAL
 * type to live in a DEVICE_LOCAL heap, and derives HOST_COHERENT and
 * HOST_CACHED from a host mapping, so each of those rides HOST_VISIBLE.
 * capacity_bytes is the per-device capacity witness supplied by the caller;
 * every heap and their checked sum stay within that observed capacity.  A
 * device with no host-visible type admits no allocation the host can write,
 * so the table also proves one exists.
 */
static inline enum r3v_memory_properties_verdict
r3v_memory_properties_check(const VkPhysicalDeviceMemoryProperties *m,
                            uint64_t capacity_bytes)
{
   if (m->memoryHeapCount == 0 || m->memoryHeapCount > VK_MAX_MEMORY_HEAPS)
      return R3V_MEMORY_PROPERTIES_HEAP_COUNT_OUT_OF_RANGE;
   if (m->memoryTypeCount == 0 || m->memoryTypeCount > VK_MAX_MEMORY_TYPES)
      return R3V_MEMORY_PROPERTIES_TYPE_COUNT_OUT_OF_RANGE;

   uint64_t total = 0;
   for (uint32_t i = 0; i < m->memoryHeapCount; i++) {
      if (m->memoryHeaps[i].size == 0)
         return R3V_MEMORY_PROPERTIES_HEAP_SIZE_EMPTY;
      if (m->memoryHeaps[i].size > capacity_bytes ||
          total > capacity_bytes - m->memoryHeaps[i].size)
         return R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CAPACITY;
      total += m->memoryHeaps[i].size;
   }
   if (total > capacity_bytes)
      return R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CAPACITY;

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

/* The native lane reports one budget: RS485M-family UMA draws the GTT
 * aperture and the firmware-selected IGP VRAM interval from the same system
 * memory, so a single DEVICE_LOCAL heap carries both kernel pools.  Type 0
 * is the GTT|CPU_ACCESS placement r3v_native_memory_type_policy allocates and
 * type 1 is the VRAM|GTT NO_CPU_ACCESS placement, which stays off the host
 * side because no CPU mapping of that range exists.  The GTT mapping is
 * cached and the aperture unsnooped: radeon_bo_create strips
 * RADEON_GEM_GTT_WC and RADEON_GEM_GTT_UC on every non-PCIE device, so the
 * GTT mmap is always ttm_cached, and rs400_gart_enable programs
 * RS480_AGP_MODE_CNTL with REQ_TYPE_SNOOP_DIS.  These source locations are
 * radeon_object.c:radeon_bo_create, rs400.c:rs400_gart_enable, and
 * radeon_drm_vk_bo.c:radeon_drm_vk_bo_cache_sync; discover them with
 * `(rg --fixed-strings "radeon_bo_create" drivers/gpu/drm/radeon/radeon_object.c)`,
 * `(rg --fixed-strings "rs400_gart_enable"
 * drivers/gpu/drm/radeon/rs400.c)`, and
 * `(rg --fixed-strings "radeon_drm_vk_bo_cache_sync"
 * src/amd/radeon/drm_vk/radeon_drm_vk_bo.c)`.
 * HOST_CACHED reports the mapping attribute, and HOST_COHERENT holds because
 * the driver publishes with radeon_drm_vk_bo_cache_sync at every
 * device-access window the synchronous submit model has -- at the vertex
 * write and over live mappings before the submission ioctl, and invalidating
 * after completion and at map establishment.  A zero kernel-pool sum leaves
 * the caller's fallback in place.
 */
static inline void
r3v_native_memory_properties_fill(VkPhysicalDeviceMemoryProperties *m,
                                  uint64_t heap_bytes)
{
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
