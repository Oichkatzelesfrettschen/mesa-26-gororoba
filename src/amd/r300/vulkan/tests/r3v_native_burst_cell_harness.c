/*
 * SPDX-License-Identifier: MIT
 *
 * Drives burst admission and prepared-submission lifetime through the native
 * ICD on the Radeon noop drm-shim. Each leg owns one device and evidence
 * directory because prepare consumes the one-shot authorization before any
 * submit ioctl.
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

#define FLOAT4_MODEL_VERTEX_BYTES                              \
   (R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *                   \
    (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +                \
     R300_R2VB_FLOAT4_MODEL_STRIDE_BYTES))

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

enum burst_lifetime_mode {
   BURST_LIFETIME_NONE,
   BURST_LIFETIME_COMMIT,
   BURST_LIFETIME_RESET,
   BURST_LIFETIME_DIFFERENT_COMMAND_BUFFER,
   BURST_LIFETIME_SUBMIT_SHAPE,
   BURST_LIFETIME_TRACE_REFUSAL,
   BURST_LIFETIME_TEARDOWN,
   BURST_LIFETIME_KNOWN_BAD_RESET,
   BURST_LIFETIME_KNOWN_BAD_BINDING,
   BURST_LIFETIME_KNOWN_BAD_TEARDOWN,
};

struct lifetime_trace {
   unsigned event_count;
   bool refuse_ioctl_enter;
};

static int
lifetime_trace_emit(void *ctx, enum r3v_native_submission_trace_event event)
{
   struct lifetime_trace *trace = ctx;
   trace->event_count++;
   return trace->refuse_ioctl_enter &&
          event == R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_ENTER
             ? -1
             : 0;
}

/* The completion BO prepare creates is a 4-byte GEM object the transport
 * waits on and never maps, so the shim's object census is the one that
 * sees it; -1 reports a shim without the counter and fails the verdict.
 */
static int
live_bos(void)
{
   int (*counter)(void) =
      (int (*)(void))dlsym(RTLD_DEFAULT, "drm_shim_test_live_bos");
   return counter != NULL ? counter() : -1;
}

static bool
prepared_state_released(const struct r3v_native_prepared_submission *prepared)
{
   return !prepared->valid && prepared->cmd_buffer == NULL &&
          prepared->reference_indices == NULL && prepared->relocs.count == 0;
}

