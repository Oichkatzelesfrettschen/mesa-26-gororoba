/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the serial status-load cell through the native ICD on the
 * Radeon noop drm-shim: one recorded command buffer, three
 * vkQueueSubmit calls under a declared bound of three, each expected to
 * complete, then a fourth submission the exhausted bound refuses.  The
 * resubmission leg is the class the one-shot cell harnesses cannot
 * exercise: every per-submission queue step -- relocation rebuild,
 * completion reference, deferred draw, evidence retention, arming
 * evaluation, serial accounting -- runs again over the same command
 * buffer.
 */

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "r3v_native.h"
#include "tests/r3v_native_shim_arming.h"

#include "util/mesa-blake3.h"

#define TUPLE_VERTEX_BYTES                                      \
   (R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *                   \
    (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +                \
     R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES))

#define SERIAL_DECLARED_BOUND 3

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

static unsigned failures;

#define CHECK(condition, ...)                   \
   do {                                         \
      if (!(condition)) {                       \
         fprintf(stderr, "FAIL: " __VA_ARGS__); \
         fprintf(stderr, "\n");                 \
         failures++;                            \
      }                                         \
   } while (0)

static bool
same_file(const char *left, const char *right)
{
   struct stat left_status;
   struct stat right_status;
   return stat(left, &left_status) == 0 &&
          stat(right, &right_status) == 0 &&
          left_status.st_dev == right_status.st_dev &&
          left_status.st_ino == right_status.st_ino;
}

static int
attest_shim_provider(void)
{
   const char *expected = getenv("DRM_SHIM_EXPECTED_DSO");
   if (expected == NULL || expected[0] == '\0') {
      fprintf(stderr,
              "REFUSE: DRM_SHIM_EXPECTED_DSO is unset; the harness "
              "cannot attest the interposition provider\n");
      return 1;
   }

   static const char *const interposed[] = {"open", "ioctl"};
   for (unsigned index = 0; index < 2; index++) {
      dlerror();
      void *symbol = dlsym(RTLD_DEFAULT, interposed[index]);
      const char *error = dlerror();
      if (symbol == NULL || error != NULL) {
         fprintf(stderr, "REFUSE: symbol %s is unavailable: %s\n",
                 interposed[index], error != NULL ? error : "unknown");
         return 1;
      }
      Dl_info info;
      memset(&info, 0, sizeof(info));
      if (dladdr(symbol, &info) == 0 || info.dli_fname == NULL) {
         fprintf(stderr, "REFUSE: symbol %s has no provider object\n",
                 interposed[index]);
         return 1;
      }
      if (!same_file(info.dli_fname, expected)) {
         fprintf(stderr,
                 "REFUSE: symbol %s provider %s differs from expected "
                 "shim %s\n",
                 interposed[index], info.dli_fname, expected);
         return 1;
      }
   }
   return 0;
}

#define LOAD_INSTANCE(name) \
   PFN_##name name = (PFN_##name)gipa(instance, #name)
#define LOAD_DEVICE(name) \
   PFN_##name name = (PFN_##name)gdpa(device, #name)

static bool
evidence_file_present(const char *directory, const char *name)
{
   char path[1024];
   int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
   if (length < 0 || (size_t)length >= sizeof(path))
      return false;
   struct stat status;
   return stat(path, &status) == 0;
}

