/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_state.h"

#include "amd/terascale/common/terascale_format.h"
#include "util/macros.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool
terakan_rendering_attachment_has_resolve(VkRenderingAttachmentInfo const * const attachment)
{
   return attachment != NULL &&
          (attachment->resolveMode != VK_RESOLVE_MODE_NONE ||
           attachment->resolveImageView != VK_NULL_HANDLE);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBeginRendering(VkCommandBuffer const commandBuffer,
                          VkRenderingInfo const * const pRenderingInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_state_draw * const state = &command_writer->state_draw;

   uint32_t clear_attachment_count = 0;
   VkClearAttachment clear_attachments[TERAKAN_COLOR_HW_RTV_COUNT + 1];

   bool has_unsupported_resolve =
      terakan_rendering_attachment_has_resolve(pRenderingInfo->pDepthAttachment) ||
      terakan_rendering_attachment_has_resolve(pRenderingInfo->pStencilAttachment);
   if (!has_unsupported_resolve) {
      for (uint32_t color_attachment_index = 0;
           color_attachment_index < pRenderingInfo->colorAttachmentCount; ++color_attachment_index) {
         if (terakan_rendering_attachment_has_resolve(
                &pRenderingInfo->pColorAttachments[color_attachment_index])) {
            has_unsupported_resolve = true;
            break;
         }
      }
   }
   if (unlikely(has_unsupported_resolve)) {
      vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                  VK_ERROR_VALIDATION_FAILED_EXT);
      return;
   }

   uint16_t window_scissor_tl_br_xy[4];
   terakan_state_translate_window_rect_unpacked(&pRenderingInfo->renderArea,
                                                 window_scissor_tl_br_xy);
   terakan_state_draw_finalize_scissor(window_scissor_tl_br_xy);

   /* Update the window scissor in hw_state_draw so it is emitted before the next draw and
    * properly re-emitted on new indirect buffers.  The previous direct emission approach bypassed
    * dirty tracking, causing stale values to be re-emitted on IB restart.
    */
   uint32_t const pa_sc_window_scissor_tl =
      S_028204_TL_X(window_scissor_tl_br_xy[0]) |
      S_028204_TL_Y(window_scissor_tl_br_xy[1]) | S_028204_WINDOW_OFFSET_DISABLE(1);
   uint32_t const pa_sc_window_scissor_br =
      S_028208_BR_X(window_scissor_tl_br_xy[2]) |
      S_028208_BR_Y(window_scissor_tl_br_xy[3]);

   bool const scissor_modified =
      command_writer->hw_state_draw.pa_sc_window_scissor_tl != pa_sc_window_scissor_tl ||
      command_writer->hw_state_draw.pa_sc_window_scissor_br != pa_sc_window_scissor_br;
   command_writer->hw_state_draw.pa_sc_window_scissor_tl = pa_sc_window_scissor_tl;
   command_writer->hw_state_draw.pa_sc_window_scissor_br = pa_sc_window_scissor_br;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_SC_WINDOW_SCISSOR,
                                 scissor_modified);

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);
   struct terakan_instance const * const inst =
      container_of(terakan_device_physical_device(device)->vk.base.instance,
                   struct terakan_instance const, vk);
   if (unlikely(inst->debug_flags & TERAKAN_DEBUG_CS_DUMP)) {
      fprintf(stderr, "terakan: pm4: SET_CONTEXT_REG PA_SC_WINDOW_SCISSOR_TL = 0x%08X\n",
              pa_sc_window_scissor_tl);
      fprintf(stderr, "terakan: pm4: SET_CONTEXT_REG PA_SC_WINDOW_SCISSOR_BR = 0x%08X\n",
              pa_sc_window_scissor_br);
      fprintf(stderr, "terakan: state: window_scissor tl=(%u,%u) br=(%u,%u)\n",
              window_scissor_tl_br_xy[0], window_scissor_tl_br_xy[1],
              window_scissor_tl_br_xy[2], window_scissor_tl_br_xy[3]);
   }

   terakan_state_draw_set_pending(state, TERAKAN_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER);
   state->db_depth_stencil_buffer.bo = NULL;
   memset(&state->db_depth_stencil_buffer.descriptor, 0,
          sizeof(state->db_depth_stencil_buffer.descriptor));
   VkClearDepthStencilValue depth_stencil_clear_value = {};
   VkImageAspectFlags depth_stencil_clear_aspects = 0;
   if (pRenderingInfo->pDepthAttachment != NULL) {
      struct terakan_image_view const * const depth_view =
         terakan_image_view_from_handle(pRenderingInfo->pDepthAttachment->imageView);
      if (depth_view != NULL &&
          G_028040_FORMAT(depth_view->depth_stencil.z_info) != V_028040_Z_INVALID) {
         state->db_depth_stencil_buffer.bo = depth_view->bo;
         state->db_depth_stencil_buffer.descriptor.view = depth_view->depth_stencil.view;
         state->db_depth_stencil_buffer.descriptor.z_info = depth_view->depth_stencil.z_info;
         state->db_depth_stencil_buffer.descriptor.z_base = depth_view->depth_stencil.z_base;
         state->db_depth_stencil_buffer.descriptor.size = depth_view->depth_stencil.size;
         state->db_depth_stencil_buffer.descriptor.slice = depth_view->depth_stencil.slice;
         if (pRenderingInfo->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            depth_stencil_clear_value.depth =
               pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth;
            depth_stencil_clear_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
         }
      }
   }
   if (pRenderingInfo->pStencilAttachment != NULL) {
      struct terakan_image_view const * const stencil_view =
         terakan_image_view_from_handle(pRenderingInfo->pStencilAttachment->imageView);
      if (stencil_view != NULL &&
          G_028044_FORMAT(stencil_view->depth_stencil.stencil_info) != V_028044_STENCIL_INVALID) {
         if (state->db_depth_stencil_buffer.bo == NULL) {
            state->db_depth_stencil_buffer.bo = stencil_view->bo;
            state->db_depth_stencil_buffer.descriptor.view = stencil_view->depth_stencil.view;
            state->db_depth_stencil_buffer.descriptor.z_info =
               stencil_view->depth_stencil.z_info &
               (C_028040_FORMAT & C_028040_ZRANGE_PRECISION & C_028040_TILE_SPLIT);
            state->db_depth_stencil_buffer.descriptor.size = stencil_view->depth_stencil.size;
            state->db_depth_stencil_buffer.descriptor.slice = stencil_view->depth_stencil.slice;
         }
         state->db_depth_stencil_buffer.descriptor.stencil_info =
            stencil_view->depth_stencil.stencil_info;
         state->db_depth_stencil_buffer.descriptor.stencil_base =
            stencil_view->depth_stencil.stencil_base;
         if (pRenderingInfo->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            depth_stencil_clear_value.stencil =
               pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil;
            depth_stencil_clear_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
         }
      }
   }
   if (depth_stencil_clear_aspects) {
      VkClearAttachment * const clear_depth_stencil_attachment =
         &clear_attachments[clear_attachment_count++];
      clear_depth_stencil_attachment->aspectMask = depth_stencil_clear_aspects;
      clear_depth_stencil_attachment->clearValue.depthStencil = depth_stencil_clear_value;
   }

   terakan_state_draw_set_pending(state, TERAKAN_STATE_DRAW_INDEX_CB_COLOR_RTV);
   memset(state->cb_color_rtv.attachments, 0, sizeof(state->cb_color_rtv.attachments));
   for (uint32_t color_attachment_index = 0;
        color_attachment_index < pRenderingInfo->colorAttachmentCount; ++color_attachment_index) {
      VkRenderingAttachmentInfo const * const color_attachment =
         &pRenderingInfo->pColorAttachments[color_attachment_index];

      struct terakan_image_view const * const color_view =
         terakan_image_view_from_handle(color_attachment->imageView);
      if (color_view == NULL ||
          G_028C70_FORMAT(color_view->color.info) == TERASCALE_FORMAT_INDEX_INVALID) {
         continue;
      }

      struct terakan_state_draw_cb_color * const cb_color =
         &state->cb_color_rtv.attachments[color_attachment_index];
      cb_color->bo = color_view->bo;
      memcpy(&cb_color->color, &color_view->color, sizeof(struct terakan_color_descriptor));
      terakan_color_descriptor_image_view_to_color_attachment(&cb_color->color);
      memcpy(&cb_color->meta, &color_view->color_meta,
             sizeof(struct terakan_color_meta_descriptor));

      if (color_attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         VkClearAttachment * const clear_color_attachment =
            &clear_attachments[clear_attachment_count++];
         clear_color_attachment->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         clear_color_attachment->colorAttachment = color_attachment_index;
         clear_color_attachment->clearValue = color_attachment->clearValue;
      }
   }

   if (!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT) && clear_attachment_count != 0) {
      VkClearRect const clear_rect = {
         .rect = pRenderingInfo->renderArea,
         .baseArrayLayer = 0,
         .layerCount = pRenderingInfo->layerCount,
      };
      terakan_CmdClearAttachments(commandBuffer, clear_attachment_count, clear_attachments, 1,
                                  &clear_rect);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdEndRendering(UNUSED VkCommandBuffer const commandBuffer)
{
   /* Resolve attachments are currently rejected in CmdBeginRendering with
    * VK_ERROR_VALIDATION_FAILED_EXT.
    */
}
