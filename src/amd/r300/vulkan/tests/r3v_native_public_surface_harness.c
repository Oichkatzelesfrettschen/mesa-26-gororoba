/*
 * SPDX-License-Identifier: MIT
 *
 * Public-surface harness on the drm-shim fixture: an application-shaped
 * render-pass/pipeline/draw sequence records the qualified triangle
 * cell through public entry points alone, and every contract deviation
 * refuses.  The hazard gate stays closed, so each vkQueueSubmit refuses
 * before the ioctl and before the deferred vertex gather and load-op
 * clear; the harness verifies that a refused submit leaves the carrier
 * and the target untouched, and proves the execution-time boundary --
 * the stream re-read and the clear realized at execution -- by driving
 * the deferred executor directly.
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
#include "r3v_native_multi_pass_arms.h"
#include "r3v_native_shim_arming.h"

#include "util/mesa-blake3.h"

#include <sys/utsname.h>

#include "amd/r300/common/r300_compute_verb.h"
#include "amd/r300/common/r300_r2vb_public_route.h"

#include "vk_semaphore.h"

#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "util/u_math.h"

#include <assert.h>
#include <float.h>
#include <math.h>
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
   f(vkCmdClearAttachments) f(vkCmdSetViewport) f(vkCmdSetScissor)          \
   f(vkCmdBindPipeline) f(vkCmdBindVertexBuffers) f(vkCmdBindIndexBuffer) \
   f(vkCmdDraw) f(vkCmdDrawIndexed)                                      \
   f(vkCmdCopyBuffer) f(vkCmdCopyBufferToImage) f(vkCmdCopyImage)          \
   f(vkCmdCopyImageToBuffer) f(vkCmdFillBuffer)                            \
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

static VkCommandBuffer
record_triangle_draw(const VkRenderPassBeginInfo *begin_pass,
                     VkPipeline pipeline, VkBuffer vertex_buffer,
                     uint32_t vertex_count, uint32_t instance_count,
                     uint32_t first_vertex)
{
   VkCommandBuffer command_buffer = fresh_cmd();
   vkCmdBeginRenderPass(command_buffer, begin_pass,
                        VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     pipeline);
   vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(command_buffer, vertex_count, instance_count, first_vertex, 0);
   vkCmdEndRenderPass(command_buffer);
   assert(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
   return command_buffer;
}

static VkCommandBuffer
record_indexed_triangle_draw(const VkRenderPassBeginInfo *begin_pass,
                             VkPipeline pipeline, VkBuffer vertex_buffer,
                             VkBuffer index_buffer)
{
   VkCommandBuffer command_buffer = fresh_cmd();
   vkCmdBeginRenderPass(command_buffer, begin_pass,
                        VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     pipeline);
   vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdBindIndexBuffer(command_buffer, index_buffer, 0,
                        VK_INDEX_TYPE_UINT16);
   vkCmdDrawIndexed(command_buffer, 3, 1, 0, 0, 0);
   vkCmdEndRenderPass(command_buffer);
   assert(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
   return command_buffer;
}

static double
carrier_triangle_area(const float *records, uint32_t record_dwords,
                      uint32_t triangle)
{
   const float *first = &records[(triangle * 3u) * record_dwords];
   const float *second = &records[(triangle * 3u + 1u) * record_dwords];
   const float *third = &records[(triangle * 3u + 2u) * record_dwords];
   return ((double)second[0] - first[0]) *
             ((double)third[1] - first[1]) -
          ((double)third[0] - first[0]) *
             ((double)second[1] - first[1]);
}

static uint32_t
carrier_nondegenerate_triangle_count(const float *records,
                                     uint32_t record_dwords)
{
   uint32_t count = 0;
   for (uint32_t triangle = 0;
        triangle < R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT;
        triangle++) {
      count += carrier_triangle_area(records, record_dwords, triangle) != 0.0;
   }
   return count;
}

/* Counts the eight-dword records of every non-degenerate carrier
 * triangle whose varying differs from the four expected values: zero
 * under a Flat lowering, at least one when the varying interpolates
 * three distinct vertex values. */
static uint32_t
carrier_varying_mismatch_count(const float *records, const float expected[4])
{
   uint32_t mismatches = 0;
   for (uint32_t triangle = 0;
        triangle < R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT;
        triangle++) {
      if (carrier_triangle_area(records, 8, triangle) == 0.0)
         continue;
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         const float *varying = &records[(triangle * 3 + vertex) * 8 + 4];
         mismatches += memcmp(varying, expected, 16) != 0;
      }
   }
   return mismatches;
}

/* A module with every OpDecorate Flat removed: the interface reads
 * Smooth, so the pipeline it builds carries no lowering.  Returns the
 * word count written to out. */
static size_t
strip_flat_decorations(const uint32_t *words, size_t count, uint32_t *out)
{
   memcpy(out, words, count * 4);
   size_t at = 5;
   while (at < count) {
      const uint32_t len = out[at] >> 16;
      if ((out[at] & 0xffffu) == 71 && len == 3 && out[at + 2] == 14) {
         memmove(&out[at], &out[at + len], (count - at - len) * 4);
         count -= len;
         continue;
      }
      at += len;
   }
   return count;
}

static void
assert_float_near(float actual, float expected)
{
   assert(fabsf(actual - expected) <= 1.0e-5f);
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
   /* Attach a depth/stencil state: DISABLED passes the all-disabled
    * struct, ENABLED turns the depth test on (the refusal leg). */
   int depth_stencil; /* 0 none, 1 disabled, 2 enabled */
   VkFormat attribute_format;
   uint32_t stride;
   VkBool32 blend_enable;
   /* With blend_enable, select the identity configuration (ONE, ZERO,
    * ADD) instead of the zero-initialized factors. */
   VkBool32 blend_identity;
   /* Zero the color write mask (the no-channel shape). */
   VkBool32 write_mask_zero;
   /* Enable the logic op; the op itself. */
   VkBool32 logic_op_enable;
   VkLogicOp logic_op;
   VkCullModeFlags cull_mode;
   VkFrontFace front_face;
   /* Zero selects the position pass-through vertex module. */
   const uint32_t *vertex_words;
   size_t vertex_bytes;
   const uint32_t *fragment_words;
   size_t fragment_bytes;
   /* Viewport/scissor extent; zero selects the maximum target extent. */
   uint32_t extent_width;
   uint32_t extent_height;
   /* Declare viewport and scissor dynamic; the vkCmdSet values then
    * carry the extent. */
   VkBool32 dynamic_viewport_scissor;
};

