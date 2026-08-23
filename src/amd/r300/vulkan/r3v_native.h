/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V device layer over the generic Radeon DRM transport.
 */

#ifndef R3V_NATIVE_H
#define R3V_NATIVE_H

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_compute_job.h"
#include "amd/r300/common/r300_compute_verb.h"
#include "amd/r300/common/r300_vertex_job.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_bo.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_completion.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_cs.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_device.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"


#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_descriptor_set_layout.h"
#include "vk_device.h"
#include "vk_device_memory.h"
#include "vk_queue.h"

#include <stdbool.h>
#include <stdint.h>

/* The vertex-input limits the physical device publishes and the
 * pipeline admission enforces: binding count (R300 VAP_PROG_STREAM_CNTL
 * carries 16 attribute streams; the attribute count is
 * R300_VERTEX_JOB_MAX_INPUTS), attribute offset, and binding stride.
 */
#define R3V_NATIVE_MAX_VERTEX_BINDINGS 16u
#define R3V_NATIVE_MAX_VERTEX_ATTRIBUTE_OFFSET 2047u
#define R3V_NATIVE_MAX_VERTEX_BINDING_STRIDE 2048u
/* The instances one draw expands on the host into the cell's consumer
 * list: the cell family's triangle ceiling (R300_TRIANGLE_MAX_TRIANGLES,
 * the 16-bit vertex count and index bound). */
#define R3V_NATIVE_MAX_DRAW_INSTANCES 21845u

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
   /* Dword-pattern fill of a bound range: dst_buffer, dst_offset, size
    * (resolved at record; VK_WHOLE_SIZE truncates to whole dwords),
    * clear_dword the pattern.
    */
   R3V_NATIVE_COPY_FILL_BUFFER,
   /* Inline data write: update_data owns a record-time copy of the
    * application bytes, released with the recording.
    */
   R3V_NATIVE_COPY_UPDATE_BUFFER,
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
   uint8_t *update_data;
};

/* The first command-pool allocation keeps ordinary copy recordings compact;
 * r3v_native_copy_slot grows the storage when a recording reaches it.
 */
#define R3V_NATIVE_DEFERRED_COPY_INITIAL_CAPACITY 16

/* The compute descriptor surface: a set layout admits storage-buffer
 * bindings under the compute stage inside the binding bound, a pool
 * allocates sets against its declared capacity, and a set holds one
 * buffer range per binding.  vkUpdateDescriptorSets returns void, so a
 * write outside the admitted contract poisons the set and the dispatch
 * recording refuses it, keeping the surface fail-closed without a
 * result channel.
 */
#define R3V_NATIVE_DESCRIPTOR_BINDING_MAX 4
#define R3V_NATIVE_DESCRIPTOR_POOL_SET_MAX 16

struct r3v_native_descriptor_set_layout {
   struct vk_descriptor_set_layout vk;
   bool binding_present[R3V_NATIVE_DESCRIPTOR_BINDING_MAX];
};

struct r3v_native_descriptor_binding {
   bool bound;
   struct r3v_native_buffer *buffer;
   VkDeviceSize offset;
   /* Resolved byte range: VK_WHOLE_SIZE resolves against the buffer at
    * the write, so the dispatch admission compares plain numbers.
    */
   VkDeviceSize range;
};

struct r3v_native_descriptor_set {
   struct vk_object_base base;
   struct r3v_native_descriptor_set_layout *layout;
   bool poisoned;
   struct r3v_native_descriptor_binding
      bindings[R3V_NATIVE_DESCRIPTOR_BINDING_MAX];
};

struct r3v_native_descriptor_pool {
   struct vk_object_base base;
   uint32_t max_sets;
   uint32_t set_count;
   struct r3v_native_descriptor_set *sets[R3V_NATIVE_DESCRIPTOR_POOL_SET_MAX];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_descriptor_set_layout, vk.base,
                               VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)
VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_descriptor_set, base,
                               VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)
VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_descriptor_pool, base,
                               VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

/* One recorded dispatch, resolved at record time to the two memory
 * ranges the CPU compute route moves at submission.  The recording
 * proves the ranges cover the invocation footprint, so execution
 * re-checks only what can change between record and submit: the
 * memory binding.
 */
