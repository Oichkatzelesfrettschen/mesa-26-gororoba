/*
 * SPDX-License-Identifier: MIT
 *
 * End-to-end dispatch through the native compute surface under the
 * drm-shim transport: the loader resolves the ICD, the gate-off leg
 * proves the queue family withholds COMPUTE and pipeline creation
 * refuses, and the gate-on leg drives the reference identity-map
 * module through descriptors, recording, and submission to exact
 * output bytes.  Each leg runs in a child process so the
 * R3V_HYBRID_COMPUTE_EXPERIMENTAL latch at physical-device creation
 * is per-leg state.
 */

#include "r3v_native_reference_spirv.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define CHECK(cond, label)                                                   \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", label);                               \
         return 1;                                                           \
      }                                                                      \
   } while (0)

#define DISPATCH_GROUPS 2u
#define WORKGROUP_SIZE 64u
#define DISPATCH_WORDS (DISPATCH_GROUPS * WORKGROUP_SIZE)

static int
create_device(VkInstance *instance_out, VkPhysicalDevice *pdev_out,
              VkDevice *device_out)
{
   const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
   };
   CHECK(vkCreateInstance(&instance_info, NULL, instance_out) == VK_SUCCESS,
         "instance creation");

   uint32_t count = 1;
   VkResult enumerated =
      vkEnumeratePhysicalDevices(*instance_out, &count, pdev_out);
   CHECK((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
            count == 1,
         "physical device enumeration");

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   CHECK(vkCreateDevice(*pdev_out, &device_info, NULL, device_out) ==
            VK_SUCCESS,
         "device creation");
   return 0;
}

static VkQueueFlags
family_flags(VkPhysicalDevice pdev)
{
   uint32_t count = 1;
   VkQueueFamilyProperties properties;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &count, &properties);
   return count == 1 ? properties.queueFlags : 0;
}

/* Gate off: the queue family withholds COMPUTE and compute pipeline
 * creation refuses with the handle cleared.
 */
static int
gate_off_leg(void)
{
   unsetenv("R3V_HYBRID_COMPUTE_EXPERIMENTAL");

   VkInstance instance;
   VkPhysicalDevice pdev;
   VkDevice device;
   if (create_device(&instance, &pdev, &device) != 0)
      return 1;
   CHECK((family_flags(pdev) & VK_QUEUE_COMPUTE_BIT) == 0,
         "gate-off family withholds COMPUTE");

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(r3v_reference_identity_map_spirv),
      .pCode = r3v_reference_identity_map_spirv,
   };
   VkShaderModule module;
   CHECK(vkCreateShaderModule(device, &module_info, NULL, &module) ==
            VK_SUCCESS,
         "gate-off shader module");

   const VkPipelineLayoutCreateInfo empty_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(device, &empty_layout_info, NULL,
                                &layout) == VK_SUCCESS,
         "gate-off pipeline layout");

   VkPipeline pipeline = (VkPipeline)(uintptr_t)0x1;
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = layout,
   };
   VkResult created = vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);
   CHECK(created != VK_SUCCESS && pipeline == VK_NULL_HANDLE,
         "gate-off compute pipeline refuses");
   return 0;
}

static int
bind_storage_buffer(VkDevice device, VkDeviceSize size, VkBuffer *buffer,
                    VkDeviceMemory *memory, void **map)
{
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   CHECK(vkCreateBuffer(device, &buffer_info, NULL, buffer) == VK_SUCCESS,
         "storage buffer creation");

   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, *buffer, &requirements);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      /* Type 0 is the one host-visible native type. */
      .memoryTypeIndex = 0,
   };
   CHECK(vkAllocateMemory(device, &allocate_info, NULL, memory) ==
            VK_SUCCESS,
         "storage memory allocation");
   CHECK(vkBindBufferMemory(device, *buffer, *memory, 0) == VK_SUCCESS,
         "storage buffer bind");
   CHECK(vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, map) ==
            VK_SUCCESS,
         "storage memory map");
   return 0;
}

/* Gate on: the family advertises COMPUTE and the reference module
 * dispatches to exact bytes over two storage buffers.
 */
