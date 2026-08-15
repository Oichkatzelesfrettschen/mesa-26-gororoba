/*
 * SPDX-License-Identifier: MIT
 *
 * Host test for the owned fragment-binary descriptor: deep-copy ownership,
 * hash stability, and structural stream admission.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include "r300_fragment_binary.h"

#include "r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKT0(reg, count) ((((count) - 1) << 16) | ((reg) >> 2))
#define PKT0_ONE_REG(reg, count) (PKT0(reg, count) | RADEON_ONE_REG_WR)

#define US_CONFIG R300_US_CONFIG
#define US_PIXSIZE R300_US_PIXSIZE
#define US_CODE_ADDR_0 R300_US_CODE_ADDR_0
#define PFS_PARAM_FIRST R300_PFS_PARAM_0_X
#define PFS_PARAM_TAIL R300_PFS_PARAM_31_X
#define GA_US_VECTOR_INDEX R500_GA_US_VECTOR_INDEX
#define GA_US_VECTOR_DATA R500_GA_US_VECTOR_DATA
#define ZB_DEPTHOFFSET 0x4f20 /* outside the US/FG block */

static uint32_t *
make_known_good(uint32_t *size_out)
{
   /* One scalar write, one four-register sequence, one scalar write: the
    * smallest stream exercising both packet shapes.
    */
   static const uint32_t stream[] = {
      PKT0(US_CONFIG, 1),      0x00000000,
      PKT0(US_CODE_ADDR_0, 4), 0x1, 0x2, 0x3, 0x40040,
      PKT0(US_PIXSIZE, 1),     0x00000002,
   };
   uint32_t *copy = malloc(sizeof(stream));
   assert(copy != NULL);
   memcpy(copy, stream, sizeof(stream));
   *size_out = sizeof(stream) / sizeof(uint32_t);
   return copy;
}

static void
test_ownership_survives_source_destruction(void)
{
   uint32_t size;
   uint32_t *source = make_known_good(&size);

   struct r300_fragment_binary binary;
   assert(r300_fragment_binary_init(&binary, source, size, 0, 0,
                                    "r300g-rc/test") == 0);
   assert(binary.validated);
   assert(binary.cb_code != source);

   uint8_t hash_before[R300_FRAGMENT_BINARY_HASH_SIZE];
   memcpy(hash_before, binary.hash, sizeof(hash_before));

   /* Clobber and free the source; the owned copy and its hash stand. */
   memset(source, 0xA5, size * sizeof(uint32_t));
   free(source);

   uint32_t reference_size;
   uint32_t *reference = make_known_good(&reference_size);
   assert(binary.cb_code_size == reference_size);
   assert(memcmp(binary.cb_code, reference,
                 reference_size * sizeof(uint32_t)) == 0);
   free(reference);

   struct r300_fragment_binary rebuilt;
   assert(r300_fragment_binary_init(&rebuilt, binary.cb_code,
                                    binary.cb_code_size, 0, 0,
                                    "r300g-rc/test") == 0);
   assert(memcmp(rebuilt.hash, hash_before, sizeof(hash_before)) == 0);

   r300_fragment_binary_finish(&rebuilt);
   r300_fragment_binary_finish(&binary);
}

static void
test_hash_covers_register_values(void)
{
   uint32_t size;
   uint32_t *stream = make_known_good(&size);

   struct r300_fragment_binary a;
   struct r300_fragment_binary b;
   assert(r300_fragment_binary_init(&a, stream, size, 0, 0, NULL) == 0);
   assert(r300_fragment_binary_init(&b, stream, size, 1, 0, NULL) == 0);
   assert(memcmp(a.hash, b.hash, R300_FRAGMENT_BINARY_HASH_SIZE) != 0);

   r300_fragment_binary_finish(&a);
   r300_fragment_binary_finish(&b);
   free(stream);
}

