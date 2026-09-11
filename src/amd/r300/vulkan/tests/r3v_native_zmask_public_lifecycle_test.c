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
#define IMAGE_A_CLEAR_DEPTH_CODE 0x800000u
#define IMAGE_B_CLEAR_DEPTH_CODE 0xbfffffu
#define UPDATE_DEPTH_CODE 0x400000u
#define BACKING_DEPTH_CODE 0x200000u
#define IMAGE_A_CLEAR_STENCIL 0x5au
#define IMAGE_B_CLEAR_STENCIL 0xa5u
#define READBACK_GUARD_BYTES 64u
#define READBACK_PAYLOAD_BYTES (PIXELS * sizeof(uint32_t))
#define READBACK_BYTES (READBACK_GUARD_BYTES * 2u + READBACK_PAYLOAD_BYTES)

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
   bool aba_materialization;
   bool rollback;
   bool hardware;
   bool compressed_route_unselected;
   bool direct_fast_clear_read_recorded;
   bool direct_fast_clear_read_color_mask_exact;
   bool cross_command_buffer_read;
   uint32_t queue_submit_calls;
   uint32_t queue_submits_accepted;
   uint32_t queue_executions_completed;
};

enum hardware_scenario {
   HARDWARE_NONE,
   HARDWARE_COPY_MATERIALIZATION,
   HARDWARE_ABA_MATERIALIZATION,
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
         .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      },
   };
   const VkAttachmentReference color_reference = {
      .attachment = 0u,
      .layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   const VkAttachmentReference depth_reference = {
      .attachment = 1u,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
   };
   VkResult result = context->api.create_render_pass(
      context->device,
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
      NULL, &partial->render_pass);
   if (result == VK_SUCCESS)
      result = context->api.create_framebuffer(
         context->device,
         &(VkFramebufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = partial->render_pass,
            .attachmentCount = 2u,
            .pAttachments =
               (VkImageView[]){context->color_view, context->depth[0].view},
            .width = 64u,
            .height = 64u,
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
                  .width = 64.0f,
                  .height = 64.0f,
                  .maxDepth = 1.0f,
               },
               .scissorCount = 1u,
               .pScissors = &(VkRect2D){.extent = {64u, 64u}},
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
               .depthWriteEnable = VK_FALSE,
               .depthCompareOp = VK_COMPARE_OP_LESS,
               .stencilTestEnable = VK_TRUE,
               .front =
                  {
                     .failOp = VK_STENCIL_OP_KEEP,
                     .passOp = VK_STENCIL_OP_KEEP,
                     .depthFailOp = VK_STENCIL_OP_KEEP,
                     .compareOp = VK_COMPARE_OP_ALWAYS,
                     .compareMask = UINT8_MAX,
                     .writeMask = UINT8_MAX,
                  },
               .back =
                  {
                     .failOp = VK_STENCIL_OP_KEEP,
                     .passOp = VK_STENCIL_OP_KEEP,
                     .depthFailOp = VK_STENCIL_OP_KEEP,
                     .compareOp = VK_COMPARE_OP_ALWAYS,
                     .compareMask = UINT8_MAX,
                     .writeMask = UINT8_MAX,
                  },
            },
            .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
               .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
               .attachmentCount = 1u,
               .pAttachments = &(VkPipelineColorBlendAttachmentState){
                  .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                    VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT,
               },
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
build_expected_words(uint32_t words[PIXELS], uint32_t clear_depth_code,
                     uint8_t clear_stencil, bool apply_update)
{
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const bool updated = apply_update && x >= UPDATE_X &&
                              x < UPDATE_X + UPDATE_WIDTH && y >= UPDATE_Y &&
                              y < UPDATE_Y + UPDATE_HEIGHT;
         const uint32_t depth =
            updated ? UPDATE_DEPTH_CODE : clear_depth_code;
         words[y * 64u + x] = (depth << 8) | clear_stencil;
      }
   }
}

static void
check_expected_words(const uint32_t words[PIXELS], uint32_t clear_depth_code,
                     uint8_t clear_stencil, bool apply_update)
{
   uint32_t updated_count = 0u;
   uint32_t untouched_count = 0u;
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const uint32_t word = words[y * 64u + x];
         const bool updated = x >= UPDATE_X && x < UPDATE_X + UPDATE_WIDTH &&
                              y >= UPDATE_Y &&
                              y < UPDATE_Y + UPDATE_HEIGHT;
         assert((word & UINT8_MAX) == clear_stencil);
         assert((word >> 8) ==
                (apply_update && updated ? UPDATE_DEPTH_CODE
                                         : clear_depth_code));
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
         .size = READBACK_BYTES,
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
         .allocationSize = READBACK_BYTES,
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

static bool
fresh_command_buffer_requires_committed_fast_clear_owner(
   struct public_context *context)
{
   VK_FROM_HANDLE(r3v_native_image, image, context->depth[0].image);
   struct r3v_native_device *device =
      r3v_native_device_from_handle(context->device);
   const struct r3v_native_image_committed_state saved_committed =
      image->committed_submission;
   const struct r3v_native_zmask_owner_state saved_owner = device->zmask_owner;
   const struct r3v_native_zmask_metadata_state metadata = {
      .status = R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR,
      .clear_depth_code = IMAGE_A_CLEAR_DEPTH_CODE,
      .clear_stencil = IMAGE_A_CLEAR_STENCIL,
      .generation = 7u,
   };
   struct r3v_native_zmask_owner_state owner = {0};
   bool valid =
      r3v_native_zmask_owner_from_image(image, &metadata, &owner) == VK_SUCCESS;
   image->committed_submission.representation =
      R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR;
   image->committed_submission.zmask_metadata = metadata;
   device->zmask_owner = owner;

   VkCommandBuffer command = begin_command(context);
   if (command != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native, command);
      struct r3v_native_cmd_image_state *state = NULL;
      struct r300_zmask_materialize_plan plan = {0};
      bool enabled = false;
      valid &= r3v_native_cmd_buffer_append_image_state(native, image, &state) ==
               VK_SUCCESS;
      valid &= r3v_native_cmd_buffer_prepare_zmask_fast_clear_read(
                  native, image, &plan, &enabled) == VK_SUCCESS;
      valid &= enabled && native->required_zmask_owner_set &&
               !native->current_zmask_owner_set &&
               r3v_native_zmask_owner_equal(&native->required_zmask_owner,
                                             &owner);
   } else {
      valid = false;
   }

   image->committed_submission = saved_committed;
   device->zmask_owner = saved_owner;
   return valid;
}

