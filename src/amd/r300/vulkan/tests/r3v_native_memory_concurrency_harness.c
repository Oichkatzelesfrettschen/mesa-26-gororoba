/*
 * SPDX-License-Identifier: MIT
 *
 * Concurrency and device-loss matrix over the memory path on the drm-shim
 * fixture.  Vulkan permits independent VkDeviceMemory operations to execute
 * concurrently, and the transport's shared-handle table plus the atomic
 * cache-sync counter are the mechanisms that carry that permission; each
 * arm proves one of them under real thread interleaving, and the loss arm
 * proves that a lost queue refuses submissions without reaching the
 * transport while the memory model stays serviceable for teardown.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#define VK_NO_PROTOTYPES
#include "r3v_native.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_ioctl.h"
#include "vk_queue.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"

#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

enum arm {
   ARM_CONCURRENT_MAP_FREE,
   ARM_CONCURRENT_SUBMIT_TEARDOWN,
   ARM_TEARDOWN_LIVE_FREES,
   ARM_DEVICE_LOSS_CLASSIFICATION,
   /* Known-bad: a lost queue admitting a submission -- the loss
    * classification this harness proves must make this arm fail. */
   ARM_KNOWN_BAD_LOST_QUEUE_SUBMITS,
};

static enum arm current_arm;
static const struct radeon_drm_vk_ioctl_ops *saved_ops;
static struct radeon_drm_vk_ioctl_ops counted_ops;
static _Atomic uint64_t transport_calls;

static int
counted_command_write_read(int fd, unsigned long request, void *data,
                           unsigned size)
{
   atomic_fetch_add(&transport_calls, 1);
   return saved_ops->command_write_read(fd, request, data, size);
}

static PFN_vkAllocateMemory vkAllocateMemory;
static PFN_vkFreeMemory vkFreeMemory;
static PFN_vkMapMemory vkMapMemory;
static PFN_vkUnmapMemory vkUnmapMemory;
static PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkQueueWaitIdle vkQueueWaitIdle;
static PFN_vkGetDeviceQueue vkGetDeviceQueue;
static PFN_vkDestroyDevice vkDestroyDevice;

struct worker {
   VkDevice device;
   VkQueue queue;
   VkDeviceMemory *memories;
   uint32_t iterations;
   uint32_t maps_performed;
};

/* One allocate/map/write/flush/unmap/free cycle per iteration, all on
 * memory objects the thread owns alone; the cross-thread state is the
 * device's shared transport. */
static int
memory_cycle_worker(void *arg)
{
   struct worker *w = arg;
   for (uint32_t i = 0; i < w->iterations; i++) {
      VkDeviceMemory memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(
                w->device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = 8192,
                   .memoryTypeIndex = 0,
                },
                NULL, &memory) == VK_SUCCESS);
      void *map = NULL;
      assert(vkMapMemory(w->device, memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS);
      memset(map, (int)(i & 0xff), 8192);
      assert(vkFlushMappedMemoryRanges(
                w->device, 1,
                &(VkMappedMemoryRange){
                   .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                   .memory = memory,
                   .offset = 0,
                   .size = VK_WHOLE_SIZE,
                }) == VK_SUCCESS);
      vkUnmapMemory(w->device, memory);
      vkFreeMemory(w->device, memory, NULL);
      w->maps_performed++;
   }
   return 0;
}

static int
empty_submit_worker(void *arg)
{
   struct worker *w = arg;
   for (uint32_t i = 0; i < w->iterations; i++) {
      assert(vkQueueSubmit(w->queue, 0, NULL, VK_NULL_HANDLE) == VK_SUCCESS);
      assert(vkQueueWaitIdle(w->queue) == VK_SUCCESS);
   }
   return 0;
}

static int
free_half_worker(void *arg)
{
   struct worker *w = arg;
   for (uint32_t i = 0; i < w->iterations; i++)
      vkFreeMemory(w->device, w->memories[i], NULL);
   return 0;
}

