/* SPDX-License-Identifier: MIT */

/* The public Vulkan object fixture supplies image, pipeline, and dispatch
 * construction.  The logical oracle and lifecycle commands remain local to
 * the compiled test translation unit. */
int r3v_native_attended_public_zb_depth_fixture_main(int argc, char **argv);
#define main r3v_native_attended_public_zb_depth_fixture_main
#include "r3v_native_attended_public_zb_depth.c"
#undef main

#include <assert.h>

#define UPDATE_X 0u
#define UPDATE_Y 0u
#define UPDATE_WIDTH 16u
#define UPDATE_HEIGHT 20u
#define CLEAR_DEPTH_CODE 0x800000u
#define UPDATE_DEPTH_CODE 0x400000u
#define CLEAR_STENCIL 0x5au

struct readback {
   VkBuffer buffer;
   VkDeviceMemory memory;
};

struct partial_draw {
   VkRenderPass render_pass;
   VkFramebuffer framebuffer;
   VkPipeline pipeline;
};

struct lifecycle_operation_indices {
   uint32_t fast_clear;
   uint32_t materialize;
   uint32_t draw;
   uint32_t copy;
};

struct lifecycle_results {
   bool ordinary_control;
   bool copy_materialization;
   bool draw_materialization;
   bool rollback;
   bool hardware;
};

static bool
upload_partial_triangle(struct public_context *context)
{
   static const float vertices[12] = {
      -1.0f, -1.0f, 0.25f, 1.0f,
      3.0f,  -1.0f, 0.25f, 1.0f,
      -1.0f, 3.0f,  0.25f, 1.0f,
   };
   void *mapped = NULL;
   if (context->api.map_memory(context->device, context->vertex_memory, 0u,
                               VK_WHOLE_SIZE, 0u, &mapped) != VK_SUCCESS)
      return false;
   memcpy(mapped, vertices, sizeof(vertices));
   context->api.unmap_memory(context->device, context->vertex_memory);
   return true;
}

static bool
create_partial_draw(struct public_context *context,
                    struct partial_draw *partial)
{
   const VkAttachmentDescription attachment = {
      .format = VK_FORMAT_D24_UNORM_S8_UINT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
      .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   const VkAttachmentReference depth_reference = {
      .attachment = 0u,
      .layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkResult result = context->api.create_render_pass(
      context->device,
      &(VkRenderPassCreateInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1u,
         .pAttachments = &attachment,
         .subpassCount = 1u,
         .pSubpasses = &(VkSubpassDescription){
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pDepthStencilAttachment = &depth_reference,
         },
      },
      NULL, &partial->render_pass);
   if (result == VK_SUCCESS)
      result = context->api.create_framebuffer(
         context->device,
         &(VkFramebufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = partial->render_pass,
            .attachmentCount = 1u,
            .pAttachments = &context->depth[0].view,
            .width = UPDATE_WIDTH,
            .height = UPDATE_HEIGHT,
            .layers = 1u,
         },
         NULL, &partial->framebuffer);
   VkShaderModule vertex_shader = VK_NULL_HANDLE;
   VkShaderModule fragment_shader = VK_NULL_HANDLE;
   if (result == VK_SUCCESS)
      result = context->api.create_shader_module(
         context->device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(r3v_reference_vertex_spirv),
            .pCode = r3v_reference_vertex_spirv,
         },
         NULL, &vertex_shader);
   if (result == VK_SUCCESS)
      result = context->api.create_shader_module(
         context->device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(r3v_reference_fragment_spirv),
            .pCode = r3v_reference_fragment_spirv,
         },
         NULL, &fragment_shader);
   const VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vertex_shader,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fragment_shader,
       .pName = "main"},
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
   if (result == VK_SUCCESS)
      result = context->api.create_graphics_pipelines(
         context->device, VK_NULL_HANDLE, 1u,
         &(VkGraphicsPipelineCreateInfo){
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
                  .width = UPDATE_WIDTH,
                  .height = UPDATE_HEIGHT,
                  .maxDepth = 1.0f,
               },
               .scissorCount = 1u,
               .pScissors = &(VkRect2D){.extent = {UPDATE_WIDTH, UPDATE_HEIGHT}},
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
               .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            },
            .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
               .depthTestEnable = VK_TRUE,
               .depthWriteEnable = VK_TRUE,
               .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            },
            .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
               .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            },
            .layout = context->pipeline_layout,
            .renderPass = partial->render_pass,
         },
         NULL, &partial->pipeline);
   if (fragment_shader != VK_NULL_HANDLE)
      context->api.destroy_shader_module(context->device, fragment_shader,
                                         NULL);
   if (vertex_shader != VK_NULL_HANDLE)
      context->api.destroy_shader_module(context->device, vertex_shader, NULL);
   return result == VK_SUCCESS;
}

