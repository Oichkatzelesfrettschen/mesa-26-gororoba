/* SPDX-License-Identifier: MIT */

/*
 * Loader-only RS485M ZMASK lifecycle application.  The application links the
 * Vulkan loader and reaches the selected ICD exclusively through public Vulkan
 * handles and entrypoints.  Each mode records every clear, draw, materializing
 * copy, and aspect export into one primary command buffer and submits it once.
 */

#include "r3v_native_reference_spirv.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define TARGET_WIDTH 64u
#define TARGET_HEIGHT 64u
#define PIXEL_COUNT (TARGET_WIDTH * TARGET_HEIGHT)
#define COLOR_BYTES (TARGET_WIDTH * (TARGET_HEIGHT + 1u) * sizeof(uint32_t))
#define DEPTH_BACKING_BYTES 28672u
#define DEPTH_EXPORT_BYTES (PIXEL_COUNT * sizeof(uint32_t))
#define STENCIL_EXPORT_BYTES PIXEL_COUNT
#define EXPORT_GUARD_BYTES 64u
#define DEPTH_EXPORT_STORAGE_BYTES                                         \
   (EXPORT_GUARD_BYTES + DEPTH_EXPORT_BYTES + EXPORT_GUARD_BYTES)
#define STENCIL_EXPORT_STORAGE_BYTES                                       \
   (EXPORT_GUARD_BYTES + STENCIL_EXPORT_BYTES + EXPORT_GUARD_BYTES)
#define VERTEX_BYTES 4096u
#define DEPTH_BACKING_BASE 2048u
#define BACKING_DEPTH_CODE 0x200000u
#define READ_CLEAR_DEPTH_CODE 0x800000u
#define IMAGE_B_CLEAR_DEPTH_CODE 0xbfffffu
#define UPDATE_DEPTH_CODE 0x400000u
#define BACKING_STENCIL 0x5au
#define IMAGE_B_CLEAR_STENCIL 0xa5u
#define UPDATE_WIDTH 16u
#define UPDATE_HEIGHT 20u
#define BACKING_GUARD 0xa3u
#define COLOR_SENTINEL 0xa5a5a5a5u
#define DRAW_COLOR R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM
#define FAR_FAIL_COLOR 0xff0000ffu
#define FENCE_TIMEOUT_NS UINT64_C(5000000000)
#define ATI_VENDOR_ID 0x1002u
#define RS485M_DEVICE_ID 0x5974u

enum application_mode {
   MODE_READ_MATERIALIZE_EXPORT,
   MODE_ABA_MATERIALIZE_EXPORT,
};

struct allocation {
   VkBuffer buffer;
   VkDeviceMemory memory;
   VkDeviceSize size;
};

struct depth_target {
   VkImage image;
   VkDeviceMemory memory;
   VkDeviceSize size;
   VkImageView view;
};

struct application {
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkPhysicalDeviceMemoryProperties memory_properties;
   VkDevice device;
   VkQueue queue;
   VkCommandPool command_pool;
   VkCommandBuffer command_buffer;
   VkFence fence;
   VkImage color_image;
   VkDeviceMemory color_memory;
   VkDeviceSize color_size;
   VkImageView color_view;
   struct allocation vertex;
   struct depth_target depth[2];
   struct allocation depth_export[2];
   struct allocation stencil_export[2];
   VkRenderPass render_pass;
   VkFramebuffer framebuffer;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline[2];
   uint32_t pipeline_count;
   uint32_t depth_count;
   uint32_t queue_submit_calls_attempted;
   uint32_t queue_submits_accepted;
   uint32_t queue_executions_completed;
   VkResult queue_submit_result;
   bool expected_icd_dso_mapped;
};

enum submission_result {
   SUBMISSION_REFUSED,
   SUBMISSION_COMPLETED,
   SUBMISSION_WAIT_INCOMPLETE,
};

struct oracle_results {
   bool expected_icd_dso_mapped;
   bool submission_contract;
   bool read_initial_backing;
   bool read_final_backing;
   bool read_exported_depth;
   bool read_exported_stencil;
   bool read_export_guards;
   bool read_observed_color;
   bool aba_a_initial_backing;
   bool aba_b_initial_backing;
   bool aba_a_final_backing;
   bool aba_b_final_backing;
   bool aba_a_exported_depth;
   bool aba_b_exported_depth;
   bool aba_a_exported_stencil;
   bool aba_b_exported_stencil;
   bool aba_a_export_guards;
   bool aba_b_export_guards;
   bool aba_a_stencil_preserved;
   bool aba_update_extent;
   bool artifacts_retained;
};

struct preparation_results {
   bool expected_icd_dso_mapped;
   bool authorization_declarations_absent;
   bool submission_refused;
   bool submit_object_retained;
   bool attempt_token_absent;
   bool shim_counter_available;
   uint64_t shim_cs_ioctls;
   bool shim_hyperz_state_available;
   bool shim_hyperz_unowned_before;
   uint64_t shim_hyperz_acquire_ioctls;
   uint64_t shim_hyperz_release_ioctls;
   bool shim_hyperz_owned_after;
};

struct shim_hyperz_state {
   uint64_t acquire_ioctls;
   uint64_t release_ioctls;
   bool owned;
};

static bool
exact_gate_open(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
environment_absent(const char *name)
{
   return getenv(name) == NULL;
}

static bool
preparation_authorization_declarations_absent(void)
{
   static const char *const names[] = {
      "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
      "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
      "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
   };
   for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); index++) {
      if (!environment_absent(names[index]))
         return false;
   }
   return true;
}

static bool
read_shim_cs_count(uint64_t *count)
{
   uint64_t (*counter)(void) = (uint64_t (*)(void))dlsym(
      RTLD_DEFAULT, "drm_shim_test_radeon_cs_ioctls");
   if (counter == NULL)
      return false;
   *count = counter();
   return true;
}

static bool
read_shim_hyperz_state(struct shim_hyperz_state *state)
{
   uint64_t (*acquire_counter)(void) = (uint64_t (*)(void))dlsym(
      RTLD_DEFAULT, "drm_shim_test_radeon_hyperz_acquire_ioctls");
   uint64_t (*release_counter)(void) = (uint64_t (*)(void))dlsym(
      RTLD_DEFAULT, "drm_shim_test_radeon_hyperz_release_ioctls");
   bool (*owned)(void) = (bool (*)(void))dlsym(
      RTLD_DEFAULT, "drm_shim_test_radeon_hyperz_owned");
   if (acquire_counter == NULL || release_counter == NULL || owned == NULL)
      return false;
   *state = (struct shim_hyperz_state){
      .acquire_ioctls = acquire_counter(),
      .release_ioctls = release_counter(),
      .owned = owned(),
   };
   return true;
}

static bool
expected_icd_dso_mapped(void)
{
   const char *expected_dso = getenv("R3V_EXPECTED_ICD_DSO");
   if (expected_dso == NULL || expected_dso[0] == '\0')
      return false;

   FILE *maps = fopen("/proc/self/maps", "r");
   if (maps == NULL)
      return false;
   char line[4096];
   bool found = false;
   while (fgets(line, sizeof(line), maps) != NULL) {
      if (strstr(line, expected_dso) != NULL) {
         found = true;
         break;
      }
   }
   if (fclose(maps) != 0)
      return false;
   return found;
}

static bool
select_memory_type(const struct application *application,
                   uint32_t memory_type_bits,
                   VkMemoryPropertyFlags required_properties,
                   uint32_t *memory_type_index)
{
   for (uint32_t index = 0u;
        index < application->memory_properties.memoryTypeCount; index++) {
      const uint32_t type_bit = UINT32_C(1) << index;
      const VkMemoryPropertyFlags properties =
         application->memory_properties.memoryTypes[index].propertyFlags;
      if ((memory_type_bits & type_bit) != 0u &&
          (properties & required_properties) == required_properties) {
         *memory_type_index = index;
         return true;
      }
   }
   return false;
}

