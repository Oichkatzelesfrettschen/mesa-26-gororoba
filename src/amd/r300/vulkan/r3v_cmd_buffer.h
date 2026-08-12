/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_CMD_BUFFER_H
#define R3V_CMD_BUFFER_H

#include "r3v_private.h"

#include "vk_command_buffer.h"

#include "pipe/p_state.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward references for r3v_cmd_entry payload types. */
struct r3v_pipeline;
struct r3v_image;
struct r3v_buffer;
struct r3v_event;
struct r3v_descriptor_set;
struct r3v_cmd_bind_descriptor_sets;

#define R3V_MAX_BOUND_DESCRIPTOR_SETS  8u
#define R3V_MAX_DYNAMIC_OFFSETS        16u

enum r3v_cmd_type {
   R3V_CMD_BEGIN_RENDER_PASS,
   R3V_CMD_NEXT_SUBPASS,
   R3V_CMD_BIND_PIPELINE,
   R3V_CMD_SET_VIEWPORT,
   R3V_CMD_SET_SCISSOR,
   R3V_CMD_BIND_VERTEX_BUFFERS,
   R3V_CMD_DRAW,
   R3V_CMD_DRAW_INDIRECT,
   R3V_CMD_DRAW_INDEXED,
   R3V_CMD_DRAW_INDEXED_INDIRECT,
   R3V_CMD_PUSH_CONSTANTS,
   R3V_CMD_END_RENDER_PASS,
   R3V_CMD_COPY_IMAGE_TO_BUFFER,
   R3V_CMD_COPY_BUFFER_TO_IMAGE,
   R3V_CMD_COPY_IMAGE,
   R3V_CMD_BLIT_IMAGE,
   R3V_CMD_CLEAR_COLOR_IMAGE,
   R3V_CMD_CLEAR_DEPTH_STENCIL_IMAGE,
   R3V_CMD_CLEAR_ATTACHMENTS,
   R3V_CMD_FILL_BUFFER,
   R3V_CMD_COPY_BUFFER,
   R3V_CMD_UPDATE_BUFFER,
   R3V_CMD_SET_EVENT,
   R3V_CMD_RESET_EVENT,
   R3V_CMD_PIPELINE_BARRIER,
   R3V_CMD_DISPATCH,
   R3V_CMD_BIND_DESCRIPTOR_SETS,
   R3V_CMD_BEGIN_QUERY,
   R3V_CMD_END_QUERY,
   R3V_CMD_RESET_QUERY_POOL,
   R3V_CMD_COPY_QUERY_POOL_RESULTS,
   R3V_CMD_SET_DYNAMIC_STATE,
};

struct r3v_query_pool;

enum r3v_descriptor_bind_target {
   R3V_DESCRIPTOR_BIND_GRAPHICS = 1u << 0,
   R3V_DESCRIPTOR_BIND_COMPUTE = 1u << 1,
};

/* VkBindDescriptorSetsInfo stageFlags can name both graphics and compute
 * stages.  Keep the target decision in one small helper so recording and
 * replay cannot collapse a mixed bind onto one pipeline bind point. */
static inline uint32_t
r3v_descriptor_bind_targets(VkShaderStageFlags stage_flags)
{
   const VkShaderStageFlags graphics_stages =
      VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT |
      VK_SHADER_STAGE_MESH_BIT_EXT;
   uint32_t targets = 0;

   if (stage_flags & graphics_stages)
      targets |= R3V_DESCRIPTOR_BIND_GRAPHICS;
   if (stage_flags & VK_SHADER_STAGE_COMPUTE_BIT)
      targets |= R3V_DESCRIPTOR_BIND_COMPUTE;
   return targets;
}

