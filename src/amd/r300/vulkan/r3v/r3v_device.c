/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r3v_device.h"
#include "r3v_cmd_buffer.h"

#include "r3v_entrypoints.h"
#include "r3v_physical_device.h"
#include "r3v_private.h"

#include "vk_alloc.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_command_buffer.h"
#include "vk_common_entrypoints.h"
#include "vk_log.h"
#include "wsi_common.h"

#include "util/os_time.h"

#include <stdlib.h>
#include <string.h>

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "winsys/radeon_winsys.h"
#include "r300/r300_public.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r3v_GetDeviceProcAddr(VkDevice _device, const char *pName)
{
   if (_device == VK_NULL_HANDLE || pName == NULL)
      return NULL;

   VK_FROM_HANDLE(vk_device, device, _device);

   /* vkResetQueryPool is core 1.2; VK_EXT_host_query_reset exposes the same
    * command through the EXT alias.  r3v exposes Vulkan 1.0, so Mesa's
    * common device-proc gate rejects the promoted core spelling by API version
    * even when the extension is enabled.  Return the same host-side reset
    * implementation for the promoted spelling when the extension is enabled. */
   if (device->enabled_extensions.EXT_host_query_reset &&
       strcmp(pName, "vkResetQueryPool") == 0)
      return (PFN_vkVoidFunction)r3v_ResetQueryPool;

   return vk_device_get_proc_addr(device, pName);
}

