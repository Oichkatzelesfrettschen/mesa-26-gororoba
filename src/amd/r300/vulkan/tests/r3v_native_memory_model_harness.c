/*
 * SPDX-License-Identifier: MIT
 *
 * Memory-model matrix on the drm-shim fixture: allocation, map, mapped-range,
 * bind, and budget behavior, each arm on a fresh device.  Failure injection
 * rides the transport ops vtable, so a refused GEM create or a failed OS
 * mapping exercises the exact production unwind: the refusal reports the
 * Vulkan error, leaves no partial object, and the same call succeeds once
 * the injection lifts.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#define VK_NO_PROTOTYPES
#include "r3v_native.h"
#include "r3v_entrypoints.h"
#include "r3v_memory_properties_contract.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_ioctl.h"

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

enum arm {
   ARM_ALLOC_REFUSED,
   ARM_MAP_REFUSED,
   ARM_MAPPED_RANGES,
   ARM_BIND_ADMISSION,
   ARM_BUDGET_CONTRACT,
   /* Known-bad: a flush range past the end of the allocation admitted --
    * the range validation this harness proves must make this arm fail. */
   ARM_KNOWN_BAD_RANGE_ADMITS,
};

static enum arm current_arm;
static const struct radeon_drm_vk_ioctl_ops *saved_ops;
static struct radeon_drm_vk_ioctl_ops injected_ops;
static bool inject_live;

static int
injected_command_write_read(int fd, unsigned long request, void *data,
                            unsigned size)
{
   if (inject_live && current_arm == ARM_ALLOC_REFUSED &&
       request == DRM_RADEON_GEM_CREATE)
      return -ENOMEM;
   return saved_ops->command_write_read(fd, request, data, size);
}

static void *
injected_mmap(size_t size, int fd, uint64_t offset)
{
   if (inject_live && current_arm == ARM_MAP_REFUSED)
      return NULL;
   return saved_ops->mmap(size, fd, offset);
}

#define DEVICE_COMMANDS(f)                                                    \
   f(vkAllocateMemory) f(vkFreeMemory) f(vkMapMemory) f(vkUnmapMemory)        \
   f(vkFlushMappedMemoryRanges) f(vkInvalidateMappedMemoryRanges)             \
   f(vkCreateBuffer) f(vkDestroyBuffer) f(vkGetBufferMemoryRequirements)      \
   f(vkBindBufferMemory) f(vkGetDeviceMemoryCommitment) f(vkDestroyDevice)

#define DECLARE(name) static PFN_##name name;
DEVICE_COMMANDS(DECLARE)
#undef DECLARE

static VkResult
flush_one(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
          VkDeviceSize size)
{
   return vkFlushMappedMemoryRanges(
      device, 1,
      &(VkMappedMemoryRange){
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = memory,
         .offset = offset,
         .size = size,
      });
}

static VkDeviceMemory
allocate(VkDevice device, VkDeviceSize size, uint32_t type_index,
         VkResult expected)
{
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkResult result = vkAllocateMemory(
      device,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = size,
         .memoryTypeIndex = type_index,
      },
      NULL, &memory);
   assert(result == expected);
   return memory;
}

static VkBuffer
create_buffer(VkDevice device, VkDeviceSize size)
{
   VkBuffer buffer = VK_NULL_HANDLE;
   assert(vkCreateBuffer(
             device,
             &(VkBufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
             },
             NULL, &buffer) == VK_SUCCESS);
   return buffer;
}

