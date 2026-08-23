/*
 * SPDX-License-Identifier: MIT
 *
 * The compute identity carrier pass: its structure (three relocation
 * sites over the fetched body), the pinned reference identity, the
 * count and alignment refusals, the output-bound refusal, and the host
 * model's agreement with the CPU compute route inside the FP24 window
 * and its -EDOM refusal outside it.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_compute_identity_carrier.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_vertex_format.h"
#include "tests/r300_compute_identity_carrier_digests.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void
test_reference_structure_and_digest(void)
{
   struct r300_r2vb_fetched_producer_ib pass;
   assert(r300_compute_identity_carrier_reference_emit(&pass) == 0);
   assert(pass.ib_size_dwords != 0);
   assert(pass.reloc_site_count == R300_R2VB_FETCHED_PRODUCER_SLOT_COUNT);
   assert(r300_r2vb_fetched_producer_validate_reloc_sites(&pass) == 0);
   /* Deterministic: a second emission is byte-identical. */
   struct r300_r2vb_fetched_producer_ib again;
   assert(r300_compute_identity_carrier_reference_emit(&again) == 0);
   assert(again.ib_size_dwords == pass.ib_size_dwords);
   assert(memcmp(again.ib, pass.ib, pass.ib_size_dwords * 4) == 0);
   r300_r2vb_fetched_producer_release(&again);

   char hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(pass.ib, pass.ib_size_dwords, hex);
   if (pass.ib_size_dwords != R300_COMPUTE_IDENTITY_CARRIER_IB_DWORDS ||
       strcmp(hex, R300_COMPUTE_IDENTITY_CARRIER_IB_BLAKE3) != 0) {
      fprintf(stderr, "reference pass: %u dwords, blake3 %s\n",
              pass.ib_size_dwords, hex);
   }
   assert(pass.ib_size_dwords == R300_COMPUTE_IDENTITY_CARRIER_IB_DWORDS);
   assert(strcmp(hex, R300_COMPUTE_IDENTITY_CARRIER_IB_BLAKE3) == 0);
   /* The reference pass is the fetched producer's reference geometry
    * with sixteen records in place of three: it differs from the F32_4
    * fetched reference producer, whose count is three. */
   struct r300_r2vb_fetched_producer_ib three;
   assert(r300_r2vb_fetched_producer_reference_emit(
             R300_VERTEX_FORMAT_F32_4, &three) == 0);
   assert(three.ib_size_dwords == pass.ib_size_dwords);
   assert(memcmp(three.ib, pass.ib, pass.ib_size_dwords * 4) != 0);
   r300_r2vb_fetched_producer_release(&three);
   r300_r2vb_fetched_producer_release(&pass);
}

static void
test_refusals(void)
{
   struct r300_r2vb_fetched_producer_ib pass;
   struct r300_compute_identity_carrier_params params = {
      .record_count = 4,
      .carrier_bo_size_bytes = 4096,
      .source_bo_size_bytes = 4096,
      .slot_bo_size_bytes = 4096,
   };
   assert(r300_compute_identity_carrier_emit(&params, &pass) == 0);
   r300_r2vb_fetched_producer_release(&pass);

   params.record_count = 0;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -EINVAL);
   params.record_count = R300_COMPUTE_IDENTITY_CARRIER_MAX_RECORDS + 1;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -EINVAL);
   params.record_count = 4;

   params.carrier_offset = 16;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -EINVAL);
   params.carrier_offset = 0;
   params.source_offset = 2;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -EINVAL);
   params.source_offset = 0;

   /* The output bound: four records fill 64 bytes; an output BO holding
    * 63 bytes past the offset refuses, 64 admits; an odd count's pitch
    * rounding widens the bound by one slot, so three records need 64
    * bytes and refuse at their own 48. */
   params.carrier_bo_size_bytes = 63;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -ERANGE);
   params.carrier_bo_size_bytes = 64;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == 0);
   r300_r2vb_fetched_producer_release(&pass);
   params.record_count = 3;
   params.carrier_bo_size_bytes = 48;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -ERANGE);
   params.carrier_bo_size_bytes = 64;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == 0);
   r300_r2vb_fetched_producer_release(&pass);
   params.carrier_bo_size_bytes = 4096;
   params.record_count = 4;

   /* The input array bound is the producer's: four 16-byte records need
    * 64 bytes past the source offset. */
   params.source_bo_size_bytes = 63;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -ERANGE);
   params.source_bo_size_bytes = 4096;
   params.slot_bo_size_bytes = 63;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == -ERANGE);
   params.slot_bo_size_bytes = 4096;

   /* The ceiling count emits with a one-row layout of that many slots. */
   params.record_count = R300_COMPUTE_IDENTITY_CARRIER_MAX_RECORDS;
   params.carrier_bo_size_bytes =
      (uint64_t)R300_COMPUTE_IDENTITY_CARRIER_MAX_RECORDS * 16;
   params.source_bo_size_bytes = params.carrier_bo_size_bytes;
   params.slot_bo_size_bytes = params.carrier_bo_size_bytes;
   assert(r300_compute_identity_carrier_emit(&params, &pass) == 0);
   r300_r2vb_fetched_producer_release(&pass);
}

/* The host model equals the CPU route's bit copy inside the FP24
 * fixed-point window and refuses outside it. */
static void
test_expected_matches_bit_copy_inside_window(void)
{
   uint32_t input[16];
   uint32_t out[16];
   const float in_window[16] = { 0.0f, 1.0f, 2.0f, 0.5f,   4.0f, 8.0f,
                                 16.0f, 3.0f, 0.25f, 12.0f, 1.5f, 63.0f,
                                 32.0f, 0.75f, 5.0f, 20.0f };
   memcpy(input, in_window, sizeof(input));
   memset(out, 0xa5, sizeof(out));
   assert(r300_compute_identity_carrier_expected(input, 4, out, 16) == 0);
   assert(memcmp(out, input, sizeof(input)) == 0);

   /* Outside: a negative component, a NaN payload, and an off-grid
    * mantissa each refuse with -EDOM before any write. */
   const uint32_t outside[3] = { 0xbf800000u, 0x7fc00777u, 0x3f800001u };
   for (unsigned i = 0; i < 3; i++) {
      memcpy(input, in_window, sizeof(input));
      input[5] = outside[i];
      memset(out, 0xa5, sizeof(out));
      assert(r300_compute_identity_carrier_expected(input, 4, out, 16) ==
             -EDOM);
      assert(out[0] == 0xa5a5a5a5u);
   }
   memcpy(input, in_window, sizeof(input));
   assert(r300_compute_identity_carrier_expected(input, 4, out, 15) ==
          -ENOSPC);
   assert(r300_compute_identity_carrier_expected(input, 0, out, 16) ==
          -EINVAL);
   assert(r300_compute_identity_carrier_expected(NULL, 4, out, 16) ==
          -EINVAL);
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--print-digest") == 0) {
      struct r300_r2vb_fetched_producer_ib pass;
      assert(r300_compute_identity_carrier_reference_emit(&pass) == 0);
      char hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      r300_triangle_ib_digest_hex(pass.ib, pass.ib_size_dwords, hex);
      printf("%u %s\n", pass.ib_size_dwords, hex);
      r300_r2vb_fetched_producer_release(&pass);
      return 0;
   }
   test_reference_structure_and_digest();
   test_refusals();
   test_expected_matches_bit_copy_inside_window();
   printf("r300_compute_identity_carrier_test: all checks passed\n");
   return 0;
}
