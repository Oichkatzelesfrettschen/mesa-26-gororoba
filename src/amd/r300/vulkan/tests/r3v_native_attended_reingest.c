/*
 * SPDX-License-Identifier: MIT
 *
 * Attended producer-plus-re-ingest cell: submits the concatenated
 * reference stream to RS482 silicon through the native ICD, retains the
 * carrier and the color target durably, and classifies the read-back.
 * The carrier check separates the producer stage from the consuming
 * draw, and the triangle output oracle judges the re-ingested render,
 * so one run localizes a failure to its stage.  This program performs a
 * live DRM_RADEON_CS; the driver's arming conjunction admits it under
 * the re-ingest cell's own digest, and every stage prints and flushes
 * before it runs so a hang names the stage it hung in.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The run's outcome classes.  Only REINGEST_RENDERED exits zero, and the
 * run never retries: one submission, one verdict.  CARRIER_ONLY names a
 * delivered carrier under a failed render -- the vertex-fetch stage is
 * then the open mechanism -- while CARRIER_UNWRITTEN names a failed
 * producer stage before the fetch question arises.
 */
enum outcome {
   OUTCOME_REINGEST_RENDERED,
   OUTCOME_CARRIER_ONLY,
   OUTCOME_CARRIER_MISMATCH,
   OUTCOME_CARRIER_UNWRITTEN,
   OUTCOME_CONTAINMENT_FAILURE,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_REINGEST_RENDERED] = "REINGEST_RENDERED",
   [OUTCOME_CARRIER_ONLY] = "CARRIER_ONLY",
   [OUTCOME_CARRIER_MISMATCH] = "CARRIER_MISMATCH",
   [OUTCOME_CARRIER_UNWRITTEN] = "CARRIER_UNWRITTEN",
   [OUTCOME_CONTAINMENT_FAILURE] = "CONTAINMENT_FAILURE",
   [OUTCOME_SUBMISSION_REFUSED] = "SUBMISSION_REFUSED",
   [OUTCOME_COMPLETION_FAILURE] = "COMPLETION_FAILURE",
   [OUTCOME_RETENTION_FAILURE] = "RETENTION_FAILURE",
};

static int
finish(enum outcome outcome)
{
   printf("verdict: %s\n", outcome_names[outcome]);
   fflush(stdout);
   return outcome == OUTCOME_REINGEST_RENDERED ? 0 : 1;
}

/* Names the stage about to run.  A hang leaves its stage as the last line
 * on the console and in the off-box log.
 */
static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

/* One directory reached by two spellings is still one directory, so the
 * comparison resolves both paths when they exist.
 */