static const VkImageSubresourceRange packed_range = {
   .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
   .levelCount = 1u,
   .layerCount = 1u,
};

static void
record_initial_clear(struct public_context *context, VkCommandBuffer command,
                     VkImage image, float depth, uint32_t stencil)
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
      &(VkClearDepthStencilValue){.depth = depth, .stencil = stencil}, 1u,
      &packed_range);
}

static void
record_export(struct public_context *context, VkCommandBuffer command,
              VkImage image, VkBuffer buffer,
              PFN_vkCmdCopyImageToBuffer copy_image_to_buffer,
              VkImageLayout source_layout,
              VkPipelineStageFlags producer_stage,
              VkAccessFlags producer_access)
{
   const VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = producer_access,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = source_layout,
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
         .bufferOffset = READBACK_GUARD_BYTES,
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
                .clear_depth_code == IMAGE_A_CLEAR_DEPTH_CODE &&
          fast_clear->payload.image_fast_clear.resulting_metadata
                .clear_stencil == IMAGE_A_CLEAR_STENCIL;
}

static bool
copy_segments_valid(const struct r3v_native_cmd_buffer *native)
{
   if (native->rb2d_copy_operation_count == 0u)
      return false;
   for (uint32_t copy_index = 0u;
        copy_index < native->rb2d_copy_operation_count; copy_index++) {
      const struct r3v_native_rb2d_copy_operation *copy =
         &native->rb2d_copy_operations[copy_index];
      if (copy->segment_count == 0u)
         return false;
      for (uint32_t segment_index = 0u; segment_index < copy->segment_count;
           segment_index++) {
         const struct r300_rb2d_copy_segment *segment =
            &copy->segments[segment_index];
         if (segment->source_offset_bytes > copy->source_buffer_bytes ||
             segment->byte_count >
                copy->source_buffer_bytes - segment->source_offset_bytes ||
             segment->destination_offset_bytes >
                copy->destination_buffer_bytes ||
             segment->byte_count > copy->destination_buffer_bytes -
                                      segment->destination_offset_bytes)
            return false;
      }
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
direct_fast_clear_read_valid(const struct r3v_native_cmd_buffer *native,
                             uint32_t deferred_index)
{
   if (deferred_index >= native->deferred_draw_count)
      return false;
   const struct r3v_native_deferred_draw *draw =
      &native->deferred_draws[deferred_index];
   const struct r300_zmask_materialize_plan *plan =
      &draw->zmask_fast_clear_read;
   if (!draw->has_zmask_fast_clear_read || !draw->has_depth_pipeline ||
       draw->depth_pipeline.hardware.depth_write ||
       draw->depth_pipeline.hardware.depth_function != R300_ZS_LESS ||
       plan->clear_word !=
          ((IMAGE_A_CLEAR_DEPTH_CODE << 8) | IMAGE_A_CLEAR_STENCIL) ||
       plan->begin_dword_count != 9u || plan->dword_count != 15u ||
       draw->ib_span_offset > native->ib_size_dwords ||
       draw->ib_span_dwords >
          native->ib_size_dwords - draw->ib_span_offset)
      return false;

   uint32_t depth_clear = UINT32_MAX;
   uint32_t zmask_bind = UINT32_MAX;
   uint32_t peq = UINT32_MAX;
   uint32_t enable = UINT32_MAX;
   uint32_t draw_packet = UINT32_MAX;
   uint32_t flush = UINT32_MAX;
   uint32_t disable = UINT32_MAX;
   const uint32_t end = draw->ib_span_offset + draw->ib_span_dwords;
   for (uint32_t word = draw->ib_span_offset; word + 1u < end; word++) {
      if (native->ib[word] == CP_PACKET0(R300_ZB_DEPTHCLEARVALUE, 0) &&
          native->ib[word + 1u] == plan->clear_word)
         depth_clear = word;
      else if (native->ib[word] == CP_PACKET0(R300_ZB_ZMASK_OFFSET, 1) &&
               word + 2u < end && native->ib[word + 1u] == 0u &&
               native->ib[word + 2u] == 64u)
         zmask_bind = word;
      else if (native->ib[word] == CP_PACKET0(R300_GB_Z_PEQ_CONFIG, 0) &&
               native->ib[word + 1u] ==
                  R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4) {
         if (draw_packet == UINT32_MAX)
            peq = word;
      } else if (native->ib[word] == CP_PACKET0(R300_ZB_BW_CNTL, 0)) {
         if (native->ib[word + 1u] ==
             (R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE))
            enable = word;
         else if (native->ib[word + 1u] == 0u &&
                  draw_packet != UINT32_MAX)
            disable = word;
      } else if ((native->ib[word] >> 30) == 3u &&
                 (native->ib[word] & 0xff00u) ==
                    R300_PACKET3_3D_DRAW_VBUF_2)
         draw_packet = word;
      else if (native->ib[word] ==
               CP_PACKET0(R300_ZB_ZCACHE_CTLSTAT, 0))
         flush = word;
   }
   return depth_clear < zmask_bind && zmask_bind < peq && peq < enable &&
          enable < draw_packet && draw_packet < flush && flush < disable;
}

static bool
aba_lifecycle_valid(VkCommandBuffer command, const struct public_context *context,
                    const struct readback readbacks[2])
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, command);
   VK_FROM_HANDLE(r3v_native_image, image_a, context->depth[0].image);
   VK_FROM_HANDLE(r3v_native_image, image_b, context->depth[1].image);
   VK_FROM_HANDLE(r3v_native_image, color_image, context->color_image);
   VK_FROM_HANDLE(r3v_native_buffer, readback_a, readbacks[0].buffer);
   VK_FROM_HANDLE(r3v_native_buffer, readback_b, readbacks[1].buffer);
   uint32_t clear_a_first = UINT32_MAX;
   uint32_t materialize_a_first = UINT32_MAX;
   uint32_t clear_b = UINT32_MAX;
   uint32_t materialize_b = UINT32_MAX;
   uint32_t clear_a_final = UINT32_MAX;
   uint32_t draw_a = UINT32_MAX;
   uint32_t first_copy_a = UINT32_MAX;
   uint32_t last_copy_a = UINT32_MAX;
   uint32_t materialize_a_final = UINT32_MAX;
   uint32_t first_copy_b = UINT32_MAX;
   uint32_t fast_clear_count = 0u;
   uint32_t materialize_count = 0u;

   for (uint32_t index = 0u; index < native->ordered_operation_count; index++) {
      const struct r3v_native_ordered_operation *operation =
         &native->ordered_operations[index];
      if (operation->kind == R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR) {
         const struct r3v_native_zmask_metadata_state *metadata =
            &operation->payload.image_fast_clear.resulting_metadata;
         fast_clear_count++;
         if (operation->payload.image_fast_clear.authority !=
             R3V_NATIVE_ZMASK_FAST_CLEAR_AUTOMATIC) {
            fprintf(stderr, "A/B/A clear %u has authority %u\n", index,
                    operation->payload.image_fast_clear.authority);
            return false;
         }
         if (operation->payload.image_fast_clear.image == image_a &&
             metadata->clear_depth_code == IMAGE_A_CLEAR_DEPTH_CODE &&
             metadata->clear_stencil == IMAGE_A_CLEAR_STENCIL) {
            if (clear_a_first == UINT32_MAX)
               clear_a_first = index;
            else
               clear_a_final = index;
         }
         else if (operation->payload.image_fast_clear.image == image_b &&
                  metadata->clear_depth_code == IMAGE_B_CLEAR_DEPTH_CODE &&
                  metadata->clear_stencil == IMAGE_B_CLEAR_STENCIL)
            clear_b = index;
         else {
            fprintf(stderr,
                    "A/B/A clear %u has unexpected image=%p depth=0x%06x "
                    "stencil=0x%02x\n",
                    index, (void *)operation->payload.image_fast_clear.image,
                    metadata->clear_depth_code, metadata->clear_stencil);
            return false;
         }
      } else if (operation->kind ==
                 R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE) {
         materialize_count++;
         if (operation->payload.image_materialize.source_representation !=
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR) {
            fprintf(stderr,
                    "A/B/A materialization %u has source representation %u\n",
                    index,
                    operation->payload.image_materialize.source_representation);
            return false;
         }
         if (operation->payload.image_materialize.image == image_a) {
            if (materialize_a_first == UINT32_MAX)
               materialize_a_first = index;
            else
               materialize_a_final = index;
         }
         else if (operation->payload.image_materialize.image == image_b)
            materialize_b = index;
         else {
            fprintf(stderr, "A/B/A materialization %u has unknown image\n",
                    index);
            return false;
         }
      } else if (operation->kind == R3V_NATIVE_ORDERED_OPERATION_DRAW) {
         const uint32_t deferred = operation->payload.draw.deferred_draw_index;
         if (deferred >= native->deferred_draw_count ||
             native->deferred_draws[deferred].depth_memory != image_a->memory ||
             !direct_fast_clear_read_valid(native, deferred)) {
            fprintf(stderr, "A/B/A draw %u has unexpected depth target\n",
                    index);
            return false;
         }
         draw_a = index;
      } else if (operation->kind == R3V_NATIVE_ORDERED_OPERATION_RB2D_COPY) {
         const uint32_t copy_index = operation->payload.rb2d_copy.rb2d_copy_index;
         if (copy_index >= native->rb2d_copy_operation_count) {
            fprintf(stderr, "A/B/A copy %u has invalid index %u\n", index,
                    copy_index);
            return false;
         }
         const struct r3v_native_rb2d_copy_operation *copy =
            &native->rb2d_copy_operations[copy_index];
         if (copy->source_memory == image_a->memory &&
             copy->destination_memory == readback_a->memory) {
            if (first_copy_a == UINT32_MAX)
               first_copy_a = index;
            last_copy_a = index;
         } else if (copy->source_memory == image_b->memory &&
                    copy->destination_memory == readback_b->memory) {
            if (first_copy_b == UINT32_MAX)
               first_copy_b = index;
         } else {
            fprintf(stderr, "A/B/A copy %u has unexpected storage pair\n",
                    index);
            return false;
         }
      }
   }

   if (fast_clear_count != 3u || materialize_count != 3u ||
       clear_a_first == UINT32_MAX || materialize_a_first == UINT32_MAX ||
       clear_b == UINT32_MAX || materialize_b == UINT32_MAX ||
       clear_a_final == UINT32_MAX || draw_a == UINT32_MAX ||
       first_copy_a == UINT32_MAX || last_copy_a == UINT32_MAX ||
       materialize_a_final == UINT32_MAX || first_copy_b == UINT32_MAX ||
       !(clear_a_first < materialize_a_first &&
         materialize_a_first < clear_b && clear_b < materialize_b &&
         materialize_b < clear_a_final && clear_a_final < draw_a &&
         draw_a < materialize_a_final &&
         materialize_a_final < first_copy_a &&
         last_copy_a < first_copy_b) ||
       native->image_state_count != 3u) {
      fprintf(stderr,
              "A/B/A order invalid: operations=%u states=%u clears=%u "
              "materializations=%u Aclear=%u Amaterialize=%u Bclear=%u "
              "Bmaterialize=%u Aclear2=%u draw=%u Amaterialize2=%u "
              "Acopy=%u..%u Bcopy=%u\n",
              native->ordered_operation_count, native->image_state_count,
              fast_clear_count, materialize_count, clear_a_first,
              materialize_a_first, clear_b, materialize_b, clear_a_final,
              draw_a, materialize_a_final, first_copy_a, last_copy_a,
              first_copy_b);
      return false;
   }

   uint32_t depth_state_count = 0u;
   uint32_t color_state_count = 0u;
   for (uint32_t index = 0u; index < native->image_state_count; index++) {
      const struct r3v_native_cmd_image_state *state =
         &native->image_states[index];
      if (state->image == color_image) {
         color_state_count++;
         continue;
      }
      if (state->image != image_a && state->image != image_b)
         return false;
      depth_state_count++;
      if (state->current_representation !=
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED ||
          state->current_zmask_metadata.status !=
             R3V_NATIVE_ZMASK_METADATA_RETIRED)
         return false;
   }
   return depth_state_count == 2u && color_state_count == 1u &&
          copy_segments_valid(native);
}

