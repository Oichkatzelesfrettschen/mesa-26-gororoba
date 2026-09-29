/*
 * SPDX-License-Identifier: MIT
 *
 * Attended ZMASK read-group discovery: submits the four-quadrant cell
 * (src/amd/r300/common/r300_zmask_read_quadrant_cell.h) to RS485M
 * silicon through the native ICD once, retains the color target and the
 * depth allocation before and after, and reports the reading of every
 * quadrant.  This program performs a live DRM_RADEON_CS and runs only
 * under the operator's authorization: the driver's arming conjunction
 * admits it under the cell's own digest, the queue acquires HyperZ
 * ownership for the stream, and every stage prints and flushes before it
 * runs so a hang names the stage it hung in.
 *
 * The outcome vector (A, B, C, D) is the measurement, so an executed
 * submission whose color and depth were both judged exits zero whatever
 * the vector reads; a transport failure, a guard write, or a depth write
 * by draws that enable none exits nonzero.
 */

#include "../r3v_memory_properties_contract.h"

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_zmask_read_quadrant_cell.h"
#include "util/mesa-blake3.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

enum outcome {
   OUTCOME_EXECUTED,
   OUTCOME_DEPTH_WRITTEN,
   OUTCOME_CONTAINMENT_FAILURE,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_EXECUTED] = "EXECUTED",
   [OUTCOME_DEPTH_WRITTEN] = "DEPTH_WRITTEN",
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
   return outcome == OUTCOME_EXECUTED ? 0 : 1;
}

static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

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

static void
blake3_hex(const void *data, size_t size, char out[BLAKE3_OUT_LEN * 2 + 1])
{
   struct mesa_blake3 ctx;
   blake3_hash digest;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, data, size);
   _mesa_blake3_final(&ctx, digest);
   _mesa_blake3_format(out, digest);
}

static bool
parse_arm(const char *name, enum r300_zmask_read_quadrant_arm *arm)
{
   for (int a = 0; a < R300_ZMASK_READ_QUADRANT_ARM_COUNT; a++) {
      if (strcmp(name, r300_zmask_read_quadrant_arm_name(
                          (enum r300_zmask_read_quadrant_arm)a)) == 0) {
         *arm = (enum r300_zmask_read_quadrant_arm)a;
         return true;
      }
   }
   return false;
}