struct r3v_native_deferred_dispatch {
   bool pending;
   struct r300_compute_job job;
   uint32_t group_counts[3];
   struct r3v_native_buffer *input_buffer;
   struct r3v_native_buffer *output_buffer;
   /* Byte offsets into each buffer's bound memory, the buffer bind
    * offset and the descriptor offset already summed.
    */
   uint64_t input_memory_offset;
   uint64_t output_memory_offset;
   uint64_t byte_count;
   /* Set when the identity verb's GPU route admitted this dispatch: the
    * queue installed the compute identity carrier pass, the device
    * writes the output, and gpu_expected (gpu_record_count * 4 dwords,
    * command-pool allocated) is the CPU oracle the post-completion
    * read-back is judged against. */
   bool gpu_carrier_delivery;
   uint32_t gpu_record_count;
   uint32_t *gpu_expected;
};

/* One attribute slot's stream at draw time: the bound buffer of the
 * attribute's binding, the bind offset plus the attribute offset into
 * the buffer range, the binding stride, and the attribute format.
 */
struct r3v_native_deferred_stream {
   struct r3v_native_buffer *buffer;
   uint64_t stream_base;
   uint32_t stride;
   int format_id;
   /* An instance-rate binding: the record read is the Vulkan vertex
    * input address calculation over first_instance and the relative
    * instance (r300_vertex_stream); the core divisor is one. */
   bool instance_rate;
   uint32_t instance_divisor;
};

/* One recorded vkCmdClearAttachments rectangle, bounded per pass. */
#define R3V_NATIVE_PASS_CLEAR_RECT_MAX 8

struct r3v_native_pass_clear_rect {
   uint32_t x, y, width, height;
   uint32_t dword;
};