static bool
record_ordinary_control(struct public_context *context)
{
   VkCommandBuffer command = begin_command(context);
   if (command == VK_NULL_HANDLE)
      return false;
   record_initial_clear(context, command, context->depth[0].image, 0.5f,
                        0x15au);
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
   record_initial_clear(context, command, context->depth[0].image, 0.5f,
                        0x15au);
   record_export(context, command, context->depth[0].image, readback->buffer,
                 copy_image_to_buffer, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
   const bool valid = context->api.end_command_buffer(command) == VK_SUCCESS &&
                      ordered_lifecycle_valid(command, false);
   if (valid)
      *command_out = command;
   return valid;
}

static bool
record_aba_materialization(struct public_context *context,
                           const struct partial_draw *partial,
                           const struct readback readbacks[2],
                           PFN_vkCmdCopyImageToBuffer copy_image_to_buffer,
                           VkCommandBuffer *command_out)
{
   VkCommandBuffer command = begin_command(context);
   if (command == VK_NULL_HANDLE)
      return false;
   record_initial_clear(context, command, context->depth[0].image, 0.5f,
                        0x15au);
   record_initial_clear(context, command, context->depth[1].image, 0.75f,
                        0x1a5u);
   context->api.cmd_clear_depth_stencil_image(
      command, context->depth[0].image, VK_IMAGE_LAYOUT_GENERAL,
      &(VkClearDepthStencilValue){.depth = 0.5f, .stencil = 0x15au}, 1u,
      &packed_range);
   const VkImageMemoryBarrier clear_to_depth = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
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
         .renderArea = {{0, 0}, {64u, 64u}},
         .clearValueCount = 1u,
         .pClearValues = &(VkClearValue){
            .color = {.uint32 = {0u, 0u, 0u, 0u}},
         },
      },
      VK_SUBPASS_CONTENTS_INLINE);
   context->api.cmd_bind_pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  partial->pipeline);
   context->api.cmd_bind_vertex_buffers(
      command, 0u, 1u, &context->vertex_buffer, &(VkDeviceSize){0u});
   context->api.cmd_draw(command, 3u, 1u, 0u, 0u);
   context->api.cmd_end_render_pass(command);
   record_export(context, command, context->depth[0].image,
                 readbacks[0].buffer, copy_image_to_buffer,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
   record_export(context, command, context->depth[1].image,
                 readbacks[1].buffer, copy_image_to_buffer,
                 VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
   const VkResult end_result = context->api.end_command_buffer(command);
   if (end_result != VK_SUCCESS)
      fprintf(stderr, "A/B/A lifecycle end result: %d\n", end_result);
   const bool valid =
      end_result == VK_SUCCESS &&
      aba_lifecycle_valid(command, context, readbacks);
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
   record_initial_clear(context, command, context->depth[0].image, 0.5f,
                        0x15au);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, command);
   const uint32_t image_state_count = native->image_state_count;
   const uint32_t ordered_operation_count = native->ordered_operation_count;
   const uint32_t deferred_copy_count = native->deferred_copy_count;
   const uint32_t rb2d_copy_operation_count =
      native->rb2d_copy_operation_count;
   const uint32_t deferred_draw_count = native->deferred_draw_count;
   const uint32_t ib_size_dwords = native->ib_size_dwords;
   const uint32_t reference_count = native->reference_count;
   const bool required_owner_set = native->required_zmask_owner_set;
   const bool current_owner_set = native->current_zmask_owner_set;
   const struct r3v_native_zmask_owner_state required_owner =
      native->required_zmask_owner;
   const struct r3v_native_zmask_owner_state current_owner =
      native->current_zmask_owner;
   struct r3v_native_cmd_image_state *image_states =
      malloc(image_state_count * sizeof(*image_states));
   struct r3v_native_ordered_operation *ordered_operations =
      malloc(ordered_operation_count * sizeof(*ordered_operations));
   uint32_t *ib = malloc(ib_size_dwords * sizeof(*ib));
   struct r3v_native_bo_reference *references =
      malloc(reference_count * sizeof(*references));
   uint8_t backing_before[DEPTH_BYTES];
   uint8_t *backing = NULL;
   bool preparation_preserved = image_states != NULL &&
                                ordered_operations != NULL && ib != NULL &&
                                references != NULL;
   if (preparation_preserved) {
      memcpy(image_states, native->image_states,
             image_state_count * sizeof(*image_states));
      memcpy(ordered_operations, native->ordered_operations,
             ordered_operation_count * sizeof(*ordered_operations));
      memcpy(ib, native->ib, ib_size_dwords * sizeof(*ib));
      memcpy(references, native->references,
             reference_count * sizeof(*references));
      preparation_preserved =
         context->api.map_memory(context->device, context->depth[0].memory, 0u,
                                 VK_WHOLE_SIZE, 0u,
                                 (void **)&backing) == VK_SUCCESS;
      if (preparation_preserved) {
         memcpy(backing_before, backing, sizeof(backing_before));
         context->api.unmap_memory(context->device,
                                   context->depth[0].memory);
      }
   }
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
   preparation_preserved &=
      native->image_state_count == image_state_count &&
      native->ordered_operation_count == ordered_operation_count &&
      native->deferred_copy_count == deferred_copy_count &&
      native->rb2d_copy_operation_count == rb2d_copy_operation_count &&
      native->deferred_draw_count == deferred_draw_count &&
      native->ib_size_dwords == ib_size_dwords &&
      native->reference_count == reference_count &&
      native->required_zmask_owner_set == required_owner_set &&
      native->current_zmask_owner_set == current_owner_set &&
      r3v_native_zmask_owner_equal(&native->required_zmask_owner,
                                    &required_owner) &&
      r3v_native_zmask_owner_equal(&native->current_zmask_owner,
                                    &current_owner) &&
      memcmp(native->image_states, image_states,
             image_state_count * sizeof(*image_states)) == 0 &&
      memcmp(native->ordered_operations, ordered_operations,
             ordered_operation_count * sizeof(*ordered_operations)) == 0 &&
      memcmp(native->ib, ib, ib_size_dwords * sizeof(*ib)) == 0 &&
      memcmp(native->references, references,
             reference_count * sizeof(*references)) == 0;
   if (preparation_preserved &&
       context->api.map_memory(context->device, context->depth[0].memory, 0u,
                               VK_WHOLE_SIZE, 0u,
                               (void **)&backing) == VK_SUCCESS) {
      preparation_preserved &=
         memcmp(backing, backing_before, sizeof(backing_before)) == 0;
      context->api.unmap_memory(context->device, context->depth[0].memory);
   } else {
      preparation_preserved = false;
   }
   free(references);
   free(ib);
   free(ordered_operations);
   free(image_states);
   return preparation_preserved &&
          context->api.end_command_buffer(command) != VK_SUCCESS &&
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
submit_and_wait(struct public_context *context, VkCommandBuffer command,
                struct lifecycle_results *results)
{
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1u,
      .pCommandBuffers = &command,
   };
   PFN_vkQueueWaitIdle queue_wait_idle = load_queue_wait_idle(context);
   if (queue_wait_idle == NULL)
      return false;
   results->queue_submit_calls++;
   if (context->api.queue_submit(context->queue, 1u, &submit,
                                 VK_NULL_HANDLE) != VK_SUCCESS)
      return false;
   results->queue_submits_accepted++;
   if (queue_wait_idle(context->queue) != VK_SUCCESS)
      return false;
   results->queue_executions_completed++;
   return true;
}

