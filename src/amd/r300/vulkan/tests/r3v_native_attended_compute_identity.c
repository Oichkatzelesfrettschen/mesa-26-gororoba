/*
 * SPDX-License-Identifier: MIT
 *
 * Attended runner for the compute identity carrier cell: the public
 * Vulkan compute surface -- storage buffers, descriptor set, the
 * reference identity-map kernel, one dispatch, submit -- on the identity
 * verb's GPU route, so RS482 silicon fetches the input buffer as F32_4
 * records through the VAP and writes the output buffer as one C4_32_FP
 * slot row through the color backend.  The driver's own admission
 * records the CPU bit copy as the oracle and its post-completion
 * read-back judges the output against it; the runner retains the input,
 * the output, and the verdict beside the submit objects.  The arming
 * digest is the reference pass's pin, which the runner prints beside its
 * recording.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"

#include "amd/r300/common/r300_compute_identity_carrier.h"
#include "amd/r300/common/r300_compute_verb.h"
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

/* The run's outcome classes.  Every class except TARGET_DELIVERED exits
 * nonzero, and the run never retries: one submission, one verdict.
 */
enum outcome {
   OUTCOME_TARGET_DELIVERED,
   OUTCOME_OUTPUT_MISMATCH,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_TARGET_DELIVERED] = "TARGET_DELIVERED",
   [OUTCOME_OUTPUT_MISMATCH] = "OUTPUT_MISMATCH",
   [OUTCOME_SUBMISSION_REFUSED] = "SUBMISSION_REFUSED",
   [OUTCOME_COMPLETION_FAILURE] = "COMPLETION_FAILURE",
   [OUTCOME_RETENTION_FAILURE] = "RETENTION_FAILURE",
};

