/*
 * SPDX-License-Identifier: MIT
 *
 * The identity verb's GPU route over the drm-shim: one fresh device per
 * arm under its own gates, the reference identity-map kernel over
 * sixteen words (four F32_4 records), and the verdict read from the
 * queue status, the recorded stream, the carrier pass's pinned digest,
 * the output bytes, and the quarantine flag.
 */

/* The asserts carry this harness's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#define VK_NO_PROTOTYPES

#include "r3v_native.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_shim_arming.h"

#include "amd/r300/common/r300_compute_identity_carrier.h"
#include "amd/r300/common/r300_compute_verb.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/tests/r300_compute_identity_carrier_digests.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_ioctl.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

enum arm {
   /* The verb gate closed: the dispatch keeps the CPU route, no IB. */
   ARM_GATE_OFF_CPU,
   /* Both gates open over in-window words: the recorded stream is the
    * pinned reference pass, the ioctl runs, the shim delivers nothing,
    * so the read-back diverges from the oracle, reports device loss,
    * quarantines, and retains the observed and expected bytes. */
   ARM_COMPOSED,
   /* A negative input word refuses at admission. */
   ARM_OUT_OF_DOMAIN_REFUSED,
   /* Input and output in one buffer object refuse at admission. */
   ARM_ALIAS_REFUSED,
   /* A record count past the single-row ceiling refuses at admission:
    * 65 workgroups name 1040 records over the 1024-slot row. */
   ARM_CEILING_REFUSED,
   /* The gate table: every ledger row's gate opens on the literal "1"
    * alone -- "0", "", "yes", and unset stay closed -- and an open gate
    * on a row without an executing GPU route selects nothing, so the
    * identity dispatch keeps the CPU route under every other gate. */
   ARM_GATE_TABLE,
   /* The oracle's positive side, calibrated: the output pre-seeded with
    * the expected bytes reads back as agreement after the shim's empty
    * delivery, so the verdict is decided by the bytes -- COMPLETED, no
    * quarantine, both byte files retained.  A calibration of the
    * comparator, never a delivery claim: the pre-seed is what the
    * device would have had to write. */
   ARM_PRESEEDED_AGREEMENT,
   /* After a divergence quarantines the capability, a further dispatch
    * refuses at admission with nothing installed. */
   ARM_QUARANTINED_REFUSED,
};

static const struct {
   const char *name;
   enum arm arm;
} arm_names[] = {
   { "gate-off-cpu", ARM_GATE_OFF_CPU },
   { "composed", ARM_COMPOSED },
   { "out-of-domain-refused", ARM_OUT_OF_DOMAIN_REFUSED },
   { "alias-refused", ARM_ALIAS_REFUSED },
   { "ceiling-refused", ARM_CEILING_REFUSED },
   { "gate-table", ARM_GATE_TABLE },
   { "preseeded-agreement", ARM_PRESEEDED_AGREEMENT },
   { "quarantined-refused", ARM_QUARANTINED_REFUSED },
};

static const struct radeon_drm_vk_ioctl_ops *saved_ops;
static struct radeon_drm_vk_ioctl_ops injected_ops;
static unsigned cs_ioctls;

static int
counting_command_write_read(int fd, unsigned long request, void *data,
                            unsigned size)
{
   if (request == DRM_RADEON_CS)
      cs_ioctls++;
   return saved_ops->command_write_read(fd, request, data, size);
}

static bool
file_present(const char *dir, const char *name)
{
   char path[4096];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   struct stat status;
   return stat(path, &status) == 0;
}

#define DEVICE_COMMANDS(f)                                                 \
   f(vkGetDeviceQueue) f(vkCreateBuffer) f(vkAllocateMemory)               \
   f(vkBindBufferMemory) f(vkMapMemory) f(vkUnmapMemory)                   \
   f(vkCreateDescriptorSetLayout) f(vkCreateDescriptorPool)                \
   f(vkAllocateDescriptorSets) f(vkUpdateDescriptorSets)                   \
   f(vkCreatePipelineLayout) f(vkCreateShaderModule)                       \
   f(vkCreateComputePipelines) f(vkCreateCommandPool)                      \
   f(vkAllocateCommandBuffers) f(vkBeginCommandBuffer)                     \
   f(vkCmdBindPipeline) f(vkCmdBindDescriptorSets) f(vkCmdDispatch)        \
   f(vkEndCommandBuffer) f(vkQueueSubmit)