static void
destroy_partial_draw(struct public_context *context,
                     struct partial_draw *partial)
{
   if (partial->pipeline != VK_NULL_HANDLE)
      context->api.destroy_pipeline(context->device, partial->pipeline, NULL);
   if (partial->framebuffer != VK_NULL_HANDLE)
      context->api.destroy_framebuffer(context->device, partial->framebuffer,
                                      NULL);
   if (partial->render_pass != VK_NULL_HANDLE)
      context->api.destroy_render_pass(context->device, partial->render_pass,
                                      NULL);
}

static void
build_expected_words(uint32_t words[PIXELS], bool apply_update)
{
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const bool updated = apply_update && x >= UPDATE_X &&
                              x < UPDATE_X + UPDATE_WIDTH && y >= UPDATE_Y &&
                              y < UPDATE_Y + UPDATE_HEIGHT;
         const uint32_t depth =
            updated ? UPDATE_DEPTH_CODE : CLEAR_DEPTH_CODE;
         words[y * 64u + x] = (depth << 8) | CLEAR_STENCIL;
      }
   }
}

static void
check_expected_words(const uint32_t words[PIXELS], bool apply_update)
{
   uint32_t updated_count = 0u;
   uint32_t untouched_count = 0u;
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const uint32_t word = words[y * 64u + x];
         const bool updated = x >= UPDATE_X && x < UPDATE_X + UPDATE_WIDTH &&
                              y >= UPDATE_Y &&
                              y < UPDATE_Y + UPDATE_HEIGHT;
         assert((word & UINT8_MAX) == CLEAR_STENCIL);
         assert((word >> 8) ==
                (apply_update && updated ? UPDATE_DEPTH_CODE
                                         : CLEAR_DEPTH_CODE));
         updated_count += apply_update && updated;
         untouched_count += !(apply_update && updated);
      }
   }
   assert(updated_count == (apply_update ? UPDATE_WIDTH * UPDATE_HEIGHT : 0u));
   assert(untouched_count ==
          PIXELS - (apply_update ? UPDATE_WIDTH * UPDATE_HEIGHT : 0u));
}

static bool
create_readback(struct public_context *context, struct readback *readback)
{
   VkResult result = context->api.create_buffer(
      context->device,
      &(VkBufferCreateInfo){
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = PIXELS * sizeof(uint32_t),
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      },
      NULL, &readback->buffer);
   if (result != VK_SUCCESS)
      return false;
   result = context->api.allocate_memory(
      context->device,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = PIXELS * sizeof(uint32_t),
         .memoryTypeIndex = 0u,
      },
      NULL, &readback->memory);
   return result == VK_SUCCESS &&
          context->api.bind_buffer_memory(context->device, readback->buffer,
                                          readback->memory, 0u) == VK_SUCCESS;
}

static void
destroy_readback(struct public_context *context, struct readback *readback)
{
   if (readback->buffer != VK_NULL_HANDLE)
      context->api.destroy_buffer(context->device, readback->buffer, NULL);
   if (readback->memory != VK_NULL_HANDLE)
      context->api.free_memory(context->device, readback->memory, NULL);
}

