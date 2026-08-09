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
#include "r3v_native_reference_spirv.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

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

static VkDevice device;
static VkCommandPool pool;

#define DEVICE_COMMANDS(f)                                                 \
   f(vkAllocateMemory) f(vkFreeMemory) f(vkMapMemory) f(vkUnmapMemory)     \
   f(vkCreateBuffer) f(vkDestroyBuffer) f(vkBindBufferMemory)              \
   f(vkCreateImage) f(vkDestroyImage) f(vkGetImageMemoryRequirements)      \
   f(vkBindImageMemory) f(vkCreateImageView) f(vkDestroyImageView)         \
   f(vkCreateRenderPass) f(vkDestroyRenderPass) f(vkCreateFramebuffer)     \
   f(vkDestroyFramebuffer) f(vkCreateShaderModule)                         \
   f(vkDestroyShaderModule) f(vkCreatePipelineLayout)                      \
   f(vkDestroyPipelineLayout) f(vkCreateGraphicsPipelines)                 \
   f(vkDestroyPipeline) f(vkCreateCommandPool) f(vkDestroyCommandPool)     \
   f(vkAllocateCommandBuffers) f(vkBeginCommandBuffer)                     \
   f(vkEndCommandBuffer) f(vkCmdBeginRenderPass) f(vkCmdEndRenderPass)     \
   f(vkCmdBindPipeline) f(vkCmdBindVertexBuffers) f(vkCmdDraw)             \
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

int
main(void)
{
   /* The gate stays closed by construction: recording is submit-free
    * and this harness never opens the hazard environment.
    */
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   assert(create_instance(&(VkInstanceCreateInfo){
                             .sType =
                                VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
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
             },
             NULL, &device) == VK_SUCCESS);

#define LOAD(name) name = (PFN_##name)gdpa(device, #name); assert(name);
   DEVICE_COMMANDS(LOAD)
#undef LOAD

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

   /* The positive leg: the full public sequence ends EXECUTABLE and
    * installs the reference cell against the byte-identical carrier.
    */
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

   VkCommandBuffer cmd = fresh_cmd();
   vkCmdBeginRenderPass(cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);

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

   /* The R2VB identity delivery route: on the exact opt-in value the
    * deferred draw delivers the F32_4 stream through the FP24
    * fixed-point identity model instead of the CPU gather, and the
    * final carrier is byte-identical to the CPU route's -- the NDC
    * reference payload is FP24-exact and the shared viewport transform
    * runs after delivery on both routes.  The routing itself is
    * witnessed by the domain narrowing: a component with set low
    * mantissa bits (0.1) rides the CPU gather and refuses only under
    * the R2VB route, and a non-"1" gate value keeps the CPU route.
    */
   {
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_cmd) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_cmd->owned_carrier->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_cmd->owned_carrier->bo, carrier_map);

      float narrow[12];
      memcpy(narrow, ndc_triangle, sizeof(narrow));
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

   /* The synthesized delivery shapes ride the same route: under the
    * gate, F32_3 delivers xyz with W synthesized as 1.0 and F32_2
    * delivers xy with Z = 0.0 / W = 1.0, the reference payload's own
    * values, so both carriers stay byte-identical to the reference --
    * on the delivery route and the CPU oracle alike.  The routing is
    * witnessed per shape by the domain narrowing: an off-grid source
    * component refuses only under the gate.
    */
   {
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);

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

      /* Domain narrowing per shape: an off-grid x refuses the F32_3
       * and F32_2 deliveries under the gate and rides the CPU gather
       * without it.
       */
      float xyz_narrow[9];
      memcpy(xyz_narrow, xyz, sizeof(xyz_narrow));
      xyz_narrow[0] = 0.1f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, xyz_narrow, sizeof(xyz_narrow));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_xyz) != VK_SUCCESS);

      float xy_narrow[6];
      memcpy(xy_narrow, xy, sizeof(xy_narrow));
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

   bad_cmd = fresh_cmd();
   vkCmdBeginRenderPass(bad_cmd, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdEndRenderPass(bad_cmd);
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

   /* A second render pass after the recorded cell refuses: its load-op
    * clear has no lowering, so accepting it would record a pass that
    * never executes.
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