static int
run_burst_leg(VkInstance instance,
              PFN_vkVoidFunction (*gipa)(VkInstance, const char *),
              VkPhysicalDevice physical_device,
              const struct r300_r2vb_float2_tuple_burst_ib *reference,
              uint32_t carrier_bytes, bool resubmit,
              enum burst_lifetime_mode lifetime_mode, bool model_float4,
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
   LOAD_DEVICE(vkResetCommandBuffer);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   VkDeviceMemory carrier_memory = VK_NULL_HANDLE;
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   VkCommandBuffer other_command_buffer = VK_NULL_HANDLE;

   VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = carrier_bytes,
      .memoryTypeIndex = 0,
   };
   result = vkAllocateMemory(device, &allocation, NULL, &carrier_memory);
   CHECK(result == VK_SUCCESS, "carrier vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS)
      goto done;
   allocation.allocationSize =
      model_float4 ? FLOAT4_MODEL_VERTEX_BYTES : TUPLE_VERTEX_BYTES;
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
   const bool needs_other_command_buffer =
      lifetime_mode == BURST_LIFETIME_DIFFERENT_COMMAND_BUFFER ||
      lifetime_mode == BURST_LIFETIME_SUBMIT_SHAPE ||
      lifetime_mode == BURST_LIFETIME_KNOWN_BAD_BINDING;
   result = vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = needs_other_command_buffer ? 2 : 1,
      },
      command_buffers);
   CHECK(result == VK_SUCCESS, "vkAllocateCommandBuffers: %d", result);
   if (result != VK_SUCCESS)
      goto done;
   command_buffer = command_buffers[0];
   other_command_buffer = command_buffers[1];
   result = vkBeginCommandBuffer(
      command_buffer,
      &(VkCommandBufferBeginInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      goto done;

   if (needs_other_command_buffer) {
      result = vkBeginCommandBuffer(
         other_command_buffer,
         &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         });
      CHECK(result == VK_SUCCESS, "other vkBeginCommandBuffer: %d", result);
      if (result == VK_SUCCESS)
         result = vkEndCommandBuffer(other_command_buffer);
      CHECK(result == VK_SUCCESS, "other vkEndCommandBuffer: %d", result);
      if (result != VK_SUCCESS)
         goto done;
   }

   result = model_float4
               ? r3v_native_record_r2vb_status_load_burst_float4_model(
                    command_buffer, carrier_memory, vertex_memory,
                    BURST_DRAWS)
               : r3v_native_record_r2vb_status_load_burst(
                    command_buffer, carrier_memory, vertex_memory,
                    BURST_DRAWS);
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

   struct lifetime_trace trace = {0};
   if (lifetime_mode != BURST_LIFETIME_NONE) {
      native_device->submission_trace = (struct r3v_native_submission_trace){
         .ctx = &trace,
         .emit = lifetime_trace_emit,
      };
   }

   int live_before_prepare = -1;
   if (lifetime_mode == BURST_LIFETIME_TEARDOWN ||
       lifetime_mode == BURST_LIFETIME_KNOWN_BAD_TEARDOWN)
      live_before_prepare = live_bos();

   if (lifetime_mode != BURST_LIFETIME_NONE) {
      /* The prepared lane consumes authorization ahead of the submit:
       * the token and both evidence groups exist before any ioctl, and
       * the following vkQueueSubmit carries the transport tail alone.
       */
      VkResult prepared =
         r3v_native_queue_prepare_submission(device, command_buffer);
      CHECK(prepared == VK_SUCCESS, "transport preparation: %d", prepared);
      CHECK(evidence_file_present(manifest_dir, "attempt.token"),
            "prepare writes the one-shot token before any submit");
      CHECK(evidence_file_present(manifest_dir, "ib.bin") &&
               evidence_file_present(manifest_dir, "submit_manifest.json"),
            "prepare retains the semantic cell and submit object before "
            "any submit");
      CHECK(r3v_native_queue_prepare_submission(device, command_buffer) ==
               VK_ERROR_DEVICE_LOST,
            "a second prepare refuses while one is pending");
      CHECK(native_device->prepared.valid &&
               native_device->prepared.cmd_buffer == native_cmd &&
               native_device->prepared.reference_indices != NULL &&
               native_device->prepared.relocs.count ==
                  native_cmd->reference_count + 1,
            "the refused second prepare preserves the first prepared state");
      if (failures != 0)
         goto done;
      if (lifetime_mode == BURST_LIFETIME_RESET ||
          lifetime_mode == BURST_LIFETIME_KNOWN_BAD_RESET) {
         /* The prepared CS names this buffer's IB storage rather than a
          * copy of it, and the commit guard admits on command-buffer
          * pointer equality, which a reset preserves.  Releasing the IB
          * therefore retires the prepared transport, so no ioctl can
          * reach freed dwords.
          */
         if (lifetime_mode == BURST_LIFETIME_KNOWN_BAD_RESET)
            native_device->prepared.cmd_buffer = NULL;
         VkResult reset = vkResetCommandBuffer(command_buffer, 0);
         CHECK(reset == VK_SUCCESS, "reset after prepare: %d", reset);
         CHECK(prepared_state_released(&native_device->prepared),
               "the command-buffer reset retires the prepared transport");
         CHECK(trace.event_count == 0,
               "reset after prepare reaches no submission ioctl");
         if (native_device->prepared.valid) {
            native_device->prepared.cmd_buffer = native_cmd;
            r3v_native_prepared_release(native_device);
         }
         rc = 0;
         goto done;
      }

      if (lifetime_mode == BURST_LIFETIME_DIFFERENT_COMMAND_BUFFER ||
          lifetime_mode == BURST_LIFETIME_KNOWN_BAD_BINDING) {
         if (lifetime_mode == BURST_LIFETIME_KNOWN_BAD_BINDING) {
            native_device->prepared.cmd_buffer =
               r3v_native_cmd_buffer_from_handle(other_command_buffer);
         }
         out->submit_result = vkQueueSubmit(
            queue, 1,
            &(VkSubmitInfo){
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
               .commandBufferCount = 1,
               .pCommandBuffers = &other_command_buffer,
            },
            VK_NULL_HANDLE);
         out->queue_status = r3v_native_queue_submission_status(device);
         CHECK(out->submit_result == VK_ERROR_DEVICE_LOST &&
                  out->queue_status ==
                     R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
               "a different command buffer refuses the prepared transport");
         CHECK(prepared_state_released(&native_device->prepared),
               "the different-buffer refusal releases prepared state");
         CHECK(trace.event_count == 0,
               "the different-buffer refusal reaches no submission ioctl");
         rc = 0;
         goto done;
      }

      if (lifetime_mode == BURST_LIFETIME_SUBMIT_SHAPE) {
         VkCommandBuffer shape[2] = {command_buffer, other_command_buffer};
         out->submit_result = vkQueueSubmit(
            queue, 1,
            &(VkSubmitInfo){
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
               .commandBufferCount = 2,
               .pCommandBuffers = shape,
            },
            VK_NULL_HANDLE);
         out->queue_status = r3v_native_queue_submission_status(device);
         CHECK(out->submit_result == VK_ERROR_DEVICE_LOST &&
                  out->queue_status ==
                     R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
               "a different submit shape refuses the prepared transport");
         CHECK(prepared_state_released(&native_device->prepared),
               "the submit-shape refusal releases prepared state");
         CHECK(trace.event_count == 0,
               "the submit-shape refusal reaches no submission ioctl");
         rc = 0;
         goto done;
      }

      if (lifetime_mode == BURST_LIFETIME_TRACE_REFUSAL)
         trace.refuse_ioctl_enter = true;

      if (lifetime_mode == BURST_LIFETIME_TEARDOWN ||
          lifetime_mode == BURST_LIFETIME_KNOWN_BAD_TEARDOWN) {
         const int live_prepared = live_bos();
         CHECK(live_before_prepare >= 0 &&
                  live_prepared == live_before_prepare + 1,
               "prepare adds one completion BO to the shim object census");
         if (lifetime_mode == BURST_LIFETIME_KNOWN_BAD_TEARDOWN)
            native_device->prepared.valid = false;
         /* This internal harness deliberately hands teardown a live command
          * pool and memory objects.  The verdict is narrower than Vulkan
          * child-object lifetime: only the completion BO added by prepare is
          * compared with the pre-prepare shim object census.
          */
         vkDestroyDevice(device, NULL);
         device = VK_NULL_HANDLE;
         pool = VK_NULL_HANDLE;
         carrier_memory = VK_NULL_HANDLE;
         vertex_memory = VK_NULL_HANDLE;
         CHECK(live_bos() <= live_before_prepare,
               "device teardown releases the uncommitted prepared BO");
         CHECK(trace.event_count == 0,
               "device teardown reaches no submission ioctl");
         rc = 0;
         goto done;
      }
   }

   out->submit_result = vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      },
      VK_NULL_HANDLE);
   out->queue_status = r3v_native_queue_submission_status(device);

   if (lifetime_mode == BURST_LIFETIME_COMMIT) {
      CHECK(out->submit_result == VK_SUCCESS &&
               out->queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "the matching prepared transport commits once");
      CHECK(prepared_state_released(&native_device->prepared),
            "the matching commit releases prepared state");
      CHECK(trace.event_count == 4,
            "the matching commit records ioctl and completion boundaries");
   } else if (lifetime_mode == BURST_LIFETIME_TRACE_REFUSAL) {
      CHECK(out->submit_result == VK_ERROR_DEVICE_LOST &&
               out->queue_status ==
                  R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
            "a trace refusal stops before the submission ioctl");
      CHECK(prepared_state_released(&native_device->prepared),
            "the pre-ioctl refusal releases prepared state");
      CHECK(trace.event_count == 1,
            "the pre-ioctl refusal records only the enter boundary");
   }

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
   if (device != VK_NULL_HANDLE) {
      if (pool != VK_NULL_HANDLE)
         vkDestroyCommandPool(device, pool, NULL);
      vkFreeMemory(device, carrier_memory, NULL);
      vkFreeMemory(device, vertex_memory, NULL);
      vkDestroyDevice(device, NULL);
   }
   return rc;
}