static bool
same_directory(const char *a, const char *b)
{
   if (strcmp(a, b) == 0)
      return true;
   char resolved_a[PATH_MAX];
   char resolved_b[PATH_MAX];
   return realpath(a, resolved_a) != NULL && realpath(b, resolved_b) != NULL &&
          strcmp(resolved_a, resolved_b) == 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <evidence-directory>\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   /* A silicon result binds to the real libc entry points.  A preloaded
    * interposer would let the run report a silicon verdict it never
    * earned, so any LD_PRELOAD refuses before the first Vulkan call.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The armed directory and the readback directory are one directory,
    * so a disagreement refuses.
    */
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared == NULL || declared[0] == '\0' ||
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s; "
              "the armed directory and the readback directory are one "
              "directory\n",
              declared != NULL ? declared : "(unset)", evidence_dir);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   uint32_t carrier_bytes = 0;
   if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0) {
      fprintf(stderr, "producer carrier geometry unresolved\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   const uint32_t color_bytes = R3V_NATIVE_TARGET_MEMORY_BYTES;
   uint32_t expected[R300_R2VB_PRODUCER_REFERENCE_COUNT * 4];
   const uint32_t expected_dwords =
      (uint32_t)(sizeof(expected) / sizeof(expected[0]));
   if (r300_r2vb_producer_reference_expected(expected, expected_dwords) != 0) {
      fprintf(stderr, "carrier identity delivery failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("instance");
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
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       pdev_count != 1 || pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS482\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("device");
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = vkCreateDevice(
      pdev,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos =
            &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0,
               .queueCount = 1,
               .pQueuePriorities = &priority,
            },
      },
      NULL, &device);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   stage("memory");
   VkDeviceMemory carrier_memory = VK_NULL_HANDLE;
   if (vkAllocateMemory(device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = carrier_bytes,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &carrier_memory) != VK_SUCCESS) {
      fprintf(stderr, "carrier allocation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   if (vkAllocateMemory(device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = color_bytes,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &color_memory) != VK_SUCCESS) {
      fprintf(stderr, "color target allocation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   if (vkCreateCommandPool(
          device,
          &(VkCommandPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
             .queueFamilyIndex = 0,
          },
          NULL, &pool) != VK_SUCCESS) {
      fprintf(stderr, "command pool creation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (vkAllocateCommandBuffers(
          device,
          &(VkCommandBufferAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
             .commandPool = pool,
             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
             .commandBufferCount = 1,
          },
          &cmd) != VK_SUCCESS) {
      fprintf(stderr, "command buffer allocation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkBeginCommandBuffer: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = r3v_native_record_r2vb_reingest(cmd, carrier_memory,
                                            color_memory);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "cell recording failed: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = vkEndCommandBuffer(cmd);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkEndCommandBuffer: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: a live DRM_RADEON_CS reaches the command processor here,
    * and the bounded completion wait follows it inside the queue.  The
    * submission is one-shot; whatever it returns, no resubmission follows.
    */
   stage("submit");
   VkResult submit_result =
      vkQueueSubmit(queue, 1,
                    &(VkSubmitInfo){
                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cmd,
                    },
                    VK_NULL_HANDLE);
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);
   printf("[submit] vkQueueSubmit returned %d status=%s\n", submit_result,
          r3v_native_queue_status_name(queue_status));
   fflush(stdout);

   /* Readback and retention run for every submit result: a refused or
    * incomplete submission still leaves both allocations as evidence.
    */
   stage("readback");
   void *carrier_map = NULL;
   if (vkMapMemory(device, carrier_memory, 0, VK_WHOLE_SIZE, 0,
                   &carrier_map) != VK_SUCCESS ||
       carrier_map == NULL) {
      fprintf(stderr, "carrier readback map failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "carrier.bin",
                                      carrier_map, carrier_bytes) != 0) {
      fprintf(stderr, "carrier retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   void *color_map = NULL;
   if (vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                   &color_map) != VK_SUCCESS ||
       color_map == NULL) {
      fprintf(stderr, "color readback map failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "color.bin", color_map,
                                      color_bytes) != 0) {
      fprintf(stderr, "color retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("oracle");
   struct r300_r2vb_producer_carrier_verdict carrier_verdict;
   if (r300_r2vb_producer_carrier_check(
          expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
          carrier_map, carrier_bytes, &carrier_verdict) != 0) {
      fprintf(stderr, "carrier check refused its inputs\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   struct r300_triangle_oracle_verdict target_verdict;
   r300_tcl_bypass_triangle_oracle(color_map, color_bytes, &target_verdict);
   printf("[oracle] carrier expected_pass=%d tail_poison_pass=%d "
          "mismatched=%u tail_disturbed=%u\n",
          carrier_verdict.expected_pass, carrier_verdict.tail_poison_pass,
          carrier_verdict.mismatched_dwords,
          carrier_verdict.disturbed_tail_dwords);
   printf("[oracle] target executed=%d interior=%d exterior=%d canary=%d "
          "samples=%u/%u\n",
          target_verdict.executed, target_verdict.interior_pass,
          target_verdict.exterior_pass, target_verdict.canary_pass,
          target_verdict.interior_samples, target_verdict.exterior_samples);
   fflush(stdout);

   /* Classification order: containment first -- a write past either
    * allocation's declared extent stops the sequence whatever else
    * passed -- then the transport's own failures, then the two stages in
    * dependency order: a carrier still poison throughout names an
    * unwritten producer stage, a carrier written to wrong values names
    * the producer's delivery, and a delivered carrier under a failed
    * render names the re-ingest fetch or draw.
    */
   const bool render_pass = target_verdict.executed &&
                            target_verdict.interior_pass &&
                            target_verdict.exterior_pass &&
                            target_verdict.canary_pass;
   enum outcome outcome;
   if (!carrier_verdict.tail_poison_pass || !target_verdict.canary_pass)
      outcome = OUTCOME_CONTAINMENT_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED ||
            submit_result != VK_SUCCESS)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (queue_status != R3V_NATIVE_QUEUE_STATUS_COMPLETED)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (!carrier_verdict.expected_pass)
      outcome = carrier_verdict.poison_dwords == expected_dwords
                   ? OUTCOME_CARRIER_UNWRITTEN
                   : OUTCOME_CARRIER_MISMATCH;
   else if (!render_pass)
      outcome = OUTCOME_CARRIER_ONLY;
   else
      outcome = OUTCOME_REINGEST_RENDERED;

   char outcome_json[1280];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"r3v-native-r2vb-reingest-outcome/1\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"carrier_size_bytes\": %u,\n"
      "  \"carrier_expected_pass\": %s,\n"
      "  \"carrier_tail_poison_pass\": %s,\n"
      "  \"carrier_mismatched_dwords\": %u,\n"
      "  \"color_size_bytes\": %u,\n"
      "  \"target_executed\": %s,\n"
      "  \"target_interior_pass\": %s,\n"
      "  \"target_exterior_pass\": %s,\n"
      "  \"target_canary_pass\": %s\n"
      "}\n",
      outcome_names[outcome], submit_result,
      r3v_native_queue_status_name(queue_status), carrier_bytes,
      carrier_verdict.expected_pass ? "true" : "false",
      carrier_verdict.tail_poison_pass ? "true" : "false",
      carrier_verdict.mismatched_dwords, color_bytes,
      target_verdict.executed ? "true" : "false",
      target_verdict.interior_pass ? "true" : "false",
      target_verdict.exterior_pass ? "true" : "false",
      target_verdict.canary_pass ? "true" : "false");
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      "reingest_outcome.json", outcome_json,
                                      (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, carrier_memory, NULL);
   vkFreeMemory(device, color_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   return finish(outcome);
}
