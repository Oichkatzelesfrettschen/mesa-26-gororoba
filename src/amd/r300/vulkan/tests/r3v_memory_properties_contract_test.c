/*
 * SPDX-License-Identifier: MIT
 *
 * Calibrates r3v_memory_properties_check against known-good and known-bad
 * tables, then holds the native report to it.
 */

#undef NDEBUG

#include "r3v_memory_properties_contract.h"

#include <assert.h>
#include <stdio.h>

static VkPhysicalDeviceMemoryProperties
native_table(uint64_t heap_bytes)
{
   VkPhysicalDeviceMemoryProperties m = {0};
   r3v_native_memory_properties_fill(&m, heap_bytes);
   return m;
}

/* The runtime GEM_INFO query supplies the capacity.  A larger per-device
 * report remains visible instead of being truncated by a family constant.
 */
static void
check_native_heap_size(void)
{
   VkPhysicalDeviceMemoryProperties m = native_table(64ULL * 1024 * 1024);
   assert(m.memoryHeaps[0].size == 64ULL * 1024 * 1024);

   m = native_table(4ULL * 1024 * 1024 * 1024);
   assert(m.memoryHeaps[0].size == 4ULL * 1024 * 1024 * 1024);
}

/* Vulkan Device Memory requires a strict subset of property flags to
 * precede its superset. Equal and incomparable flag sets retain either
 * order when the memory types use the same heap.
 */
static void
check_native_type_order(void)
{
   const uint64_t capacity_bytes = 1ULL << 30;
   VkPhysicalDeviceMemoryProperties control = native_table(capacity_bytes);
   control.memoryTypeCount = 3;
   control.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   control.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   control.memoryTypes[2].propertyFlags =
      control.memoryTypes[0].propertyFlags | control.memoryTypes[1].propertyFlags;
   assert(r3v_memory_properties_check(&control, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_OK);

   VkMemoryType temporary = control.memoryTypes[0];
   control.memoryTypes[0] = control.memoryTypes[1];
   control.memoryTypes[1] = temporary;
   assert(r3v_memory_properties_check(&control, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_OK);

   control.memoryTypes[0] = control.memoryTypes[2];
   assert(r3v_memory_properties_check(&control, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_TYPE_SUBSET_ORDER);
   control.memoryTypes[1] = control.memoryTypes[2];
   assert(r3v_memory_properties_check(&control, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_OK);
}

/* The device-only type reaches shared VRAM through NO_CPU_ACCESS
 * placement; the host-visible type is the mapped GTT
 * placement and carries every host property the lane advertises.
 */
static void
check_native_types(void)
{
   const uint64_t capacity_bytes = 4ULL * 1024 * 1024 * 1024;
   const VkPhysicalDeviceMemoryProperties m = native_table(capacity_bytes);

   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_OK);
   assert(m.memoryTypes[R3V_NATIVE_MEMORY_HOST_VISIBLE].propertyFlags &
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   assert((m.memoryTypes[R3V_NATIVE_MEMORY_DEVICE_LOCAL].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0);
   assert((m.memoryTypes[R3V_NATIVE_MEMORY_DEVICE_LOCAL].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == 0);
}

static void
check_known_bad(void)
{
   VkPhysicalDeviceMemoryProperties m;
   const uint64_t capacity_bytes = 4ULL * 1024 * 1024 * 1024;

   m = native_table(capacity_bytes);
   m.memoryHeapCount = 0;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_HEAP_COUNT_OUT_OF_RANGE);

   m = native_table(capacity_bytes);
   m.memoryTypeCount = VK_MAX_MEMORY_TYPES + 1;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_TYPE_COUNT_OUT_OF_RANGE);

   m = native_table(capacity_bytes);
   m.memoryHeaps[0].size = 0;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_HEAP_SIZE_EMPTY);

   m = native_table(capacity_bytes);
   m.memoryHeaps[0].size = capacity_bytes + 1;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CAPACITY);

   /* Two heaps that each fit still exceed the observed capacity together. */
   m = native_table(768ULL * 1024 * 1024);
   m.memoryHeapCount = 2;
   m.memoryHeaps[1] = m.memoryHeaps[0];
   assert(r3v_memory_properties_check(
             &m, 1024ULL * 1024 * 1024) ==
          R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CAPACITY);

   m = native_table(capacity_bytes);
   m.memoryTypes[R3V_NATIVE_MEMORY_DEVICE_LOCAL].heapIndex = m.memoryHeapCount;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_HEAP_INDEX_OUT_OF_RANGE);

   m = native_table(capacity_bytes);
   m.memoryHeaps[0].flags = 0;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_DEVICE_LOCAL_TYPE_ON_HOST_HEAP);

   /* A coherency promise over memory the host cannot map names a window
    * that does not exist. */
   m = native_table(capacity_bytes);
   m.memoryTypes[R3V_NATIVE_MEMORY_DEVICE_LOCAL].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_HOST_PROPERTY_WITHOUT_HOST_VISIBLE);

   m = native_table(capacity_bytes);
   m.memoryTypes[R3V_NATIVE_MEMORY_HOST_VISIBLE].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_NO_HOST_VISIBLE_TYPE);

   m = native_table(capacity_bytes);
   VkMemoryType temporary = m.memoryTypes[R3V_NATIVE_MEMORY_DEVICE_LOCAL];
   m.memoryTypes[R3V_NATIVE_MEMORY_DEVICE_LOCAL] =
      m.memoryTypes[R3V_NATIVE_MEMORY_HOST_VISIBLE];
   m.memoryTypes[R3V_NATIVE_MEMORY_HOST_VISIBLE] = temporary;
   assert(r3v_memory_properties_check(&m, capacity_bytes) ==
          R3V_MEMORY_PROPERTIES_TYPE_SUBSET_ORDER);

   /* A checked sum must reject wraparound even when each heap equals the
    * largest representable capacity. */
   m = native_table(UINT64_MAX);
   m.memoryHeapCount = 2;
   m.memoryHeaps[1] = (VkMemoryHeap){
      .size = 1,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
   };
   assert(r3v_memory_properties_check(&m, UINT64_MAX) ==
          R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CAPACITY);
}

int
main(void)
{
   check_native_heap_size();
   check_native_types();
   check_known_bad();
   check_native_type_order();
   printf("r3v memory-property contract: OK\n");
   return 0;
}