struct r3v_native_deferred_draw {
   bool pending;
   /* The attribute slots the vertex job reads, one bit per slot; zero
    * is a pass carrying its load-op clear alone.  streams[slot] is
    * filled for each set bit.
    */
   uint32_t stream_mask;
   struct r3v_native_deferred_stream streams[R300_VERTEX_JOB_MAX_INPUTS];
   uint32_t first_vertex;
   /* Vertices per instance: a multiple of three, at least three; the
    * host expands vertex_count * instance_count records into the cell
    * family's vertex list.
    */
   uint32_t vertex_count;
   /* An indexed draw dereferences its indices on the host at execution:
    * index_bytes (2 or 4) names the bound index type, index_base is
    * the bind offset into index_buffer, and each of the three indices
    * from first_index on sums with vertex_offset in 32-bit wrapping
    * arithmetic to the vertex number the streams gather.  The record
    * proved the index range readable; the vertex numbers are judged at
    * execution under the robust rule like any other record.
    */
   bool indexed;
   struct r3v_native_buffer *index_buffer;
   uint64_t index_base;
   uint32_t index_bytes;
   uint32_t first_index;
   int32_t vertex_offset;
   /* The draw's instances: the host executes the three vertices once
    * per instance from first_instance, instance-major, into the
    * carrier the cell family's 3 * instance_count vertex-list draw
    * consumes; InstanceIndex observes first_instance + instance. */
   uint32_t first_instance;
   uint32_t instance_count;
   /* The pipeline's facing state, applied by the host after the
    * viewport transform: a culled triangle's records collapse to its
    * first vertex.
    */
   VkCullModeFlags cull_mode;
   VkFrontFace front_face;
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
   /* In-pass attachment clears over a draw-less pass: rectangles
    * inside the render area with their packed little-endian B8G8R8A8
    * texels, applied after the load-op clear on the zero-IB path.
    */
   uint32_t clear_rect_count;
   struct r3v_native_pass_clear_rect
      clear_rects[R3V_NATIVE_PASS_CLEAR_RECT_MAX];
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
 * destination, allocated at draw recording and released with the buffer,
 * and owned_slot is the fetched producer's slot-position array, allocated
 * at the first fetched admission and released with the buffer;
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
   /* Dynamic viewport/scissor state: undefined at begin, set by the
    * vkCmdSet commands, persisting across binds and passes within the
    * recording; the draw of a dynamic-state pipeline resolves its
    * extent here.
    */
   bool viewport_set;
   bool scissor_set;
   VkViewport dynamic_viewport;
   VkRect2D dynamic_scissor;
   /* Per-binding vertex buffers from CmdBindVertexBuffers, one bit of
    * vertex_bound_mask per bound binding.
    */
   struct r3v_native_buffer
      *bound_vertex_buffers[R3V_NATIVE_MAX_VERTEX_BINDINGS];
   VkDeviceSize bound_vertex_offsets[R3V_NATIVE_MAX_VERTEX_BINDINGS];
   uint32_t vertex_bound_mask;
   /* The index buffer from CmdBindIndexBuffer: the buffer, its bind
    * offset, and the index size in bytes the bound VkIndexType names
    * (2 for UINT16, 4 for UINT32); zero bytes is no binding.
    */
   struct r3v_native_buffer *bound_index_buffer;
   VkDeviceSize bound_index_offset;
   uint32_t bound_index_bytes;
   bool draw_recorded;
   struct r3v_native_memory *owned_carrier;
   /* The fetched producer's slot-position BO, allocated at the first
    * fetched admission and released with the buffer; it holds the
    * (v + 0.5, 0.5, 0, 1) record per vertex the fetched body's first
    * array reads.
    */
   struct r3v_native_memory *owned_slot;
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
   /* The compute recording state: a dispatch-only command buffer binds
    * a compute pipeline and one set-0 descriptor set and records one
    * dispatch; the recording refuses a mix with the render pass or the
    * transfer copies, so the buffer carries exactly one execution
    * shape.
    */
   struct r3v_native_pipeline *bound_compute_pipeline;
   struct r3v_native_descriptor_set *bound_compute_set;
   struct r3v_native_deferred_dispatch deferred_dispatch;
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
   /* The last submission's transport interval on the raw monotonic
    * clock: the reading immediately before DRM_RADEON_CS and the one
    * immediately after the completion wait retires.  A caller timing a
    * delivery route needs this bracket rather than the vkQueueSubmit
    * wall, because the wall also spans the semantic-cell and
    * submit-object evidence writes, the IB digest, the completion BO
    * allocation, and the arming facts read from sysfs and uname.  Both
    * read zero until a submission reaches the ioctl.
    */
   uint64_t transport_enter_ns;
   uint64_t transport_return_ns;
   /* Whether the last submission's deferred draw delivered its carrier
    * through the device-side producer.  The declared route names what a
    * caller asked for; this names what the resolver and the admission
    * actually did, so an evidence bundle states the route that ran.
    */
   bool transport_gpu_producer_delivery;
   /* The cell kind of the last submission that reached the ioctl; with
    * the flag above it names which producer -- immediate or fetched --
    * wrote the carrier, so a timing row over one route is not credited
    * to the other.
    */
   enum r3v_native_cell_kind transport_cell_kind;
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
   const char *r2vb_fetched_gate;
   /* The compute verb gate table, one entry per ledger row read from
    * that row's gpu_gate the same way (the literal "1" or NULL): with
    * the compute gate, a verb's open gate selects its GPU route when the
    * row's route executes, and an open gate on a row without an
    * executing route selects nothing. */
   const char *compute_verb_gates[R300_COMPUTE_VERB_COUNT];
   /* Failure injection at the fetched route's composition boundary: a
    * nonzero negative errno makes the admission treat the composed route
    * as refused with that errno, after the emitters ran and before any
    * allocation, reference, IB, or carrier write.  Test harnesses set it;
    * the production path leaves it zero.
    */
   int gpu_producer_compose_inject_errno;
   /* A completed GPU-producer submission whose carrier read-back
    * diverged from the CPU oracle quarantines the capability: every
    * later admission on this device refuses, the evidence stays
    * retained, and the caller sees the failure instead of bytes the
    * two routes disagree on.
    */
   bool gpu_producer_quarantined;
   /* Set when a completed compute identity carrier delivery diverged
    * from the CPU oracle; every later GPU compute admission refuses. */
   bool gpu_compute_quarantined;
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

/* A sampler is pure recorded state: no native route samples, so the
 * descriptor surface refuses every write that names one, and the object
 * carries its creation parameters alone.
 */
struct r3v_native_sampler {
   struct vk_object_base base;
   VkSamplerCreateInfo info;
};

/* One vertex attribute the job reads: its binding, the attribute offset
 * into that binding's record, and the F32 format of its data.
 */
struct r3v_native_vertex_attribute {
   uint32_t binding;
   uint32_t offset;
   int format_id;
};

/* The qualified graphics pipeline: creation admits exactly the fixed
 * cell's state vector plus the vertex-input freedom the CPU executor
 * covers, so the pipeline's own state is the per-vertex F32 attributes
 * the job reads, each over its binding's stride.
 */
struct r3v_native_pipeline {
   struct vk_object_base base;
   /* Set for a compute pipeline: compute_job is the admitted lowering
    * and the graphics fields below stay zero.  The bind and dispatch
    * recordings check the kind, so one handle type serves both bind
    * points without a graphics field ever reading as compute state.
    */
   bool is_compute;
   struct r300_compute_job compute_job;
   /* The compute pipeline layout's set-0 binding map, copied at
    * creation: two set layouts are compatible exactly when they are
    * identically defined, and the bounded shape (storage-buffer
    * bindings of one descriptor under the compute stage) makes the
    * presence map the whole definition, so the dispatch admission
    * compares maps rather than handles.
    */
   bool set0_binding_present[R3V_NATIVE_DESCRIPTOR_BINDING_MAX];
   /* The attribute slots the vertex job reads (r300_vertex_job_input_mask
    * of vertex_job), each described in attributes[slot], and the stride
    * of every binding an attribute names.
    */
   uint32_t attribute_mask;
   struct r3v_native_vertex_attribute
      attributes[R300_VERTEX_JOB_MAX_INPUTS];
   uint32_t binding_strides[R3V_NATIVE_MAX_VERTEX_BINDINGS];
   /* The bindings declared VK_VERTEX_INPUT_RATE_INSTANCE, one bit per
    * binding; their attributes read by instance at the core divisor of
    * one. */
   uint32_t instance_rate_bindings;
   /* The immutable CPU vertex job the semantic front end lowered from
    * the vertex module, and the GPU-route admission metadata: the
    * TCL-bypass cell delivers the raw attribute stream, so only the
    * identity job is GPU-admissible.
    */
   struct r300_vertex_job vertex_job;
   bool gpu_vertex_job_identity;
   /* Set when the vertex job stores the location-0 varying and the
    * fragment module is the pass-through: the draw records the varying
    * cell over eight-dword records.
    */
   bool varying;
   /* The viewport/scissor extent creation admitted; the draw requires
    * it equal to the pass target's extent.  With the dynamic flag the
    * extent resolves from the recorded vkCmdSetViewport/SetScissor
    * state instead and the static fields stay zero.
    */
   bool dynamic_viewport_scissor;
   uint32_t target_width;
   uint32_t target_height;
   /* Rasterization facing state: the host computes each triangle's
    * signed area in window coordinates and collapses a culled triangle
    * to degenerate records the raster draws nothing for, so the IB is
    * unchanged by culling.
    */
   VkCullModeFlags cull_mode;
   VkFrontFace front_face;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_image, base, VkImage,
                               VK_OBJECT_TYPE_IMAGE)
VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_image_view, base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW)
VK_DEFINE_NONDISP_HANDLE_CASTS(r3v_native_sampler, base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)
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

