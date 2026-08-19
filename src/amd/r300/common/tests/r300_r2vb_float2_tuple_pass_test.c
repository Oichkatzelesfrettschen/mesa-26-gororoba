/*
 * SPDX-License-Identifier: MIT
 *
 * Host calibration for the FLOAT_4 + FLOAT_2 tuple producer pass: the
 * PSC tuple and XY01 selector words, the six-dword fetch size, the
 * two-array LOAD_VBPNTR framing, the vertex-stream serialization, the
 * XY01 delivery expectation, and the refusal edges.
 */

#include "r300_fragment_binary.h"
#include "r300_r2vb_float2_tuple_pass.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_reg.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond, ...)                                                      \
   do {                                                                       \
      if (!(cond)) {                                                          \
         fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                 \
         fprintf(stderr, __VA_ARGS__);                                        \
         fprintf(stderr, "\n");                                               \
         failures++;                                                          \
      }                                                                       \
   } while (0)

static uint32_t
f32_bits(float f)
{
   uint32_t bits;
   memcpy(&bits, &f, sizeof(bits));
   return bits;
}

/* Scan the IB for a packet0 write of one register value. */
static bool
ib_writes_reg(const struct r300_r2vb_float2_tuple_ib *ib, uint32_t reg,
              uint32_t value)
{
   for (uint32_t i = 0; i + 1 < ib->ib_size_dwords; i++) {
      const uint32_t header = ib->ib[i];
      if ((header & 0xC0000000u) != 0)
         continue;
      const uint32_t base = (header & 0x1FFFu) << 2;
      const uint32_t count = ((header >> 16) & 0x3FFFu) + 1;
      if (reg >= base && reg < base + count * 4 &&
          i + 1 + (reg - base) / 4 < ib->ib_size_dwords &&
          ib->ib[i + 1 + (reg - base) / 4] == value)
         return true;
   }
   return false;
}

static void
test_reference_records_pinned(void)
{
   static const uint32_t bits[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT][2] = {
      { 0x41000000u, 0x3f400000u }, /* 8.0, 0.75 */
      { 0x42600000u, 0x3f800000u }, /* 56.0, 1.0 */
      { 0x4479c000u, 0x40000000u }, /* 999.0, 2.0 */
   };
   for (unsigned v = 0; v < R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT; v++)
      for (unsigned c = 0; c < 2; c++)
         CHECK(f32_bits(r300_r2vb_float2_tuple_reference_records[v][c]) ==
                  bits[v][c],
               "reference record %u.%u drifted", v, c);
}

static void
test_expected_is_xy01_expansion(void)
{
   uint32_t expected[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 4];
   CHECK(r300_r2vb_float2_tuple_reference_expected(
            expected, R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 4) == 0,
         "reference expectation failed");
   for (unsigned v = 0; v < R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT; v++) {
      CHECK(expected[v * 4 + 0] ==
               f32_bits(r300_r2vb_float2_tuple_reference_records[v][0]),
            "slot %u lane x is not the record", v);
      CHECK(expected[v * 4 + 1] ==
               f32_bits(r300_r2vb_float2_tuple_reference_records[v][1]),
            "slot %u lane y is not the record", v);
      CHECK(expected[v * 4 + 2] == 0, "slot %u lane z is not +0.0", v);
      CHECK(expected[v * 4 + 3] == 0x3f800000u,
            "slot %u lane w is not 1.0", v);
   }
   CHECK(r300_r2vb_float2_tuple_reference_expected(expected, 11) == -ENOSPC,
         "short expectation storage not refused");

   static const float off_lattice[1][2] = { { 0.1f, 1.0f } };
   CHECK(r300_r2vb_float2_tuple_expected(off_lattice, 1, expected, 4) ==
            -EDOM,
         "off-lattice model component not refused");
}

