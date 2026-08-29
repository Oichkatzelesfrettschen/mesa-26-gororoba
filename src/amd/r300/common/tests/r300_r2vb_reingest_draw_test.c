/*
 * SPDX-License-Identifier: MIT
 *
 * Exact-stream and refusal coverage for the neutral re-ingest draw body.
 */

#include "r300_r2vb_reingest_draw.h"

#include "r300_pm4_builder.h"
#include "r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STREAM_CNTL_WORD (R300_DATA_TYPE_FLOAT_4 | R300_LAST_VEC)
#define STREAM_EXT_WORD 0x00f688u

static struct r300_r2vb_reingest_draw_params bare_params(void)
{
   return (struct r300_r2vb_reingest_draw_params){
      .stream_cntl_word = STREAM_CNTL_WORD,
      .stream_cntl_ext_word = STREAM_EXT_WORD,
      .vertex_count = 5,
      .vf_prim = R300_VAP_VF_CNTL__PRIM_POINTS,
      .vertex_offset_bytes = 64,
      .vertex_bo_size_bytes = 64 + 5 * 16,
      .vertex_relocation_payload = 8,
   };
}

/* The bare tail is the exact 18-dword stream the emitter documents:
 * stream declaration pair, VTX_SIZE, the complete index-range run,
 * the one-array LOAD_VBPNTR with its NOP relocation, and DRAW_VBUF_2.
 */
static void test_bare_tail_exact_stream(void)
{
   struct r300_r2vb_reingest_draw_params params = bare_params();
   uint32_t words[18];
   struct r300_pm4_builder b;
   struct r300_r2vb_reingest_draw_relocs relocs;
   r300_pm4_builder_init(&b, words, 18);

   assert(r300_r2vb_reingest_draw_dwords(&params) == 18);
   assert(r300_r2vb_reingest_draw_emit(&b, &params, &relocs) == 0);
   uint32_t count = 0;
   assert(r300_pm4_builder_finish(&b, &count) == 0);
   assert(count == 18);

   assert(words[0] == CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_0, 0));
   assert(words[1] == STREAM_CNTL_WORD);
   assert(words[2] == CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_EXT_0, 0));
   assert(words[3] == STREAM_EXT_WORD);
   assert(words[4] == CP_PACKET0(R300_VAP_VTX_SIZE, 0));
   assert(words[5] == 4);
   assert(words[6] == CP_PACKET0(R300_VAP_VF_MAX_VTX_INDX, 1));
   assert(words[7] == 4);
   assert(words[8] == 0);
   assert(words[9] == CP_PACKET3(R300_PACKET3_3D_LOAD_VBPNTR, 3));
   assert(words[10] == (1 | R300_VC_FORCE_PREFETCH));
   assert(words[11] == (4 | (4 << 8)));
   assert(words[12] == 64);
   assert(words[13] == 0);
   assert(words[14] == CP_PACKET3(R300_PM4_PACKET3_NOP, 0));
   assert(words[15] == 8);
   assert(words[16] == CP_PACKET3(R300_PACKET3_3D_DRAW_VBUF_2, 0));
   assert(words[17] ==
          ((5u << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
           R300_VAP_VF_CNTL__PRIM_POINTS |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST));

   assert(relocs.color_ib_index == R300_PM4_NO_INDEX);
   assert(relocs.vertex_ib_index == 15);
}

/* Every gate on: the block order and per-block headers hold, the reloc
 * indices land where the layout predicts, and the emission is
 * deterministic.
 */