#define DECLARE(name) static PFN_##name name;
DEVICE_COMMANDS(DECLARE)
#undef DECLARE

/* The reference kernel's workgroup is 64 wide, so one group moves 64
 * words: sixteen F32_4 records, the reference pass's geometry.  The
 * in-window words are the small integers 0..63 as binary32 -- FP24
 * fixed points the carrier reproduces; the ceiling arm's 65 groups name
 * 1040 records. */
#define GROUP_WORDS 64u
#define CEILING_GROUPS 65u
#define OUTPUT_SEED 0x5c5c5c5cu

static int
run_arm(enum arm arm, const char *name)
{
   cs_ioctls = 0;
   char manifest_dir[] = "/tmp/r3v-native-compute-gpu-route-XXXXXX";
   assert(mkdtemp(manifest_dir) != NULL);
   setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
   setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
   setenv("R3V_HYBRID_COMPUTE_EXPERIMENTAL", "1", 1);
   const char *verb_gate =
      r300_compute_verb_row(R300_COMPUTE_VERB_IDENTITY_MAP)->gpu_gate;
   uint32_t verb_count = 0;
   const struct r300_compute_verb_row *rows =
      r300_compute_verb_rows(&verb_count);
   for (uint32_t v = 0; v < verb_count; v++)
      unsetenv(rows[v].gpu_gate);
   if (arm == ARM_GATE_TABLE) {
      /* Every gate but the identity's open, the identity's closed by a
       * value other than the literal. */
      for (uint32_t v = 0; v < verb_count; v++)
         setenv(rows[v].gpu_gate, v == R300_COMPUTE_VERB_IDENTITY_MAP ? "yes"
                                                                    : "1", 1);
   } else if (arm != ARM_GATE_OFF_CPU) {
      setenv(verb_gate, "1", 1);
   }

   /* The declared authorization: the reference pass's pinned identity. */
   struct r300_r2vb_fetched_producer_ib reference;
   assert(r300_compute_identity_carrier_reference_emit(&reference) == 0);
   char reference_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);
   assert(reference.ib_size_dwords == R300_COMPUTE_IDENTITY_CARRIER_IB_DWORDS);
   assert(strcmp(reference_digest, R300_COMPUTE_IDENTITY_CARRIER_IB_BLAKE3) ==
          0);
   r300_r2vb_fetched_producer_release(&reference);
   setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", reference_digest, 1);
   struct utsname host;
   assert(uname(&host) == 0);
   setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
   setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
          R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   assert(create_instance(&(VkInstanceCreateInfo){
                             .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo =
                                &(VkApplicationInfo){
                                   .sType =
                                      VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_0,
                                },
                          },
                          NULL, &instance) == VK_SUCCESS);
   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(instance,
                                           "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice create_device =
      (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
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
                      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                      .queueFamilyIndex = 0,
                      .queueCount = 1,
                      .pQueuePriorities = &priority,
                   },
             },
             NULL, &device) == VK_SUCCESS);
#define LOAD(name) name = (PFN_##name)gdpa(device, #name); assert(name);
   DEVICE_COMMANDS(LOAD)
