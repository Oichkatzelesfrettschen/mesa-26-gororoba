/*
 * SPDX-License-Identifier: MIT
 *
 * Exact-word, bounds, and capacity controls for the neutral two-array
 * BO-fetched R2VB draw body.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG

#include "r300_r2vb_fetch_pass.h"

#include "r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Hand-authored expected stream for the reference case: FLOAT_4 slot
 * positions (4 dwords, stride 4) plus a FLOAT_4 model array, three
 * vertices, POINTS.  The words are written from the packet grammar
 * directly, independent of the emitter, so a copied emitter mistake
 * cannot make both sides agree.
 */
static const struct r300_r2vb_fetch_state reference_state = {
   .vap_prog_stream_cntl = {0x11111111, 0x22222222},
   .vap_prog_stream_cntl_ext = {0xf688f688, 0xf688f688},
   .fetch_dwords = 8,
   .vap_vtx_state_cntl = 0x00005555,
   .vap_vsm_vtx_assm = 0x00000405,
   .vap_out_vtx_fmt = {0x00000001, 0},
   .gb_enable = 0,
   .rs_ip = {0x00000c00},
   .rs_count = 0x00040080,
   .rs_inst_count = 0,
   .rs_inst = {0},
};

static struct r300_r2vb_fetch_pass_params
reference_params(void)
{
   struct r300_r2vb_fetch_pass_params p = {
      .state = &reference_state,
      .stream = {
         {
            .role = R300_R2VB_BO_SLOT,
            .size_dwords = 4,
            .stride_dwords = 4,
            .offset_bytes = 0,
            .bo_size_bytes = 3 * 16,
            .relocation_payload = 0,
         },
         {
            .role = R300_R2VB_BO_MODEL,
            .size_dwords = 4,
            .stride_dwords = 4,
            .offset_bytes = 64,
            .bo_size_bytes = 64 + 3 * 16,
            .relocation_payload = 4,
         },
      },
      .vertex_count = 3,
      .vf_prim = R300_VAP_VF_CNTL__PRIM_POINTS,
   };
   return p;
}

static uint32_t
expected_reference_stream(uint32_t *ib)
{
   uint32_t n = 0;
   ib[n++] = CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_0, 7);
   for (unsigned i = 0; i < 8; i++)
      ib[n++] = reference_state.vap_prog_stream_cntl[i];
   ib[n++] = CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_EXT_0, 7);
   for (unsigned i = 0; i < 8; i++)
      ib[n++] = reference_state.vap_prog_stream_cntl_ext[i];
   ib[n++] = CP_PACKET0(R300_VAP_VTX_SIZE, 0);
   ib[n++] = 8;
   ib[n++] = CP_PACKET0(R300_VAP_VTX_STATE_CNTL, 0);
   ib[n++] = 0x00005555;
   ib[n++] = CP_PACKET0(R300_VAP_VSM_VTX_ASSM, 0);
   ib[n++] = 0x00000405;
   ib[n++] = CP_PACKET0(R300_VAP_OUTPUT_VTX_FMT_0, 1);
   ib[n++] = 0x00000001;
   ib[n++] = 0;
   ib[n++] = CP_PACKET0(R300_GB_ENABLE, 0);
   ib[n++] = 0;
   ib[n++] = CP_PACKET0(R300_RS_IP_0, 7);
   ib[n++] = 0x00000c00;
   for (unsigned i = 1; i < 8; i++)
      ib[n++] = 0;
   ib[n++] = CP_PACKET0(R300_RS_COUNT, 1);
   ib[n++] = 0x00040080;
   ib[n++] = 0;
   ib[n++] = CP_PACKET0(R300_RS_INST_0, 7);
   for (unsigned i = 0; i < 8; i++)
      ib[n++] = 0;
   ib[n++] = CP_PACKET0(R300_VAP_VF_MAX_VTX_INDX, 1);
   ib[n++] = 2;
   ib[n++] = 0;
   ib[n++] = CP_PACKET3(R300_PACKET3_3D_LOAD_VBPNTR, 3);
   ib[n++] = 2 | R300_VC_FORCE_PREFETCH;
   ib[n++] = R300_VBPNTR_SIZE0(16) | R300_VBPNTR_STRIDE0(16) |
             R300_VBPNTR_SIZE1(16) | R300_VBPNTR_STRIDE1(16);
   ib[n++] = 0;
   ib[n++] = 64;
   ib[n++] = 0xc0001000;
   ib[n++] = 0;
   ib[n++] = 0xc0001000;
   ib[n++] = 4;
   ib[n++] = CP_PACKET3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
   ib[n++] = (3u << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
             R300_VAP_VF_CNTL__PRIM_POINTS |
             R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST;
   return n;
}

