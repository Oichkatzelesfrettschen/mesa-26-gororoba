/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V command buffers: fixed-IB carriers for the device-internal
 * emitters.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "vk_alloc.h"
#include "vk_command_pool.h"

#include <stdlib.h>
#include <string.h>

static void
r3v_native_cmd_buffer_release_ib(struct r3v_native_cmd_buffer *cmd_buffer)
{
   /* radeon_drm_vk_cs_build binds the IB chunk to this pointer rather than
    * copying the dwords (rg --fixed-strings RADEON_CHUNK_ID_IB
    * src/amd/radeon/drm_vk/radeon_drm_vk_cs.c), so a prepared submission
    * names storage this free returns to the allocator.  The prepared state
    * admits a commit on command-buffer pointer equality alone, and both a
    * reset and a destroy-then-reallocate keep that pointer equal, so the IB
    * release is the point that retires the transport holding it.  A
    * host-model carrier installs an IB with no device attached, so the
    * base device decides whether a prepared submission can exist at all.
    */
   if (cmd_buffer->vk.base.device != NULL) {
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      if (device->prepared.valid && device->prepared.cmd_buffer == cmd_buffer)
         r3v_native_prepared_release(device);
   }

   free(cmd_buffer->ib);
   free(cmd_buffer->references);
   cmd_buffer->cell_kind = R3V_NATIVE_CELL_KIND_UNDECLARED;
   cmd_buffer->ib = NULL;
   cmd_buffer->ib_size_dwords = 0;
   cmd_buffer->references = NULL;
   cmd_buffer->reference_count = 0;
   cmd_buffer->burst_draws = 0;
}

void
r3v_native_cmd_buffer_release_recording(
   struct r3v_native_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->owned_carrier != NULL) {
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      radeon_drm_vk_bo_free(&device->drm, &cmd_buffer->owned_carrier->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_carrier);
      cmd_buffer->owned_carrier = NULL;
   }
   if (cmd_buffer->owned_slot != NULL) {
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      radeon_drm_vk_bo_free(&device->drm, &cmd_buffer->owned_slot->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_slot);
      cmd_buffer->owned_slot = NULL;
   }
   if (cmd_buffer->owned_multisample != NULL) {
      struct r3v_native_device *device = container_of(
         cmd_buffer->vk.base.device, struct r3v_native_device, vk);
      radeon_drm_vk_bo_free(&device->drm,
                            &cmd_buffer->owned_multisample->bo);
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->owned_multisample);
      cmd_buffer->owned_multisample = NULL;
   }
   for (uint32_t i = 0; i < cmd_buffer->deferred_copy_count; i++)
      vk_free(&cmd_buffer->vk.pool->alloc,
              cmd_buffer->deferred_copies[i].update_data);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->deferred_copies);
   cmd_buffer->deferred_copies = NULL;
   cmd_buffer->deferred_copy_capacity = 0;
   cmd_buffer->pass_target = NULL;
   cmd_buffer->pass_target_layer_offset = 0;
   cmd_buffer->bound_pipeline = NULL;
   cmd_buffer->bound_graphics_set = NULL;
   cmd_buffer->viewport_set = false;
   cmd_buffer->scissor_set = false;
   cmd_buffer->query_op_count = 0;
   cmd_buffer->active_query_pool = NULL;
   cmd_buffer->event_op_count = 0;
   memset(cmd_buffer->bound_vertex_buffers, 0,
          sizeof(cmd_buffer->bound_vertex_buffers));
   memset(cmd_buffer->bound_vertex_offsets, 0,
          sizeof(cmd_buffer->bound_vertex_offsets));
   cmd_buffer->vertex_bound_mask = 0;
   cmd_buffer->bound_index_buffer = NULL;
   cmd_buffer->bound_index_offset = 0;
   cmd_buffer->bound_index_bytes = 0;
   cmd_buffer->draw_recorded = false;
   cmd_buffer->deferred_draw = (struct r3v_native_deferred_draw){0};
   cmd_buffer->deferred_copy_count = 0;
   cmd_buffer->bound_compute_pipeline = NULL;
   cmd_buffer->bound_compute_set = NULL;
   vk_free(&cmd_buffer->vk.pool->alloc,
           cmd_buffer->deferred_dispatch.gpu_expected);
   cmd_buffer->deferred_dispatch =
      (struct r3v_native_deferred_dispatch){0};
}