struct r3v_cmd_begin_render_pass {
   /* Up to PIPE_MAX_COLOR_BUFS color attachments, indexed by attachment slot
    * so a fragment-shader output at location i targets color_image[i] (the
    * R300 ROP binds COLOROFFSET0+4*i and US_OUT_FMT_i in the same slot order).
    * A slot for VK_ATTACHMENT_UNUSED stays NULL here; replay fills holes before
    * a later bound slot with throwaway cbufs because r300g cannot represent NULL
    * MRT holes.  Replay selects each attachment tile from the pass origin, so
    * different full image extents can have different final-tile sizes. */
   uint32_t              color_count;
   struct r3v_image  *color_image[PIPE_MAX_COLOR_BUFS];
   VkAttachmentLoadOp    load_op[PIPE_MAX_COLOR_BUFS];
   VkClearColorValue     clear_color[PIPE_MAX_COLOR_BUFS];
   enum pipe_format      color_format[PIPE_MAX_COLOR_BUFS];
   int32_t               render_area_offset_x;
   int32_t               render_area_offset_y;
   uint32_t              width;
   uint32_t              height;
   /* Depth/stencil attachment; NULL when the pass has none, in which case
    * every draw in the pass runs with depth and stencil tests disabled (the
    * Vulkan no-attachment semantics, and the only defined r300g behaviour
    * since no zsbuf is bound). */
   struct r3v_image  *ds_image;
   enum pipe_format      ds_format;
   VkAttachmentLoadOp    ds_load_op;
   float                 clear_depth;
   uint32_t              clear_stencil;
   /* The subpass reads an attachment it also writes (self-dependency); the
    * replay binds the input attachment through a snapshot copy refreshed at
    * each in-pass pipeline barrier. */
   bool                  input_self_dep;
};

struct r3v_cmd_bind_pipeline {
   struct r3v_pipeline *pipeline;
};

struct r3v_cmd_set_viewport {
   VkViewport vp;
};

struct r3v_cmd_set_scissor {
   VkRect2D scissor;
};

struct r3v_cmd_bind_vertex_buffers {
   uint32_t              first_binding;
   uint32_t              binding_count;
   struct r3v_buffer *buffers[R3V_MAX_VERTEX_BINDINGS];
   VkDeviceSize          offsets[R3V_MAX_VERTEX_BINDINGS];
   /* vkCmdBindVertexBuffers2 payload: with the dynamic-stride state the
    * bind-time strides are authoritative over the pipeline's vertex-input
    * description, and sizes bound the robustness clamp below whole-buffer. */
   VkDeviceSize          strides[R3V_MAX_VERTEX_BINDINGS];
   VkDeviceSize          sizes[R3V_MAX_VERTEX_BINDINGS];
   bool                  has_strides;
};

struct r3v_cmd_draw {
   uint32_t            count;
   uint32_t            first;
   uint32_t            instances;
   uint32_t            first_instance;
   VkPrimitiveTopology topology; /* snapshotted from bound pipeline at record time */
};

/* One vkCmdDrawIndirect.  The draw parameters live in a buffer read at execution
 * time, so replay CPU-maps it (r3v buffers are host-visible), reads each
 * VkDrawIndirectCommand at offset + i*stride, and runs the normal draw path per
 * command.  topology is snapshotted from the bound pipeline like the direct draw. */
struct r3v_cmd_draw_indirect {
   struct r3v_buffer *buffer;
   VkDeviceSize          offset;
   uint32_t              draw_count;
   uint32_t              stride;
   VkPrimitiveTopology   topology;
};

/* One vkCmdDrawIndexed.  The index buffer bound by vkCmdBindIndexBuffer[2] is
 * snapshotted here at record time (like topology), so replay needs no separate
 * index-bind command in the stream.  index_offset is the byte offset of the
 * bound range and index_range its byte length; replay folds the offset into the
 * gallium element start (pipe_draw_start_count_bias.start is in index elements)
 * and clamps index_count against index_range for robustBufferAccess. */
struct r3v_cmd_draw_indexed {
   struct r3v_buffer *index_buffer;
   VkDeviceSize          index_offset;
   VkDeviceSize          index_range;
   uint32_t              index_size;   /* 1, 2, or 4 bytes per index */
   uint32_t              index_count;
   uint32_t              first_index;
   int32_t               vertex_offset;
   uint32_t              instances;
   uint32_t              first_instance;
   VkPrimitiveTopology   topology;
};

