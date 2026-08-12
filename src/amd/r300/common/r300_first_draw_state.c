/* SPDX-License-Identifier: MIT */

#include "r300_first_draw_state.h"

#include "r300_reg.h"

#include <errno.h>
#include <string.h>

/* Scissor and clip-rectangle coordinates on non-R500 silicon carry a 1440
 * offset in both axes; the packed word is x | (y << 13).
 */
#define R300_FDS_SCISSOR_BIAS 1440
#define R300_FDS_SCISSOR_MAX 4095
#define R300_FDS_MAX_EXTENT \
   (R300_FDS_SCISSOR_MAX - R300_FDS_SCISSOR_BIAS + 1)

static uint32_t
scissor_word(uint32_t x, uint32_t y)
{
   return (x + R300_FDS_SCISSOR_BIAS) |
          ((y + R300_FDS_SCISSOR_BIAS) << 13);
}

/* The table lists every register a verified-rendering r300g TCL-bypass
 * triangle on RS482 establishes before its draw and the fixed cell does
 * not, in emission order, with the traced reference value. Entries whose
 * value derives from the
 * draw parameters carry 0 here and are resolved in
 * r300_first_draw_contract_resolve; entries marked REFERENCE_ARTIFACT
 * document reference state the neutral cell must not import and are
 * excluded from resolution.
 */