static void
test_validator_rejects_known_bad(void)
{
   struct r300_fragment_binary binary;

   /* Empty stream. */
   assert(r300_fragment_binary_init(&binary, NULL, 0, 0, 0, NULL) == -EINVAL);

   /* Truncated payload: header promises two dwords, stream ends after one. */
   const uint32_t truncated[] = {PKT0(US_CONFIG, 2), 0x0};
   assert(!r300_fragment_binary_stream_valid(truncated, 2));

   /* Register outside the US/FG block. */
   const uint32_t out_of_window[] = {PKT0(ZB_DEPTHOFFSET, 1), 0x0};
   assert(!r300_fragment_binary_stream_valid(out_of_window, 2));

   /* Packet0 bit 14 belongs to the register field.  It cannot be discarded
    * as part of the one-register flag because the resulting address is
    * outside the admitted register domain.
    */
   const uint32_t register_bit_fourteen[] = {
      PKT0(US_CONFIG, 1) | (1u << 14), 0x0,
   };
   assert(!r300_fragment_binary_stream_valid(register_bit_fourteen, 2));

   /* The vector data port requires the one-register packet bit. */
   const uint32_t vector_without_one_reg[] = {
      PKT0(GA_US_VECTOR_DATA, 2), 1, 2,
   };
   assert(!r300_fragment_binary_stream_valid(
      vector_without_one_reg,
      sizeof(vector_without_one_reg) / sizeof(uint32_t)));

   /* Vector data is stateful: an owned stream selects its starting index
    * before the autoincrementing data port consumes payload dwords.
    */
   const uint32_t vector_without_index[] = {
      PKT0_ONE_REG(GA_US_VECTOR_DATA, 2), 1, 2,
   };
   assert(!r300_fragment_binary_stream_valid(
      vector_without_index,
      sizeof(vector_without_index) / sizeof(uint32_t)));

   /* The one-register packet bit is specific to stream ports. */
   const uint32_t one_reg_on_register_sequence[] = {
      PKT0_ONE_REG(US_CONFIG, 2), 1, 2,
   };
   assert(!r300_fragment_binary_stream_valid(
      one_reg_on_register_sequence,
      sizeof(one_reg_on_register_sequence) / sizeof(uint32_t)));

   /* The vector index selects one source before the data stream starts. */
   const uint32_t vector_index_sequence[] = {
      PKT0(GA_US_VECTOR_INDEX, 2), 1, 2,
   };
   assert(!r300_fragment_binary_stream_valid(
      vector_index_sequence,
      sizeof(vector_index_sequence) / sizeof(uint32_t)));

   /* A sequence that starts inside the block and runs past its end. */
   const uint32_t runs_out[] = {
      PKT0(PFS_PARAM_TAIL, 12), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   };
   assert(!r300_fragment_binary_stream_valid(
      runs_out, sizeof(runs_out) / sizeof(uint32_t)));

   /* A type-3 packet header. */
   const uint32_t pkt3[] = {0xC0001000u, 0x0};
   assert(!r300_fragment_binary_stream_valid(pkt3, 2));

   /* The GA US vector data port takes an arbitrarily long payload. */
   const uint32_t vector_stream[] = {
      PKT0(GA_US_VECTOR_INDEX, 1), 0,
      PKT0_ONE_REG(GA_US_VECTOR_DATA, 6), 1, 2, 3, 4, 5, 6,
   };
   assert(r300_fragment_binary_stream_valid(
      vector_stream, sizeof(vector_stream) / sizeof(uint32_t)));

   /* Fragment parameters occupy the upper part of the US/FG block. */
   const uint32_t parameter_stream[] = {
      PKT0(PFS_PARAM_FIRST, 4), 1, 2, 3, 4,
      PKT0(PFS_PARAM_TAIL, 4), 5, 6, 7, 8,
   };
   assert(r300_fragment_binary_stream_valid(
      parameter_stream, sizeof(parameter_stream) / sizeof(uint32_t)));
}

int
main(void)
{
   test_ownership_survives_source_destruction();
   test_hash_covers_register_values();
   test_validator_rejects_known_bad();
   printf("r300_fragment_binary_test: all checks passed\n");
   return 0;
}
