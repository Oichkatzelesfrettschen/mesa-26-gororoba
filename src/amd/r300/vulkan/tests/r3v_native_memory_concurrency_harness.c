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
#include "r3v_measurement_session.h"
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
   ARM_ALLOCATION_GENERATIONS,
   ARM_ALLOCATION_FAILURE_STAMPS_NOTHING,
   ARM_DEVICES_COUNT_INDEPENDENTLY,
   ARM_GATE_REFRESH_PRESERVES_THE_SESSION,
   /* Known-bad: a lost queue admitting a submission -- the loss
    * classification this harness proves must make this arm fail. */
   ARM_KNOWN_BAD_LOST_QUEUE_SUBMITS,
   /* Known-bad: two allocations sharing one generation -- the counter
    * this harness proves must make this arm fail. */
   ARM_KNOWN_BAD_REPEATED_GENERATION,
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

/* The GEM create the allocation-failure arm refuses.  Every other
 * request passes through, so the failure is the allocation's alone. */
static _Atomic bool refuse_gem_create;

static int
refusing_command_write_read(int fd, unsigned long request, void *data,
                            unsigned size)
{
   if (request == DRM_RADEON_GEM_CREATE && atomic_load(&refuse_gem_create))
      return -ENOMEM;
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
static PFN_vkCreateBuffer vkCreateBuffer;
static PFN_vkDestroyBuffer vkDestroyBuffer;
static PFN_vkBindBufferMemory vkBindBufferMemory;

struct worker {
   VkDevice device;
   VkQueue queue;
   VkDeviceMemory *memories;
   uint32_t iterations;
   uint32_t maps_performed;
   /* One generation per iteration, written by this thread alone. */
   uint64_t *generations;
};

/* Allocates `iterations` objects, records the generation each carries,
 * and frees them.  The objects are this thread's alone; the cross-thread
 * state is the device's one allocation counter. */
static int
generation_worker(void *arg)
{
   struct worker *w = arg;
   for (uint32_t i = 0; i < w->iterations; i++) {
      VkDeviceMemory memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(
                w->device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = 4096,
                   .memoryTypeIndex = 0,
                },
                NULL, &memory) == VK_SUCCESS);
      w->generations[i] = r3v_native_memory_from_handle(memory)->generation;
      vkFreeMemory(w->device, memory, NULL);
   }
   return 0;
}

