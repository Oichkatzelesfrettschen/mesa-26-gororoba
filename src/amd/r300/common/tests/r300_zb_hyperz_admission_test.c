/*
 * SPDX-License-Identifier: MIT
 *
 * The admission table mirrors r300_packet0_check and r300_packet3_check
 * row for row: every gated write refuses without ownership and admits
 * with it, an ungated value of a gated register admits either way, the
 * stream walker frames packets the way the kernel does, and the depth
 * state the driver emits today carries no HyperZ write.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_zb_hyperz_admission.h"
#include "r300_zb_depth_state.h"
#include "r300_pm4_builder.h"
#include "r300_reg.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
test_rows(void)
{
   assert(r300_zb_hyperz_rows_self_check() == 0);
   uint32_t n = 0u;
   const struct r300_zb_hyperz_row *rows = r300_zb_hyperz_rows(&n);
   assert(n == 9u);
   uint32_t packet0 = 0u, packet3 = 0u, silent = 0u;
   for (uint32_t i = 0; i < n; i++) {
      if (rows[i].kind == R300_ZB_HYPERZ_ROW_PACKET0)
         packet0++;
      else
         packet3++;
      if (rows[i].disposition == R300_ZB_HYPERZ_KERNEL_CLEARS_SILENTLY) {
         silent++;
         assert(rows[i].key == R300_SC_HYPERZ);
      }
   }
   assert(packet0 == 7u && packet3 == 2u && silent == 1u);
   for (unsigned v = 0; v <= R300_ZB_HYPERZ_REFUSE_STREAM; v++)
      assert(r300_zb_hyperz_verdict_name(v) != NULL);
}

/* Every PACKET0 row: each gated bit alone refuses unowned and admits
 * owned; the register with its gated bits clear admits unowned. */
