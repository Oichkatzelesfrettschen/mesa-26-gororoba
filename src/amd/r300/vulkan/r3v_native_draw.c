/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V public draw surface: render-pass begin/end, pipeline and
 * vertex-buffer binds, and the draw lowering into the qualified cell.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "vk_framebuffer.h"
#include "vk_log.h"
#include "vk_render_pass.h"

#include <radeon_drm.h>
#include <stdlib.h>
#include <string.h>

static void
poison(VkCommandBuffer commandBuffer, VkResult error)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_set_error(cmd_buffer, error);
}

/* The clear value the cell realizes: the sentinel fill 0xa5a5a5a5,
 * which in B8G8R8A8_UNORM is 0xa5 in every channel.  The comparison is
 * exact -- 165/255 is representable and the application writes the
 * same expression -- so a different clear color refuses rather than
 * silently clearing to the sentinel.
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

   if (cmd_buffer->pass_target != NULL ||
       contents != VK_SUBPASS_CONTENTS_INLINE ||
       !r3v_native_render_pass_matches_cell(pass) || framebuffer == NULL ||
       framebuffer->width != R3V_NATIVE_TARGET_WIDTH ||
       framebuffer->height != R3V_NATIVE_TARGET_HEIGHT ||
       framebuffer->layers != 1 || framebuffer->attachment_count != 1) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   VK_FROM_HANDLE(r3v_native_image_view, view, framebuffer->attachments[0]);
   const VkRect2D *area = &pRenderPassBegin->renderArea;
   if (view == NULL || view->image == NULL ||
       view->image->memory == NULL || area->offset.x != 0 ||
       area->offset.y != 0 ||
       area->extent.width != R3V_NATIVE_TARGET_WIDTH ||
       area->extent.height != R3V_NATIVE_TARGET_HEIGHT ||
       pRenderPassBegin->clearValueCount < 1 ||
       !clear_is_sentinel(&pRenderPassBegin->pClearValues[0])) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   cmd_buffer->pass_target = view->image;
}

/* The bounded pass contract is one draw: the cell's IB carries exactly
 * one draw packet, so a pass that recorded none has no lowering and
 * refuses at its close.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (cmd_buffer->pass_target == NULL || !cmd_buffer->draw_recorded) {
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

   if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
       pipeline == NULL) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   cmd_buffer->bound_pipeline = pipeline;
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                         uint32_t firstBinding, uint32_t bindingCount,
                         const VkBuffer *pBuffers,
                         const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   if (firstBinding != 0 || bindingCount != 1) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   VK_FROM_HANDLE(r3v_native_buffer, buffer, pBuffers[0]);
   if (buffer == NULL || buffer->memory == NULL ||
       pOffsets[0] > buffer->vk.size) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   cmd_buffer->bound_vertex_buffer = buffer;
   cmd_buffer->bound_vertex_offset = pOffsets[0];
   cmd_buffer->vertex_bound = true;
}

/* The draw lowering realizes the CPU_VERTEX node and the fixed cell in
 * one step: the bound buffer's records gather through the byte-defined
 * executor into a command-buffer-owned GTT carrier, and the cell
 * records against that carrier and the pass target's memory.  The cell
 * draws exactly three vertices in one instance, so the accepted draw
 * arguments are that shape with any firstVertex the bound range
 * proves readable.
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
   struct r3v_native_buffer *buffer = cmd_buffer->bound_vertex_buffer;

   if (cmd_buffer->pass_target == NULL || pipeline == NULL ||
       !cmd_buffer->vertex_bound || cmd_buffer->draw_recorded ||
       vertexCount != 3 || instanceCount != 1 || firstInstance != 0) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   /* The readable stream range is the bound buffer past the bind and
    * attribute offsets; the gather then proves each requested record
    * against it.  The buffer range itself must close inside the bound
    * memory, which BindBufferMemory2 recorded without validating.
    */
   struct r3v_native_memory *memory = buffer->memory;
   uint64_t stream_base = (uint64_t)cmd_buffer->bound_vertex_offset +
                          pipeline->attribute_offset;
   if (stream_base > buffer->vk.size ||
       buffer->offset > memory->bo.size ||
       buffer->vk.size > memory->bo.size - buffer->offset) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   bool owns_map = memory->map == NULL;
   if (owns_map &&
       radeon_drm_vk_bo_map(&device->drm, &memory->bo, &memory->map) != 0) {
      poison(commandBuffer, VK_ERROR_MEMORY_MAP_FAILED);
      return;
   }

   struct r3v_native_memory *carrier = calloc(1, sizeof(*carrier));
   if (carrier == NULL ||
       radeon_drm_vk_bo_create(&device->drm, 4096, 4096,
                               RADEON_GEM_DOMAIN_GTT, 0, false,
                               &carrier->bo) != 0) {
      free(carrier);
      if (owns_map) {
         radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
         memory->map = NULL;
      }
      poison(commandBuffer, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   const struct r3v_native_vertex_stream_desc stream = {
      .records = (const uint8_t *)memory->map + buffer->offset + stream_base,
      .size_bytes = buffer->vk.size - stream_base,
      .stride = pipeline->binding_stride,
      .first_vertex = firstVertex,
      .format_id = pipeline->format_id,
   };
   VkResult result = r3v_native_record_tcl_bypass_triangle_gathered(
      device, cmd_buffer, carrier, cmd_buffer->pass_target->memory,
      &stream);

   if (owns_map) {
      radeon_drm_vk_bo_unmap(&device->drm, &memory->bo, memory->map);
      memory->map = NULL;
   }

   if (result != VK_SUCCESS) {
      radeon_drm_vk_bo_free(&device->drm, &carrier->bo);
      free(carrier);
      poison(commandBuffer, result);
      return;
   }

   cmd_buffer->owned_carrier = carrier;
   cmd_buffer->draw_recorded = true;
}