static bool
backing_words_match(const uint8_t depth_bytes[DEPTH_BYTES],
                    uint32_t clear_depth_code, uint8_t clear_stencil,
                    bool apply_update, bool written[DEPTH_BYTES])
{
   bool valid = true;
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const bool updated = apply_update && x < UPDATE_WIDTH &&
                              y < UPDATE_HEIGHT;
         const uint32_t depth =
            updated ? UPDATE_DEPTH_CODE : clear_depth_code;
         const uint32_t expected_word = (depth << 8u) | clear_stencil;
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
exported_depth_matches(const uint8_t *readback_bytes,
                       uint32_t clear_depth_code, bool apply_update)
{
   bool valid = true;
   for (uint32_t y = 0u; y < 64u; y++) {
      for (uint32_t x = 0u; x < 64u; x++) {
         const bool updated = apply_update && x < UPDATE_WIDTH &&
                              y < UPDATE_HEIGHT;
         const uint32_t expected_depth =
            updated ? UPDATE_DEPTH_CODE : clear_depth_code;
         uint32_t exported_word;
         memcpy(&exported_word, readback_bytes + 4u * (y * 64u + x),
                sizeof(exported_word));
         /* D24 image-to-buffer copies place the depth code in bits 0..23. */
         valid &= (exported_word & 0x00ffffffu) == expected_depth;
      }
   }
   return valid;
}

static bool
readback_guards_match(const uint8_t readback_bytes[READBACK_BYTES])
{
   for (uint32_t offset = 0u; offset < READBACK_GUARD_BYTES; offset++) {
      if (readback_bytes[offset] != DEPTH_GUARD ||
          readback_bytes[READBACK_GUARD_BYTES + READBACK_PAYLOAD_BYTES +
                         offset] != DEPTH_GUARD)
         return false;
   }
   return true;
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
                        const char *evidence_dir, const char *label,
                        struct lifecycle_results *results)
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
   memset(readback_bytes, DEPTH_GUARD, READBACK_BYTES);
   if (!retain_evidence_file(evidence_dir, label, "initial-backing", depth_bytes,
                             DEPTH_BYTES)) {
      context->api.unmap_memory(context->device, readback->memory);
      context->api.unmap_memory(context->device, context->depth[0].memory);
      return false;
   }
   context->api.unmap_memory(context->device, readback->memory);
   context->api.unmap_memory(context->device, context->depth[0].memory);

   if (!submit_and_wait(context, command, results))
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
   bool valid = backing_words_match(
                   depth_bytes, IMAGE_A_CLEAR_DEPTH_CODE,
                   IMAGE_A_CLEAR_STENCIL, apply_update, written) &&
                exported_depth_matches(readback_bytes + READBACK_GUARD_BYTES,
                                       IMAGE_A_CLEAR_DEPTH_CODE,
                                       apply_update) &&
                backing_guards_match(depth_bytes, written) &&
                readback_guards_match(readback_bytes);
   valid &= retain_evidence_file(evidence_dir, label, "final-backing",
                                 depth_bytes, DEPTH_BYTES);
   valid &= retain_evidence_file(evidence_dir, label, "exported-depth",
                                 readback_bytes + READBACK_GUARD_BYTES,
                                 READBACK_PAYLOAD_BYTES);
   context->api.unmap_memory(context->device, readback->memory);
   context->api.unmap_memory(context->device, context->depth[0].memory);
   return valid;
}