static bool
create_buffer_allocation(struct application *application, VkDeviceSize size,
                         VkBufferUsageFlags usage,
                         struct allocation *allocation)
{
   VkResult result = vkCreateBuffer(
      application->device,
      &(VkBufferCreateInfo){
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = size,
         .usage = usage,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      },
      NULL, &allocation->buffer);
   if (result != VK_SUCCESS)
      return false;

   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(application->device, allocation->buffer,
                                 &requirements);
   uint32_t memory_type_index;
   const VkMemoryPropertyFlags host_properties =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   if (!select_memory_type(application, requirements.memoryTypeBits,
                           host_properties, &memory_type_index))
      return false;
   result = vkAllocateMemory(
      application->device,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type_index,
      },
      NULL, &allocation->memory);
   if (result != VK_SUCCESS)
      return false;
   allocation->size = requirements.size;
   return vkBindBufferMemory(application->device, allocation->buffer,
                             allocation->memory, 0u) == VK_SUCCESS;
}

static bool
create_depth_target(struct application *application,
                    struct depth_target *target)
{
   VkResult result = vkCreateImage(
      application->device,
      &(VkImageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_D24_UNORM_S8_UINT,
         .extent = {TARGET_WIDTH, TARGET_HEIGHT, 1u},
         .mipLevels = 1u,
         .arrayLayers = 1u,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      },
      NULL, &target->image);
   if (result != VK_SUCCESS)
      return false;

   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(application->device, target->image,
                                &requirements);
   uint32_t memory_type_index;
   const VkMemoryPropertyFlags host_properties =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   if (requirements.size != DEPTH_BACKING_BYTES ||
       !select_memory_type(application, requirements.memoryTypeBits,
                           host_properties, &memory_type_index))
      return false;
   result = vkAllocateMemory(
      application->device,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type_index,
      },
      NULL, &target->memory);
   if (result != VK_SUCCESS)
      return false;
   target->size = requirements.size;
   result = vkBindImageMemory(application->device, target->image,
                              target->memory, 0u);
   if (result != VK_SUCCESS)
      return false;
   return vkCreateImageView(
             application->device,
             &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = target->image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_D24_UNORM_S8_UINT,
                .subresourceRange = {
                   .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                 VK_IMAGE_ASPECT_STENCIL_BIT,
                   .levelCount = 1u,
                   .layerCount = 1u,
                },
             },
             NULL, &target->view) == VK_SUCCESS;
}

static bool
create_color_target(struct application *application)
{
   VkResult result = vkCreateImage(
      application->device,
      &(VkImageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .extent = {TARGET_WIDTH, TARGET_HEIGHT, 1u},
         .mipLevels = 1u,
         .arrayLayers = 1u,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      },
      NULL, &application->color_image);
   if (result != VK_SUCCESS)
      return false;

   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(application->device, application->color_image,
                                &requirements);
   uint32_t memory_type_index;
   const VkMemoryPropertyFlags host_properties =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   if (requirements.size != COLOR_BYTES ||
       !select_memory_type(application, requirements.memoryTypeBits,
                           host_properties, &memory_type_index))
      return false;
   result = vkAllocateMemory(
      application->device,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type_index,
      },
      NULL, &application->color_memory);
   if (result != VK_SUCCESS)
      return false;
   application->color_size = requirements.size;
   result = vkBindImageMemory(application->device, application->color_image,
                              application->color_memory, 0u);
   if (result != VK_SUCCESS)
      return false;
   return vkCreateImageView(
             application->device,
             &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = application->color_image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_B8G8R8A8_UNORM,
                .subresourceRange = {
                   .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                   .levelCount = 1u,
                   .layerCount = 1u,
                },
             },
             NULL, &application->color_view) == VK_SUCCESS;
}

static bool
create_vulkan_context(struct application *application, uint32_t depth_count)
{
   if (!exact_gate_open("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED") ||
       !exact_gate_open("R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL")) {
      fprintf(stderr,
              "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1 and "
              "R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL=1 are required\n");
      return false;
   }
   VkResult result = vkCreateInstance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &application->instance);
   if (result != VK_SUCCESS)
      return false;

   uint32_t physical_device_count = 1u;
   result = vkEnumeratePhysicalDevices(application->instance,
                                       &physical_device_count,
                                       &application->physical_device);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       physical_device_count != 1u ||
       application->physical_device == VK_NULL_HANDLE)
      return false;
   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(application->physical_device, &properties);
   if (properties.vendorID != ATI_VENDOR_ID ||
       properties.deviceID != RS485M_DEVICE_ID)
      return false;
   vkGetPhysicalDeviceMemoryProperties(application->physical_device,
                                       &application->memory_properties);

   const float priority = 1.0f;
   result = vkCreateDevice(
      application->physical_device,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1u,
         .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0u,
            .queueCount = 1u,
            .pQueuePriorities = &priority,
         },
      },
      NULL, &application->device);
   if (result != VK_SUCCESS)
      return false;
   vkGetDeviceQueue(application->device, 0u, 0u, &application->queue);
   application->expected_icd_dso_mapped = expected_icd_dso_mapped();
   if (application->queue == VK_NULL_HANDLE ||
       !application->expected_icd_dso_mapped)
      return false;

   result = vkCreateCommandPool(
      application->device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0u,
      },
      NULL, &application->command_pool);
   if (result != VK_SUCCESS)
      return false;
   result = vkAllocateCommandBuffers(
      application->device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = application->command_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1u,
      },
      &application->command_buffer);
   if (result != VK_SUCCESS)
      return false;
   result = vkCreateFence(
      application->device,
      &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL,
      &application->fence);
   if (result != VK_SUCCESS || !create_color_target(application))
      return false;

   application->depth_count = depth_count;
   for (uint32_t index = 0u; index < depth_count; index++) {
      if (!create_depth_target(application, &application->depth[index]) ||
          !create_buffer_allocation(application, DEPTH_EXPORT_STORAGE_BYTES,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &application->depth_export[index]) ||
          !create_buffer_allocation(application, STENCIL_EXPORT_STORAGE_BYTES,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &application->stencil_export[index]))
         return false;
   }
   return create_buffer_allocation(application, VERTEX_BYTES,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   &application->vertex);
}