static PFN_vkCmdCopyImageToBuffer
load_copy_image_to_buffer(struct public_context *context)
{
   PFN_vkGetDeviceProcAddr get_device_proc_addr =
      (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(
         context->instance, "vkGetDeviceProcAddr");
   return get_device_proc_addr != NULL
             ? (PFN_vkCmdCopyImageToBuffer)get_device_proc_addr(
                  context->device, "vkCmdCopyImageToBuffer")
             : NULL;
}

static VkCommandBuffer
begin_command(struct public_context *context)
{
   VkCommandBuffer command = VK_NULL_HANDLE;
   VkResult result = context->api.allocate_command_buffers(
      context->device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = context->command_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1u,
      },
      &command);
   if (result == VK_SUCCESS)
      result = context->api.begin_command_buffer(
         command,
         &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         });
   return result == VK_SUCCESS ? command : VK_NULL_HANDLE;
}

static const VkImageSubresourceRange packed_range = {
   .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
   .levelCount = 1u,
   .layerCount = 1u,
};

static void
record_initial_clear(struct public_context *context, VkCommandBuffer command,
                     VkImage image)
{
   const VkImageMemoryBarrier initial = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = packed_range,
   };
   context->api.cmd_pipeline_barrier(
      command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &initial);
   context->api.cmd_clear_depth_stencil_image(
      command, image, VK_IMAGE_LAYOUT_GENERAL,
      &(VkClearDepthStencilValue){.depth = 0.5f, .stencil = 0x15au}, 1u,
      &packed_range);
}

static void
record_export(struct public_context *context, VkCommandBuffer command,
              VkImage image, VkBuffer buffer,
              PFN_vkCmdCopyImageToBuffer copy_image_to_buffer,
              VkPipelineStageFlags producer_stage,
              VkAccessFlags producer_access)
{
   const VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = producer_access,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = packed_range,
   };
   context->api.cmd_pipeline_barrier(
      command, producer_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL,
      0u, NULL, 1u, &barrier);
   copy_image_to_buffer(
      command, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1u,
      &(VkBufferImageCopy){
         .bufferRowLength = 64u,
         .bufferImageHeight = 64u,
         .imageSubresource =
            {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .layerCount = 1u},
         .imageExtent = {64u, 64u, 1u},
      });
}

static struct lifecycle_operation_indices
find_lifecycle_operations(const struct r3v_native_cmd_buffer *native)
{
   struct lifecycle_operation_indices indices = {
      .fast_clear = UINT32_MAX,
      .materialize = UINT32_MAX,
      .draw = UINT32_MAX,
      .copy = UINT32_MAX,
   };
   for (uint32_t index = 0u; index < native->ordered_operation_count; index++) {
      const struct r3v_native_ordered_operation *operation =
         &native->ordered_operations[index];
      if (operation->kind == R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR)
         indices.fast_clear = index;
      else if (operation->kind ==
               R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE)
         indices.materialize = index;
      else if (operation->kind == R3V_NATIVE_ORDERED_OPERATION_DRAW)
         indices.draw = index;
      else if (operation->kind == R3V_NATIVE_ORDERED_OPERATION_RB2D_COPY)
         indices.copy = index;
   }
   return indices;
}

static bool
lifecycle_order_valid(const struct r3v_native_cmd_buffer *native,
                      const struct lifecycle_operation_indices *indices,
                      bool require_draw)
{
   const bool base_order =
      indices->fast_clear != UINT32_MAX && indices->materialize != UINT32_MAX &&
      indices->copy != UINT32_MAX &&
      indices->fast_clear < indices->materialize &&
      indices->materialize < indices->copy;
   if (!base_order) {
      fprintf(stderr,
              "ordered lifecycle missing: operations=%u clear=%u materialize=%u draw=%u copy=%u\n",
              native->ordered_operation_count, indices->fast_clear,
              indices->materialize, indices->draw, indices->copy);
      return false;
   }
   return !require_draw ||
          (indices->draw != UINT32_MAX &&
           indices->materialize < indices->draw && indices->draw < indices->copy);
}

