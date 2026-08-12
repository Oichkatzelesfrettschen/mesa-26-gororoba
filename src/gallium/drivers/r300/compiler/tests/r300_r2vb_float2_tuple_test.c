/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 *
 * Six-dword FLOAT_4 + FLOAT_2 producer tuple, captured without submit
 * through the gated pure chain, plus the standing-exclusion proof that
 * every ungated entry declines the F32_2 source.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "amd/r300/common/r300_r2vb_source_contract.h"
#include "r300_r2vb_plan.h"
#include "r300_reg.h"

static void
test_gated_six_dword_tuple(void)
{
   /* Stride 8 bytes, tightly packed R32G32: two physical dwords fetched,
    * XY01 expansion, six dwords total beside the FLOAT_4 slot stream.
    */
   struct r300_r2vb_producer_streams streams;
   assert(r300_r2vb_producer_streams_init_gated(
      0, 0, 8, PIPE_FORMAT_R32G32_FLOAT, 0, true, &streams));
   assert(streams.stream[1].size_dwords == 2);
   assert(streams.fetch_dwords == 6);

   struct r300_r2vb_producer_fetch fetch;
   assert(r300_r2vb_producer_fetch_init_gated(&streams, 64, 64 * 16, 64 * 8,
                                              true, &fetch));
   assert(fetch.vap_vtx_size == 6);

   struct r300_r2vb_producer_interface interface;
   assert(r300_r2vb_producer_interface_init_gated(&fetch, 0, 6, true,
                                                  &interface));
   assert(interface.vap_vtx_size == 6);

   /* Element pair: FLOAT_4 slot at destination 0, FLOAT_2 model at
    * destination 6 carrying LAST_VEC.
    */
   uint32_t e0 = interface.prog_stream_cntl[0] & 0xffff;
   uint32_t e1 = interface.prog_stream_cntl[0] >> 16;
   assert((e0 & 0xf) == R300_DATA_TYPE_FLOAT_4);
   assert(!(e0 & R300_LAST_VEC));
   assert((e1 & 0xf) == R300_DATA_TYPE_FLOAT_2);
   assert(((e1 >> R300_DST_VEC_LOC_SHIFT) & 0x1f) == 6);
   assert(e1 & R300_LAST_VEC);

   /* The model swizzle synthesizes the XY01 expansion: X and Y from
    * memory, Z from FP_ZERO, W from FP_ONE, full write mask.
    */
   uint32_t model_swizzle = interface.prog_stream_cntl_ext[0] >> 16;
   assert(((model_swizzle >> 0) & 7) == R300_SWIZZLE_SELECT_X);
   assert(((model_swizzle >> 3) & 7) == R300_SWIZZLE_SELECT_Y);
   assert(((model_swizzle >> 6) & 7) == R300_SWIZZLE_SELECT_FP_ZERO);
   assert(((model_swizzle >> 9) & 7) == R300_SWIZZLE_SELECT_FP_ONE);
   assert(((model_swizzle >> 12) & 0xf) == 0xf);

   /* The contract record agrees with the assembled chain: 64 vertices at
    * stride 8 inside an exact 512-byte allocation.
    */
   struct r300_r2vb_source_contract contract;
   assert(r300_r2vb_source_contract_init(R300_VERTEX_FORMAT_F32_2, true,
                                         64 * 8, 0, 0, 8, 0, 64,
                                         &contract));
   assert(contract.vap_vtx_size_dwords == 6);
}

static void
test_ungated_entries_decline(void)
{
   /* The standing exclusion is structural: no production entry reads the
    * gate, so the unsuffixed chain and the gated chain with the flag low
    * both decline the F32_2 source even when every standing-route gate is
    * armed in the environment.
    */
   struct r300_r2vb_producer_streams streams;
   assert(!r300_r2vb_producer_streams_init(0, 0, 8, PIPE_FORMAT_R32G32_FLOAT,
                                           0, &streams));
   assert(!r300_r2vb_producer_streams_init_gated(
      0, 0, 8, PIPE_FORMAT_R32G32_FLOAT, 0, false, &streams));

   assert(r300_r2vb_producer_streams_init_gated(
      0, 0, 8, PIPE_FORMAT_R32G32_FLOAT, 0, true, &streams));
   struct r300_r2vb_producer_fetch fetch;
   assert(!r300_r2vb_producer_fetch_init(&streams, 64, 64 * 16, 64 * 8,
                                         &fetch));
   assert(r300_r2vb_producer_fetch_init_gated(&streams, 64, 64 * 16, 64 * 8,
                                              true, &fetch));
   struct r300_r2vb_producer_interface interface;
   assert(!r300_r2vb_producer_interface_init(&fetch, 0, 6, &interface));
}

static void
test_known_bad_fixtures_decline(void)
{
   /* Stride under one record overlaps fetches. */
   struct r300_r2vb_producer_streams streams;
   assert(!r300_r2vb_producer_streams_init_gated(
      0, 0, 4, PIPE_FORMAT_R32G32_FLOAT, 0, true, &streams));

   /* F32_1 stays outside the producer contract even gated. */
   assert(!r300_r2vb_producer_streams_init_gated(
      0, 0, 4, PIPE_FORMAT_R32_FLOAT, 0, true, &streams));

   /* An allocation one byte short of the model extent declines. */
   assert(r300_r2vb_producer_streams_init_gated(
      0, 0, 8, PIPE_FORMAT_R32G32_FLOAT, 0, true, &streams));
   struct r300_r2vb_producer_fetch fetch;
   assert(!r300_r2vb_producer_fetch_init_gated(&streams, 64, 64 * 16,
                                               64 * 8 - 1, true, &fetch));

   /* The gate parser opens on the exact value only. */
   assert(r300_r2vb_float2_source_gate_value("1"));
   assert(!r300_r2vb_float2_source_gate_value(NULL));
   assert(!r300_r2vb_float2_source_gate_value(""));
   assert(!r300_r2vb_float2_source_gate_value("0"));
   assert(!r300_r2vb_float2_source_gate_value("true"));
   assert(!r300_r2vb_float2_source_gate_value("11"));
}

int
main(void)
{
   test_gated_six_dword_tuple();
   test_ungated_entries_decline();
   test_known_bad_fixtures_decline();
   printf("r300_r2vb_float2_tuple_test: all checks passed\n");
   return 0;
}
