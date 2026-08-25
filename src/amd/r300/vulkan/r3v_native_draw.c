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
   /* An active occlusion query's zero count is exact only while no
    * fragment-producing span records, so the pass refuses inside one.
    */
   if (cmd_buffer->pass_target != NULL || cmd_buffer->draw_recorded ||
       cmd_buffer->active_query_pool != NULL ||
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
    * declares.  The pass's attachment format names the lane order the
    * cell emits into US_OUT_FMT_0, so it holds equal to the attached
    * image's own format.
    */
   VK_FROM_HANDLE(r3v_native_image_view, view, framebuffer->attachments[0]);
   const VkRect2D *area = &pRenderPassBegin->renderArea;
   if (view == NULL || view->image == NULL ||
       view->image->memory == NULL ||
       pass->attachments[0].format != view->image->format ||
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
      .target_fill_bytes = r3v_native_render_footprint_bytes(
         view->image->row_pitch_bytes, view->image->height),
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

/* The index types the host dereference reads: UINT16 and UINT32, the
 * two Vulkan 1.0 types.  UINT8 rides an extension the device withholds
 * and NONE names no buffer, so both refuse.  The bind offset is a
 * multiple of the index size (the Vulkan bind rule), and the whole
 * call is checked before any state changes.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                       VkDeviceSize offset, VkIndexType indexType)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(r3v_native_buffer, buffer, _buffer);

   const uint32_t index_bytes = indexType == VK_INDEX_TYPE_UINT16   ? 2
                                : indexType == VK_INDEX_TYPE_UINT32 ? 4
                                                                     : 0;
   if (index_bytes == 0 || buffer == NULL || buffer->memory == NULL ||
       offset > buffer->vk.size || offset % index_bytes != 0) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   cmd_buffer->bound_index_buffer = buffer;
   cmd_buffer->bound_index_offset = offset;
   cmd_buffer->bound_index_bytes = index_bytes;
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
      target_lo + r3v_native_render_footprint_bytes(target->row_pitch_bytes,
                                                   target->height);
   return lo < target_hi && target_lo < lo + bytes;
}

/* The draw arguments the two draw entry points lower: a linear draw
 * names its first vertex; an indexed draw names the first index and the
 * base vertex and reads the bound index buffer at execution; both name
 * the instance range the host expands.
 */
struct draw_args {
   bool indexed;
   uint32_t vertex_count;
   uint32_t first_vertex;
   uint32_t first_index;
   int32_t vertex_offset;
   uint32_t first_instance;
   uint32_t instance_count;
};

/* The draw lowering realizes the CPU_VERTEX node and the fixed cell:
 * recording installs the cell IB against a command-buffer-owned GTT
 * carrier and the pass target's memory, and the vertex gather plus the
 * load-op clear ride deferred_draw to queue submission, so resource
 * reads and the clear carry execution-time semantics.  The cell
 * family draws whole triangles, so the accepted draw arguments are
 * that shape: a linear draw of vertex_count vertices (a positive
 * multiple of three) from any firstVertex the bound range proves
 * readable, or an indexed draw whose vertex_count indices the record
 * proves readable, over instance_count instances from first_instance
 * that the host expands into the family's vertex_count *
 * instance_count vertex list.
 */
static void
record_draw(VkCommandBuffer commandBuffer, const struct draw_args *args)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   struct r3v_native_device *device = container_of(
      cmd_buffer->vk.base.device, struct r3v_native_device, vk);
   struct r3v_native_pipeline *pipeline = cmd_buffer->bound_pipeline;

   /* A recorded attachment clear executes on the host after the
    * load-op clear, and the device draw executes after every host
    * write, so a clear-then-draw pass would reorder the clear over the
    * draw's output; the pass carries one or the other.
    */
   if (cmd_buffer->pass_target == NULL || pipeline == NULL ||
       cmd_buffer->draw_recorded ||
       cmd_buffer->deferred_draw.clear_rect_count != 0) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   /* An indexed draw reads its indices from the bound index
    * buffer at execution, so the record proves the index range alone:
    * the buffer is bound, the range from first_index closes inside the
    * buffer (robustBufferAccess covers vertex records; the index range
    * itself has no robust form in this device, so it refuses), the
    * buffer closes inside its memory, and the range shares no byte with
    * the pass target, the same host-read-before-clear hazard a vertex
    * stream carries.  The vertex numbers the indices select are judged
    * at execution under the robust rule.
    */
   struct r3v_native_buffer *index_buffer = NULL;
   uint64_t index_base = 0;
   if (args->indexed) {
      index_buffer = cmd_buffer->bound_index_buffer;
      const uint32_t index_bytes = cmd_buffer->bound_index_bytes;
      if (index_buffer == NULL || index_bytes == 0) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
      struct r3v_native_memory *memory = index_buffer->memory;
      index_base = cmd_buffer->bound_index_offset;
      if (index_base > index_buffer->vk.size ||
          index_buffer->offset > memory->bo.size ||
          index_buffer->vk.size > memory->bo.size - index_buffer->offset ||
          ((uint64_t)args->first_index + args->vertex_count) * index_bytes >
             index_buffer->vk.size - index_base ||
          stream_overlaps_target(memory, index_buffer->offset + index_base,
                                 index_buffer->vk.size - index_base,
                                 cmd_buffer->pass_target)) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
   }

   /* The pipeline's viewport/scissor claim and the pass target carry
    * one extent: the cell's scissor words resolve from it once.  A
    * dynamic-state pipeline resolves its extent from the recorded
    * vkCmdSet state instead, held to the same cell shape the static
    * admission pins -- unset state, a shifted origin, a non-identity
    * depth range, or a viewport diverging from the scissor extent each
    * refuses.
    */
   uint32_t claim_width = pipeline->target_width;
   uint32_t claim_height = pipeline->target_height;
   if (pipeline->dynamic_viewport_scissor) {
      const VkViewport *viewport = &cmd_buffer->dynamic_viewport;
      const VkRect2D *scissor = &cmd_buffer->dynamic_scissor;
      claim_width = scissor->extent.width;
      claim_height = scissor->extent.height;
      if (!cmd_buffer->viewport_set || !cmd_buffer->scissor_set ||
          scissor->offset.x != 0 || scissor->offset.y != 0 ||
          claim_width < 1 || claim_width > R3V_NATIVE_RENDER_MAX_EXTENT ||
          claim_height < 1 || claim_height > R3V_NATIVE_RENDER_MAX_EXTENT ||
          viewport->x != 0.0f || viewport->y != 0.0f ||
          viewport->width != (float)claim_width ||
          viewport->height != (float)claim_height ||
          viewport->minDepth != 0.0f || viewport->maxDepth != 1.0f) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
   }
   if (claim_width != cmd_buffer->pass_target->width ||
       claim_height != cmd_buffer->pass_target->height) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }

   /* Every attribute slot the job reads resolves to a stream: its
    * binding is bound, the readable range is the bound buffer past the
    * bind and attribute offsets, the buffer range closes inside the
    * bound memory,
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
      /* The last record the draw reads from this stream: an
       * instance-rate binding reads records first_instance .. first_instance
       * + instance_count - 1 at the core divisor of one; a per-vertex
       * binding of a linear draw reads first_vertex .. first_vertex + 2,
       * and an indexed draw's vertex numbers are judged at execution. */
      const bool instance_rate =
         (pipeline->instance_rate_bindings & (1u << attribute->binding)) != 0;
      const bool record_bound_proved = instance_rate || !args->indexed;
      const uint64_t last_record =
         instance_rate
            ? (uint64_t)args->first_instance + args->instance_count - 1
            : (uint64_t)args->first_vertex + args->vertex_count - 1;
      if (record_bound_proved &&
          !device->vk.enabled_features.robustBufferAccess &&
          (available < format->semantic_record_bytes ||
           last_record * stride > available - format->semantic_record_bytes)) {
         poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
         return;
      }
      streams[slot] = (struct r3v_native_deferred_stream){
         .buffer = buffer,
         .stream_base = stream_base,
         .stride = stride,
         .format_id = attribute->format_id,
         .instance_rate = instance_rate,
         .instance_divisor = 1,
      };
   }

   /* The carrier descriptor is recording-lifetime state, so it rides
    * the command pool's allocator and a custom host-memory policy
    * covers it with the command buffer itself.  The carrier BO holds
    * the 3 * instance_count records the host expansion writes, in whole
    * GTT pages; one page carries the reference three.
    */
   struct r3v_native_memory *carrier =
      vk_zalloc(&cmd_buffer->vk.pool->alloc, sizeof(*carrier), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (carrier == NULL) {
      poison(commandBuffer, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   const uint64_t carrier_record_bytes =
      4 * r300_vertex_job_record_dwords(&pipeline->vertex_job);
   const uint64_t carrier_bytes =
      ((uint64_t)args->vertex_count * args->instance_count *
          carrier_record_bytes +
       4095) &
      ~(uint64_t)4095;
   if (radeon_drm_vk_bo_create(&device->drm, carrier_bytes,
                               R3V_NATIVE_MEMORY_ALIGNMENT,
                               RADEON_GEM_DOMAIN_GTT, 0, false,
                               &carrier->bo) != 0) {
      vk_free(&cmd_buffer->vk.pool->alloc, carrier);
      poison(commandBuffer, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   VkResult result = r3v_native_record_tcl_bypass_triangle_carrier(
      device, cmd_buffer, carrier, cmd_buffer->pass_target,
      pipeline->varying,
      (args->vertex_count / 3) * args->instance_count,
      pipeline->color_bits);
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
      .first_vertex = args->first_vertex,
      .vertex_count = args->vertex_count,
      .indexed = args->indexed,
      .index_buffer = index_buffer,
      .index_base = index_base,
      .index_bytes = args->indexed ? cmd_buffer->bound_index_bytes : 0,
      .first_index = args->first_index,
      .vertex_offset = args->vertex_offset,
      .first_instance = args->first_instance,
      .instance_count = args->instance_count,
      .cull_mode = pipeline->cull_mode,
      .front_face = pipeline->front_face,
      .sample_mask_zero = pipeline->sample_mask_zero,
      .vertex_job = pipeline->vertex_job,
      .vertex_job_identity = pipeline->gpu_vertex_job_identity,
      .target_memory = cmd_buffer->pass_target->memory,
      .target_fill_bytes = r3v_native_render_footprint_bytes(
         cmd_buffer->pass_target->row_pitch_bytes,
         cmd_buffer->pass_target->height),
      .target_width = cmd_buffer->pass_target->width,
      .target_height = cmd_buffer->pass_target->height,
   };
   memcpy(cmd_buffer->deferred_draw.streams, streams, sizeof(streams));
   cmd_buffer->draw_recorded = true;
}

/* The instance range both draws accept: at least one instance (the cell
 * family has no empty member; a zero count draws nothing and refuses
 * rather than recording a cell that draws), at most the family's
 * triangle ceiling, from any firstInstance -- the per-instance streams
 * and the InstanceIndex builtin carry it, the Vulkan semantics in which
 * the base instance is part of the instance index.
 */
static bool
instance_range_admitted(uint32_t instanceCount)
{
   return instanceCount >= 1 && instanceCount <= R3V_NATIVE_MAX_DRAW_INSTANCES;
}

/* The vertex-list shape both draws accept: whole triangles (a count
 * that is a positive multiple of three), expanded per instance, with
 * the triangle product inside the cell family's ceiling -- the
 * family's vertex list tops out at 3 * R3V_NATIVE_MAX_DRAW_INSTANCES
 * vertices, the VAP_VF_MAX_VTX_INDX bound.
 */
static bool
list_shape_admitted(uint32_t vertexCount, uint32_t instanceCount)
{
   if (vertexCount < 3 || vertexCount % 3 != 0 ||
       !instance_range_admitted(instanceCount))
      return false;
   return (uint64_t)(vertexCount / 3) * instanceCount <=
          R3V_NATIVE_MAX_DRAW_INSTANCES;
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
            uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance)
{
   if (!list_shape_admitted(vertexCount, instanceCount)) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   record_draw(commandBuffer, &(struct draw_args){
                                 .vertex_count = vertexCount,
                                 .first_vertex = firstVertex,
                                 .first_instance = firstInstance,
                                 .instance_count = instanceCount,
                              });
}

/* The indexed draw keeps the family's whole-triangle shape: indexCount
 * indices from first_index, dereferenced on the host at execution into
 * the carrier records the consumer draws linearly, with vertexOffset
 * summed onto each index.  The pipeline admission already holds
 * primitive restart disabled, so the restart value is an ordinary
 * index here.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                   uint32_t instanceCount, uint32_t firstIndex,
                   int32_t vertexOffset, uint32_t firstInstance)
{
   if (!list_shape_admitted(indexCount, instanceCount)) {
      poison(commandBuffer, R3V_NATIVE_REFUSAL_RESULT);
      return;
   }
   record_draw(commandBuffer, &(struct draw_args){
                                 .indexed = true,
                                 .vertex_count = indexCount,
                                 .first_index = firstIndex,
                                 .vertex_offset = vertexOffset,
                                 .first_instance = firstInstance,
                                 .instance_count = instanceCount,
                              });
}