#define E(reg_, value_, disp_) \
   {reg_, value_, R300_FDS_##disp_, #reg_}
static const struct r300_first_draw_contract_entry r300_fds_table[] = {
   /* 1: pre-draw cache domain: destination and Z cache flush requests precede
    * the idle wait, so the wait retires both requests before new state lands.
    */
   E(R300_RB3D_DSTCACHE_CTLSTAT, 0x0000000a, ORDERING_BARRIER),
   E(R300_ZB_ZCACHE_CTLSTAT, 0x00000003, ORDERING_BARRIER),
   E(RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN, ORDERING_BARRIER),

   /* 2: unpipelined geometry-block and AA state. */
   E(R300_GB_ENABLE, 0x00000000, REQUIRED_INVARIANT),
   E(R300_GB_SELECT, 0x00000000, REQUIRED_INVARIANT),
   E(R300_GB_AA_CONFIG, 0x00000000, EXPLICIT_DISABLE),
   E(R300_GB_Z_PEQ_CONFIG, 0x00000000, EXPLICIT_DISABLE),
   E(R300_RB3D_AARESOLVE_CTL, 0x00000000, EXPLICIT_DISABLE),

   /* 3: depth, stencil, alpha, blend, and the color write mask. The draw
    * binds no depth buffer, so every ZB function is written disabled; the
    * blend words select source-copy; the channel mask is a proven gate.
    */
   E(R300_ZB_ZSTENCILCNTL, 0x00000000, EXPLICIT_DISABLE),
   E(R300_ZB_STENCILREFMASK, 0x00000000, EXPLICIT_DISABLE),
   E(R300_ZB_BW_CNTL, 0x00000000, EXPLICIT_DISABLE),
   E(R300_ZB_ZTOP, 0x00000001, REQUIRED_INVARIANT),
   E(R300_SC_HYPERZ, 0x0000001c, EXPLICIT_DISABLE),
   E(R300_FG_ALPHA_FUNC, 0x00000000, EXPLICIT_DISABLE),
   E(R300_FG_FOG_BLEND, 0x00000000, EXPLICIT_DISABLE),
   E(R300_RB3D_ROPCNTL, 0x00000000, EXPLICIT_DISABLE),
   E(R300_RB3D_CBLEND, 0x00000000, EXPLICIT_DISABLE),
   E(R300_RB3D_ABLEND, 0x00000000, EXPLICIT_DISABLE),
   E(RB3D_COLOR_CHANNEL_MASK, 0x0000000f, PROVEN_GATE),
   E(R300_RB3D_BLEND_COLOR, 0x00000000, EXPLICIT_DISABLE),
   E(R300_RB3D_DITHER_CTL, 0x00000000, EXPLICIT_DISABLE),
   E(R500_RB3D_DISCARD_SRC_PIXEL_LTE_THRESHOLD, 0x01010101, CHIP_DERIVED),
   E(R500_RB3D_DISCARD_SRC_PIXEL_GTE_THRESHOLD, 0xfefefefe, CHIP_DERIVED),

   /* 4: scissor, clip rectangle, clip rule, screendoor, edge rule. The
    * scissor and clip words derive from the target extent; SC_SCREENDOOR
    * is a proven gate whose zero drops every sample.
    */
   E(R300_SC_SCISSORS_TL, 0, GEOMETRY_PARAMETER),
   E(R300_SC_SCISSORS_BR, 0, GEOMETRY_PARAMETER),
   E(R300_SC_CLIPRECT_TL_0, 0, GEOMETRY_PARAMETER),
   E(R300_SC_CLIPRECT_BR_0, 0, GEOMETRY_PARAMETER),
   E(R300_SC_CLIP_RULE, 0x0000aaaa, REQUIRED_INVARIANT),
   E(R300_SC_SCREENDOOR, 0x00ffffff, PROVEN_GATE),
   E(R300_SC_EDGERULE, 0x2da49525, REQUIRED_INVARIANT),

   /* 5: VAP control, vertex engine, guard band, viewport, and clipping.
    * VAP_CNTL packs the chip's PVS slot configuration; VTE_CNTL selects
    * pretransformed XY/Z for TCL bypass; the viewport transform is
    * identity-by-zero because the vertices arrive in screen space; the
    * guard band is unity; CLIP_CNTL disables clipping.
    */
   E(R300_VAP_CNTL, 0x0014025a, CHIP_DERIVED),
   E(R300_VAP_VTE_CNTL, 0x00000300, REQUIRED_INVARIANT),
   E(R300_VAP_VTX_STATE_CNTL, 0x00005555, REQUIRED_INVARIANT),
   E(R300_VAP_VSM_VTX_ASSM, 0x00000001, REQUIRED_INVARIANT),
   E(R300_VAP_PSC_SGN_NORM_CNTL, 0xaaaaaaaa, REQUIRED_INVARIANT),
   E(R300_VAP_CLIP_CNTL, 0x00010000, EXPLICIT_DISABLE),
   E(R300_VAP_GB_VERT_CLIP_ADJ, 0x3f800000, REQUIRED_INVARIANT),
   E(R300_VAP_GB_VERT_DISC_ADJ, 0x3f800000, REQUIRED_INVARIANT),
   E(R300_VAP_GB_HORZ_CLIP_ADJ, 0x3f800000, REQUIRED_INVARIANT),
   E(R300_VAP_GB_HORZ_DISC_ADJ, 0x3f800000, REQUIRED_INVARIANT),
   E(R300_VAP_PVS_STATE_FLUSH_REG, 0x00000000, ORDERING_BARRIER),
   E(VAP_PVS_VTX_TIMEOUT_REG, 0x0000ffff, REQUIRED_INVARIANT),
   E(R300_VAP_VF_MAX_VTX_INDX, 0, GEOMETRY_PARAMETER),
   E(R300_SE_VPORT_XSCALE, 0x00000000, REQUIRED_INVARIANT),
   E(R300_SE_VPORT_XOFFSET, 0x00000000, REQUIRED_INVARIANT),
   E(R300_SE_VPORT_YSCALE, 0x00000000, REQUIRED_INVARIANT),
   E(R300_SE_VPORT_YOFFSET, 0x00000000, REQUIRED_INVARIANT),
   E(R300_SE_VPORT_ZSCALE, 0x00000000, REQUIRED_INVARIANT),
   E(R300_SE_VPORT_ZOFFSET, 0x00000000, REQUIRED_INVARIANT),

   /* 6: GA/SU raster parameters and the RS interpolator linkage. RS_COUNT
    * with a zero instruction count still needs its high configuration
    * field; RS_IP_0 routes the single interpolator.
    */
   E(R300_GA_POINT_S0, 0x00000000, REQUIRED_INVARIANT),
   E(R300_GA_POINT_T0, 0x00000000, REQUIRED_INVARIANT),
   E(R300_GA_POINT_S1, 0x3f800000, REQUIRED_INVARIANT),
   E(R300_GA_POINT_T1, 0x00000000, REQUIRED_INVARIANT),
   E(R300_GA_POINT_SIZE, 0x00060006, GEOMETRY_PARAMETER),
   E(R300_GA_POINT_MINMAX, 0x00060006, GEOMETRY_PARAMETER),
   E(R300_GA_LINE_CNTL, 0x00020006, GEOMETRY_PARAMETER),
   E(R300_GA_LINE_STIPPLE_CONFIG, 0x00000000, EXPLICIT_DISABLE),
   E(R300_GA_LINE_STIPPLE_VALUE, 0x00000000, EXPLICIT_DISABLE),
   E(R300_GA_POLY_MODE, 0x00000000, REQUIRED_INVARIANT),
   E(R300_GA_ROUND_MODE, 0x00000005, REQUIRED_INVARIANT),
   E(R300_GA_OFFSET, 0x00000000, EXPLICIT_DISABLE),
   E(R300_GA_COLOR_CONTROL, 0x0003aaaa, REQUIRED_INVARIANT),
   E(R300_SU_TEX_WRAP, 0x00000000, EXPLICIT_DISABLE),
   E(R300_SU_DEPTH_SCALE, 0x4b7fffff, REQUIRED_INVARIANT),
   E(R300_SU_DEPTH_OFFSET, 0x00000000, REQUIRED_INVARIANT),
   E(R300_SU_POLY_OFFSET_ENABLE, 0x00000000, EXPLICIT_DISABLE),
   E(R300_SU_CULL_MODE, 0x00000004, REQUIRED_INVARIANT),
   E(R300_RS_IP_0, 0x00000c00, REQUIRED_INVARIANT),
   E(R300_RS_COUNT, 0x00040080, REQUIRED_INVARIANT),
   E(R300_RS_INST_COUNT, 0x00000000, REQUIRED_INVARIANT),
   E(R300_RS_INST_0, 0x00000000, REQUIRED_INVARIANT),

   /* 7: pipelined framebuffer state follows the unpipelined registers.
    * FMT_0 is a proven gate whose UNUSED value drops every fragment; FMT_1..3
    * are written UNUSED because the program drives one output. GB_MSPOS0/1
    * carry the pipelined sample positions after that unpipelined state.
    */
   E(R300_US_OUT_FMT_0, 0x00003900, PROVEN_GATE),
   E(R300_US_OUT_FMT_0 + 4, 0x0000000f, EXPLICIT_DISABLE),
   E(R300_US_OUT_FMT_0 + 8, 0x0000000f, EXPLICIT_DISABLE),
   E(R300_US_OUT_FMT_0 + 12, 0x0000000f, EXPLICIT_DISABLE),
   E(R300_GB_MSPOS0, 0x66666666, REQUIRED_INVARIANT),
   E(R300_GB_MSPOS1, 0x06666666, REQUIRED_INVARIANT),

   /* 8: texture disable. The r300g reference enables one dummy texture
    * for its non-R500 texkill policy; the neutral draw binds no texture,
    * so the contract writes the block disabled instead of importing a
    * nonexistent binding.
    */
   E(R300_TX_INVALTAGS, 0x00000000, ORDERING_BARRIER),
   E(R300_TX_ENABLE, 0x00000000, EXPLICIT_DISABLE),

   /* Reference artifacts, excluded from resolution: depth-buffer resource
    * words for a depth buffer the draw does not bind, and the dummy
    * texture's descriptor words. ZB_FORMAT through ZB_DEPTHCLEARVALUE
    * describe r300g's allocated zbuffer; TX_FILTER through TX_OFFSET
    * describe its dummy texture.
    */
   E(R300_ZB_FORMAT, 0x00000002, REFERENCE_ARTIFACT),
   E(R300_ZB_DEPTHOFFSET, 0x00002000, REFERENCE_ARTIFACT),
   E(R300_ZB_DEPTHPITCH, 0x00030040, REFERENCE_ARTIFACT),
   E(R300_ZB_DEPTHCLEARVALUE, 0x00000000, REFERENCE_ARTIFACT),
   E(R300_TX_FILTER0_0, 0x00002a00, REFERENCE_ARTIFACT),
   E(R300_TX_FILTER1_0, 0x00000000, REFERENCE_ARTIFACT),
   E(R300_TX_FORMAT0_0, 0x00000000, REFERENCE_ARTIFACT),
   E(R300_TX_FORMAT1_0, 0x00000000, REFERENCE_ARTIFACT),
   E(R300_TX_FORMAT2_0, 0x00000000, REFERENCE_ARTIFACT),
   E(R300_TX_OFFSET_0, 0x00000000, REFERENCE_ARTIFACT),
   E(R300_TX_BORDER_COLOR_0, 0x00000000, REFERENCE_ARTIFACT),
};
#undef E

#define R300_FDS_TABLE_COUNT \
   (sizeof(r300_fds_table) / sizeof(r300_fds_table[0]))

int
r300_first_draw_contract_resolve(const struct r300_first_draw_params *params,
                                 struct r300_first_draw_contract *out)
{
   if (params->chip_family != CHIP_RS480)
      return -ENOTSUP;

   /* The resolver bounds each extent with R300_FDS_MAX_EXTENT before
    * computing the biased high coordinate, so unsigned scissor arithmetic
    * cannot wrap into the accepted range.
    */
   if (params->width == 0 || params->height == 0 ||
       params->width > R300_FDS_MAX_EXTENT ||
       params->height > R300_FDS_MAX_EXTENT) {
      return -EINVAL;
   }

   memset(out, 0, sizeof(*out));
   for (uint32_t i = 0; i < R300_FDS_TABLE_COUNT; i++) {
      struct r300_first_draw_contract_entry entry = r300_fds_table[i];
      if (entry.disposition == R300_FDS_REFERENCE_ARTIFACT) {
         continue;
      }
      if (params->texture_enabled &&
          (entry.reg == R300_TX_INVALTAGS || entry.reg == R300_TX_ENABLE)) {
         /* A texturing caller owns the TX block on top of the contract. */
         continue;
      }
      switch (entry.reg) {
      case R300_SC_SCISSORS_TL:
      case R300_SC_CLIPRECT_TL_0:
         entry.value = scissor_word(0, 0);
         break;
      case R300_SC_SCISSORS_BR:
      case R300_SC_CLIPRECT_BR_0:
         entry.value = scissor_word(params->width - 1, params->height - 1);
         break;
      case R300_VAP_VF_MAX_VTX_INDX:
         entry.value = params->max_vtx_index;
         break;
      default:
         break;
      }
      out->entries[out->count++] = entry;
   }
   return 0;
}

int
r300_first_draw_state_emit(const struct r300_first_draw_contract *contract,
                           uint32_t *ib, uint32_t max_dwords)
{
   if (contract->count * 2 > max_dwords) {
      return -ENOSPC;
   }
   uint32_t n = 0;
   for (uint32_t i = 0; i < contract->count; i++) {
      ib[n++] = CP_PACKET0(contract->entries[i].reg, 0);
      ib[n++] = contract->entries[i].value;
   }
   return (int)n;
}

static bool
is_draw_packet(uint32_t header)
{
   if ((header >> 30) != 3)
      return false;

   switch (header & 0xff00) {
   case R300_PACKET3_3D_DRAW_VBUF:
   case R300_PACKET3_3D_DRAW_IMMD:
   case R300_PACKET3_3D_DRAW_INDX:
   case R300_PACKET3_3D_DRAW_VBUF_2:
   case R300_PACKET3_3D_DRAW_IMMD_2:
   case R300_PACKET3_3D_DRAW_INDX_2:
   case R300_PACKET3_3D_DRAW_128:
      return true;
   default:
      return false;
   }
}

uint32_t
r300_first_draw_state_check(const struct r300_first_draw_contract *contract,
                            const uint32_t *ib, uint32_t ib_dwords,
                            uint32_t poison,
                            struct r300_first_draw_check_report *report)
{
   uint32_t state[96];
   bool written[96];
   uint32_t pre_draw_value[96];
   bool pre_draw_written[96];
   for (uint32_t i = 0; i < contract->count; i++) {
      state[i] = poison;
      written[i] = false;
      pre_draw_value[i] = poison;
      pre_draw_written[i] = false;
   }

   /* Replay every PACKET0 write over the poisoned seed. Type-3 packets
    * advance over their payload, while unknown packet kinds advance by one
    * dword. A PACKET0 claiming payload past the end is the pad and ends it.
    * Ordering barriers retain the last value written before the first draw,
    * because a later write cannot make an earlier cache or idle operation
    * effective at that draw boundary.
    */
   uint32_t i = 0;
   bool draw_seen = false;
   while (i < ib_dwords) {
      uint32_t header = ib[i];
      uint32_t kind = header >> 30;
      uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (kind == 0 || kind == 3) {
         if (i + count >= ib_dwords) {
            break;
         }
      }
      if (kind == 0) {
         const uint32_t base = (header & 0x3FFF) * 4;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
         for (uint32_t k = 0; k < count; k++) {
            const uint32_t reg = base + (one_reg ? 0 : 4 * k);
            for (uint32_t e = 0; e < contract->count; e++) {
               if (contract->entries[e].reg == reg) {
                  state[e] = ib[i + 1 + k];
                  written[e] = true;
                  if (!draw_seen &&
                      contract->entries[e].disposition ==
                         R300_FDS_ORDERING_BARRIER) {
                        pre_draw_value[e] = ib[i + 1 + k];
                        pre_draw_written[e] = true;
                     }
               }
            }
         }
         i += 1 + count;
      } else if (kind == 3) {
         if (is_draw_packet(header))
            draw_seen = true;
         i += 1 + count;
      } else {
         i += 1;
      }
   }

   memset(report, 0, sizeof(*report));
   /* A clause is satisfied only by a write in the stream that reaches the
    * contract value. A seed equal to the value leaves an unwritten
    * register unsatisfied, so a stream passes only when it establishes
    * every value itself, independent of predecessor state. An ordering
    * barrier additionally requires its contract value at the first draw
    * boundary; a post-draw write cannot repair a pre-draw value.
    */
   for (uint32_t e = 0; e < contract->count; e++) {
      if (!written[e] || state[e] != contract->entries[e].value ||
          (contract->entries[e].disposition == R300_FDS_ORDERING_BARRIER &&
           (!pre_draw_written[e] ||
            pre_draw_value[e] != contract->entries[e].value))) {
         report->unsatisfied[report->unsatisfied_count++] = e;
      }
   }
   return report->unsatisfied_count;
}
