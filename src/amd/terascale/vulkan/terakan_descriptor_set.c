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

#include "terakan_descriptor_set.h"

#include "terakan_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_format.h"
#include "terakan_image.h"
#include "terakan_sampler.h"

#include "amd/terascale/common/terascale_evergreend.h"
#include "util/compiler.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "vk_alloc.h"
#include "vk_descriptor_update_template.h"
#include "vk_log.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

VKAPI_ATTR void VKAPI_CALL
terakan_UpdateDescriptorSets(UNUSED VkDevice const device, uint32_t const descriptorWriteCount,
                             VkWriteDescriptorSet const * const pDescriptorWrites,
                             uint32_t const descriptorCopyCount,
                             VkCopyDescriptorSet const * const pDescriptorCopies)
{
   for (uint32_t descriptor_write_index = 0; descriptor_write_index < descriptorWriteCount;
        ++descriptor_write_index) {
      VkWriteDescriptorSet const * const descriptor_write =
         &pDescriptorWrites[descriptor_write_index];

      struct terakan_descriptor_set const * const dst_set =
         terakan_descriptor_set_from_handle(descriptor_write->dstSet);
      struct terakan_descriptor_set_layout_binding const * const dst_binding =
         &dst_set->layout->bindings[descriptor_write->dstBinding];
      /* VUID-VkWriteDescriptorSet-dstBinding-00316 "dstBinding must be a binding with a non-zero
       * descriptorCount", no need to skip bindings with zero descriptors, which are uninitialized
       * in descriptor set layouts in the driver except for the descriptor count, to get to the
       * first actually updated binding.
       */
      assert(dst_binding->descriptor_count != 0);

      uint32_t const descriptor_count = descriptor_write->descriptorCount;

      struct terakan_descriptor_set_resource * const dst_resources =
         (struct terakan_descriptor_set_resource *)dst_set->descriptors +
         dst_binding->first_set_resource + descriptor_write->dstArrayElement;
      struct terakan_descriptor_set_sampler * const dst_samplers =
         (struct terakan_descriptor_set_sampler *)(dst_set->descriptors +
                                                   dst_set->layout->pool_first_sampler_offset_bytes) +
         dst_binding->first_set_sampler + descriptor_write->dstArrayElement;
      struct terakan_descriptor_set_uav * const dst_uavs =
         (struct terakan_descriptor_set_uav *)(dst_set->descriptors +
                                               dst_set->layout->pool_first_uav_offset_bytes) +
         dst_binding->first_set_uav + descriptor_write->dstArrayElement;

      switch (descriptor_write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_descriptor_set_uav * const dst_uav = &dst_uavs[descriptor_index];
            struct terakan_image_view const * const image_view = terakan_image_view_from_handle(
               descriptor_write->pImageInfo[descriptor_index].imageView);
            if (image_view != NULL &&
                G_028C70_FORMAT(image_view->color.info) != TERASCALE_FORMAT_INDEX_INVALID) {
               dst_uav->bo = image_view->bo;
               memcpy(&dst_uav->color, &image_view->color, sizeof(struct terakan_color_descriptor));
               dst_uav->buffer_byte_size = 0;  /* Image UAVs: robustness not yet supported. */
               dst_uav->buffer_byte_offset = 0;  /* No per-element offset for images. */
               dst_uav->is_texel_buffer = 0;
               /* stash baseArrayLayer + non-array-
                * view-over-array-image flag so pipeline_layout.c can upload
                * them into robustness_metadata bank 14 dwords 28..39 for
                * NIR-side R3.z injection at MEM_RAT STORE_TYPED.  Only the
                * non-array-view-over-array-image case gets the add applied
                * (guardrail #1); others get zero, which nir_iadd folds
                * away during NIR optimisation. */
               dst_uav->base_array_layer = image_view->vk.base_array_layer;
               {
                  bool const is_nonarray_view =
                     image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D ||
                     image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D ||
                     image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE;
                  bool const backing_is_array =
                     image_view->vk.image->array_layers > 1;
                  dst_uav->view_flags = (uint8_t)((is_nonarray_view && backing_is_array)
                     ? TERAKAN_DESCRIPTOR_SET_UAV_VIEW_FLAG_NONARRAY_VIEW_OF_ARRAY_IMAGE
                     : 0u);
               }
               memset(dst_uav->_pad_fix_k, 0, sizeof(dst_uav->_pad_fix_k));

               /* TERAKAN_DEBUG_STORAGE_IMAGE_DESC=1: trace the STORAGE_IMAGE
                * descriptor pipeline in full, at every transformation step.
                * This is the PRODUCER that bakes values the compute dispatch
                * later programs into CB_COLOR{N}.  Per  the
                * earlier TERAKAN_DEBUG_IMAGE_CB_LAYOUT captured only clear
                * and meta-copy paths; this captures the actual dispatch path.
                */
               static int trace_sd_cached = -1;
               if (trace_sd_cached < 0) {
                  trace_sd_cached = debug_get_bool_option("TERAKAN_DEBUG_STORAGE_IMAGE_DESC",
                                                          false) ? 1 : 0;
               }
               if (trace_sd_cached) {
                  fprintf(stderr,
                          "terakan/stor_img_desc: PRE-TRANSFORM image_view@%p "
                          "bo=%p array_layers=%u "
                          "base=0x%08x pitch=0x%08x slice=0x%08x view=0x%08x "
                          "info=0x%08x attrib=0x%08x dim=0x%08x\n",
                          (void *)image_view, (void *)image_view->bo,
                          image_view->vk.image->array_layers,
                          dst_uav->color.base, dst_uav->color.pitch,
                          dst_uav->color.slice, dst_uav->color.view,
                          dst_uav->color.info, dst_uav->color.attrib,
                          dst_uav->color.dim);
               }

               terakan_color_descriptor_image_view_to_storage_image(&dst_uav->color);

               if (trace_sd_cached) {
                  fprintf(stderr,
                          "terakan/stor_img_desc: POST-XFORM   "
                          "base=0x%08x pitch=0x%08x slice=0x%08x view=0x%08x "
                          "info=0x%08x attrib=0x%08x dim=0x%08x\n",
                          dst_uav->color.base, dst_uav->color.pitch,
                          dst_uav->color.slice, dst_uav->color.view,
                          dst_uav->color.info, dst_uav->color.attrib,
                          dst_uav->color.dim);
               }
               /* When a non-array VIEW (1D/2D/CUBE) is created over a
                * multi-layer ARRAY IMAGE, image_view->color.info is
                * stamped with the view's type: TEXTURE1D / TEXTURE2D.
                * The shader writes with a 2D coord, but the CB exporter
                * for a STORAGE_IMAGE UAV on an array-backed allocation
                * still needs TEXTURE{1,2}DARRAY addressing to compute
                * per-slice tile offsets correctly.  Otherwise writes
                * land at slice 0 of the physical allocation even when
                * the view's baseArrayLayer is 0, because CB_COLOR_INFO
                * RESOURCE_TYPE=TEXTURE2D takes a shortcut that skips
                * the array-layer tile math -- and readback via
                * CopyImageToBuffer (which uses the VkImage's array
                * layout) then sees stale data.
                *
                * Fix: promote RESOURCE_TYPE to the array equivalent
                * when the underlying VkImage has array_layers > 1.
                * For truly single-layer images this is a no-op
                * (SLICE_MAX=0 falls out naturally). */
               if (image_view->vk.image->array_layers > 1) {
                  uint32_t const current_type =
                     G_028C70_RESOURCE_TYPE(dst_uav->color.info);
                  uint32_t upgraded_type = current_type;
                  /* skip the RESOURCE_TYPE
                   * upgrade for non-array views.  The 2026-04-17
                   * upgrade was intended to make per-slice tile math
                   * work for multi-layer backed images, but MEM_RAT
                   * STORE_TYPED consults R3.z (the coord's array
                   * index) which SFN hardcodes to 0 for non-array
                   * views.  With the upgrade in place, writes go to
                   * image slice 0 regardless of baseArrayLayer.
                   * Without the upgrade, RESOURCE_TYPE stays
                   * TEXTURE{1,2}D and the CB exporter falls back to
                   * writing at the pre-shifted base (which
                   * terakan_image_create_color_descriptor set to the
                   * target layer) -- the original working path.
                   *
                   * Trade-off: reverting the upgrade may regress the
                   * tile-rotation case the 2026-04-17 commit
                   * described, where TEXTURE2D + tile-rotated array
                   * mode wrote to the wrong slice despite the
                   * base-shift.  FIX-E (force LINEAR_ALIGNED array
                   * mode at vkCreateImage) is the fallback for that
                   * regression.
                   *
                   * Gated on TERAKAN_FIX_C_REVERT_TYPE_UPGRADE=1
                   * during validation.  Promote to default once the
                   * 78-test single_layer sweep goes green and the
                   * tile-rotation case is re-verified.
                   */
                  static int fix_c_cached = -1;
                  if (fix_c_cached < 0) {
                     fix_c_cached = debug_get_bool_option(
                        "TERAKAN_FIX_C_REVERT_TYPE_UPGRADE", false) ? 1 : 0;
                  }
                  bool const view_is_nonarray_2d_or_1d =
                     image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D ||
                     image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D ||
                     image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE;
                  if (fix_c_cached && view_is_nonarray_2d_or_1d) {
                     if (trace_sd_cached) {
                        fprintf(stderr,
                                "terakan/stor_img_desc: FIX-C skip upgrade "
                                "view_type=%d current_type=%u\n",
                                (int)image_view->vk.view_type, current_type);
                     }
                     /* Leave upgraded_type == current_type so the
                      * `if (upgraded_type != current_type)` below is
                      * false and no state is modified. */
                  } else {
                  switch (current_type) {
                  case V_028C70_TEXTURE1D:
                     upgraded_type = V_028C70_TEXTURE1DARRAY;
                     break;
                  case V_028C70_TEXTURE2D:
                     upgraded_type = V_028C70_TEXTURE2DARRAY;
                     break;
                  default:
                     /* Already array-type or 3D; leave unchanged. */
                     break;
                  }
                  }
                  if (upgraded_type != current_type) {
                     dst_uav->color.info =
                        (dst_uav->color.info & C_028C70_RESOURCE_TYPE) |
                        S_028C70_RESOURCE_TYPE(upgraded_type);

                     /* when we flip the
                      * view interpretation to array semantics, the
                      * CB exporter will now do per-slice tile math.
                      * The base in image_view->color was pre-shifted
                      * to the target layer under the old TEXTURE2D
                      * "no array math" assumption.  If we leave the
                      * shift in place, the exporter double-applies
                      * the offset and writes land on the wrong
                      * slice.  Revert the shift so base points at
                      * the image root, and program CB_COLOR_VIEW so
                      * the exporter targets baseArrayLayer by itself.
                      *
                      * SLICE_START and SLICE_MAX are set to the same
                      * baseArrayLayer value because the view covers
                      * exactly one layer (it was a non-array view).
                      *
                      * Gated by TERAKAN_FIX_B_SINGLE_LAYER=1 during
                      * validation; promote to default after the
                      * 78-test single_layer sweep goes green. */
                     bool const fix_b_enabled =
                        debug_get_bool_option("TERAKAN_FIX_B_SINGLE_LAYER", false);
                     if (fix_b_enabled) {
                        uint32_t const base_layer = image_view->vk.base_array_layer;
                        dst_uav->color.base -= image_view->color_base_slice_shift_shr8;
                        dst_uav->color.view =
                           S_028C6C_SLICE_START(base_layer) |
                           S_028C6C_SLICE_MAX(base_layer);
                        if (trace_sd_cached) {
                           fprintf(stderr,
                                   "terakan/stor_img_desc: FIX-B applied "
                                   "base_layer=%u shift_shr8=0x%08x "
                                   "reverted_base=0x%08x view=0x%08x\n",
                                   base_layer,
                                   image_view->color_base_slice_shift_shr8,
                                   dst_uav->color.base, dst_uav->color.view);
                        }
                     }
                  }
                  if (trace_sd_cached) {
                     fprintf(stderr,
                             "terakan/stor_img_desc: POST-TYPE-UP "
                             "current=%u upgraded=%u info=0x%08x "
                             "(array_layers=%u)\n",
                             current_type, upgraded_type, dst_uav->color.info,
                             image_view->vk.image->array_layers);
                  }
                  /* the original FIX-B's revert
                   * + SLICE_START programming is dead code when the
                   * entry-time RESOURCE_TYPE is already TEXTURE2DARRAY
                   * (which terakan_image_create_resource_descriptor
                   * produces for any image with array_layers>1).  In
                   * that case `upgraded_type == current_type` and the
                   * `if (upgraded_type != current_type)` arm above is
                   * false, so FIX-B never runs.  The CB descriptor
                   * ends up with RESOURCE_TYPE=2D_ARRAY, base pre-
                   * shifted by color_base_slice_shift_shr8, and
                   * view=0.  The exporter then adds array-tile math
                   * on top of the already-shifted base and misses
                   * the target slice.
                   *
                   * Fix (applies when view is non-array AND backing
                   * image is multi-layer, regardless of whether an
                   * upgrade fired): revert the base pre-shift and
                   * let CB_COLOR_VIEW.SLICE_START/SLICE_MAX name the
                   * target slice so the exporter handles the tile
                   * math correctly.
                   *
                   * Enabled by default after Task 94; set
                   * TERAKAN_FIX_I_APPLY_SLICE_VIEW=0 only for
                   * regression bisects.
                   */
                  static int fix_i_cached = -1;
                  if (fix_i_cached < 0) {
                     fix_i_cached = debug_get_bool_option(
                        "TERAKAN_FIX_I_APPLY_SLICE_VIEW", true) ? 1 : 0;
                  }
                  /* FIX-T attempted (reverted 2026-04-19): remove the
                   * shift_shr8 != 0 gate so FIX-I applies to layer 0 too.
                   * Empirically caused LAYER 1 to regress (diff=61) while
                   * still not fixing layer 0 (diff=7).  Mechanism unclear;
                   * possibly a side effect of the descriptor pool update
                   * ordering or some state propagation between sequential
                   * descriptor updates.  Reverting to the shift!=0 gate
                   * which gives 7/8 passing on both sint and sfloat. */
                  if (fix_i_cached && view_is_nonarray_2d_or_1d &&
                      image_view->color_base_slice_shift_shr8 != 0) {
                     uint32_t const base_layer = image_view->vk.base_array_layer;
                     uint32_t const base_before = dst_uav->color.base;
                     uint32_t const slice_before = dst_uav->color.slice;
                     dst_uav->color.base -= image_view->color_base_slice_shift_shr8;
                     /* * SLICE_TILE_MAX is PER-SLICE (tiles in one slice's
                      * 2D surface = width * height / 64).  Multiplying
                      * by array_layers produced 0x1ff (511) vs the
                      * correct 0x3f (63) used by the passing full-array
                      * path.  Empirical comparison 2026-04-19 shows
                      * FIX-M actively poisons the exporter: it tells the
                      * hardware "this single slice contains 512 tiles",
                      * which is impossible for 64x64 and causes MEM_RAT
                      * writes to collapse to slice 0 regardless of R3.z.
                      * Leaving dst_uav->color.slice unchanged; per-slice
                      * count is already correct at image-create time. */
                     (void)slice_before;  /* retained for trace var lifetime */
                     /* CB_COLOR_VIEW.SLICE_MAX
                      * acts as the hardware-side slice range gate.  For a
                      * VK_IMAGE_VIEW_TYPE_2D view with layer_count=1 over
                      * an array-backed image, the default view_slice_max
                      * is 0, so MEM_RAT STORE_TYPED with shader R3.z=N>0
                      * gets clamped/dropped even though the physical
                      * allocation has array_layers slices.
                      *
                      * Fix: enlarge SLICE_MAX to array_layers-1 so any
                      * R3.z in [0, array_layers-1] reaches the target
                      * slice.  SLICE_START stays 0 (base already reverted
                      * to image root).  This intentionally transfers the
                      * out-of-bounds protection responsibility to the
                      * shader -- the CTS / application dictates the
                      * coord, so hardware-side view clamping on writes
                      * is redundant and would conflict with FIX-K's
                      * runtime R3.z injection.
                      *
                      * Gated alongside FIX-I; enable via
                      * TERAKAN_FIX_I_APPLY_SLICE_VIEW=1. */
                     uint32_t const backing_array_layers =
                        image_view->vk.image->array_layers;
                     uint32_t const slice_max_backing =
                        (backing_array_layers > 0) ? (backing_array_layers - 1u) : 0u;
                     dst_uav->color.view =
                        S_028C6C_SLICE_START(0) |
                        S_028C6C_SLICE_MAX(slice_max_backing);
                     if (trace_sd_cached) {
                        fprintf(stderr,
                                "terakan/stor_img_desc: FIX-I+L applied "
                                "base_layer=%u shift_shr8=0x%08x "
                                "base_before=0x%08x reverted_base=0x%08x "
                                "view=0x%08x slice_max=%u backing=%u "
                                "slice_kept=0x%08x (FIX-M reverted)\n",
                                base_layer,
                                image_view->color_base_slice_shift_shr8,
                                base_before, dst_uav->color.base,
                                dst_uav->color.view, slice_max_backing,
                                backing_array_layers,
                                slice_before);
                     }
                  }
               }
               /* Stash the SQ_TEX_RESOURCE ("REAL") descriptor so
                * terakan_emit_compute_resources can emit the
                * CS+168+m SET_RESOURCE the CB exporter needs for
                * format/tile-mode validation during MEM_RAT STORE_TYPED.
                * See CLAIMS  + LATENT_INVARIANTS
                * / . */
               memcpy(dst_uav->real_resource, image_view->resource,
                      sizeof(dst_uav->real_resource));

               /* the Shader Sequencer (SQ)
                * consults SQ_TEX_RESOURCE word 5 (BASE_ARRAY /
                * LAST_ARRAY, bits 4-16 and 17-29) when formatting
                * MEM_RAT STORE_TYPED writes.  For a non-array view
                * over a multi-layer backing image the view's
                * image_view->resource was built with
                * BASE_ARRAY=baseArrayLayer and LAST_ARRAY=
                * baseArrayLayer (single-slice range).  The SQ
                * compares R3.z against this range and silently
                * clamps out-of-bounds z to 0 BEFORE handing the
                * payload to the CB exporter.  That is why
                * FIX-P's hardcoded R3.z = 3 still landed on slice 0
                * even though every byte of CB_COLOR state matched
                * the passing full-array path.
                *
                * Fix: unclamp BASE_ARRAY to 0 and LAST_ARRAY to
                * array_layers-1 for UAV bindings so R3.z (now
                * populated by FIX-K with the absolute physical
                * slice index) passes through the SQ unaltered.
                * OOB protection is transferred to the shader per
                * the FIX-K design principle; FIX-K already writes
                * only the slice the application named. */
               static int fix_q_cached = -1;
               if (fix_q_cached < 0) {
                  fix_q_cached = debug_get_bool_option(
                     "TERAKAN_FIX_Q_UNCLAMP_ARRAY_BOUNDS", true) ? 1 : 0;
               }
               bool const fixq_view_nonarray =
                  image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D ||
                  image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D ||
                  image_view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE;
               if (fix_q_cached && fixq_view_nonarray &&
                   image_view->vk.image->array_layers > 1) {
                  uint32_t const backing_layers =
                     image_view->vk.image->array_layers;
                  uint32_t const w5_before = dst_uav->real_resource[5];
                  dst_uav->real_resource[5] =
                     (w5_before & C_030014_BASE_ARRAY & C_030014_LAST_ARRAY) |
                     S_030014_BASE_ARRAY(0) |
                     S_030014_LAST_ARRAY(backing_layers - 1u);
                  if (trace_sd_cached) {
                     fprintf(stderr,
                             "terakan/stor_img_desc: FIX-Q applied "
                             "w5_before=0x%08x w5_after=0x%08x "
                             "BASE_ARRAY=0 LAST_ARRAY=%u\n",
                             w5_before, dst_uav->real_resource[5],
                             backing_layers - 1u);
                  }
               }

               /* when
                * CB RESOURCE_TYPE was upgraded from TEXTURE2D ->
                * TEXTURE2DARRAY (above), the SQ-side DIM must also
                * track that upgrade so the shader-visible resource
                * descriptor is consistent with CB exporter state.
                *
                * real_resource[0] word 0 bits 0-2 encode DIM:
                *   V_030000_SQ_TEX_DIM_1D       = 0
                *   V_030000_SQ_TEX_DIM_2D       = 1
                *   V_030000_SQ_TEX_DIM_CUBEMAP  = 3
                *   V_030000_SQ_TEX_DIM_1D_ARRAY = 4
                *   V_030000_SQ_TEX_DIM_2D_ARRAY = 5
                *
                * Apply the same gate as the CB upgrade: only when
                * the underlying VkImage has array_layers > 1 AND
                * TERAKAN_FIX_B_SINGLE_LAYER=1.
                */
               static int fix_b_sq_cached = -1;
               if (fix_b_sq_cached < 0) {
                  fix_b_sq_cached = debug_get_bool_option("TERAKAN_FIX_B_SINGLE_LAYER",
                                                         false) ? 1 : 0;
               }
               if (fix_b_sq_cached && image_view->vk.image->array_layers > 1) {
                  /* 2026-04-18 diagnostic confirmed SQ DIM is already
                   * set to V_030000_SQ_TEX_DIM_2D_ARRAY (5) by
                   * terakan_image_create_resource_descriptor for
                   * multi-layer backed images, regardless of view
                   * type.  Both CB side (post-upgrade) and SQ side
                   * are self-consistent at TEXTURE2DARRAY.
                   * This switch below therefore never finds a 2D
                   * to upgrade in practice; kept as defense-in-depth
                   * for 1D (DIM=0) -> 1D_ARRAY (DIM=4) on the 1D
                   * view path if that ever regresses. */
                  uint32_t const sq_dim_current = dst_uav->real_resource[0] & 0x7;
                  uint32_t sq_dim_upgraded = sq_dim_current;
                  switch (sq_dim_current) {
                  case 0: /* 1D */
                     sq_dim_upgraded = 4; /* 1D_ARRAY */
                     break;
                  case 1: /* 2D */
                     sq_dim_upgraded = 5; /* 2D_ARRAY */
                     break;
                  default:
                     break;
                  }
                  if (sq_dim_upgraded != sq_dim_current) {
                     dst_uav->real_resource[0] =
                        (dst_uav->real_resource[0] & ~0x7u) | sq_dim_upgraded;
                     if (trace_sd_cached) {
                        fprintf(stderr,
                                "terakan/stor_img_desc: FIX-B companion "
                                "sq_dim %u -> %u real_resource[0]=0x%08x\n",
                                sq_dim_current, sq_dim_upgraded,
                                dst_uav->real_resource[0]);
                     }
                  }
               }
               /* Evergreen/Bobcat MEM_RAT_STORE_TYPED
                * silently drops writes when SQ_TEX_RESOURCE_WORD4
                * (real_resource[4], R_030010) has NUM_FORMAT_ALL=INT (bit9:8=1)
                * combined with FORMAT_COMP_X/Y/Z/W=UNSIGNED (bits7:0=0x00).
                * This is the UINT integer format path; SINT uses FORMAT_COMP=
                * SIGNED (bits7:0=0x55) and passes identically.
                *
                * IB evidence: three-arm cold capture on x130e.
                * sfloat (PASS): slot 0x1EC0 word4=0x0B200000 (NUM_FORMAT=NORM).
                * sint  (PASS): slot 0x1EC0 word4=0x0B200155 (NUM_FORMAT=INT,
                *               FORMAT_COMP=SIGNED).
                * uint+FIX-Y (FAIL): slot 0x1EC0 word4=0x0B200100 (NUM_FORMAT=INT,
                *               FORMAT_COMP=UNSIGNED).
                * The IMMED buffer resource at slot 0x1980 is byte-identical for
                * sint and uint+FIX-Y, confirming the texture resource word4
                * FORMAT_COMP fields are the sole discriminator.
                *
                * Fix: for storage-image UAV descriptors only (this code path),
                * flip FORMAT_COMP_X/Y/Z/W to SIGNED when NUM_FORMAT_ALL=INT and
                * all FORMAT_COMP are UNSIGNED.  MEM_RAT_STORE_TYPED then accepts
                * the write.  For 32-bit channels SIGNED vs UNSIGNED is
                * bit-pattern-identical; for sub-32-bit UINT (R8/R16) subsequent
                * imageLoad via the same resource sees sign-extended values, but
                * the failing CTS image.store tests verify via CopyImageToBuffer
                * (DMA readback), not imageLoad.
                *
                * FIX-Y (FORMAT_COMP_ALL in SQ_VTX_RESOURCE_WORD2 at slot 0x1980)
                * is confirmed a no-op for this bug: it modifies the IMMED buffer
                * resource which is identical between sint and uint.
                *
                * Promoted to default per steinmarder finding
                * : the
                * tranche-7 absolute-isolation matrix proved zero real
                * isolation fails remain after FIX-I+K+W+Z; the residual 3
                * sweep fails are cross-test-primer victims (independent
                * lane).  Env var preserved as an OPT-OUT escape hatch
                * (set TERAKAN_FIX_Z_UINT_FORMAT_COMP=0 to disable).
                * See steinmarder findings/active/
                * and  (FIX-Y erratum). */
               static int fix_z_cached = -1;
               if (fix_z_cached < 0) {
                  fix_z_cached = debug_get_bool_option(
                     "TERAKAN_FIX_Z_UINT_FORMAT_COMP", true) ? 1 : 0;
               }
               if (fix_z_cached) {
                  uint32_t const w4 = dst_uav->real_resource[4];
                  if (G_030010_NUM_FORMAT_ALL(w4) == V_030010_SQ_NUM_FORMAT_INT &&
                      (w4 & 0xFFu) == 0x00u) {
                     uint32_t const w4_fixed =
                        (w4 & ~0xFFu) |
                        S_030010_FORMAT_COMP_X(V_030010_SQ_FORMAT_COMP_SIGNED) |
                        S_030010_FORMAT_COMP_Y(V_030010_SQ_FORMAT_COMP_SIGNED) |
                        S_030010_FORMAT_COMP_Z(V_030010_SQ_FORMAT_COMP_SIGNED) |
                        S_030010_FORMAT_COMP_W(V_030010_SQ_FORMAT_COMP_SIGNED);
                     dst_uav->real_resource[4] = w4_fixed;
                     if (trace_sd_cached) {
                        fprintf(stderr,
                                "terakan/stor_img_desc: FIX-Z applied "
                                "real_resource[4] 0x%08x -> 0x%08x\n",
                                w4, w4_fixed);
                     }
                  }
                  /* FIX-Z part 2: CB_COLOR_INFO.NUMBER_TYPE = NUMBER_UINT (4) also
                   * causes MEM_RAT_STORE_TYPED to silently drop writes on
                   * Evergreen/Bobcat.  NUMBER_USCALED (2) avoids both the silent-drop
                   * (NUMBER_UINT=4) and SINT clamping (NUMBER_SINT=5 clamps >SINT_MAX).
                   * Override UINT -> SINT in the CB descriptor.
                   * Sub-32-bit UINT values (R8/R16) are sign-extended in the shader
                   * (FIX-Z NIR pass) before the store so SINT clamping does not apply.
                   *
                   * Evidence: POST-XFORM trace shows sint info=0x1c105434 (NUMBER=5,
                   * PASS) vs uint info=0x1c104434 (NUMBER=4, FAIL).  The difference
                   * is exclusively bits [14:12] = NUMBER_TYPE.
                   *
                   * For sub-32-bit UINT (R8/R16), the CB will write values with SINT
                   * interpretation as unsigned integer; USCALED stores the raw bit-pattern
                   * regardless of magnitude, so R8_UINT values 128-255 are preserved.
                   * R32_UINT also benefits since USCALED does not clamp to SINT_MAX. */
                  if (G_028C70_NUMBER_TYPE(dst_uav->color.info) == V_028C70_NUMBER_UINT) {
                     uint32_t const cb_info_before = dst_uav->color.info;
                     dst_uav->color.info =
                        (dst_uav->color.info & C_028C70_NUMBER_TYPE) |
                        S_028C70_NUMBER_TYPE(V_028C70_NUMBER_SINT);
                     if (trace_sd_cached) {
                        fprintf(stderr,
                                "terakan/stor_img_desc: FIX-Z part2 CB_NUMBER_TYPE "
                                "UINT->SINT info 0x%08x -> 0x%08x\n",
                                cb_info_before, dst_uav->color.info);
                     }
                  }
               }
               if (trace_sd_cached) {
                  fprintf(stderr,
                          "terakan/stor_img_desc: FINAL-STATE" " rr=%08x %08x %08x %08x" " %08x %08x %08x %08x" " info=%08x\n",
                          dst_uav->real_resource[0], dst_uav->real_resource[1],
                          dst_uav->real_resource[2], dst_uav->real_resource[3],
                          dst_uav->real_resource[4], dst_uav->real_resource[5],
                          dst_uav->real_resource[6], dst_uav->real_resource[7],
                          dst_uav->color.info);
               }
            } else {
               dst_uav->bo = NULL;
               dst_uav->buffer_byte_offset = 0;
               memset(dst_uav->real_resource, 0, sizeof(dst_uav->real_resource));
               dst_uav->base_array_layer = 0;
               dst_uav->view_flags = 0;
               memset(dst_uav->_pad_fix_k, 0, sizeof(dst_uav->_pad_fix_k));
            }
         }
      }
         FALLTHROUGH;
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
         if ((descriptor_write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
              descriptor_write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
             dst_binding->first_immutable_sampler_or_dynamic_offset == UINT16_MAX) {
            for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
                 ++descriptor_index) {
               terakan_descriptor_set_sampler_init(
                  &dst_samplers[descriptor_index],
                  terakan_sampler_from_handle(
                     descriptor_write->pImageInfo[descriptor_index].sampler));
            }
         }
         if (descriptor_write->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
            for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
                 ++descriptor_index) {
               struct terakan_descriptor_set_resource * const dst_resource =
                  &dst_resources[descriptor_index];
               struct terakan_image_view const * const image_view = terakan_image_view_from_handle(
                  descriptor_write->pImageInfo[descriptor_index].imageView);
               if (image_view != NULL &&
                   G_03001C_TYPE(image_view->resource[7]) == V_03001C_SQ_TEX_VTX_VALID_TEXTURE) {
                  dst_resource->bo = image_view->bo;
                  memcpy(dst_resource->resource, image_view->resource, sizeof(uint32_t) * 8);
               } else {
                  dst_resource->bo = NULL;
               }
            }
         }
      } break;

      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_descriptor_set_uav * const dst_uav = &dst_uavs[descriptor_index];
            struct terakan_buffer_view const * const buffer_view = terakan_buffer_view_from_handle(
               descriptor_write->pTexelBufferView[descriptor_index]);
            if (buffer_view != NULL &&
                G_028C70_FORMAT(buffer_view->color.info) != TERASCALE_FORMAT_INDEX_INVALID) {
               dst_uav->bo = buffer_view->bo;
               memcpy(&dst_uav->color, &buffer_view->color,
                      sizeof(struct terakan_color_descriptor));
               /* Texel buffer UAV: store the VIEW element count (not byte
                * size) for robustness write guards.  The image_deref_store
                * and image_deref_atomic paths compare element indices against
                * this value.  SSBOs use bytes; the two never share a UAV
                * slot, so the dual semantics are safe. */
               dst_uav->buffer_byte_size = (uint32_t)buffer_view->vk.elements;
               dst_uav->buffer_byte_offset = 0;  /* Texel-buffer view bakes offset into descriptor. */
               dst_uav->is_texel_buffer = 1;
            } else {
               dst_uav->bo = NULL;
               dst_uav->buffer_byte_size = 0;
               dst_uav->buffer_byte_offset = 0;
               dst_uav->is_texel_buffer = 1;
            }
            /* FIX-K: buffer UAVs never need baseArrayLayer injection. */
            dst_uav->base_array_layer = 0;
            dst_uav->view_flags = 0;
            memset(dst_uav->_pad_fix_k, 0, sizeof(dst_uav->_pad_fix_k));
         }
      }
         FALLTHROUGH;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_descriptor_set_resource * const dst_resource =
               &dst_resources[descriptor_index];
            struct terakan_buffer_view const * const buffer_view = terakan_buffer_view_from_handle(
               descriptor_write->pTexelBufferView[descriptor_index]);
            if (buffer_view != NULL &&
                G_03001C_TYPE(buffer_view->resource[7]) == V_03001C_SQ_TEX_VTX_VALID_BUFFER) {
               dst_resource->bo = buffer_view->bo;
               memcpy(dst_resource->resource, buffer_view->resource, sizeof(uint32_t) * 8);
               if (descriptor_write->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                  /* UNCACHED=1 is conservative (bypasses TC) but correct for writable bindings.
                   * Read-only storage texel buffers could use UNCACHED=0 for better bandwidth;
                   * requires pipeline-layout writability tracking -- same constraint as in
                   * terakan_descriptor_create_for_storage_buffer.
                   */
                  dst_resource->resource[3] |= S_03000C_UNCACHED(1);
               }
            } else {
               dst_resource->bo = NULL;
            }
         }
      } break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_descriptor_set_resource * const dst_resource =
               &dst_resources[descriptor_index];
            dst_resource->bo = terakan_buffer_create_uniform_buffer_descriptor(
               &descriptor_write->pBufferInfo[descriptor_index], dst_resource->resource);
         }
      } break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_descriptor_set_resource * const dst_resource =
               &dst_resources[descriptor_index];
            struct terakan_descriptor_set_uav * const dst_uav = &dst_uavs[descriptor_index];
            VkDescriptorBufferInfo const * const buf_info =
               &descriptor_write->pBufferInfo[descriptor_index];
            struct terakan_bo const * const bo = terakan_buffer_create_storage_buffer_descriptor(
               buf_info, dst_resource->resource, &dst_uav->color);
            dst_resource->bo = bo;
            dst_uav->bo = bo;
            dst_uav->is_texel_buffer = 0;
            /* Store exact Vulkan byte range for robustness metadata.
             * vk_buffer_range() resolves VK_WHOLE_SIZE. */
            if (bo != NULL) {
               struct terakan_buffer const * const buffer =
                  terakan_buffer_from_handle(buf_info->buffer);
               dst_uav->buffer_byte_size = (uint32_t)vk_buffer_range(
                  &buffer->vk, buf_info->offset, buf_info->range);
               /* Capture per-element offset for shared-BO array elements.
                * The radeon CS validator replaces SET_RESOURCE WORD0 with
                * bo->va_low, so this offset cannot reach the shader via
                * the descriptor; it travels via KCACHE bank 14 dwords
                * 52..63 (robustness_metadata.view_offsets[]) and is
                * added to byte_offset by
                * terakan_nir_lower_bindings_instr_load_ssbo. */
               dst_uav->buffer_byte_offset = (uint32_t)buf_info->offset;
            } else {
               dst_uav->buffer_byte_size = 0;
               dst_uav->buffer_byte_offset = 0;
            }
            /* FIX-K: SSBOs never need baseArrayLayer injection. */
            dst_uav->base_array_layer = 0;
            dst_uav->view_flags = 0;
            memset(dst_uav->_pad_fix_k, 0, sizeof(dst_uav->_pad_fix_k));
         }
      } break;

      default:
         assert(!"Unsupported descriptor type");
      }
   }

   for (uint32_t descriptor_copy_index = 0; descriptor_copy_index < descriptorCopyCount;
        ++descriptor_copy_index) {
      VkCopyDescriptorSet const * const descriptor_copy = &pDescriptorCopies[descriptor_copy_index];
      if (unlikely(descriptor_copy->descriptorCount == 0)) {
         /* There doesn't seem to be a VU rule about descriptorCount in VkCopyDescriptorSet being
          * nonzero, consider it valid, but don't bother trying to locate the first non-empty
          * binding.
          */
         continue;
      }

      /* VkCopyDescriptorSet's valid usage doesn't require that srcBinding and dstBinding themselves
       * are non-empty. However, empty bindings are completely skipped and not initialized by the
       * driver during descriptor set layout creation, so take the type and the offsets from the
       * first non-empty binding starting from srcBinding or dstBinding.
       */
      struct terakan_descriptor_set const * const src_set =
         terakan_descriptor_set_from_handle(descriptor_copy->srcSet);
      size_t src_binding_index = descriptor_copy->srcBinding;
      while (src_binding_index < src_set->layout->binding_count &&
             src_set->layout->bindings[src_binding_index].descriptor_count == 0) {
         ++src_binding_index;
      }
      assert(src_binding_index < src_set->layout->binding_count);
      if (unlikely(src_binding_index >= src_set->layout->binding_count)) {
         continue;
      }
      struct terakan_descriptor_set_layout_binding const * const src_binding =
         &src_set->layout->bindings[src_binding_index];

      VkDescriptorType const descriptor_type = src_binding->descriptor_type;

      struct terakan_descriptor_set const * const dst_set =
         terakan_descriptor_set_from_handle(descriptor_copy->dstSet);
      size_t dst_binding_index = descriptor_copy->dstBinding;
      while (dst_binding_index < dst_set->layout->binding_count &&
             dst_set->layout->bindings[dst_binding_index].descriptor_count == 0) {
         ++dst_binding_index;
      }
      assert(dst_binding_index < dst_set->layout->binding_count);
      if (unlikely(dst_binding_index >= dst_set->layout->binding_count)) {
         continue;
      }
      struct terakan_descriptor_set_layout_binding const * const dst_binding =
         &dst_set->layout->bindings[dst_binding_index];

      if (terakan_descriptor_type_has_resource(descriptor_type)) {
         assert(terakan_descriptor_type_has_resource(dst_binding->descriptor_type));
         memcpy(dst_set->descriptors +
                   sizeof(struct terakan_descriptor_set_resource) *
                      (dst_binding->first_set_resource + descriptor_copy->dstArrayElement),
                src_set->descriptors +
                   sizeof(struct terakan_descriptor_set_resource) *
                      (src_binding->first_set_resource + descriptor_copy->srcArrayElement),
                sizeof(struct terakan_descriptor_set_resource) * descriptor_copy->descriptorCount);

         if (terakan_descriptor_type_has_uav(descriptor_type)) {
            assert(terakan_descriptor_type_has_uav(dst_binding->descriptor_type));
            memcpy(dst_set->descriptors + dst_set->layout->pool_first_uav_offset_bytes +
                      sizeof(struct terakan_descriptor_set_uav) *
                         (dst_binding->first_set_uav + descriptor_copy->dstArrayElement),
                   src_set->descriptors + src_set->layout->pool_first_uav_offset_bytes +
                      sizeof(struct terakan_descriptor_set_uav) *
                         (src_binding->first_set_uav + descriptor_copy->srcArrayElement),
                   sizeof(struct terakan_descriptor_set_uav) * descriptor_copy->descriptorCount);
         }
      }

      if (terakan_descriptor_type_has_sampler(descriptor_type)) {
         assert(terakan_descriptor_type_has_sampler(dst_binding->descriptor_type));
         memcpy(dst_set->descriptors + dst_set->layout->pool_first_sampler_offset_bytes +
                   sizeof(struct terakan_descriptor_set_sampler) *
                      (dst_binding->first_set_sampler + descriptor_copy->dstArrayElement),
                src_set->descriptors + src_set->layout->pool_first_sampler_offset_bytes +
                   sizeof(struct terakan_descriptor_set_sampler) *
                      (src_binding->first_set_sampler + descriptor_copy->srcArrayElement),
                sizeof(struct terakan_descriptor_set_sampler) * descriptor_copy->descriptorCount);
      }
   }
}

