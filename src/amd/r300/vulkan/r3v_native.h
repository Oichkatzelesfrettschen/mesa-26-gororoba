/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V device layer over the generic Radeon DRM transport.
 */

#ifndef R3V_NATIVE_H
#define R3V_NATIVE_H

#include "r3v_native_arming.h"

#include "amd/r300/compiler/r300_vertex_job.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_bo.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_completion.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_cs.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_device.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"

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

/* The common Vulkan queue runtime maps every driver-submit error to
 * VK_ERROR_DEVICE_LOST in vk_queue_flush()
 * (rg --fixed-strings "vk_queue_flush" src/vulkan/runtime/vk_queue.c).
 * The native queue keeps the transport boundary as a sideband status so an
 * attended control can distinguish refusal before the ioctl from a failed
 * completion wait after an accepted ioctl.
 */
enum r3v_native_queue_status {
   R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION,
   R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
   R3V_NATIVE_QUEUE_STATUS_SUBMITTED,
   R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE,
   R3V_NATIVE_QUEUE_STATUS_COMPLETED,
};

static inline enum r3v_native_queue_status
r3v_native_queue_status_from_transport(bool ioctl_accepted,
                                        bool completion_succeeded)
{
   if (!ioctl_accepted)
      return R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED;
   return completion_succeeded ? R3V_NATIVE_QUEUE_STATUS_COMPLETED
                               : R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE;
}

/* A submit with no executable IB finishes without a transport boundary.
 * r3v_native_queue_submit (rg --fixed-strings "r3v_native_queue_submit"
 * src/amd/r300/vulkan/r3v_native_queue.c) records executable-buffer presence
 * across the submit before finalization, so a zero-IB buffer cannot make the
 * result depend on command-buffer order.
 */
static inline enum r3v_native_queue_status
r3v_native_queue_status_finalize_submit(
   enum r3v_native_queue_status status, bool has_executable_ib)
{
   if (!has_executable_ib &&
       status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED)
      return R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION;
   return status;
}

static inline const char *
r3v_native_queue_status_name(enum r3v_native_queue_status status)
{
   switch (status) {
   case R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION:
      return "NO_SUBMISSION";
   case R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED:
      return "SUBMISSION_REFUSED";
   case R3V_NATIVE_QUEUE_STATUS_SUBMITTED:
      return "SUBMITTED";
   case R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE:
      return "COMPLETION_FAILURE";
   case R3V_NATIVE_QUEUE_STATUS_COMPLETED:
      return "COMPLETED";
   }
   return "UNKNOWN";
}

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
   /* Pipeline lifetime ends at the application's discretion, so the
    * deferred draw carries its own copy of the vertex job and the
    * GPU-route identity metadata.
    */
   struct r300_vertex_job vertex_job;
   bool vertex_job_identity;
   /* Set when the GPU producer route is admitted for this submission:
    * the queue composed the producer pass ahead of the consumer IB, the
    * carrier is poisoned instead of host-filled, and the words below
    * are the CPU oracle the post-completion read-back is judged
    * against -- the delivery-identity records plus the poison the
    * odd-count pad slot keeps.
    */
   bool gpu_producer_delivery;
   uint32_t gpu_producer_dwords;
   uint32_t gpu_expected_carrier[16];
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
   enum r3v_native_cell_kind cell_kind;
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
   /* Member count the burst status-load cell composed; zero for every
    * other kind.  The arming gate matches it against the declared
    * count, and the geometry predicate sizes the carrier by it.
    */
   uint32_t burst_draws;
   /* The burst cell's recorded fetch width: the FLOAT_4 model form
    * carries a 96-byte vertex stream, so the geometry fact binds each
    * width to its own vertex allocation size.
    */
   bool burst_model_float4;
};

struct r3v_native_queue {
   struct vk_queue vk;
};

/* The attended status-load runner installs this optional hook while it
 * records the exact transport interval inside vkQueueSubmit.  Ordinary
 * queue users leave emit NULL, so the driver carries no transcript policy
 * outside that runner.
 */
enum r3v_native_submission_trace_event {
   R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_ENTER,
   R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_RETURN,
   R3V_NATIVE_SUBMISSION_TRACE_COMPLETION_WAIT_BEGIN,
   R3V_NATIVE_SUBMISSION_TRACE_COMPLETION_WAIT_RETURN,
};

struct r3v_native_submission_trace {
   void *ctx;
   int (*emit)(void *ctx, enum r3v_native_submission_trace_event event);
};