static bool
fast_clear_payload_valid(
   const struct r3v_native_cmd_buffer *native,
   const struct lifecycle_operation_indices *indices)
{
   const struct r3v_native_ordered_operation *fast_clear =
      &native->ordered_operations[indices->fast_clear];
   return fast_clear->payload.image_fast_clear.authority ==
             R3V_NATIVE_ZMASK_FAST_CLEAR_AUTOMATIC &&
          fast_clear->payload.image_fast_clear.resulting_metadata
                .clear_depth_code == CLEAR_DEPTH_CODE &&
          fast_clear->payload.image_fast_clear.resulting_metadata
                .clear_stencil == CLEAR_STENCIL;
}

static bool
copy_segments_valid(const struct r3v_native_cmd_buffer *native)
{
   if (native->rb2d_copy_operation_count == 0u)
      return false;
   const struct r3v_native_rb2d_copy_operation *copy =
      &native->rb2d_copy_operations[native->rb2d_copy_operation_count - 1u];
   for (uint32_t index = 0u; index < copy->segment_count; index++) {
      const struct r300_rb2d_copy_segment *segment = &copy->segments[index];
      if (segment->source_offset_bytes > copy->source_buffer_bytes ||
          segment->byte_count >
             copy->source_buffer_bytes - segment->source_offset_bytes ||
          segment->destination_offset_bytes > copy->destination_buffer_bytes ||
          segment->byte_count >
             copy->destination_buffer_bytes - segment->destination_offset_bytes)
         return false;
   }
   return true;
}

static bool
ordered_lifecycle_valid(VkCommandBuffer command, bool require_draw)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, command);
   const struct lifecycle_operation_indices indices =
      find_lifecycle_operations(native);
   return lifecycle_order_valid(native, &indices, require_draw) &&
          fast_clear_payload_valid(native, &indices) &&
          copy_segments_valid(native) && native->image_state_count == 1u &&
          native->image_states[0].current_representation ==
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED &&
          native->image_states[0].current_zmask_metadata.status ==
             R3V_NATIVE_ZMASK_METADATA_RETIRED;
}

static bool
record_ordinary_control(struct public_context *context)
{
   VkCommandBuffer command = begin_command(context);
   if (command == VK_NULL_HANDLE)
      return false;
   record_initial_clear(context, command, context->depth[0].image);
   if (context->api.end_command_buffer(command) != VK_SUCCESS)
      return false;
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, command);
   bool ordinary_clear = false;
   for (uint32_t index = 0u; index < native->ordered_operation_count; index++) {
      const enum r3v_native_ordered_operation_kind kind =
         native->ordered_operations[index].kind;
      if (kind == R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR)
         return false;
      ordinary_clear |= kind == R3V_NATIVE_ORDERED_OPERATION_RB2D_DEPTH_CLEAR;
   }
   return ordinary_clear;
}

static bool
record_copy_materialization(struct public_context *context,
                            const struct readback *readback,
                            PFN_vkCmdCopyImageToBuffer copy_image_to_buffer,
                            VkCommandBuffer *command_out)
{
   VkCommandBuffer command = begin_command(context);
   if (command == VK_NULL_HANDLE)
      return false;
   record_initial_clear(context, command, context->depth[0].image);
   record_export(context, command, context->depth[0].image, readback->buffer,
                 copy_image_to_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
   const bool valid = context->api.end_command_buffer(command) == VK_SUCCESS &&
                      ordered_lifecycle_valid(command, false);
   if (valid)
      *command_out = command;
   return valid;
}

static bool
record_draw_materialization(struct public_context *context,
                            const struct partial_draw *partial,
                            const struct readback *readback,
                            PFN_vkCmdCopyImageToBuffer copy_image_to_buffer,
                            VkCommandBuffer *command_out)
{
   VkCommandBuffer command = begin_command(context);
   if (command == VK_NULL_HANDLE)
      return false;
   record_initial_clear(context, command, context->depth[0].image);
   const VkImageMemoryBarrier clear_to_depth = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = context->depth[0].image,
      .subresourceRange = packed_range,
   };
   context->api.cmd_pipeline_barrier(
      command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      0u, 0u, NULL, 0u, NULL, 1u, &clear_to_depth);
   context->api.cmd_begin_render_pass(
      command,
      &(VkRenderPassBeginInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = partial->render_pass,
         .framebuffer = partial->framebuffer,
         .renderArea = {{0, 0}, {UPDATE_WIDTH, UPDATE_HEIGHT}},
      },
      VK_SUBPASS_CONTENTS_INLINE);
   context->api.cmd_bind_pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  partial->pipeline);
   context->api.cmd_bind_vertex_buffers(
      command, 0u, 1u, &context->vertex_buffer, &(VkDeviceSize){0u});
   context->api.cmd_draw(command, 3u, 1u, 0u, 0u);
   context->api.cmd_end_render_pass(command);
   record_export(context, command, context->depth[0].image, readback->buffer,
                 copy_image_to_buffer,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
   const VkResult end_result = context->api.end_command_buffer(command);
   if (end_result != VK_SUCCESS)
      fprintf(stderr, "draw lifecycle end result: %d\n", end_result);
   const bool valid =
      end_result == VK_SUCCESS && ordered_lifecycle_valid(command, true);
   if (valid)
      *command_out = command;
   return valid;
}

