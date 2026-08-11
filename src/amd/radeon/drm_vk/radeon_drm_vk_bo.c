/* SPDX-License-Identifier: MIT */

#include "radeon_drm_vk_bo.h"
#include "radeon_drm_vk_device.h"

#include "util/hash_table.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <xf86drm.h>
#include <radeon_drm.h>

static void
radeon_drm_vk_bo_record_close(struct radeon_drm_vk_device *device,
                              uint32_t handle)
{
   mtx_lock(&device->cache_event_mutex);
   device->bo_close_last.sequence = ++device->cache_event_sequence;
   device->bo_close_last.bo_handle = handle;
   mtx_unlock(&device->cache_event_mutex);
}

int
radeon_drm_vk_bo_create(struct radeon_drm_vk_device *device, uint64_t size,
                        uint64_t alignment, uint32_t domains, uint32_t flags,
                        bool shareable, struct radeon_drm_vk_bo *bo)
{
   struct drm_radeon_gem_create arguments = {
      .size = size,
      .alignment = alignment,
      .initial_domain = domains,
      .flags = flags,
   };
   int result = device->ops->command_write_read(device->fd,
                                                DRM_RADEON_GEM_CREATE,
                                                &arguments,
                                                sizeof(arguments));
   if (result != 0) {
      return result;
   }

   if (shareable) {
      /* Register the handle before the BO becomes visible so a concurrent
       * PRIME import of the same GEM object finds the count.
       */
      mtx_lock(&device->shared_bo_mutex);
      bool inserted =
         _mesa_hash_table_insert(device->shared_bo_reference_counts,
                                 (void *)(uintptr_t)arguments.handle,
                                 (void *)1) != NULL;
      mtx_unlock(&device->shared_bo_mutex);
      if (!inserted) {
         device->ops->gem_close(device->fd, arguments.handle);
         return -ENOMEM;
      }
   }

   bo->size = size;
   bo->handle = arguments.handle;
   bo->domains = domains;
   bo->shareable = shareable;
   return 0;
}

int
radeon_drm_vk_bo_map(struct radeon_drm_vk_device *device,
                     const struct radeon_drm_vk_bo *bo, void **map)
{
   struct drm_radeon_gem_mmap arguments = {
      .handle = bo->handle,
      .size = bo->size,
   };
   int result = device->ops->command_write_read(device->fd,
                                                DRM_RADEON_GEM_MMAP,
                                                &arguments,
                                                sizeof(arguments));
   if (result != 0) {
      return result;
   }

   void *mapping = device->ops->mmap((size_t)bo->size, device->fd,
                                     arguments.addr_ptr);
   if (mapping == NULL) {
      return -EACCES;
   }
   *map = mapping;
   return 0;
}

void
radeon_drm_vk_bo_unmap(struct radeon_drm_vk_device *device,
                       const struct radeon_drm_vk_bo *bo, void *map)
{
   device->ops->munmap(map, (size_t)bo->size);
}

void
radeon_drm_vk_bo_free(struct radeon_drm_vk_device *device,
                      struct radeon_drm_vk_bo *bo)
{
   bool close_bo;
   if (bo->shareable) {
      /* Drop the reference under the mutex so the GEM close cannot race a
       * concurrent import resolving the same handle.
       */
      mtx_lock(&device->shared_bo_mutex);
      struct hash_entry *entry =
         _mesa_hash_table_search(device->shared_bo_reference_counts,
                                 (void *)(uintptr_t)bo->handle);
      assert(entry != NULL && "shareable BOs are reference-counted");
      uintptr_t count = (uintptr_t)entry->data;
      assert(count > 0);
      if (count > 1) {
         entry->data = (void *)(count - 1);
         close_bo = false;
      } else {
         _mesa_hash_table_remove(device->shared_bo_reference_counts, entry);
         close_bo = true;
      }
      if (close_bo) {
         device->ops->gem_close(device->fd, bo->handle);
         radeon_drm_vk_bo_record_close(device, bo->handle);
      }
      mtx_unlock(&device->shared_bo_mutex);
   } else {
      device->ops->gem_close(device->fd, bo->handle);
      radeon_drm_vk_bo_record_close(device, bo->handle);
   }

   bo->handle = 0;
   bo->size = 0;
}

int
radeon_drm_vk_bo_export_fd(struct radeon_drm_vk_device *device,
                           const struct radeon_drm_vk_bo *bo, bool writable,
                           int *prime_fd)
{
   if (!bo->shareable) {
      return -EINVAL;
   }
   return device->ops->prime_handle_to_fd(device->fd, bo->handle,
                                          DRM_CLOEXEC |
                                             (writable ? DRM_RDWR : 0),
                                          prime_fd);
}

int
radeon_drm_vk_bo_import_fd(struct radeon_drm_vk_device *device, int prime_fd,
                           uint64_t size, uint32_t domains,
                           struct radeon_drm_vk_bo *bo)
{
   /* Resolve and count the handle inside one critical section, so a free of
    * the last other reference cannot close the handle between resolution and
    * count insertion.
    */
   mtx_lock(&device->shared_bo_mutex);

   uint32_t handle;
   int result = device->ops->prime_fd_to_handle(device->fd, prime_fd,
                                                &handle);
   if (result != 0) {
      mtx_unlock(&device->shared_bo_mutex);
      return result;
   }

   struct hash_entry *entry =
      _mesa_hash_table_search(device->shared_bo_reference_counts,
                              (void *)(uintptr_t)handle);
   if (entry != NULL) {
      assert((uintptr_t)entry->data > 0);
      entry->data = (void *)((uintptr_t)entry->data + 1);
   } else {
      if (_mesa_hash_table_insert(device->shared_bo_reference_counts,
                                  (void *)(uintptr_t)handle,
                                  (void *)1) == NULL) {
         device->ops->gem_close(device->fd, handle);
         mtx_unlock(&device->shared_bo_mutex);
         return -ENOMEM;
      }
   }

   mtx_unlock(&device->shared_bo_mutex);

   bo->size = size;
   bo->handle = handle;
   bo->domains = domains;
   bo->shareable = true;
   return 0;
}

void
radeon_drm_vk_bo_cache_sync_for_bo(struct radeon_drm_vk_device *device,
                                   const struct radeon_drm_vk_bo *bo,
                                   const void *map, uint64_t size)
{
   if (device == NULL || map == NULL || size == 0)
      return;

#if defined(__x86_64__) || defined(__i386__)
   /* SEQ_CST fences compile to MFENCE, ordering the flush against the
    * stores it publishes and the loads that follow it; CLFLUSH itself is
    * unordered against other lines.  64 bytes is the K8 cache-line size.
    */
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
   const uintptr_t line = 64;
   uintptr_t start = (uintptr_t)map & ~(line - 1);
   uintptr_t end = (uintptr_t)map + size;
   for (uintptr_t p = start; p < end; p += line)
      __builtin_ia32_clflush((const void *)p);
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
   mtx_lock(&device->cache_event_mutex);
   device->cache_sync_last.sequence = ++device->cache_event_sequence;
   device->cache_sync_last.map = (uintptr_t)map;
   device->cache_sync_last.bo_handle = bo != NULL ? bo->handle : 0;
   mtx_unlock(&device->cache_event_mutex);
   atomic_fetch_add_explicit(&device->cache_sync_count, 1,
                             memory_order_acq_rel);
}

void
radeon_drm_vk_bo_cache_sync(struct radeon_drm_vk_device *device,
                            const void *map, uint64_t size)
{
   radeon_drm_vk_bo_cache_sync_for_bo(device, NULL, map, size);
}
