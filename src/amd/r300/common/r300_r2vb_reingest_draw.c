/*
 * SPDX-License-Identifier: MIT
 *
 * Neutral re-ingest draw body: binds a GPU-written buffer as the vertex
 * array of a follow-on draw on R300-class silicon.
 */

#include "r300_r2vb_reingest_draw.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>

/* The fixed tail declares one FP32x4 stream: sixteen bytes per vertex,
 * size and stride both four dwords in the VBPNTR control word.
 */
#define REINGEST_VERTEX_BYTES 16u
#define REINGEST_VERTEX_DWORDS 4u

#define FLOAT_ONE_BITS 0x3f800000u

uint32_t r300_r2vb_reingest_draw_dwords(
   const struct r300_r2vb_reingest_draw_params *params)
{
   if (!params ||
       (params->rs &&
        (params->rs->table_len == 0 || params->rs->table_len > 8)))
      return 0;
   uint32_t total = 18;
   if (params->redirect)
      total += 9;
   if (params->position_only_output)
      total += 3;
   if (params->viewport_identity)
      total += 7;
   if (params->vte_w0)
      total += 4;
   if (params->vertex_assembly)
      total += 3;
   if (params->polygon_offset_clear)
      total += 5;
   if (params->stream_control_clear)
      total += 8;
   if (params->fog_stipple_clear)
      total += 6;
   if (params->rs)
      total += 7 + 2 * params->rs->table_len;
   return total;
}

int r300_r2vb_reingest_draw_emit(
   struct r300_pm4_builder *b,
   const struct r300_r2vb_reingest_draw_params *params,
   struct r300_r2vb_reingest_draw_relocs *relocs)
{
   if (!b)
      return -EINVAL;
   if (!params || !relocs) {
      if (!b->error)
         b->error = -EINVAL;
      return -EINVAL;
   }
   if (params->vertex_count == 0 ||
       params->vertex_count > R300_PM4_VTX_COUNT_LIMIT)
      goto invalid;
   /* 64-bit last-byte bound: the draw fetches vertex_count contiguous
    * FP32x4 rows from vertex_offset_bytes. */
   if ((uint64_t)params->vertex_offset_bytes +
          (uint64_t)params->vertex_count * REINGEST_VERTEX_BYTES >
       params->vertex_bo_size_bytes)
      goto invalid;
   if (params->rs &&
       (params->rs->table_len == 0 || params->rs->table_len > 8))
      goto invalid;

   if (!r300_pm4_builder_reserve(b, r300_r2vb_reingest_draw_dwords(params)))
      return b->error;

   relocs->color_ib_index = R300_PM4_NO_INDEX;

   if (params->redirect) {
      r300_pm4_reg(b, R300_RB3D_COLOROFFSET0, 0);
      relocs->color_ib_index =
         r300_pm4_reloc_nop(b, params->redirect->relocation_payload);
      r300_pm4_reg(b, R300_RB3D_COLORPITCH0,
                   params->redirect->color_pitch_word);
      const uint32_t scissors[2] = {
         params->redirect->scissor_tl_word,
         params->redirect->scissor_br_word,
      };
      r300_pm4_packet0(b, R300_SC_SCISSORS_TL, scissors, 2);
   }

   if (params->position_only_output) {
      const uint32_t fmt[2] = {
         params->out_vtx_fmt0,
         params->out_vtx_fmt1,
      };
      r300_pm4_packet0(b, R300_VAP_OUTPUT_VTX_FMT_0, fmt, 2);
   }

   if (params->viewport_identity) {
      /* Scale 1.0 and offset 0.0 on X, Y, and Z through
       * SE_VPORT_XSCALE..SE_VPORT_ZOFFSET. */
      const uint32_t viewport[6] = {
         FLOAT_ONE_BITS, 0, FLOAT_ONE_BITS, 0, FLOAT_ONE_BITS, 0,
      };
      r300_pm4_packet0(b, R300_SE_VPORT_XSCALE, viewport, 6);
   }

   if (params->vte_w0) {
      /* X, Y, Z, and W declared already in window space, so the VAP
       * applies no perspective divide on the re-ingest path. */
      r300_pm4_reg(b, R300_VAP_VTE_CNTL,
                   R300_VTX_XY_FMT | R300_VTX_Z_FMT | R300_VTX_W0_FMT);
   }

   if (params->vertex_assembly) {
      const uint32_t assembly[2] = {
         params->vtx_state_cntl,
         params->vsm_vtx_assm,
      };
      r300_pm4_packet0(b, R300_VAP_VTX_STATE_CNTL, assembly, 2);
   }

   if (params->polygon_offset_clear) {
      const uint32_t zeros[4] = { 0, 0, 0, 0 };
      r300_pm4_packet0(b, R300_SU_POLY_OFFSET_FRONT_SCALE, zeros, 4);
   }

   if (params->stream_control_clear) {
      const uint32_t zeros[7] = { 0, 0, 0, 0, 0, 0, 0 };
      r300_pm4_packet0(b, R300_VAP_PROG_STREAM_CNTL_1, zeros, 7);
   }

   if (params->fog_stipple_clear) {
      /* Three separate writes: the registers are not contiguous. */
      r300_pm4_reg(b, R300_GA_TRIANGLE_STIPPLE, 0);
      r300_pm4_reg(b, R300_GA_FOG_SCALE, 0);
      r300_pm4_reg(b, R300_GA_FOG_OFFSET, 0);
   }

   if (params->rs) {
      r300_pm4_reg(b, R300_GB_ENABLE, params->rs->gb_enable);
      r300_pm4_packet0(b, R300_RS_IP_0, params->rs->ip,
                       params->rs->table_len);
      const uint32_t counts[2] = {
         params->rs->count_word,
         params->rs->inst_count_word,
      };
      r300_pm4_packet0(b, R300_RS_COUNT, counts, 2);
      r300_pm4_packet0(b, R300_RS_INST_0, params->rs->inst,
                       params->rs->table_len);
   }

   r300_pm4_reg(b, R300_VAP_PROG_STREAM_CNTL_0, params->stream_cntl_word);
   r300_pm4_reg(b, R300_VAP_PROG_STREAM_CNTL_EXT_0,
                params->stream_cntl_ext_word);
   r300_pm4_reg(b, R300_VAP_VTX_SIZE, REINGEST_VERTEX_DWORDS);
   r300_pm4_emit_vertex_index_range(b, 0, params->vertex_count - 1);

   const uint32_t vbpntr[4] = {
      1 | R300_VC_FORCE_PREFETCH,
      REINGEST_VERTEX_DWORDS | (REINGEST_VERTEX_DWORDS << 8),
      params->vertex_offset_bytes,
      0,
   };
   r300_pm4_packet3(b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr, 4);
   relocs->vertex_ib_index =
      r300_pm4_reloc_nop(b, params->vertex_relocation_payload);

   const uint32_t vf_cntl =
      (params->vertex_count << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
      params->vf_prim | R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST;
   r300_pm4_packet3(b, R300_PACKET3_3D_DRAW_VBUF_2, &vf_cntl, 1);

   if (params->vte_w0)
      r300_pm4_reg(b, R300_VAP_VTE_CNTL, params->vte_restore_word);

   return b->error;

invalid:
   if (!b->error)
      b->error = -EINVAL;
   return -EINVAL;
}
