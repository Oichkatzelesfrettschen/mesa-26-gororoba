/*
 * SPDX-License-Identifier: MIT
 *
 * Public-surface harness on the drm-shim fixture: an application-shaped
 * render-pass/pipeline/draw sequence records the qualified triangle
 * cell through public entry points alone, and every contract deviation
 * refuses.  The hazard gate stays closed, so each vkQueueSubmit refuses
 * before the ioctl; the submit attempts exist because the deferred
 * vertex gather and load-op clear execute at submission, and the
 * harness verifies that execution-time boundary from both sides.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

/* The harness resolves every command through the ICD's procaddr chain,
 * so the loader prototypes stay out and the command names below are the
 * resolved pointers.
 */
#define VK_NO_PROTOTYPES

#include "r3v_native.h"
#include "r3v_cpu_sync.h"
#include "r3v_native_reference_spirv.h"

#include "vk_semaphore.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

VKAPI_ATTR void VKAPI_CALL r3v_GetDeviceBufferMemoryRequirements(
   VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo,
   VkMemoryRequirements2 *pMemoryRequirements);

#define CLEAR_SENTINEL ((float)0xa5 / 255.0f)

/* The application payload is NDC: the public route's CPU vertex node
 * applies the Vulkan viewport transform, and over the 64x64 target
 * this triangle maps byte-exactly onto the window-space reference
 * payload the silicon witnessed -- (x + 1) * 32 lands on 8, 56, and
 * 32 in binary32 with no rounding.
 */
static const float ndc_triangle[12] = {
   -0.75f, -0.75f, 0.0f, 1.0f,
    0.75f, -0.75f, 0.0f, 1.0f,
    0.00f,  0.75f, 0.0f, 1.0f,
};

/* Every source component is a non-negative FP24 fixed point, so this
 * payload isolates host-model admission from the negative-NDC CPU route.
 */
static const float positive_triangle[12] = {
   0.0f,  0.25f, 0.0f, 1.0f,
   0.25f, 0.5f,  0.0f, 1.0f,
   0.5f,  0.75f, 0.0f, 1.0f,
};

static VkDevice device;
static VkCommandPool pool;

struct deferred_copy_allocation_control {
   uint32_t command_allocations_before_failure;
};

static void *
deferred_copy_test_allocate(void *user_data, size_t size, size_t alignment,
                            VkSystemAllocationScope scope)
{
   struct deferred_copy_allocation_control *control = user_data;
   (void)alignment;
   if (scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) {
      if (control->command_allocations_before_failure == 0)
         return NULL;
      control->command_allocations_before_failure--;
   }
   return malloc(size);
}

static void *
deferred_copy_test_reallocate(void *user_data, void *memory, size_t size,
                              size_t alignment, VkSystemAllocationScope scope)
{
   (void)user_data;
   (void)alignment;
   (void)scope;
   return realloc(memory, size);
}

static void
deferred_copy_test_free(void *user_data, void *memory)
{
   (void)user_data;
   free(memory);
}

static bool
r3v_native_cache_publication_precedes_close(uint64_t cache_event,
                                            uint64_t close_event)
{
   return cache_event < close_event;
}

static bool
r3v_native_memory_type_bits_are_type_zero_only(uint32_t memory_type_bits)
{
   return memory_type_bits == 0x1u;
}

static bool
r3v_native_binary_semaphore_is_signaled(VkSemaphore semaphore_handle)
{
   VK_FROM_HANDLE(vk_semaphore, semaphore, semaphore_handle);
   struct r3v_cpu_sync *sync = r3v_cpu_sync_from_vk(
      vk_semaphore_get_active_sync(semaphore));
   mtx_lock(&sync->lock);
   bool signaled = sync->signaled;
   mtx_unlock(&sync->lock);
   return signaled;
}

static void
check_timeline_wait_consumption(void)
{
   struct vk_sync binary_sync = { 0 };
   struct vk_sync timeline_sync = { .flags = VK_SYNC_IS_TIMELINE };
   struct vk_sync_wait waits[] = {
      { .sync = &binary_sync },
      { .sync = &timeline_sync },
      { .sync = &binary_sync },
      { .sync = &binary_sync },
      { .sync = &binary_sync },
   };
   struct vk_sync_timeline_point *wait_points[] = {
      NULL, NULL, (struct vk_sync_timeline_point *)1, NULL, NULL,
   };
   struct vk_sync *wait_temps[] = { NULL, NULL, NULL, &binary_sync, NULL };
   struct vk_sync_signal signals[] = { { .sync = &timeline_sync } };
   struct vk_queue_submit submit = {
      .wait_count = ARRAY_SIZE(waits),
      .signal_count = ARRAY_SIZE(signals),
      .waits = waits,
      .signals = signals,
      ._wait_points = wait_points,
      ._wait_temps = wait_temps,
   };

   assert(r3v_native_queue_wait_is_permanent_binary(&submit, 0));
   assert(!r3v_native_queue_wait_is_permanent_binary(&submit, 1));
   assert(!r3v_native_queue_wait_is_permanent_binary(&submit, 2));
   assert(!r3v_native_queue_wait_is_permanent_binary(&submit, 3));

   signals[0].sync = &binary_sync;
   assert(!r3v_native_queue_wait_is_permanent_binary(&submit, 4));
   signals[0].sync = &timeline_sync;
   assert(r3v_native_queue_wait_is_permanent_binary(&submit, 4));
}

#define DEVICE_COMMANDS(f)                                                 \
   f(vkAllocateMemory) f(vkFreeMemory) f(vkMapMemory) f(vkUnmapMemory)     \
   f(vkCreateBuffer) f(vkDestroyBuffer) f(vkGetBufferMemoryRequirements2KHR) \
   f(vkBindBufferMemory)                                                    \
   f(vkCreateImage) f(vkDestroyImage) f(vkGetImageMemoryRequirements)      \
   f(vkGetImageSubresourceLayout) f(vkBindImageMemory)                     \
   f(vkCreateImageView) f(vkDestroyImageView)                              \
   f(vkCreateRenderPass) f(vkDestroyRenderPass) f(vkCreateFramebuffer)     \
   f(vkDestroyFramebuffer) f(vkCreateShaderModule)                         \
   f(vkDestroyShaderModule) f(vkCreatePipelineLayout)                      \
   f(vkDestroyPipelineLayout) f(vkCreateGraphicsPipelines)                 \
   f(vkDestroyPipeline) f(vkCreateCommandPool) f(vkDestroyCommandPool)     \
   f(vkAllocateCommandBuffers) f(vkBeginCommandBuffer)                     \
   f(vkEndCommandBuffer) f(vkCmdBeginRenderPass) f(vkCmdEndRenderPass)     \
   f(vkCmdBindPipeline) f(vkCmdBindVertexBuffers) f(vkCmdDraw)             \
   f(vkCmdCopyBuffer) f(vkCmdCopyBufferToImage) f(vkCmdCopyImage)          \
   f(vkCmdCopyImageToBuffer)                                               \
   f(vkCmdClearColorImage) f(vkCmdPipelineBarrier)                         \
   f(vkCreateFence) f(vkDestroyFence) f(vkGetFenceStatus)                   \
   f(vkWaitForFences) f(vkCreateSemaphore) f(vkDestroySemaphore)            \
   f(vkGetDeviceQueue) f(vkQueueSubmit) f(vkDestroyDevice)

#define DECLARE(name) static PFN_##name name;
DEVICE_COMMANDS(DECLARE)
#undef DECLARE

static VkCommandBuffer
fresh_cmd(void)
{
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   assert(vkAllocateCommandBuffers(
             device,
             &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
             },
             &cmd) == VK_SUCCESS);
   vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
                           .sType =
                              VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                        });
   return cmd;
}

static void
record_image_barrier(VkCommandBuffer command_buffer, VkImage image,
                              VkImageLayout old_layout,
                              VkImageLayout new_layout,
                              VkPipelineStageFlags src_stage,
                              VkPipelineStageFlags dst_stage,
                              VkAccessFlags src_access,
                              VkAccessFlags dst_access)
{
   vkCmdPipelineBarrier(
      command_buffer, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
      &(VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = src_access,
         .dstAccessMask = dst_access,
         .oldLayout = old_layout,
         .newLayout = new_layout,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1,
                               .layerCount = 1 },
      });
}

static VkShaderModule
make_module(const uint32_t *words, size_t bytes)
{
   VkShaderModule module = VK_NULL_HANDLE;
   assert(vkCreateShaderModule(
             device,
             &(VkShaderModuleCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = bytes,
                .pCode = words,
             },
             NULL, &module) == VK_SUCCESS);
   return module;
}

/* One contract-shaped graphics pipeline over the given vertex format
 * and stride; the mutate hook lets the negative legs deviate exactly
 * one member.
 */
struct pipeline_shape {
   VkFormat attribute_format;
   uint32_t stride;
   VkBool32 blend_enable;
   const uint32_t *fragment_words;
   size_t fragment_bytes;
   /* Viewport/scissor extent; zero selects the maximum target extent. */
   uint32_t extent_width;
   uint32_t extent_height;
};

static VkResult
make_pipeline(const struct pipeline_shape *shape, VkRenderPass pass,
              VkPipelineLayout layout, VkPipeline *pipeline)
{
   VkShaderModule vs = make_module(r3v_reference_vertex_spirv,
                                   sizeof(r3v_reference_vertex_spirv));
   VkShaderModule fs =
      make_module(shape->fragment_words, shape->fragment_bytes);
   const uint32_t extent_width = shape->extent_width != 0
                                    ? shape->extent_width
                                    : R3V_NATIVE_TARGET_WIDTH;
   const uint32_t extent_height = shape->extent_height != 0
                                     ? shape->extent_height
                                     : R3V_NATIVE_TARGET_HEIGHT;

   const VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages =
         (VkPipelineShaderStageCreateInfo[]){
            { .sType =
                 VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT,
              .module = vs,
              .pName = "main" },
            { .sType =
                 VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = fs,
              .pName = "main" },
         },
      .pVertexInputState =
         &(VkPipelineVertexInputStateCreateInfo){
            .sType =
               VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions =
               &(VkVertexInputBindingDescription){
                  .binding = 0,
                  .stride = shape->stride,
                  .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
               },
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions =
               &(VkVertexInputAttributeDescription){
                  .location = 0,
                  .binding = 0,
                  .format = shape->attribute_format,
                  .offset = 0,
               },
         },
      .pInputAssemblyState =
         &(VkPipelineInputAssemblyStateCreateInfo){
            .sType =
               VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
         },
      .pViewportState =
         &(VkPipelineViewportStateCreateInfo){
            .sType =
               VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports =
               &(VkViewport){
                  .width = (float)extent_width,
                  .height = (float)extent_height,
                  .maxDepth = 1.0f,
               },
            .scissorCount = 1,
            .pScissors =
               &(VkRect2D){
                  .extent = { extent_width, extent_height },
               },
         },
      .pRasterizationState =
         &(VkPipelineRasterizationStateCreateInfo){
            .sType =
               VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f,
         },
      .pMultisampleState =
         &(VkPipelineMultisampleStateCreateInfo){
            .sType =
               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
         },
      .pColorBlendState =
         &(VkPipelineColorBlendStateCreateInfo){
            .sType =
               VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments =
               &(VkPipelineColorBlendAttachmentState){
                  .blendEnable = shape->blend_enable,
                  .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                    VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT,
               },
         },
      .layout = layout,
      .renderPass = pass,
   };

   *pipeline = VK_NULL_HANDLE;
   VkResult result =
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL,
                                pipeline);
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyShaderModule(device, fs, NULL);
   return result;
}