int
main(int argc, char **argv)
{
   enum r300_zmask_read_quadrant_arm arm;
   if (argc != 3 || !parse_arm(argv[2], &arm)) {
      fprintf(stderr, "usage: %s <evidence-directory> near|far\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   const struct r300_zb_depth_surface *surface;
   struct r300_zb_depth_layout layout;
   struct r300_zmask_layout zmask;
   if (r300_zmask_read_quadrant_surface(&surface, &layout, &zmask) != 0) {
      fprintf(stderr, "surface resolves no layout\n");
      return 2;
   }
   const uint64_t depth_bytes = layout.total_bytes;
   printf("[surface] %s allocation=%llu envelope=[%llu,%llu) zmask_dwords=%u "
          "zmask_pitch=%u block=%s\n",
          surface->name, (unsigned long long)depth_bytes,
          (unsigned long long)layout.base_offset_bytes,
          (unsigned long long)(layout.base_offset_bytes +
                               layout.storage_bytes),
          zmask.dwords, zmask.stride_in_pixels,
          zmask.zcomp8x8 ? "8x8" : "4x4");
   for (uint32_t q = 0; q < R300_ZMASK_READ_QUADRANT_COUNT; q++) {
      const struct r300_zmask_read_quadrant_draw *d =
         &r300_zmask_read_quadrant_draws[q];
      printf("[quadrant] %s arm=%s origin=(%u,%u) zb_bw_cntl=0x%02x "
             "depth=0x%06x color=0x%08x\n",
             d->name, r300_zmask_read_quadrant_arm_name(arm), d->origin_x,
             d->origin_y, d->zb_bw_cntl, d->depth_code[arm], d->color);
   }
   fflush(stdout);

   /* A silicon result binds to the real libc entry points. */
   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0') {
      fprintf(stderr, "LD_PRELOAD is set (%s); a hardware run admits no "
                      "interposer\n", preload);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared == NULL || declared[0] == '\0' ||
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr, "R3V_NATIVE_MANIFEST_DIR names %s and the argument "
                      "names %s; the armed directory and the readback "
                      "directory are one directory\n",
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
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS485M\n");
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
   struct { VkDeviceSize size; VkDeviceMemory memory; } allocations[] = {
      { R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION, VK_NULL_HANDLE },
      { R300_ZMASK_READ_QUADRANT_COLOR_BYTES, VK_NULL_HANDLE },
      { depth_bytes, VK_NULL_HANDLE },
   };
   for (unsigned i = 0; i < 3; i++) {
      if (vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = allocations[i].size,
                              .memoryTypeIndex = R3V_NATIVE_MEMORY_HOST_VISIBLE,
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
   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkBeginCommandBuffer: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = r3v_native_record_zmask_read_quadrants(
      cmd, vertex_memory, color_memory, depth_memory, arm);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "cell recording failed: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = vkEndCommandBuffer(cmd);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkEndCommandBuffer: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The depth image read back from the allocation the recorder seeded,
    * checked against the declared seed and kept as bytes. */
   stage("initial image");
   void *depth_map = NULL;
   if (vkMapMemory(device, depth_memory, 0, VK_WHOLE_SIZE, 0, &depth_map) !=
          VK_SUCCESS ||
       depth_map == NULL) {
      fprintf(stderr, "depth map before submission failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   uint8_t *before = malloc((size_t)depth_bytes);
   uint8_t *declared_image = malloc((size_t)depth_bytes);
   if (before == NULL || declared_image == NULL) {
      fprintf(stderr, "initial image allocation failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   memcpy(before, depth_map, (size_t)depth_bytes);
   const bool initialization_exact =
      r300_zmask_read_quadrant_fill_depth(&layout, declared_image,
                                          depth_bytes) == 0 &&
      memcmp(declared_image, before, (size_t)depth_bytes) == 0;
   free(declared_image);
   char initial_blake3[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(before, (size_t)depth_bytes, initial_blake3);
   printf("[initial] declared=%d blake3=%s\n", initialization_exact,
          initial_blake3);
   fflush(stdout);
   if (r3v_native_evidence_write_file(evidence_dir, "depth_before.bin", before,
                                      (size_t)depth_bytes) != 0) {
      fprintf(stderr, "initial image retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: a live DRM_RADEON_CS reaches the command processor here
    * under a HyperZ grant the queue acquires for this stream, and the
    * bounded completion wait follows inside the queue.  One shot. */
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
                                      R300_ZMASK_READ_QUADRANT_COLOR_BYTES) !=
          0 ||
       r3v_native_evidence_write_file(evidence_dir, "depth_after.bin",
                                      depth_map, (size_t)depth_bytes) != 0) {
      fprintf(stderr, "readback retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("oracle");
   struct r300_zmask_read_quadrant_color_observation seen;
   r300_zmask_read_quadrant_observe_color(
      color_map, R300_ZMASK_READ_QUADRANT_COLOR_BYTES, &seen);
   const uint64_t depth_changed = r300_zmask_read_quadrant_depth_changed_bytes(
      before, depth_map, depth_bytes);
   for (uint32_t q = 0; q < R300_ZMASK_READ_QUADRANT_COUNT; q++)
      printf("[quadrant] %s reading=%s colored=%u sentinel=%u foreign=%u\n",
             r300_zmask_read_quadrant_draws[q].name,
             r300_zmask_read_quadrant_reading_name(seen.reading[q]),
             seen.colored[q], seen.sentinel[q], seen.foreign[q]);
   printf("[oracle] vector=(%s, %s, %s, %s) guard_changed=%u "
          "depth_changed_bytes=%llu\n",
          r300_zmask_read_quadrant_reading_name(seen.reading[0]),
          r300_zmask_read_quadrant_reading_name(seen.reading[1]),
          r300_zmask_read_quadrant_reading_name(seen.reading[2]),
          r300_zmask_read_quadrant_reading_name(seen.reading[3]),
          seen.guard_changed, (unsigned long long)depth_changed);
   fflush(stdout);

   enum outcome outcome;
   if (seen.judged && seen.guard_changed != 0u)
      outcome = OUTCOME_CONTAINMENT_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED ||
            submit_result != VK_SUCCESS)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (depth_changed != 0u)
      outcome = OUTCOME_DEPTH_WRITTEN;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            seen.judged && initialization_exact)
      outcome = OUTCOME_EXECUTED;
   else
      outcome = OUTCOME_RETENTION_FAILURE;

   char quadrants_json[2048];
   size_t used = 0;
   quadrants_json[0] = '\0';
   for (uint32_t q = 0; q < R300_ZMASK_READ_QUADRANT_COUNT; q++) {
      const struct r300_zmask_read_quadrant_draw *d =
         &r300_zmask_read_quadrant_draws[q];
      const int written = snprintf(
         quadrants_json + used, sizeof(quadrants_json) - used,
         "%s{\"name\": \"%s\", \"origin\": [%u, %u], \"zb_bw_cntl\": "
         "\"0x%02x\", \"depth_code\": \"0x%06x\", \"color\": \"0x%08x\", "
         "\"reading\": \"%s\", \"colored\": %u, \"sentinel\": %u, "
         "\"foreign\": %u}",
         q == 0 ? "" : ", ", d->name, d->origin_x, d->origin_y,
         d->zb_bw_cntl, d->depth_code[arm], d->color,
         r300_zmask_read_quadrant_reading_name(seen.reading[q]),
         seen.colored[q], seen.sentinel[q], seen.foreign[q]);
      if (written <= 0 || (size_t)written >= sizeof(quadrants_json) - used) {
         fprintf(stderr, "quadrant list does not compose\n");
         return finish(OUTCOME_RETENTION_FAILURE);
      }
      used += (size_t)written;
   }

   char outcome_json[4096];
   const int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"r3v-native-zmask-read-quadrant-outcome/1\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"arm\": \"%s\",\n"
      "  \"surface\": \"%s\",\n"
      "  \"backing_word\": \"0x%06x%02x\",\n"
      "  \"clear_word\": \"0x%06x%02x\",\n"
      "  \"zmask_dwords\": %u,\n"
      "  \"zmask_block\": \"%s\",\n"
      "  \"initial_image_blake3\": \"%s\",\n"
      "  \"initialization_declared\": %s,\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"color_judged\": %s,\n"
      "  \"guard_changed\": %u,\n"
      "  \"depth_changed_bytes\": %llu,\n"
      "  \"quadrants\": [%s]\n"
      "}\n",
      outcome_names[outcome], r300_zmask_read_quadrant_arm_name(arm),
      surface->name,
      R300_ZMASK_READ_QUADRANT_BACKING_DEPTH_CODE,
      R300_ZMASK_READ_QUADRANT_BACKING_STENCIL,
      R300_ZMASK_READ_QUADRANT_CLEAR_DEPTH_CODE,
      R300_ZMASK_READ_QUADRANT_CLEAR_STENCIL, zmask.dwords,
      zmask.zcomp8x8 ? "8x8" : "4x4", initial_blake3,
      initialization_exact ? "true" : "false", submit_result,
      r3v_native_queue_status_name(queue_status),
      seen.judged ? "true" : "false", seen.guard_changed,
      (unsigned long long)depth_changed, quadrants_json);
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      "zmask_read_quadrant_outcome.json",
                                      outcome_json, (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("teardown");
   free(before);
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 3; i++)
      vkFreeMemory(device, allocations[i].memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return finish(outcome);
}
