/* SPDX-License-Identifier: MIT */

#include "radeon_drm_vk_cs.h"
#include "radeon_drm_vk_device.h"
#include "radeon_drm_vk_reloc.h"

#include "util/macros.h"

#include <assert.h>
#include <stddef.h>

void
radeon_drm_vk_cs_build(struct radeon_drm_vk_cs *cs, const uint32_t *ib,
                       uint32_t ib_size_dwords,
                       const struct radeon_drm_vk_reloc_list *relocs,
                       uint32_t ring, bool keep_tiling_flags)
{
   assert(ib_size_dwords != 0);

   cs->flags[0] = keep_tiling_flags ? RADEON_CS_KEEP_TILING_FLAGS : 0;
   cs->flags[1] = ring;
   cs->flags[2] = 0;

   cs->chunks[0] = (struct drm_radeon_cs_chunk){
      .chunk_id = RADEON_CHUNK_ID_RELOCS,
      .length_dw = (uint32_t)((sizeof(struct drm_radeon_cs_reloc) /
                               sizeof(uint32_t)) *
                              relocs->count),
      .chunk_data = (uint64_t)(uintptr_t)relocs->relocs,
   };
   cs->chunks[1] = (struct drm_radeon_cs_chunk){
      .chunk_id = RADEON_CHUNK_ID_IB,
      .length_dw = ib_size_dwords,
      .chunk_data = (uint64_t)(uintptr_t)ib,
   };
   cs->chunks[2] = (struct drm_radeon_cs_chunk){
      .chunk_id = RADEON_CHUNK_ID_FLAGS,
      .length_dw = ARRAY_SIZE(cs->flags),
      .chunk_data = (uint64_t)(uintptr_t)cs->flags,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(cs->chunks); i++) {
      cs->chunk_pointers[i] = (uint64_t)(uintptr_t)&cs->chunks[i];
   }

   cs->args = (struct drm_radeon_cs){
      .num_chunks = ARRAY_SIZE(cs->chunks),
      .chunks = (uint64_t)(uintptr_t)cs->chunk_pointers,
   };
}

int
radeon_drm_vk_cs_submit(struct radeon_drm_vk_device *device,
                        struct radeon_drm_vk_cs *cs)
{
   /* Host writes to GTT write-combining mappings can still sit in per-CPU WC
    * store buffers at the ioctl boundary; the syscall path is not a memory
    * fence on x86.  The fence publishes every vertex and IB byte before the
    * kernel schedules the GPU read.
    */
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
   atomic_store_explicit(
      &device->submit_boundary_sync_count,
      atomic_load_explicit(&device->cache_sync_count, memory_order_acquire),
      memory_order_release);

   return device->ops->command_write_read(device->fd, DRM_RADEON_CS,
                                          &cs->args, sizeof(cs->args));
}
