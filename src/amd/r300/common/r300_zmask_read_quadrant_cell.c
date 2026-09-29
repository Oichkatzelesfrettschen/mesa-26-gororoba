/* SPDX-License-Identifier: MIT */

#include "r300_zmask_read_quadrant_cell.h"

#include "r300_chipset.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_zb_depth_state.h"
#include "r300_zmask_clear_plan.h"
#include "r300_zmask_materialize_plan.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot. */
#define QUADRANT_RELOC_PAYLOAD(slot) ((slot) * 4u)

/* Identity PSC swizzle select, the per-word value the kernel's
 * TCL-bypass vertex-output check requires on every
 * VAP_PROG_STREAM_CNTL_EXT word. */
#define QUADRANT_PSC_EXT_IDENTITY 0xF688F688u

#define QUADRANT_MAX_VTX_INDEX (R300_ZMASK_READ_QUADRANT_VERTEX_COUNT - 1u)

#define FP24_ONE 0x003f0000u

#define ZCACHE_FLUSH_AND_FREE                                                \
   (R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |                         \
    R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE)

const struct r300_zmask_read_quadrant_draw
   r300_zmask_read_quadrant_draws[R300_ZMASK_READ_QUADRANT_COUNT] = {
      [R300_ZMASK_READ_QUADRANT_A] = {
         .name = "A", .origin_x = 0u, .origin_y = 0u,
         .zb_bw_cntl = 0u, .wrapped = false,
         .z = 0.0625f, .depth_code = 0x100000u,
         .constant = { FP24_ONE, 0u, 0u, FP24_ONE },
         .color = 0xffff0000u,
      },
      [R300_ZMASK_READ_QUADRANT_B] = {
         .name = "B", .origin_x = 32u, .origin_y = 0u,
         .zb_bw_cntl = R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE |
                       R300_WR_COMP_ENABLE,
         .wrapped = true,
         .z = 0.25f, .depth_code = 0x400000u,
         .constant = { 0u, FP24_ONE, 0u, FP24_ONE },
         .color = 0xff00ff00u,
      },
      [R300_ZMASK_READ_QUADRANT_C] = {
         .name = "C", .origin_x = 0u, .origin_y = 32u,
         .zb_bw_cntl = R300_FAST_FILL_ENABLE, .wrapped = true,
         .z = 0.25f, .depth_code = 0x400000u,
         .constant = { 0u, 0u, FP24_ONE, FP24_ONE },
         .color = 0xff0000ffu,
      },
      [R300_ZMASK_READ_QUADRANT_D] = {
         .name = "D", .origin_x = 32u, .origin_y = 32u,
         .zb_bw_cntl = R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE,
         .wrapped = true,
         .z = 0.25f, .depth_code = 0x400000u,
         .constant = { FP24_ONE, FP24_ONE, 0u, FP24_ONE },
         .color = 0xffffff00u,
      },
   };

#define QUADRANT_VERTICES(zv)                                                \
   {                                                                         \
       0.0f,  0.0f, (zv), 1.0f,                                              \
      64.0f,  0.0f, (zv), 1.0f,                                              \
       0.0f, 64.0f, (zv), 1.0f,                                              \
      64.0f,  0.0f, (zv), 1.0f,                                              \
      64.0f, 64.0f, (zv), 1.0f,                                              \
       0.0f, 64.0f, (zv), 1.0f,                                              \
   }

const float r300_zmask_read_quadrant_vertices
   [R300_ZMASK_READ_QUADRANT_COUNT][R300_ZMASK_READ_QUADRANT_VERTEX_COUNT * 4] = {
      QUADRANT_VERTICES(0.0625f),
      QUADRANT_VERTICES(0.25f),
      QUADRANT_VERTICES(0.25f),
      QUADRANT_VERTICES(0.25f),
   };