VkResult
r3v_CreateDevice(VkPhysicalDevice physicalDevice,
                    const VkDeviceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkDevice *pDevice)
{
   VK_FROM_HANDLE(r3v_physical_device, pdevice, physicalDevice);
   struct r3v_device *device;
   VkResult result;

   device = vk_zalloc2(&pdevice->vk.instance->alloc, pAllocator,
                        sizeof(*device), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(pdevice, VK_ERROR_OUT_OF_HOST_MEMORY);

   list_inithead(&device->memory_list);
   simple_mtx_init(&device->memory_list_lock, mtx_plain);
   simple_mtx_init(&device->identity_map_cso_lock, mtx_plain);

   /* Four-table dispatch, highest precedence first.  Secondary command buffer
    * support is implemented by recording into r3v_cmd_buffer (vk_cmd_enqueue
    * path), then replaying recorded entries against the pipe_context at submit
    * time.  r3v_device_entrypoints carries the driver's own implementations;
    * wsi fills present-related entrypoints; vk_common_device_entrypoints is last
    * so the runtime's generic state-tracking implementations fill every device
    * entrypoint the driver does not override -- including the descriptor-set and
    * pipeline-layout machinery (CreateDescriptorSetLayout, CreatePipelineLayout,
    * CreateDescriptorPool, AllocateDescriptorSets, UpdateDescriptorSets).
    * Without this table those entrypoints are NULL and any descriptor-using
    * path (every compute pipeline, and any graphics shader with bound
    * resources) calls through a NULL dispatch pointer.  The command dispatch
    * table below already layers vk_common; the main table must too. */
   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_cmd_enqueue_unless_primary_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &r3v_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_common_device_entrypoints, false);

   result = vk_device_init(&device->vk, &pdevice->vk, &dispatch_table,
                            pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return result;
   }

   /* Command dispatch table for secondary command buffer replay. */
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch_table,
                                             &r3v_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch_table,
                                             &vk_common_device_entrypoints, false);
   device->vk.command_dispatch_table = &device->command_dispatch_table;
   device->vk.command_buffer_ops = &r3v_cmd_buffer_ops;

   /* Initialize the Gallium-mediated r300g backend.
    * radeon_drm_winsys_create() opens a new fd reference to the render node
    * and populates rws->screen with the r300 pipe_screen.  The ICD owns
    * both from this point; screen->destroy() releases both when called from
    * DestroyDevice.  r300g routes NIR shader states through nir_to_rc
    * internally; the ICD never calls nir_to_tgsi. */
   struct pipe_screen_config screen_config = {0};
   device->rws = radeon_drm_winsys_create(pdevice->render_node_fd,
                                           &screen_config,
                                           r300_screen_create);
   if (!device->rws) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "r3v: radeon_drm_winsys_create failed on fd %d",
                         pdevice->render_node_fd);
      goto fail_device;
   }
   device->screen = device->rws->screen;

   device->pipe =
      device->screen->context_create(device->screen, NULL,
                                     PIPE_CONTEXT_ROBUST_BUFFER_ACCESS);
   if (!device->pipe) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "r3v: pipe_screen->context_create failed");
      goto fail_screen;
   }

   /* One graphics-plus-transfer queue.  RS482/RS485 has no hardware vertex
    * processor; the vertex stage runs through Gallium Draw software TCL.
    * No compute queue is exposed. */
   if (pCreateInfo->queueCreateInfoCount == 0) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "r3v: queueCreateInfoCount must be at least 1");
      goto fail_pipe;
   }
   result = vk_queue_init(&device->queue.vk, &device->vk,
                           &pCreateInfo->pQueueCreateInfos[0], 0);
   if (result != VK_SUCCESS)
      goto fail_pipe;

   device->queue.vk.driver_submit = r3v_queue_driver_submit;

   /* Backend dispatch: check for explicit cs-direct-emit opt-in.
    * The env var must be exactly "1"; unset, empty, or any other value
    * leaves the default pipe_context-mediated Backend A path active.
    * Checked once at device creation and stored as a device flag so the
    * submit hot path avoids repeated getenv() calls. */
   const char *cs_gate = r3v_getenv_compat("R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED", "R300VK_CS_DIRECT_BACKEND_HAZARD_ACCEPTED");
   device->use_cs_backend = cs_gate && strcmp(cs_gate, "1") == 0;

   /* Inherit the hybrid-compute gate the physical device cached at creation
    * rather than re-reading the environment, so the device's runtime behavior
    * cannot diverge from the advertised queue flags if the environment changes
    * mid-process.  The physical-device value is the single source of truth. */
   device->hybrid_compute_enabled = pdevice->hybrid_compute_enabled;

   /* R3V_DEBUG is parsed once so the replay hot path reads device flags only.
    * no_overlay binds only
    * the pipeline's static CSOs (dynamic-state shadow ignored); no_topo
    * replays the recorded per-draw topology without the dynamic override;
   * log_draws emits one stderr line per replayed draw. */
   const char *dbg = r3v_getenv_compat("R3V_DEBUG", "R300VK_DEBUG");
   device->dbg_identity_map     = r3v_debug_option_enabled(dbg, "identity_map");
   device->dbg_classify_nir     = r3v_debug_option_enabled(dbg, "classify_nir");
   device->dbg_no_dyn_overlay   = r3v_debug_option_enabled(dbg, "no_overlay");
   device->dbg_no_topo_override = r3v_debug_option_enabled(dbg, "no_topo");
   device->dbg_log_draws        = r3v_debug_option_enabled(dbg, "log_draws");
   device->dbg_log_pixels       = r3v_debug_option_enabled(dbg, "log_pixels");

   *pDevice = r3v_device_to_handle(device);
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
r3v_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
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

   /* Queue teardown before context teardown: vk_queue_finish must complete
    * before the pipe_context it could reference is destroyed. */
   vk_queue_finish(&device->queue.vk);

   /* Identity-map cached CSOs were created via pipe->create_*_state at lazy
    * init time; the matching delete_*_state must run before the pipe_context
    * itself is destroyed. */
   if (device->identity_map_cso.blend)
      device->pipe->delete_blend_state(device->pipe,
                                       device->identity_map_cso.blend);
   if (device->identity_map_cso.rasterizer)
      device->pipe->delete_rasterizer_state(device->pipe,
                                            device->identity_map_cso.rasterizer);
   if (device->identity_map_cso.dsa)
      device->pipe->delete_depth_stencil_alpha_state(device->pipe,
                                                     device->identity_map_cso.dsa);
   if (device->identity_map_cso.sampler)
      device->pipe->delete_sampler_state(device->pipe,
                                         device->identity_map_cso.sampler);
   if (device->blend_acc_reduction_blend_cso)
      device->pipe->delete_blend_state(device->pipe,
                                       device->blend_acc_reduction_blend_cso);
   pipe_sampler_view_reference(&device->shift_variable_lut_view, NULL);
   pipe_resource_reference(&device->shift_variable_lut, NULL);
   pipe_sampler_view_reference(&device->shift_variable_fill_lut_view, NULL);
   pipe_resource_reference(&device->shift_variable_fill_lut, NULL);
   pipe_resource_reference(&device->ia_snapshot, NULL);
   pipe_resource_reference(&device->ia_snapshot_src, NULL);

   /* Destroy in ownership order: context -> screen (which also destroys
    * the radeon_winsys backing store and closes the internal DRM fd). */
   device->pipe->destroy(device->pipe);
   device->screen->destroy(device->screen);

   simple_mtx_destroy(&device->identity_map_cso_lock);
   simple_mtx_destroy(&device->memory_list_lock);

   vk_device_finish(&device->vk);
   vk_free2(&device->vk.alloc, pAllocator, device);
}