static void
terakan_descriptor_set_free_descriptors_and_finish(struct terakan_descriptor_pool * const pool,
                                                   uint32_t const set_index)
{
   struct terakan_descriptor_set * const set = &pool->sets[set_index];

   uint32_t const descriptors_size = set->layout->pool_size_bytes;
   if (descriptors_size != 0) {
      assert(pool->descriptor_memory_size - pool->descriptor_memory_unallocated >=
             descriptors_size);
      pool->descriptor_memory_unallocated += descriptors_size;
      util_vma_heap_free(&pool->descriptor_memory_heap, (uint64_t)set->descriptors,
                         descriptors_size);
   }

   vk_descriptor_set_layout_unref(set->base.device, &set->layout->vk);

   vk_object_base_finish(&set->base);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_FreeDescriptorSets(UNUSED VkDevice const device, VkDescriptorPool const descriptorPool,
                           uint32_t const descriptorSetCount,
                           VkDescriptorSet const * const pDescriptorSets)
{
   struct terakan_descriptor_pool * const pool =
      terakan_descriptor_pool_from_handle(descriptorPool);

   pool->sets_allocated -= descriptorSetCount;

   for (uint32_t array_set_index = 0; array_set_index < descriptorSetCount; ++array_set_index) {
      struct terakan_descriptor_set * const set =
         terakan_descriptor_set_from_handle(pDescriptorSets[array_set_index]);

      uint32_t const set_index = set - pool->sets;
      assert(set_index < pool->max_sets);

      terakan_descriptor_set_free_descriptors_and_finish(pool, set_index);

      if (set->pool_allocated_prev != UINT32_MAX) {
         struct terakan_descriptor_set * const set_prev = &pool->sets[set->pool_allocated_prev];
         assert(set_prev->pool_allocated_or_freed_next == set_index);
         set_prev->pool_allocated_or_freed_next = set->pool_allocated_or_freed_next;
      } else {
         assert(pool->allocated_sets_head == set_index);
         pool->allocated_sets_head = set->pool_allocated_or_freed_next;
      }
      if (set->pool_allocated_or_freed_next != UINT32_MAX) {
         struct terakan_descriptor_set * const set_next =
            &pool->sets[set->pool_allocated_or_freed_next];
         assert(set_next->pool_allocated_prev == set_index);
         set_next->pool_allocated_prev = set->pool_allocated_prev;
      }

      set->pool_allocated_or_freed_next = pool->freed_sets_head;
      pool->freed_sets_head = set_index;
   }

   pool->sets_freed += descriptorSetCount;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_AllocateDescriptorSets(VkDevice const deviceHandle,
                               VkDescriptorSetAllocateInfo const * const pAllocateInfo,
                               VkDescriptorSet * const pDescriptorSets)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_descriptor_pool * const pool =
      terakan_descriptor_pool_from_handle(pAllocateInfo->descriptorPool);
   uint32_t const set_count = pAllocateInfo->descriptorSetCount;

   if (pool->max_sets - pool->sets_allocated < set_count) {
      for (uint32_t null_set_index = 0; null_set_index < set_count; ++null_set_index) {
         pDescriptorSets[null_set_index] = VK_NULL_HANDLE;
      }
      return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
   }

   for (uint32_t array_set_index = 0; array_set_index < set_count; ++array_set_index) {
      struct terakan_descriptor_set_layout * const set_layout =
         terakan_descriptor_set_layout_from_handle(pAllocateInfo->pSetLayouts[array_set_index]);
      size_t const set_size = set_layout->pool_size_bytes;

      char * set_descriptors = NULL;
      if (set_size != 0) {
         VkResult set_descriptors_allocate_error = VK_ERROR_OUT_OF_POOL_MEMORY;
         /* Distinguish between trying to allocate too much memory and fragmentation. */
         if (pool->descriptor_memory_unallocated >= set_size) {
            set_descriptors_allocate_error = VK_ERROR_FRAGMENTED_POOL;
            set_descriptors =
               (char *)util_vma_heap_alloc(&pool->descriptor_memory_heap, set_size,
                                           TERAKAN_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT);
         }
         if (set_descriptors == NULL) {
            terakan_FreeDescriptorSets(deviceHandle, pAllocateInfo->descriptorPool, array_set_index,
                                       pDescriptorSets);
            for (uint32_t null_set_index = 0; null_set_index < set_count; ++null_set_index) {
               pDescriptorSets[null_set_index] = VK_NULL_HANDLE;
            }
            return vk_error(device, set_descriptors_allocate_error);
         }
         assert(pool->descriptor_memory_unallocated >= set_size);
         pool->descriptor_memory_unallocated -= set_size;
      }

      uint32_t set_index = pool->freed_sets_head;
      if (set_index != UINT32_MAX) {
         assert(pool->sets_freed != 0);
         --pool->sets_freed;
         pool->freed_sets_head = pool->sets[set_index].pool_allocated_or_freed_next;
      } else {
         set_index = pool->sets_allocated + pool->sets_freed;
         assert(set_index < pool->max_sets);
      }
      ++pool->sets_allocated;
      struct terakan_descriptor_set * const set = &pool->sets[set_index];

      set->pool_allocated_prev = UINT32_MAX;
      set->pool_allocated_or_freed_next = pool->allocated_sets_head;
      if (pool->allocated_sets_head != UINT32_MAX) {
         struct terakan_descriptor_set * const allocated_head_set =
            &pool->sets[pool->allocated_sets_head];
         assert(allocated_head_set->pool_allocated_prev == UINT32_MAX);
         allocated_head_set->pool_allocated_prev = set_index;
      }
      pool->allocated_sets_head = set_index;

      vk_object_base_init(&device->vk, &set->base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

      vk_descriptor_set_layout_ref(&set_layout->vk);
      set->layout = set_layout;

      set->descriptors = set_descriptors;

      if (set_size != 0) {
         /* Section 14.2.3. "Allocation of Descriptor Sets" of the Vulkan 1.3.275 specification
          * says:
          *
          *     "Entries that are not used by a pipeline can have undefined descriptors."
          *
          * Make sure hardware binding setters can work with potentially outdated, but never with
          * completely invalid data with potentially broken invariants. Initialize BO pointers to
          * NULL, and samplers to TYPE = 0, border color unused, and unnormalized coordinates
          * disabled.
          */
         memset(set->descriptors, 0, set_size);

         /* Write immutable samplers. */
         struct terakan_descriptor_set_sampler * const set_samplers =
            (struct terakan_descriptor_set_sampler *)(set->descriptors +
                                                      set_layout->pool_first_sampler_offset_bytes);
         for (uint8_t immutable_sampler_index = 0;
              immutable_sampler_index < set_layout->immutable_sampler_count;
              ++immutable_sampler_index) {
            terakan_descriptor_set_sampler_init(
               &set_samplers[set_layout->immutable_sampler_indices_in_set[immutable_sampler_index]],
               set_layout->immutable_samplers[immutable_sampler_index]);
         }
      }

      pDescriptorSets[array_set_index] = terakan_descriptor_set_to_handle(set);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_ATTR
terakan_ResetDescriptorPool(UNUSED VkDevice const device, VkDescriptorPool const descriptorPool,
                            UNUSED VkDescriptorPoolResetFlags const flags)
{
   struct terakan_descriptor_pool * const pool =
      terakan_descriptor_pool_from_handle(descriptorPool);

   for (uint32_t set_index = pool->allocated_sets_head; set_index != UINT32_MAX;
        set_index = pool->sets[set_index].pool_allocated_or_freed_next) {
      terakan_descriptor_set_free_descriptors_and_finish(pool, set_index);
   }

   pool->sets_allocated = 0;
   pool->allocated_sets_head = UINT32_MAX;

   pool->sets_freed = 0;
   pool->freed_sets_head = UINT32_MAX;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyDescriptorPool(VkDevice const deviceHandle, VkDescriptorPool const descriptorPool,
                              VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_descriptor_pool * const pool =
      terakan_descriptor_pool_from_handle(descriptorPool);

   if (pool == NULL) {
      return;
   }

   for (uint32_t set_index = pool->allocated_sets_head; set_index != UINT32_MAX;
        set_index = pool->sets[set_index].pool_allocated_or_freed_next) {
      terakan_descriptor_set_free_descriptors_and_finish(pool, set_index);
   }

   if (pool->descriptor_memory_size != 0) {
      util_vma_heap_finish(&pool->descriptor_memory_heap);
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_object_base_finish(&pool->base);

   vk_free2(&device->vk.alloc, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateDescriptorPool(VkDevice const deviceHandle,
                             VkDescriptorPoolCreateInfo const * const pCreateInfo,
                             VkAllocationCallbacks const * const pAllocator,
                             VkDescriptorPool * const pDescriptorPool)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   size_t resource_count = 0, sampler_count = 0, uav_count = 0;
   for (uint32_t pool_size_index = 0; pool_size_index < pCreateInfo->poolSizeCount;
        ++pool_size_index) {
      VkDescriptorPoolSize const pool_size = pCreateInfo->pPoolSizes[pool_size_index];
      if (terakan_descriptor_type_has_resource(pool_size.type)) {
         resource_count += pool_size.descriptorCount;
         if (terakan_descriptor_type_has_uav(pool_size.type)) {
            uav_count += pool_size.descriptorCount;
         }
      }
      if (terakan_descriptor_type_has_sampler(pool_size.type)) {
         sampler_count += pool_size.descriptorCount;
      }
   }
   size_t const descriptor_memory_size =
      sizeof(struct terakan_descriptor_set_resource) * resource_count +
      sizeof(struct terakan_descriptor_set_sampler) * sampler_count +
      sizeof(struct terakan_descriptor_set_uav) * uav_count;

   VK_MULTIALLOC(multialloc);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_descriptor_pool, pool, 1);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_descriptor_set, sets, pCreateInfo->maxSets);
   void * descriptor_memory;
   vk_multialloc_add_size_align(&multialloc, &descriptor_memory, descriptor_memory_size,
                                TERAKAN_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT);
   if (vk_multialloc_alloc2(&multialloc, &device->vk.alloc, pAllocator,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_DESCRIPTOR_POOL);

   pool->descriptor_memory_size = descriptor_memory_size;
   pool->descriptor_memory = descriptor_memory;
   if (descriptor_memory_size != 0) {
      static_assert(
         sizeof(descriptor_memory) <= sizeof(uint64_t),
         "Using VMA directly for CPU pointers, expecting offsets in it to be large enough to store "
         "one.");
      /* Use the descriptor memory pointer directly as the start because VMA expects the start
       * address to be nonzero since it uses 0 to report allocation errors.
       */
      util_vma_heap_init(&pool->descriptor_memory_heap, (uint64_t)descriptor_memory,
                         descriptor_memory_size);
   }
   pool->descriptor_memory_unallocated = descriptor_memory_size;

   pool->sets = sets;
   pool->max_sets = pCreateInfo->maxSets;

   pool->sets_allocated = 0;
   pool->allocated_sets_head = UINT32_MAX;

   pool->sets_freed = 0;
   pool->freed_sets_head = UINT32_MAX;

   *pDescriptorPool = terakan_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

/* VK_KHR_descriptor_update_template / Vulkan 1.1 core.
 *
 * Implementation strategy: walk the vk_descriptor_update_template entries,
 * synthesize a per-entry VkWriteDescriptorSet referring back into the
 * user-supplied data buffer at each entry's offset/stride, then dispatch
 * to terakan_UpdateDescriptorSets which already implements the full
 * descriptor-write logic for every supported descriptor type.
 *
 * Create/Destroy of the template object are handled by the generic
 * vk_common_* runtime helpers wired through the dispatch table; this
 * driver only needs to implement the Update path.
 */
VKAPI_ATTR void VKAPI_CALL
terakan_UpdateDescriptorSetWithTemplate(VkDevice const deviceHandle,
                                        VkDescriptorSet const descriptorSetHandle,
                                        VkDescriptorUpdateTemplate const templateHandle,
                                        void const * const pData)
{
   VK_FROM_HANDLE(vk_descriptor_update_template, template, templateHandle);

   /* Each template entry becomes one VkWriteDescriptorSet that points at a
    * slice of `pData` at `entry->offset + i * entry->stride` per descriptor.
    * Build the per-entry pointer arrays on the heap (template entry count
    * is bounded by the layout's binding count, normally a handful). */
   uint32_t const entry_count = template->entry_count;
   if (entry_count == 0) {
      return;
   }

   VkWriteDescriptorSet * const writes =
      calloc(entry_count, sizeof(VkWriteDescriptorSet));
   if (writes == NULL) {
      return;
   }

   /* Worst case: every descriptor in every entry needs its own
    * VkDescriptorImageInfo / VkDescriptorBufferInfo / VkBufferView entry.
    * Sum the array_count fields up front and allocate one combined info
    * buffer that holds any of the three union variants per descriptor. */
   uint32_t total_descriptors = 0;
   for (uint32_t i = 0; i < entry_count; ++i) {
      total_descriptors += template->entries[i].array_count;
   }

   VkDescriptorImageInfo * const image_infos =
      total_descriptors == 0 ? NULL : calloc(total_descriptors,
                                             sizeof(VkDescriptorImageInfo));
   VkDescriptorBufferInfo * const buffer_infos =
      total_descriptors == 0 ? NULL : calloc(total_descriptors,
                                             sizeof(VkDescriptorBufferInfo));
   VkBufferView * const texel_views =
      total_descriptors == 0 ? NULL : calloc(total_descriptors,
                                             sizeof(VkBufferView));
   if (total_descriptors != 0 &&
       (image_infos == NULL || buffer_infos == NULL || texel_views == NULL)) {
      free(writes);
      free(image_infos);
      free(buffer_infos);
      free(texel_views);
      return;
   }

   uint32_t next_info_slot = 0;
   uint8_t const * const data = (uint8_t const *)pData;
   for (uint32_t i = 0; i < entry_count; ++i) {
      struct vk_descriptor_template_entry const * const entry =
         &template->entries[i];

      VkWriteDescriptorSet * const w = &writes[i];
      w->sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w->pNext           = NULL;
      w->dstSet          = descriptorSetHandle;
      w->dstBinding      = entry->binding;
      w->dstArrayElement = entry->array_element;
      w->descriptorCount = entry->array_count;
      w->descriptorType  = entry->type;

      /* Populate the appropriate pImageInfo / pBufferInfo / pTexelBufferView
       * pointer based on descriptor type.  Each per-descriptor info struct
       * lives in the combined buffer at `next_info_slot`. */
      uint32_t const slot_base = next_info_slot;
      next_info_slot += entry->array_count;

      switch (entry->type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (uint32_t j = 0; j < entry->array_count; ++j) {
            image_infos[slot_base + j] = *(VkDescriptorImageInfo const *)(
               data + entry->offset + (size_t)j * entry->stride);
         }
         w->pImageInfo = &image_infos[slot_base];
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < entry->array_count; ++j) {
            texel_views[slot_base + j] = *(VkBufferView const *)(
               data + entry->offset + (size_t)j * entry->stride);
         }
         w->pTexelBufferView = &texel_views[slot_base];
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < entry->array_count; ++j) {
            buffer_infos[slot_base + j] = *(VkDescriptorBufferInfo const *)(
               data + entry->offset + (size_t)j * entry->stride);
         }
         w->pBufferInfo = &buffer_infos[slot_base];
         break;
      default:
         /* Unsupported descriptor type for templates -- skip the entry by
          * leaving descriptorCount zero so UpdateDescriptorSets ignores it. */
         w->descriptorCount = 0;
         break;
      }
   }

   terakan_UpdateDescriptorSets(deviceHandle, entry_count, writes, 0, NULL);

   free(writes);
   free(image_infos);
   free(buffer_infos);
   free(texel_views);
}