static bool
mode_selected(const char *selected, const char *mode)
{
   return selected == NULL || strcmp(selected, mode) == 0;
}

static int
run_lifetime_leg(VkInstance instance,
                 PFN_vkVoidFunction (*gipa)(VkInstance, const char *),
                 VkPhysicalDevice physical_device,
                 const struct r300_r2vb_float2_tuple_burst_ib *reference,
                 uint32_t carrier_bytes, enum burst_lifetime_mode mode,
                 const char *directory_stem)
{
   char directory[128];
   int length = snprintf(directory, sizeof(directory),
                         "/tmp/r3v-native-%s-XXXXXX", directory_stem);
   if (length < 0 || (size_t)length >= sizeof(directory) ||
       mkdtemp(directory) == NULL) {
      fprintf(stderr, "%s evidence directory creation failed\n",
              directory_stem);
      return 2;
   }

   setenv("R3V_NATIVE_MANIFEST_DIR", directory, 1);
   struct burst_leg leg = {0};
   return run_burst_leg(instance, gipa, physical_device, reference,
                        carrier_bytes, false, mode, false, directory, &leg);
}

int
main(int argc, char **argv)
{
   const char *selected = argc == 2 ? argv[1] : NULL;
   if (argc > 2) {
      fprintf(stderr, "usage: %s [admission|commit|reset|different-buffer|"
                      "submit-shape|trace-refusal|teardown|"
                      "known-bad-reset|known-bad-binding|"
                      "known-bad-teardown]\n",
              argv[0]);
      return 2;
   }
   if (selected != NULL && strcmp(selected, "admission") != 0 &&
       strcmp(selected, "commit") != 0 && strcmp(selected, "reset") != 0 &&
       strcmp(selected, "different-buffer") != 0 &&
       strcmp(selected, "submit-shape") != 0 &&
       strcmp(selected, "trace-refusal") != 0 &&
       strcmp(selected, "teardown") != 0 &&
       strcmp(selected, "known-bad-reset") != 0 &&
       strcmp(selected, "known-bad-binding") != 0 &&
       strcmp(selected, "known-bad-teardown") != 0) {
      fprintf(stderr, "unknown mode: %s\n", selected);
      return 2;
   }

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

   if (mode_selected(selected, "admission")) {
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
      struct burst_leg mismatch = {0};
      if (run_burst_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, false, BURST_LIFETIME_NONE, false,
                        mismatch_dir, &mismatch) != 0)
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
      struct burst_leg admit = {0};
      if (run_burst_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, true, BURST_LIFETIME_NONE, false,
                        admit_dir, &admit) != 0)
         goto done;
      CHECK(admit.submit_result == VK_SUCCESS &&
               admit.queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "the matching declaration admits the burst once: %d %s",
            admit.submit_result,
            r3v_native_queue_status_name(admit.queue_status));
   }

   /* Every prepared leg declares the four-member burst; a single selected
    * mode reaches here without the admission leg's declaration.
    */
   setenv("R3V_NATIVE_AUTHORIZED_BURST_DRAWS", "4", 1);

   /* Prepared leg: the transport prepares ahead of the submit, evidence
    * and token land before any ioctl, and the commit completes; the
    * resubmission then refuses on the token the prepare wrote.
    */
   if (mode_selected(selected, "commit")) {
      char prepared_dir[] = "/tmp/r3v-native-burst-prepared-XXXXXX";
      if (mkdtemp(prepared_dir) == NULL) {
         fprintf(stderr, "prepared evidence directory creation failed\n");
         return 2;
      }
      setenv("R3V_NATIVE_MANIFEST_DIR", prepared_dir, 1);
      struct burst_leg prepared = {0};
      if (run_burst_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, true, BURST_LIFETIME_COMMIT, false,
                        prepared_dir, &prepared) != 0)
         goto done;
      CHECK(prepared.submit_result == VK_SUCCESS &&
               prepared.queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "the prepared transport commits once: %d %s",
            prepared.submit_result,
            r3v_native_queue_status_name(prepared.queue_status));
   }

   /* Reset-after-prepare leg: the IB release retires the prepared
    * transport, so a buffer reset between prepare and submit leaves no
    * CS aimed at freed storage.
    */
   if (mode_selected(selected, "reset")) {
      char reset_dir[] = "/tmp/r3v-native-burst-reset-XXXXXX";
      if (mkdtemp(reset_dir) == NULL) {
         fprintf(stderr, "reset evidence directory creation failed\n");
         return 2;
      }
      setenv("R3V_NATIVE_MANIFEST_DIR", reset_dir, 1);
      struct burst_leg reset_leg = {0};
      if (run_burst_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, false, BURST_LIFETIME_RESET, false,
                        reset_dir, &reset_leg) != 0)
         goto done;
   }

   if (mode_selected(selected, "different-buffer") &&
       run_lifetime_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes,
                        BURST_LIFETIME_DIFFERENT_COMMAND_BUFFER,
                        "submit-lifetime-different-buffer") != 0)
      goto done;
   if (mode_selected(selected, "submit-shape") &&
       run_lifetime_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, BURST_LIFETIME_SUBMIT_SHAPE,
                        "submit-lifetime-submit-shape") != 0)
      goto done;
   if (mode_selected(selected, "trace-refusal") &&
       run_lifetime_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, BURST_LIFETIME_TRACE_REFUSAL,
                        "submit-lifetime-trace-refusal") != 0)
      goto done;
   if (mode_selected(selected, "teardown") &&
       run_lifetime_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, BURST_LIFETIME_TEARDOWN,
                        "submit-lifetime-teardown") != 0)
      goto done;
   if (selected != NULL && strcmp(selected, "known-bad-reset") == 0 &&
       run_lifetime_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, BURST_LIFETIME_KNOWN_BAD_RESET,
                        "submit-lifetime-known-bad-reset") != 0)
      goto done;
   if (selected != NULL && strcmp(selected, "known-bad-binding") == 0 &&
       run_lifetime_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, BURST_LIFETIME_KNOWN_BAD_BINDING,
                        "submit-lifetime-known-bad-binding") != 0)
      goto done;
   if (selected != NULL && strcmp(selected, "known-bad-teardown") == 0 &&
       run_lifetime_leg(instance, gipa, physical_device, &reference,
                        carrier_bytes, BURST_LIFETIME_KNOWN_BAD_TEARDOWN,
                        "submit-lifetime-known-bad-teardown") != 0)
      goto done;

   /* Fetch-width leg: the FLOAT_4 model burst under its own declared
    * digest and evidence directory -- the widened vertex object and the
    * changed stream admit once through the same gate.
    */
   if (mode_selected(selected, "admission")) {
      struct r300_r2vb_float2_tuple_burst_ib float4_reference;
      CHECK(r300_r2vb_float4_model_burst_reference_emit(
               BURST_DRAWS, &float4_reference) == 0,
            "float4 model reference burst emits");
      if (failures != 0) {
         r300_r2vb_float2_tuple_burst_release(&float4_reference);
         goto done;
      }
      char float4_digest[BLAKE3_OUT_LEN * 2 + 1];
      r300_triangle_ib_digest_hex(float4_reference.ib,
                                  float4_reference.ib_size_dwords,
                                  float4_digest);
      CHECK(strcmp(float4_digest, reference_digest) != 0,
            "the width contrast produces its own stream digest");
      char float4_dir[] = "/tmp/r3v-native-burst-float4-XXXXXX";
      if (mkdtemp(float4_dir) == NULL) {
         fprintf(stderr, "float4 evidence directory creation failed\n");
         r300_r2vb_float2_tuple_burst_release(&float4_reference);
         return 2;
      }
      setenv("R3V_NATIVE_MANIFEST_DIR", float4_dir, 1);
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", float4_digest, 1);
      struct burst_leg float4 = {0};
      int float4_rc =
         run_burst_leg(instance, gipa, physical_device, &float4_reference,
                       carrier_bytes, true, BURST_LIFETIME_NONE, true,
                       float4_dir, &float4);
      r300_r2vb_float2_tuple_burst_release(&float4_reference);
      if (float4_rc != 0)
         goto done;
      CHECK(float4.submit_result == VK_SUCCESS &&
               float4.queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "the float4 model burst admits once under its own digest: %d %s",
            float4.submit_result,
            r3v_native_queue_status_name(float4.queue_status));
   }

done:
   r300_r2vb_float2_tuple_burst_release(&reference);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_burst_cell_harness: all checks passed\n");
      return 0;
   }
   return 1;
}