void
r3v_native_cmd_buffer_install_ib(struct r3v_native_cmd_buffer *cmd_buffer,
                                 enum r3v_native_cell_kind kind,
                                 uint32_t *ib, uint32_t ib_size_dwords,
                                 struct r3v_native_bo_reference *references,
                                 uint32_t reference_count)
{
   r3v_native_cmd_buffer_release_ib(cmd_buffer);
   cmd_buffer->cell_kind = kind;
   cmd_buffer->ib = ib;
   cmd_buffer->ib_size_dwords = ib_size_dwords;
   cmd_buffer->references = references;
   cmd_buffer->reference_count = reference_count;
}

static VkResult
r3v_native_cmd_buffer_create(struct vk_command_pool *pool,
                             VkCommandBufferLevel level,
                             struct vk_command_buffer **cmd_buffer_out)
{
   struct r3v_native_cmd_buffer *cmd_buffer =
      vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = vk_command_buffer_init(pool, &cmd_buffer->vk,
                                            &r3v_native_cmd_buffer_ops,
                                            level);
   if (result != VK_SUCCESS) {
      vk_free(&pool->alloc, cmd_buffer);
      return result;
   }

   *cmd_buffer_out = &cmd_buffer->vk;
   return VK_SUCCESS;
}

static void
r3v_native_cmd_buffer_reset(struct vk_command_buffer *cmd_buffer_base,
                            UNUSED VkCommandBufferResetFlags flags)
{
   struct r3v_native_cmd_buffer *cmd_buffer =
      container_of(cmd_buffer_base, struct r3v_native_cmd_buffer, vk);
   vk_command_buffer_reset(&cmd_buffer->vk);
   r3v_native_cmd_buffer_release_ib(cmd_buffer);
   r3v_native_cmd_buffer_release_recording(cmd_buffer);
}

static void
r3v_native_cmd_buffer_destroy(struct vk_command_buffer *cmd_buffer_base)
{
   struct r3v_native_cmd_buffer *cmd_buffer =
      container_of(cmd_buffer_base, struct r3v_native_cmd_buffer, vk);
   r3v_native_cmd_buffer_release_ib(cmd_buffer);
   r3v_native_cmd_buffer_release_recording(cmd_buffer);
   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
}

const struct vk_command_buffer_ops r3v_native_cmd_buffer_ops = {
   .create = r3v_native_cmd_buffer_create,
   .reset = r3v_native_cmd_buffer_reset,
   .destroy = r3v_native_cmd_buffer_destroy,
};

/* The runtime lifecycle carries the fail-closed recording contract: begin
 * enters RECORDING (an implicit re-begin resets, releasing any installed
 * IB), a poisoned recording ends INVALID with its recorded error returned,
 * and the queue admits only EXECUTABLE buffers.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_begin(cmd_buffer, pBeginInfo);
   return vk_command_buffer_get_record_result(cmd_buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);

   /* A render pass left open has no closing lowering, and a query
    * left active has no end publishing its availability, so the buffer
    * poisons instead of becoming executable with either incomplete.
    */
   if (cmd_buffer->pass_target != NULL ||
       cmd_buffer->active_query_pool != NULL) {
      vk_command_buffer_set_error(&cmd_buffer->vk,
                                  R3V_NATIVE_REFUSAL_RESULT);
   }
   return vk_command_buffer_end(&cmd_buffer->vk);
}