static bool
create_render_objects(struct application *application, bool depth_write)
{
   const VkImageLayout depth_layout =
      depth_write ? VK_IMAGE_LAYOUT_GENERAL
                  : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   const VkAttachmentDescription attachments[2] = {
      {
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
      },
      {
         .format = VK_FORMAT_D24_UNORM_S8_UINT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = depth_layout,
         .finalLayout = depth_layout,
      },
   };
   const VkAttachmentReference color_reference = {
      .attachment = 0u,
      .layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   const VkAttachmentReference depth_reference = {
      .attachment = 1u,
      .layout = depth_layout,
   };
   VkResult result = vkCreateRenderPass(
      application->device,
      &(VkRenderPassCreateInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 2u,
         .pAttachments = attachments,
         .subpassCount = 1u,
         .pSubpasses = &(VkSubpassDescription){
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1u,
            .pColorAttachments = &color_reference,
            .pDepthStencilAttachment = &depth_reference,
         },
      },
      NULL, &application->render_pass);
   if (result != VK_SUCCESS)
      return false;

   const VkImageView framebuffer_attachments[2] = {
      application->color_view,
      application->depth[0].view,
   };
   result = vkCreateFramebuffer(
      application->device,
      &(VkFramebufferCreateInfo){
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = application->render_pass,
         .attachmentCount = 2u,
         .pAttachments = framebuffer_attachments,
         .width = TARGET_WIDTH,
         .height = TARGET_HEIGHT,
         .layers = 1u,
      },
      NULL, &application->framebuffer);
   if (result != VK_SUCCESS)
      return false;

   VkShaderModule vertex_shader = VK_NULL_HANDLE;
   VkShaderModule fragment_shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   result = vkCreateShaderModule(
      application->device,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(r3v_reference_vertex_spirv),
         .pCode = r3v_reference_vertex_spirv,
      },
      NULL, &vertex_shader);
   if (result == VK_SUCCESS)
      result = vkCreateShaderModule(
         application->device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(r3v_reference_fragment_spirv),
            .pCode = r3v_reference_fragment_spirv,
         },
         NULL, &fragment_shaders[0]);
   if (result == VK_SUCCESS && !depth_write)
      result = vkCreateShaderModule(
         application->device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(r3v_reference_fragment_blue_spirv),
            .pCode = r3v_reference_fragment_blue_spirv,
         },
         NULL, &fragment_shaders[1]);
   if (result == VK_SUCCESS)
      result = vkCreatePipelineLayout(
         application->device,
         &(VkPipelineLayoutCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         },
         NULL, &application->pipeline_layout);

   VkPipelineShaderStageCreateInfo stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex_shader,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment_shaders[0],
         .pName = "main",
      },
   };
   const VkVertexInputBindingDescription binding = {
      .binding = 0u,
      .stride = 16u,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   const VkVertexInputAttributeDescription attribute = {
      .location = 0u,
      .binding = 0u,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
   };
   const VkGraphicsPipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2u,
            .pStages = stages,
            .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
               .vertexBindingDescriptionCount = 1u,
               .pVertexBindingDescriptions = &binding,
               .vertexAttributeDescriptionCount = 1u,
               .pVertexAttributeDescriptions = &attribute,
            },
            .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
               .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            },
            .pViewportState = &(VkPipelineViewportStateCreateInfo){
               .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
               .viewportCount = 1u,
               .pViewports = &(VkViewport){
                  .width = (float)TARGET_WIDTH,
                  .height = (float)TARGET_HEIGHT,
                  .maxDepth = 1.0f,
               },
               .scissorCount = 1u,
               .pScissors = &(VkRect2D){
                  .extent = {TARGET_WIDTH, TARGET_HEIGHT},
               },
            },
            .pRasterizationState =
               &(VkPipelineRasterizationStateCreateInfo){
                  .sType =
                     VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                  .polygonMode = VK_POLYGON_MODE_FILL,
                  .cullMode = VK_CULL_MODE_NONE,
                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                  .lineWidth = 1.0f,
               },
            .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            },
            .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
               .depthTestEnable = VK_TRUE,
               .depthWriteEnable = depth_write,
               .depthCompareOp = VK_COMPARE_OP_LESS,
            },
            .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
               .attachmentCount = 1u,
               .pAttachments = &(VkPipelineColorBlendAttachmentState){
                  .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                    VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT,
               },
            },
            .layout = application->pipeline_layout,
            .renderPass = application->render_pass,
         };
   application->pipeline_count = depth_write ? 1u : 2u;
   for (uint32_t index = 0u;
        result == VK_SUCCESS && index < application->pipeline_count; index++) {
      stages[1].module = fragment_shaders[index];
      result = vkCreateGraphicsPipelines(
         application->device, VK_NULL_HANDLE, 1u, &pipeline_info, NULL,
         &application->pipeline[index]);
   }
   for (uint32_t index = 0u; index < 2u; index++) {
      if (fragment_shaders[index] != VK_NULL_HANDLE)
         vkDestroyShaderModule(application->device, fragment_shaders[index],
                               NULL);
   }
   if (vertex_shader != VK_NULL_HANDLE)
      vkDestroyShaderModule(application->device, vertex_shader, NULL);
   return result == VK_SUCCESS;
}

static uint32_t
depth_backing_offset(uint32_t x, uint32_t y)
{
   const uint32_t local_x = (x >> 2u) & 7u;
   const uint32_t local_y = (y >> 1u) & 7u;
   const uint32_t macro_x = x >> 5u;
   const uint32_t macro_y = y >> 4u;
   const uint32_t local_block =
      ((local_x & 1u) << 0u) | ((local_y & 1u) << 1u) |
      ((((local_x >> 1u) ^ (local_y >> 2u)) & 1u) << 2u) |
      ((((local_x >> 2u) ^ (local_y >> 1u)) & 1u) << 3u) |
      ((((local_x >> 2u) ^ macro_y) & 1u) << 4u) |
      ((((local_y >> 2u) ^ macro_x) & 1u) << 5u);
   const uint32_t lane = ((y & 1u) << 2u) | (x & 3u);
   return DEPTH_BACKING_BASE + 2048u * (macro_y * 2u + macro_x) +
          32u * local_block + 4u * lane;
}

static bool
seed_depth_target(struct application *application,
                  const struct depth_target *target)
{
   uint8_t *mapped = NULL;
   if (vkMapMemory(application->device, target->memory, 0u, target->size, 0u,
                   (void **)&mapped) != VK_SUCCESS)
      return false;
   memset(mapped, BACKING_GUARD, (size_t)target->size);
   const uint32_t backing_word =
      (BACKING_DEPTH_CODE << 8u) | BACKING_STENCIL;
   for (uint32_t y = 0u; y < TARGET_HEIGHT; y++) {
      for (uint32_t x = 0u; x < TARGET_WIDTH; x++) {
         const uint32_t offset = depth_backing_offset(x, y);
         memcpy(mapped + offset, &backing_word, sizeof(backing_word));
      }
   }
   vkUnmapMemory(application->device, target->memory);
   return true;
}

static bool
seed_host_allocations(struct application *application)
{
   uint32_t *color_words = NULL;
   if (vkMapMemory(application->device, application->color_memory, 0u,
                   application->color_size, 0u,
                   (void **)&color_words) != VK_SUCCESS)
      return false;
   for (VkDeviceSize index = 0u;
        index < application->color_size / sizeof(*color_words); index++)
      color_words[index] = COLOR_SENTINEL;
   vkUnmapMemory(application->device, application->color_memory);

   float vertices[24] = {
      -1.0f, -1.0f, 0.25f, 1.0f,
      3.0f,  -1.0f, 0.25f, 1.0f,
      -1.0f, 3.0f,  0.25f, 1.0f,
      -1.0f, -1.0f, 0.75f, 1.0f,
      3.0f,  -1.0f, 0.75f, 1.0f,
      -1.0f, 3.0f,  0.75f, 1.0f,
   };
   if (application->depth_count == 2u) {
      vertices[4] = -1.0f + 4.0f * UPDATE_WIDTH / TARGET_WIDTH;
      vertices[9] = -1.0f + 4.0f * UPDATE_HEIGHT / TARGET_HEIGHT;
   }
   void *vertex_map = NULL;
   if (vkMapMemory(application->device, application->vertex.memory, 0u,
                   application->vertex.size, 0u,
                   &vertex_map) != VK_SUCCESS)
      return false;
   memcpy(vertex_map, vertices, sizeof(vertices));
   vkUnmapMemory(application->device, application->vertex.memory);

   for (uint32_t index = 0u; index < application->depth_count; index++) {
      if (!seed_depth_target(application, &application->depth[index]))
         return false;
      void *depth_export = NULL;
      void *stencil_export = NULL;
      if (vkMapMemory(application->device,
                      application->depth_export[index].memory, 0u,
                      application->depth_export[index].size, 0u,
                      &depth_export) != VK_SUCCESS)
         return false;
      memset(depth_export, BACKING_GUARD,
             (size_t)application->depth_export[index].size);
      vkUnmapMemory(application->device,
                    application->depth_export[index].memory);
      if (vkMapMemory(application->device,
                      application->stencil_export[index].memory, 0u,
                      application->stencil_export[index].size, 0u,
                      &stencil_export) != VK_SUCCESS)
         return false;
      memset(stencil_export, BACKING_GUARD,
             (size_t)application->stencil_export[index].size);
      vkUnmapMemory(application->device,
                    application->stencil_export[index].memory);
   }
   return true;
}

static const VkImageSubresourceRange depth_stencil_range = {
   .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
   .levelCount = 1u,
   .layerCount = 1u,
};

static void
record_fast_clear(VkCommandBuffer command_buffer, VkImage image, float depth,
                  uint32_t stencil)
{
   const VkImageMemoryBarrier transition = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = depth_stencil_range,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u,
                        NULL, 1u, &transition);
   vkCmdClearDepthStencilImage(
      command_buffer, image, VK_IMAGE_LAYOUT_GENERAL,
      &(VkClearDepthStencilValue){.depth = depth, .stencil = stencil}, 1u,
      &depth_stencil_range);
}