int
r300_zmask_read_quadrant_surface(
   const struct r300_zb_depth_surface **surface_out,
   struct r300_zb_depth_layout *depth_layout_out,
   struct r300_zmask_layout *zmask_layout_out)
{
   if (surface_out == NULL || depth_layout_out == NULL ||
       zmask_layout_out == NULL)
      return -EINVAL;
   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   struct r300_zb_depth_layout layout;
   if (r300_zb_depth_layout_compute(
          surface, R300_ZMASK_READ_QUADRANT_DEPTH_GUARD_BYTES, &layout) != 0)
      return -EINVAL;

   /* The parameters r3v_native_image.c resolves for a 64x64 D24S8 image,
    * at the level's own block, so the clear's coverage and the plane
    * equations every wrapped draw programs are the lifecycle's. */
   const struct r300_zmask_layout_params params = {
      .stride_in_pixels = layout.pitch_pixels,
      .height = surface->height,
      .depth_bytes_per_pixel = layout.bytes_per_pixel,
      .is_depth_or_stencil = true,
      .microtile = layout.microtile_width != 0u,
      .macrotile = layout.macrotile_width != 0u,
      .num_samples = 1u,
      .zcomp8x8_capable = r300_zmask_zcomp8x8_capable(CHIP_RS480),
      .pipes = 1u,
      .zmask_ram_dwords_per_pipe = r300_zmask_ram_dwords_per_pipe(CHIP_RS480),
   };
   struct r300_zmask_layout zmask;
   if (r300_zmask_layout_compute_at_block(&params, R300_ZCOMP_8X8, &zmask) !=
          0 ||
       !zmask.fits_zmask_ram || !zmask.zcomp8x8)
      return -EINVAL;

   *surface_out = surface;
   *depth_layout_out = layout;
   *zmask_layout_out = zmask;
   return 0;
}

uint64_t
r300_zmask_read_quadrant_depth_bytes(void)
{
   const struct r300_zb_depth_surface *surface;
   struct r300_zb_depth_layout layout;
   struct r300_zmask_layout zmask;
   if (r300_zmask_read_quadrant_surface(&surface, &layout, &zmask) != 0)
      return 0u;
   return layout.total_bytes;
}

static void
write_reloc(struct r300_pm4_builder *b, struct r300_zmask_read_quadrant_ib *out,
            uint32_t slot)
{
   if (b->error != 0)
      return;
   if (slot >= R300_ZMASK_READ_QUADRANT_SLOT_COUNT ||
       out->reloc_site_count >= R300_ZMASK_READ_QUADRANT_MAX_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }
   const uint32_t index = r300_pm4_reloc_nop(b, QUADRANT_RELOC_PAYLOAD(slot));
   if (index == R300_PM4_NO_INDEX)
      return;
   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_zmask_read_quadrant_reloc_site){ .ib_index = index,
                                                    .slot = slot };
}

static void
record_site(struct r300_pm4_builder *b, struct r300_zmask_read_quadrant_ib *out,
            uint32_t index, uint32_t slot)
{
   if (b->error != 0 || index == R300_PM4_NO_INDEX)
      return;
   if (out->reloc_site_count >= R300_ZMASK_READ_QUADRANT_MAX_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }
   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_zmask_read_quadrant_reloc_site){ .ib_index = index,
                                                    .slot = slot };
}

/* The dependency r3v_native_append_global_dependency records around a
 * fast clear, less the 2D cache this cell never touches: depth and color
 * caches flushed and freed, then the CP waits for the 3D engine. */