static bool
failed_preparation_preserves_published_state(
   struct public_context *context, const struct readback *readback,
   PFN_vkCmdCopyImageToBuffer copy_image_to_buffer)
{
   VK_FROM_HANDLE(r3v_native_image, image, context->depth[0].image);
   struct r3v_native_device *device =
      r3v_native_device_from_handle(context->device);
   const struct r3v_native_image_committed_state committed =
      image->committed_submission;
   const struct r3v_native_zmask_owner_state owner = device->zmask_owner;
   VkCommandBuffer command = begin_command(context);
   if (command == VK_NULL_HANDLE)
      return false;
   record_initial_clear(context, command, context->depth[0].image);
   copy_image_to_buffer(
      command, context->depth[0].image, VK_IMAGE_LAYOUT_GENERAL,
      readback->buffer, 1u,
      &(VkBufferImageCopy){
         .bufferRowLength = 63u,
         .bufferImageHeight = 64u,
         .imageSubresource =
            {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .layerCount = 1u},
         .imageExtent = {64u, 64u, 1u},
      });
   return context->api.end_command_buffer(command) != VK_SUCCESS &&
          memcmp(&image->committed_submission, &committed,
                 sizeof(committed)) == 0 &&
          r3v_native_zmask_owner_equal(&device->zmask_owner, &owner);
}

static uint32_t
independent_address(uint32_t x, uint32_t y)
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
   return DEPTH_BASE +
          2048u * (macro_y * 2u + macro_x) + 32u * local_block + 4u * lane;
}

static bool
format_evidence_name(char name[64], const char *label, const char *suffix)
{
   const int length = snprintf(name, 64u, "%s-%s.bin", label, suffix);
   return length > 0 && (size_t)length < 64u;
}

static bool
retain_evidence_file(const char *evidence_dir, const char *label,
                     const char *suffix, const void *contents, size_t size)
{
   char name[64];
   return format_evidence_name(name, label, suffix) &&
          r3v_native_evidence_write_file(evidence_dir, name, contents, size) ==
             0;
}

static PFN_vkQueueWaitIdle
load_queue_wait_idle(struct public_context *context)
{
   PFN_vkGetDeviceProcAddr get_device_proc_addr =
      (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(
         context->instance, "vkGetDeviceProcAddr");
   return get_device_proc_addr != NULL
             ? (PFN_vkQueueWaitIdle)get_device_proc_addr(context->device,
                                                         "vkQueueWaitIdle")
             : NULL;
}

static bool
submit_and_wait(struct public_context *context, VkCommandBuffer command)
{
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1u,
      .pCommandBuffers = &command,
   };
   PFN_vkQueueWaitIdle queue_wait_idle = load_queue_wait_idle(context);
   return queue_wait_idle != NULL &&
          context->api.queue_submit(context->queue, 1u, &submit,
                                    VK_NULL_HANDLE) == VK_SUCCESS &&
          queue_wait_idle(context->queue) == VK_SUCCESS;
}