static void test_all_gates_layout(void)
{
   const struct r300_r2vb_reingest_redirect redirect = {
      .color_pitch_word = 0x1234,
      .scissor_tl_word = 0x5678,
      .scissor_br_word = 0x9abc,
      .relocation_payload = 4,
   };
   const struct r300_r2vb_reingest_rs_block rs = {
      .gb_enable = 0x11,
      .ip = { 1, 2 },
      .count_word = 0x22,
      .inst_count_word = 1,
      .inst = { 3, 4 },
      .table_len = 2,
   };
   struct r300_r2vb_reingest_draw_params params = bare_params();
   params.redirect = &redirect;
   params.position_only_output = true;
   params.out_vtx_fmt0 = 0x1;
   params.out_vtx_fmt1 = 0x0;
   params.viewport_identity = true;
   params.vte_w0 = true;
   params.vte_restore_word = 0x300;
   params.vertex_assembly = true;
   params.vtx_state_cntl = 0x5;
   params.vsm_vtx_assm = 0x6;
   params.polygon_offset_clear = true;
   params.stream_control_clear = true;
   params.fog_stipple_clear = true;
   params.rs = &rs;

   /* 18 + 9 + 3 + 7 + 4 + 3 + 5 + 8 + 6 + (7 + 2 * 2) = 74. */
   const uint32_t total = r300_r2vb_reingest_draw_dwords(&params);
   assert(total == 74);

   uint32_t words[74];
   struct r300_pm4_builder b;
   struct r300_r2vb_reingest_draw_relocs relocs;
   r300_pm4_builder_init(&b, words, total);
   assert(r300_r2vb_reingest_draw_emit(&b, &params, &relocs) == 0);
   uint32_t count = 0;
   assert(r300_pm4_builder_finish(&b, &count) == 0);
   assert(count == total);

   /* Redirect block leads: COLOROFFSET0 zero, its relocation, pitch,
    * scissor pair. */
   assert(words[0] == CP_PACKET0(R300_RB3D_COLOROFFSET0, 0));
   assert(words[1] == 0);
   assert(words[2] == CP_PACKET3(R300_PM4_PACKET3_NOP, 0));
   assert(words[3] == 4);
   assert(relocs.color_ib_index == 3);
   assert(words[4] == CP_PACKET0(R300_RB3D_COLORPITCH0, 0));
   assert(words[5] == 0x1234);
   assert(words[6] == CP_PACKET0(R300_SC_SCISSORS_TL, 1));
   assert(words[7] == 0x5678);
   assert(words[8] == 0x9abc);

   /* Block headers at their computed offsets: output format (9),
    * viewport (12), VTE override (19), assembly (21), polygon offset
    * (24), stream control (29), fog/stipple (37), RS block (43). */
   assert(words[9] == CP_PACKET0(R300_VAP_OUTPUT_VTX_FMT_0, 1));
   assert(words[12] == CP_PACKET0(R300_SE_VPORT_XSCALE, 5));
   assert(words[13] == 0x3f800000u);
   assert(words[19] == CP_PACKET0(R300_VAP_VTE_CNTL, 0));
   assert(words[20] ==
          (R300_VTX_XY_FMT | R300_VTX_Z_FMT | R300_VTX_W0_FMT));
   assert(words[21] == CP_PACKET0(R300_VAP_VTX_STATE_CNTL, 1));
   assert(words[24] == CP_PACKET0(R300_SU_POLY_OFFSET_FRONT_SCALE, 3));
   assert(words[29] == CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_1, 6));
   assert(words[37] == CP_PACKET0(R300_GA_TRIANGLE_STIPPLE, 0));
   assert(words[39] == CP_PACKET0(R300_GA_FOG_SCALE, 0));
   assert(words[41] == CP_PACKET0(R300_GA_FOG_OFFSET, 0));
   assert(words[43] == CP_PACKET0(R300_GB_ENABLE, 0));
   assert(words[44] == 0x11);
   assert(words[45] == CP_PACKET0(R300_RS_IP_0, 1));
   assert(words[48] == CP_PACKET0(R300_RS_COUNT, 1));
   assert(words[51] == CP_PACKET0(R300_RS_INST_0, 1));

   /* Fixed tail begins at 54; the VTE restore closes the stream. */
   assert(words[54] == CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_0, 0));
   assert(relocs.vertex_ib_index == 69);
   assert(words[72] == CP_PACKET0(R300_VAP_VTE_CNTL, 0));
   assert(words[73] == 0x300);

   /* Determinism: a second emission is byte-identical. */
   uint32_t again_words[74];
   struct r300_pm4_builder again;
   struct r300_r2vb_reingest_draw_relocs again_relocs;
   r300_pm4_builder_init(&again, again_words, total);
   assert(r300_r2vb_reingest_draw_emit(&again, &params, &again_relocs) == 0);
   assert(memcmp(words, again_words, total * sizeof(uint32_t)) == 0);
   assert(again_relocs.color_ib_index == relocs.color_ib_index);
   assert(again_relocs.vertex_ib_index == relocs.vertex_ib_index);
}

static void expect_refusal(struct r300_r2vb_reingest_draw_params params)
{
   uint32_t words[80];
   struct r300_pm4_builder b;
   struct r300_r2vb_reingest_draw_relocs relocs;
   r300_pm4_builder_init(&b, words, 80);
   assert(r300_r2vb_reingest_draw_emit(&b, &params, &relocs) == -EINVAL);
   assert(b.count == 0);
}