/* The last submission's transport interval in raw monotonic nanoseconds:
 * DRM_RADEON_CS plus the bounded completion wait, with the evidence
 * writes, digest, allocation, and arming reads that surround them left
 * outside.  Returns 0 when no submission reached the ioctl.
 */
uint64_t r3v_native_queue_transport_wall_ns(VkDevice device);

/* Whether the last submission delivered its carrier through the
 * device-side producer route, as the resolver and the submit-time
 * admission decided it rather than as a caller declared it.
 */
bool r3v_native_queue_observed_gpu_producer(VkDevice device);

/* The delivery route the last submission's deferred draw took, as the
 * resolver and the submit-time admission decided it: the CPU vertex
 * job, the immediate GPU producer (records embedded as DRAW_IMMD_2
 * dwords), or the fetched GPU producer (records fetched from the bound
 * vertex BO).  A route timing row rests on this rather than on the
 * gates the caller set.
 */
enum r3v_native_observed_route {
   R3V_NATIVE_OBSERVED_ROUTE_CPU = 0,
   R3V_NATIVE_OBSERVED_ROUTE_GPU_PRODUCER = 1,
   R3V_NATIVE_OBSERVED_ROUTE_GPU_PRODUCER_FETCHED = 2,
   R3V_NATIVE_OBSERVED_ROUTE_COMPUTE_IDENTITY_CARRIER = 3,
};