/* One vkCmdDrawIndexedIndirect.  Combines the indirect-args buffer of
 * r3v_cmd_draw_indirect with the bound index state of r3v_cmd_draw_indexed:
 * replay CPU-reads a VkDrawIndexedIndirectCommand at offset + i*stride and
 * synthesizes one R3V_CMD_DRAW_INDEXED per command against the snapshotted
 * index buffer. */
struct r3v_cmd_draw_indexed_indirect {
   struct r3v_buffer *buffer;       /* indirect-args buffer */
   VkDeviceSize          offset;
   uint32_t              draw_count;
   uint32_t              stride;
   struct r3v_buffer *index_buffer; /* bound index buffer, snapshotted */
   VkDeviceSize          index_offset;
   VkDeviceSize          index_range;
   uint32_t              index_size;   /* 1, 2, or 4 bytes per index */
   VkPrimitiveTopology   topology;
};

/* One vkCmdPushConstants.  Recorded into the entry stream so replay applies the
 * window updates in order before each draw; the replay loop keeps a running
 * maxPushConstantsSize buffer that a push-constants-only pipeline binds at
 * CONST[0].  data carries the size bytes written at offset. */
struct r3v_cmd_push_constants {
   uint32_t              offset;
   uint32_t              size;
   uint8_t               data[R3V_MAX_PUSH_CONSTANTS_SIZE];
};

struct r3v_cmd_copy_image_to_buf {
   struct r3v_image  *src;
   struct r3v_buffer *dst;
   VkBufferImageCopy2    region;
};

/* One region of a vkCmdCopyBufferToImage2.  Replayed as a tile-iterated CPU
 * upload: map the source buffer once, then map each touched destination image
 * tile for write.  This mirrors r3v_copy_image_region_to_buffer, so
 * multi-tile images are handled by the same tile walk in the opposite direction. */
struct r3v_cmd_copy_buf_to_image {
   struct r3v_buffer *src;
   struct r3v_image  *dst;
   VkBufferImageCopy2    region;
};

/* One region of a vkCmdCopyImage2.  Replayed as image -> linear staging buffer
 * -> image, reusing the two tile-iterated transfer paths.  The full region is
 * staged before any destination write, so same-image copies never overwrite
 * source texels before they are read. */
struct r3v_cmd_copy_image {
   struct r3v_image *src;
   struct r3v_image *dst;
   VkImageCopy2         region;
};

/* One region of a vkCmdBlitImage2.  Unlike the copy commands, a blit can scale
 * and filter, so it is replayed on the GPU through pipe->blit (r300_blit ->
 * util_blitter), which carries the scale, filter, and format cast the CPU tile
 * walk does not.  r300 samples the blit source as a texture and takes TX_WIDTH
 * from the source resource, so r3v tiles every optimal image at the sampler
 * cap and r3v_replay_blit walks the source and destination tile grids,
 * issuing one pipe->blit per tile pair so a source larger than the cap is still
 * sampled one in-cap tile at a time. */
struct r3v_cmd_blit_image {
   struct r3v_image *src;
   struct r3v_image *dst;
   VkImageBlit2         region;
   VkFilter             filter;
};

/* One vkCmdClearColorImage subresource range.  Replayed as a tile-iterated CPU
 * fill: pack the clear value to the image format once, then write it to every
 * texel of each tile.  r3v images are single mip and single layer, so the
 * range covers the whole image. */
struct r3v_cmd_clear_color_image {
   struct r3v_image    *image;
   VkClearColorValue       color;
   VkImageSubresourceRange range;
};

struct r3v_cmd_clear_depth_stencil_image {
   struct r3v_image       *image;
   VkClearDepthStencilValue   value;
   VkImageSubresourceRange    range;   /* range.aspectMask selects depth/stencil */
};

/* One clear rect from vkCmdClearAttachments.  Replayed in the active render
 * pass with the rect clipped to each r3v render-area tile; color uses the
 * named subpass slot, while depth/stencil clears the bound zsbuf tile. */
struct r3v_cmd_clear_attachments {
   VkImageAspectFlags aspect;
   /* For a colour clear, the subpass colour-attachment slot to clear (the
    * VkClearAttachment::colorAttachment index); selects color_image[slot] in
    * the bound render pass.  Ignored for depth/stencil aspects. */
   uint32_t           color_attachment;
   VkClearColorValue  color;
   float              depth;
   uint32_t           stencil;
   VkRect2D           rect;
};

