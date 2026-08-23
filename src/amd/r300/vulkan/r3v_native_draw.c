/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V public draw surface: render-pass begin/end, pipeline and
 * vertex-buffer binds, and the draw lowering into the qualified cell.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"

#include "vk_framebuffer.h"
#include "vk_log.h"
#include "vk_render_pass.h"

#include <radeon_drm.h>
#include "vk_alloc.h"
#include <string.h>

static void
poison(VkCommandBuffer commandBuffer, VkResult error)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_set_error(cmd_buffer, error);
}

/* The clear value the cell realizes: the sentinel fill 0xa5a5a5a5,
 * which in B8G8R8A8_UNORM is 0xa5 in every channel.  The comparison is
 * exact against the identically evaluated expression: 165.0f/255.0f
 * rounds to one binary32 value, the application writes that same
 * expression, and both sides land on the same bits, so a different
 * clear color refuses rather than silently clearing to the sentinel.
 */
static bool
clear_is_sentinel(const VkClearValue *value)
{
   const float sentinel = (float)0xa5 / 255.0f;
   for (unsigned c = 0; c < 4; c++) {
      if (value->color.float32[c] != sentinel)
         return false;
   }
   return true;
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                       const VkRenderPassBeginInfo *pRenderPassBegin,
                       VkSubpassContents contents)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(vk_render_pass, pass, pRenderPassBegin->renderPass);
   VK_FROM_HANDLE(vk_framebuffer, framebuffer,
                  pRenderPassBegin->framebuffer);

   /* The bounded contract is one pass with at most one draw per command
    * buffer; a second pass would need a second clear and draw lowering the
    * cell does not carry, so it refuses instead of recording a pass whose
    * load op never executes.
    */
   if (cmd_buffer->pass_target != NULL || cmd_buffer->draw_recorded ||
       cmd_buffer->deferred_draw.pending ||
       cmd_buffer->deferred_copy_count != 0 ||
       contents != VK_SUBPASS_CONTENTS_INLINE ||
       !r3v_native_render_pass_matches_cell(pass) || framebuffer == NULL ||
       framebuffer->layers != 1 || framebuffer->attachment_count != 1) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   /* The framebuffer and render area name the attached image's own
    * extent in full: the load-op clear realizes over the image
    * footprint, so a partial render area would clear more than it
    * declares.
    */
   VK_FROM_HANDLE(r3v_native_image_view, view, framebuffer->attachments[0]);
   const VkRect2D *area = &pRenderPassBegin->renderArea;
   if (view == NULL || view->image == NULL ||
       view->image->memory == NULL ||
       framebuffer->width != view->image->width ||
       framebuffer->height != view->image->height ||
       area->offset.x != 0 || area->offset.y != 0 ||
       area->extent.width != view->image->width ||
       area->extent.height != view->image->height ||
       pRenderPassBegin->clearValueCount < 1 ||
       !clear_is_sentinel(&pRenderPassBegin->pClearValues[0])) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   cmd_buffer->pass_target = view->image;
   cmd_buffer->deferred_draw = (struct r3v_native_deferred_draw){
      .pending = true,
      .target_memory = view->image->memory,
      .target_fill_bytes =
         r3v_native_image_footprint_bytes(view->image->height),
      .target_width = view->image->width,
      .target_height = view->image->height,
   };
}

/* A pass with no draw still carries its load-op clear through deferred_draw.
 * The zero-IB queue path executes that clear, while a draw extends the same
 * record with the carrier and vertex stream needed by the cell.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (cmd_buffer->pass_target == NULL) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   cmd_buffer->pass_target = NULL;
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindPipeline(VkCommandBuffer commandBuffer,
                    VkPipelineBindPoint pipelineBindPoint,
                    VkPipeline _pipeline)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_pipeline, pipeline, _pipeline);

   if (pipeline == NULL) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   /* Each bind point admits its own pipeline kind alone, so a compute
    * pipeline can never stand where the draw lowering reads graphics
    * state, and the reverse bind refuses the same way.
    */
   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS &&
       !pipeline->is_compute) {
      cmd_buffer->bound_pipeline = pipeline;
      return;
   }
   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE &&
       pipeline->is_compute) {
      cmd_buffer->bound_compute_pipeline = pipeline;
      return;
   }
   poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                         uint32_t firstBinding, uint32_t bindingCount,
                         const VkBuffer *pBuffers,
                         const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (bindingCount == 0 || firstBinding >= R3V_NATIVE_MAX_VERTEX_BINDINGS ||
       bindingCount > R3V_NATIVE_MAX_VERTEX_BINDINGS - firstBinding) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   /* Every binding of the call is checked before any is installed, so a
    * refused call leaves the bound state as it was. */
   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(r3v_native_buffer, buffer, pBuffers[i]);
      if (buffer == NULL || buffer->memory == NULL ||
          pOffsets[i] > buffer->vk.size) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
   }
   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(r3v_native_buffer, buffer, pBuffers[i]);
      cmd_buffer->bound_vertex_buffers[firstBinding + i] = buffer;
      cmd_buffer->bound_vertex_offsets[firstBinding + i] = pOffsets[i];
      cmd_buffer->vertex_bound_mask |= 1u << (firstBinding + i);
   }
}

/* True when the byte range [lo, lo + bytes) of one bound memory shares
 * a byte with the pass target's footprint in the same buffer object.
 * The draw's load-op clear and color writes land in that footprint
 * and the vertex executor reads the stream on the host ahead of the
 * ioctl, so a stream inside it has no defined order against the clear
 * the Vulkan render pass sequences before the draw.
 */