static void
test_exact_words_and_reloc_sites(void)
{
   uint32_t expected[R300_R2VB_FETCH_PASS_DWORDS];
   assert(expected_reference_stream(expected) == R300_R2VB_FETCH_PASS_DWORDS);

   uint32_t ib[R300_R2VB_FETCH_PASS_DWORDS];
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   struct r300_r2vb_fetch_pass_params p = reference_params();
   struct r300_r2vb_fetch_pass_relocs relocs;
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == 0);
   uint32_t count = 0;
   assert(r300_pm4_builder_finish(&b, &count) == 0);
   assert(count == R300_R2VB_FETCH_PASS_DWORDS);
   assert(memcmp(ib, expected, sizeof(expected)) == 0);

   /* Each reported site is the payload dword behind a reloc NOP header
    * and carries its stream's role.
    */
   assert(relocs.role[0] == R300_R2VB_BO_SLOT);
   assert(relocs.role[1] == R300_R2VB_BO_MODEL);
   for (unsigned i = 0; i < 2; i++) {
      const uint32_t idx = relocs.ib_index[i];
      assert(idx > 0 && idx < count);
      assert(ib[idx - 1] == 0xc0001000u);
   }
   assert(ib[relocs.ib_index[0]] == 0);
   assert(ib[relocs.ib_index[1]] == 4);

   /* Determinism: a second emission is byte-identical. */
   uint32_t ib2[R300_R2VB_FETCH_PASS_DWORDS];
   struct r300_pm4_builder b2;
   r300_pm4_builder_init(&b2, ib2, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b2, &p, &relocs) == 0);
   assert(memcmp(ib, ib2, sizeof(ib)) == 0);
}

static void
test_bounds_reject(void)
{
   uint32_t ib[R300_R2VB_FETCH_PASS_DWORDS];
   struct r300_pm4_builder b;
   struct r300_r2vb_fetch_pass_relocs relocs;

   /* Last byte one past the BO refuses without writing. */
   struct r300_r2vb_fetch_pass_params p = reference_params();
   p.stream[0].bo_size_bytes = 3 * 16 - 1;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   memset(ib, 0, sizeof(ib));
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -ERANGE);
   assert(b.count == 0 && ib[0] == 0);

   /* The exact bound passes. */
   p.stream[0].bo_size_bytes = 3 * 16;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == 0);

   /* An offset pushing the last byte past the BO refuses. */
   p = reference_params();
   p.stream[1].offset_bytes = 65;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -ERANGE);

   /* Zero vertices, a count past the 16-bit VAP_VF_CNTL field, a stride
    * below the fetch size, an oversized VBPNTR field, and a VTX_SIZE
    * that disagrees with the stream sum each refuse as -EINVAL.
    */
   p = reference_params();
   p.vertex_count = 0;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -EINVAL);

   p = reference_params();
   p.vertex_count = R300_PM4_VTX_COUNT_LIMIT + 1;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -EINVAL);

   /* The largest encodable count passes the count gate and reaches the
    * fixed packet body; its vertex-index range still ends at count - 1. */
   p = reference_params();
   p.vertex_count = R300_PM4_VTX_COUNT_LIMIT;
   p.stream[0].bo_size_bytes = (uint64_t)p.vertex_count * 16;
   p.stream[1].bo_size_bytes = 64 + (uint64_t)p.vertex_count * 16;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == 0);
   assert(ib[R300_R2VB_FETCH_PASS_DWORDS - 1] ==
          ((R300_PM4_VTX_COUNT_LIMIT <<
            R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
           R300_VAP_VF_CNTL__PRIM_POINTS |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST));

   p = reference_params();
   p.stream[0].stride_dwords = 3;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -EINVAL);

   p = reference_params();
   p.stream[1].size_dwords = 0x100;
   p.stream[1].stride_dwords = 0x100;
   p.stream[1].bo_size_bytes = 1u << 20;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -EINVAL);

   struct r300_r2vb_fetch_state bad_state = reference_state;
   bad_state.fetch_dwords = 7;
   p = reference_params();
   p.state = &bad_state;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -EINVAL);
}

static void
test_capacity_is_all_or_nothing(void)
{
   struct r300_r2vb_fetch_pass_relocs relocs;
   struct r300_r2vb_fetch_pass_params p = reference_params();
   uint32_t ib[R300_R2VB_FETCH_PASS_DWORDS];

   /* One dword short takes none of the body. */
   memset(ib, 0, sizeof(ib));
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, ib, R300_R2VB_FETCH_PASS_DWORDS - 1);
   assert(r300_r2vb_fetch_pass_emit(&b, &p, &relocs) == -ENOSPC);
   assert(b.count == 0);
   for (unsigned i = 0; i < R300_R2VB_FETCH_PASS_DWORDS; i++)
      assert(ib[i] == 0);
}

int
main(void)
{
   test_exact_words_and_reloc_sites();
   test_bounds_reject();
   test_capacity_is_all_or_nothing();
   printf("r300_r2vb_fetch_pass_test: exact words, bounds, and capacity "
          "controls held\n");
   return 0;
}