static int
gate_on_leg(void)
{
   setenv("R3V_HYBRID_COMPUTE_EXPERIMENTAL", "1", 1);

   VkInstance instance;
   VkPhysicalDevice pdev;
   VkDevice device;
   if (create_device(&instance, &pdev, &device) != 0)
      return 1;
   CHECK((family_flags(pdev) & VK_QUEUE_COMPUTE_BIT) != 0,
         "gate-on family advertises COMPUTE");

   const VkDeviceSize buffer_bytes = (DISPATCH_WORDS + 1) * 4;
   VkBuffer input_buffer, output_buffer;
   VkDeviceMemory input_memory, output_memory;
   void *input_map = NULL, *output_map = NULL;
   if (bind_storage_buffer(device, buffer_bytes, &input_buffer,
                           &input_memory, &input_map) != 0 ||
       bind_storage_buffer(device, buffer_bytes, &output_buffer,
                           &output_memory, &output_map) != 0)
      return 1;

   uint32_t *input = input_map;
   uint32_t *output = output_map;
   for (uint32_t i = 0; i < DISPATCH_WORDS + 1; i++) {
      input[i] = 0x1000u ^ (i * 0x9e3779b9u);
      output[i] = 0xdeadbeefu;
   }
   input[7] = 0x7fc00777u; /* NaN payload survives the bit copy. */

   const VkDescriptorSetLayoutBinding bindings[2] = {
      { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
      { .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
   };
   const VkDescriptorSetLayoutCreateInfo set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout set_layout;
   CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL,
                                     &set_layout) == VK_SUCCESS,
         "descriptor set layout");

   const VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 2,
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool pool;
   CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool) ==
            VK_SUCCESS,
         "descriptor pool");

   const VkDescriptorSetAllocateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet set;
   CHECK(vkAllocateDescriptorSets(device, &set_info, &set) == VK_SUCCESS,
         "descriptor set allocation");

   const VkDescriptorBufferInfo buffer_infos[2] = {
      { .buffer = input_buffer, .offset = 0, .range = DISPATCH_WORDS * 4 },
      { .buffer = output_buffer, .offset = 0,
        .range = DISPATCH_WORDS * 4 },
   };
   const VkWriteDescriptorSet writes[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_infos[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_infos[1] },
   };
   vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(r3v_reference_identity_map_spirv),
      .pCode = r3v_reference_identity_map_spirv,
   };
   VkShaderModule module;
   CHECK(vkCreateShaderModule(device, &module_info, NULL, &module) ==
            VK_SUCCESS,
         "shader module");

   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout) ==
            VK_SUCCESS,
         "pipeline layout");

   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = layout,
   };
   VkPipeline pipeline;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                  &pipeline_info, NULL, &pipeline) ==
            VK_SUCCESS,
         "compute pipeline creation");

   /* The scatter module refuses at creation, so no admitted pipeline
    * reaches an unmatched no-op at dispatch.
    */
   const VkShaderModuleCreateInfo scatter_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(r3v_reference_scatter_reject_spirv),
      .pCode = r3v_reference_scatter_reject_spirv,
   };
   VkShaderModule scatter_module;
   CHECK(vkCreateShaderModule(device, &scatter_info, NULL,
                              &scatter_module) == VK_SUCCESS,
         "scatter shader module");
   VkComputePipelineCreateInfo scatter_pipeline_info = pipeline_info;
   scatter_pipeline_info.stage.module = scatter_module;
   VkPipeline scatter_pipeline = (VkPipeline)(uintptr_t)0x1;
   VkResult scatter_created = vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &scatter_pipeline_info, NULL,
      &scatter_pipeline);
   CHECK(scatter_created != VK_SUCCESS &&
            scatter_pipeline == VK_NULL_HANDLE,
         "scatter pipeline refuses at creation");

   const VkCommandPoolCreateInfo cmd_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool cmd_pool;
   CHECK(vkCreateCommandPool(device, &cmd_pool_info, NULL, &cmd_pool) ==
            VK_SUCCESS,
         "command pool");
   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd) == VK_SUCCESS,
         "command buffer allocation");

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK(vkBeginCommandBuffer(cmd, &begin_info) == VK_SUCCESS,
         "command buffer begin");
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                           1, &set, 0, NULL);
   vkCmdDispatch(cmd, DISPATCH_GROUPS, 1, 1);
   CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS, "command buffer end");

   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE) ==
            VK_SUCCESS,
         "queue submit");
   CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS, "queue wait idle");

   CHECK(memcmp(output, input, DISPATCH_WORDS * 4) == 0,
         "output words equal input words");
   CHECK(output[DISPATCH_WORDS] == 0xdeadbeefu,
         "word past the dispatch range stays poisoned");

   /* A dispatch past the bound ranges poisons the recording, so the
    * submit refuses instead of moving bytes the descriptor never
    * covered.
    */
   CHECK(vkResetCommandPool(device, cmd_pool, 0) == VK_SUCCESS,
         "command pool reset");
   CHECK(vkBeginCommandBuffer(cmd, &begin_info) == VK_SUCCESS,
         "oversized dispatch begin");
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                           1, &set, 0, NULL);
   vkCmdDispatch(cmd, DISPATCH_GROUPS + 1, 1, 1);
   CHECK(vkEndCommandBuffer(cmd) != VK_SUCCESS,
         "oversized dispatch poisons the recording");

   return 0;
}

static int
run_leg(const char *label, int (*body)(void))
{
   fflush(NULL);
   pid_t pid = fork();
   if (pid == 0)
      _exit(body() == 0 ? 0 : 1);
   int status = 0;
   if (pid < 0 || waitpid(pid, &status, 0) != pid ||
       !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "FAIL: %s leg\n", label);
      return 1;
   }
   return 0;
}

int main(void)
{
   unsigned failures = 0;
   failures += run_leg("gate-off", gate_off_leg);
   failures += run_leg("gate-on", gate_on_leg);
   if (failures != 0) {
      fprintf(stderr, "r3v-native-compute-dispatch: %u failures\n",
              failures);
      return 1;
   }
   printf("r3v-native-compute-dispatch: all cases passed\n");
   return 0;
}