static void
emit_dependency(struct r300_pm4_builder *b)
{
   r300_pm4_reg(b, R300_ZB_ZCACHE_CTLSTAT, ZCACHE_FLUSH_AND_FREE);
   r300_pm4_reg(b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(b, RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
}

static void
emit_scissor(struct r300_pm4_builder *b,
             const struct r300_zmask_read_quadrant_draw *draw)
{
   uint32_t tl = 0u;
   uint32_t br = 0u;
   /* SC_SCISSORS_BR is inclusive. */
   if (r300_first_draw_scissor_word(draw->origin_x, draw->origin_y, &tl) !=
          0 ||
       r300_first_draw_scissor_word(
          draw->origin_x + R300_ZMASK_READ_QUADRANT_EXTENT - 1u,
          draw->origin_y + R300_ZMASK_READ_QUADRANT_EXTENT - 1u, &br) != 0) {
      if (b->error == 0)
         b->error = -EINVAL;
      return;
   }
   r300_pm4_reg(b, R300_SC_SCISSORS_TL, tl);
   r300_pm4_reg(b, R300_SC_SCISSORS_BR, br);
}

static void
emit_quadrant_draw(struct r300_pm4_builder *b,
                   struct r300_zmask_read_quadrant_ib *out, uint32_t quadrant)
{
   const uint32_t vbpntr[3] = {
      1u | R300_VC_FORCE_PREFETCH,
      R300_VBPNTR_SIZE0(R300_ZMASK_READ_QUADRANT_VERTEX_STRIDE_BYTES) |
         R300_VBPNTR_STRIDE0(R300_ZMASK_READ_QUADRANT_VERTEX_STRIDE_BYTES),
      quadrant * R300_ZMASK_READ_QUADRANT_VERTEX_SET_BYTES,
   };
   r300_pm4_packet3(b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr,
                    ARRAY_SIZE(vbpntr));
   write_reloc(b, out, R300_ZMASK_READ_QUADRANT_SLOT_VERTEX);

   const uint32_t draw = R300_VAP_VF_CNTL__PRIM_TRIANGLES |
                         R300_PRIM_WALK_LIST |
                         (R300_ZMASK_READ_QUADRANT_VERTEX_COUNT
                          << R300_PRIM_NUM_VERTICES_SHIFT);
   r300_pm4_packet3(b, R300_PACKET3_3D_DRAW_VBUF_2, &draw, 1);
}

static int
emit_into(const struct r300_zmask_read_quadrant_params *params,
          uint32_t *words, uint32_t capacity,
          struct r300_zmask_read_quadrant_ib *out)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;
   if (fs == NULL || !fs->validated || params->first_draw_contract == NULL ||
       params->surface == NULL || params->zmask_layout == NULL)
      return -EINVAL;
   const struct r300_zb_depth_surface *surface = params->surface;
   if (surface->width != R300_ZMASK_READ_QUADRANT_TARGET_WIDTH ||
       surface->height != R300_ZMASK_READ_QUADRANT_TARGET_HEIGHT)
      return -EINVAL;

   struct r300_zmask_clear_plan clear;
   if (r300_zmask_fast_clear_plan_build(
          surface, params->zmask_layout,
          R300_ZMASK_READ_QUADRANT_CLEAR_DEPTH_CODE,
          R300_ZMASK_READ_QUADRANT_CLEAR_STENCIL, &clear) != 0)
      return -EINVAL;

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, capacity);

   const uint32_t state_dwords =
      r300_first_draw_state_dwords(params->first_draw_contract);
   if (r300_pm4_builder_reserve(&b, state_dwords)) {
      const int emitted = r300_first_draw_state_emit(
         params->first_draw_contract, &b.words[b.count], state_dwords);
      if (emitted < 0)
         b.error = emitted;
      else if ((uint32_t)emitted != state_dwords)
         b.error = -EINVAL;
      else
         b.count += state_dwords;
   }

   r300_pm4_reg(&b, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   r300_pm4_reg(&b, R300_VAP_PROG_STREAM_CNTL_0,
                R300_DATA_TYPE_FLOAT_4 | (0 << R300_DST_VEC_LOC_SHIFT) |
                   R300_LAST_VEC);
   static const uint32_t psc_ext_identity[8] = {
      QUADRANT_PSC_EXT_IDENTITY, QUADRANT_PSC_EXT_IDENTITY,
      QUADRANT_PSC_EXT_IDENTITY, QUADRANT_PSC_EXT_IDENTITY,
      QUADRANT_PSC_EXT_IDENTITY, QUADRANT_PSC_EXT_IDENTITY,
      QUADRANT_PSC_EXT_IDENTITY, QUADRANT_PSC_EXT_IDENTITY,
   };
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_EXT_0, psc_ext_identity,
                    ARRAY_SIZE(psc_ext_identity));
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_0,
                R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_1, 0);
   r300_pm4_reg(&b, R300_VAP_VTX_SIZE, 4);

   r300_pm4_block(&b, fs->cb_code, fs->cb_code_size);
   r300_pm4_reg(&b, R300_FG_DEPTH_SRC, fs->fg_depth_src);
   r300_pm4_reg(&b, R300_US_W_FMT, fs->us_out_w);

   r300_pm4_reg(&b, R300_RB3D_CCTL, 0);
   r300_pm4_reg(&b, R300_RB3D_COLOROFFSET0, 0);
   write_reloc(&b, out, R300_ZMASK_READ_QUADRANT_SLOT_COLOR);
   r300_pm4_reg(&b, R300_RB3D_COLORPITCH0, params->color_pitch_format);

   /* Test on, LESS, no depth write: ZB_CNTL 0x2 and ZB_ZSTENCILCNTL 0x1,
    * the pair the lifecycle's read draw executed under. */
   const struct r300_zb_depth_state_params depth = {
      .pitch_pixels = surface->pitch_pixels,
      .depth_format = surface->depth_format,
      .pitch_tile_bits = r300_zb_depth_surface_tile_bits(surface),
      .depth_offset_bytes = params->depth_offset_bytes,
      .depth_relocation_payload =
         QUADRANT_RELOC_PAYLOAD(R300_ZMASK_READ_QUADRANT_SLOT_DEPTH),
      .depth_function = R300_ZS_LESS,
      .depth_write = false,
   };
   uint32_t depth_reloc_index = R300_PM4_NO_INDEX;
   const int depth_rc = r300_zb_depth_state_emit(&b, &depth, &depth_reloc_index);
   if (depth_rc != 0 && b.error == 0)
      b.error = depth_rc;
   record_site(&b, out, depth_reloc_index, R300_ZMASK_READ_QUADRANT_SLOT_DEPTH);

   /* The fast clear inside the dependency the lifecycle records around
    * it, so every draw reads metadata the clear has retired. */
   emit_dependency(&b);
   r300_pm4_block(&b, clear.words, clear.dword_count);
   emit_dependency(&b);

   /* A reads under the compression-disabled state: the suffix words
    * r300_zmask_materialize_suffix appends, which also free every Z-cache
    * line the clear touched. */
   r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT, ZCACHE_FLUSH_AND_FREE);
   r300_pm4_reg(&b, R300_ZB_BW_CNTL, 0u);
   r300_pm4_reg(&b, R300_GB_Z_PEQ_CONFIG, R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);

   for (uint32_t q = 0; q < R300_ZMASK_READ_QUADRANT_COUNT; q++) {
      const struct r300_zmask_read_quadrant_draw *draw =
         &r300_zmask_read_quadrant_draws[q];
      emit_scissor(&b, draw);
      r300_pm4_packet0(&b, R300_PFS_PARAM_0_X, draw->constant, 4);

      if (!draw->wrapped) {
         emit_quadrant_draw(&b, out, q);
         r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT, ZCACHE_FLUSH_AND_FREE);
         continue;
      }

      struct r300_zmask_materialize_plan plan;
      if (r300_zmask_read_prefix_at_group(
             surface, params->zmask_layout,
             R300_ZMASK_READ_QUADRANT_CLEAR_DEPTH_CODE,
             R300_ZMASK_READ_QUADRANT_CLEAR_STENCIL, draw->zb_bw_cntl,
             &plan) != 0 ||
          r300_zmask_materialize_suffix(&plan) != 0) {
         if (b.error == 0)
            b.error = -EINVAL;
         break;
      }
      r300_pm4_block(&b, plan.words, plan.begin_dword_count);
      emit_quadrant_draw(&b, out, q);
      r300_pm4_block(&b, r300_zmask_materialize_end_words(&plan),
                     r300_zmask_materialize_end_dword_count(&plan));
   }

   /* Both caches publish before the host reads the color quadrants and
    * the depth allocation. */
   emit_dependency(&b);

   const int rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (rc != 0) {
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   return 0;
}