int
main(int argc, char **argv)
{
   assert(argc == 2);
   if (strcmp(argv[1], "alloc-refused") == 0)
      current_arm = ARM_ALLOC_REFUSED;
   else if (strcmp(argv[1], "map-refused") == 0)
      current_arm = ARM_MAP_REFUSED;
   else if (strcmp(argv[1], "mapped-ranges") == 0)
      current_arm = ARM_MAPPED_RANGES;
   else if (strcmp(argv[1], "bind-admission") == 0)
      current_arm = ARM_BIND_ADMISSION;
   else if (strcmp(argv[1], "budget-contract") == 0)
      current_arm = ARM_BUDGET_CONTRACT;
   else if (strcmp(argv[1], "known-bad-range-admits") == 0)
      current_arm = ARM_KNOWN_BAD_RANGE_ADMITS;
   else {
      fprintf(stderr, "unknown arm %s\n", argv[1]);
      return 2;
   }

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   assert(create_instance(&(VkInstanceCreateInfo){
                             .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                          },
                          NULL, &instance) == VK_SUCCESS);
   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(instance,
                                           "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice create_device =
      (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
         instance, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties get_properties =
      (PFN_vkGetPhysicalDeviceProperties)gipa(
         instance, "vkGetPhysicalDeviceProperties");
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated = enumerate(instance, &pdev_count, &pdev);
   assert((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
          pdev_count == 1);

   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   assert(create_device(
             pdev,
             &(VkDeviceCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .queueCreateInfoCount = 1,
                .pQueueCreateInfos =
                   &(VkDeviceQueueCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                      .queueFamilyIndex = 0,
                      .queueCount = 1,
                      .pQueuePriorities = &priority,
                   },
             },
             NULL, &device) == VK_SUCCESS);
#define LOAD(name) name = (PFN_##name)gdpa(device, #name); assert(name);
   DEVICE_COMMANDS(LOAD)
#undef LOAD

   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   saved_ops = native_device->drm.ops;
   injected_ops = *saved_ops;
   injected_ops.command_write_read = injected_command_write_read;
   injected_ops.mmap = injected_mmap;
   native_device->drm.ops = &injected_ops;

   switch (current_arm) {
   case ARM_ALLOC_REFUSED: {
      /* The injected GEM create refusal reports OUT_OF_DEVICE_MEMORY and
       * constructs no memory object; the identical request succeeds once
       * the injection lifts, so the refusal left no device state behind. */
      inject_live = true;
      allocate(device, 4096, 0, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      inject_live = false;
      VkDeviceMemory memory = allocate(device, 4096, 0, VK_SUCCESS);
      vkFreeMemory(device, memory, NULL);
      break;
   }
   case ARM_MAP_REFUSED: {
      VkDeviceMemory memory = allocate(device, 8192, 0, VK_SUCCESS);
      inject_live = true;
      void *map = NULL;
      assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_ERROR_MEMORY_MAP_FAILED);
      inject_live = false;
      /* Map establishment invalidates the fresh window, so the transport's
       * cache-sync count moves exactly once per established mapping. */
      uint64_t syncs_before =
         atomic_load(&native_device->drm.cache_sync_count);
      void *at_offset = NULL;
      assert(vkMapMemory(device, memory, 64, VK_WHOLE_SIZE, 0,
                         &at_offset) == VK_SUCCESS);
      assert(atomic_load(&native_device->drm.cache_sync_count) ==
             syncs_before + 1);
      memset(at_offset, 0x5a, 16);
      vkUnmapMemory(device, memory);
      void *base = NULL;
      assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &base) ==
             VK_SUCCESS);
      assert(((unsigned char *)base)[64] == 0x5a &&
             ((unsigned char *)base)[79] == 0x5a);
      assert((char *)at_offset == (char *)base + 64);
      vkUnmapMemory(device, memory);
      vkFreeMemory(device, memory, NULL);
      break;
   }
   case ARM_MAPPED_RANGES: {
      VkDeviceMemory memory = allocate(device, 4096, 0, VK_SUCCESS);
      void *map = NULL;
      assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS);
      assert(flush_one(device, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
      assert(flush_one(device, memory, 64, 128) == VK_SUCCESS);
      /* offset == size leaves an empty tail, which VK_WHOLE_SIZE names. */
      assert(flush_one(device, memory, 4096, VK_WHOLE_SIZE) == VK_SUCCESS);
      const VkMappedMemoryRange pair[2] = {
         {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory,
            .offset = 0,
            .size = 64,
         },
         {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memory,
            .offset = 2048,
            .size = VK_WHOLE_SIZE,
         },
      };
      assert(vkInvalidateMappedMemoryRanges(device, 2, pair) == VK_SUCCESS);
      /* An offset past the allocation and a size the remaining bytes
       * cannot carry each refuse; the wrap-guard computes against the
       * remaining size, so a near-UINT64_MAX size cannot fold back in. */
      assert(flush_one(device, memory, 4097, VK_WHOLE_SIZE) ==
             R3V_NATIVE_REFUSAL_RESULT);
      assert(flush_one(device, memory, 64, UINT64_MAX - 1) ==
             R3V_NATIVE_REFUSAL_RESULT);
      assert(vkInvalidateMappedMemoryRanges(device, 1, &(VkMappedMemoryRange){
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = memory,
                .offset = 4097,
                .size = VK_WHOLE_SIZE,
             }) == R3V_NATIVE_REFUSAL_RESULT);
      vkUnmapMemory(device, memory);
      vkFreeMemory(device, memory, NULL);
      break;
   }
   case ARM_BIND_ADMISSION: {
      VkDeviceMemory memory = allocate(device, 16384, 0, VK_SUCCESS);
      VkBuffer bound = create_buffer(device, 256);
      VkMemoryRequirements reqs;
      vkGetBufferMemoryRequirements(device, bound, &reqs);
      assert(reqs.alignment == R3V_NATIVE_MEMORY_ALIGNMENT &&
             reqs.memoryTypeBits == 0x1 && reqs.size == 256);
      assert(vkBindBufferMemory(device, bound, memory, 4096) == VK_SUCCESS);
      /* Rebinding, a misaligned offset, and a footprint past the end of
       * the allocation each refuse and leave the named buffer unbound. */
      assert(vkBindBufferMemory(device, bound, memory, 0) ==
             R3V_NATIVE_REFUSAL_RESULT);
      VkBuffer misaligned = create_buffer(device, 256);
      assert(vkBindBufferMemory(device, misaligned, memory, 64) ==
             R3V_NATIVE_REFUSAL_RESULT);
      VkBuffer overflowing = create_buffer(device, 256);
      assert(vkBindBufferMemory(device, overflowing, memory, 16384) ==
             R3V_NATIVE_REFUSAL_RESULT);
      /* Type 1 allocates without CPU access, so the gather could never
       * read a buffer bound there; the bind refuses it by type. */
      VkDeviceMemory device_local = allocate(device, 4096, 1, VK_SUCCESS);
      assert(vkBindBufferMemory(device, misaligned, device_local, 0) ==
             R3V_NATIVE_REFUSAL_RESULT);
      /* Aliasing is admitted by construction: two buffers over
       * overlapping windows of one allocation both bind, and a write
       * through the mapping lands in both windows. */
      VkBuffer alias_low = create_buffer(device, 8192);
      VkBuffer alias_high = create_buffer(device, 8192);
      assert(vkBindBufferMemory(device, alias_low, memory, 0) == VK_SUCCESS);
      assert(vkBindBufferMemory(device, alias_high, memory, 4096) ==
             VK_SUCCESS);
      void *map = NULL;
      assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS);
      ((unsigned char *)map)[4096] = 0xc3;
      struct r3v_native_buffer *low =
         r3v_native_buffer_from_handle(alias_low);
      struct r3v_native_buffer *high =
         r3v_native_buffer_from_handle(alias_high);
      assert(low->memory == high->memory && low->offset == 0 &&
             high->offset == 4096);
      assert(((unsigned char *)map)[low->offset + 4096] == 0xc3 &&
             ((unsigned char *)map)[high->offset] == 0xc3);
      vkUnmapMemory(device, memory);
      vkDestroyBuffer(device, alias_low, NULL);
      vkDestroyBuffer(device, alias_high, NULL);
      vkDestroyBuffer(device, overflowing, NULL);
      vkDestroyBuffer(device, misaligned, NULL);
      /* Freeing the memory before the still-bound buffer proves the
       * binding stores no back-reference the free path must chase. */
      vkFreeMemory(device, memory, NULL);
      vkDestroyBuffer(device, bound, NULL);
      vkFreeMemory(device, device_local, NULL);
      break;
   }
   case ARM_BUDGET_CONTRACT: {
      VkPhysicalDeviceMemoryProperties m;
      get_memory_properties(pdev, &m);
      assert(r3v_memory_properties_check(
                &m, R3V_RS48X_MEMORY_CEILING_BYTES) ==
             R3V_MEMORY_PROPERTIES_OK);
      assert(m.memoryHeapCount == 1 && m.memoryTypeCount == 2);
      assert(m.memoryTypes[0].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      assert(m.memoryTypes[1].propertyFlags ==
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkPhysicalDeviceProperties properties;
      get_properties(pdev, &properties);
      /* 64 bytes is the K8 cache-line granule the CLFLUSH walk covers. */
      assert(properties.limits.nonCoherentAtomSize == 64);
      VkDeviceMemory memory = allocate(device, 4096, 0, VK_SUCCESS);
      VkDeviceSize committed = 1;
      vkGetDeviceMemoryCommitment(device, memory, &committed);
      assert(committed == 0);
      VkBuffer buffer = create_buffer(device, 512);
      VkMemoryDedicatedRequirements dedicated = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
         .prefersDedicatedAllocation = VK_TRUE,
         .requiresDedicatedAllocation = VK_TRUE,
      };
      VkMemoryRequirements2 reqs2 = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
         .pNext = &dedicated,
      };
      r3v_GetBufferMemoryRequirements2(
         device,
         &(VkBufferMemoryRequirementsInfo2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
            .buffer = buffer,
         },
         &reqs2);
      assert(dedicated.prefersDedicatedAllocation == VK_FALSE &&
             dedicated.requiresDedicatedAllocation == VK_FALSE);
      vkDestroyBuffer(device, buffer, NULL);
      vkFreeMemory(device, memory, NULL);
      break;
   }
   case ARM_KNOWN_BAD_RANGE_ADMITS: {
      VkDeviceMemory memory = allocate(device, 4096, 0, VK_SUCCESS);
      assert(flush_one(device, memory, 4097, VK_WHOLE_SIZE) == VK_SUCCESS);
      vkFreeMemory(device, memory, NULL);
      break;
   }
   }

   native_device->drm.ops = saved_ops;
   vkDestroyDevice(device, NULL);
   destroy_instance(instance, NULL);
   printf("memory-model arm %s: OK\n", argv[1]);
   return 0;
}