/* One vkCmdFillBuffer: fill [offset, offset+size) of a buffer with a repeated
 * 32-bit value.  Replayed as a CPU map-and-fill in the post-fence pass; size
 * may be VK_WHOLE_SIZE, resolved to the buffer tail at replay. */
struct r3v_cmd_fill_buffer {
   struct r3v_buffer *buffer;
   VkDeviceSize          offset;
   VkDeviceSize          size;
   uint32_t              data;
};

/* One region of a vkCmdCopyBuffer2.  Replayed as a CPU memcpy in the post-fence
 * pass; an aliasing src==dst copy maps the union of both ranges once and uses
 * memmove so overlap is well defined. */
struct r3v_cmd_copy_buffer {
   struct r3v_buffer *src;
   struct r3v_buffer *dst;
   VkDeviceSize          src_offset;
   VkDeviceSize          dst_offset;
   VkDeviceSize          size;
};

/* One vkCmdUpdateBuffer.  The inline source bytes are caller-owned only for the
 * call, so the recorder copies them into command-pool storage (data); the cmd
 * buffer frees it at reset and destroy because the buffer may be submitted
 * repeatedly. */
struct r3v_cmd_update_buffer {
   struct r3v_buffer *buffer;
   VkDeviceSize          offset;
   VkDeviceSize          size;
   void                 *data;
};

/* One vkCmdSetEvent2 / vkCmdResetEvent2.  Replayed in the post-fence CPU pass as
 * a host status write to the event, so a GetEventStatus after submit observes
 * it.  The event object outlives the command buffer's use (spec rule), so the
 * recorder keeps a plain pointer. */
struct r3v_cmd_event {
   struct r3v_event *event;
};

/* One image layout transition from a vkCmdPipelineBarrier2 call, so the
 * resource-state ledger can be updated at replay time.  vkCmdPipelineBarrier2
 * records one entry per VkImageMemoryBarrier2 in the dependency, so every
 * image's layout reaches the ledger; image is NULL for a barrier with no
 * image memory barriers (the entry then only marks a replay flush boundary). */
struct r3v_cmd_pipeline_barrier {
   struct r3v_image *image;
   VkImageLayout        new_layout;
};

/* One vkCmdDispatch.  The group counts are recorded for the executor that
 * lowers an admitted kernel onto the compute-as-raster substrate.  The
 * pipeline snapshot is taken at record time (cmd->bound_compute_pipeline)
 * so the replay can decide whether to drive the identity-map orchestrator
 * (and reach the kernel's vs_cso / fs_cso / identity_map slots) or fall
 * through to the no-op compute-lifecycle path. */
struct r3v_cmd_dispatch {
   uint32_t                       group_count_x;
   uint32_t                       group_count_y;
   uint32_t                       group_count_z;
   const struct r3v_pipeline  *pipeline;
};

/* One vkCmdBindDescriptorSets2KHR.  The runtime's legacy vk_common shim
 * (vk_command_buffer.c) forwards both the 1.0/1.1 entrypoint and the
 * VkBindDescriptorSetsInfoKHR form through CmdBindDescriptorSets2KHR on the
 * device dispatch table, so r3v only implements the 2KHR variant.  The
 * set handles stay valid for the cmd-buffer's lifetime of use (spec rule);
 * dynamic offsets are caller-owned for the call so the values are copied
 * inline.  The replay stage maps the bound sets to pipe_context
 * sampler_views / shader_buffers / shader_images / constant_buffers. */