int
r300_zmask_read_quadrant_emit(
   const struct r300_zmask_read_quadrant_params *params,
   struct r300_zmask_read_quadrant_ib *out)
{
   if (out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   if (params == NULL)
      return -EINVAL;
   uint32_t *words =
      calloc(R300_ZMASK_READ_QUADRANT_MAX_DWORDS, sizeof(*words));
   if (words == NULL)
      return -ENOMEM;
   const int rc =
      emit_into(params, words, R300_ZMASK_READ_QUADRANT_MAX_DWORDS, out);
   if (rc != 0) {
      free(words);
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->owns_ib = true;
   return 0;
}

void
r300_zmask_read_quadrant_release(struct r300_zmask_read_quadrant_ib *ib)
{
   if (ib == NULL)
      return;
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_zmask_read_quadrant_reference_emit(
   struct r300_zmask_read_quadrant_ib *out)
{
   if (out == NULL)
      return -EINVAL;
   const struct r300_zb_depth_surface *surface;
   struct r300_zb_depth_layout layout;
   struct r300_zmask_layout zmask;
   int rc = r300_zmask_read_quadrant_surface(&surface, &layout, &zmask);
   if (rc != 0)
      return rc;
   if (layout.base_offset_bytes > UINT32_MAX)
      return -EINVAL;

   struct r300_fragment_binary fs;
   rc = r300_tcl_bypass_triangle_reference_fs(&fs);
   if (rc != 0)
      return rc;

   const struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = R300_ZMASK_READ_QUADRANT_TARGET_WIDTH,
      .height = R300_ZMASK_READ_QUADRANT_TARGET_HEIGHT,
      .min_vtx_index = 0,
      .max_vtx_index = QUADRANT_MAX_VTX_INDEX,
      .texture_enabled = false,
      .multisample = false,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc == 0)
      rc = r300_first_draw_contract_set_us_out_fmt_0(
         &contract, R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
                       R300_C2_SEL_R | R300_C3_SEL_A);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }

   const struct r300_zmask_read_quadrant_params params = {
      .surface = surface,
      .zmask_layout = &zmask,
      .depth_offset_bytes = (uint32_t)layout.base_offset_bytes,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(
         R300_ZMASK_READ_QUADRANT_PITCH_PIXELS),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   rc = r300_zmask_read_quadrant_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_zmask_read_quadrant_validate_reloc_sites(
   const struct r300_zmask_read_quadrant_ib *ib)
{
   if (ib == NULL || ib->ib == NULL ||
       ib->reloc_site_count != R300_ZMASK_READ_QUADRANT_MAX_RELOC_SITES)
      return -EINVAL;
   uint32_t counts[R300_ZMASK_READ_QUADRANT_SLOT_COUNT] = {0};
   uint32_t previous = 0;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_zmask_read_quadrant_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot >= R300_ZMASK_READ_QUADRANT_SLOT_COUNT ||
          site->ib_index >= ib->ib_size_dwords ||
          (i > 0 && site->ib_index <= previous) ||
          ib->ib[site->ib_index] != QUADRANT_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
      previous = site->ib_index;
      counts[site->slot]++;
   }
   return counts[R300_ZMASK_READ_QUADRANT_SLOT_COLOR] == 1u &&
                counts[R300_ZMASK_READ_QUADRANT_SLOT_DEPTH] == 1u &&
                counts[R300_ZMASK_READ_QUADRANT_SLOT_VERTEX] ==
                   R300_ZMASK_READ_QUADRANT_COUNT
             ? 0
             : -EINVAL;
}

struct walk_registers {
   uint32_t zb_bw_cntl;
   uint32_t gb_z_peq_config;
   uint32_t zb_cntl;
   uint32_t zb_zstencilcntl;
   uint32_t zb_depthclearvalue;
   uint32_t zb_depthoffset;
   uint32_t zb_depthpitch;
   uint32_t zb_zmask_pitch;
   uint32_t sc_scissors_tl;
   uint32_t sc_scissors_br;
   uint32_t pfs_param_0[4];
   uint32_t vertex_offset;
   uint32_t zcache_flushes;
};

static void
apply_write(struct walk_registers *r, uint32_t reg, uint32_t value)
{
   switch (reg) {
   case R300_ZB_BW_CNTL: r->zb_bw_cntl = value; break;
   case R300_GB_Z_PEQ_CONFIG: r->gb_z_peq_config = value; break;
   case R300_ZB_CNTL: r->zb_cntl = value; break;
   case R300_ZB_ZSTENCILCNTL: r->zb_zstencilcntl = value; break;
   case R300_ZB_DEPTHCLEARVALUE: r->zb_depthclearvalue = value; break;
   case R300_ZB_DEPTHOFFSET: r->zb_depthoffset = value; break;
   case R300_ZB_DEPTHPITCH: r->zb_depthpitch = value; break;
   case R300_ZB_ZMASK_PITCH: r->zb_zmask_pitch = value; break;
   case R300_SC_SCISSORS_TL: r->sc_scissors_tl = value; break;
   case R300_SC_SCISSORS_BR: r->sc_scissors_br = value; break;
   case R300_ZB_ZCACHE_CTLSTAT:
      if (value == ZCACHE_FLUSH_AND_FREE)
         r->zcache_flushes++;
      break;
   default:
      if (reg >= R300_PFS_PARAM_0_X && reg < R300_PFS_PARAM_0_X + 16u)
         r->pfs_param_0[(reg - R300_PFS_PARAM_0_X) / 4u] = value;
      break;
   }
}

int
r300_zmask_read_quadrant_read_state(
   const uint32_t *ib, uint32_t dwords,
   struct r300_zmask_read_quadrant_stream_state *out)
{
   if (out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   if (ib == NULL)
      return -EINVAL;

   struct walk_registers r;
   memset(&r, 0, sizeof(r));
   for (uint32_t i = 0; i < dwords;) {
      const uint32_t header = ib[i];
      const uint32_t type = header >> 30;
      if (type == 1u)
         return -EINVAL;
      if (type == 2u) {
         i += 1u;
         continue;
      }
      const uint32_t count = ((header >> 16) & 0x3fffu) + 1u;
      if (count > dwords - 1u - i)
         return -EINVAL;
      if (type == 0u) {
         const uint32_t base = (header & 0x1fffu) << 2;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0u;
         for (uint32_t j = 0; j < count; j++)
            apply_write(&r, one_reg ? base : base + 4u * j, ib[i + 1u + j]);
      } else {
         const uint32_t opcode = header & 0xff00u;
         if (opcode == R300_PACKET3_3D_CLEAR_ZMASK) {
            out->zmask_clears++;
            for (uint32_t j = 0; j < count && j < 3u; j++)
               out->zmask_clear_payload[j] = ib[i + 1u + j];
            r.zcache_flushes = 0u;
         } else if (opcode == R300_PACKET3_3D_LOAD_VBPNTR && count >= 3u) {
            r.vertex_offset = ib[i + 3u];
         } else if (r300_first_draw_is_draw_packet(header)) {
            if (out->draw_count >= R300_ZMASK_READ_QUADRANT_COUNT)
               return -EINVAL;
            out->draws[out->draw_count++] =
               (struct r300_zmask_read_quadrant_draw_state){
                  .zb_bw_cntl = r.zb_bw_cntl,
                  .gb_z_peq_config = r.gb_z_peq_config,
                  .zb_cntl = r.zb_cntl,
                  .zb_zstencilcntl = r.zb_zstencilcntl,
                  .zb_depthclearvalue = r.zb_depthclearvalue,
                  .zb_depthoffset = r.zb_depthoffset,
                  .zb_depthpitch = r.zb_depthpitch,
                  .zb_zmask_pitch = r.zb_zmask_pitch,
                  .sc_scissors_tl = r.sc_scissors_tl,
                  .sc_scissors_br = r.sc_scissors_br,
                  .pfs_param_0 = { r.pfs_param_0[0], r.pfs_param_0[1],
                                   r.pfs_param_0[2], r.pfs_param_0[3] },
                  .vertex_offset = r.vertex_offset,
                  .zcache_flushes_before = r.zcache_flushes,
               };
            r.zcache_flushes = 0u;
         }
      }
      i += count + 1u;
   }
   out->final_zb_bw_cntl = r.zb_bw_cntl;
   out->final_gb_z_peq_config = r.gb_z_peq_config;
   return 0;
}

int
r300_zmask_read_quadrant_check_state(
   const struct r300_zmask_read_quadrant_params *params, const uint32_t *ib,
   uint32_t dwords)
{
   if (params == NULL || params->surface == NULL ||
       params->zmask_layout == NULL)
      return -EINVAL;
   struct r300_zmask_read_quadrant_stream_state state;
   if (r300_zmask_read_quadrant_read_state(ib, dwords, &state) != 0)
      return -EINVAL;

   /* One clear from dword zero over the layout's whole metadata, value
    * zero: the cleared encoding every draw reads. */
   if (state.zmask_clears != 1u || state.zmask_clear_payload[0] != 0u ||
       state.zmask_clear_payload[1] != params->zmask_layout->dwords ||
       state.zmask_clear_payload[2] != 0u ||
       state.draw_count != R300_ZMASK_READ_QUADRANT_COUNT)
      return -EINVAL;

   uint32_t clear_word = 0u;
   if (r300_zb_depth_pack(params->surface,
                          R300_ZMASK_READ_QUADRANT_CLEAR_DEPTH_CODE,
                          R300_ZMASK_READ_QUADRANT_CLEAR_STENCIL,
                          &clear_word) != 0)
      return -EINVAL;
   const uint32_t peq_wrapped = params->zmask_layout->zcomp8x8
                                   ? R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8
                                   : R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4;

   for (uint32_t q = 0; q < R300_ZMASK_READ_QUADRANT_COUNT; q++) {
      const struct r300_zmask_read_quadrant_draw *draw =
         &r300_zmask_read_quadrant_draws[q];
      const struct r300_zmask_read_quadrant_draw_state *s = &state.draws[q];
      uint32_t tl = 0u;
      uint32_t br = 0u;
      if (r300_first_draw_scissor_word(draw->origin_x, draw->origin_y, &tl) !=
             0 ||
          r300_first_draw_scissor_word(
             draw->origin_x + R300_ZMASK_READ_QUADRANT_EXTENT - 1u,
             draw->origin_y + R300_ZMASK_READ_QUADRANT_EXTENT - 1u, &br) != 0)
         return -EINVAL;
      if (s->zb_bw_cntl != draw->zb_bw_cntl ||
          s->gb_z_peq_config !=
             (draw->wrapped ? peq_wrapped
                            : R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4) ||
          s->zb_cntl != R300_Z_ENABLE ||
          s->zb_zstencilcntl != R300_ZS_LESS ||
          s->zb_depthclearvalue != clear_word ||
          s->zb_depthoffset != params->depth_offset_bytes ||
          s->zb_zmask_pitch != params->zmask_layout->stride_in_pixels ||
          s->sc_scissors_tl != tl || s->sc_scissors_br != br ||
          memcmp(s->pfs_param_0, draw->constant, sizeof(draw->constant)) !=
             0 ||
          s->vertex_offset != q * R300_ZMASK_READ_QUADRANT_VERTEX_SET_BYTES ||
          s->zcache_flushes_before == 0u)
         return -EINVAL;
   }
   if (state.final_zb_bw_cntl != 0u ||
       state.final_gb_z_peq_config != R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4)
      return -EINVAL;
   return 0;
}

int
r300_zmask_read_quadrant_fill_depth(const struct r300_zb_depth_layout *layout,
                                    uint8_t *bytes, uint64_t size)
{
   if (layout == NULL || bytes == NULL || size < layout->total_bytes ||
       layout->storage_bytes % 4u != 0u)
      return -EINVAL;
   uint32_t word = 0u;
   if (r300_zb_depth_pack(&r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
                          R300_ZMASK_READ_QUADRANT_BACKING_DEPTH_CODE,
                          R300_ZMASK_READ_QUADRANT_BACKING_STENCIL,
                          &word) != 0)
      return -EINVAL;
   memset(bytes, R300_ZMASK_READ_QUADRANT_DEPTH_GUARD_FILL, (size_t)size);
   /* Every slot carries the same word, so the seed needs no model of
    * where a tiled pixel lands. */
   for (uint64_t offset = 0; offset < layout->storage_bytes; offset += 4u)
      memcpy(bytes + layout->base_offset_bytes + offset, &word, sizeof(word));
   return 0;
}

void
r300_zmask_read_quadrant_fill_color(uint8_t *bytes, uint64_t size)
{
   const uint32_t sentinel = R300_ZMASK_READ_QUADRANT_COLOR_SENTINEL;
   for (uint64_t offset = 0; offset + 4u <= size; offset += 4u)
      memcpy(bytes + offset, &sentinel, sizeof(sentinel));
}

const char *
r300_zmask_read_quadrant_reading_name(
   enum r300_zmask_read_quadrant_reading reading)
{
   switch (reading) {
   case R300_ZMASK_READ_QUADRANT_COLORED:
      return "colored";
   case R300_ZMASK_READ_QUADRANT_SENTINEL:
      return "sentinel";
   case R300_ZMASK_READ_QUADRANT_MIXED:
      return "mixed";
   }
   return "unknown";
}

void
r300_zmask_read_quadrant_observe_color(
   const uint8_t *bytes, uint64_t size,
   struct r300_zmask_read_quadrant_color_observation *out)
{
   if (out == NULL)
      return;
   memset(out, 0, sizeof(*out));
   if (bytes == NULL || size < R300_ZMASK_READ_QUADRANT_COLOR_BYTES)
      return;

   for (uint32_t y = 0; y < R300_ZMASK_READ_QUADRANT_TARGET_HEIGHT; y++) {
      for (uint32_t x = 0; x < R300_ZMASK_READ_QUADRANT_TARGET_WIDTH; x++) {
         const uint32_t q = (y / R300_ZMASK_READ_QUADRANT_EXTENT) * 2u +
                            x / R300_ZMASK_READ_QUADRANT_EXTENT;
         uint32_t word;
         memcpy(&word,
                bytes + 4u * (y * R300_ZMASK_READ_QUADRANT_PITCH_PIXELS + x),
                sizeof(word));
         if (word == r300_zmask_read_quadrant_draws[q].color)
            out->colored[q]++;
         else if (word == R300_ZMASK_READ_QUADRANT_COLOR_SENTINEL)
            out->sentinel[q]++;
         else
            out->foreign[q]++;
      }
   }
   for (uint32_t y = R300_ZMASK_READ_QUADRANT_TARGET_HEIGHT;
        y < R300_ZMASK_READ_QUADRANT_COLOR_ROWS; y++) {
      for (uint32_t x = 0; x < R300_ZMASK_READ_QUADRANT_PITCH_PIXELS; x++) {
         uint32_t word;
         memcpy(&word,
                bytes + 4u * (y * R300_ZMASK_READ_QUADRANT_PITCH_PIXELS + x),
                sizeof(word));
         if (word != R300_ZMASK_READ_QUADRANT_COLOR_SENTINEL)
            out->guard_changed++;
      }
   }
   const uint32_t pixels =
      R300_ZMASK_READ_QUADRANT_EXTENT * R300_ZMASK_READ_QUADRANT_EXTENT;
   for (uint32_t q = 0; q < R300_ZMASK_READ_QUADRANT_COUNT; q++)
      out->reading[q] = out->colored[q] == pixels
                           ? R300_ZMASK_READ_QUADRANT_COLORED
                        : out->sentinel[q] == pixels
                           ? R300_ZMASK_READ_QUADRANT_SENTINEL
                           : R300_ZMASK_READ_QUADRANT_MIXED;
   out->judged = true;
}

uint64_t
r300_zmask_read_quadrant_depth_changed_bytes(const uint8_t *before,
                                             const uint8_t *after,
                                             uint64_t size)
{
   uint64_t changed = 0u;
   for (uint64_t i = 0; i < size; i++)
      changed += before[i] != after[i];
   return changed;
}