int
main(int argc, char **argv)
{
   assert(argc == 2);
   if (strcmp(argv[1], "concurrent-map-free") == 0)
      current_arm = ARM_CONCURRENT_MAP_FREE;
   else if (strcmp(argv[1], "concurrent-submit-teardown") == 0)
      current_arm = ARM_CONCURRENT_SUBMIT_TEARDOWN;
   else if (strcmp(argv[1], "teardown-live-frees") == 0)
      current_arm = ARM_TEARDOWN_LIVE_FREES;
   else if (strcmp(argv[1], "device-loss-classification") == 0)
      current_arm = ARM_DEVICE_LOSS_CLASSIFICATION;
   else if (strcmp(argv[1], "known-bad-lost-queue-submits") == 0)
      current_arm = ARM_KNOWN_BAD_LOST_QUEUE_SUBMITS;
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
   LOAD(vkAllocateMemory) LOAD(vkFreeMemory) LOAD(vkMapMemory)
   LOAD(vkUnmapMemory) LOAD(vkFlushMappedMemoryRanges) LOAD(vkQueueSubmit)
   LOAD(vkQueueWaitIdle) LOAD(vkGetDeviceQueue) LOAD(vkDestroyDevice)
#undef LOAD

   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   switch (current_arm) {
   case ARM_CONCURRENT_MAP_FREE: {
      /* Map establishment and the unmap publication each move the atomic
       * cache-sync counter once, so a lost update under contention shows
       * as a short count. */
      uint64_t syncs_before =
         atomic_load(&native_device->drm.cache_sync_count);
      enum { THREADS = 4, ITERATIONS = 32 };
      struct worker workers[THREADS];
      thrd_t threads[THREADS];
      for (int t = 0; t < THREADS; t++) {
         workers[t] = (struct worker){
            .device = device,
            .iterations = ITERATIONS,
         };
         assert(thrd_create(&threads[t], memory_cycle_worker,
                            &workers[t]) == thrd_success);
      }
      uint32_t total_maps = 0;
      for (int t = 0; t < THREADS; t++) {
         int result = 0;
         assert(thrd_join(threads[t], &result) == thrd_success &&
                result == 0);
         total_maps += workers[t].maps_performed;
      }
      assert(total_maps == THREADS * ITERATIONS);
      assert(atomic_load(&native_device->drm.cache_sync_count) ==
             syncs_before + 2ull * total_maps);
      break;
   }
   case ARM_CONCURRENT_SUBMIT_TEARDOWN: {
      /* Empty submissions and independent memory cycles interleave on one
       * device; every operation still reports success and the counter
       * arithmetic still closes. */
      uint64_t syncs_before =
         atomic_load(&native_device->drm.cache_sync_count);
      struct worker submitter = {
         .device = device,
         .queue = queue,
         .iterations = 32,
      };
      struct worker cyclers[2];
      thrd_t submit_thread, cycle_threads[2];
      assert(thrd_create(&submit_thread, empty_submit_worker, &submitter) ==
             thrd_success);
      for (int t = 0; t < 2; t++) {
         cyclers[t] = (struct worker){
            .device = device,
            .iterations = 32,
         };
         assert(thrd_create(&cycle_threads[t], memory_cycle_worker,
                            &cyclers[t]) == thrd_success);
      }
      int result = 0;
      assert(thrd_join(submit_thread, &result) == thrd_success &&
             result == 0);
      uint32_t total_maps = 0;
      for (int t = 0; t < 2; t++) {
         assert(thrd_join(cycle_threads[t], &result) == thrd_success &&
                result == 0);
         total_maps += cyclers[t].maps_performed;
      }
      assert(total_maps == 64);
      assert(atomic_load(&native_device->drm.cache_sync_count) ==
             syncs_before + 2ull * total_maps);
      break;
   }
   case ARM_TEARDOWN_LIVE_FREES: {
      /* Two threads free live-mapped memory objects concurrently; each
       * free is an implicit unmap with its final publication, so the
       * counter closes at one map plus one free-unmap per object. */
      enum { OBJECTS = 64 };
      static VkDeviceMemory memories[OBJECTS];
      uint64_t syncs_before =
         atomic_load(&native_device->drm.cache_sync_count);
      for (int i = 0; i < OBJECTS; i++) {
         assert(vkAllocateMemory(
                   device,
                   &(VkMemoryAllocateInfo){
                      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                      .allocationSize = 4096,
                      .memoryTypeIndex = 0,
                   },
                   NULL, &memories[i]) == VK_SUCCESS);
         void *map = NULL;
         assert(vkMapMemory(device, memories[i], 0, VK_WHOLE_SIZE, 0,
                            &map) == VK_SUCCESS);
         memset(map, 0x77, 4096);
      }
      struct worker halves[2] = {
         {
            .device = device,
            .memories = &memories[0],
            .iterations = OBJECTS / 2,
         },
         {
            .device = device,
            .memories = &memories[OBJECTS / 2],
            .iterations = OBJECTS / 2,
         },
      };
      thrd_t threads[2];
      for (int t = 0; t < 2; t++)
         assert(thrd_create(&threads[t], free_half_worker, &halves[t]) ==
                thrd_success);
      for (int t = 0; t < 2; t++) {
         int result = 0;
         assert(thrd_join(threads[t], &result) == thrd_success &&
                result == 0);
      }
      assert(atomic_load(&native_device->drm.cache_sync_count) ==
             syncs_before + 2ull * OBJECTS);
      break;
   }
   case ARM_DEVICE_LOSS_CLASSIFICATION:
   case ARM_KNOWN_BAD_LOST_QUEUE_SUBMITS: {
      /* The loss classification: once the queue is lost through the same
       * runtime mechanism the transport-failure path uses, a submission
       * reports VK_ERROR_DEVICE_LOST from the runtime without reaching
       * the transport, while allocation, map, and free stay serviceable
       * so the application can tear down. */
      struct vk_queue *queue_vk = vk_queue_from_handle(queue);
      vk_queue_set_lost(queue_vk, "memory-concurrency harness induced loss");
      saved_ops = native_device->drm.ops;
      counted_ops = *saved_ops;
      counted_ops.command_write_read = counted_command_write_read;
      native_device->drm.ops = &counted_ops;
      atomic_store(&transport_calls, 0);
      VkResult lost = vkQueueSubmit(queue, 0, NULL, VK_NULL_HANDLE);
      native_device->drm.ops = saved_ops;
      if (current_arm == ARM_KNOWN_BAD_LOST_QUEUE_SUBMITS) {
         assert(lost == VK_SUCCESS);
      } else {
         assert(lost == VK_ERROR_DEVICE_LOST);
      }
      assert(atomic_load(&transport_calls) == 0);
      VkDeviceMemory memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(
                device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = 4096,
                   .memoryTypeIndex = 0,
                },
                NULL, &memory) == VK_SUCCESS);
      void *map = NULL;
      assert(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS);
      vkFreeMemory(device, memory, NULL);
      break;
   }
   }

   vkDestroyDevice(device, NULL);
   destroy_instance(instance, NULL);
   printf("memory-concurrency arm %s: OK\n", argv[1]);
   return 0;
}