static int
compare_u64(const void *a, const void *b)
{
   const uint64_t x = *(const uint64_t *)a;
   const uint64_t y = *(const uint64_t *)b;
   return x < y ? -1 : x > y ? 1 : 0;
}

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
   else if (strcmp(argv[1], "allocation-generations") == 0)
      current_arm = ARM_ALLOCATION_GENERATIONS;
   else if (strcmp(argv[1], "allocation-failure-stamps-nothing") == 0)
      current_arm = ARM_ALLOCATION_FAILURE_STAMPS_NOTHING;
   else if (strcmp(argv[1], "devices-count-independently") == 0)
      current_arm = ARM_DEVICES_COUNT_INDEPENDENTLY;
   else if (strcmp(argv[1], "gate-refresh-preserves-the-session") == 0)
      current_arm = ARM_GATE_REFRESH_PRESERVES_THE_SESSION;
   else if (strcmp(argv[1], "known-bad-lost-queue-submits") == 0)
      current_arm = ARM_KNOWN_BAD_LOST_QUEUE_SUBMITS;
   else if (strcmp(argv[1], "known-bad-repeated-generation") == 0)
      current_arm = ARM_KNOWN_BAD_REPEATED_GENERATION;
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
   LOAD(vkCreateBuffer) LOAD(vkDestroyBuffer) LOAD(vkBindBufferMemory)
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
   case ARM_ALLOCATION_GENERATIONS:
   case ARM_KNOWN_BAD_REPEATED_GENERATION: {
      /* Every published allocation carries a generation of its own.
       * Sixteen threads allocate and free with no lock between them, and
       * the device's counter is what makes the values distinct.  The
       * known-bad arm collapses the recorded values to prove the check
       * sees a repeat.
       *
       * What this arm proves is that the shipped counter hands out
       * distinct nonzero values under real interleaving.  It is not a
       * calibration of the atomic: replacing p_atomic_inc_return with a
       * plain increment loses an update in roughly one run in fifteen at
       * this width on a twelve-core host, so the race is real and the
       * observation is too rare to gate on.  The atomic stands on the
       * rule rather than on this measurement -- vkAllocateMemory carries
       * no external-synchronization requirement, so nothing orders two
       * threads allocating at once.
       *
       * Freeing is not unstamping.  The threads free every object as they
       * go, so the handles are recycled throughout, and the generations
       * still never repeat and never fall. */
      enum { THREADS = 16, ITERATIONS = 512 };
      static uint64_t generations[THREADS][ITERATIONS];
      struct worker workers[THREADS];
      thrd_t threads[THREADS];
      const uint64_t counter_before =
         native_device->allocation_generation_counter;
      for (int t = 0; t < THREADS; t++) {
         workers[t] = (struct worker){
            .device = device,
            .iterations = ITERATIONS,
            .generations = generations[t],
         };
         assert(thrd_create(&threads[t], generation_worker,
                            &workers[t]) == thrd_success);
      }
      for (int t = 0; t < THREADS; t++) {
         int result = 0;
         assert(thrd_join(threads[t], &result) == thrd_success &&
                result == 0);
      }

      static uint64_t flat[THREADS * ITERATIONS];
      uint32_t n = 0;
      for (int t = 0; t < THREADS; t++) {
         for (int i = 0; i < ITERATIONS; i++) {
            flat[n++] = current_arm == ARM_KNOWN_BAD_REPEATED_GENERATION
                           ? 1u
                           : generations[t][i];
         }
      }
      qsort(flat, n, sizeof(flat[0]), compare_u64);
      for (uint32_t i = 0; i < n; i++) {
         /* Zero is no allocation, so a published one never carries it. */
         assert(flat[i] != 0);
         if (i > 0)
            assert(flat[i] != flat[i - 1]);
      }
      /* The counter advanced once per allocation and never fell, so no
       * free returned a generation to it. */
      assert(native_device->allocation_generation_counter ==
             counter_before + n);

      /* A recycled GEM handle carries a new generation.  The allocation
       * below reuses a handle the loop above closed on most runs; where
       * the shim hands out a fresh one instead, the generation is new
       * either way, which is the property under test. */
      VkDeviceMemory recycled = VK_NULL_HANDLE;
      assert(vkAllocateMemory(
                device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = 4096,
                   .memoryTypeIndex = 0,
                },
                NULL, &recycled) == VK_SUCCESS);
      const struct r3v_native_memory *reused =
         r3v_native_memory_from_handle(recycled);
      assert(reused->generation > flat[n - 1]);
      vkFreeMemory(device, recycled, NULL);
      break;
   }
   case ARM_ALLOCATION_FAILURE_STAMPS_NOTHING: {
      /* A refused GEM create publishes no handle and spends no
       * generation, so the next allocation to succeed carries the value
       * the failed one would have taken. */
      saved_ops = native_device->drm.ops;
      counted_ops = *saved_ops;
      counted_ops.command_write_read = refusing_command_write_read;
      native_device->drm.ops = &counted_ops;

      const uint64_t counter_before =
         native_device->allocation_generation_counter;
      atomic_store(&refuse_gem_create, true);
      VkDeviceMemory refused = VK_NULL_HANDLE;
      const VkResult failed = vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = 4096,
            .memoryTypeIndex = 0,
         },
         NULL, &refused);
      atomic_store(&refuse_gem_create, false);
      native_device->drm.ops = saved_ops;

      assert(failed == VK_ERROR_OUT_OF_DEVICE_MEMORY);
      assert(refused == VK_NULL_HANDLE);
      assert(native_device->allocation_generation_counter == counter_before);

      VkDeviceMemory allocated = VK_NULL_HANDLE;
      assert(vkAllocateMemory(
                device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = 4096,
                   .memoryTypeIndex = 0,
                },
                NULL, &allocated) == VK_SUCCESS);
      assert(r3v_native_memory_from_handle(allocated)->generation ==
             counter_before + 1u);
      vkFreeMemory(device, allocated, NULL);
      break;
   }
   case ARM_DEVICES_COUNT_INDEPENDENTLY: {
      /* Each device counts its own allocations, so both first
       * allocations carry generation one.  Both devices share the
       * physical device's render-node file descriptor and so one GEM
       * handle table, which is why an equal generation across two devices
       * names no relation between the objects: a VkDeviceMemory binds
       * only to buffers of the device that allocated it, so a comparison
       * only ever runs inside one device's counting. */
      VkDevice second = VK_NULL_HANDLE;
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
                NULL, &second) == VK_SUCCESS);
      struct r3v_native_device *second_native =
         r3v_native_device_from_handle(second);
      assert(second_native != native_device);
      assert(second_native->drm.fd == native_device->drm.fd);
      assert(second_native->allocation_generation_counter == 0u);

      VkDeviceMemory first_on_each[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
      VkDevice devices[2] = { device, second };
      for (int d = 0; d < 2; d++) {
         assert(vkAllocateMemory(
                   devices[d],
                   &(VkMemoryAllocateInfo){
                      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                      .allocationSize = 4096,
                      .memoryTypeIndex = 0,
                   },
                   NULL, &first_on_each[d]) == VK_SUCCESS);
      }
      assert(r3v_native_memory_from_handle(first_on_each[0])->generation ==
             1u);
      assert(r3v_native_memory_from_handle(first_on_each[1])->generation ==
             1u);

      /* One device's allocations move its counter alone. */
      for (int i = 0; i < 4; i++) {
         VkDeviceMemory extra = VK_NULL_HANDLE;
         assert(vkAllocateMemory(
                   device,
                   &(VkMemoryAllocateInfo){
                      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                      .allocationSize = 4096,
                      .memoryTypeIndex = 0,
                   },
                   NULL, &extra) == VK_SUCCESS);
         vkFreeMemory(device, extra, NULL);
      }
      assert(native_device->allocation_generation_counter == 5u);
      assert(second_native->allocation_generation_counter == 1u);

      /* Two devices assign one generation to two different objects over
       * one GEM handle table, so a buffer of one device bound to memory
       * of the other would carry a handle and generation pair its own
       * device never stamped.  The bind refuses that, and refuses the
       * mirrored case, so the pair a session compares is always one its
       * own device produced. */
      VkBuffer cross = VK_NULL_HANDLE;
      assert(vkCreateBuffer(device,
                            &(VkBufferCreateInfo){
                               .sType =
                                  VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 4096,
                               .usage =
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            },
                            NULL, &cross) == VK_SUCCESS);
      assert(vkBindBufferMemory(device, cross, first_on_each[1], 0) !=
             VK_SUCCESS);
      /* The same buffer still binds to its own device's memory, so the
       * refusal above is the device mismatch and not the buffer. */
      assert(vkBindBufferMemory(device, cross, first_on_each[0], 0) ==
             VK_SUCCESS);
      vkDestroyBuffer(device, cross, NULL);

      vkFreeMemory(device, first_on_each[0], NULL);
      vkFreeMemory(second, first_on_each[1], NULL);
      vkDestroyDevice(second, NULL);
      break;
   }
   case ARM_GATE_REFRESH_PRESERVES_THE_SESSION: {
      /* r3v_native_device_refresh_delivery_gates re-reads the environment
       * and runs many times over one device.  Its name invites reuse as
       * the place a declaration is read, and a session opened or reset
       * there would clear the bindings and restore the allowance the
       * previous pass spent.
       *
       * The session has to be live for a reset to be visible: an
       * inactive one is already every byte a reset would write.  The
       * device's session is opened here over a declaration the epoch and
       * board checks never see, because the shim sits on no board, and
       * part of its allowance is spent before the refreshes run. */
      static const char declaration[] =
         "schema = r3v-measurement-session-v1\n"
         "session_nonce = 7f3a19c2\n"
         "platform = vostro1000_rs485m_5974\n"
         "route = rb2d_const_fill_v2\n"
         "pci_vendor_id = 0x1002\n"
         "pci_device_id = 0x5974\n"
         "kernel_release = 7.1.8-1-cachyos\n"
         "module_srcversion = 729892A3F3530EB12B8D842\n"
         "allocation_bytes = 8388608\n"
         "buffer_bytes = 8388608\n"
         "binding_offset = 0\n"
         "memory_property_flags = 0x2\n"
         "buffer_usage = 0x2\n"
         "write_domain = 0x2\n"
         "max_total_submissions = 72\n"
         "completion_timeout_ns = 10000000000\n"
         "case = 1, 0, 4096, 287454020, 2, 32\n";
      static const struct r3v_measurement_digest digest = {
         "2222222222222222222222222222222222222222222222222222222222222222"
      };
      struct r3v_measurement_manifest manifest;
      const char *reason = NULL;
      assert(r3v_measurement_manifest_parse(declaration,
                                            sizeof(declaration) - 1,
                                            &manifest, &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
      r3v_measurement_session_init(&native_device->measurement_session);
      assert(r3v_measurement_session_open(
                &native_device->measurement_session, &manifest, &digest,
                &reason) == R3V_MEASUREMENT_SESSION_ADMITTED);
      assert(r3v_measurement_session_bind(
                &native_device->measurement_session, 0u, 0u, 4096u,
                287454020u, 11u, 401u, &digest, &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
      for (int i = 0; i < 5; i++) {
         assert(r3v_measurement_session_consume(
                   &native_device->measurement_session, 0u, 0u, 4096u,
                   287454020u, 11u, 401u, &digest, &reason) ==
                R3V_MEASUREMENT_SESSION_ADMITTED);
      }
      const struct r3v_measurement_session before =
         native_device->measurement_session;
      assert(before.active && !before.closed);
      assert(before.consumed_submissions == 5u);
      assert(before.remaining_submissions == 34u - 5u);

      for (int i = 0; i < 8; i++)
         r3v_native_device_refresh_delivery_gates(native_device);

      const struct r3v_measurement_session *after =
         &native_device->measurement_session;
      assert(after->active == before.active);
      assert(after->closed == before.closed);
      assert(after->consumed_submissions == before.consumed_submissions);
      assert(after->remaining_submissions == before.remaining_submissions);
      assert(strcmp(after->manifest_digest.hex,
                    before.manifest_digest.hex) == 0);
      assert(memcmp(after->bindings, before.bindings,
                    sizeof(before.bindings)) == 0);
      /* Every remaining field by name.  A memcmp over the whole struct
       * would compare padding bytes, which a struct assignment need not
       * carry across, so the byte-identity it appears to assert is not
       * one the language gives. */
      assert(memcmp(after->closed_reason, before.closed_reason,
                    sizeof(before.closed_reason)) == 0);
      assert(memcmp(&after->manifest, &before.manifest,
                    sizeof(before.manifest)) == 0);

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
