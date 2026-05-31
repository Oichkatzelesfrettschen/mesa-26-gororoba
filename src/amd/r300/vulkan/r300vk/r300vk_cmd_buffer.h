/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_CMD_BUFFER_H
#define R300VK_CMD_BUFFER_H

#include "r300vk_private.h"

#include "vk_command_buffer.h"

#include "pipe/p_state.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward references for r300vk_cmd_entry payload types. */
struct r300vk_pipeline;
struct r300vk_image;
struct r300vk_buffer;
struct r300vk_event;
struct r300vk_descriptor_set;
struct r300vk_cmd_bind_descriptor_sets;

#define R300VK_MAX_BOUND_DESCRIPTOR_SETS  8u
#define R300VK_MAX_DYNAMIC_OFFSETS        16u

enum r300vk_cmd_type {
   R300VK_CMD_BEGIN_RENDER_PASS,
   R300VK_CMD_BIND_PIPELINE,
   R300VK_CMD_SET_VIEWPORT,
   R300VK_CMD_SET_SCISSOR,
   R300VK_CMD_BIND_VERTEX_BUFFERS,
   R300VK_CMD_DRAW,
   R300VK_CMD_DRAW_INDIRECT,
   R300VK_CMD_END_RENDER_PASS,
   R300VK_CMD_COPY_IMAGE_TO_BUFFER,
   R300VK_CMD_COPY_BUFFER_TO_IMAGE,
   R300VK_CMD_CLEAR_COLOR_IMAGE,
   R300VK_CMD_CLEAR_ATTACHMENTS,
   R300VK_CMD_FILL_BUFFER,
   R300VK_CMD_COPY_BUFFER,
   R300VK_CMD_UPDATE_BUFFER,
   R300VK_CMD_SET_EVENT,
   R300VK_CMD_RESET_EVENT,
   R300VK_CMD_PIPELINE_BARRIER,
   R300VK_CMD_DISPATCH,
   R300VK_CMD_BIND_DESCRIPTOR_SETS,
};

struct r300vk_cmd_begin_render_pass {
   struct r300vk_image  *color_image;
   VkAttachmentLoadOp    load_op;
   VkClearColorValue     clear_color;
   uint32_t              width;
   uint32_t              height;
   enum pipe_format      color_format;
};

struct r300vk_cmd_bind_pipeline {
   struct r300vk_pipeline *pipeline;
};

struct r300vk_cmd_set_viewport {
   VkViewport vp;
};

struct r300vk_cmd_set_scissor {
   VkRect2D scissor;
};

struct r300vk_cmd_bind_vertex_buffers {
   uint32_t              first_binding;
   uint32_t              binding_count;
   struct r300vk_buffer *buffers[R300VK_MAX_VERTEX_BINDINGS];
   VkDeviceSize          offsets[R300VK_MAX_VERTEX_BINDINGS];
};

struct r300vk_cmd_draw {
   uint32_t            count;
   uint32_t            first;
   uint32_t            instances;
   uint32_t            first_instance;
   VkPrimitiveTopology topology; /* snapshotted from bound pipeline at record time */
};

/* One vkCmdDrawIndirect.  The draw parameters live in a buffer read at execution
 * time, so replay CPU-maps it (r300vk buffers are host-visible), reads each
 * VkDrawIndirectCommand at offset + i*stride, and runs the normal draw path per
 * command.  topology is snapshotted from the bound pipeline like the direct draw. */
struct r300vk_cmd_draw_indirect {
   struct r300vk_buffer *buffer;
   VkDeviceSize          offset;
   uint32_t              draw_count;
   uint32_t              stride;
   VkPrimitiveTopology   topology;
};

struct r300vk_cmd_copy_image_to_buf {
   struct r300vk_image  *src;
   struct r300vk_buffer *dst;
   VkBufferImageCopy2    region;
};

/* One region of a vkCmdCopyBufferToImage2.  Replayed as a tile-iterated CPU
 * upload: map the source buffer once, then map each touched destination image
 * tile for write.  This mirrors r300vk_copy_image_region_to_buffer, so
 * multi-tile images are handled by the same tile walk in the opposite direction. */
struct r300vk_cmd_copy_buf_to_image {
   struct r300vk_buffer *src;
   struct r300vk_image  *dst;
   VkBufferImageCopy2    region;
};

/* One vkCmdClearColorImage subresource range.  Replayed as a tile-iterated CPU
 * fill: pack the clear value to the image format once, then write it to every
 * texel of each tile.  r300vk images are single mip and single layer, so the
 * range covers the whole image. */
struct r300vk_cmd_clear_color_image {
   struct r300vk_image    *image;
   VkClearColorValue       color;
   VkImageSubresourceRange range;
};

/* One color clear rect from vkCmdClearAttachments.  Replayed in the active
 * render pass with the rect clipped to each r300vk tile.  Depth/stencil aspects
 * are intentionally ignored until r300vk has a depth/stencil attachment model. */
struct r300vk_cmd_clear_attachments {
   VkClearColorValue color;
   VkRect2D          rect;
};

/* One vkCmdFillBuffer: fill [offset, offset+size) of a buffer with a repeated
 * 32-bit value.  Replayed as a CPU map-and-fill in the post-fence pass; size
 * may be VK_WHOLE_SIZE, resolved to the buffer tail at replay. */
struct r300vk_cmd_fill_buffer {
   struct r300vk_buffer *buffer;
   VkDeviceSize          offset;
   VkDeviceSize          size;
   uint32_t              data;
};