int
main(void)
{
   if (attest_shim_provider() != 0)
      return 3;

   char manifest_path[] = "/tmp/r3v-native-serial-cell-XXXXXX";
   if (mkdtemp(manifest_path) == NULL) {
      fprintf(stderr, "manifest directory creation failed\n");
      return 2;
   }
   if (setenv("R3V_NATIVE_MANIFEST_DIR", manifest_path, 1) != 0) {
      fprintf(stderr, "manifest directory export failed\n");
      rmdir(manifest_path);
      return 2;
   }
   const char *manifest_dir = manifest_path;

   struct r300_r2vb_float2_tuple_ib reference;
   CHECK(r300_r2vb_float2_tuple_reference_emit(&reference) == 0,
         "reference tuple pass emits");
   if (failures != 0)
      return 1;
   char reference_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);

   uint32_t carrier_bytes = 0;
   CHECK(r3v_native_producer_carrier_bytes(&carrier_bytes) == 0,
         "tuple carrier geometry resolves");
   if (failures != 0)
      return 1;

   setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
   setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", reference_digest, 1);
   struct utsname host;
   if (uname(&host) != 0) {
      fprintf(stderr, "uname failed\n");
      return 2;
   }
   setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
   setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
          R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);
   char bound_text[8];
   snprintf(bound_text, sizeof(bound_text), "%d", SERIAL_DECLARED_BOUND);
   setenv("R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS", bound_text, 1);

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   CHECK(result == VK_SUCCESS, "vkCreateInstance: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);
   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;

   uint32_t physical_device_count = 1;
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                       &physical_device);
   CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
            physical_device_count == 1,
         "one shim physical device enumerates: %d count %u", result,
         physical_device_count);
   if (physical_device == VK_NULL_HANDLE)
      return 1;

   const float queue_priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = vkCreateDevice(
      physical_device,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos =
            &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0,
               .queueCount = 1,
               .pQueuePriorities = &queue_priority,
            },
      },
      NULL, &device);
   CHECK(result == VK_SUCCESS, "vkCreateDevice: %d", result);
   if (result != VK_SUCCESS)
      return 1;
   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   native_device->arming_provider = &r3v_native_shim_arming_provider;

   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = carrier_bytes,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory carrier_memory = VK_NULL_HANDLE;
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;

   result = vkAllocateMemory(device, &allocation, NULL, &carrier_memory);
   CHECK(result == VK_SUCCESS && carrier_memory != VK_NULL_HANDLE,
         "carrier vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS || carrier_memory == VK_NULL_HANDLE)
      goto done;

   allocation.allocationSize = TUPLE_VERTEX_BYTES;
   result = vkAllocateMemory(device, &allocation, NULL, &vertex_memory);
   CHECK(result == VK_SUCCESS && vertex_memory != VK_NULL_HANDLE,
         "vertex vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS || vertex_memory == VK_NULL_HANDLE)
      goto done;

   result = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = 0,
      },
      NULL, &pool);
   CHECK(result == VK_SUCCESS && pool != VK_NULL_HANDLE,
         "vkCreateCommandPool: %d", result);
   if (result != VK_SUCCESS || pool == VK_NULL_HANDLE)
      goto done;

   result = vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &command_buffer);
   CHECK(result == VK_SUCCESS && command_buffer != VK_NULL_HANDLE,
         "vkAllocateCommandBuffers: %d", result);
   if (result != VK_SUCCESS || command_buffer == VK_NULL_HANDLE)
      goto done;
   result = vkBeginCommandBuffer(
      command_buffer,
      &(VkCommandBufferBeginInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      goto done;

   result = r3v_native_record_r2vb_status_load_serial(
      command_buffer, carrier_memory, vertex_memory);
   CHECK(result == VK_SUCCESS, "serial cell recording: %d", result);
   if (result != VK_SUCCESS)
      goto done;
   result = vkEndCommandBuffer(command_buffer);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      goto done;

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(command_buffer);
   CHECK(native_cmd->cell_kind ==
            R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL,
         "the recorded cell declares the serial kind");
   CHECK(native_cmd->ib != NULL &&
            native_cmd->ib_size_dwords == reference.ib_size_dwords &&
            memcmp(native_cmd->ib, reference.ib,
                   reference.ib_size_dwords * sizeof(uint32_t)) == 0,
         "the serial cell reuses the reference tuple stream");
   if (native_cmd->ib == NULL ||
       native_cmd->ib_size_dwords != reference.ib_size_dwords)
      goto done;

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);
   CHECK(queue != VK_NULL_HANDLE, "the native device exposes queue family 0");
   if (queue == VK_NULL_HANDLE)
      goto done;

   for (unsigned submission = 0; submission < SERIAL_DECLARED_BOUND;
        submission++) {
      result = vkQueueSubmit(
         queue, 1,
         &(VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command_buffer,
         },
         VK_NULL_HANDLE);
      CHECK(result == VK_SUCCESS,
            "serial submission %u through the shim: %d", submission,
            result);
      CHECK(r3v_native_queue_submission_status(device) ==
               R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "serial submission %u reports completed transport: %s",
            submission,
            r3v_native_queue_status_name(
               r3v_native_queue_submission_status(device)));
      CHECK(evidence_file_present(manifest_dir, "attempt.token"),
            "submission %u leaves the serial token in place", submission);
      char admission_name[64];
      snprintf(admission_name, sizeof(admission_name),
               "submit_manifest_%02u.json", submission);
      CHECK(evidence_file_present(manifest_dir, admission_name),
            "submission %u retains its own submit object", submission);
      CHECK(evidence_file_present(manifest_dir, "ib.bin"),
            "the semantic cell stays retained through submission %u",
            submission);
      if (failures != 0)
         goto done;
   }

   result = vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      },
      VK_NULL_HANDLE);
   CHECK(result == VK_ERROR_DEVICE_LOST,
         "the exhausted serial bound refuses submission %d: %d",
         SERIAL_DECLARED_BOUND, result);
   CHECK(r3v_native_queue_submission_status(device) ==
            R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
         "the exhausted bound reports a refusal: %s",
         r3v_native_queue_status_name(
            r3v_native_queue_submission_status(device)));

done:
   r300_r2vb_float2_tuple_pass_release(&reference);
   if (pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, carrier_memory, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_serial_cell_harness: all checks passed\n");
      return 0;
   }
   return 1;
}
