/* SPDX-License-Identifier: MIT */

#include "radeon_drm_vk_completion.h"
#include "radeon_drm_vk_device.h"
#include "radeon_drm_vk_reloc.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <radeon_drm.h>

int
radeon_drm_vk_completion_init(struct radeon_drm_vk_device *device,
                              struct radeon_drm_vk_completion *completion)
{
   return radeon_drm_vk_bo_create(device, sizeof(uint32_t),
                                  sizeof(uint32_t), RADEON_GEM_DOMAIN_GTT, 0,
                                  false, &completion->bo);
}

void
radeon_drm_vk_completion_finish(struct radeon_drm_vk_device *device,
                                struct radeon_drm_vk_completion *completion)
{
   radeon_drm_vk_bo_free(device, &completion->bo);
}

int
radeon_drm_vk_completion_reference(
   const struct radeon_drm_vk_completion *completion,
   struct radeon_drm_vk_reloc_list *relocs, uint32_t *index)
{
   assert(completion->bo.handle != 0);
   return radeon_drm_vk_reloc_list_add(relocs, completion->bo.handle, 0,
                                       completion->bo.domains, 0, index);
}

int
radeon_drm_vk_completion_await(
   struct radeon_drm_vk_device *device,
   const struct radeon_drm_vk_completion *completion)
{
   struct drm_radeon_gem_wait_idle arguments = {
      .handle = completion->bo.handle,
   };

   int result = -EBUSY;
   for (uint32_t attempt = 0; attempt < 3; attempt++) {
      result = device->ops->command_write(device->fd,
                                          DRM_RADEON_GEM_WAIT_IDLE,
                                          &arguments, sizeof(arguments));
      if (result != -EBUSY) {
         break;
      }
   }
   return result;
}