struct image_usage_case {
   VkImageUsageFlags usage;
   VkResult query_result;
   VkResult create_result;
};

/* Query and creation are separate public entry points, so exercise the
 * complete image-family matrix through both dispatch paths.  The native
 * transfer mask is exactly the two transfer bits; color is its own exclusive
 * family, and zero or a mixed color/transfer mask refuses at both boundaries.
 */
static void
check_image_usage_surface(
   VkPhysicalDevice physical_device,
   VkDevice image_device,
   PFN_vkGetPhysicalDeviceImageFormatProperties2KHR query_properties)
{
   const VkImageUsageFlags transfer_usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   const struct image_usage_case cases[] = {
      { 0, VK_ERROR_FORMAT_NOT_SUPPORTED, R3V_NATIVE_REFUSAL_RESULT },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SUCCESS, VK_SUCCESS },
      { transfer_usage, VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_ERROR_FORMAT_NOT_SUPPORTED, R3V_NATIVE_REFUSAL_RESULT },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_ERROR_FORMAT_NOT_SUPPORTED, R3V_NATIVE_REFUSAL_RESULT },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_ERROR_FORMAT_NOT_SUPPORTED, R3V_NATIVE_REFUSAL_RESULT },
   };

   assert(transfer_usage ==
          (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT));
   assert(transfer_usage !=
          (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT |
           VK_IMAGE_USAGE_SAMPLED_BIT));

   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      const VkPhysicalDeviceImageFormatInfo2 query_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
         .format = R3V_NATIVE_TARGET_FORMAT,
         .type = VK_IMAGE_TYPE_2D,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = cases[i].usage,
      };
      VkImageFormatProperties2 properties = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      };
      assert(query_properties(physical_device, &query_info, &properties) ==
             cases[i].query_result);

      const VkImageCreateInfo create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = R3V_NATIVE_TARGET_FORMAT,
         .extent = { R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = cases[i].usage,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkImage image = VK_NULL_HANDLE;
      assert(vkCreateImage(image_device, &create_info, NULL, &image) ==
             cases[i].create_result);
      if (cases[i].create_result == VK_SUCCESS) {
         assert(image != VK_NULL_HANDLE);
         vkDestroyImage(image_device, image, NULL);
      } else {
         assert(image == VK_NULL_HANDLE);
      }
   }
}