struct r3v_cmd_bind_descriptor_sets {
   VkPipelineBindPoint           bind_point; /* single-target diagnostic */
   VkShaderStageFlags            stage_flags; /* authoritative replay mask */
   VkPipelineLayout              pipeline_layout;
   uint32_t                      first_set;
   uint32_t                      set_count;
   struct r3v_descriptor_set *sets[R3V_MAX_BOUND_DESCRIPTOR_SETS];
   uint32_t                      dynamic_offset_count;
   uint32_t                      dynamic_offsets[R3V_MAX_DYNAMIC_OFFSETS];
   /* Replay state points these views at per-bind-point storage.  Keeping the
    * tables outside each recorded entry avoids paying for replay-only state in
    * every command. */
   uint32_t                     *dynamic_offset_count_by_set;
   uint32_t                    (*dynamic_offsets_by_set)[R3V_MAX_DYNAMIC_OFFSETS];
};

/* One vkCmdBeginQuery / vkCmdEndQuery.  r300 supports only occlusion queries;
 * the replay brackets the spanned draws of a single-tile submit with one r300
 * occlusion query and stores the count into pool->queries[query] at end-query.
 * The pool outlives the command buffer's use (spec rule), so a plain pointer. */
struct r3v_cmd_query {
   struct r3v_query_pool *pool;
   uint32_t                  query;
};

/* One vkCmdResetQueryPool range.  Replayed as a host clear of the slots'
 * availability and result (tile-independent, applied once per submit). */
struct r3v_cmd_reset_query_pool {
   struct r3v_query_pool *pool;
   uint32_t                  first_query;
   uint32_t                  query_count;
};

/* One vkCmdCopyQueryPoolResults.  r3v assigns no buffer device address, so
 * the vk_common implementation (which resolves the buffer through
 * vk_buffer_address and asserts on a zero device address) cannot run; copy on
 * the host from the same per-slot storage GetQueryPoolResults reads, into the
 * destination buffer's mapped resource at replay. */
struct r3v_cmd_copy_query_pool_results {
   struct r3v_query_pool *pool;
   struct r3v_buffer     *dst;
   uint32_t                  first_query;
   uint32_t                  query_count;
   VkDeviceSize              dst_offset;
   VkDeviceSize              stride;
   VkQueryResultFlags        flags;
};


/* One recorded vkCmdSet* dynamic-state call.  flags names which fields the
 * entry carries; the replay walker merges entries into its shadow and applies
 * the result at draw time (transient rasterizer/DSA CSOs, stencil ref, blend
 * colour, topology override). */
#define R3V_DYN_CULL          (1u << 0)
#define R3V_DYN_FRONT_FACE    (1u << 1)
#define R3V_DYN_TOPOLOGY      (1u << 2)
#define R3V_DYN_DEPTH_TEST    (1u << 3)
#define R3V_DYN_DEPTH_WRITE   (1u << 4)
#define R3V_DYN_DEPTH_OP      (1u << 5)
#define R3V_DYN_DEPTH_BOUNDS  (1u << 6)
#define R3V_DYN_STENCIL_TEST  (1u << 7)
#define R3V_DYN_STENCIL_OP    (1u << 8)
#define R3V_DYN_STENCIL_CMP_MASK (1u << 9)
#define R3V_DYN_STENCIL_WR_MASK  (1u << 10)
#define R3V_DYN_STENCIL_REF   (1u << 11)
#define R3V_DYN_DEPTH_BIAS    (1u << 12)
#define R3V_DYN_BLEND_CONST   (1u << 13)
#define R3V_DYN_LINE_WIDTH    (1u << 14)
#define R3V_DYN_DEPTH_BIAS_EN (1u << 15)
#define R3V_DYN_LINE_STIPPLE  (1u << 16)

struct r3v_cmd_set_dynamic {
   uint32_t            flags;
   VkCullModeFlags     cull;
   VkFrontFace         front;
   VkPrimitiveTopology topology;
   VkBool32            depth_test;
   VkBool32            depth_write;
   VkCompareOp         depth_op;
   VkBool32            depth_bounds;
   VkBool32            stencil_test;
   VkStencilFaceFlags  face_mask;     /* for the per-face stencil fields */
   VkStencilOp         sfail, spass, sdepth_fail;
   VkCompareOp         scompare;
   uint32_t            cmp_mask, wr_mask, ref;
   float               bias_const, bias_clamp, bias_slope;
   VkBool32            bias_enable;
   float               blend_const[4];
   float               line_width;
   uint32_t            stipple_factor;
   uint16_t            stipple_pattern;
};

