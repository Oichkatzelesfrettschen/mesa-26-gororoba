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

enum r300vk_cmd_type {
   R300VK_CMD_BEGIN_RENDER_PASS,
   R300VK_CMD_BIND_PIPELINE,
   R300VK_CMD_SET_VIEWPORT,
   R300VK_CMD_SET_SCISSOR,
   R300VK_CMD_BIND_VERTEX_BUFFERS,
   R300VK_CMD_DRAW,
   R300VK_CMD_END_RENDER_PASS,
   R300VK_CMD_COPY_IMAGE_TO_BUFFER,
   R300VK_CMD_PIPELINE_BARRIER,
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

#define R300VK_MAX_VERTEX_BINDINGS 16

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

struct r300vk_cmd_copy_image_to_buf {
   struct r300vk_image  *src;
   struct r300vk_buffer *dst;
   VkBufferImageCopy2    region;
};

struct r300vk_cmd_entry {
   enum r300vk_cmd_type type;
   union {
      struct r300vk_cmd_begin_render_pass   begin_rp;
      struct r300vk_cmd_bind_pipeline       bind_pipeline;
      struct r300vk_cmd_set_viewport        set_vp;
      struct r300vk_cmd_set_scissor         set_sc;
      struct r300vk_cmd_bind_vertex_buffers bind_vbufs;
      struct r300vk_cmd_draw                draw;
      struct r300vk_cmd_copy_image_to_buf   copy_img_buf;
   };
};

struct r300vk_cmd_buffer {
   struct vk_command_buffer  base;  /* must be first; dispatchable */
   struct r300vk_cmd_entry  *entries;
   uint32_t                  entry_count;
   uint32_t                  entry_cap;
   struct r300vk_pipeline   *bound_pipeline;
   struct r300vk_image      *current_color_image;
};

VK_DEFINE_HANDLE_CASTS(r300vk_cmd_buffer, base.base, VkCommandBuffer,
                        VK_OBJECT_TYPE_COMMAND_BUFFER)

extern const struct vk_command_buffer_ops r300vk_cmd_buffer_ops;

#ifdef __cplusplus
}
#endif

#endif /* R300VK_CMD_BUFFER_H */