static bool
aba_hardware_result_matches(struct public_context *context,
                            const struct readback readbacks[2],
                            VkCommandBuffer command, const char *evidence_dir,
                            struct lifecycle_results *results)
{
   uint8_t *depth_bytes[2] = {NULL, NULL};
   uint8_t *readback_bytes[2] = {NULL, NULL};
   const char *const labels[2] = {"aba-a", "aba-b"};
   bool valid = true;

   for (uint32_t index = 0u; index < 2u; index++) {
      if (context->api.map_memory(context->device,
                                  context->depth[index].memory, 0u,
                                  VK_WHOLE_SIZE, 0u,
                                  (void **)&depth_bytes[index]) != VK_SUCCESS)
         goto prepare_fail;
      if (context->api.map_memory(context->device, readbacks[index].memory,
                                  0u, VK_WHOLE_SIZE, 0u,
                                  (void **)&readback_bytes[index]) !=
          VK_SUCCESS)
         goto prepare_fail;
      memset(depth_bytes[index], DEPTH_GUARD, DEPTH_BYTES);
      memset(readback_bytes[index], DEPTH_GUARD, READBACK_BYTES);
      if (!set_uniform_logical_depth(depth_bytes[index], BACKING_DEPTH_CODE,
                                     index == 0u ? IMAGE_A_CLEAR_STENCIL
                                                 : IMAGE_B_CLEAR_STENCIL))
         goto prepare_fail;
      valid &= retain_evidence_file(evidence_dir, labels[index],
                                    "initial-backing", depth_bytes[index],
                                    DEPTH_BYTES);
   }
   for (uint32_t index = 0u; index < 2u; index++) {
      context->api.unmap_memory(context->device, readbacks[index].memory);
      context->api.unmap_memory(context->device, context->depth[index].memory);
      readback_bytes[index] = NULL;
      depth_bytes[index] = NULL;
   }
   if (!valid || !submit_and_wait(context, command, results))
      return false;