#undef LOAD

   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   native_device->arming_provider = &r3v_native_shim_arming_provider;
   saved_ops = native_device->drm.ops;
   injected_ops = *saved_ops;
   injected_ops.command_write_read = counting_command_write_read;
   native_device->drm.ops = &injected_ops;
   assert((native_device->compute_verb_gates[R300_COMPUTE_VERB_IDENTITY_MAP] !=
           NULL) == (arm != ARM_GATE_OFF_CPU && arm != ARM_GATE_TABLE));
   if (arm == ARM_GATE_TABLE) {
      /* The table read at device creation: the literal opens, every
       * other value stays closed, and a refresh re-reads each gate. */
      for (uint32_t v = 0; v < verb_count; v++) {
         const bool open = native_device->compute_verb_gates[v] != NULL;
         assert(open == (v != R300_COMPUTE_VERB_IDENTITY_MAP));
      }
      static const char *const closed_values[] = { "0", "", "yes", "1 ",
                                                   "01" };
      for (unsigned c = 0; c < sizeof(closed_values) / sizeof(closed_values[0]);
           c++) {
         setenv(verb_gate, closed_values[c], 1);
         r3v_native_device_refresh_delivery_gates(native_device);
         assert(native_device->compute_verb_gates[R300_COMPUTE_VERB_IDENTITY_MAP] ==
                NULL);
      }
      setenv(verb_gate, "1", 1);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(strcmp(native_device->compute_verb_gates
                       [R300_COMPUTE_VERB_IDENTITY_MAP], "1") == 0);
      unsetenv(verb_gate);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(native_device->compute_verb_gates[R300_COMPUTE_VERB_IDENTITY_MAP] ==
             NULL);
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* Input and output: one page each (the alias arm binds both buffers
    * into one page at distinct offsets, ranges disjoint -- the CPU
    * route's admissible shape, the carrier's refused one); the ceiling
    * arm dispatches 65 groups over five-page buffers. */
   const uint32_t groups = arm == ARM_CEILING_REFUSED ? CEILING_GROUPS : 1;
   const uint32_t words = groups * GROUP_WORDS;
   const VkDeviceSize bytes = (VkDeviceSize)words * 4;
   const VkDeviceSize allocation = (bytes + 4095) & ~(VkDeviceSize)4095;
   VkDeviceMemory input_memory = VK_NULL_HANDLE, output_memory = VK_NULL_HANDLE;
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = allocation,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &input_memory) == VK_SUCCESS);
   if (arm == ARM_ALIAS_REFUSED) {
      output_memory = input_memory;
   } else {
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = allocation,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &output_memory) == VK_SUCCESS);
   }
   VkBuffer input_buffer = VK_NULL_HANDLE, output_buffer = VK_NULL_HANDLE;
   for (unsigned b = 0; b < 2; b++) {
      assert(vkCreateBuffer(device,
                            &(VkBufferCreateInfo){
                               .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = bytes,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            },
                            NULL, b == 0 ? &input_buffer : &output_buffer) ==
             VK_SUCCESS);
   }
   const VkDeviceSize output_bind_offset =
      arm == ARM_ALIAS_REFUSED ? 2048 : 0;
   assert(vkBindBufferMemory(device, input_buffer, input_memory, 0) ==
          VK_SUCCESS);
   assert(vkBindBufferMemory(device, output_buffer, output_memory,
                             output_bind_offset) == VK_SUCCESS);
   uint32_t *input_map = NULL, *output_map = NULL;
   assert(vkMapMemory(device, input_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&input_map) == VK_SUCCESS);
   if (arm == ARM_ALIAS_REFUSED) {
      output_map = input_map + 2048 / 4;
   } else {
      assert(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&output_map) == VK_SUCCESS);
   }
   uint32_t *input_words = calloc(words, sizeof(uint32_t));
   assert(input_words != NULL);
   for (uint32_t i = 0; i < words; i++) {
      const float value = (float)(i % GROUP_WORDS);
      memcpy(&input_words[i], &value, sizeof(value));
   }
   if (arm == ARM_OUT_OF_DOMAIN_REFUSED)
      input_words[5] = 0xbf800000u; /* -1.0: outside the window */
   memcpy(input_map, input_words, bytes);
   for (uint32_t i = 0; i < words; i++)
      output_map[i] = arm == ARM_PRESEEDED_AGREEMENT ? input_words[i]
                                                     : OUTPUT_SEED;

   const VkDescriptorSetLayoutBinding bindings[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
   };
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   assert(vkCreateDescriptorSetLayout(
             device,
             &(VkDescriptorSetLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 2,
                .pBindings = bindings,
             },
             NULL, &set_layout) == VK_SUCCESS);
   VkDescriptorPool pool = VK_NULL_HANDLE;
   assert(vkCreateDescriptorPool(
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
             NULL, &pool) == VK_SUCCESS);
   VkDescriptorSet set = VK_NULL_HANDLE;
   assert(vkAllocateDescriptorSets(
             device,
             &(VkDescriptorSetAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &set_layout,
             },
             &set) == VK_SUCCESS);
   const VkDescriptorBufferInfo buffer_infos[2] = {
      { .buffer = input_buffer, .offset = 0, .range = bytes },
      { .buffer = output_buffer, .offset = 0, .range = bytes },
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
   VkPipelineLayout layout = VK_NULL_HANDLE;
   assert(vkCreatePipelineLayout(
             device,
             &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &set_layout,
             },
             NULL, &layout) == VK_SUCCESS);
   VkShaderModule module = VK_NULL_HANDLE;
   assert(vkCreateShaderModule(
             device,
             &(VkShaderModuleCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = sizeof(r3v_reference_identity_map_spirv),
                .pCode = r3v_reference_identity_map_spirv,
             },
             NULL, &module) == VK_SUCCESS);
   VkPipeline pipeline = VK_NULL_HANDLE;
   assert(vkCreateComputePipelines(
             device, VK_NULL_HANDLE, 1,
             &(VkComputePipelineCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage =
                   {
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module,
                      .pName = "main",
                   },
                .layout = layout,
             },
             NULL, &pipeline) == VK_SUCCESS);

   /* One dispatch of the arm's groups over the descriptor ranges the
    * recording proves. */
   VkCommandPool cmd_pool = VK_NULL_HANDLE;
   assert(vkCreateCommandPool(
             device,
             &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = 0,
             },
             NULL, &cmd_pool) == VK_SUCCESS);
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   assert(vkAllocateCommandBuffers(
             device,
             &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = cmd_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
             },
             &cmd) == VK_SUCCESS);
   assert(vkBeginCommandBuffer(
             cmd, &(VkCommandBufferBeginInfo){
                     .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                  }) == VK_SUCCESS);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                           &set, 0, NULL);
   vkCmdDispatch(cmd, groups, 1, 1);
   const VkResult ended = vkEndCommandBuffer(cmd);
   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(cmd);
   assert(ended == VK_SUCCESS);
   assert(native_cmd->deferred_dispatch.pending);
   assert(native_cmd->ib_size_dwords == 0);
   assert(native_cmd->deferred_dispatch.byte_count == bytes);

   const VkResult submitted = vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
      },
      VK_NULL_HANDLE);
   const enum r3v_native_queue_status status =
      r3v_native_queue_submission_status(device);
   native_device->drm.ops = saved_ops;

   bool output_is_seed = true, output_is_input = true;
   for (uint32_t i = 0; i < words; i++) {
      output_is_seed &= output_map[i] == OUTPUT_SEED;
      output_is_input &= output_map[i] == input_words[i];
   }
   printf("%s: result=%d status=%d cs_ioctls=%u ib_dwords=%u output=%s "
          "quarantined=%d\n",
          name, submitted, status, cs_ioctls, native_cmd->ib_size_dwords,
          output_is_input ? "input" : output_is_seed ? "seed" : "other",
          native_device->gpu_compute_quarantined);
   fflush(stdout);

   switch (arm) {
   case ARM_GATE_OFF_CPU:
   case ARM_GATE_TABLE:
      /* The CPU route: the bit copy lands, no IB, no ioctl. */
      assert(submitted == VK_SUCCESS);
      assert(cs_ioctls == 0);
      assert(native_cmd->ib_size_dwords == 0);
      assert(output_is_input);
      assert(!native_device->gpu_compute_quarantined);
      break;
   case ARM_COMPOSED:
   case ARM_PRESEEDED_AGREEMENT:
   case ARM_QUARANTINED_REFUSED: {
      /* The recorded stream is the reference pass and the ioctl ran.
       * The shim wrote nothing: with the seed in place the read-back
       * diverged -- device loss, quarantine, both byte files retained,
       * the seed untouched (the route never writes the output from the
       * host); with the expected bytes pre-seeded the comparator reads
       * agreement -- COMPLETED, no quarantine, the same two files. */
      assert(native_cmd->cell_kind ==
             R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER);
      assert(native_cmd->ib_size_dwords ==
             R300_COMPUTE_IDENTITY_CARRIER_IB_DWORDS);
      char recorded[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      r300_triangle_ib_digest_hex(native_cmd->ib, native_cmd->ib_size_dwords,
                                  recorded);
      assert(strcmp(recorded, R300_COMPUTE_IDENTITY_CARRIER_IB_BLAKE3) == 0);
      assert(native_cmd->reference_count == 3);
      assert(native_cmd->owned_slot != NULL);
      assert(native_cmd->references[1].memory == native_cmd->owned_slot);
      assert(native_cmd->deferred_dispatch.gpu_carrier_delivery);
      assert(native_cmd->deferred_dispatch.gpu_record_count ==
             R300_COMPUTE_IDENTITY_CARRIER_REFERENCE_RECORDS);
      assert(memcmp(native_cmd->deferred_dispatch.gpu_expected, input_words,
                    bytes) == 0);
      assert(cs_ioctls == 1);
      if (arm == ARM_PRESEEDED_AGREEMENT) {
         assert(submitted == VK_SUCCESS);
         assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
         assert(output_is_input);
         assert(!native_device->gpu_compute_quarantined);
      } else {
         assert(submitted == VK_ERROR_DEVICE_LOST);
         assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMITTED);
         assert(output_is_seed);
         assert(native_device->gpu_compute_quarantined);
      }
      assert(file_present(manifest_dir, "ib.bin"));
      assert(file_present(manifest_dir, "gpu_compute_observed.bin"));
      assert(file_present(manifest_dir, "gpu_compute_expected.bin"));
      assert(r3v_native_queue_observed_route(device) ==
             R3V_NATIVE_OBSERVED_ROUTE_COMPUTE_IDENTITY_CARRIER);
      assert(strcmp(r3v_native_observed_route_name(
                       R3V_NATIVE_OBSERVED_ROUTE_COMPUTE_IDENTITY_CARRIER),
                    "compute-identity-carrier") == 0);
      {
         void *slot_map = NULL;
         assert(radeon_drm_vk_bo_map(&native_device->drm,
                                     &native_cmd->owned_slot->bo,
                                     &slot_map) == 0);
         uint32_t slot_expected[64];
         assert(r300_r2vb_fetched_producer_slot_positions(16, slot_expected,
                                                          64) == 0);
         assert(memcmp(slot_map, slot_expected, sizeof(slot_expected)) == 0);
         radeon_drm_vk_bo_unmap(&native_device->drm,
                                &native_cmd->owned_slot->bo, slot_map);
      }
      if (arm == ARM_QUARANTINED_REFUSED) {
         /* A second dispatch recorded after the divergence: its
          * admission refuses on the quarantine with nothing installed
          * -- the runtime marks the queue lost after the first submit,
          * so the admission is driven directly and judged on the
          * recording it leaves. */
         VkCommandBuffer second = VK_NULL_HANDLE;
         assert(vkAllocateCommandBuffers(
                   device,
                   &(VkCommandBufferAllocateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                      .commandPool = cmd_pool,
                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                      .commandBufferCount = 1,
                   },
                   &second) == VK_SUCCESS);
         assert(vkBeginCommandBuffer(
                   second,
                   &(VkCommandBufferBeginInfo){
                      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                   }) == VK_SUCCESS);
         vkCmdBindPipeline(second, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
         vkCmdBindDescriptorSets(second, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 layout, 0, 1, &set, 0, NULL);
         vkCmdDispatch(second, groups, 1, 1);
         assert(vkEndCommandBuffer(second) == VK_SUCCESS);
         struct r3v_native_cmd_buffer *native_second =
            r3v_native_cmd_buffer_from_handle(second);
         assert(native_second->deferred_dispatch.pending);
         const VkResult admitted = r3v_native_deferred_dispatch_admit_gpu(
            native_device, native_second);
         assert(admitted == VK_ERROR_DEVICE_LOST);
         assert(native_second->ib_size_dwords == 0);
         assert(native_second->reference_count == 0);
         assert(native_second->owned_slot == NULL);
         assert(!native_second->deferred_dispatch.gpu_carrier_delivery);
         assert(native_second->deferred_dispatch.gpu_expected == NULL);
      }
      break;
   }
   case ARM_OUT_OF_DOMAIN_REFUSED:
   case ARM_ALIAS_REFUSED:
   case ARM_CEILING_REFUSED:
      /* Each admission refusal stops before any allocation, reference,
       * IB, or write: the recording is as recorded, no ioctl, the output
       * seed in place, the capability unquarantined, no evidence. */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(cs_ioctls == 0);
      assert(native_cmd->ib_size_dwords == 0);
      assert(native_cmd->reference_count == 0);
      assert(native_cmd->owned_slot == NULL);
      assert(!native_cmd->deferred_dispatch.gpu_carrier_delivery);
      assert(output_is_seed);
      assert(!native_device->gpu_compute_quarantined);
      assert(!file_present(manifest_dir, "ib.bin"));
      break;
   }
   free(input_words);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <arm>\n", argv[0]);
      return 2;
   }
   for (unsigned i = 0; i < sizeof(arm_names) / sizeof(arm_names[0]); i++) {
      if (strcmp(argv[1], arm_names[i].name) == 0)
         return run_arm(arm_names[i].arm, arm_names[i].name);
   }
   fprintf(stderr, "unknown arm %s\n", argv[1]);
   return 2;
}
