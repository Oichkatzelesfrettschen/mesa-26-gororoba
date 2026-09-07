/* SPDX-License-Identifier: MIT */

#include "r300_zb_depth_discovery_cell.h"

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_zb_depth_state.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot. */
#define DISCOVERY_RELOC_PAYLOAD(slot) ((slot) * 4)

/* Identity PSC swizzle select: X, Y, Z, W in place with a full write
 * mask, the per-word value the kernel's TCL-bypass vertex-output check
 * requires on every VAP_PROG_STREAM_CNTL_EXT word. */
#define DISCOVERY_PSC_EXT_IDENTITY 0xF688F688u

#define DISCOVERY_VERTEX_COUNT 6u
#define DISCOVERY_MAX_VTX_INDEX (DISCOVERY_VERTEX_COUNT - 1u)

const float
   r300_zb_depth_discovery_vertices[R300_ZB_DISCOVERY_VERTEX_DWORDS] = {
       0.0f,  0.0f, R300_ZB_DISCOVERY_MARKER_Z, 1.0f,
      64.0f,  0.0f, R300_ZB_DISCOVERY_MARKER_Z, 1.0f,
       0.0f, 64.0f, R300_ZB_DISCOVERY_MARKER_Z, 1.0f,
      64.0f,  0.0f, R300_ZB_DISCOVERY_MARKER_Z, 1.0f,
      64.0f, 64.0f, R300_ZB_DISCOVERY_MARKER_Z, 1.0f,
       0.0f, 64.0f, R300_ZB_DISCOVERY_MARKER_Z, 1.0f,
};

static void
write_reloc(struct r300_pm4_builder *b, struct r300_zb_depth_discovery_ib *out,
            uint32_t slot)
{
   if (b->error != 0)
      return;
   if (slot >= R300_ZB_DISCOVERY_SLOT_COUNT ||
       out->reloc_site_count >= R300_ZB_DISCOVERY_MAX_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }

   const uint32_t index = r300_pm4_reloc_nop(b, DISCOVERY_RELOC_PAYLOAD(slot));
   if (index == R300_PM4_NO_INDEX)
      return;
   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_zb_depth_discovery_reloc_site){ .ib_index = index,
                                                   .slot = slot };
}

/* The depth state emitter placed the depth relocation payload, so its
 * site comes from the index that emitter reports rather than from this
 * cell re-deriving the packet layout. */
static void
record_depth_site(struct r300_pm4_builder *b,
                  struct r300_zb_depth_discovery_ib *out, uint32_t index)
{
   if (b->error != 0 || index == R300_PM4_NO_INDEX)
      return;
   if (out->reloc_site_count >= R300_ZB_DISCOVERY_MAX_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }
   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_zb_depth_discovery_reloc_site){
         .ib_index = index, .slot = R300_ZB_DISCOVERY_SLOT_DEPTH };
}