static void
record_draw(struct application *application, uint32_t width, uint32_t height,
            VkAccessFlags depth_access, VkImageLayout depth_layout,
            bool record_fail_discriminator)
{
   const VkImageMemoryBarrier clear_to_draw = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = depth_access,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = depth_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = application->depth[0].image,
      .subresourceRange = depth_stencil_range,
   };
   vkCmdPipelineBarrier(application->command_buffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        0u, 0u, NULL, 0u, NULL, 1u, &clear_to_draw);
   const VkClearValue clear_values[2] = {
      {.color = {.float32 = {1.0f, 0.0f, 0.0f, 1.0f}}},
      {.depthStencil = {.depth = 0.0f, .stencil = 0u}},
   };
   vkCmdBeginRenderPass(
      application->command_buffer,
      &(VkRenderPassBeginInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = application->render_pass,
         .framebuffer = application->framebuffer,
         .renderArea = {.extent = {width, height}},
         .clearValueCount = 2u,
         .pClearValues = clear_values,
      },
      VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(application->command_buffer,
                     VK_PIPELINE_BIND_POINT_GRAPHICS,
                     application->pipeline[0]);
   vkCmdBindVertexBuffers(application->command_buffer, 0u, 1u,
                          &application->vertex.buffer,
                          &(VkDeviceSize){0u});
   vkCmdDraw(application->command_buffer, 3u, 1u, 0u, 0u);
   if (record_fail_discriminator) {
      vkCmdBindPipeline(application->command_buffer,
                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                        application->pipeline[1]);
      vkCmdDraw(application->command_buffer, 3u, 1u, 3u, 0u);
   }
   vkCmdEndRenderPass(application->command_buffer);
}

static void
record_aspect_exports(struct application *application, uint32_t index,
                      VkPipelineStageFlags producer_stage,
                      VkAccessFlags producer_access,
                      VkImageLayout source_layout)
{
   const VkImageMemoryBarrier export_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = producer_access,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = source_layout,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = application->depth[index].image,
      .subresourceRange = depth_stencil_range,
   };
   vkCmdPipelineBarrier(application->command_buffer, producer_stage,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u,
                        NULL, 1u, &export_barrier);
   const VkBufferImageCopy depth_region = {
      .bufferOffset = EXPORT_GUARD_BYTES,
      .bufferRowLength = TARGET_WIDTH,
      .bufferImageHeight = TARGET_HEIGHT,
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .layerCount = 1u,
      },
      .imageExtent = {TARGET_WIDTH, TARGET_HEIGHT, 1u},
   };
   vkCmdCopyImageToBuffer(application->command_buffer,
                          application->depth[index].image,
                          VK_IMAGE_LAYOUT_GENERAL,
                          application->depth_export[index].buffer, 1u,
                          &depth_region);
   const VkBufferImageCopy stencil_region = {
      .bufferOffset = EXPORT_GUARD_BYTES,
      .bufferRowLength = TARGET_WIDTH,
      .bufferImageHeight = TARGET_HEIGHT,
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
         .layerCount = 1u,
      },
      .imageExtent = {TARGET_WIDTH, TARGET_HEIGHT, 1u},
   };
   vkCmdCopyImageToBuffer(application->command_buffer,
                          application->depth[index].image,
                          VK_IMAGE_LAYOUT_GENERAL,
                          application->stencil_export[index].buffer, 1u,
                          &stencil_region);
}

static bool
record_application(struct application *application, enum application_mode mode)
{
   VkResult result = vkBeginCommandBuffer(
      application->command_buffer,
      &(VkCommandBufferBeginInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      });
   if (result != VK_SUCCESS)
      return false;

   record_fast_clear(application->command_buffer,
                     application->depth[0].image, 0.5f, 0x15au);
   if (mode == MODE_READ_MATERIALIZE_EXPORT) {
      record_draw(application, TARGET_WIDTH, TARGET_HEIGHT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true);
      record_aspect_exports(
         application, 0u,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
   } else {
      record_fast_clear(application->command_buffer,
                        application->depth[1].image, 0.75f, 0x1a5u);
      record_draw(application, TARGET_WIDTH, TARGET_HEIGHT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, false);
      record_aspect_exports(
         application, 0u,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_IMAGE_LAYOUT_GENERAL);
      record_aspect_exports(application, 1u,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_GENERAL);
   }
   result = vkEndCommandBuffer(application->command_buffer);
   if (result != VK_SUCCESS)
      fprintf(stderr, "vkEndCommandBuffer failed: %d\n", result);
   return result == VK_SUCCESS;
}

static enum submission_result
submit_once(struct application *application)
{
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1u,
      .pCommandBuffers = &application->command_buffer,
   };
   application->queue_submit_calls_attempted++;
   application->queue_submit_result =
      vkQueueSubmit(application->queue, 1u, &submit, application->fence);
   if (application->queue_submit_result != VK_SUCCESS)
      return SUBMISSION_REFUSED;
   application->queue_submits_accepted++;
   if (vkWaitForFences(application->device, 1u, &application->fence, VK_TRUE,
                       FENCE_TIMEOUT_NS) != VK_SUCCESS)
      return SUBMISSION_WAIT_INCOMPLETE;
   application->queue_executions_completed++;
   return SUBMISSION_COMPLETED;
}

static bool
backing_matches(const uint8_t *backing, uint32_t default_depth,
                uint8_t stencil, bool apply_update, uint32_t *updated_pixels)
{
   bool used[DEPTH_BACKING_BYTES] = {false};
   uint32_t update_count = 0u;
   for (uint32_t y = 0u; y < TARGET_HEIGHT; y++) {
      for (uint32_t x = 0u; x < TARGET_WIDTH; x++) {
         const bool updated =
            apply_update && x < UPDATE_WIDTH && y < UPDATE_HEIGHT;
         const uint32_t expected_depth =
            updated ? UPDATE_DEPTH_CODE : default_depth;
         const uint32_t expected_word =
            (expected_depth << 8u) | stencil;
         const uint32_t offset = depth_backing_offset(x, y);
         uint32_t observed_word;
         memcpy(&observed_word, backing + offset, sizeof(observed_word));
         if (observed_word != expected_word)
            return false;
         for (uint32_t byte = 0u; byte < sizeof(observed_word); byte++)
            used[offset + byte] = true;
         update_count += updated;
      }
   }
   for (uint32_t offset = 0u; offset < DEPTH_BACKING_BYTES; offset++) {
      if (!used[offset] && backing[offset] != BACKING_GUARD)
         return false;
   }
   if (updated_pixels != NULL)
      *updated_pixels = update_count;
   return true;
}

static bool
exported_depth_matches(const uint8_t *bytes, uint32_t default_depth,
                       bool apply_update, uint32_t *updated_pixels)
{
   uint32_t update_count = 0u;
   for (uint32_t y = 0u; y < TARGET_HEIGHT; y++) {
      for (uint32_t x = 0u; x < TARGET_WIDTH; x++) {
         const bool updated =
            apply_update && x < UPDATE_WIDTH && y < UPDATE_HEIGHT;
         const uint32_t expected_depth =
            updated ? UPDATE_DEPTH_CODE : default_depth;
         uint32_t observed_word;
         memcpy(&observed_word,
                bytes + sizeof(observed_word) * (y * TARGET_WIDTH + x),
                sizeof(observed_word));
         if ((observed_word & 0x00ffffffu) != expected_depth)
            return false;
         update_count += updated;
      }
   }
   if (updated_pixels != NULL)
      *updated_pixels = update_count;
   return true;
}

static bool
exported_stencil_matches(const uint8_t *bytes, uint8_t stencil)
{
   for (uint32_t index = 0u; index < STENCIL_EXPORT_BYTES; index++) {
      if (bytes[index] != stencil)
         return false;
   }
   return true;
}

static bool
export_guards_match(const uint8_t *bytes, size_t payload_size)
{
   for (size_t index = 0u; index < EXPORT_GUARD_BYTES; index++) {
      if (bytes[index] != BACKING_GUARD ||
          bytes[EXPORT_GUARD_BYTES + payload_size + index] != BACKING_GUARD)
         return false;
   }
   return true;
}

static bool
read_color_matches(const uint32_t *words)
{
   for (uint32_t index = 0u; index < PIXEL_COUNT; index++) {
      if (words[index] != DRAW_COLOR)
         return false;
   }
   for (uint32_t index = PIXEL_COUNT;
        index < COLOR_BYTES / sizeof(*words); index++) {
      if (words[index] != COLOR_SENTINEL)
         return false;
   }
   return true;
}

