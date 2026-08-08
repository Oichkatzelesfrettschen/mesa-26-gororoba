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

static void
r3v_native_cmd_buffer_release_ib(struct r3v_native_cmd_buffer *cmd_buffer)
{
   free(cmd_buffer->ib);
   free(cmd_buffer->references);
   cmd_buffer->ib = NULL;
   cmd_buffer->ib_size_dwords = 0;
   cmd_buffer->references = NULL;
   cmd_buffer->reference_count = 0;
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
   cmd_buffer->pass_target = NULL;
   cmd_buffer->bound_pipeline = NULL;
   cmd_buffer->bound_vertex_buffer = NULL;
   cmd_buffer->bound_vertex_offset = 0;
   cmd_buffer->vertex_bound = false;
   cmd_buffer->draw_recorded = false;
   cmd_buffer->deferred_draw = (struct r3v_native_deferred_draw){0};
}

void
r3v_native_cmd_buffer_install_ib(struct r3v_native_cmd_buffer *cmd_buffer,
                                 uint32_t *ib, uint32_t ib_size_dwords,
                                 struct r3v_native_bo_reference *references,
                                 uint32_t reference_count)
{
   r3v_native_cmd_buffer_release_ib(cmd_buffer);
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
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   return vk_command_buffer_end(cmd_buffer);
}
