/* SPDX-License-Identifier: MIT */

#include "radeon_drm_vk_device.h"

#include "util/hash_table.h"

#include <errno.h>
#include <stddef.h>
#include <xf86drm.h>
#include <radeon_drm.h>

int
radeon_drm_vk_device_init(struct radeon_drm_vk_device *device, int fd,
                          const struct radeon_drm_vk_ioctl_ops *ops)
{
   device->fd = fd;
   device->ops = ops != NULL ? ops : &radeon_drm_vk_ioctl_ops_drm;
   atomic_init(&device->cache_sync_count, 0);
   atomic_init(&device->cache_event_sequence, 0);
   atomic_init(&device->cache_sync_last_map, 0);
   atomic_init(&device->cache_sync_last_bo_handle, 0);
   atomic_init(&device->cache_sync_last_event, 0);
   atomic_init(&device->bo_close_last_handle, 0);
   atomic_init(&device->bo_close_last_event, 0);
   device->shared_bo_reference_counts =
      _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);
   if (device->shared_bo_reference_counts == NULL) {
      return -ENOMEM;
   }
   if (mtx_init(&device->shared_bo_mutex, mtx_plain) != thrd_success) {
      _mesa_hash_table_destroy(device->shared_bo_reference_counts, NULL);
      device->shared_bo_reference_counts = NULL;
      return -ENOMEM;
   }
   return 0;
}

void
radeon_drm_vk_device_finish(struct radeon_drm_vk_device *device)
{
   mtx_destroy(&device->shared_bo_mutex);
   _mesa_hash_table_destroy(device->shared_bo_reference_counts, NULL);
   device->shared_bo_reference_counts = NULL;
   device->fd = -1;
}

int
radeon_drm_vk_device_getparam(struct radeon_drm_vk_device *device,
                              uint32_t param, uint32_t *value)
{
   /* The kernel writes through the value pointer, so the wrapper keeps the
    * out-parameter alive across the ioctl.
    */
   int kernel_value = 0;
   struct drm_radeon_getparam arguments = {
      .param = (int)param,
      .value = &kernel_value,
   };
   int result = device->ops->command_write_read(device->fd,
                                                DRM_RADEON_GETPARAM,
                                                &arguments,
                                                sizeof(arguments));
   if (result == 0) {
      *value = (uint32_t)kernel_value;
   }
   return result;
}

int
radeon_drm_vk_device_info(struct radeon_drm_vk_device *device,
                          uint32_t request, uint32_t *value)
{
   uint32_t kernel_value = *value;
   struct drm_radeon_info arguments = {
      .request = request,
      .value = (uintptr_t)&kernel_value,
   };
   int result = device->ops->command_write_read(device->fd, DRM_RADEON_INFO,
                                                &arguments,
                                                sizeof(arguments));
   if (result == 0) {
      *value = kernel_value;
   }
   return result;
}
