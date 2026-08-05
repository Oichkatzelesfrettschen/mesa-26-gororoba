/* SPDX-License-Identifier: MIT */

#include "radeon_drm_vk_ioctl.h"

#include "util/os_mman.h"

#include <sys/mman.h>
#include <xf86drm.h>

static int
radeon_drm_vk_ioctl_command_write_read(int fd, unsigned long request,
                                       void *data, unsigned size)
{
   return drmCommandWriteRead(fd, request, data, size);
}

static int
radeon_drm_vk_ioctl_command_write(int fd, unsigned long request, void *data,
                                  unsigned size)
{
   return drmCommandWrite(fd, request, data, size);
}

static int
radeon_drm_vk_ioctl_gem_close(int fd, uint32_t handle)
{
   struct drm_gem_close arguments = {.handle = handle};
   return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &arguments);
}

static int
radeon_drm_vk_ioctl_prime_handle_to_fd(int fd, uint32_t handle, uint32_t flags,
                                       int *prime_fd)
{
   return drmPrimeHandleToFD(fd, handle, flags, prime_fd);
}

static int
radeon_drm_vk_ioctl_prime_fd_to_handle(int fd, int prime_fd, uint32_t *handle)
{
   return drmPrimeFDToHandle(fd, prime_fd, handle);
}

static void *
radeon_drm_vk_ioctl_mmap(size_t size, int fd, uint64_t offset)
{
   void *mapping = os_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                           (off_t)offset);
   return mapping == MAP_FAILED ? NULL : mapping;
}

static void
radeon_drm_vk_ioctl_munmap(void *address, size_t size)
{
   os_munmap(address, size);
}

const struct radeon_drm_vk_ioctl_ops radeon_drm_vk_ioctl_ops_drm = {
   .command_write_read = radeon_drm_vk_ioctl_command_write_read,
   .command_write = radeon_drm_vk_ioctl_command_write,
   .gem_close = radeon_drm_vk_ioctl_gem_close,
   .prime_handle_to_fd = radeon_drm_vk_ioctl_prime_handle_to_fd,
   .prime_fd_to_handle = radeon_drm_vk_ioctl_prime_fd_to_handle,
   .mmap = radeon_drm_vk_ioctl_mmap,
   .munmap = radeon_drm_vk_ioctl_munmap,
};
