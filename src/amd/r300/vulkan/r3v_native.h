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

struct r3v_native_image;
struct r3v_native_pipeline;
struct r3v_native_buffer;
struct r3v_native_arming_provider;

/* Deferred draw execution: vertex reads and the load-op clear happen at
 * queue submission, matching Vulkan's execution-time semantics, so the
 * recorded state names the sources by reference.  The application keeps
 * the bound buffer and target memory alive until execution completes,
 * the lifetime the Vulkan command-buffer contract already requires.
 */
/* One recorded transfer copy over the linear families.  Regions are
 * admitted at record time -- subresource, bounds, usage, and the
 * buffer byte footprint -- so execution resolves mappings and moves
 * rows without re-deciding validity.  buffer_row_length is resolved to
 * texels at record: the region's bufferRowLength, or the copy width
 * when the application passed zero.
 */
enum r3v_native_copy_kind {
   R3V_NATIVE_COPY_BUFFER_TO_BUFFER,
   R3V_NATIVE_COPY_BUFFER_TO_IMAGE,
   R3V_NATIVE_COPY_IMAGE_TO_BUFFER,
   R3V_NATIVE_COPY_IMAGE_TO_IMAGE,
   /* Whole-subresource color fill: dst_image alone, clear_dword the
    * packed little-endian B8G8R8A8 texel.
    */
   R3V_NATIVE_COPY_CLEAR_IMAGE,
};

struct r3v_native_deferred_copy {
   enum r3v_native_copy_kind kind;
   struct r3v_native_buffer *buffer;
   struct r3v_native_buffer *src_buffer;
   struct r3v_native_buffer *dst_buffer;
   struct r3v_native_image *src_image;
   struct r3v_native_image *dst_image;
   uint64_t buffer_offset;
   uint64_t src_offset;
   uint64_t dst_offset;
   uint64_t size;
   uint32_t buffer_row_length;
   uint32_t src_x, src_y;
   uint32_t dst_x, dst_y;
   uint32_t width, height;
   uint32_t clear_dword;
};

/* The first command-pool allocation keeps ordinary copy recordings compact;
 * r3v_native_copy_slot grows the storage when a recording reaches it.
 */
#define R3V_NATIVE_DEFERRED_COPY_INITIAL_CAPACITY 16

struct r3v_native_deferred_draw {
   bool pending;
   struct r3v_native_buffer *buffer;
   /* Bind offset plus the pipeline's attribute offset, into the buffer
    * range.
    */
   uint64_t stream_base;
   uint32_t stride;
   uint32_t first_vertex;
   int format_id;
   struct r3v_native_memory *target_memory;
   /* The pass target's declared footprint: the load-op clear's exact
    * byte bound at execution.
    */
   uint64_t target_fill_bytes;
   /* The pass target's extent: the viewport transform's window scale
    * at execution.
    */
   uint32_t target_width;
   uint32_t target_height;
};

/* Native command buffer: one fixed IB dword vector plus its BO references,
 * installed whole by a device-internal emitter or by the public triangle
 * recording surface.  The recording-state members carry the public
 * surface's begin/bind/draw progress; a command outside the qualified
 * contract poisons the buffer, so EndCommandBuffer returns the error and
 * the queue refuses it.  owned_carrier is the CPU_VERTEX node's gather
 * destination, allocated at draw recording and released with the buffer;
 * deferred_draw carries the vertex and clear work the queue executes at
 * submission.
 */
struct r3v_native_cmd_buffer {
   struct vk_command_buffer vk;
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r3v_native_bo_reference *references;
   uint32_t reference_count;

