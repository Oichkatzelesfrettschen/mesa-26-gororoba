/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V device: transport ownership, queue, and fail-closed command
 * surface.
 */

#include "r3v_native.h"

#include "amd/r300/common/r300_compute_verb.h"

#include "r3v_entrypoints.h"
#include "r3v_physical_device.h"
#include "r3v_private.h"

#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_log.h"

#include <stdlib.h>
#include <string.h>

/* The exact-value native submission gate: "1" opens; unset, empty, and every
 * other value stay closed.  Read once at device creation so the decision
 * cannot drift mid-process.
 */
static bool
r3v_native_submit_hazard_accepted(void)
{
   const char *value = getenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   return value != NULL && strcmp(value, "1") == 0;
}

/* An empty evidence path has no retention destination.  Treat it like an
 * unset value so the queue cannot format artifact names against the root
 * directory while the submit gate is open.
 */
static const char *
r3v_native_manifest_dir(void)
{
   const char *value = getenv("R3V_NATIVE_MANIFEST_DIR");
   return value != NULL && value[0] != '\0' ? value : NULL;
}

/* An R2VB gate opens on the exact value "1" alone; the cached literal
 * keeps the decision independent of later environment mutation.
 */
static const char *
exact_gate(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && strcmp(value, "1") == 0 ? "1" : NULL;
}

void
r3v_native_device_refresh_delivery_gates(struct r3v_native_device *device)
{
   device->r2vb_delivery_gate =
      exact_gate("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL");
   device->r2vb_gpu_delivery_gate =
      exact_gate("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
   device->r2vb_fetched_gate =
      exact_gate("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL");
   uint32_t verb_count = 0;
   const struct r300_compute_verb_row *rows =
      r300_compute_verb_rows(&verb_count);
   for (uint32_t v = 0; v < verb_count; v++)
      device->compute_verb_gates[v] = exact_gate(rows[v].gpu_gate);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r3v_GetDeviceProcAddr(VkDevice _device, const char *pName)
{
   if (_device == VK_NULL_HANDLE || pName == NULL)
      return NULL;
   VK_FROM_HANDLE(vk_device, device, _device);
   return vk_device_get_proc_addr(device, pName);
}

VkResult
r3v_CreateDevice(VkPhysicalDevice physicalDevice,
                 const VkDeviceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(r3v_physical_device, pdevice, physicalDevice);
   VkResult result;

   struct r3v_native_device *device =
      vk_zalloc2(&pdevice->vk.instance->alloc, pAllocator, sizeof(*device),
                 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(pdevice, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Driver entrypoints first; the runtime's common table fills the generic
    * state-tracking surface.  Entrypoints in neither table stay NULL, and
    * GetDeviceProcAddr reports them absent rather than pretending support.
    */
   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &r3v_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_common_device_entrypoints, false);

   result = vk_device_init(&device->vk, &pdevice->vk, &dispatch_table,
                           pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return result;
   }

   device->pdevice = pdevice;
   device->vk.command_buffer_ops = &r3v_native_cmd_buffer_ops;

   if (radeon_drm_vk_device_init(&device->drm, pdevice->render_node_fd,
                                 NULL) != 0) {
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return vk_error(pdevice, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   result = vk_queue_init(&device->queue.vk, &device->vk,
                          &(VkDeviceQueueCreateInfo){
                             .sType =
                                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                             .queueFamilyIndex = 0,
                             .queueCount = 1,
                          },
                          0);
   if (result != VK_SUCCESS) {
      radeon_drm_vk_device_finish(&device->drm);
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return result;
   }
   device->queue.vk.driver_submit = r3v_native_queue_submit;

   device->submit_hazard_accepted = r3v_native_submit_hazard_accepted();
   device->manifest_dir = r3v_native_manifest_dir();
   r3v_native_device_refresh_delivery_gates(device);

   *pDevice = r3v_native_device_to_handle(device);
   return VK_SUCCESS;
}

void
r3v_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   if (!device)
      return;

   vk_queue_finish(&device->queue.vk);
   r3v_native_prepared_release(device);
   radeon_drm_vk_device_finish(&device->drm);
   vk_device_finish(&device->vk);
   vk_free2(&device->vk.alloc, pAllocator, device);
}
