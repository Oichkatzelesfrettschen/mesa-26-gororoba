/*
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADEON_DRM_CS_H
#define RADEON_DRM_CS_H

#include "radeon_drm_bo.h"

struct radeon_ctx {
   struct radeon_drm_winsys *ws;
   uint32_t gpu_reset_counter;
};

struct radeon_bo_item {
   struct radeon_bo    *bo;
   union {
      struct {
         uint32_t    priority_usage;
      } real;
      struct {
         unsigned    real_idx;
      } slab;
   } u;
};

struct radeon_drm_cs;

struct radeon_cs_context {
   uint32_t                    buf[16 * 1024];

   /* Owning cmdbuf, set once at init.  The async submit job is handed this
    * context directly, so the worker reaches the cmdbuf through here rather
    * than reading the producer's live cs->cst, which the next flush has
    * usually already advanced to another triple-buffer slot. */
   struct radeon_drm_cs        *owner;

   int                         fd;
   struct drm_radeon_cs        cs;
   struct drm_radeon_cs_chunk  chunks[3];
   uint64_t                    chunk_array[3];
   uint32_t                    flags[2];

   /* Buffers. */
   unsigned                    max_relocs;
   unsigned                    num_relocs;
   unsigned                    num_validated_relocs;
   struct radeon_bo_item       *relocs_bo;
   struct drm_radeon_cs_reloc  *relocs;

   unsigned                    num_slab_buffers;
   unsigned                    max_slab_buffers;
   struct radeon_bo_item       *slab_buffers;

   int                         reloc_indices_hashlist[4096];

   /* Selective hash clear: track which slots were written */
   unsigned                    num_used_hash_slots;
   uint16_t                    used_hash_slots[512]; /* max unique BOs per CS */
};

struct radeon_drm_cs {
   enum amd_ip_type          ip_type;

   /* Triple-buffered CS: while one is being consumed by the kernel
    * in the submit thread and one is pending, the third is being
    * filled by the pipe driver. This eliminates the 81% CPU stall
    * from synchronous fence waits in the double-buffer design. */
   struct radeon_cs_context csc[3];
   /* Index of the CS currently being filled by the CPU. */
   uint8_t csc_fill_idx;
   /* The currently-used CS (being filled). */
   struct radeon_cs_context *csc_cur;
   /* The CS most recently submitted to the GPU (owned by submit thread). */
   struct radeon_cs_context *cst;

   /* The winsys. */
   struct radeon_drm_winsys *ws;

   /* Flush CS. */
   void (*flush_cs)(void *ctx, unsigned flags, struct pipe_fence_handle **fence);
   void *flush_data;

   /* Per-context fences: wait on [next_fill] not the global fence. */
   struct util_queue_fence flush_completed[3];
   struct pipe_fence_handle *next_fence;

   /* RADEON_FENCE_TRACE=1 flush ordinal: advanced only while the trace
    * path is enabled, once per traced cs_flush on this cmdbuf, so the
    * trace lines order deterministically across the triple-buffer
    * rotation.  Idle (trace-off) flushes leave the field unchanged. */
   uint32_t fence_trace_ordinal;
};

int radeon_lookup_buffer(struct radeon_winsys *rws, struct radeon_cs_context *csc,
                         struct radeon_bo *bo);

static inline struct radeon_drm_cs *
radeon_drm_cs(struct radeon_cmdbuf *rcs)
{
   return (struct radeon_drm_cs*)rcs->priv;
}

static inline bool
radeon_bo_is_referenced_by_cs(struct radeon_drm_cs *cs,
                              struct radeon_bo *bo)
{
   int num_refs = bo->num_cs_references;
   return num_refs == bo->rws->num_cs ||
         (num_refs && radeon_lookup_buffer(&cs->ws->base, cs->csc_cur, bo) != -1);
}

static inline bool
radeon_bo_is_referenced_by_cs_for_write(struct radeon_drm_cs *cs,
                                        struct radeon_bo *bo)
{
   int index;

   if (!bo->num_cs_references)
      return false;

   index = radeon_lookup_buffer(&cs->ws->base, cs->csc_cur, bo);
   if (index == -1)
      return false;

   if (!bo->handle)
      index = cs->csc_cur->slab_buffers[index].u.slab.real_idx;

   return cs->csc_cur->relocs[index].write_domain != 0;
}

static inline bool
radeon_bo_is_referenced_by_any_cs(struct radeon_bo *bo)
{
   return bo->num_cs_references != 0;
}

void radeon_drm_cs_sync_flush(struct radeon_cmdbuf *rcs);
void radeon_drm_cs_init_functions(struct radeon_drm_winsys *ws);
void radeon_drm_cs_emit_ioctl_oneshot(void *job, void *gdata, int thread_index);

#endif