enum r3v_native_observed_route
r3v_native_queue_observed_route(VkDevice device);

const char *r3v_native_observed_route_name(enum r3v_native_observed_route route);

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
   /* Vertices per instance: a multiple of three, at least three; the
    * host expands vertex_count * instance_count records into the cell
    * family's vertex list.
    */
   uint32_t vertex_count;
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
 * cell family member -- the varying cell when the bound pipeline's job
 * stores the varying, the position-only cell otherwise, over
 * triangle_count triangles (the draw's instances) -- against the
 * carrier and color references and installs it, with no memory writes.
 * The carrier holds 3 * triangle_count records.  The vertex gather and
 * the sentinel clear ride cmd_buffer->deferred_draw and execute at
 * queue submission.
 */
VkResult r3v_native_record_tcl_bypass_triangle_carrier(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer,
   struct r3v_native_memory *carrier_memory,
   struct r3v_native_image *target_image, bool varying,
   uint32_t triangle_count);

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

/* Executes the command buffer's recorded dispatch at submission
 * through host mappings of the two bound memories, the CPU compute
 * route: the record-time admission proved the ranges, so execution
 * resolves mappings, re-checks the memory bindings, runs the job, and
 * publishes the output for the unsnooped GART.
 */
VkResult r3v_native_cmd_buffer_execute_deferred_dispatch(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer);

/* The identity verb's GPU route: under the compute gate and the verb's
 * gate, an admissible dispatch (whole F32_4 records inside the carrier
 * ceiling, input and output in distinct buffer objects at the aligned
 * offsets, every input word inside the FP24 window) installs the
 * compute identity carrier pass over three references and records the
 * CPU oracle; a closed gate leaves the CPU route; a refusal names its
 * cause before any allocation, reference, IB, or write.  After the
 * completion wait the verifier reads the output back against the
 * oracle, retains both beside the submit objects, and a divergence
 * quarantines the capability with device loss.
 */
VkResult r3v_native_deferred_dispatch_admit_gpu(
   struct r3v_native_device *device,
   struct r3v_native_cmd_buffer *cmd_buffer);
VkResult r3v_native_deferred_dispatch_verify_gpu(
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

/* Depth control recorder: lowers the two-triangle depth-test cell
 * (src/amd/r300/common/r300_zb_depth_control_cell.h) into the command
 * buffer from three live memories.  The recorder writes the six-vertex
 * payload, sentinel-fills the color target with the color sentinel and
 * the depth surface with the Z16 depth sentinel, publishes each for the
 * unsnooped GART, and installs the reference IB with the vertex read,
 * color write, and depth read-write GTT references in slot order.  Each
 * memory's allocation is exactly the cell's declared footprint, so the
 * frozen geometry the arming gate checks is the shape the recorder
 * admitted.  Recording is submit-free; the queue's hazard gate guards
 * execution.
 */
/* The depth control's vertex allocation: one page holding the six
 * FLOAT_4 positions at its head, the size the recorder admits and the
 * queue's frozen-geometry fact compares.
 */
#define R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION 4096u

VkResult r3v_native_record_zb_depth_control(VkCommandBuffer commandBuffer,
                                            VkDeviceMemory vertexMemory,
                                            VkDeviceMemory colorMemory,
                                            VkDeviceMemory depthMemory);

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
 * (r300_r2vb_reingest_pass_emit), so a completed run proves the
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