static void
test_vertex_stream_layout(void)
{
   uint8_t bytes[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 24];
   CHECK(r300_r2vb_float2_tuple_vertex_stream(
            r300_r2vb_float2_tuple_reference_records,
            R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, bytes,
            (uint32_t)sizeof(bytes)) == 0,
         "vertex stream serialization failed");

   /* Slot 1 position starts at byte 16: (1.5, 0.5, 0, 1). */
   const uint32_t x1 = (uint32_t)bytes[16] | ((uint32_t)bytes[17] << 8) |
                       ((uint32_t)bytes[18] << 16) |
                       ((uint32_t)bytes[19] << 24);
   CHECK(x1 == f32_bits(1.5f), "slot 1 position x is not 1.5");

   /* Model records start at 3 * 16 = 48: record 0 x = 8.0. */
   const uint32_t m0 = (uint32_t)bytes[48] | ((uint32_t)bytes[49] << 8) |
                       ((uint32_t)bytes[50] << 16) |
                       ((uint32_t)bytes[51] << 24);
   CHECK(m0 == f32_bits(8.0f), "model record 0 x is not 8.0");

   CHECK(r300_r2vb_float2_tuple_vertex_stream(
            r300_r2vb_float2_tuple_reference_records,
            R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, bytes,
            (uint32_t)sizeof(bytes) - 1) == -ENOSPC,
         "short vertex storage not refused");
}

static void
test_emission_contract(void)
{
   struct r300_r2vb_float2_tuple_ib pass;
   CHECK(r300_r2vb_float2_tuple_reference_emit(&pass) == 0,
         "reference emission failed");
   CHECK(r300_r2vb_float2_tuple_pass_validate_reloc_sites(&pass) == 0,
         "relocation sites invalid");

   /* The PSC declaration the kernel width predicate decodes: FLOAT_4
    * identity at vector 0, FLOAT_2 XY01 at vector 6 with LAST, and the
    * six-dword fetch.
    */
   const uint32_t cntl0 =
      (R300_DATA_TYPE_FLOAT_4 | (0u << R300_DST_VEC_LOC_SHIFT)) |
      ((R300_DATA_TYPE_FLOAT_2 | (6u << R300_DST_VEC_LOC_SHIFT) |
        R300_LAST_VEC)
       << 16);
   CHECK(ib_writes_reg(&pass, R300_VAP_PROG_STREAM_CNTL_0, cntl0),
         "PSC CNTL_0 does not declare the FLOAT_4 + FLOAT_2 tuple");
   const uint32_t ext0 =
      R300_VAP_SWIZZLE_XYZW | (R300_VAP_SWIZZLE_XY01 << 16);
   CHECK(ib_writes_reg(&pass, R300_VAP_PROG_STREAM_CNTL_EXT_0, ext0),
         "PSC EXT_0 does not carry identity plus XY01");
   CHECK(ib_writes_reg(&pass, R300_VAP_VTX_SIZE,
                       R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS),
         "VAP_VTX_SIZE is not the six-dword tuple fetch");
   /* Known-bad calibration for the scanner itself: a value the stream
    * never writes to this register must not be found, so a payload
    * dword that happens to decode as a header cannot fake a hit.
    */
   CHECK(!ib_writes_reg(&pass, R300_VAP_VTX_SIZE, 0x5a5a5a5au),
         "ib_writes_reg reports a value the stream never writes");

   /* The fetched draw: a two-array LOAD_VBPNTR and a vertex-list POINTS
    * draw, with no embedded-body packet in the stream.
    */
   bool found_vbpntr = false;
   bool found_immd = false;
   for (uint32_t i = 0; i < pass.ib_size_dwords; i++) {
      const uint32_t header = pass.ib[i];
      if ((header & 0xC0000000u) != 0xC0000000u)
         continue;
      if ((header & 0xFF00u) == R300_PACKET3_3D_LOAD_VBPNTR &&
          i + 1 < pass.ib_size_dwords &&
          (pass.ib[i + 1] & 0x1Fu) == 2)
         found_vbpntr = true;
      if ((header & 0xFF00u) == R300_PACKET3_3D_DRAW_IMMD_2)
         found_immd = true;
   }
   CHECK(found_vbpntr, "no two-array LOAD_VBPNTR in the stream");
   CHECK(!found_immd, "an embedded-body draw leaked into the tuple pass");

   r300_r2vb_float2_tuple_pass_release(&pass);
}