static VkResult
make_pipeline(const struct pipeline_shape *shape, VkRenderPass pass,
              VkPipelineLayout layout, VkPipeline *pipeline)
{
   const uint32_t *vertex_words = shape->vertex_words != NULL
                                     ? shape->vertex_words
                                     : r3v_reference_vertex_spirv;
   const size_t vertex_bytes = shape->vertex_words != NULL
                                  ? shape->vertex_bytes
                                  : sizeof(r3v_reference_vertex_spirv);
   VkShaderModule vs = make_module(vertex_words, vertex_bytes);
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
            .cullMode = shape->cull_mode,
            .frontFace = shape->front_face,
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
            .logicOpEnable = shape->logic_op_enable,
            .logicOp = shape->logic_op,
            .pAttachments =
               &(VkPipelineColorBlendAttachmentState){
                  .blendEnable = shape->blend_enable,
                  .srcColorBlendFactor = shape->blend_identity
                                            ? VK_BLEND_FACTOR_ONE
                                            : VK_BLEND_FACTOR_ZERO,
                  .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                  .colorBlendOp = VK_BLEND_OP_ADD,
                  .srcAlphaBlendFactor = shape->blend_identity
                                            ? VK_BLEND_FACTOR_ONE
                                            : VK_BLEND_FACTOR_ZERO,
                  .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                  .alphaBlendOp = VK_BLEND_OP_ADD,
                  .colorWriteMask =
                     shape->write_mask_zero
                        ? 0
                        : VK_COLOR_COMPONENT_R_BIT |
                             VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT,
               },
         },
      .pDepthStencilState =
         shape->depth_stencil != 0
            ? &(VkPipelineDepthStencilStateCreateInfo){
                 .sType =
                    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                 .depthTestEnable =
                    shape->depth_stencil == 2 ? VK_TRUE : VK_FALSE,
                 .depthCompareOp = VK_COMPARE_OP_LESS,
              }
            : NULL,
      .pDynamicState =
         shape->dynamic_viewport_scissor
            ? &(VkPipelineDynamicStateCreateInfo){
                 .sType =
                    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                 .dynamicStateCount = 2,
                 .pDynamicStates =
                    (VkDynamicState[]){ VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR },
              }
            : NULL,
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
 * complete image-family matrix through both dispatch paths.  The
 * sampling family takes the two transfer bits and the sampled bit; the
 * render family takes the color-attachment bit with the sampled and
 * transfer bits beside it, and zero or a mask naming any other usage
 * refuses at both boundaries.
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
        VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_SAMPLED_BIT, VK_SUCCESS, VK_SUCCESS },
      { VK_IMAGE_USAGE_STORAGE_BIT, VK_ERROR_FORMAT_NOT_SUPPORTED,
        R3V_NATIVE_REFUSAL_RESULT },
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

   /* The gates stay closed by construction: recording is submit-free,
    * this harness never opens the hazard environment, and the delivery
    * legs below open and close the R2VB gates around their own
    * assertions, so an ambient value cannot reroute a leg.
    */
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL");
   unsetenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");

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

   /* The one queue family is the shape the inventory pins: GRAPHICS
    * base, COMPUTE as the verb ledger claims it under the exact opt-in,
    * queueCount 1, timestampValidBits 0.
    */
   PFN_vkGetPhysicalDeviceQueueFamilyProperties query_queue_families =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(
         instance, "vkGetPhysicalDeviceQueueFamilyProperties");
   assert(query_queue_families != NULL);
   uint32_t family_count = 0;
   query_queue_families(pdev, &family_count, NULL);
   assert(family_count == 1);
   VkQueueFamilyProperties family;
   query_queue_families(pdev, &family_count, &family);
   const char *gate = getenv(R300_COMPUTE_QUEUE_CLAIM_GATE);
   const VkQueueFlags expected_flags =
      VK_QUEUE_GRAPHICS_BIT |
      (r300_compute_verb_queue_claim(
          gate != NULL &&
          strcmp(gate, R300_COMPUTE_QUEUE_CLAIM_GATE_VALUE) == 0)
          ? VK_QUEUE_COMPUTE_BIT
          : 0);
   assert(family.queueFlags == expected_flags);
   assert(family.queueCount == 1);
   assert(family.timestampValidBits == 0);

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

   /* The GPU producer route selected by the exact double opt-in names a
    * delivery this deferred draw cannot execute, so it refuses by name
    * instead of downgrading to a host copy; closing the gates restores
    * the CPU route on the same recording.
    */
   assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
   assert(setenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
   r3v_native_device_refresh_delivery_gates(constant_device);
   assert(r3v_native_cmd_buffer_execute_deferred_draws(
             constant_device, native_constant) ==
          VK_ERROR_INITIALIZATION_FAILED);
   assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
   assert(unsetenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL") == 0);
   r3v_native_device_refresh_delivery_gates(constant_device);

   assert(r3v_native_cmd_buffer_execute_deferred_draws(
             constant_device, native_constant) == VK_SUCCESS);
   void *constant_carrier_map = NULL;
   assert(radeon_drm_vk_bo_map(&constant_device->drm,
                               &native_constant->owned_carriers[0]->bo,
                               &constant_carrier_map) == 0);
   static const float expected_constant[4] = { 8.0f, 8.0f, 0.0f, 1.0f };
   for (unsigned vertex = 0; vertex < 3; vertex++)
      assert(memcmp((const uint8_t *)constant_carrier_map + vertex * 16,
                    expected_constant, sizeof(expected_constant)) == 0);
   radeon_drm_vk_bo_unmap(&constant_device->drm,
                          &native_constant->owned_carriers[0]->bo,
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
   assert(native_empty->deferred_draws[0].pending);
   assert(native_empty->deferred_draws[0].stream_mask == 0);
   assert(native_empty->owned_carriers[0] == NULL);
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
   assert(r3v_native_queue_submission_status(device) ==
          R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION);
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

   /* The executing boundary for the multi-slice view types.  An array
    * view creates and destroys -- the object_management cases that name
    * it never sample through it -- while the color backend places one
    * slice's base in RB3D_COLOROFFSET0, so binding that view as an
    * attachment refuses at the pass and the command buffer never
    * reaches EXECUTABLE.  Admission without this refusal would render a
    * capability the TX program and the color backend do not carry.
    */
   {
      VkImageView array_view = VK_NULL_HANDLE;
      assert(vkCreateImageView(
                device,
                &(VkImageViewCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                   .image = image,
                   .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                   .format = R3V_NATIVE_TARGET_FORMAT,
                   .subresourceRange = { .aspectMask =
                                            VK_IMAGE_ASPECT_COLOR_BIT,
                                         .levelCount = 1,
                                         .layerCount = 1 },
                },
                NULL, &array_view) == VK_SUCCESS &&
             array_view != VK_NULL_HANDLE);

      VkFramebuffer array_framebuffer = VK_NULL_HANDLE;
      assert(vkCreateFramebuffer(
                device,
                &(VkFramebufferCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                   .renderPass = pass,
                   .attachmentCount = 1,
                   .pAttachments = &array_view,
                   .width = R3V_NATIVE_TARGET_WIDTH,
                   .height = R3V_NATIVE_TARGET_HEIGHT,
                   .layers = 1,
                },
                NULL, &array_framebuffer) == VK_SUCCESS);

      VkCommandBuffer array_cmd = fresh_cmd();
      VkRenderPassBeginInfo array_begin = begin_pass;
      array_begin.framebuffer = array_framebuffer;
      vkCmdBeginRenderPass(array_cmd, &array_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      assert(vkEndCommandBuffer(array_cmd) == R3V_NATIVE_REFUSAL_RESULT);

      vkDestroyFramebuffer(device, array_framebuffer, NULL);
      vkDestroyImageView(device, array_view, NULL);
   }

   /* Two render passes in one command buffer.  Each carries its own
    * load-op clear, its own carrier, and its own vertex execution, so
    * the second executes exactly what the first does over its own
    * state, and the queue runs them in record order.  The clear-only
    * shape takes the zero-IB path, which the closed submission gate
    * admits, so both targets carry their own clear after one submit.
    */
   {
      VkImage second_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &image_info, NULL, &second_image) ==
             VK_SUCCESS);
      VkMemoryRequirements second_reqs;
      vkGetImageMemoryRequirements(device, second_image, &second_reqs);
      VkDeviceMemory second_memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(
                device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = second_reqs.size,
                   .memoryTypeIndex = 0,
                },
                NULL, &second_memory) == VK_SUCCESS);
      assert(vkBindImageMemory(device, second_image, second_memory, 0) ==
             VK_SUCCESS);
      VkImageView second_view = VK_NULL_HANDLE;
      assert(vkCreateImageView(
                device,
                &(VkImageViewCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                   .image = second_image,
                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                   .format = R3V_NATIVE_TARGET_FORMAT,
                   .subresourceRange = { .aspectMask =
                                            VK_IMAGE_ASPECT_COLOR_BIT,
                                         .levelCount = 1,
                                         .layerCount = 1 },
                },
                NULL, &second_view) == VK_SUCCESS);
      VkFramebuffer second_framebuffer = VK_NULL_HANDLE;
      assert(vkCreateFramebuffer(
                device,
                &(VkFramebufferCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                   .renderPass = pass,
                   .attachmentCount = 1,
                   .pAttachments = &second_view,
                   .width = R3V_NATIVE_TARGET_WIDTH,
                   .height = R3V_NATIVE_TARGET_HEIGHT,
                   .layers = 1,
                },
                NULL, &second_framebuffer) == VK_SUCCESS);

      /* Distinct clear colors, so each target names the pass that wrote
       * it.  B8G8R8A8 stores the bytes B, G, R, A, so (1, 0, 0, 1)
       * reads 0xffff0000 as a little-endian dword and (0, 0, 1, 1)
       * reads 0xff0000ff.
       */
      VkRenderPassBeginInfo first_begin = begin_pass;
      first_begin.pClearValues = &(VkClearValue){
         .color = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } },
      };
      VkRenderPassBeginInfo second_begin = begin_pass;
      second_begin.framebuffer = second_framebuffer;
      second_begin.pClearValues = &(VkClearValue){
         .color = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } },
      };

      uint32_t *seed = NULL;
      assert(vkMapMemory(device, second_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&seed) == VK_SUCCESS);
      for (unsigned i = 0; i < R3V_NATIVE_TARGET_MEMORY_BYTES / 4; i++)
         seed[i] = COLOR_SEED;
      vkUnmapMemory(device, second_memory);
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&seed) == VK_SUCCESS);
      for (unsigned i = 0; i < R3V_NATIVE_TARGET_MEMORY_BYTES / 4; i++)
         seed[i] = COLOR_SEED;
      vkUnmapMemory(device, color_memory);

      VkCommandBuffer two_pass_cmd = fresh_cmd();
      vkCmdBeginRenderPass(two_pass_cmd, &first_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(two_pass_cmd);
      vkCmdBeginRenderPass(two_pass_cmd, &second_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(two_pass_cmd);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_two_pass, two_pass_cmd);
      assert(native_two_pass->deferred_draw_count == 2);
      assert(native_two_pass->deferred_draws[0].pending &&
             native_two_pass->deferred_draws[1].pending);
      /* Neither pass records a draw, so the buffer carries no cell and
       * takes the zero-IB path.
       */
      assert(native_two_pass->ib_size_dwords == 0);
      assert(vkEndCommandBuffer(two_pass_cmd) == VK_SUCCESS);
      assert(vkQueueSubmit(
                queue, 1,
                &(VkSubmitInfo){
                   .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                   .commandBufferCount = 1,
                   .pCommandBuffers = &two_pass_cmd,
                },
                VK_NULL_HANDLE) == VK_SUCCESS);

      uint32_t *first_map = NULL;
      uint32_t *second_map = NULL;
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&first_map) == VK_SUCCESS);
      assert(vkMapMemory(device, second_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&second_map) == VK_SUCCESS);
      assert(first_map[0] == 0xffff0000u);
      assert(first_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] ==
             0xffff0000u);
      assert(second_map[0] == 0xff0000ffu);
      assert(second_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] ==
             0xff0000ffu);
      vkUnmapMemory(device, color_memory);
      vkUnmapMemory(device, second_memory);

      /* Two-pass state isolation: the first pass draws under the Flat
       * pipeline and the second under the Smooth varying pipeline over
       * one vertex buffer whose tint is the position, so the first
       * carrier replicates vertex 0's position into every record while
       * the second keeps each vertex's own, and the lowering of one
       * pass leaves the other's records untouched. */
      {
         struct pipeline_shape flat_pass_shape = {
            .attribute_format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .stride = 16,
            .blend_enable = VK_FALSE,
            .vertex_words = r3v_reference_vertex_flat_spirv,
            .vertex_bytes = sizeof(r3v_reference_vertex_flat_spirv),
            .fragment_words = r3v_reference_fragment_flat_spirv,
            .fragment_bytes = sizeof(r3v_reference_fragment_flat_spirv),
         };
         struct pipeline_shape smooth_pass_shape = flat_pass_shape;
         smooth_pass_shape.vertex_words = r3v_reference_vertex_varying_spirv;
         smooth_pass_shape.vertex_bytes =
            sizeof(r3v_reference_vertex_varying_spirv);
         smooth_pass_shape.fragment_words =
            r3v_reference_fragment_varying_spirv;
         smooth_pass_shape.fragment_bytes =
            sizeof(r3v_reference_fragment_varying_spirv);
         VkPipeline flat_pass = VK_NULL_HANDLE, smooth_pass = VK_NULL_HANDLE;
         assert(make_pipeline(&flat_pass_shape, pass, layout, &flat_pass) ==
                VK_SUCCESS);
         assert(make_pipeline(&smooth_pass_shape, pass, layout,
                              &smooth_pass) == VK_SUCCESS);
         VkCommandBuffer isolation_cmd = fresh_cmd();
         vkCmdBeginRenderPass(isolation_cmd, &first_begin,
                              VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(isolation_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           flat_pass);
         vkCmdBindVertexBuffers(isolation_cmd, 0, 1, &vertex_buffer,
                                &(VkDeviceSize){ 0 });
         vkCmdDraw(isolation_cmd, 3, 1, 0, 0);
         vkCmdEndRenderPass(isolation_cmd);
         vkCmdBeginRenderPass(isolation_cmd, &second_begin,
                              VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(isolation_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           smooth_pass);
         vkCmdBindVertexBuffers(isolation_cmd, 0, 1, &vertex_buffer,
                                &(VkDeviceSize){ 0 });
         vkCmdDraw(isolation_cmd, 3, 1, 0, 0);
         vkCmdEndRenderPass(isolation_cmd);
         assert(vkEndCommandBuffer(isolation_cmd) == VK_SUCCESS);
         VK_FROM_HANDLE(r3v_native_cmd_buffer, native_isolation,
                        isolation_cmd);
         VK_FROM_HANDLE(r3v_native_device, native_device, device);
         assert(native_isolation->deferred_draw_count == 2);
         assert(native_isolation->deferred_draws[0].post_vs.flat_mask == 1 &&
                native_isolation->deferred_draws[1].post_vs.flat_mask == 0);
         assert(r3v_native_cmd_buffer_execute_deferred_draws(
                   native_device, native_isolation) == VK_SUCCESS);
         void *pass_map = NULL;
         assert(radeon_drm_vk_bo_map(
                   &native_device->drm,
                   &native_isolation->owned_carriers[0]->bo, &pass_map) == 0);
         const float *flat_pass_records = pass_map;
         assert(carrier_nondegenerate_triangle_count(flat_pass_records, 8) ==
                1);
         assert(carrier_varying_mismatch_count(flat_pass_records,
                                               &ndc_triangle[0]) == 0);
         radeon_drm_vk_bo_unmap(&native_device->drm,
                                &native_isolation->owned_carriers[0]->bo,
                                pass_map);
         assert(radeon_drm_vk_bo_map(
                   &native_device->drm,
                   &native_isolation->owned_carriers[1]->bo, &pass_map) == 0);
         const float *smooth_pass_records = pass_map;
         assert(carrier_nondegenerate_triangle_count(smooth_pass_records,
                                                     8) == 1);
         /* The smooth reference vertex module writes
          * fma(position, (0.5, 0.5, 0, 0), (0.5, 0.5, 0.25, 1)), so each
          * record keeps its own vertex's tint. */
         for (uint32_t vertex = 0; vertex < 3; vertex++) {
            const float own_tint[4] = {
               ndc_triangle[vertex * 4 + 0] * 0.5f + 0.5f,
               ndc_triangle[vertex * 4 + 1] * 0.5f + 0.5f, 0.25f, 1.0f,
            };
            for (uint32_t c = 0; c < 4; c++)
               assert_float_near(smooth_pass_records[vertex * 8 + 4 + c],
                                 own_tint[c]);
         }
         radeon_drm_vk_bo_unmap(&native_device->drm,
                                &native_isolation->owned_carriers[1]->bo,
                                pass_map);
         vkDestroyPipeline(device, flat_pass, NULL);
         vkDestroyPipeline(device, smooth_pass, NULL);
      }

      /* A third pass has no deferred record to fill and refuses. */
      VkCommandBuffer three_pass_cmd = fresh_cmd();
      vkCmdBeginRenderPass(three_pass_cmd, &first_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(three_pass_cmd);
      vkCmdBeginRenderPass(three_pass_cmd, &second_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(three_pass_cmd);
      vkCmdBeginRenderPass(three_pass_cmd, &first_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      assert(vkEndCommandBuffer(three_pass_cmd) ==
             R3V_NATIVE_REFUSAL_RESULT);

      /* Two passes that each record a draw carry two cells, so the
       * second appends to the installed stream: the concatenation keeps
       * both lengths, merges the four buffer references by handle, and
       * binds the appended payloads to the merged indices.  Each half
       * opens with its own first-draw contract and closes with the
       * destination-cache flush, so no state crosses the boundary.
       */
      VkCommandBuffer two_draw_cmd = fresh_cmd();
      vkCmdBeginRenderPass(two_draw_cmd, &first_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(two_draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline);
      vkCmdBindVertexBuffers(two_draw_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(two_draw_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(two_draw_cmd);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_two_draw, two_draw_cmd);
      const uint32_t one_cell_dwords = native_two_draw->ib_size_dwords;
      assert(one_cell_dwords != 0);

      /* The second pass binds a pipeline over the blue fragment module,
       * so the two passes carry distinct constants and the stream is
       * the public two-draw arm's.
       */
      VkPipeline blue_pipeline = VK_NULL_HANDLE;
      const struct pipeline_shape blue_shape = {
         .attribute_format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .stride = 16,
         .blend_enable = VK_FALSE,
         .fragment_words = r3v_reference_fragment_blue_spirv,
         .fragment_bytes = sizeof(r3v_reference_fragment_blue_spirv),
      };
      assert(make_pipeline(&blue_shape, pass, layout, &blue_pipeline) ==
                VK_SUCCESS &&
             blue_pipeline != VK_NULL_HANDLE);
      vkCmdBeginRenderPass(two_draw_cmd, &second_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(two_draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        blue_pipeline);
      vkCmdBindVertexBuffers(two_draw_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(two_draw_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(two_draw_cmd);
      assert(vkEndCommandBuffer(two_draw_cmd) == VK_SUCCESS);
      vkDestroyPipeline(device, blue_pipeline, NULL);

      assert(native_two_draw->deferred_draw_count == 2);
      assert(native_two_draw->deferred_draws[0].stream_mask != 0 &&
             native_two_draw->deferred_draws[1].stream_mask != 0);
      assert(native_two_draw->owned_carriers[0] != NULL &&
             native_two_draw->owned_carriers[1] != NULL &&
             native_two_draw->owned_carriers[0] !=
                native_two_draw->owned_carriers[1]);
      assert(native_two_draw->cell_kind ==
             R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS);
      assert(native_two_draw->ib_size_dwords == 2 * one_cell_dwords);
      /* Four distinct buffer objects: a carrier and a target per pass. */
      assert(native_two_draw->reference_count == 4);
      for (uint32_t a = 0; a < native_two_draw->reference_count; a++) {
         for (uint32_t b = a + 1; b < native_two_draw->reference_count; b++)
            assert(native_two_draw->references[a].handle !=
                   native_two_draw->references[b].handle);
      }

      /* The offline two-pass emitter reproduces the recorded stream:
       * both passes are the reference shape, the first with the
       * reference fragment module's green and the second with the blue
       * module's constant, the second bound to its own carrier (index
       * 2) and target (index 3), so the digest the arming gate compares
       * against a public two-draw submission is computable with no
       * recording, and r3v_native_multi_pass_public_reference is that
       * computation.
       */
      {
         struct r300_triangle_multi_pass mp;
         r3v_native_multi_pass_public_reference(&mp);
         struct r300_tcl_bypass_triangle_ib offline;
         assert(r300_tcl_bypass_triangle_clip_space_multi_pass_emit(
                   &mp, &offline) == 0);
         assert(offline.ib_size_dwords == native_two_draw->ib_size_dwords);
         uint32_t offline_differing = 0;
         for (uint32_t i = 0; i < offline.ib_size_dwords; i++) {
            if (offline.ib[i] != native_two_draw->ib[i]) {
               if (offline_differing < 8)
                  fprintf(stderr,
                          "two-draw dword %u: recorded 0x%08x offline 0x%08x\n",
                          i, native_two_draw->ib[i], offline.ib[i]);
               offline_differing++;
            }
         }
         assert(offline_differing == 0);
         r300_tcl_bypass_triangle_release(&offline);
      }

      /* The public form reaches the arming gate with both deferred
       * draws pending, and the multi-pass predicate freezes that form,
       * so an armed submission under the emitter's digest admits on the
       * shim: the two load-op clears realize on the host and the noop
       * command stream completes.  The gate reopens only for this
       * submission; every fixture that follows runs closed.
       */
      {
         struct r300_triangle_multi_pass armed_mp;
         r3v_native_multi_pass_public_reference(&armed_mp);
         struct r300_tcl_bypass_triangle_ib armed_cell;
         assert(r300_tcl_bypass_triangle_clip_space_multi_pass_emit(
                   &armed_mp, &armed_cell) == 0);
         char armed_digest[BLAKE3_OUT_LEN * 2 + 1];
         r300_triangle_ib_digest_hex(armed_cell.ib, armed_cell.ib_size_dwords,
                                     armed_digest);
         r300_tcl_bypass_triangle_release(&armed_cell);
         char manifest_template[] = "/tmp/r3v-public-two-draw-XXXXXX";
         const char *manifest_dir = mkdtemp(manifest_template);
         assert(manifest_dir != NULL);
         struct utsname host;
         assert(uname(&host) == 0);
         setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
         setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
         setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", armed_digest, 1);
         setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
         setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
                R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);
         /* The device captured its evidence directory and its hazard
          * gate at creation, when the environment named neither, so the
          * armed submission binds both on the device itself for this
          * one call; the environment stays closed throughout.
          */
         r3v_native_device_from_handle(device)->manifest_dir = manifest_dir;
         r3v_native_device_from_handle(device)->submit_hazard_accepted = true;
         r3v_native_device_from_handle(device)->arming_provider =
            &r3v_native_shim_arming_provider;
         const VkResult armed_result = vkQueueSubmit(
            queue, 1,
            &(VkSubmitInfo){
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
               .commandBufferCount = 1,
               .pCommandBuffers = &two_draw_cmd,
            },
            VK_NULL_HANDLE);
         assert(armed_result == VK_SUCCESS);
         unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
         unsetenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3");
         unsetenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE");
         unsetenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION");
         unsetenv("R3V_NATIVE_MANIFEST_DIR");
         r3v_native_device_from_handle(device)->arming_provider = NULL;
         r3v_native_device_from_handle(device)->manifest_dir = NULL;
         r3v_native_device_from_handle(device)->submit_hazard_accepted = false;

         /* Each pass's load-op clear reached its own target on the host;
          * the noop stream wrote nothing.
          */
         assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                            (void **)&first_map) == VK_SUCCESS);
         assert(vkMapMemory(device, second_memory, 0, VK_WHOLE_SIZE, 0,
                            (void **)&second_map) == VK_SUCCESS);
         assert(first_map[0] == 0xffff0000u &&
                first_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] ==
                   0xffff0000u);
         assert(second_map[0] == 0xff0000ffu &&
                second_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] ==
                   0xff0000ffu);
         vkUnmapMemory(device, color_memory);
         vkUnmapMemory(device, second_memory);
      }

      /* The Flat two-draw form armed on the shim: one pipeline over the
       * saturated Flat pair, each pass drawing the reference triangle
       * in its own vertex order from one six-record buffer (B, C, A,
       * then C, A, B), so the provoking vertex differs between the
       * passes while the stream bytes are the varying two-pass
       * emitter's, r3v_native_multi_pass_public_flat_reference.  After
       * the submission each pass's own carrier holds its provoking
       * vertex's doubled position in every record, and neither
       * carrier holds the other pass's.  This is the gather the
       * attended runner's record-only calibration stops ahead of.
       */
      {
         struct pipeline_shape flat_two_draw_shape = {
            .attribute_format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .stride = 16,
            .blend_enable = VK_FALSE,
            .vertex_words = r3v_reference_vertex_flat_saturated_spirv,
            .vertex_bytes = sizeof(r3v_reference_vertex_flat_saturated_spirv),
            .fragment_words = r3v_reference_fragment_flat_spirv,
            .fragment_bytes = sizeof(r3v_reference_fragment_flat_spirv),
         };
         VkPipeline flat_two_draw = VK_NULL_HANDLE;
         assert(make_pipeline(&flat_two_draw_shape, pass, layout,
                              &flat_two_draw) == VK_SUCCESS);
         static const uint32_t order[2][3] = { { 1, 2, 0 }, { 2, 0, 1 } };
         float *stream = NULL;
         assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                            (void **)&stream) == VK_SUCCESS);
         for (unsigned p = 0; p < 2; p++)
            for (unsigned v = 0; v < 3; v++)
               memcpy(&stream[(p * 3 + v) * 4], &ndc_triangle[order[p][v] * 4],
                      16);
         vkUnmapMemory(device, vertex_memory);

         VkCommandBuffer flat_cmd = fresh_cmd();
         vkCmdBeginRenderPass(flat_cmd, &first_begin,
                              VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(flat_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           flat_two_draw);
         vkCmdBindVertexBuffers(flat_cmd, 0, 1, &vertex_buffer,
                                &(VkDeviceSize){ 0 });
         vkCmdDraw(flat_cmd, 3, 1, 0, 0);
         vkCmdEndRenderPass(flat_cmd);
         vkCmdBeginRenderPass(flat_cmd, &second_begin,
                              VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(flat_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           flat_two_draw);
         vkCmdBindVertexBuffers(flat_cmd, 0, 1, &vertex_buffer,
                                &(VkDeviceSize){ 0 });
         vkCmdDraw(flat_cmd, 3, 1, 3, 0);
         vkCmdEndRenderPass(flat_cmd);
         assert(vkEndCommandBuffer(flat_cmd) == VK_SUCCESS);
         VK_FROM_HANDLE(r3v_native_cmd_buffer, native_flat, flat_cmd);
         assert(native_flat->cell_kind ==
                R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS);
         assert(native_flat->deferred_draw_count == 2 &&
                native_flat->deferred_draws[0].post_vs.flat_mask == 1u &&
                native_flat->deferred_draws[1].post_vs.flat_mask == 1u &&
                native_flat->deferred_draws[1].first_vertex == 3);

         struct r300_triangle_multi_pass flat_mp;
         r3v_native_multi_pass_public_flat_reference(&flat_mp);
         struct r300_tcl_bypass_triangle_ib flat_cell;
         assert(r300_tcl_bypass_triangle_clip_space_multi_pass_emit(
                   &flat_mp, &flat_cell) == 0);
         assert(flat_cell.ib_size_dwords == native_flat->ib_size_dwords);
         assert(memcmp(flat_cell.ib, native_flat->ib,
                       flat_cell.ib_size_dwords * sizeof(uint32_t)) == 0);
         char flat_digest[BLAKE3_OUT_LEN * 2 + 1];
         r300_triangle_ib_digest_hex(flat_cell.ib, flat_cell.ib_size_dwords,
                                     flat_digest);
         r300_tcl_bypass_triangle_release(&flat_cell);

         char flat_manifest_template[] = "/tmp/r3v-public-flat-two-draw-XXXXXX";
         const char *flat_manifest_dir = mkdtemp(flat_manifest_template);
         assert(flat_manifest_dir != NULL);
         struct utsname flat_host;
         assert(uname(&flat_host) == 0);
         setenv("R3V_NATIVE_MANIFEST_DIR", flat_manifest_dir, 1);
         setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
         setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", flat_digest, 1);
         setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", flat_host.release, 1);
         setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
                R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);
         r3v_native_device_from_handle(device)->manifest_dir =
            flat_manifest_dir;
         r3v_native_device_from_handle(device)->submit_hazard_accepted = true;
         r3v_native_device_from_handle(device)->arming_provider =
            &r3v_native_shim_arming_provider;
         const VkResult flat_result = vkQueueSubmit(
            queue, 1,
            &(VkSubmitInfo){
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
               .commandBufferCount = 1,
               .pCommandBuffers = &flat_cmd,
            },
            VK_NULL_HANDLE);
         assert(flat_result == VK_SUCCESS);
         unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
         unsetenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3");
         unsetenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE");
         unsetenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION");
         unsetenv("R3V_NATIVE_MANIFEST_DIR");
         r3v_native_device_from_handle(device)->arming_provider = NULL;
         r3v_native_device_from_handle(device)->manifest_dir = NULL;
         r3v_native_device_from_handle(device)->submit_hazard_accepted = false;

         /* The saturated module writes fma(position, 2, 0), so each
          * pass's carrier replicates its provoking vertex's doubled
          * position: B doubled in the first, C doubled in the second.
          */
         VK_FROM_HANDLE(r3v_native_device, native_device, device);
         for (unsigned p = 0; p < 2; p++) {
            const float *own = &ndc_triangle[order[p][0] * 4];
            const float own_tint[4] = { own[0] * 2.0f, own[1] * 2.0f,
                                        own[2] * 2.0f, own[3] * 2.0f };
            const float *other = &ndc_triangle[order[1 - p][0] * 4];
            const float other_tint[4] = { other[0] * 2.0f, other[1] * 2.0f,
                                          other[2] * 2.0f, other[3] * 2.0f };
            void *carrier_map = NULL;
            assert(radeon_drm_vk_bo_map(
                      &native_device->drm,
                      &native_flat->owned_carriers[p]->bo, &carrier_map) == 0);
            const float *records = carrier_map;
            assert(carrier_nondegenerate_triangle_count(records, 8) == 1);
            assert(carrier_varying_mismatch_count(records, own_tint) == 0);
            assert(carrier_varying_mismatch_count(records, other_tint) == 3);
            radeon_drm_vk_bo_unmap(&native_device->drm,
                                   &native_flat->owned_carriers[p]->bo,
                                   carrier_map);
         }
         vkDestroyPipeline(device, flat_two_draw, NULL);

         /* Restore the three-record payload the following fixtures read. */
         assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                            (void **)&stream) == VK_SUCCESS);
         memcpy(stream, ndc_triangle, sizeof(ndc_triangle));
         vkUnmapMemory(device, vertex_memory);
      }

      vkDestroyFramebuffer(device, second_framebuffer, NULL);
      vkDestroyImageView(device, second_view, NULL);
      vkDestroyImage(device, second_image, NULL);
      vkFreeMemory(device, second_memory, NULL);

      /* Restore the seed the following fixtures expect. */
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&first_map) == VK_SUCCESS);
      for (unsigned i = 0;
           i < (R3V_NATIVE_TARGET_MEMORY_BYTES + 4096) / 4; i++)
         first_map[i] = COLOR_SEED;
      vkUnmapMemory(device, color_memory);
   }

   /* The load-op clear realizes the pass's own VkClearColorValue: the
    * attachment's format is UNORM, so the live member is float32 and
    * the texel is that quadruple through the color buffer's UNORM8
    * conversion under the image's lane order.  (0.25, 0.5, 0.75, 1)
    * rounds to bytes 64, 128, 191, 255, which B8G8R8A8 stores as
    * 0xff4080bf.
    */
   {
      VkCommandBuffer color_clear_cmd = fresh_cmd();
      VkRenderPassBeginInfo color_begin = begin_pass;
      color_begin.pClearValues = &(VkClearValue){
         .color = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } },
      };
      vkCmdBeginRenderPass(color_clear_cmd, &color_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(color_clear_cmd);
      assert(vkEndCommandBuffer(color_clear_cmd) == VK_SUCCESS);
      assert(vkQueueSubmit(
                queue, 1,
                &(VkSubmitInfo){
                   .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                   .commandBufferCount = 1,
                   .pCommandBuffers = &color_clear_cmd,
                },
                VK_NULL_HANDLE) == VK_SUCCESS);
      uint32_t *color_clear_map = NULL;
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&color_clear_map) == VK_SUCCESS);
      assert(color_clear_map[0] == 0xff4080bfu);
      assert(color_clear_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] ==
             0xff4080bfu);
      /* The fill stops at the image's declared footprint. */
      assert(color_clear_map[R3V_NATIVE_TARGET_MEMORY_BYTES / 4] ==
             COLOR_SEED);
      for (unsigned i = 0; i < (R3V_NATIVE_TARGET_MEMORY_BYTES + 4096) / 4;
           i++)
         color_clear_map[i] = COLOR_SEED;
      vkUnmapMemory(device, color_memory);
   }

   /* In-pass attachment clears over a draw-less pass: each rectangle
    * lands after the load-op clear on the zero-IB path, exact bytes at
    * the rect corners and the sentinel outside; a rect past the render
    * area refuses; a draw after a recorded clear refuses (the device
    * draw executes after every host write); a clear outside a pass
    * refuses.
    */
   {
      VkCommandBuffer clear_cmd = fresh_cmd();
      vkCmdBeginRenderPass(clear_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      const VkClearAttachment red = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .colorAttachment = 0,
         .clearValue = { .color = { .float32 = { 1.0f, 0.0f, 0.0f,
                                                 1.0f } } },
      };
      const VkClearRect rect = {
         .rect = { .offset = { 8, 8 }, .extent = { 16, 16 } },
         .layerCount = 1,
      };
      vkCmdClearAttachments(clear_cmd, 1, &red, 1, &rect);
      vkCmdEndRenderPass(clear_cmd);
      assert(vkEndCommandBuffer(clear_cmd) == VK_SUCCESS);
      assert(vkQueueSubmit(
                queue, 1,
                &(VkSubmitInfo){
                   .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                   .commandBufferCount = 1,
                   .pCommandBuffers = &clear_cmd,
                },
                VK_NULL_HANDLE) == VK_SUCCESS);
      uint32_t *clear_map = NULL;
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&clear_map) == VK_SUCCESS);
      const uint32_t row_words = R3V_NATIVE_TARGET_ROW_BYTES / 4;
      /* B8G8R8A8 red = 0x00, 0x00, 0xff, 0xff bytes = LE 0xffff0000. */
      assert(clear_map[8 * row_words + 8] == 0xffff0000u);
      assert(clear_map[23 * row_words + 23] == 0xffff0000u);
      assert(clear_map[7 * row_words + 8] == R300_TRIANGLE_COLOR_SENTINEL);
      assert(clear_map[24 * row_words + 24] ==
             R300_TRIANGLE_COLOR_SENTINEL);
      for (unsigned i = 0; i < (R3V_NATIVE_TARGET_MEMORY_BYTES + 4096) / 4;
           i++)
         clear_map[i] = COLOR_SEED;
      vkUnmapMemory(device, color_memory);

      VkCommandBuffer bad_rect_cmd = fresh_cmd();
      vkCmdBeginRenderPass(bad_rect_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      const VkClearRect oversize_rect = {
         .rect = { .offset = { 60, 0 }, .extent = { 8, 8 } },
         .layerCount = 1,
      };
      vkCmdClearAttachments(bad_rect_cmd, 1, &red, 1, &oversize_rect);
      assert(vkEndCommandBuffer(bad_rect_cmd) ==
             R3V_NATIVE_REFUSAL_RESULT);

      VkCommandBuffer clear_draw_cmd = fresh_cmd();
      vkCmdBeginRenderPass(clear_draw_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdClearAttachments(clear_draw_cmd, 1, &red, 1, &rect);
      vkCmdBindPipeline(clear_draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline);
      vkCmdBindVertexBuffers(clear_draw_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(clear_draw_cmd, 3, 1, 0, 0);
      assert(vkEndCommandBuffer(clear_draw_cmd) ==
             R3V_NATIVE_REFUSAL_RESULT);

      VkCommandBuffer outside_cmd = fresh_cmd();
      vkCmdClearAttachments(outside_cmd, 1, &red, 1, &rect);
      assert(vkEndCommandBuffer(outside_cmd) == R3V_NATIVE_REFUSAL_RESULT);
   }

   /* Blend, logic op, and write mask: the identity blend (ONE, ZERO,
    * ADD) and the COPY logic op equal the straight write and admit; a
    * zero write mask admits and collapses the draw; the zeroed blend
    * factors and any other logic op refuse.
    */
   {
      struct pipeline_shape cb_shape = contract_shape;
      VkPipeline cb_pipeline = VK_NULL_HANDLE;
      cb_shape.blend_enable = VK_TRUE;
      cb_shape.blend_identity = VK_TRUE;
      assert(make_pipeline(&cb_shape, pass, layout, &cb_pipeline) ==
             VK_SUCCESS);
      vkDestroyPipeline(device, cb_pipeline, NULL);

      cb_shape = contract_shape;
      cb_shape.logic_op_enable = VK_TRUE;
      cb_shape.logic_op = VK_LOGIC_OP_COPY;
      assert(make_pipeline(&cb_shape, pass, layout, &cb_pipeline) ==
             VK_SUCCESS);
      vkDestroyPipeline(device, cb_pipeline, NULL);

      cb_shape.logic_op = VK_LOGIC_OP_XOR;
      assert(make_pipeline(&cb_shape, pass, layout, &cb_pipeline) ==
             R3V_NATIVE_REFUSAL_RESULT);

      cb_shape = contract_shape;
      cb_shape.write_mask_zero = VK_TRUE;
      assert(make_pipeline(&cb_shape, pass, layout, &cb_pipeline) ==
             VK_SUCCESS);
      vkDestroyPipeline(device, cb_pipeline, NULL);
   }

   /* Depth/stencil state: the fully disabled struct admits (the pass
    * carries no depth attachment, so disabled state is the one valid
    * shape) and an enabled depth test refuses at pipeline creation.
    */
   {
      struct pipeline_shape ds_shape = contract_shape;
      ds_shape.depth_stencil = 1;
      VkPipeline ds_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&ds_shape, pass, layout, &ds_pipeline) ==
             VK_SUCCESS);
      vkDestroyPipeline(device, ds_pipeline, NULL);
      ds_shape.depth_stencil = 2;
      assert(make_pipeline(&ds_shape, pass, layout, &ds_pipeline) ==
             R3V_NATIVE_REFUSAL_RESULT);
      assert(ds_pipeline == VK_NULL_HANDLE);
   }

   /* Dynamic viewport/scissor: the pipeline declares the pair dynamic
    * and the recorded vkCmdSet values carry the extent, held to the
    * cell shape at the draw.  The set persists across the pass
    * boundary (recorded before vkCmdBeginRenderPass); unset state, a
    * mismatched extent, and a non-identity depth range each refuse.
    */
   {
      struct pipeline_shape dynamic_shape = contract_shape;
      dynamic_shape.dynamic_viewport_scissor = VK_TRUE;
      VkPipeline dynamic_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&dynamic_shape, pass, layout,
                           &dynamic_pipeline) == VK_SUCCESS);
      const VkViewport full_viewport = {
         .width = (float)R3V_NATIVE_TARGET_WIDTH,
         .height = (float)R3V_NATIVE_TARGET_HEIGHT,
         .maxDepth = 1.0f,
      };
      const VkRect2D full_scissor = {
         .extent = { R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT },
      };

      VkCommandBuffer dyn_cmd = fresh_cmd();
      vkCmdSetViewport(dyn_cmd, 0, 1, &full_viewport);
      vkCmdSetScissor(dyn_cmd, 0, 1, &full_scissor);
      vkCmdBeginRenderPass(dyn_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(dyn_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dynamic_pipeline);
      vkCmdBindVertexBuffers(dyn_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(dyn_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(dyn_cmd);
      assert(vkEndCommandBuffer(dyn_cmd) == VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_dyn, dyn_cmd);
      assert(native_dyn->ib_size_dwords != 0);

      VkCommandBuffer unset_cmd = fresh_cmd();
      vkCmdBeginRenderPass(unset_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(unset_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dynamic_pipeline);
      vkCmdBindVertexBuffers(unset_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(unset_cmd, 3, 1, 0, 0);
      assert(vkEndCommandBuffer(unset_cmd) == R3V_NATIVE_REFUSAL_RESULT);

      VkCommandBuffer small_cmd = fresh_cmd();
      const VkViewport small_viewport = {
         .width = 32.0f, .height = 32.0f, .maxDepth = 1.0f,
      };
      const VkRect2D small_scissor = { .extent = { 32, 32 } };
      vkCmdSetViewport(small_cmd, 0, 1, &small_viewport);
      vkCmdSetScissor(small_cmd, 0, 1, &small_scissor);
      vkCmdBeginRenderPass(small_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(small_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dynamic_pipeline);
      vkCmdBindVertexBuffers(small_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(small_cmd, 3, 1, 0, 0);
      assert(vkEndCommandBuffer(small_cmd) == R3V_NATIVE_REFUSAL_RESULT);

      VkCommandBuffer depth_cmd = fresh_cmd();
      VkViewport shallow_viewport = full_viewport;
      shallow_viewport.maxDepth = 0.5f;
      vkCmdSetViewport(depth_cmd, 0, 1, &shallow_viewport);
      vkCmdSetScissor(depth_cmd, 0, 1, &full_scissor);
      vkCmdBeginRenderPass(depth_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(depth_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        dynamic_pipeline);
      vkCmdBindVertexBuffers(depth_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(depth_cmd, 3, 1, 0, 0);
      assert(vkEndCommandBuffer(depth_cmd) == R3V_NATIVE_REFUSAL_RESULT);

      vkDestroyPipeline(device, dynamic_pipeline, NULL);
   }

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
    * render-shape cell at the reference geometry carrying the bound
    * fragment module's constant, over the reference vertex bytes.
    */
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_cmd, cmd);
   VK_FROM_HANDLE(r3v_native_device, native_device, device);
   struct r300_triangle_render_shape module_shape;
   r300_tcl_bypass_triangle_render_shape_reference(&module_shape);
   const uint32_t module_color[4] = R3V_REFERENCE_FRAGMENT_COLOR_BITS;
   memcpy(module_shape.color_bits, module_color, sizeof(module_color));
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_clip_space_render_shape_emit(
             &module_shape, 1, &reference) == 0);
   assert(native_cmd->ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(native_cmd->ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   struct r300_tcl_bypass_triangle_ib window_reference;
   assert(r300_tcl_bypass_triangle_render_shape_emit(&module_shape,
                                                     &window_reference) == 0);
   assert(native_cmd->window_space_ib_size_dwords ==
          window_reference.ib_size_dwords);
   assert(memcmp(native_cmd->window_space_ib, window_reference.ib,
                 window_reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   assert(native_cmd->reference_count == R300_TRIANGLE_RENDER_SLOT_COUNT);
   assert(native_cmd->owned_carriers[0] != NULL);
   struct r3v_native_memory *const recorded_carrier =
      native_cmd->owned_carriers[0];
   assert(native_cmd->references[R300_TRIANGLE_SLOT_VERTEX].handle ==
          recorded_carrier->bo.handle);
   r300_tcl_bypass_triangle_release(&reference);
   r300_tcl_bypass_triangle_release(&window_reference);

   /* Beginning a new recording releases both position-space IBs and the
    * carrier owned by the prior recording.  The command buffer then accepts a
    * new empty recording without retaining either consumer.
    */
   VkCommandBuffer window_lifecycle_cmd = record_triangle_draw(
      &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_window_lifecycle,
                  window_lifecycle_cmd);
   assert(native_window_lifecycle->ib != NULL);
   assert(native_window_lifecycle->window_space_ib != NULL);
   assert(native_window_lifecycle->owned_carriers[0] != NULL);
   assert(vkBeginCommandBuffer(
             window_lifecycle_cmd,
             &(VkCommandBufferBeginInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
             }) == VK_SUCCESS);
   assert(native_window_lifecycle->ib == NULL);
   assert(native_window_lifecycle->ib_size_dwords == 0);
   assert(native_window_lifecycle->window_space_ib == NULL);
   assert(native_window_lifecycle->window_space_ib_size_dwords == 0);
   assert(native_window_lifecycle->owned_carriers[0] == NULL);
   assert(vkEndCommandBuffer(window_lifecycle_cmd) == VK_SUCCESS);

   /* Execution-time boundary, record side: recording defers the vertex
    * gather and load-op clear, so the executable command buffer has
    * touched neither the application's image memory nor its own
    * carrier.
    */
   assert(native_cmd->deferred_draws[0].pending);
   uint32_t *color_map = NULL;
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&color_map) == VK_SUCCESS);
   assert(color_map[0] == COLOR_SEED);
   vkUnmapMemory(device, color_memory);

   /* Execution-time boundary, submit side: the stream bytes the carrier
    * travels with are the ones live at execution, so a write after
    * recording is honored and each execution re-reads.  The closed
    * hazard gate refuses before the deferred execution, so the refused
    * submit below changes nothing and the direct execution that follows
    * carries the re-read proof.
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

      /* An empty render pass retains its load-op clear after
       * EndRenderPass, and a transfer recorded on either side of it
       * admits under the group its record position fixes: the copy
       * before the pass joins the pre-draw group, the copy after it the
       * post-draw group, and a second pass still refuses.
       */
      VkCommandBuffer ordered_copy_cmd = fresh_cmd();
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_ordered_copy,
                     ordered_copy_cmd);
      vkCmdCopyBuffer(ordered_copy_cmd, staging, staging, 1, &buffer_copy);
      assert(native_ordered_copy->deferred_copy_count == 1);
      assert(native_ordered_copy->deferred_copies[0].group ==
             R3V_NATIVE_COPY_GROUP_BEFORE_DRAW);
      vkCmdBeginRenderPass(ordered_copy_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(ordered_copy_cmd);
      vkCmdCopyBuffer(ordered_copy_cmd, staging, staging, 1, &buffer_copy);
      assert(native_ordered_copy->deferred_draws[0].pending);
      assert(native_ordered_copy->deferred_copy_count == 2);
      assert(native_ordered_copy->deferred_copies[1].group ==
             R3V_NATIVE_COPY_GROUP_AFTER_DRAW);
      assert(vkEndCommandBuffer(ordered_copy_cmd) == VK_SUCCESS);

      VkCommandBuffer second_pass_cmd = fresh_cmd();
      vkCmdBeginRenderPass(second_pass_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(second_pass_cmd);
      vkCmdBeginRenderPass(second_pass_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      assert(vkEndCommandBuffer(second_pass_cmd) ==
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

      /* Execution order around the deferred draw, through the queue:
       * one command buffer records a buffer-to-image upload, an
       * image-to-buffer read of the render target, the pass carrying
       * its load-op clear, and a second image-to-buffer read of the
       * same target.  The two reads land in separate readback rows, so
       * the row the pre-draw group wrote holds the target's seed and
       * the row the post-draw group wrote holds the clear texel: the
       * clear separates them, and either group executing on the wrong
       * side of it lands the other row's bytes.  Each image source read
       * runs its own cache invalidate at execution, so the sync count
       * advances over the submission.
       */
      {
         const uint32_t ordered_seed = 0xa1a1a1a1u;
         VkImageCreateInfo ordered_info = image_info;
         ordered_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
         VkImage ordered_image = VK_NULL_HANDLE;
         assert(vkCreateImage(device, &ordered_info, NULL,
                              &ordered_image) == VK_SUCCESS);
         VkDeviceMemory ordered_memory = VK_NULL_HANDLE;
         assert(vkAllocateMemory(
                   device,
                   &(VkMemoryAllocateInfo){
                      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                      .allocationSize = R3V_NATIVE_TARGET_MEMORY_BYTES,
                      .memoryTypeIndex = 0,
                   },
                   NULL, &ordered_memory) == VK_SUCCESS);
         assert(vkBindImageMemory(device, ordered_image, ordered_memory,
                                  0) == VK_SUCCESS);
         uint32_t *ordered_map = NULL;
         assert(vkMapMemory(device, ordered_memory, 0, VK_WHOLE_SIZE, 0,
                            (void **)&ordered_map) == VK_SUCCESS);
         for (unsigned i = 0; i < R3V_NATIVE_TARGET_MEMORY_BYTES / 4; i++)
            ordered_map[i] = ordered_seed;
         vkUnmapMemory(device, ordered_memory);

         VkImageView ordered_view = VK_NULL_HANDLE;
         assert(vkCreateImageView(
                   device,
                   &(VkImageViewCreateInfo){
                      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                      .image = ordered_image,
                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                      .format = R3V_NATIVE_TARGET_FORMAT,
                      .subresourceRange =
                         {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .levelCount = 1,
                            .layerCount = 1,
                         },
                   },
                   NULL, &ordered_view) == VK_SUCCESS);
         VkFramebuffer ordered_framebuffer = VK_NULL_HANDLE;
         assert(vkCreateFramebuffer(
                   device,
                   &(VkFramebufferCreateInfo){
                      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                      .renderPass = pass,
                      .attachmentCount = 1,
                      .pAttachments = &ordered_view,
                      .width = R3V_NATIVE_TARGET_WIDTH,
                      .height = R3V_NATIVE_TARGET_HEIGHT,
                      .layers = 1,
                   },
                   NULL, &ordered_framebuffer) == VK_SUCCESS);

         /* (0.25, 0.5, 0.75, 1) reaches bytes 64, 128, 191, 255, which
          * B8G8R8A8 stores as 0xff4080bf.
          */
         const uint32_t ordered_clear_dword = 0xff4080bfu;
         VkRenderPassBeginInfo ordered_begin = begin_pass;
         ordered_begin.framebuffer = ordered_framebuffer;
         ordered_begin.pClearValues = &(VkClearValue){
            .color = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } },
         };

         VkBuffer readback = VK_NULL_HANDLE;
         assert(vkCreateBuffer(
                   device,
                   &(VkBufferCreateInfo){
                      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                      .size = 2 * R3V_NATIVE_TARGET_ROW_BYTES,
                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                   },
                   NULL, &readback) == VK_SUCCESS);
         VkDeviceMemory readback_memory = VK_NULL_HANDLE;
         assert(vkAllocateMemory(
                   device,
                   &(VkMemoryAllocateInfo){
                      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                      .allocationSize = 2 * R3V_NATIVE_TARGET_ROW_BYTES,
                      .memoryTypeIndex = 0,
                   },
                   NULL, &readback_memory) == VK_SUCCESS);
         assert(vkBindBufferMemory(device, readback, readback_memory, 0) ==
                VK_SUCCESS);

         VkBufferImageCopy row_read = {
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .layerCount = 1 },
            .imageExtent = { R3V_NATIVE_TARGET_WIDTH, 1, 1 },
         };

         /* The transfer image returns to the sentinel, so the upload
          * this submission carries is the one the assert reads.
          */
         uint32_t *reseed_map = NULL;
         assert(vkMapMemory(device, mem_a, 0, VK_WHOLE_SIZE, 0,
                            (void **)&reseed_map) == VK_SUCCESS);
         for (uint32_t t = 0; t < transfer_allocation_words; t++)
            reseed_map[t] = R300_TRIANGLE_COLOR_SENTINEL;
         vkUnmapMemory(device, mem_a);

         VkCommandBuffer ordered_cmd = fresh_cmd();
         VK_FROM_HANDLE(r3v_native_cmd_buffer, native_ordered, ordered_cmd);
         vkCmdCopyBufferToImage(ordered_cmd, staging, img_a,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &upload);
         vkCmdCopyImageToBuffer(ordered_cmd, ordered_image,
                                VK_IMAGE_LAYOUT_GENERAL, readback, 1,
                                &row_read);
         vkCmdBeginRenderPass(ordered_cmd, &ordered_begin,
                              VK_SUBPASS_CONTENTS_INLINE);
         vkCmdEndRenderPass(ordered_cmd);
         row_read.bufferOffset = R3V_NATIVE_TARGET_ROW_BYTES;
         vkCmdCopyImageToBuffer(ordered_cmd, ordered_image,
                                VK_IMAGE_LAYOUT_GENERAL, readback, 1,
                                &row_read);
         assert(vkEndCommandBuffer(ordered_cmd) == VK_SUCCESS);
         assert(native_ordered->ib_size_dwords == 0);
         assert(native_ordered->deferred_copy_count == 3);
         assert(native_ordered->deferred_copies[0].group ==
                R3V_NATIVE_COPY_GROUP_BEFORE_DRAW);
         assert(native_ordered->deferred_copies[1].group ==
                R3V_NATIVE_COPY_GROUP_BEFORE_DRAW);
         assert(native_ordered->deferred_copies[2].group ==
                R3V_NATIVE_COPY_GROUP_AFTER_DRAW);

         const uint64_t ordered_sync_before =
            native_device->drm.cache_sync_count;
         assert(vkQueueSubmit(queue, 1,
                              &(VkSubmitInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &ordered_cmd,
                              },
                              VK_NULL_HANDLE) == VK_SUCCESS);
         /* Nine syncs: each of the three copies establishes its two
          * mappings, each of the two render-target reads invalidates its
          * source, and the load-op clear publishes the target.  The count
          * is exact, so a dropped source invalidate lands at seven.
          */
         assert(native_device->drm.cache_sync_count ==
                ordered_sync_before + 9);

         uint32_t *readback_map = NULL;
         assert(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                            (void **)&readback_map) == VK_SUCCESS);
         assert(readback_map[0] == ordered_seed);
         assert(readback_map[R3V_NATIVE_TARGET_WIDTH - 1] == ordered_seed);
         assert(readback_map[R3V_NATIVE_TARGET_WIDTH] ==
                ordered_clear_dword);
         assert(readback_map[2 * R3V_NATIVE_TARGET_WIDTH - 1] ==
                ordered_clear_dword);
         vkUnmapMemory(device, readback_memory);

         /* The upload the pre-draw group carried landed in the transfer
          * image the pass never touches.
          */
         uint32_t *upload_map = NULL;
         assert(vkMapMemory(device, mem_a, 0, VK_WHOLE_SIZE, 0,
                            (void **)&upload_map) == VK_SUCCESS);
         assert(upload_map[transfer_image_base_word + 16 + 2] ==
                (0x40000000u | 0));
         vkUnmapMemory(device, mem_a);

         vkDestroyBuffer(device, readback, NULL);
         vkFreeMemory(device, readback_memory, NULL);
         vkDestroyFramebuffer(device, ordered_framebuffer, NULL);
         vkDestroyImageView(device, ordered_view, NULL);
         vkDestroyImage(device, ordered_image, NULL);
         vkFreeMemory(device, ordered_memory, NULL);
      }

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

   /* Pre-commit refusal leaves bytes unchanged: the closed gate refuses
    * before the deferred draw, so the carrier keeps its pre-submit
    * content and the target keeps its seed.
    */
   uint32_t carrier_before[R300_TRIANGLE_VERTEX_DWORDS];
   void *carrier_map = NULL;
   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_cmd->owned_carriers[0]->bo,
                               &carrier_map) == 0);
   memcpy(carrier_before, carrier_map, sizeof(carrier_before));
   radeon_drm_vk_bo_unmap(&native_device->drm,
                          &native_cmd->owned_carriers[0]->bo, carrier_map);
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   assert(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE) ==
          VK_ERROR_DEVICE_LOST);
   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_cmd->owned_carriers[0]->bo,
                               &carrier_map) == 0);
   assert(memcmp(carrier_before, carrier_map, sizeof(carrier_before)) == 0);
   radeon_drm_vk_bo_unmap(&native_device->drm,
                          &native_cmd->owned_carriers[0]->bo, carrier_map);
   assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&color_map) == VK_SUCCESS);
   assert(color_map[0] == COLOR_SEED);
   assert(color_map[(R3V_NATIVE_TARGET_MEMORY_BYTES / 4) - 1] == COLOR_SEED);
   vkUnmapMemory(device, color_memory);

   /* The execution re-reads the live stream: the deferred executor,
    * driven directly, carries the mutated record into the carrier.
    */
   assert(r3v_native_cmd_buffer_execute_deferred_draws(
             native_device, native_cmd) == VK_SUCCESS);
   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_cmd->owned_carriers[0]->bo,
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
                          &native_cmd->owned_carriers[0]->bo, carrier_map);

   /* Restore and re-execute: the runtime latches the device lost after
    * the refused submit and later submits return before the driver
    * runs, so execution exercises the queue's execution step directly
    * -- the harness links the implementation.  The command buffer
    * retains its carrier BO across executions, and each execution
    * re-reads the stream into that carrier.
    */
   assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
          VK_SUCCESS);
   memcpy(map, ndc_triangle, sizeof(ndc_triangle));
   vkUnmapMemory(device, vertex_memory);
   assert(r3v_native_cmd_buffer_execute_deferred_draws(
             native_device, native_cmd) == VK_SUCCESS);
   assert(native_cmd->owned_carriers[0] == recorded_carrier);

   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_cmd->owned_carriers[0]->bo,
                               &carrier_map) == 0);
   assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                 R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
   radeon_drm_vk_bo_unmap(&native_device->drm,
                          &native_cmd->owned_carriers[0]->bo, carrier_map);

   /* The load-op clear executed over the image's declared footprint
    * alone: sentinel inside, the page past the footprint untouched.
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
   assert(r3v_native_cmd_buffer_execute_deferred_draws(
             native_device, native_xyz) == VK_SUCCESS);
   assert(radeon_drm_vk_bo_map(&native_device->drm,
                               &native_xyz->owned_carriers[0]->bo,
                               &carrier_map) == 0);
   assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                 R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
   radeon_drm_vk_bo_unmap(&native_device->drm,
                          &native_xyz->owned_carriers[0]->bo, carrier_map);

   /* Homogeneous clipping: finite non-unit W, each default clip plane, full
    * rejection, the seven-triangle capacity case, and non-finite refusal all
    * execute through the same public draw and deferred carrier path.
    */
   {
      float homogeneous[12];
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         homogeneous[vertex * 4 + 0] = ndc_triangle[vertex * 4 + 0] * 2.0f;
         homogeneous[vertex * 4 + 1] = ndc_triangle[vertex * 4 + 1] * 2.0f;
         homogeneous[vertex * 4 + 2] = ndc_triangle[vertex * 4 + 2] * 2.0f;
         homogeneous[vertex * 4 + 3] = 2.0f;
      }
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, homogeneous, sizeof(homogeneous));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer homogeneous_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_homogeneous,
                     homogeneous_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_homogeneous) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_homogeneous->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *homogeneous_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(homogeneous_records, 4) ==
             1);
      static const float expected_xy[3][2] = {
         { 8.0f, 8.0f }, { 56.0f, 8.0f }, { 32.0f, 56.0f },
      };
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         assert_float_near(homogeneous_records[vertex * 4 + 0],
                           expected_xy[vertex][0]);
         assert_float_near(homogeneous_records[vertex * 4 + 1],
                           expected_xy[vertex][1]);
         assert_float_near(homogeneous_records[vertex * 4 + 2], 0.0f);
         assert_float_near(homogeneous_records[vertex * 4 + 3], 0.5f);
      }
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_homogeneous->owned_carriers[0]->bo,
         carrier_map);

      /* A scale-invariant clip position remains valid at the smallest
       * positive normal W.  The divide recovers the same window coordinates
       * and publishes a finite reciprocal instead of refusing the draw.
       */
      float small_w[12];
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         small_w[vertex * 4 + 0] = ndc_triangle[vertex * 4 + 0] * FLT_MIN;
         small_w[vertex * 4 + 1] = ndc_triangle[vertex * 4 + 1] * FLT_MIN;
         small_w[vertex * 4 + 2] = ndc_triangle[vertex * 4 + 2] * FLT_MIN;
         small_w[vertex * 4 + 3] = FLT_MIN;
      }
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, small_w, sizeof(small_w));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer small_w_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_small_w, small_w_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_small_w) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm, &native_small_w->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *small_w_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(small_w_records, 4) == 1);
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         assert_float_near(small_w_records[vertex * 4 + 0],
                           expected_xy[vertex][0]);
         assert_float_near(small_w_records[vertex * 4 + 1],
                           expected_xy[vertex][1]);
         assert_float_near(small_w_records[vertex * 4 + 2], 0.0f);
         assert(isfinite(small_w_records[vertex * 4 + 3]));
         assert(small_w_records[vertex * 4 + 3] == 1.0f / FLT_MIN);
      }
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_small_w->owned_carriers[0]->bo,
         carrier_map);

      /* A positive subnormal W also represents valid homogeneous geometry.
       * One common reciprocal scale keeps every source-triangle W finite
       * while preserving the ratios used for perspective interpolation.
       */
      const float subnormal_w_value = ldexpf(1.0f, -140);
      static const float subnormal_ndc[3][3] = {
         { -0.5f, -0.5f, 0.5f },
         { 0.5f, -0.5f, 0.5f },
         { 0.0f, 0.5f, 0.5f },
      };
      float subnormal_w[12];
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         const float vertex_w = ldexpf(subnormal_w_value, vertex);
         for (uint32_t component = 0; component < 3; component++) {
            subnormal_w[vertex * 4 + component] =
               subnormal_ndc[vertex][component] * vertex_w;
         }
         subnormal_w[vertex * 4 + 3] = vertex_w;
      }
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, subnormal_w, sizeof(subnormal_w));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer subnormal_w_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_subnormal_w,
                     subnormal_w_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_subnormal_w) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_subnormal_w->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *subnormal_w_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(subnormal_w_records, 4) ==
             1);
      static const float subnormal_expected_xy[3][2] = {
         { 16.0f, 16.0f }, { 48.0f, 16.0f }, { 32.0f, 48.0f },
      };
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         assert_float_near(subnormal_w_records[vertex * 4 + 0],
                           subnormal_expected_xy[vertex][0]);
         assert_float_near(subnormal_w_records[vertex * 4 + 1],
                           subnormal_expected_xy[vertex][1]);
         assert_float_near(subnormal_w_records[vertex * 4 + 2], 0.5f);
         assert(isfinite(subnormal_w_records[vertex * 4 + 3]));
         assert(subnormal_w_records[vertex * 4 + 3] > 0.0f);
      }
      assert(subnormal_w_records[3] == FLT_MAX);
      assert(subnormal_w_records[7] == FLT_MAX / 2.0f);
      assert(subnormal_w_records[11] == FLT_MAX / 4.0f);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_subnormal_w->owned_carriers[0]->bo,
         carrier_map);

      const float inside[12] = {
         -0.5f, -0.5f, 0.5f, 1.0f,
          0.5f, -0.5f, 0.5f, 1.0f,
          0.0f,  0.5f, 0.5f, 1.0f,
      };
      float plane_crossings[6][12];
      for (uint32_t plane = 0; plane < 6; plane++)
         memcpy(plane_crossings[plane], inside, sizeof(inside));
      plane_crossings[0][0] = -1.5f;
      plane_crossings[1][4] = 1.5f;
      plane_crossings[2][1] = -1.5f;
      plane_crossings[3][9] = 1.5f;
      plane_crossings[4][2] = -0.5f;
      plane_crossings[5][10] = 1.5f;
      static const uint32_t boundary_component[6] = { 0, 0, 1, 1, 2, 2 };
      static const float boundary_value[6] = {
         0.0f, 64.0f, 0.0f, 64.0f, 0.0f, 1.0f,
      };
      for (uint32_t plane = 0; plane < 6; plane++) {
         assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                            &map) == VK_SUCCESS);
         memcpy(map, plane_crossings[plane], sizeof(plane_crossings[plane]));
         vkUnmapMemory(device, vertex_memory);
         VkCommandBuffer plane_cmd = record_triangle_draw(
            &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
         VK_FROM_HANDLE(r3v_native_cmd_buffer, native_plane, plane_cmd);
         assert(r3v_native_cmd_buffer_execute_deferred_draws(
                   native_device, native_plane) == VK_SUCCESS);
         assert(radeon_drm_vk_bo_map(
                   &native_device->drm, &native_plane->owned_carriers[0]->bo,
                   &carrier_map) == 0);
         const float *plane_records = carrier_map;
         assert(carrier_nondegenerate_triangle_count(plane_records, 4) == 2);
         bool found_boundary = false;
         for (uint32_t vertex = 0; vertex < 6; vertex++) {
            const float *record = &plane_records[vertex * 4];
            assert(isfinite(record[0]) && record[0] >= 0.0f &&
                   record[0] <= 64.0f);
            assert(isfinite(record[1]) && record[1] >= 0.0f &&
                   record[1] <= 64.0f);
            assert(isfinite(record[2]) && record[2] >= 0.0f &&
                   record[2] <= 1.0f);
            assert_float_near(record[3], 1.0f);
            found_boundary |=
               fabsf(record[boundary_component[plane]] -
                     boundary_value[plane]) <= 1.0e-5f;
         }
         assert(found_boundary);
         radeon_drm_vk_bo_unmap(
            &native_device->drm, &native_plane->owned_carriers[0]->bo,
            carrier_map);
      }

      /* Binary64 intersections preserve previously established clip planes
       * when one edge joins clip positions with widely separated scales.
       * Every reserved record must remain finite and inside the viewport.
       */
      static const float disparate_scale[12] = {
         -1241626443776.0f, 2212753571840.0f, -2019629465600.0f,
         -2088773484544.0f,
         -3.025635297859708e-11f, 2.617332912902004e-11f,
         1.5946070852645988e-11f, 1.8961415770846202e-11f,
         -28365.953125f, 68691.125f, -117244.2421875f, 128468.484375f,
      };
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, disparate_scale, sizeof(disparate_scale));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer disparate_scale_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_disparate_scale,
                     disparate_scale_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_disparate_scale) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_disparate_scale->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *disparate_scale_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(disparate_scale_records,
                                                  4) >= 1);
      for (uint32_t vertex = 0;
           vertex <
              R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u;
           vertex++) {
         const float *position = &disparate_scale_records[vertex * 4u];
         assert(isfinite(position[0]) && position[0] >= 0.0f &&
                position[0] <= 64.0f);
         assert(isfinite(position[1]) && position[1] >= 0.0f &&
                position[1] <= 64.0f);
         assert(isfinite(position[2]) && position[2] >= 0.0f &&
                position[2] <= 1.0f);
         assert(isfinite(position[3]) && position[3] > 0.0f);
      }
      radeon_drm_vk_bo_unmap(
         &native_device->drm,
         &native_disparate_scale->owned_carriers[0]->bo, carrier_map);

      /* Clipping can generate a positive W below FLT_TRUE_MIN.  When one
       * source polygon also reaches FLT_MAX, no exact common binary32
       * reciprocal scale exists; every live and reserved record still keeps
       * a finite positive reciprocal instead of rounding to zero.
       */
      const float minimum_float = FLT_TRUE_MIN;
      const float reciprocal_range[12] = {
         -2.0f * minimum_float, 0.0f, 0.0f, minimum_float,
         3.0f * minimum_float, 0.0f, 0.0f, -minimum_float,
         0.0f, FLT_MAX / 2.0f, FLT_MAX / 2.0f, FLT_MAX,
      };
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, reciprocal_range, sizeof(reciprocal_range));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer reciprocal_range_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_reciprocal_range,
                     reciprocal_range_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_reciprocal_range) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_reciprocal_range->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *reciprocal_range_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(reciprocal_range_records,
                                                  4) == 3);
      bool found_minimum_reciprocal = false;
      for (uint32_t vertex = 0;
           vertex <
              R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u;
           vertex++) {
         const float *position = &reciprocal_range_records[vertex * 4u];
         assert(isfinite(position[0]) && position[0] >= 0.0f &&
                position[0] <= 64.0f);
         assert(isfinite(position[1]) && position[1] >= 0.0f &&
                position[1] <= 64.0f);
         assert(isfinite(position[2]) && position[2] >= 0.0f &&
                position[2] <= 1.0f);
         assert(isfinite(position[3]) && position[3] > 0.0f);
         found_minimum_reciprocal |= position[3] == FLT_TRUE_MIN;
      }
      assert(found_minimum_reciprocal);
      radeon_drm_vk_bo_unmap(
         &native_device->drm,
         &native_reciprocal_range->owned_carriers[0]->bo, carrier_map);

      float fully_clipped[12];
      memcpy(fully_clipped, inside, sizeof(fully_clipped));
      fully_clipped[0] = -2.0f;
      fully_clipped[4] = -2.0f;
      fully_clipped[8] = -2.0f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, fully_clipped, sizeof(fully_clipped));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer fully_clipped_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_fully_clipped,
                     fully_clipped_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_fully_clipped) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_fully_clipped->owned_carriers[0]->bo,
                &carrier_map) == 0);
      assert(carrier_nondegenerate_triangle_count(carrier_map, 4) == 0);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_fully_clipped->owned_carriers[0]->bo,
         carrier_map);

      static const float seven_triangle_input[12] = {
         -5.57813016f, -6.03323114f,  3.33600657f, 6.03171393f,
         -8.93891653f,  8.02412914f,  8.80076537f, 3.93535488f,
          9.53063369f, -0.01053131f, -3.90294560f, 3.07934032f,
      };
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, seven_triangle_input, sizeof(seven_triangle_input));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer seven_triangle_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_seven_triangle,
                     seven_triangle_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_seven_triangle) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_seven_triangle->owned_carriers[0]->bo,
                &carrier_map) == 0);
      assert(carrier_nondegenerate_triangle_count(carrier_map, 4) ==
             R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_seven_triangle->owned_carriers[0]->bo,
         carrier_map);

      const size_t clip_carrier_dwords =
         R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u * 4u;
      uint32_t carrier_seed
         [R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u * 4u];
      for (uint32_t dword = 0; dword < clip_carrier_dwords; dword++)
         carrier_seed[dword] = 0x5a000000u | dword;
      static const uint32_t nonfinite_bits[2] = { 0x7fc00000u, 0x7f800000u };
      for (uint32_t mutant = 0; mutant < ARRAY_SIZE(nonfinite_bits); mutant++) {
         float nonfinite[12];
         memcpy(nonfinite, inside, sizeof(nonfinite));
         memcpy(&nonfinite[0], &nonfinite_bits[mutant], sizeof(uint32_t));
         assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                            &map) == VK_SUCCESS);
         memcpy(map, nonfinite, sizeof(nonfinite));
         vkUnmapMemory(device, vertex_memory);
         VkCommandBuffer nonfinite_cmd = record_triangle_draw(
            &begin_pass, pipeline, vertex_buffer, 3, 1, 0);
         VK_FROM_HANDLE(r3v_native_cmd_buffer, native_nonfinite,
                        nonfinite_cmd);
         assert(radeon_drm_vk_bo_map(
                   &native_device->drm,
                   &native_nonfinite->owned_carriers[0]->bo,
                   &carrier_map) == 0);
         memcpy(carrier_map, carrier_seed, sizeof(carrier_seed));
         radeon_drm_vk_bo_cache_sync(
            &native_device->drm, carrier_map, sizeof(carrier_seed));
         radeon_drm_vk_bo_unmap(
            &native_device->drm, &native_nonfinite->owned_carriers[0]->bo,
            carrier_map);
         assert(r3v_native_cmd_buffer_execute_deferred_draws(
                   native_device, native_nonfinite) != VK_SUCCESS);
         assert(radeon_drm_vk_bo_map(
                   &native_device->drm,
                   &native_nonfinite->owned_carriers[0]->bo,
                   &carrier_map) == 0);
         assert(memcmp(carrier_map, carrier_seed, sizeof(carrier_seed)) == 0);
         radeon_drm_vk_bo_unmap(
            &native_device->drm, &native_nonfinite->owned_carriers[0]->bo,
            carrier_map);
      }

      /* Publication is transactional across the complete source list: a
       * non-finite position in the second triangle leaves both fixed-capacity
       * output groups byte-identical to their pre-execution seed.
       */
      float later_nonfinite[24];
      memcpy(later_nonfinite, inside, sizeof(inside));
      memcpy(later_nonfinite + 12, inside, sizeof(inside));
      memcpy(&later_nonfinite[12], &nonfinite_bits[0], sizeof(uint32_t));
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, later_nonfinite, sizeof(later_nonfinite));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer later_nonfinite_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 6, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_later_nonfinite,
                     later_nonfinite_cmd);
      uint32_t two_group_seed
         [2u * R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u * 4u];
      for (uint32_t dword = 0; dword < ARRAY_SIZE(two_group_seed); dword++)
         two_group_seed[dword] = 0xa5000000u | dword;
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_later_nonfinite->owned_carriers[0]->bo,
                &carrier_map) == 0);
      memcpy(carrier_map, two_group_seed, sizeof(two_group_seed));
      radeon_drm_vk_bo_cache_sync(&native_device->drm, carrier_map,
                                  sizeof(two_group_seed));
      radeon_drm_vk_bo_unmap(
         &native_device->drm,
         &native_later_nonfinite->owned_carriers[0]->bo, carrier_map);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_later_nonfinite) != VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_later_nonfinite->owned_carriers[0]->bo,
                &carrier_map) == 0);
      assert(memcmp(carrier_map, two_group_seed, sizeof(two_group_seed)) == 0);
      radeon_drm_vk_bo_unmap(
         &native_device->drm,
         &native_later_nonfinite->owned_carriers[0]->bo, carrier_map);

      /* Restore the reference stream for the delivery-route legs below. */
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

   /* Generated vertices carry the smooth varying with the same clip-space
    * edge parameter as gl_Position.  The left-plane crossing produces two
    * intersections whose colors are the exact half-edge interpolants.
    */
   {
      struct pipeline_shape varying_shape = contract_shape;
      varying_shape.vertex_words = r3v_reference_vertex_varying_spirv;
      varying_shape.vertex_bytes = sizeof(r3v_reference_vertex_varying_spirv);
      varying_shape.fragment_words = r3v_reference_fragment_varying_spirv;
      varying_shape.fragment_bytes =
         sizeof(r3v_reference_fragment_varying_spirv);
      VkPipeline varying_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&varying_shape, pass, layout, &varying_pipeline) ==
             VK_SUCCESS);
      const float varying_crossing[12] = {
         -2.0f,  0.0f, 0.5f, 1.0f,
          0.0f, -0.5f, 0.5f, 1.0f,
          0.0f,  0.5f, 0.5f, 1.0f,
      };
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, varying_crossing, sizeof(varying_crossing));
      vkUnmapMemory(device, vertex_memory);
      VkCommandBuffer varying_cmd = record_triangle_draw(
         &begin_pass, varying_pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_varying, varying_cmd);
      assert(native_varying->window_space_ib == NULL);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_varying) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm, &native_varying->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *varying_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(varying_records, 8) == 2);
      static const float first_intersection[8] = {
         0.0f, 40.0f, 0.5f, 1.0f, 0.0f, 0.625f, 0.25f, 1.0f,
      };
      static const float second_intersection[8] = {
         0.0f, 24.0f, 0.5f, 1.0f, 0.0f, 0.375f, 0.25f, 1.0f,
      };
      for (uint32_t component = 0; component < 8; component++) {
         assert_float_near(varying_records[component],
                           first_intersection[component]);
         assert_float_near(varying_records[8 + component],
                           second_intersection[component]);
      }
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_varying->owned_carriers[0]->bo,
         carrier_map);
      vkDestroyPipeline(device, varying_pipeline, NULL);

      /* The shader-interface record rides pipeline creation: a Flat
       * vertex/fragment pair admits with the qualifier recorded on the
       * pipeline, a Flat vertex output against a NoPerspective fragment
       * input refuses as a cross-stage conflict, and a Flat vertex
       * output without a fragment consumer refuses by the link. */
      struct pipeline_shape flat_shape = varying_shape;
      flat_shape.vertex_words = r3v_reference_vertex_flat_spirv;
      flat_shape.vertex_bytes = sizeof(r3v_reference_vertex_flat_spirv);
      flat_shape.fragment_words = r3v_reference_fragment_flat_spirv;
      flat_shape.fragment_bytes = sizeof(r3v_reference_fragment_flat_spirv);
      VkPipeline flat_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&flat_shape, pass, layout, &flat_pipeline) ==
             VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_pipeline, native_flat, flat_pipeline);
      assert(native_flat->varying);
      assert(native_flat->shader_interface.varying_mask == 1u &&
             native_flat->shader_interface.flat_mask == 1u &&
             native_flat->shader_interface.noperspective_mask == 0u);
      assert(native_flat->shader_interface.varyings[0].interpolation ==
             R3V_SHADER_INTERFACE_FLAT);
      vkDestroyPipeline(device, flat_pipeline, NULL);
      struct pipeline_shape conflict_shape = flat_shape;
      conflict_shape.fragment_words =
         r3v_reference_fragment_noperspective_spirv;
      conflict_shape.fragment_bytes =
         sizeof(r3v_reference_fragment_noperspective_spirv);
      VkPipeline conflict_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&conflict_shape, pass, layout,
                           &conflict_pipeline) == R3V_NATIVE_REFUSAL_RESULT &&
             conflict_pipeline == VK_NULL_HANDLE);
      struct pipeline_shape unconsumed_shape = flat_shape;
      unconsumed_shape.fragment_words = r3v_reference_fragment_spirv;
      unconsumed_shape.fragment_bytes = sizeof(r3v_reference_fragment_spirv);
      VkPipeline unconsumed_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&unconsumed_shape, pass, layout,
                           &unconsumed_pipeline) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             unconsumed_pipeline == VK_NULL_HANDLE);

      /* Flat over the partially clipped triangle: the vertex buffer
       * still holds varying_crossing, whose tint is the vertex position
       * itself, so every non-degenerate record of the clipped fan
       * carries the provoking (first) vertex's position as its varying,
       * the generated clip vertices included. */
      static const float provoking_tint[4] = { -2.0f, 0.0f, 0.5f, 1.0f };
      assert(make_pipeline(&flat_shape, pass, layout, &flat_pipeline) ==
             VK_SUCCESS);
      VkCommandBuffer flat_cmd = record_triangle_draw(
         &begin_pass, flat_pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_flat_cmd, flat_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_flat_cmd) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm, &native_flat_cmd->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *flat_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(flat_records, 8) == 2);
      assert(carrier_varying_mismatch_count(flat_records, provoking_tint) ==
             0);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_flat_cmd->owned_carriers[0]->bo,
         carrier_map);
      vkDestroyPipeline(device, flat_pipeline, NULL);

      /* Metadata-drop mutation: the same modules with every Flat
       * decoration stripped build a Smooth pipeline, and the same draw
       * interpolates the three positions, so the flat oracle counts
       * mismatching records. */
      uint32_t stripped_vs[sizeof(r3v_reference_vertex_flat_spirv) / 4];
      uint32_t stripped_fs[sizeof(r3v_reference_fragment_flat_spirv) / 4];
      const size_t stripped_vs_words = strip_flat_decorations(
         r3v_reference_vertex_flat_spirv,
         sizeof(r3v_reference_vertex_flat_spirv) / 4, stripped_vs);
      const size_t stripped_fs_words = strip_flat_decorations(
         r3v_reference_fragment_flat_spirv,
         sizeof(r3v_reference_fragment_flat_spirv) / 4, stripped_fs);
      assert(stripped_vs_words < sizeof(r3v_reference_vertex_flat_spirv) / 4);
      assert(stripped_fs_words <
             sizeof(r3v_reference_fragment_flat_spirv) / 4);
      struct pipeline_shape dropped_shape = flat_shape;
      dropped_shape.vertex_words = stripped_vs;
      dropped_shape.vertex_bytes = stripped_vs_words * 4;
      dropped_shape.fragment_words = stripped_fs;
      dropped_shape.fragment_bytes = stripped_fs_words * 4;
      VkPipeline dropped_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&dropped_shape, pass, layout, &dropped_pipeline) ==
             VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_pipeline, native_dropped, dropped_pipeline);
      assert(native_dropped->shader_interface.flat_mask == 0 &&
             native_dropped->post_vs.flat_mask == 0);
      VkCommandBuffer dropped_cmd = record_triangle_draw(
         &begin_pass, dropped_pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_dropped_cmd, dropped_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_dropped_cmd) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_dropped_cmd->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *dropped_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(dropped_records, 8) == 2);
      assert(carrier_varying_mismatch_count(dropped_records,
                                            provoking_tint) != 0);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_dropped_cmd->owned_carriers[0]->bo,
         carrier_map);
      vkDestroyPipeline(device, dropped_pipeline, NULL);

      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

   /* Facing is decided after clipping and viewport projection.  The reference
    * triangle is counter-clockwise, so front-face culling collapses its one
    * live slot and leaves all seven fixed slots degenerate.
    */
   {
      struct pipeline_shape cull_shape = contract_shape;
      cull_shape.cull_mode = VK_CULL_MODE_FRONT_BIT;
      cull_shape.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      VkPipeline cull_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&cull_shape, pass, layout, &cull_pipeline) ==
             VK_SUCCESS);
      VkCommandBuffer cull_cmd = record_triangle_draw(
         &begin_pass, cull_pipeline, vertex_buffer, 3, 1, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_cull, cull_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_cull) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm, &native_cull->owned_carriers[0]->bo,
                &carrier_map) == 0);
      assert(carrier_nondegenerate_triangle_count(carrier_map, 4) == 0);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_cull->owned_carriers[0]->bo,
         carrier_map);
      vkDestroyPipeline(device, cull_pipeline, NULL);
   }

   /* Instance expansion and indexed gathers feed source triangles into the
    * same seven-slot clipper.  Two instances own two independent slot groups;
    * the cyclic index order proves the indexed source order reaches projection.
    */
   {
      VkCommandBuffer instanced_cmd = record_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, 3, 2, 0);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_instanced, instanced_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_instanced) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm, &native_instanced->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *instanced_records = carrier_map;
      const uint32_t records_per_source =
         R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u;
      assert(carrier_nondegenerate_triangle_count(instanced_records, 4) == 1);
      assert(carrier_nondegenerate_triangle_count(
                instanced_records + records_per_source * 4u, 4) == 1);
      assert(memcmp(instanced_records,
                    instanced_records + records_per_source * 4u,
                    R300_TRIANGLE_VERTEX_DWORDS * sizeof(float)) == 0);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_instanced->owned_carriers[0]->bo,
         carrier_map);

      VkDeviceMemory index_memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = 4096,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &index_memory) == VK_SUCCESS);
      VkBuffer index_buffer = VK_NULL_HANDLE;
      assert(vkCreateBuffer(device,
                            &(VkBufferCreateInfo){
                               .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 6,
                               .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            },
                            NULL, &index_buffer) == VK_SUCCESS);
      assert(vkBindBufferMemory(device, index_buffer, index_memory, 0) ==
             VK_SUCCESS);
      assert(vkMapMemory(device, index_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS);
      const uint16_t indices[3] = { 2, 0, 1 };
      memcpy(map, indices, sizeof(indices));
      vkUnmapMemory(device, index_memory);
      VkCommandBuffer indexed_cmd = record_indexed_triangle_draw(
         &begin_pass, pipeline, vertex_buffer, index_buffer);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_indexed, indexed_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_indexed) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm, &native_indexed->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *indexed_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(indexed_records, 4) == 1);
      static const float indexed_xy[3][2] = {
         { 32.0f, 56.0f }, { 8.0f, 8.0f }, { 56.0f, 8.0f },
      };
      for (uint32_t vertex = 0; vertex < 3; vertex++) {
         assert_float_near(indexed_records[vertex * 4 + 0],
                           indexed_xy[vertex][0]);
         assert_float_near(indexed_records[vertex * 4 + 1],
                           indexed_xy[vertex][1]);
      }
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_indexed->owned_carriers[0]->bo,
         carrier_map);

      /* The Flat pipeline over the same (2, 0, 1) list: the provoking
       * vertex is the list's first entry, vertex 2, whose tint is its
       * own position, so all three records carry it. */
      struct pipeline_shape indexed_flat_shape = contract_shape;
      indexed_flat_shape.vertex_words = r3v_reference_vertex_flat_spirv;
      indexed_flat_shape.vertex_bytes =
         sizeof(r3v_reference_vertex_flat_spirv);
      indexed_flat_shape.fragment_words = r3v_reference_fragment_flat_spirv;
      indexed_flat_shape.fragment_bytes =
         sizeof(r3v_reference_fragment_flat_spirv);
      VkPipeline indexed_flat_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&indexed_flat_shape, pass, layout,
                           &indexed_flat_pipeline) == VK_SUCCESS);
      VkCommandBuffer indexed_flat_cmd = record_indexed_triangle_draw(
         &begin_pass, indexed_flat_pipeline, vertex_buffer, index_buffer);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_indexed_flat,
                     indexed_flat_cmd);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_indexed_flat) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(
                &native_device->drm,
                &native_indexed_flat->owned_carriers[0]->bo,
                &carrier_map) == 0);
      const float *indexed_flat_records = carrier_map;
      assert(carrier_nondegenerate_triangle_count(indexed_flat_records, 8) ==
             1);
      assert(carrier_varying_mismatch_count(indexed_flat_records,
                                            &ndc_triangle[2 * 4]) == 0);
      radeon_drm_vk_bo_unmap(
         &native_device->drm, &native_indexed_flat->owned_carriers[0]->bo,
         carrier_map);
      vkDestroyPipeline(device, indexed_flat_pipeline, NULL);
      vkDestroyBuffer(device, index_buffer, NULL);
      vkFreeMemory(device, index_memory, NULL);
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
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_cmd) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_cmd->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_cmd->owned_carriers[0]->bo, carrier_map);

      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, positive_triangle, sizeof(positive_triangle));
      vkUnmapMemory(device, vertex_memory);
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_cmd) == VK_SUCCESS);

      float narrow[12];
      memcpy(narrow, positive_triangle, sizeof(narrow));
      narrow[0] = 0.1f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, narrow, sizeof(narrow));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_cmd) != VK_SUCCESS);

      /* The same stream rides the CPU route: an unset gate and a non-"1"
       * value both keep the default delivery.
       */
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "0", 1) == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_cmd) == VK_SUCCESS);
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_cmd) == VK_SUCCESS);

      /* Restore the reference stream for the legs below. */
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

   /* The public GPU-producer route: under the exact double opt-in the
    * queue-time admission composes the producer pass ahead of the
    * recorded consumer, poisons the carrier, and retains the CPU
    * oracle; the host fill stays out, the read-back verdict decides
    * delivery, and a divergence quarantines the capability.  Every
    * refusal shape -- single gate, off-domain record, quarantine --
    * refuses by name.
    */
   {
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, positive_triangle, sizeof(positive_triangle));
      vkUnmapMemory(device, vertex_memory);

      VkCommandBuffer gpu_cmd = fresh_cmd();
      vkCmdBeginRenderPass(gpu_cmd, &begin_pass,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(gpu_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline);
      vkCmdBindVertexBuffers(gpu_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(gpu_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(gpu_cmd);
      assert(vkEndCommandBuffer(gpu_cmd) == VK_SUCCESS);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_gpu, gpu_cmd);
      const uint32_t expanded_consumer_dwords = native_gpu->ib_size_dwords;
      const uint32_t window_consumer_dwords =
         native_gpu->window_space_ib_size_dwords;
      assert(native_gpu->window_space_ib != NULL);
      assert(window_consumer_dwords == expanded_consumer_dwords);
      assert(memcmp(native_gpu->window_space_ib, native_gpu->ib,
                    expanded_consumer_dwords * sizeof(uint32_t)) != 0);

      /* The GPU gate alone selects nothing: the route resolver keeps
       * the CPU default, so admission is a no-op and the consumer IB
       * stands unchanged.
       */
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      assert(setenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL", "1", 1) ==
             0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_deferred_draw_admit_gpu_producer(
                native_device, native_gpu) == VK_SUCCESS);
      assert(native_gpu->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE);
      assert(native_gpu->ib_size_dwords == expanded_consumer_dwords);
      assert(native_gpu->window_space_ib_size_dwords ==
             window_consumer_dwords);

      /* The double opt-in admits: the IB becomes producer ++ consumer
       * with the producer prefix byte-identical to the records
       * emission, the carrier relocation gains the write domain, the
       * carrier holds the poison, and the retained oracle is the
       * record dwords plus the pad slot's poison.
       */
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_deferred_draw_admit_gpu_producer(
                native_device, native_gpu) == VK_SUCCESS);
      assert(native_gpu->cell_kind ==
             R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC);
      assert(native_gpu->deferred_draws[0].gpu_producer_delivery);

      struct r300_r2vb_producer_ib expected_producer;
      assert(r300_r2vb_producer_records_emit(
                (const float(*)[4])positive_triangle,
                &expected_producer) == 0);
      assert(native_gpu->deferred_draws[0].gpu_producer_dwords ==
             expected_producer.ib_size_dwords);
      assert(native_gpu->ib_size_dwords ==
             expected_producer.ib_size_dwords + window_consumer_dwords);
      assert(native_gpu->window_space_ib == NULL);
      assert(native_gpu->window_space_ib_size_dwords == 0);
      struct r300_tcl_bypass_triangle_ib module_shape_cell;
      assert(r300_tcl_bypass_triangle_render_shape_emit(
                &module_shape, &module_shape_cell) == 0);
      assert(module_shape_cell.ib_size_dwords == window_consumer_dwords);
      assert(memcmp(native_gpu->ib, expected_producer.ib,
                    expected_producer.ib_size_dwords * 4) == 0);
      assert(native_gpu->references[R300_TRIANGLE_SLOT_VERTEX]
                .write_domain == RADEON_GEM_DOMAIN_GTT);
      r300_r2vb_producer_pass_release(&expected_producer);

      /* The stream an authorization declares is composed offline by
       * r300_r2vb_public_route_compose, and the stream the ioctl
       * carries is composed here at submission time.  An attended run
       * arms on the first and submits the second, so the two
       * compositions are compared dword for dword rather than assumed
       * equal from sharing an emitter.
       */
      struct r300_r2vb_public_route_ib authorized;
      assert(r300_r2vb_public_route_compose(
                (const float(*)[4])positive_triangle,
                R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT,
                &authorized) == 0);
      assert(r300_r2vb_public_route_validate_reloc_sites(&authorized) == 0);
      assert(authorized.ib_size_dwords == native_gpu->ib_size_dwords);
      assert(authorized.consumer_start_dwords ==
             native_gpu->deferred_draws[0].gpu_producer_dwords);
      /* The producer prefix is the offline composition's byte for
       * byte; the consumer slice carries the bound module's fragment
       * constant, so it equals the render-shape cell at the reference
       * geometry with that constant.
       */
      assert(memcmp(authorized.ib, native_gpu->ib,
                    authorized.consumer_start_dwords * 4) == 0);
      assert(memcmp(module_shape_cell.ib,
                    native_gpu->ib + authorized.consumer_start_dwords,
                    module_shape_cell.ib_size_dwords * 4) == 0);
      r300_r2vb_public_route_release(&authorized);
      r300_tcl_bypass_triangle_release(&module_shape_cell);

      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_gpu->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      const uint32_t *carrier_words = carrier_map;
      for (unsigned i = 0; i < 16; i++)
         assert(carrier_words[i] == R300_R2VB_PRODUCER_POISON_DWORD);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_gpu->owned_carriers[0]->bo, carrier_map);
      for (unsigned i = 0; i < 12; i++) {
         uint32_t bits;
         memcpy(&bits, &positive_triangle[i], 4);
         assert(native_gpu->deferred_draws[0].gpu_expected_carrier[i] == bits);
      }
      for (unsigned i = 12; i < 16; i++)
         assert(native_gpu->deferred_draws[0].gpu_expected_carrier[i] ==
                R300_R2VB_PRODUCER_POISON_DWORD);

      /* The deferred execution under an admitted delivery clears the
       * target and leaves the poisoned carrier for the device.
       */
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_gpu) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_gpu->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      assert(((const uint32_t *)carrier_map)[0] ==
             R300_R2VB_PRODUCER_POISON_DWORD);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_gpu->owned_carriers[0]->bo, carrier_map);

      /* A resubmission re-reads the stream: the producer prefix
       * re-emits over the live bytes at the same fixed length.
       */
      float shifted[12];
      memcpy(shifted, positive_triangle, sizeof(shifted));
      shifted[0] = 2.0f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, shifted, sizeof(shifted));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_deferred_draw_admit_gpu_producer(
                native_device, native_gpu) == VK_SUCCESS);
      uint32_t shifted_bits;
      memcpy(&shifted_bits, &shifted[0], 4);
      assert(native_gpu->deferred_draws[0].gpu_expected_carrier[0] ==
             shifted_bits);

      /* The read-back verdict: the unwritten carrier diverges from the
       * oracle, so the verify quarantines the capability and every
       * later admission refuses; a carrier holding the oracle bytes
       * passes once the quarantine is lifted.
       */
      assert(r3v_native_deferred_draw_verify_gpu_producer(
                native_device, native_gpu) == VK_ERROR_DEVICE_LOST);
      assert(native_device->gpu_producer_quarantined);
      assert(r3v_native_deferred_draw_admit_gpu_producer(
                native_device, native_gpu) == VK_ERROR_DEVICE_LOST);
      native_device->gpu_producer_quarantined = false;
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_gpu->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      memcpy(carrier_map, native_gpu->deferred_draws[0].gpu_expected_carrier,
             sizeof(native_gpu->deferred_draws[0].gpu_expected_carrier));
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_gpu->owned_carriers[0]->bo, carrier_map);
      /* Host-model read side: the carrier carries no live mapping here,
       * so the post-completion invalidate over the command buffer's live
       * mappings never reaches it and the read-back itself owns the
       * invalidate.  The event record names the carrier handle, which
       * pins the flush to the range the memcmp reads rather than to any
       * sync the surrounding path happens to perform.
       */
      const uint64_t readback_sync_before =
         native_device->drm.cache_sync_count;
      assert(r3v_native_deferred_draw_verify_gpu_producer(
                native_device, native_gpu) == VK_SUCCESS);
      assert(native_device->drm.cache_sync_count ==
             readback_sync_before + 1);
      assert(native_device->drm.cache_sync_last.bo_handle ==
             native_gpu->owned_carriers[0]->bo.handle);
      assert(!native_device->gpu_producer_quarantined);

      /* The closed hazard gate still refuses the composed submission
       * before any ioctl, after the queue-time admission ran.
       */
      assert(vkQueueSubmit(queue, 1,
                           &(VkSubmitInfo){
                              .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &gpu_cmd,
                           },
                           VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST);

      /* An off-domain record refuses admission by the FP24 window. */
      float off_domain[12];
      memcpy(off_domain, positive_triangle, sizeof(off_domain));
      off_domain[0] = 0.1f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, off_domain, sizeof(off_domain));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_deferred_draw_admit_gpu_producer(
                native_device, native_gpu) != VK_SUCCESS);

      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      assert(unsetenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL") == 0);
      r3v_native_device_refresh_delivery_gates(native_device);

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
      r3v_native_device_refresh_delivery_gates(native_device);

      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, xyz, sizeof(xyz));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_xyz) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_xyz->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_xyz->owned_carriers[0]->bo, carrier_map);

      float positive_xyz[9];
      for (unsigned v = 0; v < 3; v++)
         memcpy(&positive_xyz[v * 3], &positive_triangle[v * 4], 12);
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, positive_xyz, sizeof(positive_xyz));
      vkUnmapMemory(device, vertex_memory);
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_xyz) == VK_SUCCESS);
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      r3v_native_device_refresh_delivery_gates(native_device);

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
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_xy) == VK_SUCCESS);
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_xy->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, r300_tcl_bypass_triangle_vertices,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_xy->owned_carriers[0]->bo, carrier_map);

      float positive_xy[6];
      for (unsigned v = 0; v < 3; v++)
         memcpy(&positive_xy[v * 2], &positive_triangle[v * 4], 8);
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, positive_xy, sizeof(positive_xy));
      vkUnmapMemory(device, vertex_memory);
      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
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
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_xyz) != VK_SUCCESS);

      /* Replay the same F32_3 bytes before staging the F32_2 payload. */
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
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
                                  &native_xyz->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      assert(memcmp(carrier_map, expected_xyz_cpu,
                    R300_TRIANGLE_VERTEX_DWORDS * 4) == 0);
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_xyz->owned_carriers[0]->bo, carrier_map);

      assert(setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1) == 0);
      r3v_native_device_refresh_delivery_gates(native_device);

      float xy_narrow[6];
      memcpy(xy_narrow, positive_xy, sizeof(xy_narrow));
      xy_narrow[0] = 0.1f;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, xy_narrow, sizeof(xy_narrow));
      vkUnmapMemory(device, vertex_memory);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_xy) != VK_SUCCESS);
      assert(unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") == 0);
      r3v_native_device_refresh_delivery_gates(native_device);
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_xy) == VK_SUCCESS);

      vkDestroyPipeline(device, xy_pipeline, NULL);

      /* Restore the reference stream for the legs below. */
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

   /* The render-shape family through the public route: a 48x20 target
    * carries its own eight-pixel-aligned row pitch, its recorded IB is
    * the render-shape emission of exactly that target and the bound
    * pipeline's constant, and the deferred clear covers exactly the
    * declared footprint.
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
      const uint32_t sub_pitch_bytes =
         r3v_native_render_row_pitch_bytes(sub_w);
      assert(sub_pitch_bytes == 192);
      assert(sub_reqs.size == (VkDeviceSize)sub_pitch_bytes * (sub_h + 1));

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

      /* The target leaves the reference pitch, so the draw lowers through
       * the clip-space render-shape emitter carrying the module's own
       * constant; the recorded stream is that emission byte for byte.
       */
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_sub, sub_cmd);
      struct r300_triangle_render_shape sub_render_shape;
      r300_tcl_bypass_triangle_render_shape_reference(&sub_render_shape);
      sub_render_shape.width = sub_w;
      sub_render_shape.height = sub_h;
      sub_render_shape.pitch_pixels = sub_pitch_bytes / 4;
      sub_render_shape.color_bits[0] = 0;
      sub_render_shape.color_bits[1] = 0x3f800000u;
      sub_render_shape.color_bits[2] = 0;
      sub_render_shape.color_bits[3] = 0x3f800000u;
      struct r300_tcl_bypass_triangle_ib sub_expected;
      assert(r300_tcl_bypass_triangle_clip_space_render_shape_emit(
                &sub_render_shape, 1u, &sub_expected) == 0);
      assert(native_sub->ib_size_dwords == sub_expected.ib_size_dwords);
      assert(memcmp(native_sub->ib, sub_expected.ib,
                    sub_expected.ib_size_dwords * sizeof(uint32_t)) == 0);
      r300_tcl_bypass_triangle_release(&sub_expected);

      assert(r3v_native_cmd_buffer_execute_deferred_draws(
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

   /* The widened render family end to end: an R8G8B8A8_UNORM target at
    * the family's maximum extent under VK_IMAGE_TILING_OPTIMAL, with a
    * transfer-source bit beside the attachment bit, bound to a pipeline
    * whose fragment module writes a lattice constant other than the
    * reference one.  The recorded stream is the render-shape emission
    * of exactly that target and constant, and the recorded
    * vkCmdCopyImageToBuffer moves the target's own rows into a buffer.
    * The command stream never reaches the device here, so the copy
    * carries the deferred clear's sentinel.
    */
   {
      const uint32_t wide = R3V_NATIVE_RENDER_MAX_EXTENT;
      VkImageCreateInfo wide_info = image_info;
      wide_info.format = VK_FORMAT_R8G8B8A8_UNORM;
      wide_info.extent.width = wide;
      wide_info.extent.height = wide;
      wide_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      wide_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VkImage wide_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &wide_info, NULL, &wide_image) ==
             VK_SUCCESS);

      const uint32_t wide_pitch_bytes =
         r3v_native_render_row_pitch_bytes(wide);
      assert(wide_pitch_bytes == wide * 4);
      VkMemoryRequirements wide_reqs;
      vkGetImageMemoryRequirements(device, wide_image, &wide_reqs);
      assert(wide_reqs.size == (VkDeviceSize)wide_pitch_bytes * (wide + 1));

      VkDeviceMemory wide_memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = wide_reqs.size,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &wide_memory) == VK_SUCCESS);
      assert(vkBindImageMemory(device, wide_image, wide_memory, 0) ==
             VK_SUCCESS);

      VkImageView wide_view = VK_NULL_HANDLE;
      assert(vkCreateImageView(
                device,
                &(VkImageViewCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                   .image = wide_image,
                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                   .format = VK_FORMAT_R8G8B8A8_UNORM,
                   .subresourceRange =
                      {
                         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                         .levelCount = 1,
                         .layerCount = 1,
                      },
                },
                NULL, &wide_view) == VK_SUCCESS);

      VkRenderPass wide_pass = VK_NULL_HANDLE;
      assert(vkCreateRenderPass(
                device,
                &(VkRenderPassCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                   .attachmentCount = 1,
                   .pAttachments =
                      &(VkAttachmentDescription){
                         .format = VK_FORMAT_R8G8B8A8_UNORM,
                         .samples = VK_SAMPLE_COUNT_1_BIT,
                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
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
                NULL, &wide_pass) == VK_SUCCESS);

      VkFramebuffer wide_framebuffer = VK_NULL_HANDLE;
      assert(vkCreateFramebuffer(
                device,
                &(VkFramebufferCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                   .renderPass = wide_pass,
                   .attachmentCount = 1,
                   .pAttachments = &wide_view,
                   .width = wide,
                   .height = wide,
                   .layers = 1,
                },
                NULL, &wide_framebuffer) == VK_SUCCESS);

      /* The reference module's 1.0 constant carried to 0.5: another
       * value the FP24 lattice holds exactly, so the register word is
       * the value the program named.
       */
      uint32_t half_fs[sizeof(r3v_reference_fragment_spirv) / 4];
      memcpy(half_fs, r3v_reference_fragment_spirv, sizeof(half_fs));
      bool half_constant = false;
      for (size_t word_index = 0; word_index < ARRAY_SIZE(half_fs);
           word_index++) {
         if (half_fs[word_index] == 0x3f800000u) {
            half_fs[word_index] = 0x3f000000u;
            half_constant = true;
            break;
         }
      }
      assert(half_constant);

      struct pipeline_shape wide_pipeline_shape = contract_shape;
      wide_pipeline_shape.fragment_words = half_fs;
      wide_pipeline_shape.fragment_bytes = sizeof(half_fs);
      wide_pipeline_shape.extent_width = wide;
      wide_pipeline_shape.extent_height = wide;
      VkPipeline wide_pipeline = VK_NULL_HANDLE;
      assert(make_pipeline(&wide_pipeline_shape, wide_pass, layout,
                           &wide_pipeline) == VK_SUCCESS);

      VkRenderPassBeginInfo wide_begin = begin_pass;
      wide_begin.renderPass = wide_pass;
      wide_begin.framebuffer = wide_framebuffer;
      wide_begin.renderArea = (VkRect2D){ .extent = { wide, wide } };
      /* Readback through the transfer bit the render family carries:
       * one row of the target lands in a buffer, and the bytes are the
       * deferred clear's sentinel because no device wrote here.  The
       * buffer is created ahead of the recording so the same command
       * buffer can carry a fill before the pass and the readback after
       * it.
       */
      VkDeviceMemory readback_memory = VK_NULL_HANDLE;
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = wide_pitch_bytes,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &readback_memory) == VK_SUCCESS);
      VkBuffer readback_buffer = VK_NULL_HANDLE;
      assert(vkCreateBuffer(device,
                            &(VkBufferCreateInfo){
                               .sType =
                                  VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = wide_pitch_bytes,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            },
                            NULL, &readback_buffer) == VK_SUCCESS);
      assert(vkBindBufferMemory(device, readback_buffer, readback_memory,
                                0) == VK_SUCCESS);

      /* One draw-carrying command buffer holds both groups: the fill
       * ahead of the pass and the render-target readback after it.  The
       * groups execute in the queue's order below, so the readback row
       * carries the clear the draw published rather than the fill
       * pattern a reversed order would leave.
       */
      const uint32_t fill_pattern = 0xdeadbeefu;
      VkCommandBuffer wide_cmd = fresh_cmd();
      vkCmdFillBuffer(wide_cmd, readback_buffer, 0, VK_WHOLE_SIZE,
                      fill_pattern);
      vkCmdBeginRenderPass(wide_cmd, &wide_begin,
                           VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(wide_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        wide_pipeline);
      vkCmdBindVertexBuffers(wide_cmd, 0, 1, &vertex_buffer,
                             &(VkDeviceSize){ 0 });
      vkCmdDraw(wide_cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(wide_cmd);
      vkCmdCopyImageToBuffer(
         wide_cmd, wide_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         readback_buffer, 1,
         &(VkBufferImageCopy){
            .imageSubresource =
               {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .layerCount = 1,
               },
            .imageExtent = { wide, 1, 1 },
         });
      assert(vkEndCommandBuffer(wide_cmd) == VK_SUCCESS);

      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_wide, wide_cmd);
      struct r300_triangle_render_shape wide_shape;
      r300_tcl_bypass_triangle_render_shape_reference(&wide_shape);
      wide_shape.width = wide;
      wide_shape.height = wide;
      wide_shape.pitch_pixels = wide_pitch_bytes / 4;
      wide_shape.lanes = R300_TRIANGLE_LANES_R8G8B8A8;
      wide_shape.color_bits[0] = 0;
      wide_shape.color_bits[1] = 0x3f000000u;
      wide_shape.color_bits[2] = 0;
      wide_shape.color_bits[3] = 0x3f000000u;
      struct r300_tcl_bypass_triangle_ib wide_expected;
      assert(r300_tcl_bypass_triangle_clip_space_render_shape_emit(
                &wide_shape, 1u, &wide_expected) == 0);
      assert(native_wide->ib_size_dwords == wide_expected.ib_size_dwords);
      assert(memcmp(native_wide->ib, wide_expected.ib,
                    wide_expected.ib_size_dwords * sizeof(uint32_t)) == 0);
      r300_tcl_bypass_triangle_release(&wide_expected);

      /* Copies are host-only, so the recorded IB is the clip-space cell the
       * render shape emits whichever group the buffer carries.
       */
      assert(native_wide->deferred_copy_count == 2);
      assert(native_wide->deferred_copies[0].group ==
             R3V_NATIVE_COPY_GROUP_BEFORE_DRAW);
      assert(native_wide->deferred_copies[1].group ==
             R3V_NATIVE_COPY_GROUP_AFTER_DRAW);

      /* The queue's order on the transport path: pre-draw group, the
       * deferred draw, post-draw group.
       */
      assert(r3v_native_cmd_buffer_execute_deferred_copies(
                native_device, native_wide,
                R3V_NATIVE_COPY_GROUP_BEFORE_DRAW) == VK_SUCCESS);
      uint32_t *fill_map = NULL;
      assert(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&fill_map) == VK_SUCCESS);
      assert(fill_map[0] == fill_pattern);
      vkUnmapMemory(device, readback_memory);

      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_wide) == VK_SUCCESS);

      assert(r3v_native_cmd_buffer_execute_deferred_copies(
                native_device, native_wide,
                R3V_NATIVE_COPY_GROUP_AFTER_DRAW) == VK_SUCCESS);

      uint32_t *readback_map = NULL;
      assert(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&readback_map) == VK_SUCCESS);
      assert(readback_map[0] == R300_TRIANGLE_COLOR_SENTINEL);
      assert(readback_map[wide - 1] == R300_TRIANGLE_COLOR_SENTINEL);
      vkUnmapMemory(device, readback_memory);

      vkDestroyBuffer(device, readback_buffer, NULL);
      vkFreeMemory(device, readback_memory, NULL);
      vkDestroyPipeline(device, wide_pipeline, NULL);
      vkDestroyFramebuffer(device, wide_framebuffer, NULL);
      vkDestroyRenderPass(device, wide_pass, NULL);
      vkDestroyImageView(device, wide_view, NULL);
      vkDestroyImage(device, wide_image, NULL);
      vkFreeMemory(device, wide_memory, NULL);
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

   /* An off-lattice fragment constant refuses: the R300_PFS_PARAM_0
    * register encodes FP24, so a pattern with mantissa bits below the
    * lattice would round in the register and render a value the
    * program never named.  0x3e000001 carries one such bit;
    * r300_fp24_quantize_bits clears it, so the admission rejects the
    * module.
    */
   uint32_t mutated_fs[sizeof(r3v_reference_fragment_spirv) / 4];
   memcpy(mutated_fs, r3v_reference_fragment_spirv, sizeof(mutated_fs));
   bool mutated_off_lattice_constant = false;
   for (size_t word_index = 0; word_index < ARRAY_SIZE(mutated_fs);
        word_index++) {
      if (mutated_fs[word_index] == 0x3f800000u) {
         mutated_fs[word_index] = 0x3e000001u;
         mutated_off_lattice_constant = true;
         break;
      }
   }
   assert(mutated_off_lattice_constant);
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
   bad_image_info.extent.width = R3V_NATIVE_RENDER_MAX_EXTENT + 1;
   assert(vkCreateImage(device, &bad_image_info, NULL, &bad_image) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          bad_image == VK_NULL_HANDLE);
   bad_image_info = image_info;
   bad_image_info.extent.height = 0;
   assert(vkCreateImage(device, &bad_image_info, NULL, &bad_image) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          bad_image == VK_NULL_HANDLE);
   bad_image_info.extent.height = R3V_NATIVE_RENDER_MAX_EXTENT + 1;
   assert(vkCreateImage(device, &bad_image_info, NULL, &bad_image) ==
             R3V_NATIVE_REFUSAL_RESULT &&
          bad_image == VK_NULL_HANDLE);

   /* The render family binds at any page-aligned offset whose footprint
    * closes inside the allocation: the offset travels as the cell's
    * RB3D_COLOROFFSET0 payload and as the base of the host clear.  An
    * offset whose footprint overruns the allocation refuses, and the
    * image stays unbound for the layout-transition refusal below.
    */
   {
      VkImage offset_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &image_info, NULL, &offset_image) ==
             VK_SUCCESS);
      VkDeviceMemory offset_memory = VK_NULL_HANDLE;
      VkMemoryAllocateInfo offset_alloc = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = 4096 + reqs.size,
         .memoryTypeIndex = 0,
      };
      assert(vkAllocateMemory(device, &offset_alloc, NULL, &offset_memory) ==
             VK_SUCCESS);
      assert(vkBindImageMemory(device, offset_image, offset_memory, 4096) ==
             VK_SUCCESS);
      vkDestroyImage(device, offset_image, NULL);
      vkFreeMemory(device, offset_memory, NULL);
   }

   VkImage unbound_image = VK_NULL_HANDLE;
   assert(vkCreateImage(device, &image_info, NULL, &unbound_image) ==
          VK_SUCCESS);
   assert(vkBindImageMemory(device, unbound_image, color_memory, 8192) ==
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

      /* The render family takes the transfer and sampled bits beside
       * the attachment bit: the copies execute over its row pitch and
       * the TX block fetches the same rows.  A usage naming anything
       * else refuses.
       */
      VkImageCreateInfo mixed_info = image_info;
      mixed_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VkImage mixed_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &mixed_info, NULL, &mixed_image) ==
                VK_SUCCESS &&
             mixed_image != VK_NULL_HANDLE);
      vkDestroyImage(device, mixed_image, NULL);

      mixed_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT;
      assert(vkCreateImage(device, &mixed_info, NULL, &mixed_image) ==
                VK_SUCCESS &&
             mixed_image != VK_NULL_HANDLE);
      vkDestroyImage(device, mixed_image, NULL);

      mixed_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_STORAGE_BIT;
      assert(vkCreateImage(device, &mixed_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      /* An array image stacks its layers at one stride, and a view
       * selects a single layer at any index.  The subresource layout
       * reports that stride, and the layer count past the reported
       * maximum, a layered view, and the array view type each refuse.
       */
      VkImageCreateInfo layered_info = image_info;
      layered_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT;
      layered_info.arrayLayers = 12;
      VkImage layered_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &layered_info, NULL, &layered_image) ==
                VK_SUCCESS &&
             layered_image != VK_NULL_HANDLE);

      VkMemoryRequirements layered_requirements;
      vkGetImageMemoryRequirements(device, layered_image,
                                   &layered_requirements);
      VkSubresourceLayout layered_layout;
      const VkImageSubresource layered_subresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0,
         .arrayLayer = 5,
      };
      vkGetImageSubresourceLayout(device, layered_image,
                                  &layered_subresource, &layered_layout);
      assert(layered_layout.arrayPitch != 0);
      assert(layered_layout.offset == 5 * layered_layout.arrayPitch);
      assert(layered_requirements.size == 12 * layered_layout.arrayPitch);

      VkImageViewCreateInfo layered_view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = layered_image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = layered_info.format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .baseArrayLayer = 5,
            .layerCount = 1,
         },
      };
      VkImageView layered_view = VK_NULL_HANDLE;
      assert(vkCreateImageView(device, &layered_view_info, NULL,
                               &layered_view) == VK_SUCCESS &&
             layered_view != VK_NULL_HANDLE);
      vkDestroyImageView(device, layered_view, NULL);

      /* A one-slice view type resolves its slice at creation, so more
       * than one layer under VK_IMAGE_VIEW_TYPE_2D names a slice the
       * stride cannot reach and refuses.
       */
      layered_view_info.subresourceRange.layerCount = 2;
      assert(vkCreateImageView(device, &layered_view_info, NULL,
                               &layered_view) == R3V_NATIVE_REFUSAL_RESULT &&
             layered_view == VK_NULL_HANDLE);

      /* The array view types create and destroy over the layers the
       * image holds.  They name a coordinate axis no cell indexes, so
       * the executing routes refuse them and creation does not; the
       * refusal below at vkCmdBeginRenderPass is where that lands.
       */
      layered_view_info.subresourceRange.layerCount = 1;
      layered_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      assert(vkCreateImageView(device, &layered_view_info, NULL,
                               &layered_view) == VK_SUCCESS &&
             layered_view != VK_NULL_HANDLE);
      vkDestroyImageView(device, layered_view, NULL);

      layered_view_info.subresourceRange.baseArrayLayer = 0;
      layered_view_info.subresourceRange.layerCount = 12;
      assert(vkCreateImageView(device, &layered_view_info, NULL,
                               &layered_view) == VK_SUCCESS &&
             layered_view != VK_NULL_HANDLE);
      vkDestroyImageView(device, layered_view, NULL);

      layered_view_info.subresourceRange.baseArrayLayer = 5;
      layered_view_info.subresourceRange.layerCount = 1;
      layered_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      layered_view_info.subresourceRange.baseArrayLayer = 12;
      assert(vkCreateImageView(device, &layered_view_info, NULL,
                               &layered_view) == R3V_NATIVE_REFUSAL_RESULT &&
             layered_view == VK_NULL_HANDLE);

      /* A layer range past the image's own layers refuses whatever the
       * view type: the stride reaches no such slice.
       */
      layered_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      layered_view_info.subresourceRange.baseArrayLayer = 8;
      layered_view_info.subresourceRange.layerCount = 8;
      assert(vkCreateImageView(device, &layered_view_info, NULL,
                               &layered_view) == R3V_NATIVE_REFUSAL_RESULT &&
             layered_view == VK_NULL_HANDLE);
      vkDestroyImage(device, layered_image, NULL);
      /* Downstream fixtures inherit this range, so it returns to the
       * one-slice shape the 2D view type resolves.
       */
      layered_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      layered_view_info.subresourceRange.baseArrayLayer = 0;
      layered_view_info.subresourceRange.layerCount = 1;

      /* A volume image stacks its depth slices at the layer stride, so
       * the footprint the requirement reports is that stride times the
       * depth, and the 3D view type names the whole volume.  The
       * attachment route has no volume binding, so the color-attachment
       * usage refuses and the sampling and transfer usages admit.
       */
      VkImageCreateInfo volume_info = image_info;
      volume_info.imageType = VK_IMAGE_TYPE_3D;
      volume_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      volume_info.extent.depth = 4;
      volume_info.arrayLayers = 1;
      VkImage volume_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &volume_info, NULL, &volume_image) ==
                VK_SUCCESS &&
             volume_image != VK_NULL_HANDLE);
      VkMemoryRequirements volume_requirements;
      vkGetImageMemoryRequirements(device, volume_image,
                                   &volume_requirements);
      VkSubresourceLayout volume_layout;
      vkGetImageSubresourceLayout(
         device, volume_image,
         &(VkImageSubresource){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT },
         &volume_layout);
      assert(volume_layout.depthPitch != 0 && volume_layout.arrayPitch == 0);
      assert(volume_requirements.size == 4 * volume_layout.depthPitch);

      VkImageView volume_view = VK_NULL_HANDLE;
      VkImageViewCreateInfo volume_view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = volume_image,
         .viewType = VK_IMAGE_VIEW_TYPE_3D,
         .format = volume_info.format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
         },
      };
      assert(vkCreateImageView(device, &volume_view_info, NULL,
                               &volume_view) == VK_SUCCESS &&
             volume_view != VK_NULL_HANDLE);
      vkDestroyImageView(device, volume_view, NULL);

      /* A 2D view of a volume image names a slice the type cannot
       * resolve, and a 3D view of a 2D image names an axis the image
       * does not carry; both refuse.
       */
      volume_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      assert(vkCreateImageView(device, &volume_view_info, NULL,
                               &volume_view) == R3V_NATIVE_REFUSAL_RESULT &&
             volume_view == VK_NULL_HANDLE);
      vkDestroyImage(device, volume_image, NULL);

      volume_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      assert(vkCreateImage(device, &volume_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      /* VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT takes a square 2D image with
       * six layers per cube; the cube view types name whole cubes.
       */
      VkImageCreateInfo cube_info = image_info;
      cube_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
      cube_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      cube_info.extent.width = 64;
      cube_info.extent.height = 64;
      cube_info.arrayLayers = 12;
      VkImage cube_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &cube_info, NULL, &cube_image) ==
                VK_SUCCESS &&
             cube_image != VK_NULL_HANDLE);

      VkImageView cube_view = VK_NULL_HANDLE;
      VkImageViewCreateInfo cube_view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = cube_image,
         .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
         .format = cube_info.format,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 6,
         },
      };
      assert(vkCreateImageView(device, &cube_view_info, NULL, &cube_view) ==
                VK_SUCCESS &&
             cube_view != VK_NULL_HANDLE);
      vkDestroyImageView(device, cube_view, NULL);

      /* The object_management image_view_cube cases build the cube
       * image with SAMPLED | COLOR_ATTACHMENT usage
       * (vktApiObjectManagementTests.cpp imgCube), which routes creation
       * through the attachment family; the flag admits there over the
       * same array layout, while the alias flag keeps its refusal.
       */
      VkImageCreateInfo cube_attachment_info = cube_info;
      cube_attachment_info.usage =
         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      VkImage cube_attachment_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &cube_attachment_info, NULL,
                           &cube_attachment_image) == VK_SUCCESS &&
             cube_attachment_image != VK_NULL_HANDLE);
      vkDestroyImage(device, cube_attachment_image, NULL);
      cube_attachment_info.flags = VK_IMAGE_CREATE_ALIAS_BIT;
      cube_attachment_info.arrayLayers = 1;
      assert(vkCreateImage(device, &cube_attachment_info, NULL,
                           &refused_image) == R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      cube_view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
      cube_view_info.subresourceRange.layerCount = 12;
      assert(vkCreateImageView(device, &cube_view_info, NULL, &cube_view) ==
                VK_SUCCESS &&
             cube_view != VK_NULL_HANDLE);
      vkDestroyImageView(device, cube_view, NULL);

      /* A cube view over five faces is not a cube. */
      cube_view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
      cube_view_info.subresourceRange.layerCount = 5;
      assert(vkCreateImageView(device, &cube_view_info, NULL, &cube_view) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             cube_view == VK_NULL_HANDLE);
      vkDestroyImage(device, cube_image, NULL);

      /* A non-square extent and a layer count outside whole cubes each
       * refuse the flag at creation.
       */
      cube_info.extent.height = 32;
      assert(vkCreateImage(device, &cube_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);
      cube_info.extent.height = 64;
      cube_info.arrayLayers = 7;
      assert(vkCreateImage(device, &cube_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      /* The render family's layer count answers to the cell's
       * RB3D_COLOROFFSET0 ceiling, which is below the device limit the
       * sampling family reaches.
       */
      layered_info.arrayLayers = R3V_NATIVE_RENDER_MAX_ARRAY_LAYERS;
      assert(vkCreateImage(device, &layered_info, NULL, &layered_image) ==
                VK_SUCCESS);
      vkDestroyImage(device, layered_image, NULL);

      layered_info.arrayLayers = R3V_NATIVE_RENDER_MAX_ARRAY_LAYERS + 1;
      assert(vkCreateImage(device, &layered_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      /* The sampling family reaches the device limit, since TX_OFFSET_0
       * carries the full span. */
      VkImageCreateInfo sampled_layers_info = layered_info;
      sampled_layers_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      sampled_layers_info.arrayLayers = R3V_NATIVE_MAX_ARRAY_LAYERS;
      assert(vkCreateImage(device, &sampled_layers_info, NULL,
                           &layered_image) == VK_SUCCESS);
      vkDestroyImage(device, layered_image, NULL);

      sampled_layers_info.arrayLayers = R3V_NATIVE_MAX_ARRAY_LAYERS + 1;
      assert(vkCreateImage(device, &sampled_layers_info, NULL,
                           &refused_image) == R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      /* The 1D type is the height-one member of the same layout; its
       * views take VK_IMAGE_VIEW_TYPE_1D, and a height past one has no
       * 1D image to describe.
       */
      VkImageCreateInfo one_d_info = image_info;
      one_d_info.imageType = VK_IMAGE_TYPE_1D;
      one_d_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      one_d_info.extent = (VkExtent3D){ 256, 1, 1 };
      one_d_info.arrayLayers = 4;
      VkImage one_d_image = VK_NULL_HANDLE;
      assert(vkCreateImage(device, &one_d_info, NULL, &one_d_image) ==
                VK_SUCCESS &&
             one_d_image != VK_NULL_HANDLE);

      VkImageViewCreateInfo one_d_view_info = layered_view_info;
      one_d_view_info.image = one_d_image;
      one_d_view_info.viewType = VK_IMAGE_VIEW_TYPE_1D;
      one_d_view_info.format = one_d_info.format;
      one_d_view_info.subresourceRange.baseArrayLayer = 3;
      VkImageView one_d_view = VK_NULL_HANDLE;
      assert(vkCreateImageView(device, &one_d_view_info, NULL,
                               &one_d_view) == VK_SUCCESS &&
             one_d_view != VK_NULL_HANDLE);
      vkDestroyImageView(device, one_d_view, NULL);

      one_d_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      assert(vkCreateImageView(device, &one_d_view_info, NULL, &one_d_view) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             one_d_view == VK_NULL_HANDLE);
      vkDestroyImage(device, one_d_image, NULL);

      one_d_info.extent.height = 2;
      assert(vkCreateImage(device, &one_d_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);

      /* A volume image carries its depth on the depth axis alone:
       * Vulkan holds arrayLayers at one for VK_IMAGE_TYPE_3D
       * (VUID-VkImageCreateInfo-imageType-00961), and a 2D image with a
       * depth past one has no volume to describe.
       */
      VkImageCreateInfo three_d_info = image_info;
      three_d_info.imageType = VK_IMAGE_TYPE_3D;
      three_d_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      three_d_info.extent = (VkExtent3D){ 64, 64, 4 };
      three_d_info.arrayLayers = 2;
      assert(vkCreateImage(device, &three_d_info, NULL, &refused_image) ==
                R3V_NATIVE_REFUSAL_RESULT &&
             refused_image == VK_NULL_HANDLE);
      three_d_info.imageType = VK_IMAGE_TYPE_2D;
      three_d_info.arrayLayers = 1;
      assert(vkCreateImage(device, &three_d_info, NULL, &refused_image) ==
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
   /* The load-op clear reads pClearValues[0], so a pass declaring no
    * clear value refuses ahead of that read.
    */
   VkCommandBuffer bad_cmd = fresh_cmd();
   VkRenderPassBeginInfo no_clear_value = begin_pass;
   no_clear_value.clearValueCount = 0;
   no_clear_value.pClearValues = NULL;
   vkCmdBeginRenderPass(bad_cmd, &no_clear_value, VK_SUBPASS_CONTENTS_INLINE);
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