static bool
stream_overlaps_target(const struct r3v_native_memory *memory, uint64_t lo,
                       uint64_t bytes, const struct r3v_native_image *target)
{
   if (memory->bo.handle != target->memory->bo.handle)
      return false;
   const uint64_t target_lo = target->memory_offset;
   const uint64_t target_hi =
      target_lo + r3v_native_image_footprint_bytes(target->height);
   return lo < target_hi && target_lo < lo + bytes;
}

/* The draw lowering realizes the CPU_VERTEX node and the fixed cell:
 * recording installs the cell IB against a command-buffer-owned GTT
 * carrier and the pass target's memory, and the vertex gather plus the
 * load-op clear ride deferred_draw to queue submission, so resource
 * reads and the clear carry execution-time semantics.  The cell draws
 * exactly three vertices in one instance, so the accepted draw
 * arguments are that shape with any firstVertex the bound range proves
 * readable.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
            uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   struct r3v_native_pipeline *pipeline = cmd_buffer->bound_pipeline;

   if (cmd_buffer->pass_target == NULL || pipeline == NULL ||
       cmd_buffer->draw_recorded || vertexCount != 3 ||
       instanceCount != 1 || firstInstance != 0) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   /* The pipeline's viewport/scissor claim and the pass target carry
    * one extent: the cell's scissor words resolve from it once.
    */
   if (pipeline->target_width != cmd_buffer->pass_target->width ||
       pipeline->target_height != cmd_buffer->pass_target->height) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   /* Every attribute slot the job reads resolves to a stream: its
    * binding is bound, the readable range is the bound buffer past the
    * bind and attribute offsets, the buffer range closes inside the
    * bound memory (BindBufferMemory2 recorded it without validating),
    * and the range shares no byte with the pass target.  The early
    * bound proof at recording is the same arithmetic the gather
    * enforces at submission: the last requested record closes inside
    * the readable range, so a stream the execution would refuse
    * poisons here where the application still sees the recording
    * error.  With robustBufferAccess enabled the execution reads an
    * out-of-bounds record as zeros instead, so the draw records.
    */
   struct r3v_native_deferred_stream streams[R300_VERTEX_JOB_MAX_INPUTS] = {
      0
   };
   for (uint32_t slot = 0; slot < R300_VERTEX_JOB_MAX_INPUTS; slot++) {
      if (!(pipeline->attribute_mask & (1u << slot)))
         continue;
      const struct r3v_native_vertex_attribute *attribute =
         &pipeline->attributes[slot];
      if (!(cmd_buffer->vertex_bound_mask & (1u << attribute->binding))) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
      struct r3v_native_buffer *buffer =
         cmd_buffer->bound_vertex_buffers[attribute->binding];
      struct r3v_native_memory *memory = buffer->memory;
      const uint64_t stream_base =
         (uint64_t)cmd_buffer->bound_vertex_offsets[attribute->binding] +
         attribute->offset;
      if (stream_base > buffer->vk.size ||
          buffer->offset > memory->bo.size ||
          buffer->vk.size > memory->bo.size - buffer->offset) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
      const uint64_t available = buffer->vk.size - stream_base;
      if (stream_overlaps_target(memory, buffer->offset + stream_base,
                                 available, cmd_buffer->pass_target)) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
      const struct r300_vertex_format_semantics *format =
         r300_vertex_format_semantics(
            (enum r300_vertex_format_id)attribute->format_id);
      const uint32_t stride = pipeline->binding_strides[attribute->binding];
      if (format == NULL) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
      if (!device->vk.enabled_features.robustBufferAccess &&
          (available < format->semantic_record_bytes ||
           ((uint64_t)firstVertex + 2) * stride >
              available - format->semantic_record_bytes)) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
      streams[slot] = (struct r3v_native_deferred_stream){
         .buffer = buffer,
         .stream_base = stream_base,
         .stride = stride,
         .format_id = attribute->format_id,
      };
   }

   /* The carrier descriptor is recording-lifetime state, so it rides
    * the command pool's allocator and a custom host-memory policy
    * covers it with the command buffer itself.
    */
   struct r3v_native_memory *carrier =
      vk_zalloc(&cmd_buffer->vk.pool->alloc, sizeof(*carrier), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (carrier == NULL) {
      poison(commandBuffer, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   if (radeon_drm_vk_bo_create(&device->drm, 4096,
                               R3V_NATIVE_MEMORY_ALIGNMENT,
                               RADEON_GEM_DOMAIN_GTT, 0, false,
                               &carrier->bo) != 0) {
      vk_free(&cmd_buffer->vk.pool->alloc, carrier);
      poison(commandBuffer, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   VkResult result = r3v_native_record_tcl_bypass_triangle_carrier(
      device, cmd_buffer, carrier, cmd_buffer->pass_target,
      pipeline->varying);
   if (result != VK_SUCCESS) {
      radeon_drm_vk_bo_free(&device->drm, &carrier->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, carrier);
      poison(commandBuffer, result);
      return;
   }

   cmd_buffer->owned_carrier = carrier;
   cmd_buffer->deferred_draw = (struct r3v_native_deferred_draw){
      .pending = true,
      .stream_mask = pipeline->attribute_mask,
      .first_vertex = firstVertex,
      .vertex_job = pipeline->vertex_job,
      .vertex_job_identity = pipeline->gpu_vertex_job_identity,
      .target_memory = cmd_buffer->pass_target->memory,
      .target_fill_bytes =
         r3v_native_image_footprint_bytes(cmd_buffer->pass_target->height),
      .target_width = cmd_buffer->pass_target->width,
      .target_height = cmd_buffer->pass_target->height,
   };
   memcpy(cmd_buffer->deferred_draw.streams, streams, sizeof(streams));
   cmd_buffer->draw_recorded = true;
}
