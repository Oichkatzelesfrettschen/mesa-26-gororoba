/*
 * SPDX-License-Identifier: MIT
 *
 * Capture-mock ioctl table for radeon_drm_vk host tests.
 */

#ifndef RADEON_DRM_VK_MOCK_H
#define RADEON_DRM_VK_MOCK_H

#include "radeon_drm_vk_ioctl.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <radeon_drm.h>

/* The mock records every kernel-boundary call and serves configurable
 * results, so tests assert the exact argument structs the transport would
 * hand to the kernel.  Handles count up from 1 on GEM create and PRIME
 * import; the per-request error injectors return the configured errno once.
 */
struct radeon_drm_vk_mock {
   uint32_t next_handle;
   uint32_t gem_create_calls;
   uint32_t gem_close_calls;
   uint32_t gem_close_last_handle;
   uint32_t wait_idle_calls;
   int wait_idle_results[8];
   uint32_t wait_idle_result_count;
   int create_result_once;
   int prime_import_result_once;
   uint32_t prime_import_handle;
   struct drm_radeon_gem_create last_gem_create;
   struct drm_radeon_gem_mmap last_gem_mmap;
   bool cs_called;
   struct drm_radeon_cs last_cs;
   struct drm_radeon_cs_chunk cs_chunks[8];
   uint32_t cs_chunk_count;
   uint32_t cs_flags[8];
   uint32_t cs_flags_count;
   uint32_t cs_ib_dwords[256];
   uint32_t cs_ib_dword_count;
   struct drm_radeon_cs_reloc cs_relocs[16];
   uint32_t cs_reloc_count;
};

static struct radeon_drm_vk_mock radeon_drm_vk_mock;

static int
radeon_drm_vk_mock_command_write_read(int fd, unsigned long request,
                                      void *data, unsigned size)
{
   (void)fd;
   struct radeon_drm_vk_mock *mock = &radeon_drm_vk_mock;

   switch (request) {
   case DRM_RADEON_GEM_CREATE: {
      assert(size == sizeof(struct drm_radeon_gem_create));
      if (mock->create_result_once != 0) {
         int result = mock->create_result_once;
         mock->create_result_once = 0;
         return result;
      }
      struct drm_radeon_gem_create *arguments = data;
      mock->last_gem_create = *arguments;
      arguments->handle = mock->next_handle++;
      mock->gem_create_calls++;
      return 0;
   }
   case DRM_RADEON_GEM_MMAP: {
      assert(size == sizeof(struct drm_radeon_gem_mmap));
      struct drm_radeon_gem_mmap *arguments = data;
      mock->last_gem_mmap = *arguments;
      arguments->addr_ptr = 0x1000;
      return 0;
   }
   case DRM_RADEON_CS: {
      assert(size == sizeof(struct drm_radeon_cs));
      const struct drm_radeon_cs *arguments = data;
      mock->cs_called = true;
      mock->last_cs = *arguments;
      const uint64_t *chunk_pointers =
         (const uint64_t *)(uintptr_t)arguments->chunks;
      mock->cs_chunk_count = arguments->num_chunks;
      assert(arguments->num_chunks <= 8);
      for (uint32_t i = 0; i < arguments->num_chunks; i++) {
         const struct drm_radeon_cs_chunk *chunk =
            (const struct drm_radeon_cs_chunk *)(uintptr_t)chunk_pointers[i];
         mock->cs_chunks[i] = *chunk;
         if (chunk->chunk_id == RADEON_CHUNK_ID_IB) {
            assert(chunk->length_dw <= 256);
            memcpy(mock->cs_ib_dwords,
                   (const void *)(uintptr_t)chunk->chunk_data,
                   chunk->length_dw * sizeof(uint32_t));
            mock->cs_ib_dword_count = chunk->length_dw;
         } else if (chunk->chunk_id == RADEON_CHUNK_ID_RELOCS) {
            uint32_t reloc_count =
               chunk->length_dw /
               (sizeof(struct drm_radeon_cs_reloc) / sizeof(uint32_t));
            assert(reloc_count <= 16);
            memcpy(mock->cs_relocs,
                   (const void *)(uintptr_t)chunk->chunk_data,
                   reloc_count * sizeof(struct drm_radeon_cs_reloc));
            mock->cs_reloc_count = reloc_count;
         } else if (chunk->chunk_id == RADEON_CHUNK_ID_FLAGS) {
            assert(chunk->length_dw <= 8);
            memcpy(mock->cs_flags,
                   (const void *)(uintptr_t)chunk->chunk_data,
                   chunk->length_dw * sizeof(uint32_t));
            mock->cs_flags_count = chunk->length_dw;
         }
      }
      return 0;
   }
   default:
      assert(!"unexpected write-read request");
      return -22;
   }
}