/* The burst is member-wise byte identity: member 0 is the reference
 * emission whole, and every later member is the prefix-free emission at
 * its own carrier row, proven here against fresh standalone emissions.
 */
static void
test_burst_member_byte_identity(void)
{
   uint32_t stride = 0;
   CHECK(r300_r2vb_float2_tuple_burst_member_stride_bytes(&stride) == 0 &&
            stride == 64,
         "member stride is the 4-pixel reference row (64 bytes), got %u",
         stride);

   struct r300_r2vb_float2_tuple_burst_ib single;
   CHECK(r300_r2vb_float2_tuple_burst_reference_emit(1, &single) == 0,
         "single-member burst emission failed");
   struct r300_r2vb_float2_tuple_ib reference;
   CHECK(r300_r2vb_float2_tuple_reference_emit(&reference) == 0,
         "reference emission failed");
   CHECK(single.ib_size_dwords == reference.ib_size_dwords &&
            memcmp(single.ib, reference.ib,
                   reference.ib_size_dwords * sizeof(uint32_t)) == 0,
         "a one-member burst is the reference stream byte for byte");
   CHECK(single.reloc_site_count == R300_R2VB_FLOAT2_TUPLE_RELOC_SITES &&
            single.member_start[0] == 0,
         "one-member site and start bookkeeping drifted");
   CHECK(r300_r2vb_float2_tuple_burst_validate_reloc_sites(&single) == 0,
         "one-member burst sites failed validation");

   const uint32_t draws = 4;
   struct r300_r2vb_float2_tuple_burst_ib burst;
   CHECK(r300_r2vb_float2_tuple_burst_reference_emit(draws, &burst) == 0,
         "four-member burst emission failed");
   CHECK(burst.draws == draws &&
            burst.reloc_site_count ==
               draws * R300_R2VB_FLOAT2_TUPLE_RELOC_SITES,
         "burst bookkeeping drifted");
   CHECK(r300_r2vb_float2_tuple_burst_validate_reloc_sites(&burst) == 0,
         "burst sites failed validation");

   CHECK(burst.member_start[0] == 0 &&
            burst.member_start[1] == reference.ib_size_dwords,
         "member 1 does not start where the reference stream ends");

   struct r300_fragment_binary fs;
   CHECK(r300_r2vb_producer_reference_fs(&fs) == 0, "reference FS failed");
   for (uint32_t m = 0; m < draws; m++) {
      struct r300_r2vb_float2_tuple_params params = {
         .carrier_offset = m * stride,
         .vertex_offset = 0,
         .count = R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT,
         .records = r300_r2vb_float2_tuple_reference_records,
         .first_draw_contract = NULL,
         .fragment_binary = &fs,
      };
      struct r300_r2vb_float2_tuple_ib standalone;
      if (m == 0) {
         standalone = reference;
      } else {
         CHECK(r300_r2vb_float2_tuple_pass_emit(&params, &standalone) == 0,
               "standalone member %u emission failed", m);
      }
      const uint32_t start = burst.member_start[m];
      const uint32_t end = m + 1 < draws ? burst.member_start[m + 1]
                                         : burst.ib_size_dwords;
      CHECK(end - start == standalone.ib_size_dwords &&
               memcmp(&burst.ib[start], standalone.ib,
                      standalone.ib_size_dwords * sizeof(uint32_t)) == 0,
            "burst member %u differs from its standalone emission", m);
      if (m != 0)
         r300_r2vb_float2_tuple_pass_release(&standalone);
   }
   r300_fragment_binary_finish(&fs);
   r300_r2vb_float2_tuple_pass_release(&reference);

   /* Determinism: a second emission reproduces the stream exactly. */
   struct r300_r2vb_float2_tuple_burst_ib again;
   CHECK(r300_r2vb_float2_tuple_burst_reference_emit(draws, &again) == 0 &&
            again.ib_size_dwords == burst.ib_size_dwords &&
            memcmp(again.ib, burst.ib,
                   burst.ib_size_dwords * sizeof(uint32_t)) == 0,
         "burst emission is not deterministic");
   r300_r2vb_float2_tuple_burst_release(&again);
   r300_r2vb_float2_tuple_burst_release(&burst);
   r300_r2vb_float2_tuple_burst_release(&single);

   struct r300_r2vb_float2_tuple_burst_ib refused;
   CHECK(r300_r2vb_float2_tuple_burst_reference_emit(0, &refused) ==
            -EINVAL,
         "zero draws must refuse");
   CHECK(r300_r2vb_float2_tuple_burst_reference_emit(
            R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS + 1, &refused) ==
            -EINVAL,
         "draws past the bound must refuse");
}

