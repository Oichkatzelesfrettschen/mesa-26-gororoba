/*
 * SPDX-License-Identifier: MIT
 *
 * Attended depth control: submits the two-triangle depth-test cell to
 * RS482 silicon through the native ICD, retains the complete color
 * target and depth surface durably, and classifies the outcome.  This
 * program performs a live DRM_RADEON_CS and runs only under the
 * operator's authorization; the driver's arming conjunction admits it
 * under the cell's own digest, and every stage prints and flushes
 * before it runs so a hang names the stage it hung in.  The cell is the
 * B14 depth control for R300_ZB_ZCACHE_CTLSTAT: its first run is the
 * unobserved workload debut, and no reader debuts in the same boot.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zb_depth_control_cell.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The run's outcome classes.  Every class except CONTROL_PASS exits
 * nonzero, and the run never retries: a failed control is
 * INCONCLUSIVE / CONTROL FAILED, so no pipeline or transport hypothesis
 * follows from it.
 */
enum outcome {
   OUTCOME_CONTROL_PASS,
   OUTCOME_CONTROL_FAILED_INCONCLUSIVE,
   OUTCOME_CONTAINMENT_FAILURE,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_CONTROL_PASS] = "CONTROL_PASS",
   [OUTCOME_CONTROL_FAILED_INCONCLUSIVE] = "CONTROL_FAILED_INCONCLUSIVE",
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
   return outcome == OUTCOME_CONTROL_PASS ? 0 : 1;
}

