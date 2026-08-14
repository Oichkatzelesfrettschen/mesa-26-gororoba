/*
 * SPDX-License-Identifier: MIT
 *
 * Attended producer-only cell: submits the reference R2VB producer pass to
 * RS482 silicon through the native ICD, retains the complete carrier
 * durably, and classifies the read-back.  The cell renders no visible
 * target and re-ingests nothing, so the carrier bytes are the whole
 * result: the poison prefill makes an unwritten slot decidable, the
 * delivery identity supplies the expected dwords, and the padding slot
 * past the delivered extent must still hold poison.  This program
 * performs a live DRM_RADEON_CS; the driver's arming conjunction admits
 * it under the producer cell's own digest, and every stage prints and
 * flushes before it runs so a hang names the stage it hung in.  The
 * triangle and direct-write cells keep their own runners; no runner
 * records another's cell.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_r2vb_producer_pass.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The run's outcome classes.  Every class except CARRIER_DELIVERED exits
 * nonzero, and the run never retries: one submission, one verdict.
 */
enum outcome {
   OUTCOME_CARRIER_DELIVERED,
   OUTCOME_CARRIER_MISMATCH,
   OUTCOME_CARRIER_UNWRITTEN,
   OUTCOME_CONTAINMENT_FAILURE,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_CARRIER_DELIVERED] = "CARRIER_DELIVERED",
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
   return outcome == OUTCOME_CARRIER_DELIVERED ? 0 : 1;
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
   if (argc != 2 && argc != 3) {
      fprintf(stderr, "usage: %s <evidence-directory> [stream]\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];
   /* The optional stream selector: every stream embeds its own records
    * under the same cell kind and carrier geometry, so an authorization
    * differs only in the digest it declares.  A selector outside the
    * named set refuses before any Vulkan call.
    */
   const struct r300_r2vb_producer_stream_ops *stream =
      r300_r2vb_producer_stream_find(argc == 3 ? argv[2] : "reference");
   if (stream == NULL) {
      fprintf(stderr, "unknown stream selector %s\n", argv[2]);
      return 2;
   }
   printf("stream=%s\n", stream->name);
   fflush(stdout);

   /* A silicon result binds to the real libc entry points.  A preloaded
    * interposer -- the drm-shim fixture or any other -- would let the run
    * report a silicon verdict it never earned, so any LD_PRELOAD refuses
    * before the first Vulkan call.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The driver reads the evidence directory from the environment, and the
    * arming conjunction consumes the one-shot token in whichever directory
    * the environment names.  The armed directory and the readback directory
    * are one directory, so a disagreement refuses.
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
   uint32_t expected[R300_R2VB_PRODUCER_REFERENCE_COUNT * 4];
   const uint32_t expected_dwords =
      (uint32_t)(sizeof(expected) / sizeof(expected[0]));
   int expected_rc = stream->expected(expected, expected_dwords);
   if (expected_rc != 0) {
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
   /* The arming gate enforces this too; refusing here keeps the run off a
    * chip whose falsifiers were not written.
    */
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

   /* The pass writes the carrier through the color backend and no other
    * BO rides the stream, so the carrier is the run's one allocation.
    */
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
   /* The submission is one-shot, so a failed recording boundary refuses
    * here rather than carrying a non-executable command buffer into the
    * hazardous ioctl.
    */
   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkBeginCommandBuffer: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = strcmp(stream->name, "fp24-sweep") == 0
               ? r3v_native_record_r2vb_producer_fp24_sweep(cmd,
                                                            carrier_memory)
            : strcmp(stream->name, "fp24-bisect") == 0
               ? r3v_native_record_r2vb_producer_fp24_bisect(cmd,
                                                             carrier_memory)
               : r3v_native_record_r2vb_producer(cmd, carrier_memory);
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
    * incomplete submission still leaves the carrier's state as evidence.
    */
   stage("readback");
   void *carrier_map = NULL;
   if (vkMapMemory(device, carrier_memory, 0, VK_WHOLE_SIZE, 0,
                   &carrier_map) != VK_SUCCESS ||
       carrier_map == NULL) {
      fprintf(stderr, "carrier readback map failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   /* The carrier is the run's primary artifact and the submission is
    * one-shot, so the verdict reaches the console only once the complete
    * allocation is durable.
    */
   if (r3v_native_evidence_write_file(evidence_dir, "carrier.bin",
                                      carrier_map, carrier_bytes) != 0) {
      fprintf(stderr, "carrier retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("oracle");
   struct r300_r2vb_producer_carrier_verdict verdict;
   if (r300_r2vb_producer_carrier_check(
          expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
          carrier_map, carrier_bytes, &verdict) != 0) {
      fprintf(stderr, "carrier check refused its inputs\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   printf("[oracle] expected_pass=%d tail_poison_pass=%d mismatched=%u "
          "tail_disturbed=%u\n",
          verdict.expected_pass, verdict.tail_poison_pass,
          verdict.mismatched_dwords, verdict.disturbed_tail_dwords);
   const uint32_t *dwords = carrier_map;
   printf("[oracle] slot0 %08x %08x %08x %08x expected %08x %08x %08x "
          "%08x\n",
          dwords[0], dwords[1], dwords[2], dwords[3], expected[0],
          expected[1], expected[2], expected[3]);
   fflush(stdout);

   /* Classification order: a write past the delivered extent stops the
    * sequence whatever else passed; then the transport's own failures;
    * then the carrier verdict.  An expected extent still holding poison
    * throughout names an unwritten carrier; any dword written to a value
    * other than its expectation names a delivered-but-wrong carrier, the
    * class a boundary sweep exists to surface.
    */
   enum outcome outcome;
   if (!verdict.tail_poison_pass)
      outcome = OUTCOME_CONTAINMENT_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED ||
            submit_result != VK_SUCCESS)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            verdict.expected_pass)
      outcome = OUTCOME_CARRIER_DELIVERED;
   else if (verdict.poison_dwords == expected_dwords)
      outcome = OUTCOME_CARRIER_UNWRITTEN;
   else
      outcome = OUTCOME_CARRIER_MISMATCH;

   char outcome_json[1024];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"r3v-native-r2vb-producer-outcome/1\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"carrier_size_bytes\": %u,\n"
      "  \"expected_dwords\": %u,\n"
      "  \"expected_pass\": %s,\n"
      "  \"tail_poison_pass\": %s,\n"
      "  \"mismatched_dwords\": %u,\n"
      "  \"poison_dwords\": %u,\n"
      "  \"disturbed_tail_dwords\": %u\n"
      "}\n",
      outcome_names[outcome], submit_result,
      r3v_native_queue_status_name(queue_status), carrier_bytes,
      expected_dwords, verdict.expected_pass ? "true" : "false",
      verdict.tail_poison_pass ? "true" : "false", verdict.mismatched_dwords,
      verdict.poison_dwords, verdict.disturbed_tail_dwords);
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      "producer_outcome.json", outcome_json,
                                      (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, carrier_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   return finish(outcome);
}