static bool
backing_words_match(const uint8_t depth_bytes[DEPTH_BYTES], bool apply_update,
                    bool written[DEPTH_BYTES])
{
   bool valid = true;
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const bool updated = apply_update && x < UPDATE_WIDTH &&
                              y < UPDATE_HEIGHT;
         const uint32_t depth =
            updated ? UPDATE_DEPTH_CODE : CLEAR_DEPTH_CODE;
         const uint32_t expected_word = (depth << 8u) | CLEAR_STENCIL;
         const uint32_t offset = independent_address(x, y);
         uint32_t observed_word;
         memcpy(&observed_word, depth_bytes + offset, sizeof(observed_word));
         valid &= observed_word == expected_word;
         for (uint32_t byte = 0u; byte < 4u; byte++)
            written[offset + byte] = true;
      }
   }
   return valid;
}

static bool
exported_depth_matches(const uint8_t *readback_bytes, bool apply_update)
{
   bool valid = true;
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const bool updated = apply_update && x < UPDATE_WIDTH &&
                              y < UPDATE_HEIGHT;
         const uint32_t expected_depth =
            updated ? UPDATE_DEPTH_CODE : CLEAR_DEPTH_CODE;
         uint32_t exported_word;
         memcpy(&exported_word, readback_bytes + 4u * (y * 64u + x),
                sizeof(exported_word));
         /* D24 image-to-buffer copies place the depth code in bits 0..23.  The
          * independently initialized byte in bits 24..31 calibrates the copy
          * width and catches an unintended packed-word overwrite. */
         valid &= (exported_word & 0x00ffffffu) == expected_depth;
         valid &= (exported_word >> 24u) == DEPTH_GUARD;
      }
   }
   return valid;
}

static bool
backing_guards_match(const uint8_t depth_bytes[DEPTH_BYTES],
                     const bool written[DEPTH_BYTES])
{
   for (uint32_t offset = 0u; offset < DEPTH_BYTES; offset++)
      if (!written[offset] && depth_bytes[offset] != DEPTH_GUARD)
         return false;
   return true;
}

static bool
hardware_result_matches(struct public_context *context,
                        const struct readback *readback,
                        VkCommandBuffer command, bool apply_update,
                        const char *evidence_dir, const char *label)
{
   uint8_t *depth_bytes = NULL;
   uint8_t *readback_bytes = NULL;
   if (context->api.map_memory(context->device, context->depth[0].memory, 0u,
                               VK_WHOLE_SIZE, 0u,
                               (void **)&depth_bytes) != VK_SUCCESS)
      return false;
   if (context->api.map_memory(context->device, readback->memory, 0u,
                               VK_WHOLE_SIZE, 0u,
                               (void **)&readback_bytes) != VK_SUCCESS) {
      context->api.unmap_memory(context->device, context->depth[0].memory);
      return false;
   }
   memset(depth_bytes, DEPTH_GUARD, DEPTH_BYTES);
   memset(readback_bytes, DEPTH_GUARD, PIXELS * sizeof(uint32_t));
   if (!retain_evidence_file(evidence_dir, label, "initial-backing", depth_bytes,
                             DEPTH_BYTES)) {
      context->api.unmap_memory(context->device, readback->memory);
      context->api.unmap_memory(context->device, context->depth[0].memory);
      return false;
   }
   context->api.unmap_memory(context->device, readback->memory);
   context->api.unmap_memory(context->device, context->depth[0].memory);

   if (!submit_and_wait(context, command))
      return false;
   if (context->api.map_memory(context->device, context->depth[0].memory, 0u,
                               VK_WHOLE_SIZE, 0u,
                               (void **)&depth_bytes) != VK_SUCCESS)
      return false;
   if (context->api.map_memory(context->device, readback->memory, 0u,
                               VK_WHOLE_SIZE, 0u,
                               (void **)&readback_bytes) != VK_SUCCESS) {
      context->api.unmap_memory(context->device, context->depth[0].memory);
      return false;
   }
   bool written[DEPTH_BYTES] = {false};
   bool valid = backing_words_match(depth_bytes, apply_update, written) &&
                exported_depth_matches(readback_bytes, apply_update) &&
                backing_guards_match(depth_bytes, written);
   valid &= retain_evidence_file(evidence_dir, label, "final-backing",
                                 depth_bytes, DEPTH_BYTES);
   valid &= retain_evidence_file(evidence_dir, label, "exported-depth",
                                 readback_bytes, PIXELS * sizeof(uint32_t));
   context->api.unmap_memory(context->device, readback->memory);
   context->api.unmap_memory(context->device, context->depth[0].memory);
   return valid;
}