static bool
validate_oracle_discriminators(void)
{
   uint32_t color_words[COLOR_BYTES / sizeof(uint32_t)];
   for (uint32_t index = 0u; index < PIXEL_COUNT; index++)
      color_words[index] = DRAW_COLOR;
   for (uint32_t index = PIXEL_COUNT;
        index < COLOR_BYTES / sizeof(uint32_t); index++)
      color_words[index] = COLOR_SENTINEL;
   if (!read_color_matches(color_words))
      return false;
   color_words[0] = FAR_FAIL_COLOR;
   if (read_color_matches(color_words))
      return false;
   color_words[0] = 0xffff0000u;
   if (read_color_matches(color_words))
      return false;

   uint8_t depth_bytes[DEPTH_EXPORT_STORAGE_BYTES];
   uint8_t stencil_bytes[STENCIL_EXPORT_STORAGE_BYTES];
   memset(depth_bytes, BACKING_GUARD, sizeof(depth_bytes));
   memset(stencil_bytes, BACKING_GUARD, sizeof(stencil_bytes));
   for (uint32_t index = 0u; index < PIXEL_COUNT; index++) {
      memcpy(depth_bytes + EXPORT_GUARD_BYTES + index * sizeof(uint32_t),
             &(uint32_t){READ_CLEAR_DEPTH_CODE}, sizeof(uint32_t));
      stencil_bytes[EXPORT_GUARD_BYTES + index] = BACKING_STENCIL;
   }
   if (!exported_depth_matches(depth_bytes + EXPORT_GUARD_BYTES,
                               READ_CLEAR_DEPTH_CODE, false, NULL) ||
       !exported_stencil_matches(stencil_bytes + EXPORT_GUARD_BYTES,
                                 BACKING_STENCIL) ||
       !export_guards_match(depth_bytes, DEPTH_EXPORT_BYTES) ||
       !export_guards_match(stencil_bytes, STENCIL_EXPORT_BYTES))
      return false;
   depth_bytes[0] ^= 1u;
   if (export_guards_match(depth_bytes, DEPTH_EXPORT_BYTES))
      return false;
   depth_bytes[0] ^= 1u;
   stencil_bytes[EXPORT_GUARD_BYTES + STENCIL_EXPORT_BYTES] ^= 1u;
   return !export_guards_match(stencil_bytes, STENCIL_EXPORT_BYTES);
}

static bool
write_all(int file_descriptor, const void *contents, size_t size)
{
   const uint8_t *cursor = contents;
   while (size != 0u) {
      const ssize_t written = write(file_descriptor, cursor, size);
      if (written < 0 && errno == EINTR)
         continue;
      if (written <= 0)
         return false;
      cursor += (size_t)written;
      size -= (size_t)written;
   }
   return true;
}