static void test_refusals(void)
{
   struct r300_r2vb_reingest_draw_params params = bare_params();
   struct r300_r2vb_reingest_draw_relocs relocs;

   params.vertex_count = 0;
   expect_refusal(params);

   params = bare_params();
   params.vertex_count = R300_PM4_VTX_COUNT_LIMIT + 1;
   params.vertex_bo_size_bytes = UINT64_MAX;
   expect_refusal(params);

   /* The largest VAP_VF_CNTL count remains encodable when the source BO
    * covers the complete contiguous stream. */
   params = bare_params();
   params.vertex_count = R300_PM4_VTX_COUNT_LIMIT;
   params.vertex_bo_size_bytes = 64 +
                                 (uint64_t)params.vertex_count * 16;
   uint32_t max_words[18];
   struct r300_pm4_builder max_builder;
   r300_pm4_builder_init(&max_builder, max_words, 18);
   assert(r300_r2vb_reingest_draw_emit(&max_builder, &params, &relocs) == 0);
   assert(max_words[17] ==
          ((R300_PM4_VTX_COUNT_LIMIT <<
            R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
           R300_VAP_VF_CNTL__PRIM_POINTS |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST));

   /* The last fetched byte lies one past the BO. */
   params = bare_params();
   params.vertex_bo_size_bytes = 64 + 5 * 16 - 1;
   expect_refusal(params);

   /* A large offset times a large count stays refused through the
    * 64-bit bound rather than wrapping. */
   params = bare_params();
   params.vertex_offset_bytes = UINT32_MAX;
   params.vertex_count = R300_PM4_VTX_COUNT_LIMIT;
   params.vertex_bo_size_bytes = UINT32_MAX;
   expect_refusal(params);

   struct r300_r2vb_reingest_rs_block rs = { .table_len = 0 };
   params = bare_params();
   params.rs = &rs;
   expect_refusal(params);
   rs.table_len = 9;
   expect_refusal(params);

   /* One dword short: the whole-run reservation refuses before any
    * word lands. */
   params = bare_params();
   uint32_t words[17];
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, 17);
   assert(r300_r2vb_reingest_draw_emit(&b, &params, &relocs) == -ENOSPC);
   assert(b.count == 0);
   uint32_t count = 1;
   assert(r300_pm4_builder_finish(&b, &count) == -ENOSPC);
   assert(count == 0);

   /* An error-latched builder stays a no-op. */
   uint32_t latched_words[32];
   struct r300_pm4_builder latched;
   r300_pm4_builder_init(&latched, latched_words, 32);
   latched.error = -EINVAL;
   assert(r300_r2vb_reingest_draw_emit(&latched, &params, &relocs) ==
          -EINVAL);
   assert(latched.count == 0);

   /* The public dword count is checked: a null descriptor or an RS
    * table length outside 1..8 (including one that would wrap the
    * 7 + 2 * table_len arithmetic) reads 0, which no valid emission
    * can produce. */
   assert(r300_r2vb_reingest_draw_dwords(NULL) == 0);
   struct r300_r2vb_reingest_rs_block bad_rs = { .table_len = 0 };
   params = bare_params();
   params.rs = &bad_rs;
   assert(r300_r2vb_reingest_draw_dwords(&params) == 0);
   bad_rs.table_len = 9;
   assert(r300_r2vb_reingest_draw_dwords(&params) == 0);
   bad_rs.table_len = 0x80000000u;
   assert(r300_r2vb_reingest_draw_dwords(&params) == 0);
   params.rs = NULL;
   assert(r300_r2vb_reingest_draw_dwords(&params) == 18);

   /* A null builder is refused outright. */
   assert(r300_r2vb_reingest_draw_emit(NULL, &params, &relocs) == -EINVAL);

   /* Null destinations are refused without touching the builder. */
   uint32_t null_words[32];
   struct r300_pm4_builder null_builder;
   r300_pm4_builder_init(&null_builder, null_words, 32);
   assert(r300_r2vb_reingest_draw_emit(&null_builder, NULL, &relocs) ==
          -EINVAL);
   assert(null_builder.count == 0);
   r300_pm4_builder_init(&null_builder, null_words, 32);
   assert(r300_r2vb_reingest_draw_emit(&null_builder, &params, NULL) ==
          -EINVAL);
   assert(null_builder.count == 0);
}

int main(void)
{
   test_bare_tail_exact_stream();
   test_all_gates_layout();
   test_refusals();
   printf("r300_r2vb_reingest_draw_test: streams and refusals hold\n");
   return 0;
}