/* The native device owns the transport device over the physical device's
 * render-node fd; no winsys, pipe_screen, or pipe_context exists in this
 * implementation.  submit_hazard_accepted mirrors the exact-value
 * R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1 gate read once at device creation;
 * manifest_dir carries R3V_NATIVE_MANIFEST_DIR when set.
 */
/* One submission prepared ahead of vkQueueSubmit: the relocation list,
 * the completion reference, the single CS build, the retained evidence
 * (semantic cell and submit object), and the arming disarm all complete
 * in the prepare call, so the matching vkQueueSubmit performs only cache
 * publication, the DRM_RADEON_CS ioctl, and the bounded completion wait.
 * The attended status-load runner prepares before the sampler's capture
 * window opens; the pre-ioctl evidence fsync ladder is disk-bound
 * (tens of milliseconds on a rotational system disk) and dominates the
 * window otherwise.  The struct lives inside the device because the
 * built CS's argument pointers reference its own chunk arrays and the
 * relocation storage, so its address stays fixed between prepare and
 * submit.
 */
struct r3v_native_prepared_submission {
   bool valid;
   struct r3v_native_cmd_buffer *cmd_buffer;
   struct radeon_drm_vk_reloc_list relocs;
   struct radeon_drm_vk_cs cs;
   struct radeon_drm_vk_completion completion;
   uint32_t completion_index;
   uint32_t *reference_indices;
};

