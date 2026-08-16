/* SPDX-License-Identifier: MIT */

#include "r300_r2vb_fetch_pass.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>

/* The VBPNTR control word packs each size and stride as an 8-bit
 * dword-unit field: size0 at bits 0..7, stride0 at 8..15, size1 at
 * 16..23, stride1 at 24..31.
 */
#define R300_R2VB_FETCH_VBPNTR_FIELD_MAX 0xffu

static int
fetch_stream_validate(const struct r300_r2vb_fetch_stream *s,
                      uint32_t vertex_count)
{
   if (s->size_dwords == 0 ||
       s->size_dwords > R300_R2VB_FETCH_VBPNTR_FIELD_MAX ||
       s->stride_dwords > R300_R2VB_FETCH_VBPNTR_FIELD_MAX)
      return -EINVAL;
   /* A stride below the fetch size makes consecutive vertices overlap;
    * the fetched declaration reads size dwords per vertex, so the
    * stride carries at least that many.
    */
   if (s->stride_dwords < s->size_dwords)
      return -EINVAL;
   /* 64-bit last-byte bound: offset + stride * (count - 1) + size must
    * lie inside the BO.  Every operand is at most 32 bits wide, so the
    * 64-bit sum cannot wrap.
    */
   const uint64_t last_byte = (uint64_t)s->offset_bytes +
                              (uint64_t)s->stride_dwords * 4 *
                                 (vertex_count - 1) +
                              (uint64_t)s->size_dwords * 4;
   if (last_byte > s->bo_size_bytes)
      return -ERANGE;
   return 0;
}

int
r300_r2vb_fetch_pass_emit(struct r300_pm4_builder *b,
                          const struct r300_r2vb_fetch_pass_params *params,
                          struct r300_r2vb_fetch_pass_relocs *relocs)
{
   if (params == NULL || params->state == NULL || relocs == NULL)
      return -EINVAL;
   if (params->vertex_count == 0 ||
       params->vertex_count > R300_PM4_VTX_INDX_LIMIT + 1)
      return -EINVAL;
   for (unsigned i = 0; i < 2; i++) {
      const int rc =
         fetch_stream_validate(&params->stream[i], params->vertex_count);
      if (rc != 0)
         return rc;
   }
   /* VAP_VTX_SIZE names the dwords the VAP consumes per vertex, which
    * is the sum of the declared fetch sizes; a mismatch is a latent
    * stride error that reads past or short of every vertex.
    */
   if (params->state->fetch_dwords !=
       params->stream[0].size_dwords + params->stream[1].size_dwords)
      return -EINVAL;

   if (!r300_pm4_builder_reserve(b, R300_R2VB_FETCH_PASS_DWORDS))
      return b->error;

   const struct r300_r2vb_fetch_state *st = params->state;
   const struct r300_r2vb_fetch_stream *slot = &params->stream[0];
   const struct r300_r2vb_fetch_stream *model = &params->stream[1];

   /* The full stream range, zeroed tail included: a stale
    * PROG_STREAM_CNTL word from an inherited draw would add a phantom
    * fetch, so the body clears all eight pairs.
    */
   r300_pm4_packet0(b, R300_VAP_PROG_STREAM_CNTL_0,
                    st->vap_prog_stream_cntl, 8);
   r300_pm4_packet0(b, R300_VAP_PROG_STREAM_CNTL_EXT_0,
                    st->vap_prog_stream_cntl_ext, 8);
   r300_pm4_reg(b, R300_VAP_VTX_SIZE, st->fetch_dwords);
   r300_pm4_reg(b, R300_VAP_VTX_STATE_CNTL, st->vap_vtx_state_cntl);
   r300_pm4_reg(b, R300_VAP_VSM_VTX_ASSM, st->vap_vsm_vtx_assm);
   r300_pm4_packet0(b, R300_VAP_OUTPUT_VTX_FMT_0, st->vap_out_vtx_fmt, 2);
   r300_pm4_reg(b, R300_GB_ENABLE, st->gb_enable);
   r300_pm4_packet0(b, R300_RS_IP_0, st->rs_ip, 8);
   const uint32_t rs_counts[2] = {st->rs_count, st->rs_inst_count};
   r300_pm4_packet0(b, R300_RS_COUNT, rs_counts, 2);
   r300_pm4_packet0(b, R300_RS_INST_0, st->rs_inst, 8);
   r300_pm4_emit_vertex_index_range(b, 0, params->vertex_count - 1);

   /* Two-array LOAD_VBPNTR: slot positions then the model span, each
    * pointer rebased by the kernel through the NOP-form relocation that
    * follows the packet.  The VBPNTR control macros take bytes and
    * store dwords.
    */
   const uint32_t vbpntr[4] = {
      2 | R300_VC_FORCE_PREFETCH,
      R300_VBPNTR_SIZE0(slot->size_dwords * 4) |
         R300_VBPNTR_STRIDE0(slot->stride_dwords * 4) |
         R300_VBPNTR_SIZE1(model->size_dwords * 4) |
         R300_VBPNTR_STRIDE1(model->stride_dwords * 4),
      slot->offset_bytes,
      model->offset_bytes,
   };
   r300_pm4_packet3(b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr, 4);
   relocs->ib_index[0] = r300_pm4_reloc_nop(b, slot->relocation_payload);
   relocs->role[0] = slot->role;
   relocs->ib_index[1] = r300_pm4_reloc_nop(b, model->relocation_payload);
   relocs->role[1] = model->role;

   const uint32_t vf_cntl =
      (params->vertex_count << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
      params->vf_prim | R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST;
   r300_pm4_packet3(b, R300_PACKET3_3D_DRAW_VBUF_2, &vf_cntl, 1);

   return b->error;
}
