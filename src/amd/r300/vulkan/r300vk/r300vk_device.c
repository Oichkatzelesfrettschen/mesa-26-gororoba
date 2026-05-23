/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_device.h"
#include "r300vk_cmd_buffer.h"

#include "r300vk_entrypoints.h"
#include "r300vk_physical_device.h"
#include "r300vk_private.h"

#include "vk_alloc.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_command_buffer.h"
#include "vk_common_entrypoints.h"
#include "vk_log.h"
#include "wsi_common.h"

#include "util/os_time.h"

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "winsys/radeon_winsys.h"
#include "r300/r300_public.h"


VkResult
r300vk_CreateDevice(VkPhysicalDevice physicalDevice,
                    const VkDeviceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkDevice *pDevice)
{
   VK_FROM_HANDLE(r300vk_physical_device, pdevice, physicalDevice);
   struct r300vk_device *device;
   VkResult result;

   device = vk_zalloc2(&pdevice->vk.instance->alloc, pAllocator,
                        sizeof(*device), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(pdevice, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Three-table dispatch: secondary command buffer support is implemented
    * by recording into r300vk_cmd_buffer (vk_cmd_enqueue path), then
    * replaying recorded entries against the pipe_context at submit time.
    * The wsi_device_entrypoints table is last so the WSI common
    * implementation fills any present-related entrypoint the driver does
    * not override. */
   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_cmd_enqueue_unless_primary_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &r300vk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_device_entrypoints, false);

   result = vk_device_init(&device->vk, &pdevice->vk, &dispatch_table,
                            pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return result;
   }

   /* Command dispatch table for secondary command buffer replay. */
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch_table,
                                             &r300vk_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch_table,
                                             &vk_common_device_entrypoints, false);
   device->vk.command_dispatch_table = &device->command_dispatch_table;
   device->vk.command_buffer_ops = &r300vk_cmd_buffer_ops;

   /* Initialize the Gallium-mediated r300g backend.
    * radeon_drm_winsys_create() opens a new fd reference to the render node
    * and populates rws->screen with the r300 pipe_screen.  The ICD owns
    * both from this point; screen->destroy() releases both when called from
    * DestroyDevice.  r300g routes NIR shader states through
    * r300_nir_to_rc_direct internally; the ICD never calls nir_to_tgsi. */
   struct pipe_screen_config screen_config = {0};
   device->rws = radeon_drm_winsys_create(pdevice->render_node_fd,
                                           &screen_config,
                                           r300_screen_create);
   if (!device->rws) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "r300vk: radeon_drm_winsys_create failed on fd %d",
                         pdevice->render_node_fd);
      goto fail_device;
   }
   device->screen = device->rws->screen;

   device->pipe = device->screen->context_create(device->screen, NULL, 0);
   if (!device->pipe) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "r300vk: pipe_screen->context_create failed");
      goto fail_screen;
   }

   /* One graphics-plus-transfer queue.  RS482/RS485 has no hardware vertex
    * processor; the vertex stage runs through Gallium Draw software TCL.
    * No compute queue is exposed. */
   result = vk_queue_init(&device->queue.vk, &device->vk,
                           &pCreateInfo->pQueueCreateInfos[0], 0);
   if (result != VK_SUCCESS)
      goto fail_pipe;

   device->queue.vk.driver_submit = r300vk_queue_driver_submit;

   *pDevice = r300vk_device_to_handle(device);
   return VK_SUCCESS;

fail_pipe:
   device->pipe->destroy(device->pipe);
fail_screen:
   device->screen->destroy(device->screen);
fail_device:
   vk_device_finish(&device->vk);
   vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
   return result;
}

void
r300vk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   if (!device)
      return;

   /* Flush and wait before tearing down the pipe_context.  This ensures
    * any in-flight CS submitted through r300g is retired before the
    * winsys and its DRM fd references are released. */
   struct pipe_fence_handle *fence = NULL;
   device->pipe->flush(device->pipe, &fence, 0);
   if (fence) {
      device->screen->fence_finish(device->screen, NULL, fence,
                                   OS_TIMEOUT_INFINITE);
      device->screen->fence_reference(device->screen, &fence, NULL);
   }

   /* Destroy in ownership order: context -> screen (which also destroys
    * the radeon_winsys backing store and closes the internal DRM fd). */
   device->pipe->destroy(device->pipe);
   device->screen->destroy(device->screen);

   vk_queue_finish(&device->queue.vk);
   vk_device_finish(&device->vk);
   vk_free2(&device->vk.alloc, pAllocator, device);
}