/* The FLOAT_4 model contrast: the model array stores the XY01 expansion
 * explicitly, the PSC declares both elements FLOAT_4 identity, and the
 * fetch widens to eight dwords -- with the tuple's carrier expectation
 * unchanged, so fetch width is the one differing variable.
 */
static void
test_float4_model_contract(void)
{
   /* The widened stream: slots unchanged, model records at 16-byte
    * stride holding (x, y, 0, 1).
    */
   uint8_t bytes[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 32];
   CHECK(r300_r2vb_float4_model_vertex_stream(
            r300_r2vb_float2_tuple_reference_records,
            R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, bytes,
            (uint32_t)sizeof(bytes)) == 0,
         "float4 model vertex stream serialization failed");
   const uint32_t m0x = (uint32_t)bytes[48] | ((uint32_t)bytes[49] << 8) |
                        ((uint32_t)bytes[50] << 16) |
                        ((uint32_t)bytes[51] << 24);
   const uint32_t m0z = (uint32_t)bytes[56] | ((uint32_t)bytes[57] << 8) |
                        ((uint32_t)bytes[58] << 16) |
                        ((uint32_t)bytes[59] << 24);
   const uint32_t m0w = (uint32_t)bytes[60] | ((uint32_t)bytes[61] << 8) |
                        ((uint32_t)bytes[62] << 16) |
                        ((uint32_t)bytes[63] << 24);
   const uint32_t m1x = (uint32_t)bytes[64] | ((uint32_t)bytes[65] << 8) |
                        ((uint32_t)bytes[66] << 16) |
                        ((uint32_t)bytes[67] << 24);
   CHECK(m0x == f32_bits(8.0f), "float4 model record 0 x is not 8.0");
   CHECK(m0z == 0, "float4 model record 0 z is not +0.0");
   CHECK(m0w == f32_bits(1.0f), "float4 model record 0 w is not 1.0");
   CHECK(m1x == f32_bits(56.0f), "float4 model record 1 x is not 56.0");
   CHECK(r300_r2vb_float4_model_vertex_stream(
            r300_r2vb_float2_tuple_reference_records,
            R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, bytes,
            (uint32_t)sizeof(bytes) - 1) == -ENOSPC,
         "short float4 model storage not refused");

   struct r300_r2vb_float2_tuple_ib pass;
   CHECK(r300_r2vb_float4_model_reference_emit(&pass) == 0,
         "float4 model reference emission failed");
   CHECK(r300_r2vb_float2_tuple_pass_validate_reloc_sites(&pass) == 0,
         "float4 model relocation sites invalid");

   const uint32_t cntl0 =
      (R300_DATA_TYPE_FLOAT_4 | (0u << R300_DST_VEC_LOC_SHIFT)) |
      ((R300_DATA_TYPE_FLOAT_4 | (6u << R300_DST_VEC_LOC_SHIFT) |
        R300_LAST_VEC)
       << 16);
   CHECK(ib_writes_reg(&pass, R300_VAP_PROG_STREAM_CNTL_0, cntl0),
         "PSC CNTL_0 does not declare the FLOAT_4 + FLOAT_4 pair");
   const uint32_t ext0 =
      R300_VAP_SWIZZLE_XYZW | (R300_VAP_SWIZZLE_XYZW << 16);
   CHECK(ib_writes_reg(&pass, R300_VAP_PROG_STREAM_CNTL_EXT_0, ext0),
         "PSC EXT_0 does not carry identity on both elements");
   CHECK(ib_writes_reg(&pass, R300_VAP_VTX_SIZE,
                       R300_R2VB_FLOAT4_MODEL_VTX_SIZE_DWORDS),
         "VAP_VTX_SIZE is not the eight-dword full-width fetch");

   /* The contrast changes the stream: the widened emission differs from
    * the tuple emission, dword count equal (same packet shapes).
    */
   struct r300_r2vb_float2_tuple_ib tuple;
   CHECK(r300_r2vb_float2_tuple_reference_emit(&tuple) == 0,
         "tuple reference emission failed");
   CHECK(pass.ib_size_dwords == tuple.ib_size_dwords,
         "the width contrast changed the packet shape");
   CHECK(memcmp(pass.ib, tuple.ib,
                tuple.ib_size_dwords * sizeof(uint32_t)) != 0,
         "the width contrast left the stream bytes unchanged");
   r300_r2vb_float2_tuple_pass_release(&tuple);

   /* Burst composition: member 0 equals the reference emission whole,
    * later members equal prefix-free standalone emissions, and the
    * composition is deterministic.
    */
   struct r300_r2vb_float2_tuple_burst_ib single;
   CHECK(r300_r2vb_float4_model_burst_reference_emit(1, &single) == 0 &&
            single.ib_size_dwords == pass.ib_size_dwords &&
            memcmp(single.ib, pass.ib,
                   pass.ib_size_dwords * sizeof(uint32_t)) == 0,
         "a one-member float4 burst is the reference stream byte for "
         "byte");
   struct r300_r2vb_float2_tuple_burst_ib burst;
   CHECK(r300_r2vb_float4_model_burst_reference_emit(4, &burst) == 0,
         "four-member float4 burst emission failed");
   CHECK(r300_r2vb_float2_tuple_burst_validate_reloc_sites(&burst) == 0,
         "float4 burst sites failed validation");
   struct r300_r2vb_float2_tuple_burst_ib again;
   CHECK(r300_r2vb_float4_model_burst_reference_emit(4, &again) == 0 &&
            again.ib_size_dwords == burst.ib_size_dwords &&
            memcmp(again.ib, burst.ib,
                   burst.ib_size_dwords * sizeof(uint32_t)) == 0,
         "float4 burst emission is not deterministic");
   r300_r2vb_float2_tuple_burst_release(&again);
   r300_r2vb_float2_tuple_burst_release(&burst);
   r300_r2vb_float2_tuple_burst_release(&single);
   r300_r2vb_float2_tuple_pass_release(&pass);
}

int
main(void)
{
   test_reference_records_pinned();
   test_expected_is_xy01_expansion();
   test_vertex_stream_layout();
   test_emission_contract();
   test_burst_member_byte_identity();
   test_float4_model_contract();

   if (failures != 0) {
      fprintf(stderr, "r300_r2vb_float2_tuple_pass_test: %u failures\n",
              failures);
      return 1;
   }
   printf("r300_r2vb_float2_tuple_pass_test: all checks passed\n");
   return 0;
}
