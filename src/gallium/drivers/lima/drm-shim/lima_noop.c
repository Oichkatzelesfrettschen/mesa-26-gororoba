/*
 * Copyright (C) 2021 Icecream95
 * Copyright (C) 2019 Google LLC
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "drm-shim/drm_shim.h"
#include "drm-uapi/lima_drm.h"

#include "util/u_math.h"

static int
lima_ioctl_noop(int fd, unsigned long request, void *arg)
{
   return 0;
}

static int
lima_ioctl_get_param(int fd, unsigned long request, void *arg)
{
   struct drm_lima_get_param *gp = arg;

   switch (gp->param) {
   case DRM_LIMA_PARAM_GPU_ID:
      gp->value = DRM_LIMA_PARAM_GPU_ID_MALI450;
      return 0;
   case DRM_LIMA_PARAM_NUM_PP:
      gp->value = 6;
      return 0;
   default:
      fprintf(stderr, "Unknown DRM_IOCTL_LIMA_GET_PARAM %d\n", gp->param);
      return -EINVAL;
   }
}

static int
lima_ioctl_gem_create(int fd, unsigned long request, void *arg)
{
   struct drm_lima_gem_create *create = arg;

   if (!create->size)
      return -EINVAL;

   uint64_t aligned_size = align64(create->size, 4096);
   if (aligned_size > UINT32_MAX)
      return -ENOSPC;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;

   int ret = drm_shim_bo_init(bo, aligned_size);
   if (ret) {
      free(bo);
      return ret;
   }

   create->handle = drm_shim_bo_get_handle(shim_fd, bo);

   drm_shim_bo_put(bo);

   return 0;
}

static int
lima_ioctl_gem_info(int fd, unsigned long request, void *arg)
{
   struct drm_lima_gem_info *gem_info = arg;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, gem_info->handle);
   if (!bo)
      return -ENOENT;

   gem_info->va = bo->mem_addr;
   uint64_t mmap_offset;
   int ret = drm_shim_bo_get_mmap_offset(shim_fd, bo, &mmap_offset);
   if (!ret)
      gem_info->offset = mmap_offset;
   drm_shim_bo_put(bo);

   return ret;
}

static ioctl_fn_t driver_ioctls[] = {
   [DRM_LIMA_GET_PARAM] = lima_ioctl_get_param,
   [DRM_LIMA_GEM_CREATE] = lima_ioctl_gem_create,
   [DRM_LIMA_GEM_INFO] = lima_ioctl_gem_info,
   [DRM_LIMA_GEM_SUBMIT] = lima_ioctl_noop,
   [DRM_LIMA_GEM_WAIT] = lima_ioctl_noop,
   [DRM_LIMA_CTX_CREATE] = lima_ioctl_noop,
   [DRM_LIMA_CTX_FREE] = lima_ioctl_noop,
};

void
drm_shim_driver_init(void)
{
   drm_shim_set_mem_addr_range(UINT64_C(0x1000),
                               UINT64_C(0x0fff00000));

   shim_device.driver_ioctls = driver_ioctls;
   shim_device.driver_ioctl_count = ARRAY_SIZE(driver_ioctls);

   /* lima uses the DRM version to expose features, instead of getparam. */
   shim_device.version_major = 1;
   shim_device.version_minor = 1;
   shim_device.version_patchlevel = 0;

   drm_shim_platform_device_setup("lima", "/soc/lima", "arm,mali-450");
}