   struct r3v_native_image *pass_target;
   struct r3v_native_pipeline *bound_pipeline;
   struct r3v_native_buffer *bound_vertex_buffer;
   VkDeviceSize bound_vertex_offset;
   bool vertex_bound;
   bool draw_recorded;
   struct r3v_native_memory *owned_carrier;
   struct r3v_native_deferred_draw deferred_draw;
   /* Recorded transfer copies, executed in order at submission through
    * host mappings of the bound memory.  A command buffer carries
    * either the qualified render pass or transfer copies; the
    * recording refuses the mix, so execution order between the two
    * never arises.
    */
   /* Command-pool storage grows with the recorded operation count and is
    * released by r3v_native_cmd_buffer_release_recording.
    */
   struct r3v_native_deferred_copy *deferred_copies;
   uint32_t deferred_copy_count;
   uint32_t deferred_copy_capacity;
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
   /* The production path leaves this NULL and collects host facts.  The
    * drm-shim harness installs an explicit host-model provider so a missing
    * radeon module cannot become a matchable live identity. */
   const struct r3v_native_arming_provider *arming_provider;
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

/* The public recording surface's qualified render-target family:
 * B8G8R8A8_UNORM linear 2D color attachments at any extent inside the
 * 64x64 maximum, all sharing the maximum extent's row pitch.  The pitch
 * is a memory-layout property the cell's RB3D_COLORPITCH0 word freezes,
 * so the extent varies only the scissor-family dwords and the memory
 * footprint: pitch times height plus one oracle-headroom row past the
 * render extent.
 */
#define R3V_NATIVE_TARGET_WIDTH 64
#define R3V_NATIVE_TARGET_HEIGHT 64
#define R3V_NATIVE_TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM
#define R3V_NATIVE_TARGET_ROW_BYTES (R3V_NATIVE_TARGET_WIDTH * 4)
#define R3V_NATIVE_TARGET_MEMORY_BYTES \
   (R3V_NATIVE_TARGET_ROW_BYTES * (R3V_NATIVE_TARGET_HEIGHT + 1))

static inline uint64_t
r3v_native_image_footprint_bytes(uint32_t height)
{
   return (uint64_t)R3V_NATIVE_TARGET_ROW_BYTES * (height + 1);
}

/* The linear transfer family: B8G8R8A8_UNORM 2D images under transfer
 * usage alone, at any extent inside 2048 per axis -- a deliberately
 * conservative policy bound taken from the RS48x single-tile texture
 * ceiling, below every constraint that could bind a linear surface.
 * The row pitch aligns each row to 64 bytes: the 2D engine's
 * DST_PITCH_OFFSET word carries the pitch in 64-byte units, so every
 * transfer-family row layout stays addressable by the qualified
 * direct-write 2D path.  The recorded vkCmdCopy* subset executes the
 * family's copies through host mappings at submission, so the
 * footprint is the rows alone; the oracle-headroom row is the render
 * family's contract.
 */
#define R3V_NATIVE_TRANSFER_DIMENSION_MAX 2048

static inline uint32_t
r3v_native_transfer_row_pitch_bytes(uint32_t width)
{
   return (width * 4 + 63u) & ~63u;
}

static inline uint64_t
r3v_native_transfer_footprint_bytes(uint32_t width, uint32_t height)
{
   return (uint64_t)r3v_native_transfer_row_pitch_bytes(width) * height;
}

struct r3v_native_image {
   struct vk_object_base base;
   /* Bound memory and the allocation offset at which the image starts. */
   struct r3v_native_memory *memory;
   VkDeviceSize memory_offset;
   /* Creation extent, inside the family's published maximum. */
   uint32_t width;
   uint32_t height;
   /* Row layout in bytes: the fixed target pitch for the render
    * family, the width-derived 64-byte-aligned pitch for the transfer
    * family.
    */
   uint32_t row_pitch_bytes;
   /* Creation usage; the copy recording admits each direction by its
    * bit.
    */
   VkImageUsageFlags usage;
   /* Set for the transfer family: usage is transfer alone, so no view,
    * render pass, or attachment path admits the image.
    */
   bool transfer_family;
};

struct r3v_native_image_view {
   struct vk_object_base base;
   struct r3v_native_image *image;
};

/* The qualified graphics pipeline: creation admits exactly the fixed
 * cell's state vector plus the vertex-input freedom the CPU executor
 * covers, so the pipeline's own state is the one attribute's format,
 * stride, and offset.
 */
struct r3v_native_pipeline {
   struct vk_object_base base;
   int format_id;
   uint32_t binding_stride;
   uint32_t attribute_offset;
   /* The viewport/scissor extent creation admitted; the draw requires
    * it equal to the pass target's extent.
    */
   uint32_t target_width;
   uint32_t target_height;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_image, base, VkImage,
                               VK_OBJECT_TYPE_IMAGE)
VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_image_view, base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW)
VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_pipeline, base, VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)