static bool
retain_file(int directory_descriptor, const char *name, const void *contents,
            size_t size)
{
   const int file_descriptor =
      openat(directory_descriptor, name,
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (file_descriptor < 0)
      return false;
   const bool retained = write_all(file_descriptor, contents, size) &&
                         fsync(file_descriptor) == 0;
   return close(file_descriptor) == 0 && retained;
}

static bool
retain_file_atomically(int directory_descriptor, const char *name,
                       const void *contents, size_t size)
{
   char temporary_name[128];
   const int name_length =
      snprintf(temporary_name, sizeof(temporary_name), ".%s.tmp.%ld", name,
               (long)getpid());
   if (name_length <= 0 || (size_t)name_length >= sizeof(temporary_name))
      return false;
   if (unlinkat(directory_descriptor, temporary_name, 0) != 0 &&
       errno != ENOENT)
      return false;
   const int file_descriptor =
      openat(directory_descriptor, temporary_name,
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (file_descriptor < 0)
      return false;
   const bool retained = write_all(file_descriptor, contents, size) &&
                         fsync(file_descriptor) == 0;
   const bool closed = close(file_descriptor) == 0;
   if (!retained || !closed ||
       renameat(directory_descriptor, temporary_name, directory_descriptor,
                name) != 0) {
      unlinkat(directory_descriptor, temporary_name, 0);
      return false;
   }
   return true;
}

static bool
retain_initial_backing(struct application *application, uint32_t index,
                       int directory_descriptor, const char *name,
                       bool *oracle)
{
   uint8_t *mapped = NULL;
   if (vkMapMemory(application->device, application->depth[index].memory, 0u,
                   application->depth[index].size, 0u,
                   (void **)&mapped) != VK_SUCCESS)
      return false;
   *oracle = backing_matches(mapped, BACKING_DEPTH_CODE, BACKING_STENCIL,
                             false, NULL);
   const bool retained =
      retain_file(directory_descriptor, name, mapped, DEPTH_BACKING_BYTES);
   vkUnmapMemory(application->device, application->depth[index].memory);
   return retained;
}

static bool
retain_observed_target(struct application *application, uint32_t index,
                       int directory_descriptor, const char *backing_name,
                       const char *depth_name, const char *stencil_name,
                       uint32_t default_depth, uint8_t stencil,
                       bool apply_update, bool *backing_oracle,
                       bool *depth_oracle, bool *stencil_oracle,
                       bool *guards_oracle,
                       uint32_t *backing_updates, uint32_t *depth_updates)
{
   uint8_t *backing = NULL;
   uint8_t *depth = NULL;
   uint8_t *stencil_bytes = NULL;
   if (vkMapMemory(application->device, application->depth[index].memory, 0u,
                   application->depth[index].size, 0u,
                   (void **)&backing) != VK_SUCCESS)
      return false;
   if (vkMapMemory(application->device,
                   application->depth_export[index].memory, 0u,
                   application->depth_export[index].size, 0u,
                   (void **)&depth) != VK_SUCCESS) {
      vkUnmapMemory(application->device, application->depth[index].memory);
      return false;
   }
   if (vkMapMemory(application->device,
                   application->stencil_export[index].memory, 0u,
                   application->stencil_export[index].size, 0u,
                   (void **)&stencil_bytes) != VK_SUCCESS) {
      vkUnmapMemory(application->device,
                    application->depth_export[index].memory);
      vkUnmapMemory(application->device, application->depth[index].memory);
      return false;
   }
   *backing_oracle = backing_matches(backing, default_depth, stencil,
                                     apply_update, backing_updates);
   *depth_oracle = exported_depth_matches(depth + EXPORT_GUARD_BYTES,
                                          default_depth, apply_update,
                                          depth_updates);
   *stencil_oracle = exported_stencil_matches(
      stencil_bytes + EXPORT_GUARD_BYTES, stencil);
   *guards_oracle =
      export_guards_match(depth, DEPTH_EXPORT_BYTES) &&
      export_guards_match(stencil_bytes, STENCIL_EXPORT_BYTES);
   bool retained =
      retain_file(directory_descriptor, backing_name, backing,
                  DEPTH_BACKING_BYTES) &&
      retain_file(directory_descriptor, depth_name,
                  depth + EXPORT_GUARD_BYTES,
                  DEPTH_EXPORT_BYTES) &&
      retain_file(directory_descriptor, stencil_name,
                  stencil_bytes + EXPORT_GUARD_BYTES,
                  STENCIL_EXPORT_BYTES);
   vkUnmapMemory(application->device,
                 application->stencil_export[index].memory);
   vkUnmapMemory(application->device,
                 application->depth_export[index].memory);
   vkUnmapMemory(application->device, application->depth[index].memory);
   return retained;
}

static bool
retain_read_color(struct application *application, int directory_descriptor,
                  bool *oracle)
{
   uint32_t *mapped = NULL;
   if (vkMapMemory(application->device, application->color_memory, 0u,
                   application->color_size, 0u,
                   (void **)&mapped) != VK_SUCCESS)
      return false;
   *oracle = read_color_matches(mapped);
   const bool retained =
      retain_file(directory_descriptor, "read-observed-color.bin", mapped,
                  COLOR_BYTES);
   vkUnmapMemory(application->device, application->color_memory);
   return retained;
}

static bool
all_oracles_pass(enum application_mode mode,
                 const struct oracle_results *oracles)
{
   const bool common = oracles->expected_icd_dso_mapped &&
                       oracles->submission_contract &&
                       oracles->artifacts_retained;
   if (mode == MODE_READ_MATERIALIZE_EXPORT)
      return common && oracles->read_initial_backing &&
             oracles->read_final_backing && oracles->read_exported_depth &&
             oracles->read_exported_stencil &&
             oracles->read_export_guards && oracles->read_observed_color;
   return common && oracles->aba_a_initial_backing &&
          oracles->aba_b_initial_backing && oracles->aba_a_final_backing &&
          oracles->aba_b_final_backing && oracles->aba_a_exported_depth &&
          oracles->aba_b_exported_depth &&
          oracles->aba_a_exported_stencil &&
          oracles->aba_b_exported_stencil &&
          oracles->aba_a_export_guards && oracles->aba_b_export_guards &&
          oracles->aba_a_stencil_preserved && oracles->aba_update_extent;
}

static const char *
json_bool(bool value)
{
   return value ? "true" : "false";
}

static const char *
result_name(VkResult result)
{
   switch (result) {
   case VK_SUCCESS:
      return "VK_SUCCESS";
   case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
   default:
      return "VK_RESULT_OTHER";
   }
}

static bool
retain_outcome(int directory_descriptor, enum application_mode mode,
               const struct application *application,
               const struct oracle_results *oracles)
{
   char json[8192];
   const char *mode_name =
      mode == MODE_READ_MATERIALIZE_EXPORT ? "read-materialize-export"
                                           : "aba-materialize-export";
   const char *expected_artifacts =
      mode == MODE_READ_MATERIALIZE_EXPORT
         ? "    \"read-initial-backing.bin\",\n"
           "    \"read-final-backing.bin\",\n"
           "    \"read-exported-depth.bin\",\n"
           "    \"read-exported-stencil.bin\",\n"
           "    \"read-observed-color.bin\"\n"
         : "    \"aba-a-initial-backing.bin\",\n"
           "    \"aba-a-final-backing.bin\",\n"
           "    \"aba-a-exported-depth.bin\",\n"
           "    \"aba-a-exported-stencil.bin\",\n"
           "    \"aba-b-initial-backing.bin\",\n"
           "    \"aba-b-final-backing.bin\",\n"
           "    \"aba-b-exported-depth.bin\",\n"
           "    \"aba-b-exported-stencil.bin\"\n";
   const int length = snprintf(
      json, sizeof(json),
      "{\n"
      "  \"schema\": \"r3v-zmask-public-lifecycle-application-outcome/1\",\n"
      "  \"mode\": \"%s\",\n"
      "  \"expected_icd_dso_mapped\": %s,\n"
      "  \"queue_submit_calls_attempted\": %u,\n"
      "  \"queue_submits_accepted\": %u,\n"
      "  \"queue_executions_completed\": %u,\n"
      "  \"oracles\": {\n"
      "    \"expected_icd_dso_mapped\": %s,\n"
      "    \"submission_contract\": %s,\n"
      "    \"read_initial_backing\": %s,\n"
      "    \"read_final_backing\": %s,\n"
      "    \"read_exported_depth\": %s,\n"
      "    \"read_exported_stencil\": %s,\n"
      "    \"read_export_guards\": %s,\n"
      "    \"read_observed_color\": %s,\n"
      "    \"aba_a_initial_backing\": %s,\n"
      "    \"aba_b_initial_backing\": %s,\n"
      "    \"aba_a_final_backing\": %s,\n"
      "    \"aba_b_final_backing\": %s,\n"
      "    \"aba_a_exported_depth\": %s,\n"
      "    \"aba_b_exported_depth\": %s,\n"
      "    \"aba_a_exported_stencil\": %s,\n"
      "    \"aba_b_exported_stencil\": %s,\n"
      "    \"aba_a_export_guards\": %s,\n"
      "    \"aba_b_export_guards\": %s,\n"
      "    \"aba_a_stencil_preserved\": %s,\n"
      "    \"aba_update_extent\": %s,\n"
      "    \"artifacts_retained\": %s\n"
      "  },\n"
      "  \"expected_raw_artifacts\": [\n"
      "%s"
      "  ]\n"
      "}\n",
      mode_name, json_bool(oracles->expected_icd_dso_mapped),
      application->queue_submit_calls_attempted,
      application->queue_submits_accepted,
      application->queue_executions_completed,
      json_bool(oracles->expected_icd_dso_mapped),
      json_bool(oracles->submission_contract),
      json_bool(oracles->read_initial_backing),
      json_bool(oracles->read_final_backing),
      json_bool(oracles->read_exported_depth),
      json_bool(oracles->read_exported_stencil),
      json_bool(oracles->read_export_guards),
      json_bool(oracles->read_observed_color),
      json_bool(oracles->aba_a_initial_backing),
      json_bool(oracles->aba_b_initial_backing),
      json_bool(oracles->aba_a_final_backing),
      json_bool(oracles->aba_b_final_backing),
      json_bool(oracles->aba_a_exported_depth),
      json_bool(oracles->aba_b_exported_depth),
      json_bool(oracles->aba_a_exported_stencil),
      json_bool(oracles->aba_b_exported_stencil),
      json_bool(oracles->aba_a_export_guards),
      json_bool(oracles->aba_b_export_guards),
      json_bool(oracles->aba_a_stencil_preserved),
      json_bool(oracles->aba_update_extent),
      json_bool(oracles->artifacts_retained), expected_artifacts);
   return length > 0 && (size_t)length < sizeof(json) &&
          retain_file_atomically(directory_descriptor,
                                 "application_outcome.json", json,
                                 (size_t)length);
}

static bool
preparation_submit_object_retained(int directory_descriptor)
{
   static const char *const names[] = {
      "ib.bin",
      "relocs.bin",
      "manifest.json",
      "submit_relocs.bin",
      "submit_manifest.json",
   };
   for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); index++) {
      struct stat status;
      if (fstatat(directory_descriptor, names[index], &status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(status.st_mode) || status.st_size <= 0)
         return false;
   }
   return true;
}

static bool
preparation_attempt_token_absent(int directory_descriptor)
{
   struct stat status;
   errno = 0;
   return fstatat(directory_descriptor, "attempt.token", &status,
                  AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
}

static bool
retain_preparation_outcome(int directory_descriptor,
                           enum application_mode mode,
                           const struct application *application,
                           const struct preparation_results *results)
{
   char json[2048];
   const char *mode_name =
      mode == MODE_READ_MATERIALIZE_EXPORT ? "read-materialize-export"
                                           : "aba-materialize-export";
   const int length = snprintf(
      json, sizeof(json),
      "{\n"
      "  \"schema\": \"r3v-zmask-public-lifecycle-preparation-outcome/1\",\n"
      "  \"mode\": \"%s\",\n"
      "  \"queue_submit_result\": \"%s\",\n"
      "  \"queue_submit_result_value\": %d,\n"
      "  \"queue_submit_calls_attempted\": %u,\n"
      "  \"queue_submits_accepted\": %u,\n"
      "  \"queue_executions_completed\": %u,\n"
      "  \"expected_icd_dso_mapped\": %s,\n"
      "  \"authorization_declarations_absent\": %s,\n"
      "  \"submission_refused\": %s,\n"
      "  \"submit_object_retained\": %s,\n"
      "  \"attempt_token_absent\": %s,\n"
      "  \"shim_counter_available\": %s,\n"
      "  \"shim_cs_ioctls\": %llu,\n"
      "  \"shim_hyperz_state_available\": %s,\n"
      "  \"shim_hyperz_unowned_before\": %s,\n"
      "  \"shim_hyperz_acquire_ioctls\": %llu,\n"
      "  \"shim_hyperz_release_ioctls\": %llu,\n"
      "  \"shim_hyperz_owned_after\": %s\n"
      "}\n",
      mode_name, result_name(application->queue_submit_result),
      application->queue_submit_result,
      application->queue_submit_calls_attempted,
      application->queue_submits_accepted,
      application->queue_executions_completed,
      json_bool(results->expected_icd_dso_mapped),
      json_bool(results->authorization_declarations_absent),
      json_bool(results->submission_refused),
      json_bool(results->submit_object_retained),
      json_bool(results->attempt_token_absent),
      json_bool(results->shim_counter_available),
      (unsigned long long)results->shim_cs_ioctls,
      json_bool(results->shim_hyperz_state_available),
      json_bool(results->shim_hyperz_unowned_before),
      (unsigned long long)results->shim_hyperz_acquire_ioctls,
      (unsigned long long)results->shim_hyperz_release_ioctls,
      json_bool(results->shim_hyperz_owned_after));
   return length > 0 && (size_t)length < sizeof(json) &&
          retain_file_atomically(directory_descriptor,
                                 "preparation_outcome.json", json,
                                 (size_t)length);
}

static bool
remove_partial_raw_artifacts(int directory_descriptor,
                             enum application_mode mode)
{
   static const char *const read_artifacts[] = {
      "read-initial-backing.bin",
      "read-final-backing.bin",
      "read-exported-depth.bin",
      "read-exported-stencil.bin",
      "read-observed-color.bin",
   };
   static const char *const aba_artifacts[] = {
      "aba-a-initial-backing.bin",
      "aba-a-final-backing.bin",
      "aba-a-exported-depth.bin",
      "aba-a-exported-stencil.bin",
      "aba-b-initial-backing.bin",
      "aba-b-final-backing.bin",
      "aba-b-exported-depth.bin",
      "aba-b-exported-stencil.bin",
   };
   const char *const *artifacts = mode == MODE_READ_MATERIALIZE_EXPORT
                                      ? read_artifacts
                                      : aba_artifacts;
   const size_t artifact_count =
      mode == MODE_READ_MATERIALIZE_EXPORT
         ? sizeof(read_artifacts) / sizeof(read_artifacts[0])
         : sizeof(aba_artifacts) / sizeof(aba_artifacts[0]);
   bool removed = true;
   for (size_t index = 0u; index < artifact_count; index++) {
      if (unlinkat(directory_descriptor, artifacts[index], 0) != 0 &&
          errno != ENOENT)
         removed = false;
   }
   return removed;
}

static void
destroy_application(struct application *application)
{
   for (uint32_t index = 0u; index < application->pipeline_count; index++) {
      if (application->pipeline[index] != VK_NULL_HANDLE)
         vkDestroyPipeline(application->device, application->pipeline[index],
                           NULL);
   }
   if (application->pipeline_layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(application->device,
                              application->pipeline_layout, NULL);
   if (application->framebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(application->device, application->framebuffer,
                           NULL);
   if (application->render_pass != VK_NULL_HANDLE)
      vkDestroyRenderPass(application->device, application->render_pass, NULL);
   for (uint32_t index = 0u; index < application->depth_count; index++) {
      if (application->depth_export[index].buffer != VK_NULL_HANDLE)
         vkDestroyBuffer(application->device,
                         application->depth_export[index].buffer, NULL);
      if (application->depth_export[index].memory != VK_NULL_HANDLE)
         vkFreeMemory(application->device,
                      application->depth_export[index].memory, NULL);
      if (application->stencil_export[index].buffer != VK_NULL_HANDLE)
         vkDestroyBuffer(application->device,
                         application->stencil_export[index].buffer, NULL);
      if (application->stencil_export[index].memory != VK_NULL_HANDLE)
         vkFreeMemory(application->device,
                      application->stencil_export[index].memory, NULL);
      if (application->depth[index].view != VK_NULL_HANDLE)
         vkDestroyImageView(application->device,
                            application->depth[index].view, NULL);
      if (application->depth[index].image != VK_NULL_HANDLE)
         vkDestroyImage(application->device, application->depth[index].image,
                        NULL);
      if (application->depth[index].memory != VK_NULL_HANDLE)
         vkFreeMemory(application->device, application->depth[index].memory,
                      NULL);
   }
   if (application->vertex.buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(application->device, application->vertex.buffer, NULL);
   if (application->vertex.memory != VK_NULL_HANDLE)
      vkFreeMemory(application->device, application->vertex.memory, NULL);
   if (application->color_view != VK_NULL_HANDLE)
      vkDestroyImageView(application->device, application->color_view, NULL);
   if (application->color_image != VK_NULL_HANDLE)
      vkDestroyImage(application->device, application->color_image, NULL);
   if (application->color_memory != VK_NULL_HANDLE)
      vkFreeMemory(application->device, application->color_memory, NULL);
   if (application->fence != VK_NULL_HANDLE)
      vkDestroyFence(application->device, application->fence, NULL);
   if (application->command_pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(application->device, application->command_pool,
                           NULL);
   if (application->device != VK_NULL_HANDLE)
      vkDestroyDevice(application->device, NULL);
   if (application->instance != VK_NULL_HANDLE)
      vkDestroyInstance(application->instance, NULL);
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
      const bool passed = validate_oracle_discriminators();
      printf("r3v loader ZMASK lifecycle oracle calibration: %s\n",
             passed ? "PASS" : "FAIL");
      return passed ? 0 : 1;
   }
   enum application_mode mode;
   bool preparation = false;
   if (argc == 3 && strcmp(argv[1], "--read-materialize-export") == 0)
      mode = MODE_READ_MATERIALIZE_EXPORT;
   else if (argc == 3 && strcmp(argv[1], "--aba-materialize-export") == 0)
      mode = MODE_ABA_MATERIALIZE_EXPORT;
   else if (argc == 3 &&
            strcmp(argv[1], "--prepare-read-materialize-export") == 0) {
      mode = MODE_READ_MATERIALIZE_EXPORT;
      preparation = true;
   } else if (argc == 3 &&
              strcmp(argv[1], "--prepare-aba-materialize-export") == 0) {
      mode = MODE_ABA_MATERIALIZE_EXPORT;
      preparation = true;
   } else {
      fprintf(stderr,
              "usage: %s --read-materialize-export DIR | "
              "--aba-materialize-export DIR | "
              "--prepare-read-materialize-export DIR | "
              "--prepare-aba-materialize-export DIR | --self-test\n",
              argv[0]);
      return 2;
   }

   const bool authorization_declarations_absent =
      preparation_authorization_declarations_absent();
   if (preparation && !authorization_declarations_absent) {
      fprintf(stderr,
              "preparation requires the IB, kernel, and module "
              "authorization declarations to be absent\n");
      return 2;
   }

   const int directory_descriptor =
      open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
   if (directory_descriptor < 0) {
      fprintf(stderr, "cannot open artifact directory %s: %s\n", argv[2],
              strerror(errno));
      return 2;
   }

   struct application application = {0};
   struct oracle_results oracles = {0};
   const uint32_t depth_count =
      mode == MODE_READ_MATERIALIZE_EXPORT ? 1u : 2u;
   const bool initialized = create_vulkan_context(&application, depth_count);
   oracles.expected_icd_dso_mapped = application.expected_icd_dso_mapped;
   if (!initialized ||
       !create_render_objects(&application,
                              mode == MODE_ABA_MATERIALIZE_EXPORT) ||
       !seed_host_allocations(&application)) {
      fprintf(stderr, "Vulkan lifecycle fixture creation failed\n");
      destroy_application(&application);
      close(directory_descriptor);
      return 1;
   }

   bool retained = true;
   if (!preparation && mode == MODE_READ_MATERIALIZE_EXPORT) {
      retained &= retain_initial_backing(
         &application, 0u, directory_descriptor, "read-initial-backing.bin",
         &oracles.read_initial_backing);
   } else if (!preparation) {
      retained &= retain_initial_backing(
         &application, 0u, directory_descriptor,
         "aba-a-initial-backing.bin", &oracles.aba_a_initial_backing);
      retained &= retain_initial_backing(
         &application, 1u, directory_descriptor,
         "aba-b-initial-backing.bin", &oracles.aba_b_initial_backing);
   }
   if (!retained) {
      fprintf(stderr, "initial artifact retention failed\n");
      destroy_application(&application);
      close(directory_descriptor);
      return 1;
   }
   if (!record_application(&application, mode)) {
      fprintf(stderr, "one-submit lifecycle recording failed\n");
      oracles.artifacts_retained = false;
      remove_partial_raw_artifacts(directory_descriptor, mode);
      retain_outcome(directory_descriptor, mode, &application, &oracles);
      fsync(directory_descriptor);
      destroy_application(&application);
      close(directory_descriptor);
      return 1;
   }
   uint64_t shim_cs_before = 0u;
   uint64_t shim_cs_after = 0u;
   struct shim_hyperz_state shim_hyperz_before = {0};
   struct shim_hyperz_state shim_hyperz_after = {0};
   const bool shim_counter_available =
      preparation && read_shim_cs_count(&shim_cs_before);
   const bool shim_hyperz_before_available =
      preparation && read_shim_hyperz_state(&shim_hyperz_before);
   const enum submission_result submission = submit_once(&application);
   const bool shim_counter_still_available =
      shim_counter_available && read_shim_cs_count(&shim_cs_after);
   const bool shim_hyperz_after_available =
      shim_hyperz_before_available &&
      read_shim_hyperz_state(&shim_hyperz_after);
   if (preparation) {
      const bool shim_hyperz_counts_monotonic =
         shim_hyperz_after_available &&
         shim_hyperz_after.acquire_ioctls >= shim_hyperz_before.acquire_ioctls &&
         shim_hyperz_after.release_ioctls >= shim_hyperz_before.release_ioctls;
      const struct preparation_results preparation_results = {
         .expected_icd_dso_mapped = application.expected_icd_dso_mapped,
         .authorization_declarations_absent =
            authorization_declarations_absent,
         .submission_refused =
            submission == SUBMISSION_REFUSED &&
            application.queue_submit_result == VK_ERROR_DEVICE_LOST &&
            application.queue_submit_calls_attempted == 1u &&
            application.queue_submits_accepted == 0u &&
            application.queue_executions_completed == 0u,
         .submit_object_retained =
            preparation_submit_object_retained(directory_descriptor),
         .attempt_token_absent =
            preparation_attempt_token_absent(directory_descriptor),
         .shim_counter_available = shim_counter_still_available,
         .shim_cs_ioctls = shim_counter_still_available
                              ? shim_cs_after - shim_cs_before
                              : 0u,
         .shim_hyperz_state_available = shim_hyperz_counts_monotonic,
         .shim_hyperz_unowned_before =
            shim_hyperz_before_available && !shim_hyperz_before.owned,
         .shim_hyperz_acquire_ioctls =
            shim_hyperz_counts_monotonic
               ? shim_hyperz_after.acquire_ioctls -
                    shim_hyperz_before.acquire_ioctls
               : UINT64_MAX,
         .shim_hyperz_release_ioctls =
            shim_hyperz_counts_monotonic
               ? shim_hyperz_after.release_ioctls -
                    shim_hyperz_before.release_ioctls
               : UINT64_MAX,
         .shim_hyperz_owned_after =
            shim_hyperz_after_available && shim_hyperz_after.owned,
      };
      const bool preparation_passed =
         preparation_results.expected_icd_dso_mapped &&
         preparation_results.authorization_declarations_absent &&
         preparation_results.submission_refused &&
         preparation_results.submit_object_retained &&
         preparation_results.attempt_token_absent &&
         preparation_results.shim_counter_available &&
         preparation_results.shim_cs_ioctls == 0u &&
         preparation_results.shim_hyperz_state_available &&
         preparation_results.shim_hyperz_unowned_before &&
         preparation_results.shim_hyperz_acquire_ioctls == 0u &&
         preparation_results.shim_hyperz_release_ioctls == 0u &&
         !preparation_results.shim_hyperz_owned_after;
      const bool outcome_retained =
         retain_preparation_outcome(directory_descriptor, mode, &application,
                                    &preparation_results);
      const bool directory_synced =
         outcome_retained && fsync(directory_descriptor) == 0;
      printf("mode=prepare-%s queue_submit_calls_attempted=%u "
             "queue_submits_accepted=%u queue_executions_completed=%u "
             "submit_result=%d submit_object_retained=%u "
             "shim_counter_available=%u shim_cs_ioctls=%llu "
             "shim_hyperz_acquire_ioctls=%llu "
             "shim_hyperz_release_ioctls=%llu "
             "shim_hyperz_owned_after=%u\n",
             mode == MODE_READ_MATERIALIZE_EXPORT
                ? "read-materialize-export"
                : "aba-materialize-export",
             application.queue_submit_calls_attempted,
             application.queue_submits_accepted,
             application.queue_executions_completed,
             application.queue_submit_result,
             preparation_results.submit_object_retained,
             preparation_results.shim_counter_available,
             (unsigned long long)preparation_results.shim_cs_ioctls,
             (unsigned long long)
                preparation_results.shim_hyperz_acquire_ioctls,
             (unsigned long long)
                preparation_results.shim_hyperz_release_ioctls,
             preparation_results.shim_hyperz_owned_after);
      destroy_application(&application);
      const bool directory_closed = close(directory_descriptor) == 0;
      return preparation_passed && outcome_retained && directory_synced &&
                directory_closed
             ? 0
             : 1;
   }
   if (submission != SUBMISSION_COMPLETED) {
      fprintf(stderr, "one-submit lifecycle execution failed\n");
      oracles.artifacts_retained = false;
      const bool raw_artifacts_removed =
         remove_partial_raw_artifacts(directory_descriptor, mode);
      const bool outcome_retained =
         retain_outcome(directory_descriptor, mode, &application, &oracles);
      const bool directory_synced =
         outcome_retained && fsync(directory_descriptor) == 0;
      if (!raw_artifacts_removed || !outcome_retained || !directory_synced)
         fprintf(stderr, "failed execution outcome retention incomplete\n");
      if (submission == SUBMISSION_REFUSED) {
         destroy_application(&application);
         close(directory_descriptor);
         return 1;
      }
      if (close(directory_descriptor) != 0)
         fprintf(stderr, "failed execution artifact directory close failed\n");
      fflush(stderr);
      _Exit(1);
   }
   oracles.submission_contract =
      application.queue_submit_calls_attempted == 1u &&
      application.queue_submits_accepted == 1u &&
      application.queue_executions_completed == 1u;

   if (mode == MODE_READ_MATERIALIZE_EXPORT) {
      retained &= retain_observed_target(
         &application, 0u, directory_descriptor, "read-final-backing.bin",
         "read-exported-depth.bin", "read-exported-stencil.bin",
         READ_CLEAR_DEPTH_CODE, BACKING_STENCIL, false,
         &oracles.read_final_backing, &oracles.read_exported_depth,
         &oracles.read_exported_stencil, &oracles.read_export_guards, NULL,
         NULL);
      retained &= retain_read_color(&application, directory_descriptor,
                                    &oracles.read_observed_color);
   } else {
      uint32_t a_backing_updates = 0u;
      uint32_t a_depth_updates = 0u;
      retained &= retain_observed_target(
         &application, 0u, directory_descriptor,
         "aba-a-final-backing.bin", "aba-a-exported-depth.bin",
         "aba-a-exported-stencil.bin", READ_CLEAR_DEPTH_CODE,
         BACKING_STENCIL, true, &oracles.aba_a_final_backing,
         &oracles.aba_a_exported_depth, &oracles.aba_a_exported_stencil,
         &oracles.aba_a_export_guards, &a_backing_updates, &a_depth_updates);
      retained &= retain_observed_target(
         &application, 1u, directory_descriptor,
         "aba-b-final-backing.bin", "aba-b-exported-depth.bin",
         "aba-b-exported-stencil.bin", IMAGE_B_CLEAR_DEPTH_CODE,
         IMAGE_B_CLEAR_STENCIL, false, &oracles.aba_b_final_backing,
         &oracles.aba_b_exported_depth, &oracles.aba_b_exported_stencil,
         &oracles.aba_b_export_guards, NULL, NULL);
      oracles.aba_a_stencil_preserved = oracles.aba_a_exported_stencil &&
                                        oracles.aba_a_final_backing;
      oracles.aba_update_extent =
         a_backing_updates == UPDATE_WIDTH * UPDATE_HEIGHT &&
         a_depth_updates == UPDATE_WIDTH * UPDATE_HEIGHT;
   }
   if (retained && fsync(directory_descriptor) != 0)
      retained = false;
   oracles.artifacts_retained = retained;
   const bool passed = all_oracles_pass(mode, &oracles);
   const bool outcome_retained = retain_outcome(
      directory_descriptor, mode, &application, &oracles);
   const bool directory_synced =
      outcome_retained && fsync(directory_descriptor) == 0;

   printf("mode=%s queue_submit_calls_attempted=%u "
          "queue_submits_accepted=%u queue_executions_completed=%u "
          "oracles_passed=%u artifacts_retained=%u\n",
          mode == MODE_READ_MATERIALIZE_EXPORT ? "read-materialize-export"
                                               : "aba-materialize-export",
          application.queue_submit_calls_attempted,
          application.queue_submits_accepted,
          application.queue_executions_completed, passed,
          retained && outcome_retained && directory_synced);
   destroy_application(&application);
   const bool directory_closed = close(directory_descriptor) == 0;
   return passed && retained && outcome_retained && directory_synced &&
             directory_closed
          ? 0
          : 1;
}