struct r3v_cmd_entry {
   enum r3v_cmd_type type;
   union {
      struct r3v_cmd_set_dynamic            set_dyn;
      struct r3v_cmd_begin_render_pass    begin_rp;
      struct r3v_cmd_bind_pipeline        bind_pipeline;
      struct r3v_cmd_set_viewport         set_vp;
      struct r3v_cmd_set_scissor          set_sc;
      struct r3v_cmd_bind_vertex_buffers  bind_vbufs;
      struct r3v_cmd_draw                 draw;
      struct r3v_cmd_draw_indirect        draw_indirect;
      struct r3v_cmd_draw_indexed         draw_indexed;
      struct r3v_cmd_draw_indexed_indirect draw_indexed_indirect;
      struct r3v_cmd_push_constants       push_constants;
      struct r3v_cmd_copy_image_to_buf    copy_img_buf;
      struct r3v_cmd_copy_buf_to_image    copy_buf_img;
      struct r3v_cmd_copy_image           copy_image;
      struct r3v_cmd_blit_image           blit_image;
      struct r3v_cmd_clear_color_image    clear_color_image;
      struct r3v_cmd_clear_depth_stencil_image clear_depth_stencil_image;
      struct r3v_cmd_clear_attachments    clear_attachments;
      struct r3v_cmd_fill_buffer          fill_buffer;
      struct r3v_cmd_copy_buffer          copy_buffer;
      struct r3v_cmd_update_buffer        update_buffer;
      struct r3v_cmd_event                event;
      struct r3v_cmd_pipeline_barrier     barrier;
      struct r3v_cmd_dispatch             dispatch;
      struct r3v_cmd_bind_descriptor_sets bind_dsets;
      struct r3v_cmd_query                query;
      struct r3v_cmd_reset_query_pool     reset_query_pool;
      struct r3v_cmd_copy_query_pool_results copy_query_pool_results;
   };
};

struct r3v_render_pass;

/* One render-pass attachment resolved to its backing image at CmdBeginRenderPass:
 * each subpass binds its targets by indexing this by attachment number, so a
 * subpass transition needs no second framebuffer walk (and imageless views,
 * supplied only at begin time, are captured here). */
struct r3v_resolved_attachment {
   struct r3v_image *image;
   enum pipe_format     format;
   VkAttachmentLoadOp   load_op;
   VkClearValue         clear;
};

struct r3v_cmd_buffer {
   struct vk_command_buffer  base;  /* must be first; dispatchable */
   struct r3v_cmd_entry  *entries;
   uint32_t                  entry_count;
   uint32_t                  entry_cap;
   struct r3v_pipeline   *bound_pipeline;
   struct r3v_pipeline   *bound_compute_pipeline;
   struct r3v_image      *current_color_image;
   /* Index buffer bound by vkCmdBindIndexBuffer[2], snapshotted into each
    * R3V_CMD_DRAW_INDEXED entry at record time. */
   struct r3v_buffer     *bound_index_buffer;
   VkDeviceSize              bound_index_offset;
   VkDeviceSize              bound_index_range;
   uint32_t                  bound_index_size;

   /* Active classic render pass (NULL outside one / in dynamic rendering).  Set
    * at CmdBeginRenderPass, advanced by CmdNextSubpass, cleared at
    * CmdEndRenderPass; drives binding each subpass's own framebuffer. */
   const struct r3v_render_pass  *current_render_pass;
   uint32_t                          current_subpass;
   int32_t                           current_rp_offset_x;
   int32_t                           current_rp_offset_y;
   uint32_t                          current_rp_width;
   uint32_t                          current_rp_height;
   struct r3v_resolved_attachment current_attachments[PIPE_MAX_COLOR_BUFS + 1];
};

VK_DEFINE_HANDLE_CASTS(r3v_cmd_buffer, base.base, VkCommandBuffer,
                        VK_OBJECT_TYPE_COMMAND_BUFFER)

extern const struct vk_command_buffer_ops r3v_cmd_buffer_ops;

#ifdef __cplusplus
}
#endif

#endif /* R3V_CMD_BUFFER_H */