int
main(void)
{
   check_timeline_wait_consumption();

   /* The gate stays closed by construction: recording is submit-free
    * and this harness never opens the hazard environment.
    */
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const char *const instance_extensions[] = {
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
   };
   VkInstance instance = VK_NULL_HANDLE;
   assert(create_instance(&(VkInstanceCreateInfo){
                             .sType =
                                VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo =
                                &(VkApplicationInfo){
                                   .sType =
                                      VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_0,
                                },
                             .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = instance_extensions,
                          },
                          NULL, &instance) == VK_SUCCESS);

   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(instance,
                                           "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice create_device =
      (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   PFN_vkGetPhysicalDeviceImageFormatProperties2KHR query_image_properties =
      (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)gipa(
         instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
   assert(query_image_properties != NULL);

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated = enumerate(instance, &pdev_count, &pdev);
   assert((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
          pdev_count == 1);

   const float priority = 1.0f;
   const char *device_extensions[] = {
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
   };
   assert(create_device(
             pdev,
             &(VkDeviceCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .queueCreateInfoCount = 1,
                .pQueueCreateInfos =
                   &(VkDeviceQueueCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                      .queueFamilyIndex = 0,
                      .queueCount = 1,
                      .pQueuePriorities = &priority,
                   },
                .enabledExtensionCount = 2,
                .ppEnabledExtensionNames = device_extensions,
             },
             NULL, &device) == VK_SUCCESS);

#define LOAD(name) name = (PFN_##name)gdpa(device, #name); assert(name);
   DEVICE_COMMANDS(LOAD)
#undef LOAD

   /* The public query and create entry points agree over every admitted and
    * refused usage family before the longer transfer and render sequence. */
   check_image_usage_surface(pdev, device, query_image_properties);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);
   assert(queue != VK_NULL_HANDLE);

   assert(vkCreateCommandPool(
             device,
             &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = 0,
             },
             NULL, &pool) == VK_SUCCESS);

   /* The qualified image: requirements carry the cell's memory
    * contract, the bind admits offset zero, and the identity view
    * completes the attachment.
    */
   VkImage image = VK_NULL_HANDLE;
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = R3V_NATIVE_TARGET_FORMAT,
      .extent = { R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   assert(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS);

   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(device, image, &reqs);
   assert(reqs.size == R3V_NATIVE_TARGET_MEMORY_BYTES);

   /* One page past the image requirement: the tail proves the load-op
    * clear stays inside the image's declared footprint, so a resource
    * bound after it in the same allocation would survive the draw.
    */
   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = reqs.size + 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &color_memory) == VK_SUCCESS);
   assert(vkBindImageMemory(device, image, color_memory, 0) == VK_SUCCESS);

   /* Seed the whole allocation with a known non-sentinel value, so the
    * untouched-memory verdicts below compare against a defined byte
    * pattern rather than fresh-allocation content.
    */
#define COLOR_SEED 0x5c5c5c5cu
   {
      uint32_t *seed_map = NULL;
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&seed_map) == VK_SUCCESS);
      for (unsigned i = 0; i < (R3V_NATIVE_TARGET_MEMORY_BYTES + 4096) / 4;
           i++)
         seed_map[i] = COLOR_SEED;
      vkUnmapMemory(device, color_memory);
   }

   VkImageView view = VK_NULL_HANDLE;
   assert(vkCreateImageView(
             device,
             &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = R3V_NATIVE_TARGET_FORMAT,
                .subresourceRange = { .aspectMask =
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                      .levelCount = 1,
                                      .layerCount = 1 },
             },
             NULL, &view) == VK_SUCCESS);

   /* The application vertex buffer: the reference triangle's records,
    * written through the public map.
    */
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &vertex_memory) == VK_SUCCESS);
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   assert(vkCreateBuffer(device,
                         &(VkBufferCreateInfo){
                            .sType =
                               VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = 256,
                            .usage =
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &vertex_buffer) == VK_SUCCESS);
   VkMemoryDedicatedRequirements dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
      .prefersDedicatedAllocation = VK_TRUE,
      .requiresDedicatedAllocation = VK_TRUE,
   };
   VkMemoryRequirements2 buffer_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated,
   };
   VkBufferMemoryRequirementsInfo2 buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
      .buffer = vertex_buffer,
   };
   vkGetBufferMemoryRequirements2KHR(device, &buffer_info,
                                     &buffer_requirements);
   assert(dedicated.prefersDedicatedAllocation == VK_FALSE);
   assert(dedicated.requiresDedicatedAllocation == VK_FALSE);

   /* The Vulkan registry at src/vulkan/registry/vk.xml promotes
    * VK_KHR_get_memory_requirements2 to Vulkan 1.1 and
    * VK_KHR_maintenance4 to Vulkan 1.3.  R3V_API_VERSION in
    * src/amd/r300/vulkan/r3v_private.h is Vulkan 1.0, so the enabled public
    * route resolves the get-memory-requirements2 KHR alias; this direct call
    * covers the maintenance4 device-buffer implementation path.
    */
   VkBufferCreateInfo device_buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 384,
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkDeviceBufferMemoryRequirements device_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pCreateInfo = &device_buffer_create_info,
   };
   VkMemoryDedicatedRequirements device_dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
      .prefersDedicatedAllocation = VK_TRUE,
      .requiresDedicatedAllocation = VK_TRUE,
   };
   VkMemoryRequirements2 device_buffer_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &device_dedicated,
   };

   /* Calibrate the exact-mask verdict by accepting only 0x1 and rejecting
    * the empty and extra-bit masks before checking the device query.
    */
   assert(r3v_native_memory_type_bits_are_type_zero_only(0x1u));
   assert(!r3v_native_memory_type_bits_are_type_zero_only(0x0u));
   assert(!r3v_native_memory_type_bits_are_type_zero_only(0x5u));

   r3v_GetDeviceBufferMemoryRequirements(device, &device_buffer_info,
                                         &device_buffer_requirements);
   assert(r3v_native_memory_type_bits_are_type_zero_only(
      device_buffer_requirements.memoryRequirements.memoryTypeBits));
   assert(device_dedicated.prefersDedicatedAllocation == VK_FALSE);
   assert(device_dedicated.requiresDedicatedAllocation == VK_FALSE);
   assert(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0) ==
          VK_SUCCESS);
   void *map = NULL;
   assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS &&
          map != NULL);
   memcpy(map, ndc_triangle, sizeof(ndc_triangle));
   vkUnmapMemory(device, vertex_memory);

   /* Render pass, framebuffer, and layout through the runtime's common
    * objects; the native surface validates their shape at pipeline
    * creation and pass begin.
    */
   VkRenderPass pass = VK_NULL_HANDLE;
   assert(vkCreateRenderPass(
             device,
             &(VkRenderPassCreateInfo){
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments =
                   &(VkAttachmentDescription){
                      .format = R3V_NATIVE_TARGET_FORMAT,
                      .samples = VK_SAMPLE_COUNT_1_BIT,
                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                      .stencilLoadOp =
                         VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                      .stencilStoreOp =
                         VK_ATTACHMENT_STORE_OP_DONT_CARE,
                      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
                   },
                .subpassCount = 1,
                .pSubpasses =
                   &(VkSubpassDescription){
                      .pipelineBindPoint =
                         VK_PIPELINE_BIND_POINT_GRAPHICS,
                      .colorAttachmentCount = 1,
                      .pColorAttachments =
                         &(VkAttachmentReference){
                            .attachment = 0,
                            .layout = VK_IMAGE_LAYOUT_GENERAL,
                         },
                   },
             },
             NULL, &pass) == VK_SUCCESS);

   VkFramebuffer framebuffer = VK_NULL_HANDLE;
   assert(vkCreateFramebuffer(
             device,
             &(VkFramebufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = pass,
                .attachmentCount = 1,
                .pAttachments = &view,
                .width = R3V_NATIVE_TARGET_WIDTH,
                .height = R3V_NATIVE_TARGET_HEIGHT,
                .layers = 1,
             },
             NULL, &framebuffer) == VK_SUCCESS);

   VkPipelineLayout layout = VK_NULL_HANDLE;
   assert(vkCreatePipelineLayout(
             device,
             &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             },
             NULL, &layout) == VK_SUCCESS);

   const struct pipeline_shape contract_shape = {
      .attribute_format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .stride = 16,
      .blend_enable = VK_FALSE,
      .fragment_words = r3v_reference_fragment_spirv,
      .fragment_bytes = sizeof(r3v_reference_fragment_spirv),
   };
   VkPipeline pipeline = VK_NULL_HANDLE;
   assert(make_pipeline(&contract_shape, pass, layout, &pipeline) ==
             VK_SUCCESS &&
          pipeline != VK_NULL_HANDLE);

   const VkRenderPassBeginInfo begin_pass = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = pass,
      .framebuffer = framebuffer,
      .renderArea = { .extent = { R3V_NATIVE_TARGET_WIDTH,
                                  R3V_NATIVE_TARGET_HEIGHT } },
      .clearValueCount = 1,
      .pClearValues =
         &(VkClearValue){
            .color = { .float32 = { CLEAR_SENTINEL, CLEAR_SENTINEL,
                                    CLEAR_SENTINEL, CLEAR_SENTINEL } },
         },
   };

   /* The public Vulkan path admits a zero-stride binding as a constant
    * stream.  The deferred draw must repeat the first record in every
    * carrier vertex, which proves pipeline admission and execution reach the
    * same CPU gather contract as the direct harness.
    */
   VkPipeline constant_pipeline = VK_NULL_HANDLE;
   const struct pipeline_shape constant_shape = {
      .attribute_format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .stride = 0,
      .blend_enable = VK_FALSE,
      .fragment_words = r3v_reference_fragment_spirv,
      .fragment_bytes = sizeof(r3v_reference_fragment_spirv),
   };
   assert(make_pipeline(&constant_shape, pass, layout, &constant_pipeline) ==
             VK_SUCCESS &&
          constant_pipeline != VK_NULL_HANDLE);
   VkCommandBuffer constant_cmd = fresh_cmd();
   vkCmdBeginRenderPass(constant_cmd, &begin_pass,
                        VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(constant_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     constant_pipeline);
   vkCmdBindVertexBuffers(constant_cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(constant_cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(constant_cmd);
   assert(vkEndCommandBuffer(constant_cmd) == VK_SUCCESS);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_constant, constant_cmd);
   VK_FROM_HANDLE(r3v_native_device, constant_device, device);
   assert(r3v_native_cmd_buffer_execute_deferred_draw(
             constant_device, native_constant) == VK_SUCCESS);
   void *constant_carrier_map = NULL;
   assert(radeon_drm_vk_bo_map(&constant_device->drm,
                               &native_constant->owned_carrier->bo,
                               &constant_carrier_map) == 0);
   static const float expected_constant[4] = { 8.0f, 8.0f, 0.0f, 1.0f };
   for (unsigned vertex = 0; vertex < 3; vertex++)
      assert(memcmp((const uint8_t *)constant_carrier_map + vertex * 16,
                    expected_constant, sizeof(expected_constant)) == 0);
   radeon_drm_vk_bo_unmap(&constant_device->drm,
                          &native_constant->owned_carrier->bo,
                          constant_carrier_map);
   uint32_t *constant_color_map = NULL;
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&constant_color_map) == VK_SUCCESS);
   for (unsigned i = 0;
        i < (R3V_NATIVE_TARGET_MEMORY_BYTES + 4096) / sizeof(uint32_t);
        i++)
      constant_color_map[i] = COLOR_SEED;
   vkUnmapMemory(device, color_memory);
   vkDestroyPipeline(device, constant_pipeline, NULL);

   /* A render pass applies LOAD_OP_CLEAR even when its subpass records no
    * draw.  The host model must accept the empty pass, retain clear work on
    * its zero-IB command buffer, and execute that work at submission.
    */
   VkCommandBuffer empty_cmd = fresh_cmd();
   vkCmdBeginRenderPass(empty_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_empty, empty_cmd);
   vkCmdEndRenderPass(empty_cmd);
   assert(vkEndCommandBuffer(empty_cmd) == VK_SUCCESS);
   assert(native_empty->ib_size_dwords == 0);
   assert(native_empty->deferred_draw.pending);
   assert(native_empty->deferred_draw.buffer == NULL);
   assert(native_empty->owned_carrier == NULL);
   uint32_t *empty_color_map = NULL;
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&empty_color_map) == VK_SUCCESS);
   assert(empty_color_map[0] == COLOR_SEED);
   vkUnmapMemory(device, color_memory);
   assert(vkQueueSubmit(
             queue, 1,
             &(VkSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &empty_cmd,
             },
             VK_NULL_HANDLE) == VK_SUCCESS);
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&empty_color_map) == VK_SUCCESS);
   assert(empty_color_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
   assert(empty_color_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] ==
          R300_TRIANGLE_COLOR_SENTINEL);
   assert(empty_color_map[R3V_NATIVE_TARGET_MEMORY_BYTES / 4] == COLOR_SEED);
   vkUnmapMemory(device, color_memory);
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&empty_color_map) == VK_SUCCESS);
   for (unsigned i = 0; i < (R3V_NATIVE_TARGET_MEMORY_BYTES + 4096) / 4;
        i++)
      empty_color_map[i] = COLOR_SEED;
   vkUnmapMemory(device, color_memory);

   /* The positive leg: the full public sequence ends EXECUTABLE and
    * installs the reference cell against the byte-identical carrier.
    */
   VkCommandBuffer cmd = fresh_cmd();
   /* Render-family images use a separate barrier vocabulary from transfer
    * images, and the transition precedes the render pass.
    */
   record_image_barrier(
      cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
   vkCmdBeginRenderPass(cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);

   /* Render-family barriers admit the linear-image preinitialized state and
    * the color-attachment layout, while transfer layouts remain refused.
    */
   VkCommandBuffer render_layout_cmd = fresh_cmd();
   record_image_barrier(
      render_layout_cmd, image, VK_IMAGE_LAYOUT_PREINITIALIZED,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
   assert(vkEndCommandBuffer(render_layout_cmd) == VK_SUCCESS);

   VkCommandBuffer bad_render_layout_cmd = fresh_cmd();
   record_image_barrier(
      bad_render_layout_cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      VK_ACCESS_TRANSFER_WRITE_BIT);
   assert(vkEndCommandBuffer(bad_render_layout_cmd) ==
          R3V_NATIVE_REFUSAL_RESULT);

   /* The harness links the implementation, so the installed IB and the
    * owned carrier are directly readable: the public route records the
    * reference cell's exact dwords over the reference vertex bytes.
    */
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_cmd, cmd);
   VK_FROM_HANDLE(r3v_native_device, native_device, device);
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_reference_emit(&reference) == 0);
   assert(native_cmd->ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(native_cmd->ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   assert(native_cmd->reference_count == R300_TRIANGLE_SLOT_COUNT);
   assert(native_cmd->owned_carrier != NULL);
   assert(native_cmd->references[R300_TRIANGLE_SLOT_VERTEX].handle ==
          native_cmd->owned_carrier->bo.handle);
   r300_tcl_bypass_triangle_release(&reference);

   /* Execution-time boundary, record side: recording defers the vertex
    * gather and load-op clear, so the executable command buffer has
    * touched neither the application's image memory nor its own
    * carrier.
    */
   assert(native_cmd->deferred_draw.pending);
   uint32_t *color_map = NULL;
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&color_map) == VK_SUCCESS);
   assert(color_map[0] == COLOR_SEED);
   vkUnmapMemory(device, color_memory);

   /* Execution-time boundary, submit side: the stream bytes the carrier
    * travels with are the ones live at submission, so a write after
    * recording is honored and each submission re-reads.  The closed
    * hazard gate refuses the ioctl after the deferred execution ran.
    */
   uint32_t original_first_dword;
   memcpy(&original_first_dword, ndc_triangle,
          sizeof(original_first_dword));
   assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
          VK_SUCCESS);
   {
      const uint32_t mutated = original_first_dword ^ 0x00400000u;
      memcpy(map, &mutated, sizeof(mutated));
   }
   vkUnmapMemory(device, vertex_memory);

   /* The copy legs run before the first closed-gate draw submission:
    * that expected refusal marks the queue lost in the runtime, and
    * every later vkQueueSubmit short-circuits, so the copy-carrying
    * submissions must precede it.
    */
   /* Synchronous copies over the graphics family, through the public queue:
    * buffer-to-buffer and buffer-to-image with a sub-rectangle at an offset,
    * image-to-image between two images, image-to-buffer readback --
    * three ops in one command buffer, executed in recorded order at
    * submission, a copy-carrying buffer reaching no ioctl.  The byte
    * oracle is the staging pattern itself: each texel carries its own
    * coordinates, so a wrong pitch, offset, or direction lands wrong
    * bytes.
    */
   {
      const uint32_t copy_w = 8, copy_h = 4;
      VkImageCreateInfo transfer_info = image_info;
      transfer_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      transfer_info.extent.width = 16;
      transfer_info.extent.height = 8;
      VkImage img_a = VK_NULL_HANDLE, img_b = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &transfer_info, NULL, &img_a) ==
             VK_SUCCESS);
      assert(vkCreateImage(device, &transfer_info, NULL, &img_b) ==
             VK_SUCCESS);

      VkDeviceMemory mem_a = VK_NULL_HANDLE, mem_b = VK_NULL_HANDLE;
      const VkMemoryAllocateInfo transfer_alloc = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = 8192,
         .memoryTypeIndex = 0,
      };
      const VkDeviceSize transfer_image_offset = R3V_NATIVE_MEMORY_ALIGNMENT;
      const uint32_t transfer_image_base_word =
         transfer_image_offset / sizeof(uint32_t);
      const uint32_t transfer_allocation_words =
         transfer_alloc.allocationSize / sizeof(uint32_t);
      assert(vkAllocateMemory(device, &transfer_alloc, NULL, &mem_a) ==
             VK_SUCCESS);
      assert(vkAllocateMemory(device, &transfer_alloc, NULL, &mem_b) ==
             VK_SUCCESS);
      assert(vkBindImageMemory(device, img_a, mem_a,
                               transfer_image_offset) == VK_SUCCESS);
      assert(vkBindImageMemory(device, img_b, mem_b,
                               transfer_image_offset) == VK_SUCCESS);

      VkImageCreateInfo source_only_info = transfer_info;
      source_only_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VkImageCreateInfo destination_only_info = transfer_info;
      destination_only_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VkImage source_only = VK_NULL_HANDLE;
      VkImage destination_only = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &source_only_info, NULL, &source_only) ==
             VK_SUCCESS);
      assert(vkCreateImage(device, &destination_only_info, NULL,
                           &destination_only) == VK_SUCCESS);
      VkDeviceMemory source_only_memory = VK_NULL_HANDLE;
      VkDeviceMemory destination_only_memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(device, &transfer_alloc, NULL,
                              &source_only_memory) == VK_SUCCESS);
      assert(vkAllocateMemory(device, &transfer_alloc, NULL,
                              &destination_only_memory) == VK_SUCCESS);
      assert(vkBindImageMemory(device, source_only, source_only_memory,
                               transfer_image_offset) == VK_SUCCESS);
      assert(vkBindImageMemory(device, destination_only,
                               destination_only_memory,
                               transfer_image_offset) == VK_SUCCESS);

      VkBuffer staging = VK_NULL_HANDLE;
      assert(vkCreateBuffer(
                device,
                &(VkBufferCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                   .size = 4096,
                   .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                },
                NULL, &staging) == VK_SUCCESS);
      VkDeviceMemory staging_mem = VK_NULL_HANDLE;
      assert(vkAllocateMemory(device, &transfer_alloc, NULL,
                              &staging_mem) == VK_SUCCESS);
      assert(vkBindBufferMemory(device, staging, staging_mem, 0) ==
             VK_SUCCESS);

      /* Source pattern plus sentinel grounds for both images. */
      uint32_t *staging_map;
      assert(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0,
                         (void **)&staging_map) == VK_SUCCESS);
      for (uint32_t t = 0; t < copy_w * copy_h; t++)
         staging_map[t] = 0x40000000u | t;
      for (uint32_t t = copy_w * copy_h; t < 1024; t++)
         staging_map[t] = 0xdeadbeefu;
      vkUnmapMemory(device, staging_mem);
      uint32_t *pixel_map;
      assert(vkMapMemory(device, mem_a, 0, VK_WHOLE_SIZE, 0,
                         (void **)&pixel_map) == VK_SUCCESS);
      for (uint32_t t = 0; t < transfer_allocation_words; t++)
         pixel_map[t] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, mem_a);
      assert(vkMapMemory(device, mem_b, 0, VK_WHOLE_SIZE, 0,
                         (void **)&pixel_map) == VK_SUCCESS);
      for (uint32_t t = 0; t < transfer_allocation_words; t++)
         pixel_map[t] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, mem_b);

      /* Record: pattern into img_a at (2, 1); img_a rectangle into
       * img_b at (5, 3); img_b rectangle back out to the staging tail.
       */
      VkCommandBuffer copy_cmd = fresh_cmd();
      const VkBufferCopy buffer_copy = {
         .srcOffset = 0,
         .dstOffset = 1024,
         .size = copy_w * copy_h * sizeof(uint32_t),
      };

      /* An empty render pass retains its load-op clear after EndRenderPass.
       * A transfer recorded after that pass would execute before the clear
       * on the zero-IB queue path, so recording refuses the mixed buffer.
       */
      VkCommandBuffer empty_pass_copy_cmd = fresh_cmd();
      vkCmdBeginRenderPass(empty_pass_copy_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_empty_pass_copy,
                     empty_pass_copy_cmd);
      vkCmdEndRenderPass(empty_pass_copy_cmd);
      vkCmdCopyBuffer(empty_pass_copy_cmd, staging, staging, 1,
                      &buffer_copy);
      assert(native_empty_pass_copy->deferred_draw.pending);
      assert(native_empty_pass_copy->deferred_copy_count == 0);
      assert(vkEndCommandBuffer(empty_pass_copy_cmd) ==
             R3V_NATIVE_REFUSAL_RESULT);

      vkCmdCopyBuffer(copy_cmd, staging, staging, 1, &buffer_copy);
      const VkBufferImageCopy upload = {
         .bufferOffset = 0,
         .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .layerCount = 1 },
         .imageOffset = { 2, 1, 0 },
         .imageExtent = { copy_w, copy_h, 1 },
      };
      /* A newly created image starts undefined.  The transfer recorder
       * admits the transition into the destination layout before upload.
       */
      record_image_barrier(
         copy_cmd, img_a, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0);
      vkCmdCopyBufferToImage(copy_cmd, staging, img_a,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &upload);
      /* The barrier validates the image-layout transition while the
       * single-thread in-order execution carries the dependency.
       */
      record_image_barrier(
         copy_cmd, img_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      record_image_barrier(
         copy_cmd, img_b, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0);
      vkCmdPipelineBarrier(
         copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
         &(VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = 0,
            .dstQueueFamilyIndex = 0,
            .buffer = staging,
            .size = VK_WHOLE_SIZE,
         },
         0, NULL);
      const VkImageCopy cross = {
         .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1 },
         .srcOffset = { 2, 1, 0 },
         .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1 },
         .dstOffset = { 5, 3, 0 },
         .extent = { copy_w, copy_h, 1 },
      };
      vkCmdCopyImage(copy_cmd, img_a,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img_b,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cross);
      record_image_barrier(
         copy_cmd, img_b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      const VkBufferImageCopy readback = {
         .bufferOffset = 2048,
         .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .layerCount = 1 },
         .imageOffset = { 5, 3, 0 },
         .imageExtent = { copy_w, copy_h, 1 },
      };
      vkCmdCopyImageToBuffer(copy_cmd, img_b,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             staging, 1, &readback);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, copy_native, copy_cmd);
      assert(copy_native->deferred_copy_count == 4);
      assert(copy_native->deferred_copies[0].kind ==
             R3V_NATIVE_COPY_BUFFER_TO_BUFFER);
      assert(vkEndCommandBuffer(copy_cmd) == VK_SUCCESS);

      /* Transfer barriers follow the image usage bits: source-only images
       * admit source layout transitions, destination-only images admit
       * destination transitions, and each rejects the opposite layout.
       */
      VkCommandBuffer source_layout_cmd = fresh_cmd();
      record_image_barrier(
         source_layout_cmd, source_only, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, VK_ACCESS_TRANSFER_READ_BIT);
      assert(vkEndCommandBuffer(source_layout_cmd) == VK_SUCCESS);

      VkCommandBuffer bad_source_layout_cmd = fresh_cmd();
      record_image_barrier(
         bad_source_layout_cmd, source_only, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, VK_ACCESS_TRANSFER_WRITE_BIT);
      assert(vkEndCommandBuffer(bad_source_layout_cmd) ==
             R3V_NATIVE_REFUSAL_RESULT);

      VkCommandBuffer destination_layout_cmd = fresh_cmd();
      record_image_barrier(
         destination_layout_cmd, destination_only,
         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, VK_ACCESS_TRANSFER_WRITE_BIT);
      assert(vkEndCommandBuffer(destination_layout_cmd) == VK_SUCCESS);

      VkCommandBuffer bad_destination_layout_cmd = fresh_cmd();
      record_image_barrier(
         bad_destination_layout_cmd, destination_only,
         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, VK_ACCESS_TRANSFER_READ_BIT);
      assert(vkEndCommandBuffer(bad_destination_layout_cmd) ==
             R3V_NATIVE_REFUSAL_RESULT);

      /* Capacity calibration covers the first allocation, growth past the
       * former ceiling, every deferred-copy kind, reset reuse, and a
       * command-scope allocation refusal.  The command buffer owns the
       * storage through the pool allocator, so reset releases it before the
       * next begin allocates a fresh first block.
       */
      const VkImageSubresourceRange capacity_range = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      };
      const VkClearColorValue capacity_clear = {
         .float32 = { 0.25f, 0.5f, 0.75f, 1.0f },
      };

      VkCommandBuffer sixteen_cmd = fresh_cmd();
      record_image_barrier(
         sixteen_cmd, img_a, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0);
      for (uint32_t i = 0; i < 16; i++) {
         vkCmdClearColorImage(sixteen_cmd, img_a,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &capacity_clear, 1, &capacity_range);
      }
      VK_FROM_HANDLE(r3v_native_cmd_buffer, sixteen_native, sixteen_cmd);
      assert(sixteen_native->deferred_copy_count == 16);
      assert(sixteen_native->deferred_copy_capacity == 16);
      assert(sixteen_native->deferred_copies != NULL);
      assert(vkEndCommandBuffer(sixteen_cmd) == VK_SUCCESS);

      VkCommandBuffer seventeen_cmd = fresh_cmd();
      record_image_barrier(
         seventeen_cmd, img_a, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0);
      for (uint32_t i = 0; i < 17; i++) {
         vkCmdClearColorImage(seventeen_cmd, img_a,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &capacity_clear, 1, &capacity_range);
      }
      VK_FROM_HANDLE(r3v_native_cmd_buffer, seventeen_native, seventeen_cmd);
      assert(seventeen_native->deferred_copy_count == 17);
      assert(seventeen_native->deferred_copy_capacity == 32);
      assert(seventeen_native->deferred_copies != NULL);
      assert(vkEndCommandBuffer(seventeen_cmd) == VK_SUCCESS);

      VkCommandBuffer mixed_cmd = fresh_cmd();
      for (uint32_t i = 0; i < 8; i++) {
         record_image_barrier(
            mixed_cmd, img_a,
            i == 0 ? VK_IMAGE_LAYOUT_UNDEFINED
                    : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            i == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                    : VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            i == 0 ? 0 : VK_ACCESS_TRANSFER_READ_BIT, 0);
         vkCmdClearColorImage(mixed_cmd, img_a,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &capacity_clear, 1, &capacity_range);
         vkCmdCopyBufferToImage(mixed_cmd, staging, img_a,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &upload);
         record_image_barrier(
            mixed_cmd, img_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
         record_image_barrier(
            mixed_cmd, img_b,
            i == 0 ? VK_IMAGE_LAYOUT_UNDEFINED
                    : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            i == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                    : VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            i == 0 ? 0 : VK_ACCESS_TRANSFER_READ_BIT, 0);
         vkCmdCopyImage(mixed_cmd, img_a,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img_b,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cross);
         record_image_barrier(
            mixed_cmd, img_b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
         vkCmdCopyImageToBuffer(mixed_cmd, img_b,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                staging, 1, &readback);
      }
      VK_FROM_HANDLE(r3v_native_cmd_buffer, mixed_native, mixed_cmd);
      assert(mixed_native->deferred_copy_count == 32);
      assert(mixed_native->deferred_copy_capacity == 32);
      assert(mixed_native->deferred_copies != NULL);
      for (uint32_t i = 0; i < 8; i++) {
         const uint32_t base = i * 4;
         assert(mixed_native->deferred_copies[base].kind ==
                R3V_NATIVE_COPY_CLEAR_IMAGE);
         assert(mixed_native->deferred_copies[base + 1].kind ==
                R3V_NATIVE_COPY_BUFFER_TO_IMAGE);
         assert(mixed_native->deferred_copies[base + 2].kind ==
                R3V_NATIVE_COPY_IMAGE_TO_IMAGE);
         assert(mixed_native->deferred_copies[base + 3].kind ==
                R3V_NATIVE_COPY_IMAGE_TO_BUFFER);
      }
      assert(vkEndCommandBuffer(mixed_cmd) == VK_SUCCESS);

      assert(vkBeginCommandBuffer(
                mixed_cmd,
                &(VkCommandBufferBeginInfo){
                   .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                }) == VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, reset_native, mixed_cmd);
      assert(reset_native->deferred_copy_count == 0);
      assert(reset_native->deferred_copy_capacity == 0);
      assert(reset_native->deferred_copies == NULL);
      record_image_barrier(
         mixed_cmd, img_a, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0);
      vkCmdClearColorImage(mixed_cmd, img_a,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &capacity_clear, 1, &capacity_range);
      assert(reset_native->deferred_copy_count == 1);
      assert(reset_native->deferred_copy_capacity == 16);
      assert(vkEndCommandBuffer(mixed_cmd) == VK_SUCCESS);

      VkCommandBuffer allocation_failure_cmd = fresh_cmd();
      VK_FROM_HANDLE(r3v_native_cmd_buffer, allocation_failure_native,
                     allocation_failure_cmd);
      struct deferred_copy_allocation_control allocation_control = {
         .command_allocations_before_failure = 0,
      };
      const VkAllocationCallbacks saved_allocator =
         allocation_failure_native->vk.pool->alloc;
      allocation_failure_native->vk.pool->alloc = (VkAllocationCallbacks){
         .pUserData = &allocation_control,
         .pfnAllocation = deferred_copy_test_allocate,
         .pfnReallocation = deferred_copy_test_reallocate,
         .pfnFree = deferred_copy_test_free,
      };
      record_image_barrier(
         allocation_failure_cmd, img_a, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0);
      vkCmdClearColorImage(allocation_failure_cmd, img_a,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &capacity_clear, 1, &capacity_range);
      assert(allocation_failure_native->deferred_copy_count == 0);
      assert(allocation_failure_native->deferred_copy_capacity == 0);
      assert(allocation_failure_native->deferred_copies == NULL);
      assert(vkEndCommandBuffer(allocation_failure_cmd) ==
             VK_ERROR_OUT_OF_HOST_MEMORY);
      allocation_failure_native->vk.pool->alloc = saved_allocator;

      VkCommandBuffer growth_failure_cmd = fresh_cmd();
      VK_FROM_HANDLE(r3v_native_cmd_buffer, growth_failure_native,
                     growth_failure_cmd);
      allocation_control = (struct deferred_copy_allocation_control){
         .command_allocations_before_failure = 1,
      };
      const VkAllocationCallbacks growth_saved_allocator =
         growth_failure_native->vk.pool->alloc;
      growth_failure_native->vk.pool->alloc = (VkAllocationCallbacks){
         .pUserData = &allocation_control,
         .pfnAllocation = deferred_copy_test_allocate,
         .pfnReallocation = deferred_copy_test_reallocate,
         .pfnFree = deferred_copy_test_free,
      };
      record_image_barrier(
         growth_failure_cmd, img_a, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0);
      for (uint32_t i = 0; i < 17; i++) {
         vkCmdClearColorImage(growth_failure_cmd, img_a,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              &capacity_clear, 1, &capacity_range);
      }
      assert(growth_failure_native->deferred_copy_count == 16);
      assert(growth_failure_native->deferred_copy_capacity == 16);
      assert(growth_failure_native->deferred_copies != NULL);
      assert(vkEndCommandBuffer(growth_failure_cmd) ==
             VK_ERROR_OUT_OF_HOST_MEMORY);
      growth_failure_native->vk.pool->alloc = growth_saved_allocator;

      assert(vkQueueSubmit(queue, 1,
                           &(VkSubmitInfo){
                              .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &copy_cmd,
                           },
                           VK_NULL_HANDLE) == VK_SUCCESS);

      /* The pattern round-tripped through the buffer and both images: the
       * staging tail carries every texel, and the texel before and after the
       * copy rectangle in img_b still carries the sentinel, so the
       * pitch walk stayed inside the region.
       */
      assert(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0,
                         (void **)&staging_map) == VK_SUCCESS);
      for (uint32_t t = 0; t < copy_w * copy_h; t++)
         assert(staging_map[t + 1024 / sizeof(*staging_map)] ==
                (0x40000000u | t));
      for (uint32_t t = 0; t < copy_w * copy_h; t++)
         assert(staging_map[t + 512] == (0x40000000u | t));
      vkUnmapMemory(device, staging_mem);
      assert(vkMapMemory(device, mem_b, 0, VK_WHOLE_SIZE, 0,
                         (void **)&pixel_map) == VK_SUCCESS);
      assert(pixel_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
      assert(pixel_map[transfer_image_base_word + 3 * 16 + 4] ==
             R300_TRIANGLE_COLOR_SENTINEL);
      assert(pixel_map[transfer_image_base_word + 3 * 16 + 5] ==
             (0x40000000u | 0));
      assert(pixel_map[transfer_image_base_word + 2 * 16 + 5] ==
             R300_TRIANGLE_COLOR_SENTINEL);
      /* The right and bottom neighbors hold too: a pitch or extent
       * defect overruns past the rectangle's far edges, the side the
       * left and top sentinels cannot see.
       */
      assert(pixel_map[transfer_image_base_word + 3 * 16 + 13] ==
             R300_TRIANGLE_COLOR_SENTINEL);
      assert(pixel_map[transfer_image_base_word + 7 * 16 + 5] ==
             R300_TRIANGLE_COLOR_SENTINEL);
      vkUnmapMemory(device, mem_b);
      assert(vkMapMemory(device, mem_a, 0, VK_WHOLE_SIZE, 0,
                         (void **)&pixel_map) == VK_SUCCESS);
      assert(pixel_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
      assert(pixel_map[transfer_image_base_word + 16 + 2] ==
             (0x40000000u | 0));
      vkUnmapMemory(device, mem_a);

      /* Whole-image clear on a padded-pitch image: a 3-texel row rides
       * a 16-texel pitch, so the fill's row walk is observable -- the
       * cleared texels carry the packed unorm color, the pitch padding
       * and the bytes past the footprint keep the sentinel.  The
       * second clear carries a NaN green component, which converts as
       * zero rather than tripping the float-to-integer cast.
       */
      {
         VkImageCreateInfo clear_info = transfer_info;
         clear_info.extent.width = 3;
         clear_info.extent.height = 5;
         VkImage img_c = VK_NULL_HANDLE;
         assert(vkCreateImage(device, &clear_info, NULL, &img_c) ==
                VK_SUCCESS);
         VkDeviceMemory mem_c = VK_NULL_HANDLE;
         assert(vkAllocateMemory(device, &transfer_alloc, NULL, &mem_c) ==
                VK_SUCCESS);
         assert(vkBindImageMemory(device, img_c, mem_c,
                                  transfer_image_offset) == VK_SUCCESS);
         uint32_t *clear_map;
         assert(vkMapMemory(device, mem_c, 0, VK_WHOLE_SIZE, 0,
                            (void **)&clear_map) == VK_SUCCESS);
         for (uint32_t t = 0; t < transfer_allocation_words; t++)
            clear_map[t] = R300_TRIANGLE_COLOR_SENTINEL;
         vkUnmapMemory(device, mem_c);

         VkCommandBuffer clear_cmd = fresh_cmd();
         const VkImageSubresourceRange full_range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
         };
         record_image_barrier(
            clear_cmd, img_c, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0);
         vkCmdClearColorImage(
            clear_cmd, img_c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &(VkClearColorValue){ .float32 = { 1.0f, 0.5f, 0.0f, 1.0f } },
            1, &full_range);
         assert(vkEndCommandBuffer(clear_cmd) == VK_SUCCESS);
         assert(vkQueueSubmit(queue, 1,
                              &(VkSubmitInfo){
                                 .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &clear_cmd,
                              },
                              VK_NULL_HANDLE) == VK_SUCCESS);
         assert(vkMapMemory(device, mem_c, 0, VK_WHOLE_SIZE, 0,
                            (void **)&clear_map) == VK_SUCCESS);
         assert(clear_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
         for (uint32_t row = 0; row < 5; row++) {
            for (uint32_t x = 0; x < 3; x++)
               assert(clear_map[transfer_image_base_word + row * 16 + x] ==
                      util_cpu_to_le32(0xffff8000u));
            assert(clear_map[transfer_image_base_word + row * 16 + 3] ==
                   R300_TRIANGLE_COLOR_SENTINEL);
         }
         assert(clear_map[transfer_image_base_word + 5 * 16] ==
                R300_TRIANGLE_COLOR_SENTINEL);
         vkUnmapMemory(device, mem_c);

         float nan_green;
         const uint32_t nan_green_bits = 0x7fc00000u;
         memcpy(&nan_green, &nan_green_bits, sizeof(nan_green));
         VkCommandBuffer nan_cmd = fresh_cmd();
         vkCmdClearColorImage(
            nan_cmd, img_c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &(VkClearColorValue){
               .float32 = { 1.0f, nan_green, 0.0f, 1.0f } },
            1, &full_range);
         assert(vkEndCommandBuffer(nan_cmd) == VK_SUCCESS);
         assert(vkQueueSubmit(queue, 1,
                              &(VkSubmitInfo){
                                 .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &nan_cmd,
                              },
                              VK_NULL_HANDLE) == VK_SUCCESS);
         assert(vkMapMemory(device, mem_c, 0, VK_WHOLE_SIZE, 0,
                            (void **)&clear_map) == VK_SUCCESS);
         assert(clear_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
         assert(clear_map[transfer_image_base_word] ==
                util_cpu_to_le32(0xffff0000u));
         vkUnmapMemory(device, mem_c);

         /* A clear on a render-family image poisons: the family
          * carries no transfer usage.
          */
         VkCommandBuffer bad_clear = fresh_cmd();
         vkCmdClearColorImage(
            bad_clear, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &(VkClearColorValue){ .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } },
            1, &full_range);
         assert(vkEndCommandBuffer(bad_clear) == R3V_NATIVE_REFUSAL_RESULT);

         vkDestroyImage(device, img_c, NULL);
         vkFreeMemory(device, mem_c, NULL);
      }

      /* Refusals poison the recording: a region past the image, a
       * source buffer without TRANSFER_SRC, and a copy after the
       * render pass began.
       */
      VkCommandBuffer bad_copy = fresh_cmd();
      VkBufferImageCopy bad_region = upload;
      bad_region.imageOffset.x = 9;
      vkCmdCopyBufferToImage(bad_copy, staging, img_a,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &bad_region);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      vkCmdCopyBufferToImage(bad_copy, vertex_buffer, img_a,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &upload);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      vkCmdCopyBufferToImage(bad_copy, staging, img_a,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1,
                             &upload);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      vkCmdCopyBuffer(
         bad_copy, staging, staging, 1,
         &(VkBufferCopy){
            .srcOffset = 4092,
            .dstOffset = 0,
            .size = 8,
         });
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      /* Equal foreign indices are ownership-transfer metadata too: the
       * native device exposes family 0 and the ignored sentinel only.
       */
      bad_copy = fresh_cmd();
      vkCmdPipelineBarrier(
         bad_copy, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
         &(VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcQueueFamilyIndex = 1,
            .dstQueueFamilyIndex = 1,
            .buffer = staging,
            .size = VK_WHOLE_SIZE,
         },
         0, NULL);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      vkCmdPipelineBarrier(
         bad_copy, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
         &(VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img_a,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .levelCount = 1,
                                  .layerCount = 1 },
         });
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      vkCmdPipelineBarrier(
         bad_copy, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
         &(VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcQueueFamilyIndex = 1,
            .dstQueueFamilyIndex = 1,
            .image = img_a,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .levelCount = 1,
                                  .layerCount = 1 },
         });
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      vkCmdBeginRenderPass(bad_copy, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdCopyBufferToImage(bad_copy, staging, img_a,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &upload);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      /* Wrapping arithmetic refuses: an extent whose 32-bit offset sum
       * would wrap to zero, and a bufferOffset whose 64-bit footprint
       * sum would wrap past the buffer, each poison at record -- the
       * containment proofs run widened, so neither reaches execution.
       */
      bad_copy = fresh_cmd();
      const VkImageCopy wrap_extent = {
         .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1 },
         .srcOffset = { 1, 0, 0 },
         .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1 },
         .dstOffset = { 1, 0, 0 },
         .extent = { 0xffffffffu, 1, 1 },
      };
      vkCmdCopyImage(bad_copy, img_a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     img_b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &wrap_extent);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      /* Known from r3v_native_queue_submit (rg --fixed-strings
       * "r3v_native_queue_submit"
       * src/amd/r300/vulkan/r3v_native_queue.c) and r3v_cpu_sync_wait
       * (rg --fixed-strings "r3v_cpu_sync_wait"
       * src/amd/r300/vulkan/r3v_cpu_sync.c): the native queue waits each
       * binary dependency before replaying deferred copies, consumes the
       * waited binary state after replay, and signals its completion set.
       * This harness mutates the source and readback regions before a
       * second submit, then checks the semaphore payload under its mutex.
       */
      {
         VkFence fence = VK_NULL_HANDLE;
         assert(vkCreateFence(device,
                              &(VkFenceCreateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                              },
                              NULL, &fence) == VK_SUCCESS);
         assert(vkGetFenceStatus(device, fence) == VK_NOT_READY);
         VkSemaphore chain = VK_NULL_HANDLE;
         assert(vkCreateSemaphore(
                   device,
                   &(VkSemaphoreCreateInfo){
                      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                   },
                   NULL, &chain) == VK_SUCCESS);

         assert(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0,
                            (void **)&staging_map) == VK_SUCCESS);
         for (uint32_t t = 0; t < copy_w * copy_h; t++)
            staging_map[t] = 0x50000000u | t;
         for (uint32_t t = 0; t < copy_w * copy_h; t++)
            staging_map[t + 512] = 0xfeedc0deu;
         vkUnmapMemory(device, staging_mem);

         VkCommandBuffer sync_cmd = fresh_cmd();
         vkCmdCopyBuffer(sync_cmd, staging, staging, 1, &buffer_copy);
         assert(vkEndCommandBuffer(sync_cmd) == VK_SUCCESS);

         assert(vkQueueSubmit(queue, 1,
                              &(VkSubmitInfo){
                                 .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &copy_cmd,
                                 .signalSemaphoreCount = 1,
                                 .pSignalSemaphores = &chain,
                              },
                              fence) == VK_SUCCESS);
         assert(vkWaitForFences(device, 1, &fence, VK_TRUE,
                                UINT64_MAX) == VK_SUCCESS);
         assert(r3v_native_binary_semaphore_is_signaled(chain));

         const VkPipelineStageFlags chain_stage =
            VK_PIPELINE_STAGE_TRANSFER_BIT;
         assert(vkQueueSubmit(queue, 1,
                              &(VkSubmitInfo){
                                 .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &chain,
                                 .pWaitDstStageMask = &chain_stage,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &sync_cmd,
                              },
                              VK_NULL_HANDLE) == VK_SUCCESS);
         assert(!r3v_native_binary_semaphore_is_signaled(chain));

         assert(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0,
                            (void **)&staging_map) == VK_SUCCESS);
         for (uint32_t t = 0; t < copy_w * copy_h; t++)
            assert(staging_map[t + 512] == (0x50000000u | t));
         vkUnmapMemory(device, staging_mem);

         vkDestroySemaphore(device, chain, NULL);
         vkDestroyFence(device, fence, NULL);
      }

      /* Barrier refusals: an ownership transfer names a queue family
       * the one-family device does not expose, and a barrier inside
       * the render pass has no self-dependency lowering.
       */
      bad_copy = fresh_cmd();
      vkCmdPipelineBarrier(
         bad_copy, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
         &(VkBufferMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcQueueFamilyIndex = 0,
            .dstQueueFamilyIndex = 1,
            .buffer = staging,
            .size = VK_WHOLE_SIZE,
         },
         0, NULL);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      vkCmdBeginRenderPass(bad_copy, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdPipelineBarrier(bad_copy, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                           NULL, 0, NULL);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, barrier_native, bad_copy);
      assert(barrier_native->vk.record_result == R3V_NATIVE_REFUSAL_RESULT);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      bad_copy = fresh_cmd();
      VkBufferImageCopy wrap_offset = upload;
      wrap_offset.bufferOffset = 0xfffffffffffffffcull;
      vkCmdCopyBufferToImage(bad_copy, staging, img_a,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &wrap_offset);
      assert(vkEndCommandBuffer(bad_copy) == R3V_NATIVE_REFUSAL_RESULT);

      vkDestroyBuffer(device, staging, NULL);
      vkFreeMemory(device, staging_mem, NULL);
      vkDestroyImage(device, source_only, NULL);
      vkDestroyImage(device, destination_only, NULL);
      vkFreeMemory(device, source_only_memory, NULL);
      vkFreeMemory(device, destination_only_memory, NULL);
      vkDestroyImage(device, img_a, NULL);
      vkDestroyImage(device, img_b, NULL);
      vkFreeMemory(device, mem_a, NULL);
      vkFreeMemory(device, mem_b, NULL);
   }

   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   assert(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE) ==
          VK_ERROR_DEVICE_LOST);

   void *carrier_map = NULL;
   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_cmd->owned_carrier->bo,
                               &carrier_map) == 0);
   {
      /* The carrier holds the transformed stream: the mutated NDC x
       * (-0.75 with binary32 mantissa bit 22 flipped, -0.5) maps
       * through the same viewport expression the execution applies.
       */
      float mutated_ndc_x;
      const uint32_t mutated_bits = original_first_dword ^ 0x00400000u;
      memcpy(&mutated_ndc_x, &mutated_bits, sizeof(mutated_ndc_x));
      const float expected_window_x = (mutated_ndc_x + 1.0f) * 32.0f;
      uint32_t expected_bits;
      memcpy(&expected_bits, &expected_window_x, sizeof(expected_bits));
      uint32_t carrier_first;
      memcpy(&carrier_first, carrier_map, sizeof(carrier_first));
      assert(carrier_first == expected_bits);
   }
   radeon_drm_vk_bo_unmap(&native_device->drm,
                          &native_cmd->owned_carrier->bo, carrier_map);

   /* Restore and re-execute: the runtime latches the device lost after
    * the refused submit and later submits return before the driver
    * runs, so re-execution exercises the queue's execution step
    * directly -- the harness links the implementation.  Each execution
    * re-reads, so the carrier now equals the restored reference
    * stream.
    */
   assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
          VK_SUCCESS);
   memcpy(map, ndc_triangle, sizeof(ndc_triangle));
   vkUnmapMemory(device, vertex_memory);
   assert(r3v_native_cmd_buffer_execute_deferred_draw(
             native_device, native_cmd) == VK_SUCCESS);

   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_cmd->owned_carrier->bo,
                               &carrier_map) == 0);
   assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                 R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
   radeon_drm_vk_bo_unmap(&native_device->drm,
                          &native_cmd->owned_carrier->bo, carrier_map);

   /* The load-op clear executed at submission over the image's declared
    * footprint alone: sentinel inside, the page past the footprint
    * untouched.
    */
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&color_map) == VK_SUCCESS);
   assert(color_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
   assert(color_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] ==
          R300_TRIANGLE_COLOR_SENTINEL);
   assert(color_map[R3V_NATIVE_TARGET_MEMORY_BYTES / 4] == COLOR_SEED);
   vkUnmapMemory(device, color_memory);

   /* The F32_3 delivery shape reaches the same cell: the reference
    * vertices carry w = 1, so an xyz stream reproduces the carrier.
    */
   float xyz[9];
   for (unsigned v = 0; v < 3; v++)
      memcpy(&xyz[v * 3], &ndc_triangle[v * 4], 12);
   assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
          VK_SUCCESS);
   memcpy(map, xyz, sizeof(xyz));
   vkUnmapMemory(device, vertex_memory);

   const struct pipeline_shape xyz_shape = {
      .attribute_format = VK_FORMAT_R32G32B32_SFLOAT,
      .stride = 12,
      .blend_enable = VK_FALSE,
      .fragment_words = r3v_reference_fragment_spirv,
      .fragment_bytes = sizeof(r3v_reference_fragment_spirv),
   };
   VkPipeline xyz_pipeline = VK_NULL_HANDLE;
   assert(make_pipeline(&xyz_shape, pass, layout, &xyz_pipeline) ==
             VK_SUCCESS);
   VkCommandBuffer xyz_cmd = fresh_cmd();
   vkCmdBeginRenderPass(xyz_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(xyz_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     xyz_pipeline);
   vkCmdBindVertexBuffers(xyz_cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(xyz_cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(xyz_cmd);
   assert(vkEndCommandBuffer(xyz_cmd) == VK_SUCCESS);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_xyz, xyz_cmd);
   assert(r3v_native_cmd_buffer_execute_deferred_draw(
             native_device, native_xyz) == VK_SUCCESS);
   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_xyz->owned_carrier->bo,
                               &carrier_map) == 0);
   assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                 R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
   radeon_drm_vk_bo_unmap(&native_device->drm,
                          &native_xyz->owned_carrier->bo, carrier_map);

   /* A record outside the admitted clip volume, or one whose w is not
    * exactly 1, refuses at execution: the transform's perspective
    * divide is the identity only there, so the route reports instead
    * of rasterizing an untransformed stream.
    */
   {
      float out_of_domain[12];
      memcpy(out_of_domain, ndc_triangle, sizeof(out_of_domain));
      out_of_domain[3] = 2.0f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, out_of_domain, sizeof(out_of_domain));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer domain_cmd = fresh_cmd();
      vkCmdBeginRenderPass(domain_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(domain_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline);
      vkCmdBindVertexBuffers(domain_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(domain_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(domain_cmd);
      assert(vkEndCommandBuffer(domain_cmd) == VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_domain, domain_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_domain) != VK_SUCCESS);

      /* A NaN coordinate refuses through the same gate: the domain
       * check is a negated conjunction of ordered comparisons, and
       * every ordered comparison on NaN is false, so the negation
       * admits the record into the refusal branch rather than past it.
       */
      uint32_t nan_bits = 0x7fc00000u;
      memcpy(&out_of_domain[0], &nan_bits, sizeof(nan_bits));
      out_of_domain[3] = 1.0f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, out_of_domain, sizeof(out_of_domain));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer nan_cmd = fresh_cmd();
      vkCmdBeginRenderPass(nan_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(nan_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline);
      vkCmdBindVertexBuffers(nan_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(nan_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(nan_cmd);
      assert(vkEndCommandBuffer(nan_cmd) == VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_nan, nan_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_nan) != VK_SUCCESS);

      /* Restore the reference stream for the legs below. */
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

   /* The R2VB identity delivery route keeps its source-domain boundary
    * visible at this API-shaped call site.  The canonical NDC payload
    * contains negative coordinates, so it stays on the CPU route with
    * the gate unset.  A separate non-negative FP24 payload proves
    * positive host-model admission under the exact opt-in; replacing its
    * first component with off-grid 0.1 then refuses, while the same
    * bytes remain valid on the CPU route.  The common carrier test also
    * compares admitted delivery against both CPU oracles without a
    * viewport transform.
    */
   {
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_cmd) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_cmd->owned_carrier->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_cmd->owned_carrier->bo, carrier_map);

      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, positive_triangle, sizeof(positive_triangle));
      vkUnmapMemory(device, vertex_memory);
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_cmd) == VK_SUCCESS);

      float narrow[12];
      memcpy(narrow, positive_triangle, sizeof(narrow));
      narrow[0] = 0.1f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, narrow, sizeof(narrow));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_cmd) != VK_SUCCESS);

      /* The same stream rides the CPU route: an unset gate and a non-"1"
       * value both keep the default delivery.
       */
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "0", 1) == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_cmd) == VK_SUCCESS);
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_cmd) == VK_SUCCESS);

      /* Restore the reference stream for the legs below. */
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

   /* The synthesized delivery shapes preserve the same route boundary:
    * negative NDC coordinates use the CPU route with the gate unset,
    * while non-negative FP24 inputs admit under the exact host-model
    * opt-in.  Replacing one positive component with off-grid 0.1 refuses
    * under that gate and succeeds on the CPU route.  The common carrier
    * test owns the positive admitted identity comparison for F32_3 and
    * F32_2, independent of viewport execution.
    */
   {
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);

      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, xyz, sizeof(xyz));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xyz) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_xyz->owned_carrier->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_xyz->owned_carrier->bo, carrier_map);

      float positive_xyz[9];
      for (unsigned v = 0; v < 3; v++)
         memcpy(&positive_xyz[v * 3], &positive_triangle[v * 4], 12);
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, positive_xyz, sizeof(positive_xyz));
      vkUnmapMemory(device, vertex_memory);
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xyz) == VK_SUCCESS);
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);

      float xy[6];
      for (unsigned v = 0; v < 3; v++)
         memcpy(&xy[v * 2], &ndc_triangle[v * 4], 8);
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, xy, sizeof(xy));
      vkUnmapMemory(device, vertex_memory);
      const struct pipeline_shape xy_shape = {
         .attribute_format = VK_FORMAT_R32G32_SFLOAT,
         .stride = 8,
         .blend_enable = VK_FALSE,
         .fragment_words = r3v_reference_fragment_spirv,
         .fragment_bytes = sizeof(r3v_reference_fragment_spirv),
      };
      VkPipeline xy_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&xy_shape, pass, layout, &xy_pipeline) ==
                VK_SUCCESS);
      VkCommandBuffer xy_cmd = fresh_cmd();
      vkCmdBeginRenderPass(xy_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(xy_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        xy_pipeline);
      vkCmdBindVertexBuffers(xy_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(xy_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(xy_cmd);
      assert(vkEndCommandBuffer(xy_cmd) == VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_xy, xy_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xy) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_xy->owned_carrier->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_xy->owned_carrier->bo, carrier_map);

      float positive_xy[6];
      for (unsigned v = 0; v < 3; v++)
         memcpy(&positive_xy[v * 2], &positive_triangle[v * 4], 8);
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, positive_xy, sizeof(positive_xy));
      vkUnmapMemory(device, vertex_memory);
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xy) == VK_SUCCESS);

      /* Domain narrowing per shape: an off-grid x refuses the F32_3
       * and F32_2 deliveries under the gate and rides the CPU gather
       * without it.
       */
      float xyz_narrow[9];
      memcpy(xyz_narrow, positive_xyz, sizeof(xyz_narrow));
      xyz_narrow[0] = 0.1f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, xyz_narrow, sizeof(xyz_narrow));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xyz) != VK_SUCCESS);

      /* Replay the same F32_3 bytes before staging the F32_2 payload. */
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xyz) == VK_SUCCESS);
      float expected_xyz_cpu[12];
      for (unsigned v = 0; v < 3; v++) {
         expected_xyz_cpu[v * 4 + 0] =
            (xyz_narrow[v * 3 + 0] + 1.0f) * 32.0f;
         expected_xyz_cpu[v * 4 + 1] =
            (xyz_narrow[v * 3 + 1] + 1.0f) * 32.0f;
         expected_xyz_cpu[v * 4 + 2] = xyz_narrow[v * 3 + 2];
         expected_xyz_cpu[v * 4 + 3] = 1.0f;
      }
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_xyz->owned_carrier->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, expected_xyz_cpu,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_xyz->owned_carrier->bo, carrier_map);

      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);

      float xy_narrow[6];
      memcpy(xy_narrow, positive_xy, sizeof(xy_narrow));
      xy_narrow[0] = 0.1f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, xy_narrow, sizeof(xy_narrow));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xy) != VK_SUCCESS);
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xy) == VK_SUCCESS);

      vkDestroyPipeline(device, xy_pipeline, NULL);

      /* Restore the reference stream for the legs below. */
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

   /* The extent family through the public route: a 48x20 target's
    * footprint follows the fixed 256-byte pitch, its recorded IB
    * deviates from the reference cell in the two scissor-family dwords
    * alone, and the deferred clear covers exactly the declared
    * footprint.
    */
   {
      const uint32_t sub_w = 48, sub_h = 20;
      VkImage sub_image = VK_NULL_HANDLE;
      VkImageCreateInfo sub_info = image_info;
      sub_info.extent.width = sub_w;
      sub_info.extent.height = sub_h;
      assert(vkCreateImage(device, &sub_info, NULL, &sub_image) ==
             VK_SUCCESS);
      VkMemoryRequirements sub_reqs;
      vkGetImageMemoryRequirements(device, sub_image, &sub_reqs);
      assert(sub_reqs.size ==
             (VkDeviceSize)R3V_NATIVE_TARGET_ROW_BYTES * (sub_h + 1));

      VkDeviceMemory sub_memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(
                device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = sub_reqs.size + 4096,
                   .memoryTypeIndex = 0,
                },
                NULL, &sub_memory) == VK_SUCCESS);
      assert(vkBindImageMemory(device, sub_image, sub_memory, 0) ==
             VK_SUCCESS);
      uint32_t *sub_map = NULL;
      assert(vkMapMemory(device, sub_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&sub_map) == VK_SUCCESS);
      for (VkDeviceSize i = 0; i < (sub_reqs.size + 4096) / 4; i++)
         sub_map[i] = COLOR_SEED;
      vkUnmapMemory(device, sub_memory);

      VkImageView sub_view = VK_NULL_HANDLE;
      assert(vkCreateImageView(
                device,
                &(VkImageViewCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                   .image = sub_image,
                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                   .format = R3V_NATIVE_TARGET_FORMAT,
                   .subresourceRange = { .aspectMask =
                                            VK_IMAGE_ASPECT_COLOR_BIT,
                                         .levelCount = 1,
                                         .layerCount = 1 },
                },
                NULL, &sub_view) == VK_SUCCESS);
      VkFramebuffer sub_framebuffer = VK_NULL_HANDLE;
      assert(vkCreateFramebuffer(
                device,
                &(VkFramebufferCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                   .renderPass = pass,
                   .attachmentCount = 1,
                   .pAttachments = &sub_view,
                   .width = sub_w,
                   .height = sub_h,
                   .layers = 1,
                },
                NULL, &sub_framebuffer) == VK_SUCCESS);

      struct pipeline_shape sub_shape = contract_shape;
      sub_shape.extent_width = sub_w;
      sub_shape.extent_height = sub_h;
      VkPipeline sub_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&sub_shape, pass, layout, &sub_pipeline) ==
             VK_SUCCESS);

      VkRenderPassBeginInfo sub_begin = begin_pass;
      sub_begin.framebuffer = sub_framebuffer;
      sub_begin.renderArea =
         (VkRect2D){ .extent = { sub_w, sub_h } };
      VkCommandBuffer sub_cmd = fresh_cmd();
      vkCmdBeginRenderPass(sub_cmd, &sub_begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(sub_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        sub_pipeline);
      vkCmdBindVertexBuffers(sub_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(sub_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(sub_cmd);
      assert(vkEndCommandBuffer(sub_cmd) == VK_SUCCESS);

      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_sub, sub_cmd);
      struct r300_tcl_bypass_triangle_ib sub_reference;
      assert(r300_tcl_bypass_triangle_reference_emit(&sub_reference) == 0);
      assert(native_sub->ib_size_dwords == sub_reference.ib_size_dwords);
      uint32_t deviating = 0;
      for (uint32_t d = 0; d < sub_reference.ib_size_dwords; d++) {
         if (native_sub->ib[d] != sub_reference.ib[d])
            deviating++;
      }
      assert(deviating == 2);
      r300_tcl_bypass_triangle_release(&sub_reference);

      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_sub) == VK_SUCCESS);
      assert(vkMapMemory(device, sub_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&sub_map) == VK_SUCCESS);
      assert(sub_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
      assert(sub_map[(sub_reqs.size / 4) - 1] ==
             R300_TRIANGLE_COLOR_SENTINEL);
      assert(sub_map[sub_reqs.size / 4] == COLOR_SEED);
      vkUnmapMemory(device, sub_memory);

      /* A pipeline whose extent claim deviates from the pass target
       * poisons at the draw.
       */
      VkCommandBuffer mismatch_cmd = fresh_cmd();
      vkCmdBeginRenderPass(mismatch_cmd, &sub_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(mismatch_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline);
      vkCmdBindVertexBuffers(mismatch_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(mismatch_cmd, 3, 1, 0, 0);
      /* The pass closes, so the refusal below is the draw's
       * extent-mismatch poison rather than the open-pass poison.
       */
      vkCmdEndRenderPass(mismatch_cmd);
      assert(vkEndCommandBuffer(mismatch_cmd) ==
             R3V_NATIVE_REFUSAL_RESULT);

      vkDestroyPipeline(device, sub_pipeline, NULL);
      vkDestroyFramebuffer(device, sub_framebuffer, NULL);
      vkDestroyImageView(device, sub_view, NULL);
      vkDestroyImage(device, sub_image, NULL);
      vkFreeMemory(device, sub_memory, NULL);
   }

   /* Creation refusals: each deviation clears the handle and reports
    * the refusal result, so the negative legs calibrate the contract
    * checks the positive leg rode through.
    */
   struct pipeline_shape bad_shape = contract_shape;
   bad_shape.blend_enable = VK_TRUE;
   VkPipeline refused = VK_NULL_HANDLE;
   assert(make_pipeline(&bad_shape, pass, layout, &refused) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          refused == VK_NULL_HANDLE);

   uint32_t mutated_fs[sizeof(r3v_reference_fragment_spirv) / 4];
   memcpy(mutated_fs, r3v_reference_fragment_spirv, sizeof(mutated_fs));
   mutated_fs[sizeof(mutated_fs) / 4 / 2] ^= 1u;
   bad_shape = contract_shape;
   bad_shape.fragment_words = mutated_fs;
   bad_shape.fragment_bytes = sizeof(mutated_fs);
   assert(make_pipeline(&bad_shape, pass, layout, &refused) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          refused == VK_NULL_HANDLE);

   bad_shape = contract_shape;
   bad_shape.stride = 12;
   assert(make_pipeline(&bad_shape, pass, layout, &refused) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          refused == VK_NULL_HANDLE);

   VkImageCreateInfo bad_image_info = image_info;
   bad_image_info.extent.width = 0;
   VkImage bad_image = VK_NULL_HANDLE;
   assert(vkCreateImage(device, &bad_image_info, NULL, &bad_image) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          bad_image == VK_NULL_HANDLE);
   bad_image_info.extent.width = R3V_NATIVE_TARGET_WIDTH + 1;
   assert(vkCreateImage(device, &bad_image_info, NULL, &bad_image) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          bad_image == VK_NULL_HANDLE);
   bad_image_info = image_info;
   bad_image_info.extent.height = 0;
   assert(vkCreateImage(device, &bad_image_info, NULL, &bad_image) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          bad_image == VK_NULL_HANDLE);
   bad_image_info.extent.height = R3V_NATIVE_TARGET_HEIGHT + 1;
   assert(vkCreateImage(device, &bad_image_info, NULL, &bad_image) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          bad_image == VK_NULL_HANDLE);

   VkImage unbound_image = VK_NULL_HANDLE;
   assert(vkCreateImage(device, &image_info, NULL, &unbound_image) ==
          VK_SUCCESS);
   assert(vkBindImageMemory(device, unbound_image, color_memory, 4096) ==
          R3V_NATIVE_REFUSAL_RESULT);

   VkCommandBuffer unbound_layout_cmd = fresh_cmd();
   record_image_barrier(
      unbound_layout_cmd, unbound_image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
   assert(vkEndCommandBuffer(unbound_layout_cmd) ==
          R3V_NATIVE_REFUSAL_RESULT);

   /* The linear transfer family: transfer usage alone admits extents
    * past the render ceiling up to 2048 per axis, the row pitch aligns
    * to the 2D engine's 64-byte pitch unit, the footprint is the rows
    * alone, and no view admits the family.  Usage mixing the families
    * and empty usage both refuse.
    */
   {
      VkImageCreateInfo transfer_info = image_info;
      transfer_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      transfer_info.extent.width = 2048;
      transfer_info.extent.height = 2048;
      VkImage transfer_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &transfer_info, NULL,
                           &transfer_image) == VK_SUCCESS);
      VkMemoryRequirements transfer_reqs;
      vkGetImageMemoryRequirements(device, transfer_image, &transfer_reqs);
      assert(transfer_reqs.size == (VkDeviceSize)2048 * 4 * 2048);
      vkDestroyImage(device, transfer_image, NULL);

      /* A 3-pixel row aligns up to one 64-byte pitch unit, and the
       * layout query publishes the aligned pitch.
       */
      transfer_info.extent.width = 3;
      transfer_info.extent.height = 5;
      assert(vkCreateImage(device, &transfer_info, NULL,
                           &transfer_image) == VK_SUCCESS);
      vkGetImageMemoryRequirements(device, transfer_image, &transfer_reqs);
      assert(transfer_reqs.size == 64 * 5);
      VkSubresourceLayout transfer_layout;
      vkGetImageSubresourceLayout(
         device, transfer_image,
         &(VkImageSubresource){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT },
         &transfer_layout);
      assert(transfer_layout.rowPitch == 64 &&
             transfer_layout.size == 64 * 5);

      /* No view admits the transfer family. */
      VkImageView transfer_view = VK_NULL_HANDLE;
      assert(vkCreateImageView(
                device,
                &(VkImageViewCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                   .image = transfer_image,
                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                   .format = R3V_NATIVE_TARGET_FORMAT,
                   .subresourceRange = { .aspectMask =
                                            VK_IMAGE_ASPECT_COLOR_BIT,
                                         .levelCount = 1,
                                         .layerCount = 1 },
                },
                NULL, &transfer_view) == R3V_NATIVE_REFUSAL_RESULT &&
             transfer_view == VK_NULL_HANDLE);

      /* An allocation covering the row footprint binds; one below it
       * refuses -- the 64x64 transfer footprint is 16384 bytes against
       * the 4096-byte allocation.
       */
      assert(vkBindImageMemory(device, transfer_image, vertex_memory, 0) ==
             VK_SUCCESS);
      vkDestroyImage(device, transfer_image, NULL);

      transfer_info.extent.width = 64;
      transfer_info.extent.height = 64;
      assert(vkCreateImage(device, &transfer_info, NULL,
                           &transfer_image) == VK_SUCCESS);
      assert(vkBindImageMemory(device, transfer_image, vertex_memory, 0) ==
             R3V_NATIVE_REFUSAL_RESULT);
      vkDestroyImage(device, transfer_image, NULL);

      transfer_info.extent.width = R3V_NATIVE_TRANSFER_DIMENSION_MAX + 1;
      transfer_info.extent.height = 1;
      VkImage refused_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &transfer_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      VkImageCreateInfo mixed_info = image_info;
      mixed_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      assert(vkCreateImage(device, &mixed_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      VkImageCreateInfo empty_info = image_info;
      empty_info.usage = 0;
      assert(vkCreateImage(device, &empty_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);
   }


   /* Recording refusals: a deviating command poisons its buffer, so
    * vkEndCommandBuffer returns the error and the buffer never reaches
    * EXECUTABLE.
    */
   VkCommandBuffer bad_cmd = fresh_cmd();
   VkRenderPassBeginInfo bad_begin = begin_pass;
   bad_begin.pClearValues =
      &(VkClearValue){ .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
   vkCmdBeginRenderPass(bad_cmd, &bad_begin, VK_SUBPASS_CONTENTS_INLINE);
   assert(vkEndCommandBuffer(bad_cmd) == R3V_NATIVE_REFUSAL_RESULT);

   bad_cmd = fresh_cmd();
   vkCmdBeginRenderPass(bad_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(bad_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(bad_cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(bad_cmd, 4, 1, 0, 0);
   assert(vkEndCommandBuffer(bad_cmd) == R3V_NATIVE_REFUSAL_RESULT);

   bad_cmd = fresh_cmd();
   vkCmdBeginRenderPass(bad_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindVertexBuffers(bad_cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(bad_cmd, 3, 1, 0, 0);
   assert(vkEndCommandBuffer(bad_cmd) == R3V_NATIVE_REFUSAL_RESULT);

   /* A render pass left open poisons at vkEndCommandBuffer: the open
    * pass has no closing lowering, so the buffer never becomes
    * executable.
    */
   bad_cmd = fresh_cmd();
   vkCmdBeginRenderPass(bad_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(bad_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(bad_cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(bad_cmd, 3, 1, 0, 0);
   assert(vkEndCommandBuffer(bad_cmd) == R3V_NATIVE_REFUSAL_RESULT);

   /* A second render pass after the recorded cell refuses: one command
    * buffer carries one deferred target record, so it cannot represent a
    * second pass's clear and draw.
    */
   bad_cmd = fresh_cmd();
   vkCmdBeginRenderPass(bad_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(bad_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(bad_cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(bad_cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(bad_cmd);
   vkCmdBeginRenderPass(bad_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   assert(vkEndCommandBuffer(bad_cmd) == R3V_NATIVE_REFUSAL_RESULT);

   /* A bound range too short for the three records refuses at the draw
    * through the gather's bound proof.
    */
   VkBuffer short_buffer = VK_NULL_HANDLE;
   assert(vkCreateBuffer(device,
                         &(VkBufferCreateInfo){
                            .sType =
                               VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = 40,
                            .usage =
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &short_buffer) == VK_SUCCESS);
   assert(vkBindBufferMemory(device, short_buffer, vertex_memory, 0) ==
          VK_SUCCESS);
   bad_cmd = fresh_cmd();
   vkCmdBeginRenderPass(bad_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(bad_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(bad_cmd, 0, 1, &short_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(bad_cmd, 3, 1, 0, 0);
   assert(vkEndCommandBuffer(bad_cmd) == R3V_NATIVE_REFUSAL_RESULT);

   vkDestroyBuffer(device, short_buffer, NULL);
   vkDestroyImage(device, unbound_image, NULL);

   /* Freeing a mapped allocation performs the implicit unmap path.  The
    * cache publication count proves that publication runs before GEM close
    * without an explicit vkUnmapMemory call.
    */
   const uint64_t free_sync_before = native_device->drm.cache_sync_count;
   VkDeviceMemory implicitly_unmapped = VK_NULL_HANDLE;
   assert(vkAllocateMemory(
             device,
             &(VkMemoryAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = 4096,
                .memoryTypeIndex = 0,
             },
             NULL, &implicitly_unmapped) == VK_SUCCESS);
   VK_FROM_HANDLE(r3v_native_memory, native_implicitly_unmapped,
                  implicitly_unmapped);
   const uint32_t implicitly_unmapped_handle =
      native_implicitly_unmapped->bo.handle;
   void *implicitly_mapped = NULL;
   assert(vkMapMemory(device, implicitly_unmapped, 0, VK_WHOLE_SIZE, 0,
                      &implicitly_mapped) == VK_SUCCESS);
   ((uint32_t *)implicitly_mapped)[0] = COLOR_SEED;
   vkFreeMemory(device, implicitly_unmapped, NULL);
   assert(native_device->drm.cache_sync_count == free_sync_before + 2);
   mtx_lock(&native_device->drm.cache_event_mutex);
   const struct radeon_drm_vk_cache_event cache_event =
      native_device->drm.cache_sync_last;
   const struct radeon_drm_vk_close_event close_event =
      native_device->drm.bo_close_last;
   mtx_unlock(&native_device->drm.cache_event_mutex);
   assert(cache_event.map == (uintptr_t)implicitly_mapped);
   assert(cache_event.bo_handle == implicitly_unmapped_handle);
   assert(close_event.bo_handle == implicitly_unmapped_handle);
   assert(r3v_native_cache_publication_precedes_close(cache_event.sequence,
                                                      close_event.sequence));

   /* A close-before-publication sequence is an executable known-bad leg.
    * The same verdict rejects the inverted event numbers.
    */
   const uint64_t known_bad_close_event = 10;
   const uint64_t known_bad_cache_event = 11;
   assert(!r3v_native_cache_publication_precedes_close(
      known_bad_cache_event, known_bad_close_event));

   vkDestroyPipeline(device, xyz_pipeline, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyImageView(device, view, NULL);
   vkDestroyImage(device, image, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkFreeMemory(device, color_memory, NULL);

   /* Pool destruction frees every allocated command buffer, so the
    * owned-carrier release path runs for each recorded draw before the
    * device and instance close.
    */
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyDevice(device, NULL);
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
   destroy_instance(instance, NULL);

   printf("r3v_native_public_surface: the public render-pass/pipeline/draw "
          "sequence records the qualified cell, execution defers to "
          "submission, and every deviation refuses\n");
   return 0;
}