int
r300_zb_depth_discovery_emit_into(
   const struct r300_zb_depth_discovery_params *params, uint32_t *words,
   uint32_t capacity, struct r300_zb_depth_discovery_ib *out)
{
   if (out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   if (params == NULL || words == NULL)
      return -EINVAL;

   const struct r300_fragment_binary *fs = params->fragment_binary;
   if (fs == NULL || !fs->validated)
      return -EINVAL;
   if (params->first_draw_contract == NULL)
      return -EINVAL;
   if (params->depth_function > R300_ZS_ALWAYS)
      return -EINVAL;

   /* The scenario is the experiment, so it is checked before a dword is
    * placed: an unadmitted surface, a coordinate outside the extent, or
    * a marker equal to the initial code each make the run's result
    * unreadable rather than merely wrong. */
   const int scenario_rc =
      r300_zb_depth_discovery_scenario_check(params->scenario);
   if (scenario_rc != 0)
      return scenario_rc;
   const struct r300_zb_depth_surface *surface = params->scenario->surface;

   /* The cell's vertices, contract, and color oracle carry the target
    * extent as constants, so a surface naming other geometry would emit
    * one layout and be read at another. */
   if (surface->width != R300_ZB_DISCOVERY_TARGET_WIDTH ||
       surface->height != R300_ZB_DISCOVERY_TARGET_HEIGHT ||
       surface->pitch_pixels != R300_ZB_DISCOVERY_PITCH_PIXELS)
      return -EINVAL;

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, capacity);

   /* The contract's writes land before any cell state, and the contract
    * carries the one-pixel scissor, so the confinement the poison
    * checker verifies is the confinement the stream emits. */
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

   /* Pretransformed positions bypass the TCL block, one FLOAT_4 stream
    * lands whole in output vector zero, and every PSC extended selector
    * stays identity, so the kernel's vertex-output check can prove
    * VAP_VTX_SIZE = 4 covers the fetch. */
   r300_pm4_reg(&b, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   r300_pm4_reg(&b, R300_VAP_PROG_STREAM_CNTL_0,
                R300_DATA_TYPE_FLOAT_4 | (0 << R300_DST_VEC_LOC_SHIFT) |
                   R300_LAST_VEC);
   static const uint32_t psc_ext_identity[8] = {
      DISCOVERY_PSC_EXT_IDENTITY, DISCOVERY_PSC_EXT_IDENTITY,
      DISCOVERY_PSC_EXT_IDENTITY, DISCOVERY_PSC_EXT_IDENTITY,
      DISCOVERY_PSC_EXT_IDENTITY, DISCOVERY_PSC_EXT_IDENTITY,
      DISCOVERY_PSC_EXT_IDENTITY, DISCOVERY_PSC_EXT_IDENTITY,
   };
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_EXT_0, psc_ext_identity,
                    ARRAY_SIZE(psc_ext_identity));
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_0,
                R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
   r300_pm4_reg(&b, R300_VAP_OUTPUT_VTX_FMT_1, 0);
   r300_pm4_reg(&b, R300_VAP_VTX_SIZE, 4);

   /* One constant color over the covering primitive, so the color
    * attachment reports which pixels the scissor admitted rather than
    * which value a shader computed. */
   r300_pm4_block(&b, fs->cb_code, fs->cb_code_size);
   r300_pm4_reg(&b, R300_FG_DEPTH_SRC, fs->fg_depth_src);
   r300_pm4_reg(&b, R300_US_W_FMT, fs->us_out_w);

   r300_pm4_reg(&b, R300_RB3D_CCTL, 0);
   r300_pm4_reg(&b, R300_RB3D_COLOROFFSET0, 0);
   write_reloc(&b, out, R300_ZB_DISCOVERY_SLOT_COLOR);
   r300_pm4_reg(&b, R300_RB3D_COLORPITCH0, params->color_pitch_format);

   /* The depth binding and the write.  ZB_BW_CNTL leaves this emitter
    * with HiZ and fast fill disabled and every other bit clear, which is
    * the executing value: it is written after the contract's, so the
    * compression and ZB_CB_CLEAR state the run needs is this word rather
    * than the contract's entry for the same register. */
   const struct r300_zb_depth_state_params depth = {
      .pitch_pixels = surface->pitch_pixels,
      .depth_format = surface->depth_format,
      .pitch_tile_bits = r300_zb_depth_surface_tile_bits(surface),
      .depth_offset_bytes = params->depth_offset_bytes,
      .depth_relocation_payload =
         DISCOVERY_RELOC_PAYLOAD(R300_ZB_DISCOVERY_SLOT_DEPTH),
      .depth_function = params->depth_function,
      .depth_write = params->depth_write,
   };
   uint32_t depth_reloc_index = R300_PM4_NO_INDEX;
   const int depth_rc =
      r300_zb_depth_state_emit(&b, &depth, &depth_reloc_index);
   if (depth_rc != 0 && b.error == 0)
      b.error = depth_rc;
   record_depth_site(&b, out, depth_reloc_index);

   const uint32_t vbpntr[3] = {
      1 | R300_VC_FORCE_PREFETCH,
      R300_VBPNTR_SIZE0(16) | R300_VBPNTR_STRIDE0(16),
      params->vertex_offset,
   };
   r300_pm4_packet3(&b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr,
                    ARRAY_SIZE(vbpntr));
   write_reloc(&b, out, R300_ZB_DISCOVERY_SLOT_VERTEX);

   const uint32_t draw =
      R300_VAP_VF_CNTL__PRIM_TRIANGLES | R300_PRIM_WALK_LIST |
      (DISCOVERY_VERTEX_COUNT << R300_PRIM_NUM_VERTICES_SHIFT);
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, &draw, 1);

   /* Both caches publish: the color attachment is the coordinate oracle
    * and the depth surface is the address observation, so a host reading
    * either before its cache retires would read a state the draw has not
    * finished producing. */
   r300_pm4_reg(&b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);

   const int rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (rc != 0) {
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   return 0;
}