static struct lifecycle_results
record_lifecycle(struct public_context *context, struct readback *readback,
                 struct partial_draw *partial,
                 PFN_vkCmdCopyImageToBuffer copy_image_to_buffer,
                 bool hardware_requested, const char *evidence_dir)
{
   struct lifecycle_results results = {
      .ordinary_control = record_ordinary_control(context),
      .hardware = true,
   };
   struct r3v_native_device *device =
      r3v_native_device_from_handle(context->device);
   device->zmask_automatic_qualified = true;
   const bool fixtures_created =
      copy_image_to_buffer != NULL && create_readback(context, readback) &&
      create_partial_draw(context, partial);
   VkCommandBuffer copy_command = VK_NULL_HANDLE;
   VkCommandBuffer draw_command = VK_NULL_HANDLE;
   results.copy_materialization =
      fixtures_created && record_copy_materialization(
                             context, readback, copy_image_to_buffer,
                             &copy_command);
   results.draw_materialization =
      fixtures_created && record_draw_materialization(
                             context, partial, readback, copy_image_to_buffer,
                             &draw_command);
   results.rollback =
      fixtures_created && failed_preparation_preserves_published_state(
                             context, readback, copy_image_to_buffer);
   if (hardware_requested && results.copy_materialization &&
       results.draw_materialization)
      results.hardware =
         hardware_result_matches(context, readback, copy_command, false,
                                 evidence_dir, "copy") &&
         hardware_result_matches(context, readback, draw_command, true,
                                 evidence_dir, "draw");
   device->zmask_automatic_qualified = false;
   return results;
}

static bool
lifecycle_passed(const struct lifecycle_results *results)
{
   return results->ordinary_control && results->copy_materialization &&
          results->draw_materialization && results->rollback &&
          results->hardware;
}

int
main(int argc, char **argv)
{
   const bool hardware_requested =
      argc == 3 && strcmp(argv[1], "--hardware") == 0;
   if ((argc != 1 && !hardware_requested) ||
       (hardware_requested && !hardware_environment_matches(argv[2], "2")))
      return 2;
   uint32_t expected[PIXELS];
   build_expected_words(expected, false);
   check_expected_words(expected, false);
   build_expected_words(expected, true);
   check_expected_words(expected, true);

   struct public_context context;
   if (!create_public_context(&context, 1u) ||
       !upload_partial_triangle(&context))
      return 1;
   struct r3v_native_device *device = r3v_native_device_from_handle(context.device);
   device->vk.physical->instance->enable_debug_logging = true;
   assert(!device->zmask_automatic_qualified);
   struct readback readback = {0};
   struct partial_draw partial = {0};
   PFN_vkCmdCopyImageToBuffer copy_image_to_buffer =
      load_copy_image_to_buffer(&context);
   const struct lifecycle_results results =
      record_lifecycle(&context, &readback, &partial, copy_image_to_buffer,
                       hardware_requested, hardware_requested ? argv[2] : NULL);
   assert(!device->zmask_automatic_qualified);
   printf("ordinary_control=%d copy_materialization=%d "
          "draw_materialization=%d rollback=%d hardware=%d\n",
          results.ordinary_control, results.copy_materialization,
          results.draw_materialization, results.rollback, results.hardware);
   destroy_readback(&context, &readback);
   destroy_partial_draw(&context, &partial);
   destroy_public_context(&context);
   if (!lifecycle_passed(&results))
      return 1;
   puts("r3v public ZMASK clear, update, materialization lifecycle: OK");
   return 0;
}
