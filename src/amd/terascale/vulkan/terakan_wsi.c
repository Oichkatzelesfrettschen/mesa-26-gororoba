/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_wsi.h"

#include "terakan_bo.h"
#include "terakan_device.h"
#include "terakan_sync_completion.h"

#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "vk_device.h"
#include "vk_instance.h"
#include "vk_sync.h"
#include "wsi_common.h"
#include "wsi_common_private.h"

#include <assert.h>
#include <string.h>

struct terakan_wsi_hw_wait {
   struct terakan_device * device;
   struct terakan_bo * bo;
   uint32_t * mapping;
   int32_t refcount;
};

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
terakan_wsi_proc_addr(VkPhysicalDevice const physicalDevice, char const * const pName)
{
   struct terakan_physical_device const * const physical_device =
      terakan_physical_device_from_handle(physicalDevice);
   return vk_instance_get_proc_addr_unchecked(physical_device->vk.instance, pName);
}

uint32_t
terakan_wsi_hw_wait_load_value(struct terakan_wsi_hw_wait const * const hw_wait)
{
   return __atomic_load_n(hw_wait->mapping, __ATOMIC_ACQUIRE);
}

void
terakan_wsi_hw_wait_ref(struct terakan_wsi_hw_wait * const hw_wait)
{
   p_atomic_inc(&hw_wait->refcount);
}

void
terakan_wsi_hw_wait_unref(struct terakan_wsi_hw_wait * const hw_wait)
{
   if (!p_atomic_dec_zero(&hw_wait->refcount)) {
      return;
   }

   terakan_bo_free(hw_wait->bo, NULL);
   vk_free(&hw_wait->device->vk.alloc, hw_wait);
}

static void
terakan_wsi_hw_wait_store_value(struct terakan_wsi_hw_wait * const hw_wait, uint32_t const value)
{
   __atomic_store_n(hw_wait->mapping, value, __ATOMIC_RELEASE);
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static VkResult
terakan_wsi_init_image_hw_wait(VkDevice const device_h, struct wsi_image * const image)
{
   VK_FROM_HANDLE(terakan_device, device, device_h);

   struct terakan_wsi_hw_wait * const hw_wait = vk_alloc(
      &device->vk.alloc, sizeof(struct terakan_wsi_hw_wait), alignof(struct terakan_wsi_hw_wait),
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (hw_wait == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   VkResult result = device->winsys_fn->bo->allocate_device_memory(
      device, 16, 16, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, NULL,
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &hw_wait->bo);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, hw_wait);
      return result;
   }

   hw_wait->mapping = terakan_bo_map(hw_wait->bo);
   if (hw_wait->mapping == NULL) {
      terakan_bo_free(hw_wait->bo, NULL);
      vk_free(&device->vk.alloc, hw_wait);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   hw_wait->device = device;
   hw_wait->refcount = 1;
   memset(hw_wait->mapping, 0, 16);
   __atomic_thread_fence(__ATOMIC_SEQ_CST);

   image->hw_wait_state = hw_wait;
   image->hw_wait_current_value = 0;
   image->hw_wait_queued_value = 0;
   image->hw_wait_queued_is_wsi = false;
   return VK_SUCCESS;
}

static void
terakan_wsi_destroy_image_hw_wait(VkDevice const device_h, struct wsi_image * const image)
{
   VK_FROM_HANDLE(terakan_device, device, device_h);
   struct terakan_wsi_hw_wait * const hw_wait = image->hw_wait_state;
   if (hw_wait == NULL) {
      return;
   }

   assert(hw_wait->device == device);
   terakan_wsi_hw_wait_store_value(hw_wait, UINT32_MAX);
   image->hw_wait_state = NULL;
   image->hw_wait_current_value = UINT32_MAX;
   image->hw_wait_queued_value = UINT32_MAX;
   image->hw_wait_queued_is_wsi = false;
   terakan_wsi_hw_wait_unref(hw_wait);
}

static void
terakan_wsi_signal_image_hw_wait(VkDevice const device_h, struct wsi_image * const image)
{
   VK_FROM_HANDLE(terakan_device, device, device_h);
   struct terakan_wsi_hw_wait * const hw_wait = image->hw_wait_state;
   if (hw_wait == NULL) {
      return;
   }

   assert(hw_wait->device == device);
   terakan_wsi_hw_wait_store_value(hw_wait, image->hw_wait_current_value);
}

static VkResult
terakan_wsi_create_image_hw_wait_sync(VkDevice const device_h,
                                      struct wsi_image const * const image,
                                      struct vk_sync ** const sync_out)
{
   VK_FROM_HANDLE(terakan_device, device, device_h);
   struct terakan_wsi_hw_wait * const hw_wait = image->hw_wait_state;
   if (hw_wait == NULL) {
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   uint32_t const current_value = terakan_wsi_hw_wait_load_value(hw_wait);
   uint32_t const target_value = image->hw_wait_queued_value;

   struct vk_sync * sync_base;
   VkResult result = vk_sync_create(&device->vk, &terakan_sync_completion_type, 0, 0, &sync_base);
   if (result != VK_SUCCESS) {
      return result;
   }

   struct terakan_sync_completion * const sync =
      container_of(sync_base, struct terakan_sync_completion, vk);
   sync->pending_value = current_value;
   sync->current_value = current_value;
   if (image->hw_wait_queued_is_wsi && target_value != 0 && current_value < target_value) {
      terakan_wsi_hw_wait_ref(hw_wait);
      sync->presentation_wait_state = hw_wait;
      sync->presentation_wait_bo = hw_wait->bo;
      sync->presentation_wait_value = target_value;
      sync->presentation_wait_is_wsi = true;
   }

   *sync_out = sync_base;
   return VK_SUCCESS;
}

void
terakan_wsi_finish(struct terakan_physical_device * const physical_device)
{
   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device, &physical_device->vk.instance->alloc);
}

VkResult
terakan_wsi_init(struct terakan_physical_device * const physical_device)
{
   struct wsi_device_options const device_options = {.sw_device = false};
   VkResult result = wsi_device_init(
      &physical_device->wsi_device, terakan_physical_device_to_handle(physical_device),
      terakan_wsi_proc_addr, &physical_device->vk.instance->alloc, -1, NULL, &device_options);
   if (result != VK_SUCCESS) {
      return result;
   }

   physical_device->vk.wsi_device = &physical_device->wsi_device;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
   /* As of April 2024, the DXGI path uses Direct3D 12, which is not available on TeraScale.
    * Until a more direct solution is implemented, present via copying to the host.
    */
   physical_device->wsi_device.sw = true;
#endif

   if (debug_get_bool_option("TERAKAN_X11_EXPERIMENTAL_HW_WAIT", false)) {
      physical_device->wsi_device.init_image_hw_wait = terakan_wsi_init_image_hw_wait;
      physical_device->wsi_device.destroy_image_hw_wait = terakan_wsi_destroy_image_hw_wait;
      physical_device->wsi_device.signal_image_hw_wait = terakan_wsi_signal_image_hw_wait;
      physical_device->wsi_device.create_image_hw_wait_sync = terakan_wsi_create_image_hw_wait_sync;
   }

   return VK_SUCCESS;
}