   for (uint32_t index = 0u; index < 2u; index++) {
      if (context->api.map_memory(context->device,
                                  context->depth[index].memory, 0u,
                                  VK_WHOLE_SIZE, 0u,
                                  (void **)&depth_bytes[index]) != VK_SUCCESS)
         goto observe_fail;
      if (context->api.map_memory(context->device, readbacks[index].memory,
                                  0u, VK_WHOLE_SIZE, 0u,
                                  (void **)&readback_bytes[index]) !=
          VK_SUCCESS)
         goto observe_fail;
   }
   for (uint32_t index = 0u; index < 2u; index++) {
      const bool image_a = index == 0u;
      const uint32_t clear_depth_code =
         image_a ? IMAGE_A_CLEAR_DEPTH_CODE : IMAGE_B_CLEAR_DEPTH_CODE;
      const uint8_t clear_stencil =
         image_a ? IMAGE_A_CLEAR_STENCIL : IMAGE_B_CLEAR_STENCIL;
      bool written[DEPTH_BYTES] = {false};
      valid &= backing_words_match(depth_bytes[index], clear_depth_code,
                                   clear_stencil, false, written);
      valid &= exported_depth_matches(
         readback_bytes[index] + READBACK_GUARD_BYTES, clear_depth_code,
         false);
      valid &= backing_guards_match(depth_bytes[index], written);
      valid &= readback_guards_match(readback_bytes[index]);
      valid &= retain_evidence_file(evidence_dir, labels[index],
                                    "final-backing", depth_bytes[index],
                                    DEPTH_BYTES);
      valid &= retain_evidence_file(evidence_dir, labels[index],
                                    "exported-depth",
                                    readback_bytes[index] + READBACK_GUARD_BYTES,
                                    READBACK_PAYLOAD_BYTES);
   }
   uint32_t *color_words = NULL;
   if (context->api.map_memory(context->device, context->color_memory, 0u,
                               VK_WHOLE_SIZE, 0u,
                               (void **)&color_words) != VK_SUCCESS)
      goto observe_fail;
   uint8_t color_mask[PIXELS];
   results->direct_fast_clear_read_color_mask_exact = true;
   for (uint32_t pixel = 0u; pixel < PIXELS; pixel++) {
      color_mask[pixel] =
         color_words[pixel] == R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM;
      results->direct_fast_clear_read_color_mask_exact &= color_mask[pixel] != 0u;
   }
   valid &= results->direct_fast_clear_read_color_mask_exact;
   valid &= r3v_native_evidence_write_file(
               evidence_dir, "aba-direct-fast-clear-read-color-mask.bin",
               color_mask, sizeof(color_mask)) == 0;
   context->api.unmap_memory(context->device, context->color_memory);
   for (uint32_t index = 0u; index < 2u; index++) {
      context->api.unmap_memory(context->device, readbacks[index].memory);
      context->api.unmap_memory(context->device, context->depth[index].memory);
   }
   return valid;

prepare_fail:
   for (uint32_t index = 0u; index < 2u; index++) {
      if (readback_bytes[index] != NULL)
         context->api.unmap_memory(context->device, readbacks[index].memory);
      if (depth_bytes[index] != NULL)
         context->api.unmap_memory(context->device,
                                   context->depth[index].memory);
   }
   return false;

observe_fail:
   for (uint32_t index = 0u; index < 2u; index++) {
      if (readback_bytes[index] != NULL)
         context->api.unmap_memory(context->device, readbacks[index].memory);
      if (depth_bytes[index] != NULL)
         context->api.unmap_memory(context->device,
                                   context->depth[index].memory);
   }
   return false;
}