/* Names the stage about to run.  A hang leaves its stage as the last
 * line on the console and in the off-box log.
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
    * interposer -- the drm-shim fixture or any other -- would let the
    * run report a silicon verdict it never earned, so any LD_PRELOAD
    * refuses before the first Vulkan call.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware control run admits no "
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
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n",
          props.vendorID, props.deviceID, props.deviceName);
   fflush(stdout);
   /* The arming gate enforces this too; refusing here keeps the run off
    * a chip whose falsifiers were not written.
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

   /* The cell's three roles at their exact declared footprints; the
    * recorder refuses any other shape.
    */
   stage("memory");
   struct { VkDeviceSize size; VkDeviceMemory memory; } allocations[] = {
      { R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION, VK_NULL_HANDLE },
      { R300_ZB_DEPTH_CONTROL_COLOR_BYTES, VK_NULL_HANDLE },
      { R300_ZB_DEPTH_CONTROL_DEPTH_BYTES, VK_NULL_HANDLE },
   };
   for (unsigned i = 0; i < 3; i++) {
      if (vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = allocations[i].size,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &allocations[i].memory) != VK_SUCCESS) {
         fprintf(stderr, "allocation %u failed\n", i);
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
   }
   VkDeviceMemory vertex_memory = allocations[0].memory;
   VkDeviceMemory color_memory = allocations[1].memory;
   VkDeviceMemory depth_memory = allocations[2].memory;

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
   result = r3v_native_record_zb_depth_control(cmd, vertex_memory,
                                               color_memory, depth_memory);
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

   /* The hazard: a live DRM_RADEON_CS reaches the command processor
    * here, and the bounded completion wait follows it inside the queue.
    * The submission is one-shot; whatever it returns, no resubmission
    * follows.
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
    * incomplete submission still leaves both surfaces' states as
    * evidence.
    */
   stage("readback");
   void *color_map = NULL;
   if (vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0, &color_map) !=
          VK_SUCCESS ||
       color_map == NULL) {
      fprintf(stderr, "color readback map failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "color_target.bin",
                                      color_map,
                                      R300_ZB_DEPTH_CONTROL_COLOR_BYTES) !=
       0) {
      fprintf(stderr, "color target retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   void *depth_map = NULL;
   if (vkMapMemory(device, depth_memory, 0, VK_WHOLE_SIZE, 0, &depth_map) !=
          VK_SUCCESS ||
       depth_map == NULL) {
      fprintf(stderr, "depth readback map failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "depth_surface.bin",
                                      depth_map,
                                      R300_ZB_DEPTH_CONTROL_DEPTH_BYTES) !=
       0) {
      fprintf(stderr, "depth surface retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("oracle");
   struct r300_zb_depth_control_color_verdict color_verdict;
   r300_zb_depth_control_color_oracle(
      color_map, R300_ZB_DEPTH_CONTROL_COLOR_BYTES, &color_verdict);
   struct r300_zb_depth_control_depth_verdict depth_verdict;
   r300_zb_depth_control_depth_oracle(
      depth_map, R300_ZB_DEPTH_CONTROL_DEPTH_BYTES, &depth_verdict);
   printf("[oracle] color executed=%d near=%d far=%d exterior=%d canary=%d "
          "near_colored=%u/%u far_colored=%u/%u\n",
          color_verdict.executed, color_verdict.near_pass,
          color_verdict.far_pass, color_verdict.exterior_pass,
          color_verdict.canary_pass, color_verdict.near_colored,
          color_verdict.near_samples, color_verdict.far_colored,
          color_verdict.far_samples);
   printf("[oracle] depth written=%d near=%d far=%d exterior=%d canary=%d "
          "near_range=[0x%04x,0x%04x]\n",
          depth_verdict.written, depth_verdict.near_pass,
          depth_verdict.far_pass, depth_verdict.exterior_pass,
          depth_verdict.canary_pass, depth_verdict.near_min,
          depth_verdict.near_max);
   fflush(stdout);

   /* Classification order: a write past either surface's render extent
    * stops the sequence whatever else passed; then the transport's own
    * failures; then the control verdict over both oracles together --
    * the color proving the test gated the write and the depth proving
    * the write reached memory.
    */
   enum outcome outcome;
   if (!color_verdict.canary_pass || !depth_verdict.canary_pass)
      outcome = OUTCOME_CONTAINMENT_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED ||
            submit_result != VK_SUCCESS)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            color_verdict.executed && color_verdict.near_pass &&
            color_verdict.far_pass && color_verdict.exterior_pass &&
            depth_verdict.written && depth_verdict.near_pass &&
            depth_verdict.far_pass && depth_verdict.exterior_pass)
      outcome = OUTCOME_CONTROL_PASS;
   else
      outcome = OUTCOME_CONTROL_FAILED_INCONCLUSIVE;

   char outcome_json[1536];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"r3v-native-zb-depth-control-outcome/1\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"color_executed\": %s,\n"
      "  \"color_near_pass\": %s,\n"
      "  \"color_far_pass\": %s,\n"
      "  \"color_exterior_pass\": %s,\n"
      "  \"color_canary_pass\": %s,\n"
      "  \"color_near_colored\": %u,\n"
      "  \"color_near_samples\": %u,\n"
      "  \"color_far_colored\": %u,\n"
      "  \"color_far_samples\": %u,\n"
      "  \"depth_written\": %s,\n"
      "  \"depth_near_pass\": %s,\n"
      "  \"depth_far_pass\": %s,\n"
      "  \"depth_exterior_pass\": %s,\n"
      "  \"depth_canary_pass\": %s,\n"
      "  \"depth_near_min\": \"0x%04x\",\n"
      "  \"depth_near_max\": \"0x%04x\"\n"
      "}\n",
      outcome_names[outcome], submit_result,
      r3v_native_queue_status_name(queue_status),
      color_verdict.executed ? "true" : "false",
      color_verdict.near_pass ? "true" : "false",
      color_verdict.far_pass ? "true" : "false",
      color_verdict.exterior_pass ? "true" : "false",
      color_verdict.canary_pass ? "true" : "false",
      color_verdict.near_colored, color_verdict.near_samples,
      color_verdict.far_colored, color_verdict.far_samples,
      depth_verdict.written ? "true" : "false",
      depth_verdict.near_pass ? "true" : "false",
      depth_verdict.far_pass ? "true" : "false",
      depth_verdict.exterior_pass ? "true" : "false",
      depth_verdict.canary_pass ? "true" : "false", depth_verdict.near_min,
      depth_verdict.near_max);
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      "zb_depth_control_outcome.json",
                                      outcome_json, (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 3; i++)
      vkFreeMemory(device, allocations[i].memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   return finish(outcome);
}