int
r300_zb_depth_discovery_emit(
   const struct r300_zb_depth_discovery_params *params,
   struct r300_zb_depth_discovery_ib *out)
{
   if (out == NULL)
      return -EINVAL;

   uint32_t *words = calloc(R300_ZB_DISCOVERY_MAX_DWORDS, sizeof(*words));
   if (words == NULL)
      return -ENOMEM;

   const int rc = r300_zb_depth_discovery_emit_into(
      params, words, R300_ZB_DISCOVERY_MAX_DWORDS, out);
   if (rc != 0) {
      free(words);
      return rc;
   }
   out->owns_ib = true;
   return 0;
}

void
r300_zb_depth_discovery_release(struct r300_zb_depth_discovery_ib *ib)
{
   if (ib == NULL)
      return;
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_zb_depth_discovery_reference_contract(
   const struct r300_zb_depth_discovery_scenario *scenario,
   struct r300_first_draw_contract *out)
{
   if (out == NULL)
      return -EINVAL;
   const int scenario_rc = r300_zb_depth_discovery_scenario_check(scenario);
   if (scenario_rc != 0)
      return scenario_rc;

   const struct r300_first_draw_params params = {
      .chip_family = CHIP_RS480,
      .width = R300_ZB_DISCOVERY_TARGET_WIDTH,
      .height = R300_ZB_DISCOVERY_TARGET_HEIGHT,
      .min_vtx_index = 0,
      .max_vtx_index = DISCOVERY_MAX_VTX_INDEX,
      .texture_enabled = false,
      .multisample = false,
   };
   const int rc = r300_first_draw_contract_resolve(&params, out);
   if (rc != 0)
      return rc;

   const int fmt_rc = r300_first_draw_contract_set_us_out_fmt_0(
      out, R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
              R300_C2_SEL_R | R300_C3_SEL_A);
   if (fmt_rc != 0)
      return fmt_rc;

   /* The scissor narrows to the one declared pixel; the clip rectangle
    * keeps the full extent the contract resolved.  One register confines
    * the write and the other stays wider, so the color readback names
    * the scissor rather than the intersection of two narrowings. */
   /* SC_SCISSORS_BR is inclusive -- the contract resolves the full
    * extent as (width - 1, height - 1) -- so one pixel is the corner
    * repeated in both registers. */
   uint32_t word = 0;
   const int word_rc =
      r300_first_draw_scissor_word(scenario->pixel_x, scenario->pixel_y,
                                   &word);
   if (word_rc != 0)
      return word_rc;
   const int set_tl =
      r300_first_draw_contract_set_entry(out, R300_SC_SCISSORS_TL, word);
   if (set_tl != 0)
      return set_tl;
   return r300_first_draw_contract_set_entry(out, R300_SC_SCISSORS_BR, word);
}

int
r300_zb_depth_discovery_reference_emit(
   const struct r300_zb_depth_discovery_scenario *scenario,
   uint32_t depth_function, bool depth_write,
   struct r300_zb_depth_discovery_ib *out)
{
   struct r300_fragment_binary fs;
   int rc = r300_tcl_bypass_triangle_reference_fs(&fs);
   if (rc != 0)
      return rc;

   struct r300_first_draw_contract contract;
   rc = r300_zb_depth_discovery_reference_contract(scenario, &contract);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }

   const struct r300_zb_depth_discovery_params params = {
      .scenario = scenario,
      .vertex_offset = 0,
      .color_pitch_format =
         r300_rb3d_colorpitch0_pack_argb8888(R300_ZB_DISCOVERY_PITCH_PIXELS),
      .depth_offset_bytes = 0,
      .depth_function = depth_function,
      .depth_write = depth_write,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   rc = r300_zb_depth_discovery_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_zb_depth_discovery_validate_reloc_sites(
   const struct r300_zb_depth_discovery_ib *ib)
{
   if (ib == NULL || ib->ib == NULL)
      return -EINVAL;
   if (ib->reloc_site_count != R300_ZB_DISCOVERY_SLOT_COUNT)
      return -EINVAL;

   uint32_t seen = 0;
   uint32_t previous = 0;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_zb_depth_discovery_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot >= R300_ZB_DISCOVERY_SLOT_COUNT)
         return -EINVAL;
      if (seen & (1u << site->slot))
         return -EINVAL;
      seen |= 1u << site->slot;
      if (site->ib_index >= ib->ib_size_dwords)
         return -EINVAL;
      /* Sites are recorded in the order the emitter placed them, so a
       * table whose indices do not rise names a stream position the
       * emitter did not reach in that order. */
      if (i > 0 && site->ib_index <= previous)
         return -EINVAL;
      previous = site->ib_index;
      if (ib->ib[site->ib_index] != DISCOVERY_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
   }
   return seen == (1u << R300_ZB_DISCOVERY_SLOT_COUNT) - 1u ? 0 : -EINVAL;
}

int
r300_zb_depth_discovery_read_state(const uint32_t *ib, uint32_t dwords,
                                   struct r300_zb_depth_discovery_state *out)
{
   if (out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   if (ib == NULL)
      return -EINVAL;

   for (uint32_t i = 0; i < dwords;) {
      const uint32_t header = ib[i];
      const uint32_t type = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3fffu) + 1u;

      if (type == 0) {
         const uint32_t reg = (header & 0x1fffu) << 2;
         if (count > dwords - i - 1u)
            return -EINVAL;
         for (uint32_t j = 0; j < count; j++) {
            const uint32_t value = ib[i + 1u + j];
            switch (reg + 4u * j) {
            case R300_ZB_FORMAT:
               out->zb_format = value;
               out->zb_format_writes++;
               break;
            case R300_ZB_CNTL:
               out->zb_cntl = value;
               break;
            case R300_ZB_ZSTENCILCNTL:
               out->zb_zstencilcntl = value;
               break;
            case R300_ZB_BW_CNTL:
               out->zb_bw_cntl = value;
               break;
            case R300_GB_Z_PEQ_CONFIG:
               out->gb_z_peq_config = value;
               break;
            case R300_SC_SCISSORS_TL:
               out->sc_scissors_tl = value;
               break;
            case R300_SC_SCISSORS_BR:
               out->sc_scissors_br = value;
               break;
            case R300_SC_CLIPRECT_TL_0:
               out->sc_cliprect_tl = value;
               break;
            case R300_SC_CLIPRECT_BR_0:
               out->sc_cliprect_br = value;
               break;
            case R300_SC_SCREENDOOR:
               out->sc_screendoor = value;
               break;
            default:
               break;
            }
         }
         i += count + 1u;
      } else if (type == 3) {
         if (count > dwords - i - 1u)
            return -EINVAL;
         i += count + 1u;
      } else if (type == 2) {
         i += 1u;
      } else {
         return -EINVAL;
      }
   }
   return 0;
}

/* Unpacks a scissor word into its two biased fields.  The bias cancels
 * in a comparison between two words, so the checker compares words
 * directly and this exists for the containment test alone. */
static void
scissor_fields(uint32_t word, uint32_t *x, uint32_t *y)
{
   *x = word & 0x1fffu;
   *y = (word >> 13) & 0x1fffu;
}

int
r300_zb_depth_discovery_check_state(
   const struct r300_zb_depth_discovery_params *params, const uint32_t *ib,
   uint32_t dwords)
{
   if (params == NULL)
      return -EINVAL;
   const int scenario_rc =
      r300_zb_depth_discovery_scenario_check(params->scenario);
   if (scenario_rc != 0)
      return scenario_rc;

   struct r300_zb_depth_discovery_state state;
   const int rc = r300_zb_depth_discovery_read_state(ib, dwords, &state);
   if (rc != 0)
      return rc;

   /* One ZB_FORMAT write, so the format the parser reads and the format
    * the last write leaves are the same dword by construction. */
   if (state.zb_format_writes != 1u)
      return -EINVAL;
   if (state.zb_format != params->scenario->surface->depth_format)
      return -EINVAL;

   /* The depth test is armed and the write follows the parameter, so the
    * depth-writes-disabled control is checked by the same predicate that
    * checks the discovery arm. */
   if ((state.zb_cntl & R300_Z_ENABLE) == 0u)
      return -EINVAL;
   /* The stencil test stays disabled.  A packed Z24/S8 surface stores a
    * stencil byte beside every depth code, so an armed stencil test
    * would gate the depth write on a component the experiment observes
    * rather than controls, and a stencil write would move a byte the
    * seed pair reads as the depth write's own effect. */
   if ((state.zb_cntl & R300_STENCIL_ENABLE) != 0u)
      return -EINVAL;
   const bool write_enabled = (state.zb_cntl & R300_Z_WRITE_ENABLE) != 0u;
   if (write_enabled != params->depth_write)
      return -EINVAL;
   if (((state.zb_zstencilcntl >> R300_Z_FUNC_SHIFT) & R300_ZS_MASK) !=
       params->depth_function)
      return -EINVAL;

   /* Every ZB_BW_CNTL bit clear: HiZ, fast fill, read and write
    * compression, and ZB_CB_CLEAR.  ZB_CB_CLEAR's set state selects
    * cache-line-granular write-only operation, which leaves the
    * untouched portion of a partially written microtile unknown, and an
    * unknown byte inside the envelope makes a one-pixel address
    * observation unreadable. */
   if (state.zb_bw_cntl != 0u)
      return -EINVAL;
   /* R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4 is bit zero clear: plane
    * equations stay 4x4 while compression is disabled. */
   if (state.gb_z_peq_config != 0u)
      return -EINVAL;
   if (state.sc_screendoor != 0x00ffffffu)
      return -EINVAL;

   /* The scissor confines the write to the declared pixel. */
   uint32_t expected = 0;
   const int word_rc = r300_first_draw_scissor_word(
      params->scenario->pixel_x, params->scenario->pixel_y, &expected);
   if (word_rc != 0)
      return word_rc;
   if (state.sc_scissors_tl != expected || state.sc_scissors_br != expected)
      return -EINVAL;

   /* The clip rectangle stays no narrower than the scissor, so the
    * scissor alone names the confined region. */
   uint32_t clip_tl_x, clip_tl_y, clip_br_x, clip_br_y, sc_x, sc_y;
   scissor_fields(state.sc_cliprect_tl, &clip_tl_x, &clip_tl_y);
   scissor_fields(state.sc_cliprect_br, &clip_br_x, &clip_br_y);
   scissor_fields(expected, &sc_x, &sc_y);
   if (clip_tl_x > sc_x || clip_tl_y > sc_y || clip_br_x < sc_x ||
       clip_br_y < sc_y)
      return -EINVAL;

   return 0;
}