/* One region of a vkCmdCopyBuffer2.  Replayed as a CPU memcpy in the post-fence
 * pass; an aliasing src==dst copy maps the union of both ranges once and uses
 * memmove so overlap is well defined. */
struct r300vk_cmd_copy_buffer {
   struct r300vk_buffer *src;
   struct r300vk_buffer *dst;
   VkDeviceSize          src_offset;
   VkDeviceSize          dst_offset;
   VkDeviceSize          size;
};

/* One vkCmdUpdateBuffer.  The inline source bytes are caller-owned only for the
 * call, so the recorder copies them into a malloc'd block (data); the cmd buffer
 * frees it at reset and destroy because the buffer may be submitted repeatedly. */
struct r300vk_cmd_update_buffer {
   struct r300vk_buffer *buffer;
   VkDeviceSize          offset;
   VkDeviceSize          size;
   void                 *data;
};

/* One vkCmdSetEvent2 / vkCmdResetEvent2.  Replayed in the post-fence CPU pass as
 * a host status write to the event, so a GetEventStatus after submit observes
 * it.  The event object outlives the command buffer's use (spec rule), so the
 * recorder keeps a plain pointer. */
struct r300vk_cmd_event {
   struct r300vk_event *event;
};

/* One image layout transition from a vkCmdPipelineBarrier2 call, so the
 * resource-state ledger can be updated at replay time.  vkCmdPipelineBarrier2
 * records one entry per VkImageMemoryBarrier2 in the dependency, so every
 * image's layout reaches the ledger; image is NULL for a barrier with no
 * image memory barriers (the entry then only marks a replay flush boundary). */
struct r300vk_cmd_pipeline_barrier {
   struct r300vk_image *image;
   VkImageLayout        new_layout;
};

/* One vkCmdDispatch.  The group counts are recorded for the executor that
 * lowers an admitted kernel onto the compute-as-raster substrate.  The
 * pipeline snapshot is taken at record time (cmd->bound_compute_pipeline)
 * so the replay can decide whether to drive the identity-map orchestrator
 * (and reach the kernel's vs_cso / fs_cso / identity_map slots) or fall
 * through to the no-op compute-lifecycle path. */
struct r300vk_cmd_dispatch {
   uint32_t                       group_count_x;
   uint32_t                       group_count_y;
   uint32_t                       group_count_z;
   const struct r300vk_pipeline  *pipeline;
};

/* One vkCmdBindDescriptorSets2KHR.  The runtime's legacy vk_common shim
 * (vk_command_buffer.c) forwards both the 1.0/1.1 entrypoint and the
 * VkBindDescriptorSetsInfoKHR form through CmdBindDescriptorSets2KHR on the
 * device dispatch table, so r300vk only implements the 2KHR variant.  The
 * set handles stay valid for the cmd-buffer's lifetime of use (spec rule);
 * dynamic offsets are caller-owned for the call so the values are copied
 * inline.  The replay stage maps the bound sets to pipe_context
 * sampler_views / shader_buffers / shader_images / constant_buffers. */
struct r300vk_cmd_bind_descriptor_sets {
   VkPipelineBindPoint           bind_point;
   VkPipelineLayout              pipeline_layout;
   uint32_t                      first_set;
   uint32_t                      set_count;
   struct r300vk_descriptor_set *sets[R300VK_MAX_BOUND_DESCRIPTOR_SETS];
   uint32_t                      dynamic_offset_count;
   uint32_t                      dynamic_offsets[R300VK_MAX_DYNAMIC_OFFSETS];
};

struct r300vk_cmd_entry {
   enum r300vk_cmd_type type;
   union {
      struct r300vk_cmd_begin_render_pass    begin_rp;
      struct r300vk_cmd_bind_pipeline        bind_pipeline;
      struct r300vk_cmd_set_viewport         set_vp;
      struct r300vk_cmd_set_scissor          set_sc;
      struct r300vk_cmd_bind_vertex_buffers  bind_vbufs;
      struct r300vk_cmd_draw                 draw;
      struct r300vk_cmd_draw_indirect        draw_indirect;
      struct r300vk_cmd_copy_image_to_buf    copy_img_buf;
      struct r300vk_cmd_copy_buf_to_image    copy_buf_img;
      struct r300vk_cmd_clear_color_image    clear_color_image;
      struct r300vk_cmd_clear_attachments    clear_attachments;
      struct r300vk_cmd_fill_buffer          fill_buffer;
      struct r300vk_cmd_copy_buffer          copy_buffer;
      struct r300vk_cmd_update_buffer        update_buffer;
      struct r300vk_cmd_event                event;
      struct r300vk_cmd_pipeline_barrier     barrier;
      struct r300vk_cmd_dispatch             dispatch;
      struct r300vk_cmd_bind_descriptor_sets bind_dsets;
   };
};

struct r300vk_cmd_buffer {
   struct vk_command_buffer  base;  /* must be first; dispatchable */
   struct r300vk_cmd_entry  *entries;
   uint32_t                  entry_count;
   uint32_t                  entry_cap;
   struct r300vk_pipeline   *bound_pipeline;
   struct r300vk_pipeline   *bound_compute_pipeline;
   struct r300vk_image      *current_color_image;
};

VK_DEFINE_HANDLE_CASTS(r300vk_cmd_buffer, base.base, VkCommandBuffer,
                        VK_OBJECT_TYPE_COMMAND_BUFFER)

extern const struct vk_command_buffer_ops r300vk_cmd_buffer_ops;

#ifdef __cplusplus
}
#endif

#endif /* R300VK_CMD_BUFFER_H */
