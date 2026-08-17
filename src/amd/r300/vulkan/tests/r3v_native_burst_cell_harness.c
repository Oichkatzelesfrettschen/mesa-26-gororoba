/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the burst status-load cell through the native ICD on the
 * Radeon noop drm-shim: a four-member burst recorded against a
 * four-row carrier, refused while the declared member count disagrees
 * with the recorded composition, admitted once under the matching
 * declaration, and refused again by the one-shot token.  The
 * mismatch leg runs on its own device and evidence directory because a
 * refused arming still retains the semantic cell, which is the
 * single-write contract the one-shot evidence keeps.
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

#define BURST_DRAWS 4u

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

/* One device, one recorded burst command buffer, one submission; the
 * caller sets the declared draw count and the manifest directory
 * before the call and judges the returned submit result and queue
 * status.
 */
struct burst_leg {
   VkResult submit_result;
   enum r3v_native_queue_status queue_status;
};

static int
run_burst_leg(VkInstance instance,
              PFN_vkVoidFunction (*gipa)(VkInstance, const char *),
              VkPhysicalDevice physical_device,
              const struct r300_r2vb_float2_tuple_burst_ib *reference,
              uint32_t carrier_bytes, bool resubmit,
              const char *manifest_dir, struct burst_leg *out)
{
   int rc = 1;
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;

   const float queue_priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   VkResult result = vkCreateDevice(
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

   VkDeviceMemory carrier_memory = VK_NULL_HANDLE;
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;

   VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = carrier_bytes,
      .memoryTypeIndex = 0,
   };
   result = vkAllocateMemory(device, &allocation, NULL, &carrier_memory);
   CHECK(result == VK_SUCCESS, "carrier vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS)
      goto done;
   allocation.allocationSize = TUPLE_VERTEX_BYTES;
   result = vkAllocateMemory(device, &allocation, NULL, &vertex_memory);
   CHECK(result == VK_SUCCESS, "vertex vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS)
      goto done;

   result = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool);
   CHECK(result == VK_SUCCESS, "vkCreateCommandPool: %d", result);
   if (result != VK_SUCCESS)
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
   CHECK(result == VK_SUCCESS, "vkAllocateCommandBuffers: %d", result);
   if (result != VK_SUCCESS)
      goto done;
   result = vkBeginCommandBuffer(
      command_buffer,
      &(VkCommandBufferBeginInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      goto done;

   result = r3v_native_record_r2vb_status_load_burst(
      command_buffer, carrier_memory, vertex_memory, BURST_DRAWS);
   CHECK(result == VK_SUCCESS, "burst cell recording: %d", result);
   if (result != VK_SUCCESS)
      goto done;
   result = vkEndCommandBuffer(command_buffer);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      goto done;

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(command_buffer);
   CHECK(native_cmd->cell_kind ==
            R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST,
         "the recorded cell declares the burst kind");
   CHECK(native_cmd->burst_draws == BURST_DRAWS,
         "the recorded cell carries its member count");
   CHECK(native_cmd->ib != NULL &&
            native_cmd->ib_size_dwords == reference->ib_size_dwords &&
            memcmp(native_cmd->ib, reference->ib,
                   reference->ib_size_dwords * sizeof(uint32_t)) == 0,
         "the burst cell installs the composed reference stream");
   if (failures != 0)
      goto done;

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   out->submit_result = vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      },
      VK_NULL_HANDLE);
   out->queue_status = r3v_native_queue_submission_status(device);

   if (resubmit && out->submit_result == VK_SUCCESS) {
      CHECK(evidence_file_present(manifest_dir, "attempt.token"),
            "the admitted burst leaves the one-shot token");
      CHECK(evidence_file_present(manifest_dir, "ib.bin") &&
               evidence_file_present(manifest_dir, "submit_manifest.json"),
            "the admitted burst retains the semantic cell and its "
            "submit object");
      VkResult again = vkQueueSubmit(
         queue, 1,
         &(VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command_buffer,
         },
         VK_NULL_HANDLE);
      CHECK(again == VK_ERROR_DEVICE_LOST,
            "the one-shot token refuses a second burst submission: %d",
            again);
      CHECK(r3v_native_queue_submission_status(device) ==
               R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
            "the second submission reports a refusal: %s",
            r3v_native_queue_status_name(
               r3v_native_queue_submission_status(device)));
   }
   rc = 0;

done:
   if (pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, carrier_memory, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyDevice(device, NULL);
   return rc;
}

int
main(void)
{
   if (attest_shim_provider() != 0)
      return 3;

   struct r300_r2vb_float2_tuple_burst_ib reference;
   CHECK(r300_r2vb_float2_tuple_burst_reference_emit(BURST_DRAWS,
                                                     &reference) == 0,
         "reference burst emits");
   if (failures != 0)
      return 1;
   char reference_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);

   uint32_t carrier_bytes = 0;
   CHECK(r3v_native_burst_carrier_bytes(BURST_DRAWS, &carrier_bytes) == 0,
         "burst carrier geometry resolves");
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
   LOAD_INSTANCE(vkDestroyInstance);

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

   /* Mismatch leg: the declared count disagrees with the recorded
    * composition, so the arming refuses before the ioctl and before any
    * token exists.
    */
   char mismatch_dir[] = "/tmp/r3v-native-burst-mismatch-XXXXXX";
   if (mkdtemp(mismatch_dir) == NULL) {
      fprintf(stderr, "mismatch evidence directory creation failed\n");
      return 2;
   }
   setenv("R3V_NATIVE_MANIFEST_DIR", mismatch_dir, 1);
   setenv("R3V_NATIVE_AUTHORIZED_BURST_DRAWS", "8", 1);
   struct burst_leg mismatch = { 0 };
   if (run_burst_leg(instance, gipa, physical_device, &reference,
                     carrier_bytes, false, mismatch_dir, &mismatch) != 0)
      goto done;
   CHECK(mismatch.submit_result == VK_ERROR_DEVICE_LOST &&
            mismatch.queue_status ==
               R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
         "a declared count of 8 over a 4-member composition refuses: "
         "%d %s",
         mismatch.submit_result,
         r3v_native_queue_status_name(mismatch.queue_status));
   CHECK(!evidence_file_present(mismatch_dir, "attempt.token"),
         "the refused mismatch writes no attempt token");

   /* Admission leg: matching declaration, one admitted submission, then
    * the one-shot token refuses the resubmission.
    */
   char admit_dir[] = "/tmp/r3v-native-burst-cell-XXXXXX";
   if (mkdtemp(admit_dir) == NULL) {
      fprintf(stderr, "evidence directory creation failed\n");
      return 2;
   }
   setenv("R3V_NATIVE_MANIFEST_DIR", admit_dir, 1);
   setenv("R3V_NATIVE_AUTHORIZED_BURST_DRAWS", "4", 1);
   struct burst_leg admit = { 0 };
   if (run_burst_leg(instance, gipa, physical_device, &reference,
                     carrier_bytes, true, admit_dir, &admit) != 0)
      goto done;
   CHECK(admit.submit_result == VK_SUCCESS &&
            admit.queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
         "the matching declaration admits the burst once: %d %s",
         admit.submit_result,
         r3v_native_queue_status_name(admit.queue_status));

done:
   r300_r2vb_float2_tuple_burst_release(&reference);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_burst_cell_harness: all checks passed\n");
      return 0;
   }
   return 1;
}
