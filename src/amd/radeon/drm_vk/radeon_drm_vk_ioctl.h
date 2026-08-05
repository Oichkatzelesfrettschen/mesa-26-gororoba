/*
 * SPDX-License-Identifier: MIT
 *
 * Radeon DRM ioctl seam for the Vulkan transport layer.
 */

#ifndef RADEON_DRM_VK_IOCTL_H
#define RADEON_DRM_VK_IOCTL_H

#include <stddef.h>
#include <stdint.h>

/* Every transport unit reaches the kernel only through this vtable, so host
 * tests substitute a capture mock and exercise the full argument-construction
 * path with no file descriptor and no kernel.  The production table wraps
 * libdrm and OS mapping calls one-to-one.
 */
struct radeon_drm_vk_ioctl_ops {
   /* drmCommandWriteRead: returns 0 or a negative errno. */
   int (*command_write_read)(int fd, unsigned long request, void *data,
                             unsigned size);
   /* drmCommandWrite: returns 0 or a negative errno. */
   int (*command_write)(int fd, unsigned long request, void *data,
                        unsigned size);
   /* DRM_IOCTL_GEM_CLOSE: returns 0 or a negative errno. */
   int (*gem_close)(int fd, uint32_t handle);
   /* drmPrimeHandleToFD: returns 0 or a negative errno. */
   int (*prime_handle_to_fd)(int fd, uint32_t handle, uint32_t flags,
                             int *prime_fd);
   /* drmPrimeFDToHandle: returns 0 or a negative errno. */
   int (*prime_fd_to_handle)(int fd, int prime_fd, uint32_t *handle);
   /* Shared read/write mapping of the render node at the GEM mmap offset;
    * returns NULL on failure.
    */
   void *(*mmap)(size_t size, int fd, uint64_t offset);
   void (*munmap)(void *address, size_t size);
};

extern const struct radeon_drm_vk_ioctl_ops radeon_drm_vk_ioctl_ops_drm;

#endif /* RADEON_DRM_VK_IOCTL_H */
