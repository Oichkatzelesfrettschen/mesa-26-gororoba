/*
 * SPDX-License-Identifier: MIT
 *
 * Calibrates r3v_memory_properties_check against known-good and known-bad
 * tables, then holds the native report to it.
 */

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

/* The kernel pools size the heap while they stay under the platform
 * ceiling, and a larger report clamps to it.
 */
static void
check_native_heap_size(void)
{
   VkPhysicalDeviceMemoryProperties m = native_table(64ULL * 1024 * 1024);
   assert(m.memoryHeaps[0].size == 64ULL * 1024 * 1024);

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   assert(m.memoryHeaps[0].size == R3V_RS48X_MEMORY_CEILING_BYTES);

   m = native_table(4ULL * 1024 * 1024 * 1024);
   assert(m.memoryHeaps[0].size == R3V_RS48X_MEMORY_CEILING_BYTES);
}

/* Type 1 reaches the shared-VRAM carve-out through the NO_CPU_ACCESS
 * placement, so it stays off the host side; type 0 is the mapped GTT
 * placement and carries every host property the lane advertises.
 */
static void
check_native_types(void)
{
   const VkPhysicalDeviceMemoryProperties m =
      native_table(R3V_RS48X_MEMORY_CEILING_BYTES);

   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_OK);
   assert(m.memoryTypes[0].propertyFlags &
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   assert((m.memoryTypes[1].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0);
   assert((m.memoryTypes[1].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == 0);
}

static void
check_known_bad(void)
{
   VkPhysicalDeviceMemoryProperties m;

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryHeapCount = 0;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_HEAP_COUNT_OUT_OF_RANGE);

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryTypeCount = VK_MAX_MEMORY_TYPES + 1;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_TYPE_COUNT_OUT_OF_RANGE);

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryHeaps[0].size = 0;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_HEAP_SIZE_EMPTY);

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryHeaps[0].size = R3V_RS48X_MEMORY_CEILING_BYTES + 1;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CEILING);

   /* Two heaps that each fit still exceed the platform ceiling together. */
   m = native_table(R3V_RS48X_GTT_APERTURE_BYTES);
   m.memoryHeapCount = 2;
   m.memoryHeaps[1] = m.memoryHeaps[0];
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_HEAP_SIZE_ABOVE_CEILING);

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryTypes[1].heapIndex = m.memoryHeapCount;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_HEAP_INDEX_OUT_OF_RANGE);

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryHeaps[0].flags = 0;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_DEVICE_LOCAL_TYPE_ON_HOST_HEAP);

   /* A coherency promise over memory the host cannot map names a window
    * that does not exist. */
   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryTypes[1].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_HOST_PROPERTY_WITHOUT_HOST_VISIBLE);

   m = native_table(R3V_RS48X_MEMORY_CEILING_BYTES);
   m.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   assert(r3v_memory_properties_check(&m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
          R3V_MEMORY_PROPERTIES_NO_HOST_VISIBLE_TYPE);
}

int
main(void)
{
   check_native_heap_size();
   check_native_types();
   check_known_bad();
   printf("r3v memory-property contract: OK\n");
   return 0;
}