static void
test_register_rows(void)
{
   uint32_t n = 0u;
   const struct r300_zb_hyperz_row *rows = r300_zb_hyperz_rows(&n);
   const struct r300_zb_hyperz_row *row = NULL;
   for (uint32_t i = 0; i < n; i++) {
      if (rows[i].kind != R300_ZB_HYPERZ_ROW_PACKET0)
         continue;
      for (uint32_t bit = 0; bit < 32u; bit++) {
         const uint32_t value = 1u << bit;
         if ((value & rows[i].gated_mask) == 0u) {
            assert(r300_zb_hyperz_admit_register(rows[i].key, value,
                                                 R300_ZB_HYPERZ_UNOWNED,
                                                 &row) == R300_ZB_HYPERZ_ADMIT);
            assert(row == NULL);
            continue;
         }
         assert(r300_zb_hyperz_admit_register(rows[i].key, value,
                                              R300_ZB_HYPERZ_UNOWNED, &row) ==
                R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
         assert(row == &rows[i]);
         assert(r300_zb_hyperz_admit_register(rows[i].key, value,
                                              R300_ZB_HYPERZ_OWNED, &row) ==
                R300_ZB_HYPERZ_ADMIT);
         assert(row == &rows[i]);
      }
      assert(r300_zb_hyperz_admit_register(rows[i].key, 0u,
                                           R300_ZB_HYPERZ_UNOWNED, &row) ==
             R300_ZB_HYPERZ_ADMIT);
   }
   /* The disabled ZB_BW_CNTL the depth state writes admits unowned. */
   assert(r300_zb_hyperz_admit_register(R300_ZB_BW_CNTL,
                                        R300_HIZ_DISABLE |
                                           R300_FAST_FILL_DISABLE,
                                        R300_ZB_HYPERZ_UNOWNED,
                                        NULL) == R300_ZB_HYPERZ_ADMIT);
   /* A register outside the table admits at any value. */
   assert(r300_zb_hyperz_admit_register(R300_ZB_CNTL, 0xffffffffu,
                                        R300_ZB_HYPERZ_UNOWNED,
                                        NULL) == R300_ZB_HYPERZ_ADMIT);
}

/* The stream walker: the driver's depth state admits unowned; each
 * HyperZ write inserted into it refuses at its own index; ONE_REG_WR runs
 * and multi-register runs resolve the register the kernel resolves;
 * PACKET3 clears refuse by opcode; malformed headers refuse the stream. */
static void
test_stream(void)
{
   uint32_t words[128];
   struct r300_pm4_builder b;
   struct r300_zb_hyperz_site site;

   r300_pm4_builder_init(&b, words, 128u);
   const struct r300_zb_depth_state_params depth = {
      .pitch_pixels = 64u,
      .depth_format = R300_DEPTHFORMAT_16BIT_INT_Z,
      .depth_offset_bytes = 0u,
      .depth_relocation_payload = 0u,
      .depth_function = R300_ZS_LESS,
      .depth_write = true,
   };
   assert(r300_zb_depth_state_emit(&b, &depth, NULL) == 0);
   uint32_t n = 0u;
   assert(r300_pm4_builder_finish(&b, &n) == 0);
   assert(r300_zb_hyperz_admit_stream(words, n, R300_ZB_HYPERZ_UNOWNED,
                                      &site) == R300_ZB_HYPERZ_ADMIT);

   /* Append each gated write and expect a refusal at its payload. */
   uint32_t rows_n = 0u;
   const struct r300_zb_hyperz_row *rows = r300_zb_hyperz_rows(&rows_n);
   for (uint32_t i = 0; i < rows_n; i++) {
      uint32_t stream[160];
      memcpy(stream, words, n * sizeof(uint32_t));
      r300_pm4_builder_init(&b, stream, 160u);
      b.count = n;
      uint32_t expect_index;
      if (rows[i].kind == R300_ZB_HYPERZ_ROW_PACKET0) {
         const uint32_t value = rows[i].gated_mask & -rows[i].gated_mask;
         expect_index = n + 1u;
         r300_pm4_reg(&b, rows[i].key, value);
      } else {
         const uint32_t payload[1] = { 0u };
         expect_index = n;
         r300_pm4_packet3(&b, rows[i].key, payload, 1u);
      }
      uint32_t m = 0u;
      assert(r300_pm4_builder_finish(&b, &m) == 0);
      assert(r300_zb_hyperz_admit_stream(stream, m, R300_ZB_HYPERZ_UNOWNED,
                                         &site) ==
             R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
      assert(site.row == &rows[i]);
      assert(site.ib_index == expect_index);
      assert(r300_zb_hyperz_admit_stream(stream, m, R300_ZB_HYPERZ_OWNED,
                                         &site) == R300_ZB_HYPERZ_ADMIT);
   }

   /* A multi-register run reaching ZB_BW_CNTL from ZB_ZCACHE_CTLSTAT
    * (0x4f18, 0x4f1c): the second payload is the gated one. */
   {
      const uint32_t run[2] = { 0u, R300_HIZ_ENABLE };
      r300_pm4_builder_init(&b, words, 128u);
      r300_pm4_packet0(&b, R300_ZB_ZCACHE_CTLSTAT, run, 2u);
      uint32_t m = 0u;
      assert(r300_pm4_builder_finish(&b, &m) == 0);
      assert(r300_zb_hyperz_admit_stream(words, m, R300_ZB_HYPERZ_UNOWNED,
                                         &site) ==
             R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
      assert(site.ib_index == 2u && site.reg_or_opcode == R300_ZB_BW_CNTL);
   }
   /* ONE_REG_WR: three payloads to ZB_BW_CNTL, the third gated. */
   {
      words[0] = CP_PACKET0(R300_ZB_BW_CNTL, 2) | RADEON_ONE_REG_WR;
      words[1] = 0u;
      words[2] = 0u;
      words[3] = R300_FAST_FILL_ENABLE;
      assert(r300_zb_hyperz_admit_stream(words, 4u, R300_ZB_HYPERZ_UNOWNED,
                                         &site) ==
             R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
      assert(site.ib_index == 3u);
      /* The same run read as consecutive registers lands the third
       * payload on 0x4f24 ZB_DEPTHPITCH, outside the table. */
      words[0] = CP_PACKET0(R300_ZB_BW_CNTL, 2);
      assert(r300_zb_hyperz_admit_stream(words, 4u, R300_ZB_HYPERZ_UNOWNED,
                                         &site) == R300_ZB_HYPERZ_ADMIT);
   }
   /* Type-2 fillers are skipped; a header whose count runs past the end
    * and a type-1 header refuse the stream. */
   {
      words[0] = 0x80000000u;
      words[1] = CP_PACKET0(R300_ZB_HIZ_OFFSET, 0);
      words[2] = 0x1000u;
      assert(r300_zb_hyperz_admit_stream(words, 3u, R300_ZB_HYPERZ_UNOWNED,
                                         &site) ==
             R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
      assert(site.ib_index == 2u);
      words[0] = CP_PACKET0(R300_ZB_HIZ_OFFSET, 3);
      assert(r300_zb_hyperz_admit_stream(words, 3u, R300_ZB_HYPERZ_OWNED,
                                         &site) == R300_ZB_HYPERZ_REFUSE_STREAM);
      words[0] = 0x40000000u;
      assert(r300_zb_hyperz_admit_stream(words, 1u, R300_ZB_HYPERZ_OWNED,
                                         &site) == R300_ZB_HYPERZ_REFUSE_STREAM);
      assert(r300_zb_hyperz_admit_stream(NULL, 0u, R300_ZB_HYPERZ_UNOWNED,
                                         &site) == R300_ZB_HYPERZ_ADMIT);
      assert(r300_zb_hyperz_admit_stream(NULL, 1u, R300_ZB_HYPERZ_UNOWNED,
                                         &site) == R300_ZB_HYPERZ_REFUSE_STREAM);
   }
}

int
main(void)
{
   test_rows();
   test_register_rows();
   test_stream();
   printf("r300_zb_hyperz_admission_test: all checks passed\n");
   return 0;
}
