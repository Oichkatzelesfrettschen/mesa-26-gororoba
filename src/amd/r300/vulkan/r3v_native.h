/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V device layer over the generic Radeon DRM transport.
 */

#ifndef R3V_NATIVE_H
#define R3V_NATIVE_H

#include "amd/radeon/drm_vk/radeon_drm_vk_bo.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_completion.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_device.h"

#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_device.h"
#include "vk_device_memory.h"
#include "vk_queue.h"

#include <stdbool.h>
#include <stdint.h>

/* The result every native refusal returns.  Each command's registry entry
 * fixes the results it may return, and VK_ERROR_UNKNOWN is the one error the
 * whole native refusal set shares: the other universal member,
 * VK_ERROR_VALIDATION_FAILED, belongs to validation layers.  The public-surface
 * policy recomputes that intersection from vk.xml and fails when a single
 * result stops covering the set.
 */
#define R3V_NATIVE_REFUSAL_RESULT VK_ERROR_UNKNOWN

/* One recorded BO reference of a native command buffer.  The IB names the
 * reference by its index in this array; the queue folds the array into the
 * submission relocation list.
 */
struct r3v_native_memory;

struct r3v_native_bo_reference {
   uint32_t handle;
   uint32_t read_domains;
   uint32_t write_domain;
   /* Owning device memory when the reference names one; the queue keeps
    * the unsnooped-GART coherency contract over its live CPU mapping
    * (publish before submission, invalidate after completion).
    */
   struct r3v_native_memory *memory;
};

/* Native command buffer: one fixed IB dword vector plus its BO references,
 * installed whole by a device-internal emitter.  Vulkan command recording
 * into this buffer declines; the recording surface arrives with the
 * execution-node vocabulary.
 */
struct r3v_native_cmd_buffer {
   struct vk_command_buffer vk;
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r3v_native_bo_reference *references;
   uint32_t reference_count;
};

struct r3v_native_queue {
   struct vk_queue vk;
};

/* The native device owns the transport device over the physical device's
 * render-node fd; no winsys, pipe_screen, or pipe_context exists in this
 * implementation.  submit_hazard_accepted mirrors the exact-value
 * R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1 gate read once at device creation;
 * manifest_dir carries R3V_NATIVE_MANIFEST_DIR when set.
 */
struct r3v_native_device {
   struct vk_device vk;
   struct r3v_physical_device *pdevice;
   struct radeon_drm_vk_device drm;
   struct r3v_native_queue queue;
   bool submit_hazard_accepted;
   const char *manifest_dir;
};

VK_DEFINE_HANDLE_CASTS(r3v_native_device, vk.base, VkDevice,
                       VK_OBJECT_TYPE_DEVICE)
VK_DEFINE_HANDLE_CASTS(r3v_native_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

/* Device memory owns exactly one GEM BO; a mapping lives for the map/unmap
 * window.
 */
struct r3v_native_memory {
   struct vk_device_memory vk;
   struct radeon_drm_vk_bo bo;
   void *map;
};

struct r3v_native_buffer {
   struct vk_buffer vk;
   struct r3v_native_memory *memory;
   VkDeviceSize offset;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_memory, vk.base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)
VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_buffer, vk.base, VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)

extern const struct vk_command_buffer_ops r3v_native_cmd_buffer_ops;

/* Installs a complete IB and reference list into a native command buffer,
 * taking ownership of both allocations.  The fixed-cell emitters are the
 * only writers.
 */
void r3v_native_cmd_buffer_install_ib(
   struct r3v_native_cmd_buffer *cmd_buffer, uint32_t *ib,
   uint32_t ib_size_dwords, struct r3v_native_bo_reference *references,
   uint32_t reference_count);

VkResult r3v_native_queue_submit(struct vk_queue *queue,
                                 struct vk_queue_submit *submit);

/* Durable evidence writer shared by the queue's retained submit objects
 * and the attended runners' readback artifacts: temporary file, full
 * write, fsync, atomic rename, directory fsync.  Returns 0 or a negative
 * errno.
 */
int r3v_native_evidence_write_file(const char *dir, const char *name,
                                   const void *data, size_t size);

/* Fixed-cell recorder, linked directly by the pre-hardware harness and the
 * attended-cell runner: lowers the TCL-bypass triangle into the command
 * buffer from the two live buffer-object memories.  Recording is
 * submit-free; the queue's hazard gate guards execution.
 */
VkResult r3v_native_record_tcl_bypass_triangle(VkCommandBuffer commandBuffer,
                                               VkDeviceMemory vertexMemory,
                                               VkDeviceMemory colorMemory);

/* Direct-write control recorder: lowers the 2D solid-fill cell
 * (src/amd/r300/common/r300_direct_write.h) into the command buffer
 * from the one live color memory.  The cell reads no source BO, so the
 * color target is the whole relocation surface.  Recording is
 * submit-free; the queue's hazard gate guards execution.
 */
VkResult r3v_native_record_direct_write(VkCommandBuffer commandBuffer,
                                        VkDeviceMemory colorMemory);

#endif /* R3V_NATIVE_H */