static int
radeon_drm_vk_mock_command_write(int fd, unsigned long request, void *data,
                                 unsigned size)
{
   (void)fd;
   (void)data;
   struct radeon_drm_vk_mock *mock = &radeon_drm_vk_mock;

   switch (request) {
   case DRM_RADEON_GEM_WAIT_IDLE: {
      assert(size == sizeof(struct drm_radeon_gem_wait_idle));
      int result = 0;
      if (mock->wait_idle_calls < mock->wait_idle_result_count) {
         result = mock->wait_idle_results[mock->wait_idle_calls];
      }
      mock->wait_idle_calls++;
      return result;
   }
   default:
      assert(!"unexpected write request");
      return -22;
   }
}

static int
radeon_drm_vk_mock_gem_close(int fd, uint32_t handle)
{
   (void)fd;
   radeon_drm_vk_mock.gem_close_calls++;
   radeon_drm_vk_mock.gem_close_last_handle = handle;
   return 0;
}

static int
radeon_drm_vk_mock_prime_handle_to_fd(int fd, uint32_t handle, uint32_t flags,
                                      int *prime_fd)
{
   (void)fd;
   (void)flags;
   *prime_fd = (int)(1000 + handle);
   return 0;
}

static int
radeon_drm_vk_mock_prime_fd_to_handle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   (void)prime_fd;
   struct radeon_drm_vk_mock *mock = &radeon_drm_vk_mock;
   if (mock->prime_import_result_once != 0) {
      int result = mock->prime_import_result_once;
      mock->prime_import_result_once = 0;
      return result;
   }
   *handle = mock->prime_import_handle != 0 ? mock->prime_import_handle
                                            : mock->next_handle++;
   return 0;
}

static uint8_t radeon_drm_vk_mock_mapping[4096];

static void *
radeon_drm_vk_mock_mmap(size_t size, int fd, uint64_t offset)
{
   (void)fd;
   (void)offset;
   return size <= sizeof(radeon_drm_vk_mock_mapping)
             ? radeon_drm_vk_mock_mapping
             : NULL;
}

static void
radeon_drm_vk_mock_munmap(void *address, size_t size)
{
   (void)address;
   (void)size;
}

static const struct radeon_drm_vk_ioctl_ops radeon_drm_vk_mock_ops = {
   .command_write_read = radeon_drm_vk_mock_command_write_read,
   .command_write = radeon_drm_vk_mock_command_write,
   .gem_close = radeon_drm_vk_mock_gem_close,
   .prime_handle_to_fd = radeon_drm_vk_mock_prime_handle_to_fd,
   .prime_fd_to_handle = radeon_drm_vk_mock_prime_fd_to_handle,
   .mmap = radeon_drm_vk_mock_mmap,
   .munmap = radeon_drm_vk_mock_munmap,
};

static inline void
radeon_drm_vk_mock_reset(void)
{
   memset(&radeon_drm_vk_mock, 0, sizeof(radeon_drm_vk_mock));
   radeon_drm_vk_mock.next_handle = 1;
}

#endif /* RADEON_DRM_VK_MOCK_H */