struct r3v_native_device {
   struct vk_device vk;
   struct r3v_physical_device *pdevice;
   struct radeon_drm_vk_device drm;
   struct r3v_native_queue queue;
   struct r3v_native_submission_trace submission_trace;
   bool submit_hazard_accepted;
   const char *manifest_dir;
   enum r3v_native_queue_status queue_status;
   /* Serial status-load admissions this instance has counted against the
    * declared bound; the instance that wrote the attempt token is the
    * only one whose count is nonzero, which is what lets a continuation
    * run under its own token while every other process stays disarmed.
    */
   uint32_t serial_submissions_consumed;
   /* The production path leaves this NULL and collects host facts.  The
    * drm-shim harness installs an explicit host-model provider so a missing
    * radeon module cannot become a matchable live identity. */
   const struct r3v_native_arming_provider *arming_provider;
   struct r3v_native_prepared_submission prepared;
   /* R2VB delivery gates, read once at device creation: each holds the
    * literal "1" when its environment variable carried the exact opt-in
    * value, and NULL otherwise, so the route decision cannot drift
    * mid-process.  r3v_native_device_refresh_delivery_gates re-runs the
    * same read for harnesses that vary the route on one device.
    */
   const char *r2vb_delivery_gate;
   const char *r2vb_gpu_delivery_gate;
   /* A completed GPU-producer submission whose carrier read-back
    * diverged from the CPU oracle quarantines the capability: every
    * later admission on this device refuses, the evidence stays
    * retained, and the caller sees the failure instead of bytes the
    * two routes disagree on.
    */
   bool gpu_producer_quarantined;
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

/* Vulkan bindings and their GEM BOs use one page of alignment. */
#define R3V_NATIVE_MEMORY_ALIGNMENT 4096

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
   /* The immutable CPU vertex job the semantic front end lowered from
    * the vertex module, and the GPU-route admission metadata: the
    * TCL-bypass cell delivers the raw attribute stream, so only the
    * identity job is GPU-admissible.
    */
   struct r300_vertex_job vertex_job;
   bool gpu_vertex_job_identity;
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
 * only writers.  The kind travels with the installation, so every
 * installed stream carries the geometry contract the arming gate applies
 * to it.
 */
void r3v_native_cmd_buffer_install_ib(
   struct r3v_native_cmd_buffer *cmd_buffer, enum r3v_native_cell_kind kind,
   uint32_t *ib, uint32_t ib_size_dwords,
   struct r3v_native_bo_reference *references, uint32_t reference_count);

VkResult r3v_native_queue_submit(struct vk_queue *queue,
                                 struct vk_queue_submit *submit);

/* Prepares one recorded command buffer's transport ahead of
 * vkQueueSubmit: relocation list, completion reference, the single CS
 * build, semantic-cell and submit-object retention, and the arming
 * evaluation with its disarm token.  Authorization is consumed here, so
 * a prepared submission that never commits still counts against the
 * one-shot or serial bound.  Requires the open hazard gate and an
 * evidence directory, a nonempty IB, and no pending deferred draw or
 * copies (those execute submission-time semantics the prepare would
 * hoist); exactly one prepared submission exists per device, and the
 * next vkQueueSubmit either carries the same command buffer alone or
 * refuses and releases the prepared state.
 */
VkResult r3v_native_queue_prepare_submission(VkDevice device,
                                             VkCommandBuffer commandBuffer);

/* Releases a prepared submission's transport state: the completion BO,
 * the relocation list, and the reference index map.  Device teardown
 * calls it for a prepared submission that never committed.
 */
void r3v_native_prepared_release(struct r3v_native_device *device);

/* Reads the last native queue boundary status for an attended control.  The
 * Vulkan result remains the API error contract; this sideband status preserves
 * whether the command reached the ioctl and whether its completion retired.
 */
enum r3v_native_queue_status
r3v_native_queue_submission_status(VkDevice device);

/* A permanent binary wait is reset after deferred execution.  Emulated
 * timeline points, temporary payloads, and same-submit re-signals retain
 * their sync state for the common queue runtime to collect or replace. */
bool r3v_native_queue_wait_is_permanent_binary(
   const struct vk_queue_submit *submit, uint32_t wait_index);

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

/* Submit-time admission of the public GPU-producer route.  When the
 * deferred draw resolves to the R2VB GPU producer, the three predicates
 * hold or the submit refuses by name: structural (F32_4 identity vertex
 * job on the recorded triangle consumer), transport (the composed
 * producer PM4 is dword-identical to the silicon-qualified reference
 * outside the record payloads, proven by
 * r300_r2vb_producer_pass_semantic_equal), and numeric (every record is
 * an FP24 fixed point, enforced by the emitter's -EDOM refusal, with
 * the CPU-gathered records retained as the read-back oracle).  On
 * admission the command buffer's IB becomes producer ++ consumer, the
 * carrier reference gains the write domain, and the carrier is poisoned
 * so the post-completion read-back decides every slot.  A route other
 * than the GPU producer returns VK_SUCCESS untouched.
 */
VkResult r3v_native_deferred_draw_admit_gpu_producer(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer);

/* Post-completion verdict of an admitted GPU-producer delivery: reads
 * the carrier back against the retained oracle.  A divergence retains
 * the observed bytes as gpu_carrier_observed.bin beside the manifest,
 * quarantines the capability on the device, and reports device loss.
 */
VkResult r3v_native_deferred_draw_verify_gpu_producer(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer);

/* Fixed-cell emitters return negative errno.  The mapper keeps allocator
 * exhaustion distinct from structural emission failures at the Vulkan
 * recording boundary.
 */
VkResult r3v_native_cell_vk_result_from_errno(int emit_result);

/* Direct-write control recorder: lowers the 2D solid-fill cell
 * (src/amd/r300/common/r300_direct_write.h) into the command buffer
 * from the one live color memory.  The cell reads no source BO, so the
 * color target is the whole relocation surface.  Recording is
 * submit-free; the queue's hazard gate guards execution.
 */
VkResult r3v_native_record_direct_write(VkCommandBuffer commandBuffer,
                                        VkDeviceMemory colorMemory);

/* Producer-only recorder: poisons the whole carrier allocation, emits the
 * reference R2VB producer pass
 * (src/amd/r300/common/r300_r2vb_producer_pass.h), and installs it with
 * the carrier as the single relocation slot, read-write in the GTT --
 * the color backend writes it and the consuming vertex fetch reads it.
 * The cell renders no visible target and re-ingests nothing, so the
 * carrier bytes are the whole result and the CPU read-back is the
 * verdict.  Recording is submit-free; the queue's hazard gate guards
 * execution.
 */
VkResult r3v_native_record_r2vb_producer(VkCommandBuffer commandBuffer,
                                         VkDeviceMemory carrierMemory);

/* The FP24 boundary-sweep variant: the same recording contract with the
 * sweep records (r300_r2vb_producer_fp24_sweep_records) in place of the
 * reference triangle's.  The sweep count equals the reference count, so
 * the carrier geometry, cell kind, and arming predicate stay frozen and
 * the IB digest alone separates the streams.
 */
VkResult r3v_native_record_r2vb_producer_fp24_sweep(
   VkCommandBuffer commandBuffer, VkDeviceMemory carrierMemory);

/* The FP24 upper-ceiling bisection variant: the same recording contract
 * with the bisection records (r300_r2vb_producer_fp24_bisect_records),
 * whose lanes bracket the silicon's identity-delivery ceiling between
 * the sweep's largest exact delivery and its smallest deviating
 * magnitude.  Count parity keeps the frozen geometry; the IB digest
 * alone separates the streams.
 */
VkResult r3v_native_record_r2vb_producer_fp24_bisect(
   VkCommandBuffer commandBuffer, VkDeviceMemory carrierMemory);

/* Records the producer-plus-re-ingest cell: one IB carrying the
 * reference producer pass into the carrier followed by the reference
 * triangle draw fetching that carrier as its vertex stream
 * (r300_r2vb_reingest_reference_emit), so a completed run proves the
 * GPU-write to vertex-fetch ordering on one submission.  The carrier is
 * poisoned and the color target sentinel-filled at record, so the
 * producer stage and the consuming draw each keep a decidable verdict.
 */
VkResult r3v_native_record_r2vb_reingest(VkCommandBuffer commandBuffer,
                                         VkDeviceMemory carrierMemory,
                                         VkDeviceMemory colorMemory);

/* Records the fetched FLOAT_4 + FLOAT_2 tuple cell: poisons the carrier,
 * writes the reference vertex stream -- the slot-position array followed
 * by the FLOAT_2 model records -- into the vertex allocation, publishes
 * both for the unsnooped GART, and installs the reference tuple pass
 * (r300_r2vb_float2_tuple_reference_emit) against the two BOs.  The
 * vertex data travels through 3D_LOAD_VBPNTR under the PSC XY01
 * selector, so the carrier read-back judges the silicon's expansion of
 * each two-dword record to (x, y, 0, 1).
 */
VkResult r3v_native_record_r2vb_float2_tuple(VkCommandBuffer commandBuffer,
                                             VkDeviceMemory carrierMemory,
                                             VkDeviceMemory vertexMemory);

/* Records the serial status-load cell: the frozen tuple stream and BO
 * bindings of r3v_native_record_r2vb_float2_tuple under the serial cell
 * kind, whose arming admits up to the declared submission bound within
 * one device instance while the paired-status census samples.
 */
VkResult
r3v_native_record_r2vb_status_load_serial(VkCommandBuffer commandBuffer,
                                          VkDeviceMemory carrierMemory,
                                          VkDeviceMemory vertexMemory);

/* Records the burst status-load cell: one IB composing the tuple stream
 * as draws members (r300_r2vb_float2_tuple_burst_reference_emit), each
 * retargeted to its own carrier row, under the burst cell kind whose
 * arming demands the declared member count.  The carrier allocation is
 * draws member rows (r3v_native_burst_carrier_bytes) and is poisoned
 * whole, so each member's delivery decides independently.
 */
VkResult
r3v_native_record_r2vb_status_load_burst(VkCommandBuffer commandBuffer,
                                         VkDeviceMemory carrierMemory,
                                         VkDeviceMemory vertexMemory,
                                         uint32_t draws);

/* Records the fetch-width contrast burst: the same status-load burst
 * composition over the FLOAT_4 model emission
 * (r300_r2vb_float4_model_burst_reference_emit) -- model records stored
 * and fetched at full width, VAP_VTX_SIZE 8 -- against a 96-byte vertex
 * stream, with the tuple carrier expectation unchanged.  Under the same
 * observer this isolates fetch width as the one changed variable.
 */
VkResult r3v_native_record_r2vb_status_load_burst_float4_model(
   VkCommandBuffer commandBuffer, VkDeviceMemory carrierMemory,
   VkDeviceMemory vertexMemory, uint32_t draws);

/* The burst cell's carrier allocation: draws member rows of the
 * reference layout.  Returns 0 or a negative errno; draws outside the
 * burst bound refuses with -EINVAL.
 */
int r3v_native_burst_carrier_bytes(uint32_t draws, uint32_t *out);

/* The producer cell's carrier allocation: the reference layout's slot row
 * (pitch pixels of one FP32x4 texel, one row), the same product the
 * manifest publishes as carrier_size_bytes.  Returns 0 or a negative
 * errno.
 */
int r3v_native_producer_carrier_bytes(uint32_t *out);

/* Emits the producer cell and installs it against the carrier memory with
 * no memory writes, so the recorded digest depends on the BO handle
 * alone.  The poison prefill is the recorder's separate step.  Returns 0
 * or a negative errno.
 */
int r3v_native_producer_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory);

int r3v_native_producer_fp24_sweep_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory);

int r3v_native_producer_fp24_bisect_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory);

int r3v_native_reingest_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_memory *color_memory);

/* Re-reads the two R2VB delivery gates from the environment with the
 * device-creation rule (exact "1" or closed).  Route-calibration
 * harnesses call it after toggling the gates; the production path reads
 * the gates at device creation alone.
 */
void r3v_native_device_refresh_delivery_gates(
   struct r3v_native_device *device);

int r3v_native_float2_tuple_cell_install(
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_memory *vertex_memory);

#endif /* R3V_NATIVE_H */
