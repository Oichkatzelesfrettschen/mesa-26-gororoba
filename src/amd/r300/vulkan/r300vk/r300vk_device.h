/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_DEVICE_H
#define R300VK_DEVICE_H

#include "r300vk_private.h"

#include "vk_device.h"
#include "vk_queue.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "winsys/radeon_winsys.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r300vk_queue wraps vk_queue.  vk_queue must be the first member so
 * VK_DEFINE_HANDLE_CASTS can recover the r300vk_queue from any VkQueue
 * handle.  One graphics-plus-transfer queue per logical device; the
 * RS482/RS485 vertex stage runs through Gallium Draw (software TCL) so
 * there is no separate compute queue. */
struct r300vk_queue {
   struct vk_queue vk; /* must be first */
};

VK_DEFINE_HANDLE_CASTS(r300vk_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

/* r300vk_device wraps vk_device plus the Gallium-mediated backend state.
 * radeon_drm_winsys_create() initializes rws and sets rws->screen to the
 * r300 pipe_screen.  pipe is the per-device pipe_context; r300g routes
 * NIR shaders through r300_nir_to_rc_direct internally -- the ICD never
 * calls nir_to_tgsi.
 *
 * use_cs_backend: true when R300VK_CS_DIRECT_BACKEND_HAZARD_ACCEPTED=1 is
 * set in the environment at CreateDevice time.  When true, the submit path
 * would dispatch a cs-direct emitter (native PM4 via radeon_winsys)
 * rather than the pipe_context-mediated r300vk_replay_gpu() path.  That
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
struct r300vk_device {
   struct vk_device vk; /* must be first */
   struct vk_device_dispatch_table command_dispatch_table;
   struct radeon_winsys *rws;
   struct pipe_screen    *screen;
   struct pipe_context   *pipe;
   struct r300vk_queue    queue;
   bool                   use_cs_backend;
   bool                   hybrid_compute_enabled;
};

VK_DEFINE_HANDLE_CASTS(r300vk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

VkResult r300vk_CreateDevice(VkPhysicalDevice physicalDevice,
                              const VkDeviceCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDevice *pDevice);

void r300vk_DestroyDevice(VkDevice device,
                           const VkAllocationCallbacks *pAllocator);

/* Queue submit callback wired into vk_queue.driver_submit.  Replays
 * r300vk_cmd_entry arrays against the pipe_context, flushes, fence-waits,
 * then executes CPU-side readback copies (COPY_IMAGE_TO_BUFFER). */
VkResult r300vk_queue_driver_submit(struct vk_queue *queue,
                                    struct vk_queue_submit *submit);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_DEVICE_H */