static int
finish(enum outcome outcome)
{
   printf("verdict: %s\n", outcome_names[outcome]);
   fflush(stdout);
   return outcome == OUTCOME_TARGET_DELIVERED ? 0 : 1;
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

static bool
gate_open(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && strcmp(value, "1") == 0;
}

/* The reference kernel's workgroup is 64 wide, so one group moves the
 * sixteen F32_4 records of the reference pass: the words are the small
 * integers 0..63 as binary32, FP24 fixed points the carrier reproduces,
 * and the output is seeded with a pattern no input word equals.
 */
#define GROUP_WORDS 64u
#define OUTPUT_SEED 0x5c5c5c5cu

int
main(int argc, char **argv)
{
   /* --record-only builds every object and records the command buffer,
    * then stops at the recording boundary, so the drm-shim fixture
    * calibrates the sequence and the hardware mode inherits it.
    */
   bool record_only = false;
   bool usage_error = argc < 2 || argc > 3;
   if (argc == 3) {
      if (strcmp(argv[2], "--record-only") == 0)
         record_only = true;
      else
         usage_error = true;
   }
   if (usage_error) {
      fprintf(stderr, "usage: %s <evidence-directory> [--record-only]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   /* A silicon result binds to the real libc entry points: any
    * LD_PRELOAD refuses before the first Vulkan call.  The recording
    * mode reaches no ioctl and reports no verdict, so it runs on the
    * fixture.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (!record_only &&
       (declared == NULL || declared[0] == '\0' ||
        !same_directory(declared, evidence_dir))) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s; "
              "the armed directory and the readback directory are one "
              "directory\n",
              declared != NULL ? declared : "(unset)", evidence_dir);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The cell runs on the identity verb's GPU route: the compute gate
    * and the verb's gate open, every R2VB producer gate closed.  A
    * hardware run with the verb gate closed would dispatch on the CPU
    * route and report no carrier delivery, so it refuses; the recording
    * mode runs under whatever gates the fixture sets.
    */
   const char *verb_gate =
      r300_compute_verb_row(R300_COMPUTE_VERB_IDENTITY_MAP)->gpu_gate;
   if (!gate_open("R3V_HYBRID_COMPUTE_EXPERIMENTAL")) {
      fprintf(stderr, "R3V_HYBRID_COMPUTE_EXPERIMENTAL=1 opens the compute "
                      "surface the cell dispatches through\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   if (!record_only &&
       (!gate_open(verb_gate) ||
        gate_open("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") ||
        gate_open("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL") ||
        gate_open("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL"))) {
      fprintf(stderr,
              "the compute identity carrier runs under %s=1 with every "
              "R3V_NATIVE_R2VB_*_EXPERIMENTAL gate unset\n",
              verb_gate);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The cell this run records, emitted here so its digest reaches the
    * console beside the authorization the operator declared.
    */
   char cell_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   uint32_t cell_dwords = 0;
   {
      struct r300_r2vb_fetched_producer_ib cell;
      if (r300_compute_identity_carrier_reference_emit(&cell) != 0) {
         fprintf(stderr, "compute identity carrier emission failed\n");
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
      r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, cell_digest);
      cell_dwords = cell.ib_size_dwords;
      r300_r2vb_fetched_producer_release(&cell);
   }
   printf("cell compute-identity-carrier ib_dwords=%u ib_blake3=%s\n",
          cell_dwords, cell_digest);
   fflush(stdout);

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   if (create_instance(&(VkInstanceCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                       },
                       NULL, &instance) != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
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
   /* The compute surface stands behind its gate: the one family carries
    * the COMPUTE bit here, or the dispatch would refuse at pipeline
    * creation. */
   uint32_t family_count = 1;
   VkQueueFamilyProperties family;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &family_count, &family);
   if (family_count != 1 || (family.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
      fprintf(stderr, "the queue family withholds COMPUTE; the gate is "
                      "closed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("device");
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   if (vkCreateDevice(
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
          NULL, &device) != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkCreateBuffer);
   LOAD_DEVICE(vkBindBufferMemory);
   LOAD_DEVICE(vkCreateDescriptorSetLayout);
   LOAD_DEVICE(vkCreateDescriptorPool);
   LOAD_DEVICE(vkAllocateDescriptorSets);
   LOAD_DEVICE(vkUpdateDescriptorSets);
   LOAD_DEVICE(vkCreatePipelineLayout);
   LOAD_DEVICE(vkCreateShaderModule);
   LOAD_DEVICE(vkCreateComputePipelines);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkCmdBindPipeline);
   LOAD_DEVICE(vkCmdBindDescriptorSets);
   LOAD_DEVICE(vkCmdDispatch);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   /* Input and output: one page each, the reference geometry (offset
    * zero, 256 bytes of the 4096-byte page), distinct allocations so the
    * pass binds two relocations. */
   stage("buffers");
   const VkDeviceSize bytes = GROUP_WORDS * 4;
   VkDeviceMemory memories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   VkBuffer buffers[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   uint32_t *maps[2] = { NULL, NULL };
   for (unsigned b = 0; b < 2; b++) {
      if (vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &memories[b]) != VK_SUCCESS ||
          vkCreateBuffer(device,
                         &(VkBufferCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = bytes,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &buffers[b]) != VK_SUCCESS ||
          vkBindBufferMemory(device, buffers[b], memories[b], 0) !=
             VK_SUCCESS ||
          vkMapMemory(device, memories[b], 0, VK_WHOLE_SIZE, 0,
                      (void **)&maps[b]) != VK_SUCCESS) {
         fprintf(stderr, "storage buffer %u setup failed\n", b);
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
   }
   uint32_t input_words[GROUP_WORDS];
   for (uint32_t i = 0; i < GROUP_WORDS; i++) {
      const float value = (float)i;
      memcpy(&input_words[i], &value, sizeof(value));
   }
   memcpy(maps[0], input_words, sizeof(input_words));
   for (uint32_t i = 0; i < GROUP_WORDS; i++)
      maps[1][i] = OUTPUT_SEED;

   stage("descriptors");
   const VkDescriptorSetLayoutBinding bindings[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
   };
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set = VK_NULL_HANDLE;
   if (vkCreateDescriptorSetLayout(
          device,
          &(VkDescriptorSetLayoutCreateInfo){
             .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
             .bindingCount = 2,
             .pBindings = bindings,
          },
          NULL, &set_layout) != VK_SUCCESS ||
       vkCreateDescriptorPool(
          device,
          &(VkDescriptorPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
             .maxSets = 1,
             .poolSizeCount = 1,
             .pPoolSizes =
                &(VkDescriptorPoolSize){
                   .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .descriptorCount = 2,
                },
          },
          NULL, &pool) != VK_SUCCESS ||
       vkAllocateDescriptorSets(
          device,
          &(VkDescriptorSetAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
             .descriptorPool = pool,
             .descriptorSetCount = 1,
             .pSetLayouts = &set_layout,
          },
          &set) != VK_SUCCESS) {
      fprintf(stderr, "descriptor setup failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   const VkDescriptorBufferInfo buffer_infos[2] = {
      { .buffer = buffers[0], .offset = 0, .range = bytes },
      { .buffer = buffers[1], .offset = 0, .range = bytes },
   };
   const VkWriteDescriptorSet writes[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_infos[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_infos[1] },
   };
   vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

   stage("pipeline");
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkShaderModule module = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   if (vkCreatePipelineLayout(
          device,
          &(VkPipelineLayoutCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             .setLayoutCount = 1,
             .pSetLayouts = &set_layout,
          },
          NULL, &layout) != VK_SUCCESS ||
       vkCreateShaderModule(
          device,
          &(VkShaderModuleCreateInfo){
             .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
             .codeSize = sizeof(r3v_reference_identity_map_spirv),
             .pCode = r3v_reference_identity_map_spirv,
          },
          NULL, &module) != VK_SUCCESS) {
      fprintf(stderr, "pipeline layout or module creation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1,
      &(VkComputePipelineCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage =
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_COMPUTE_BIT,
               .module = module,
               .pName = "main",
            },
         .layout = layout,
      },
      NULL, &pipeline);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateComputePipelines: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("record");
   VkCommandPool cmd_pool = VK_NULL_HANDLE;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (vkCreateCommandPool(
          device,
          &(VkCommandPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
             .queueFamilyIndex = 0,
          },
          NULL, &cmd_pool) != VK_SUCCESS ||
       vkAllocateCommandBuffers(
          device,
          &(VkCommandBufferAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
             .commandPool = cmd_pool,
             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
             .commandBufferCount = 1,
          },
          &cmd) != VK_SUCCESS ||
       vkBeginCommandBuffer(
          cmd, &(VkCommandBufferBeginInfo){
                  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
               }) != VK_SUCCESS) {
      fprintf(stderr, "command buffer setup failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                           &set, 0, NULL);
   vkCmdDispatch(cmd, 1, 1, 1);
   VkResult end_result = vkEndCommandBuffer(cmd);
   if (end_result != VK_SUCCESS) {
      fprintf(stderr, "vkEndCommandBuffer: %d\n", end_result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   if (record_only) {
      printf("record: ACCEPTED\n");
      fflush(stdout);
      vkDestroyCommandPool(device, cmd_pool, NULL);
      vkFreeMemory(device, memories[0], NULL);
      vkFreeMemory(device, memories[1], NULL);
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 0;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: the admission installs the carrier pass and a live
    * DRM_RADEON_CS reaches the command processor here; the driver's
    * read-back judges the output after the completion wait.  One-shot:
    * whatever it returns, no resubmission follows.
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
   enum r3v_native_observed_route route =
      r3v_native_queue_observed_route(device);
   printf("[submit] vkQueueSubmit returned %d status=%s route=%s\n",
          submit_result, r3v_native_queue_status_name(queue_status),
          r3v_native_observed_route_name(route));
   fflush(stdout);

   /* Readback and retention run for every submit result: a refused or
    * incomplete submission still leaves the output's state as evidence.
    * The output map is the application's live mapping; the driver's
    * post-completion invalidate covered the read-back it judged, and
    * this read repeats it through the same mapping.
    */
   stage("readback");
   if (r3v_native_evidence_write_file(evidence_dir, "compute_input.bin",
                                      input_words, sizeof(input_words)) != 0 ||
       r3v_native_evidence_write_file(evidence_dir, "compute_output.bin",
                                      maps[1], sizeof(input_words)) != 0) {
      fprintf(stderr, "buffer retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   const bool output_matches =
      memcmp(maps[1], input_words, sizeof(input_words)) == 0;
   bool output_untouched = true;
   for (uint32_t i = 0; i < GROUP_WORDS; i++)
      output_untouched &= maps[1][i] == OUTPUT_SEED;
   printf("[oracle] output_matches=%d output_untouched=%d\n", output_matches,
          output_untouched);
   fflush(stdout);

   enum outcome outcome;
   if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (submit_result == VK_SUCCESS &&
            queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            route == R3V_NATIVE_OBSERVED_ROUTE_COMPUTE_IDENTITY_CARRIER &&
            output_matches)
      outcome = OUTCOME_TARGET_DELIVERED;
   else
      outcome = OUTCOME_OUTPUT_MISMATCH;

   char outcome_json[1024];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"r3v-native-compute-identity-outcome/1\",\n"
      "  \"route\": \"%s\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"record_count\": %u,\n"
      "  \"output_bytes\": %u,\n"
      "  \"output_matches\": %s,\n"
      "  \"output_untouched\": %s\n"
      "}\n",
      r3v_native_observed_route_name(route), outcome_names[outcome],
      submit_result, r3v_native_queue_status_name(queue_status), cell_dwords,
      cell_digest, (unsigned)R300_COMPUTE_IDENTITY_CARRIER_REFERENCE_RECORDS,
      (unsigned)sizeof(input_words), output_matches ? "true" : "false",
      output_untouched ? "true" : "false");
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      "compute_identity_outcome.json",
                                      outcome_json, (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("teardown");
   vkDestroyCommandPool(device, cmd_pool, NULL);
   vkFreeMemory(device, memories[0], NULL);
   vkFreeMemory(device, memories[1], NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return finish(outcome);
}