extern const struct vk_command_buffer_ops r3v_native_cmd_buffer_ops;

/* The render-pass shape whose one subpass the qualified cell realizes;
 * pipeline creation and render-pass begin validate against the same
 * predicate so the two admissions cannot drift apart.
 */
struct vk_render_pass;
bool r3v_native_render_pass_matches_cell(const struct vk_render_pass *pass);

/* Releases the public recording state and the owned carrier BO; reset,
 * destroy, and a failed draw lowering all resolve here.
 */
void r3v_native_cmd_buffer_release_recording(
   struct r3v_native_cmd_buffer *cmd_buffer);

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

/* One application-shaped vertex source for carrier delivery: host
 * records in the little-endian component encoding the VAP fetches,
 * bounded by size_bytes, with format_id naming an
 * r300_vertex_format_id row.  The gather validates the request against
 * the bound and refuses what it cannot prove readable.
 */
struct r3v_native_vertex_stream_desc {
   const void *records;
   uint64_t size_bytes;
   uint32_t stride;
   uint32_t first_vertex;
   int format_id;
};

/* Carrier-delivery recorder: gathers the triangle's three vertices from
 * the caller's stream through the CPU vertex executor into the mapped
 * GTT carrier, then records the same fixed cell as
 * r3v_native_record_tcl_bypass_triangle -- the IB and its digest do not
 * depend on the delivery route, only the vertex BO contents do.  A
 * stream the gather refuses returns VK_ERROR_INITIALIZATION_FAILED
 * before any write.
 */
VkResult r3v_native_record_tcl_bypass_triangle_from_stream(
   VkCommandBuffer commandBuffer, VkDeviceMemory vertexMemory,
   VkDeviceMemory colorMemory,
   const struct r3v_native_vertex_stream_desc *stream);

/* The struct-level carrier-delivery recorder behind the handle wrapper
 * above: the carrier memory is the gather destination BO and the color
 * memory is the render target.
 */
VkResult r3v_native_record_tcl_bypass_triangle_gathered(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_memory *color_memory,
   const struct r3v_native_vertex_stream_desc *stream);

/* Record-only cell installer for the public draw lowering: emits the
 * fixed cell IB against the carrier and color references and installs
 * it, with no memory writes.  The vertex gather and the sentinel clear
 * ride cmd_buffer->deferred_draw and execute at queue submission.
 */
VkResult r3v_native_record_tcl_bypass_triangle_carrier(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_image *target_image);

/* Executes the command buffer's deferred draw at submission: gathers the
 * bound stream through the CPU vertex executor into the owned carrier
 * and sentinel-clears the target image's declared memory footprint, each
 * published for the unsnooped GART while its mapping is live.
 */
/* Executes the command buffer's recorded transfer copies in order at
 * submission, each through host mappings of the bound memory with the
 * destination published for the unsnooped GART.
 */
VkResult r3v_native_cmd_buffer_execute_deferred_copies(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer);

VkResult r3v_native_cmd_buffer_execute_deferred_draw(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer);

/* Direct-write control recorder: lowers the 2D solid-fill cell
 * (src/amd/r300/common/r300_direct_write.h) into the command buffer
 * from the one live color memory.  The cell reads no source BO, so the
 * color target is the whole relocation surface.  Recording is
 * submit-free; the queue's hazard gate guards execution.
 */
VkResult r3v_native_record_direct_write(VkCommandBuffer commandBuffer,
                                        VkDeviceMemory colorMemory);

#endif /* R3V_NATIVE_H */