static bool
command_excludes_compressed_zmask(VkCommandBuffer command)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, command);
   for (uint32_t index = 0u; index < native->image_state_count; index++) {
      const struct r3v_native_cmd_image_state *state =
         &native->image_states[index];
      if (state->required_representation ==
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED ||
          (state->current_representation_set &&
           state->current_representation ==
              R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED) ||
          state->required_zmask_metadata.status ==
             R3V_NATIVE_ZMASK_METADATA_COMPRESSED ||
          (state->current_zmask_metadata_set &&
           state->current_zmask_metadata.status ==
              R3V_NATIVE_ZMASK_METADATA_COMPRESSED))
         return false;
   }
   return true;
}

static bool
published_images_are_uncompressed(const struct public_context *context)
{
   struct r3v_native_device *device =
      r3v_native_device_from_handle(context->device);
   if (device->zmask_owner.image != NULL)
      return false;
   for (uint32_t index = 0u; index < 2u; index++) {
      VK_FROM_HANDLE(r3v_native_image, image, context->depth[index].image);
      if (image->committed_submission.representation !=
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED ||
          image->committed_submission.zmask_metadata.status !=
             R3V_NATIVE_ZMASK_METADATA_RETIRED)
         return false;
   }
   return true;
}

static bool
retain_hardware_outcome(const char *evidence_dir, const char *scenario,
                        const struct lifecycle_results *results,
                        bool scenario_hardware_valid,
                        bool automatic_selector_restored)
{
   const bool copy = strcmp(scenario, "copy-materialization") == 0;
   char json[2048];
   const int length = snprintf(
      json, sizeof(json),
      "{\n"
      "  \"schema\": \"r3v-zmask-public-lifecycle-outcome/1\",\n"
      "  \"scenario\": \"%s\",\n"
      "  \"ordered_scenario\": true,\n"
      "  \"campaign_expected_queue_submissions\": 2,\n"
      "  \"queue_submit_calls_attempted\": %u,\n"
      "  \"queue_submits_accepted\": %u,\n"
      "  \"queue_executions_completed\": %u,\n"
      "  \"copy_materialization_recorded\": %s,\n"
      "  \"aba_materialization_recorded\": %s,\n"
      "  \"direct_fast_clear_read_recorded\": %s,\n"
      "  \"direct_fast_clear_read_color_mask_exact\": %s,\n"
      "  \"cross_command_buffer_read\": %s,\n"
      "  \"rollback_preserved\": %s,\n"
      "  \"scenario_hardware_valid\": %s,\n"
      "  \"automatic_selector_restored_false\": %s,\n"
      "  \"compressed_route_unselected\": %s,\n"
      "  \"expected_raw_artifacts\": [\n"
      "%s"
      "  ]\n"
      "}\n",
      scenario, results->queue_submit_calls, results->queue_submits_accepted,
      results->queue_executions_completed,
      results->copy_materialization ? "true" : "false",
      results->aba_materialization ? "true" : "false",
      results->direct_fast_clear_read_recorded ? "true" : "false",
      results->direct_fast_clear_read_color_mask_exact ? "true" : "false",
      results->cross_command_buffer_read ? "true" : "false",
      results->rollback ? "true" : "false",
      scenario_hardware_valid ? "true" : "false",
      automatic_selector_restored ? "true" : "false",
      results->compressed_route_unselected ? "true" : "false",
      copy ? "    \"copy-initial-backing.bin\",\n"
             "    \"copy-final-backing.bin\",\n"
             "    \"copy-exported-depth.bin\"\n"
           : "    \"aba-a-initial-backing.bin\",\n"
             "    \"aba-a-final-backing.bin\",\n"
             "    \"aba-a-exported-depth.bin\",\n"
             "    \"aba-b-initial-backing.bin\",\n"
             "    \"aba-b-final-backing.bin\",\n"
             "    \"aba-b-exported-depth.bin\",\n"
             "    \"aba-direct-fast-clear-read-color-mask.bin\"\n");
   return length > 0 && (size_t)length < sizeof(json) &&
          r3v_native_evidence_write_file(evidence_dir,
                                         "application_outcome.json", json,
                                         (size_t)length) == 0;
}

static struct lifecycle_results
record_lifecycle(struct public_context *context, struct readback readbacks[2],
                 struct partial_draw *partial,
                 PFN_vkCmdCopyImageToBuffer copy_image_to_buffer,
                 enum hardware_scenario hardware_scenario,
                 const char *evidence_dir)
{
   struct r3v_native_device *device =
      r3v_native_device_from_handle(context->device);
   const char *fast_clear_gate = device->zmask_fast_clear_gate;
   device->zmask_fast_clear_gate = NULL;
   struct lifecycle_results results = {
      .ordinary_control = record_ordinary_control(context),
      .cross_command_buffer_read =
         fresh_command_buffer_requires_committed_fast_clear_owner(context),
      .hardware = true,
   };
   device->zmask_fast_clear_gate = fast_clear_gate;
   device->zmask_automatic_qualified = true;
   const bool fixtures_created =
      copy_image_to_buffer != NULL && create_readback(context, &readbacks[0]) &&
      create_readback(context, &readbacks[1]) &&
      create_partial_draw(context, partial);
   VkCommandBuffer copy_command = VK_NULL_HANDLE;
   VkCommandBuffer aba_command = VK_NULL_HANDLE;
   results.copy_materialization =
      fixtures_created && record_copy_materialization(
                             context, &readbacks[0], copy_image_to_buffer,
                             &copy_command);
   results.aba_materialization =
      fixtures_created && record_aba_materialization(
                             context, partial, readbacks,
                             copy_image_to_buffer, &aba_command);
   results.direct_fast_clear_read_recorded = results.aba_materialization;
   results.rollback =
      fixtures_created && failed_preparation_preserves_published_state(
                             context, &readbacks[0], copy_image_to_buffer);
   results.compressed_route_unselected =
      results.copy_materialization && results.aba_materialization &&
      command_excludes_compressed_zmask(copy_command) &&
      command_excludes_compressed_zmask(aba_command);

   if (results.copy_materialization && results.aba_materialization) {
      char copy_digest[BLAKE3_OUT_LEN * 2u + 1u];
      char aba_digest[BLAKE3_OUT_LEN * 2u + 1u];
      uint32_t copy_dwords = 0u;
      uint32_t aba_dwords = 0u;
      if (!command_digest(copy_command, copy_digest, &copy_dwords) ||
          !command_digest(aba_command, aba_digest, &aba_dwords)) {
         results.copy_materialization = false;
         results.aba_materialization = false;
      } else {
         printf("copy_ib_dwords=%u copy_ib_blake3=%s\n", copy_dwords,
                copy_digest);
         printf("aba_ib_dwords=%u aba_ib_blake3=%s\n", aba_dwords,
                aba_digest);
      }
   }

   if (hardware_scenario == HARDWARE_COPY_MATERIALIZATION &&
       results.copy_materialization)
      results.hardware = hardware_result_matches(
         context, &readbacks[0], copy_command, false, evidence_dir, "copy",
         &results);
   else if (hardware_scenario == HARDWARE_ABA_MATERIALIZATION &&
            results.aba_materialization)
      results.hardware = aba_hardware_result_matches(
         context, readbacks, aba_command, evidence_dir, &results);
   else if (hardware_scenario != HARDWARE_NONE)
      results.hardware = false;
   if (hardware_scenario != HARDWARE_NONE)
      results.compressed_route_unselected &=
         published_images_are_uncompressed(context);
   device->zmask_automatic_qualified = false;

   if (hardware_scenario != HARDWARE_NONE) {
      const char *scenario =
         hardware_scenario == HARDWARE_COPY_MATERIALIZATION
            ? "copy-materialization"
            : "aba-materialization";
      results.hardware &= results.queue_submit_calls == 1u &&
                          results.queue_submits_accepted == 1u &&
                          results.queue_executions_completed == 1u;
      results.hardware &= retain_hardware_outcome(
         evidence_dir, scenario, &results, results.hardware,
         !device->zmask_automatic_qualified);
   }
   return results;
}

