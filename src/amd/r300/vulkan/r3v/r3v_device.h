/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_DEVICE_H
#define R3V_DEVICE_H

#include "r3v_private.h"

#include "vk_device.h"
#include "vk_queue.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/list.h"
#include "util/simple_mtx.h"
#include "winsys/radeon_winsys.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r3v_queue wraps vk_queue.  vk_queue must be the first member so
 * VK_DEFINE_HANDLE_CASTS can recover the r3v_queue from any VkQueue
 * handle.  One graphics-plus-transfer queue per logical device; the
 * RS482/RS485 vertex stage runs through Gallium Draw (software TCL) so
 * there is no separate compute queue. */
struct r3v_queue {
   struct vk_queue vk; /* must be first */
};

VK_DEFINE_HANDLE_CASTS(r3v_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

/* r3v_device wraps vk_device plus the Gallium-mediated backend state.
 * radeon_drm_winsys_create() initializes rws and sets rws->screen to the
 * r300 pipe_screen.  pipe is the per-device pipe_context; r300g routes
 * NIR shaders through nir_to_rc internally -- the ICD never calls nir_to_tgsi.
 *
 * use_cs_backend: true when R3V_CS_DIRECT_BACKEND_HAZARD_ACCEPTED=1 is
 * set in the environment at CreateDevice time.  When true, the submit path
 * would dispatch a cs-direct emitter (native PM4 via radeon_winsys)
 * rather than the pipe_context-mediated r3v_replay_gpu() path.  That
 * backend is not implemented: r300g's emit functions are coupled to the
 * private struct r300_context and its dirty-atom state machine, and the
 * curated RS482/RS485 safe-register set exposes no 3D-engine config
 * registers, so a standalone emitter is neither low-coupling nor separately
 * validatable.  The submit path honors the flag by reporting the gap once
 * and running the pipe_context replay.  The env var must compare equal to
 * the literal string "1"; unset, empty, or other values leave it false.
 *
 * hybrid_compute_enabled: exact opt-in for exposing the scalar hybrid
 * compute experiment.  The queue is still non-conformant until compute
 * pipeline, descriptor, dispatch, and memory semantics are implemented and
 * validated against CTS plus retained dmesg evidence. */
struct r3v_device {
   struct vk_device vk; /* must be first */
   struct vk_device_dispatch_table command_dispatch_table;
   struct radeon_winsys *rws;
   struct pipe_screen    *screen;
   struct pipe_context   *pipe;
   struct r3v_queue    queue;
   bool                   use_cs_backend;
   bool                   hybrid_compute_enabled;
   /* R3V_DEBUG comma-list, parsed once at device create. */
   bool                   dbg_identity_map;    /* "identity_map": replay diagnostics */
   bool                   dbg_classify_nir;    /* "classify_nir": lowered compute NIR */
   /* Draw-path isolation switches for live target-hardware triage. */
   bool                   dbg_no_dyn_overlay;   /* "no_overlay": static CSOs only */
   bool                   dbg_no_topo_override; /* "no_topo": recorded topology only */
   bool                   dbg_log_draws;        /* "log_draws": per-draw state line */
   bool                   dbg_log_pixels;       /* "log_pixels": attachment sample at end-pass */

   /* Every live VkDeviceMemory, linked through r3v_device_memory::
    * device_link.  The submit path walks it to give HOST_COHERENT semantics
    * to owns_buffer maps: device access happens only inside the synchronous
    * vkQueueSubmit, so syncing host -> bound resource at submit entry and
    * bound resource -> host after the GPU fence retires is exactly the
    * visibility coherent memory promises.  memory_list_lock guards the list
    * against Allocate/Free on other threads. */
   struct list_head       memory_list;
   simple_mtx_t           memory_list_lock;

   /* pipe_context state creation is not thread-safe, but pipeline creation may
    * run concurrently on one VkDevice.  identity_map_cso_lock serializes lazy
    * creation of the cached identity-map state and the per-pipeline shader
    * CSOs synthesized from the same pipe_context. */
   simple_mtx_t           identity_map_cso_lock;

   /* Cached gallium state CSOs every identity-map compute dispatch reuses
    * (blend = passthrough, rasterizer = no-cull / fill-solid, dsa = depth+
    * stencil off, sampler = NEAREST + CLAMP_TO_EDGE).  Lazily created on the
    * first identity-map pipeline-create, freed in r3v_DestroyDevice.  NULL
    * means uninitialized; r3v_device_init_identity_map_state populates
    * them on demand under identity_map_cso_lock. */
   struct {
      void *blend;
      void *rasterizer;
      void *dsa;
      void *sampler;
   } identity_map_cso;

   /* Blend-accumulation reduction state CSO -- the one CSO that differs
    * from the identity-map state set: blend enabled, ADD function,
    * blend_func (ONE, ONE) accumulates per-fragment value into bin cells.
    * Created on demand at the first blend-acc-reduction pipeline-create,
    * freed in r3v_DestroyDevice alongside the identity-map CSOs. */
   void                  *blend_acc_reduction_blend_cso;

   /* Variable-shift power-of-two lookup: a 32x1 RGBA8 texture whose texel j
    * holds the four little-endian bytes of 2^j (j in [0,31]).  The variable
    * shift gather FS reads it with the NEAREST identity-map sampler -- left at
    * index b, right at index 31-b -- to turn a per-element shift amount into the
    * 2^M multiplier the convolution then multiplies by.  Created once on the
    * first variable-shift pipeline-create under identity_map_cso_lock, freed in
    * r3v_DestroyDevice. */
   struct pipe_resource      *shift_variable_lut;
   struct pipe_sampler_view  *shift_variable_lut_view;

   /* Sign-extension fill lookup for the variable signed right shift (ishr):
    * a 32x1 RGBA8 texture whose texel b holds the four little-endian bytes of
    * 0xFFFFFFFF << (32-b) (the top b bits; b=0 -> 0).  ishr = ushr + sign(a) *
    * fill[b], the two operands bit-disjoint so the per-byte add is exact.
    * Created beside the 2^j lookup, freed in r3v_DestroyDevice. */
   struct pipe_resource      *shift_variable_fill_lut;
   struct pipe_sampler_view  *shift_variable_fill_lut_view;

   /* Self-dependent subpass-input snapshot: a copy of the attachment a
    * subpass both writes and reads, sampled in place of the live render
    * target.  ia_snapshot_src keys the copy to one attachment tile resource;
    * ia_snapshot_stale is raised at render pass begin, next-subpass, and each
    * in-pass pipeline barrier, and the next self-dependent draw re-copies --
    * so input reads observe exactly the writes made visible by the last
    * barrier.  Freed in r3v_DestroyDevice. */
   struct pipe_resource      *ia_snapshot;
   struct pipe_resource      *ia_snapshot_src;
   bool                       ia_snapshot_stale;
};

VK_DEFINE_HANDLE_CASTS(r3v_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

VkResult r3v_CreateDevice(VkPhysicalDevice physicalDevice,
                              const VkDeviceCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDevice *pDevice);

void r3v_DestroyDevice(VkDevice device,
                           const VkAllocationCallbacks *pAllocator);

/* Queue submit callback wired into vk_queue.driver_submit.  Replays
 * r3v_cmd_entry arrays against the pipe_context, flushes, fence-waits,
 * then executes CPU-side readback copies (COPY_IMAGE_TO_BUFFER). */
VkResult r3v_queue_driver_submit(struct vk_queue *queue,
                                    struct vk_queue_submit *submit);

#ifdef __cplusplus
}
#endif

#endif /* R3V_DEVICE_H */