static bool
lifecycle_passed(const struct lifecycle_results *results)
{
   return results->ordinary_control && results->copy_materialization &&
          results->aba_materialization && results->rollback &&
          results->hardware && results->compressed_route_unselected &&
          results->direct_fast_clear_read_recorded &&
          results->cross_command_buffer_read;
}

int
main(int argc, char **argv)
{
   enum hardware_scenario hardware_scenario = HARDWARE_NONE;
   if (argc == 3 && strcmp(argv[1], "--hardware-copy") == 0)
      hardware_scenario = HARDWARE_COPY_MATERIALIZATION;
   else if (argc == 3 && strcmp(argv[1], "--hardware-aba") == 0)
      hardware_scenario = HARDWARE_ABA_MATERIALIZATION;
   else if (argc != 1)
      return 2;
   const bool hardware_requested = hardware_scenario != HARDWARE_NONE;
   if (hardware_requested && !hardware_environment_matches(argv[2], "1"))
      return 2;
   uint32_t expected[PIXELS];
   build_expected_words(expected, IMAGE_A_CLEAR_DEPTH_CODE,
                        IMAGE_A_CLEAR_STENCIL, false);
   check_expected_words(expected, IMAGE_A_CLEAR_DEPTH_CODE,
                        IMAGE_A_CLEAR_STENCIL, false);
   build_expected_words(expected, IMAGE_A_CLEAR_DEPTH_CODE,
                        IMAGE_A_CLEAR_STENCIL, true);
   check_expected_words(expected, IMAGE_A_CLEAR_DEPTH_CODE,
                        IMAGE_A_CLEAR_STENCIL, true);
   build_expected_words(expected, IMAGE_B_CLEAR_DEPTH_CODE,
                        IMAGE_B_CLEAR_STENCIL, false);
   check_expected_words(expected, IMAGE_B_CLEAR_DEPTH_CODE,
                        IMAGE_B_CLEAR_STENCIL, false);

   struct public_context context;
   if (!create_public_context(&context, 2u) ||
       !upload_partial_triangle(&context))
      return 1;
   struct r3v_native_device *device = r3v_native_device_from_handle(context.device);
   device->vk.physical->instance->enable_debug_logging = true;
   assert(!device->zmask_automatic_qualified);
   assert(device->zmask_fast_clear_gate != NULL);
   assert(device->zmask_materialize_scratch_initialized);
   struct readback readbacks[2] = {0};
   struct partial_draw partial = {0};
   PFN_vkCmdCopyImageToBuffer copy_image_to_buffer =
      load_copy_image_to_buffer(&context);
   const struct lifecycle_results results =
      record_lifecycle(&context, readbacks, &partial, copy_image_to_buffer,
                       hardware_scenario,
                       hardware_requested ? argv[2] : NULL);
   assert(!device->zmask_automatic_qualified);
   printf("ordinary_control=%d copy_materialization=%d "
          "aba_materialization=%d rollback=%d hardware=%d "
          "compressed_route_unselected=%d queue_submit_calls=%u "
          "direct_fast_clear_read_recorded=%d "
          "direct_fast_clear_read_color_mask_exact=%d "
          "cross_command_buffer_read=%d "
          "queue_submits_accepted=%u queue_executions_completed=%u\n",
          results.ordinary_control, results.copy_materialization,
          results.aba_materialization, results.rollback, results.hardware,
          results.compressed_route_unselected, results.queue_submit_calls,
          results.direct_fast_clear_read_recorded,
          results.direct_fast_clear_read_color_mask_exact,
          results.cross_command_buffer_read,
          results.queue_submits_accepted,
          results.queue_executions_completed);
   destroy_readback(&context, &readbacks[0]);
   destroy_readback(&context, &readbacks[1]);
   destroy_partial_draw(&context, &partial);
   destroy_public_context(&context);
   if (!lifecycle_passed(&results))
      return 1;
   puts("r3v public ZMASK copy and A/B/A materialization lifecycle: OK");
   return 0;
}
