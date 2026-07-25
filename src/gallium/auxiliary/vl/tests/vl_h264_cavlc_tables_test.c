/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Round-trip test for the CAVLC codeword decoders: for every entry in every
 * generated table, encode the codeword into a fresh bitstream and confirm the
 * matching context decoder returns the entry's value and consumes exactly its
 * bits.  This validates the prefix matcher against the tables and that the
 * context selection reaches the right table; the spec-value correctness is the
 * later macroblock oracle gate.  The tables are included directly so the test
 * iterates the same data the decoder compiles in.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vl_h264_cavlc.h"
#include "vl_h264_cavlc_tables.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

/* Encode a codeword (MSB first) followed by a marker one bit and byte padding,
 * then start the reader on it. */
static void
init_one_code(struct vl_h264_reader *reader, uint8_t *backing,
              const struct vl_h264_vlc *entry)
{
   uint8_t buf[8];
   unsigned nbits = 0;

   memset(buf, 0, sizeof(buf));
   for (unsigned b = 0; b < entry->len; b++) {
      unsigned bit = (entry->code >> (entry->len - 1 - b)) & 1;
      if (bit)
         buf[nbits >> 3] |= (uint8_t)(0x80u >> (nbits & 7));
      nbits++;
   }
   buf[nbits >> 3] |= (uint8_t)(0x80u >> (nbits & 7)); /* marker one bit */
   nbits++;

   unsigned nbytes = (nbits + 7) / 8;
   memcpy(backing, buf, nbytes);
   vl_h264_reader_init(reader, backing, nbytes);
}

static int
check_coeff_token_table(const struct vl_h264_vlc_table *table, int nc)
{
   for (unsigned i = 0; i < table->count; i++) {
      uint8_t backing[8];
      struct vl_h264_reader reader;
      unsigned total_coeff, trailing_ones;

      init_one_code(&reader, backing, &table->entries[i]);
      CHECK(vl_h264_cavlc_coeff_token(&reader, nc, &total_coeff, &trailing_ones));
      CHECK(total_coeff == (unsigned)(table->entries[i].value >> 2));
      CHECK(trailing_ones == (unsigned)(table->entries[i].value & 3));
      CHECK(vl_h264_bits_consumed(&reader) == table->entries[i].len);
      vl_h264_reader_fini(&reader);
   }
   return 0;
}

static int
check_value_table(const struct vl_h264_vlc_table *table,
                  bool (*decode)(struct vl_h264_reader *, unsigned, unsigned *),
                  unsigned ctx)
{
   for (unsigned i = 0; i < table->count; i++) {
      uint8_t backing[8];
      struct vl_h264_reader reader;
      unsigned value;

      init_one_code(&reader, backing, &table->entries[i]);
      CHECK(decode(&reader, ctx, &value));
      CHECK(value == (unsigned)table->entries[i].value);
      CHECK(vl_h264_bits_consumed(&reader) == table->entries[i].len);
      vl_h264_reader_fini(&reader);
   }
   return 0;
}

/* Thin context-fixing wrappers so the value tables share one driver. */
static bool
total_zeros_4x4_decode(struct vl_h264_reader *r, unsigned total_coeff,
                       unsigned *out)
{
   return vl_h264_cavlc_total_zeros(r, total_coeff, 16, out);
}

static bool
total_zeros_chroma_decode(struct vl_h264_reader *r, unsigned total_coeff,
                          unsigned *out)
{
   return vl_h264_cavlc_total_zeros(r, total_coeff, 4, out);
}

int
main(void)
{
   /* coeff_token: each table behind its selecting nC. */
   const int coeff_nc[4] = { 0, 2, 4, -1 };
   for (unsigned t = 0; t < 4; t++)
      if (check_coeff_token_table(&vl_h264_coeff_token_tables[t], coeff_nc[t]))
         return 1;

   /* coeff_token nC >= 8 fixed-length path: spot-check the empty block and a
    * couple of populated codes. */
   {
      struct vl_h264_vlc flc[3] = {
         { 0x03, 6, (0 << 2) | 0 },   /* 000011 -> TotalCoeff 0 */
         { 0x00, 6, (1 << 2) | 0 },   /* 000000 -> TotalCoeff 1, T1 0 */
         { 0x06, 6, (2 << 2) | 2 },   /* 000110 -> TotalCoeff 2, T1 2 */
      };
      for (unsigned i = 0; i < 3; i++) {
         uint8_t backing[8];
         struct vl_h264_reader reader;
         unsigned tc, t1;
         init_one_code(&reader, backing, &flc[i]);
         CHECK(vl_h264_cavlc_coeff_token(&reader, 8, &tc, &t1));
         CHECK(tc == (unsigned)(flc[i].value >> 2));
         CHECK(t1 == (unsigned)(flc[i].value & 3));
         vl_h264_reader_fini(&reader);
      }
   }

   /* total_zeros: 4x4 for TotalCoeff 1..15, chroma DC for 1..3. */
   for (unsigned tc = 1; tc < ARRAY_SIZE(vl_h264_total_zeros_4x4); tc++)
      if (check_value_table(&vl_h264_total_zeros_4x4[tc],
                            total_zeros_4x4_decode, tc))
         return 1;
   for (unsigned tc = 1; tc < ARRAY_SIZE(vl_h264_total_zeros_chroma); tc++)
      if (check_value_table(&vl_h264_total_zeros_chroma[tc],
                            total_zeros_chroma_decode, tc))
         return 1;

   /* run_before: zerosLeft 1..6 and the shared >6 table at index 7. */
   for (unsigned zl = 1; zl < ARRAY_SIZE(vl_h264_run_before); zl++)
      if (check_value_table(&vl_h264_run_before[zl], vl_h264_cavlc_run_before,
                            zl == 7 ? 7 : zl))
         return 1;

   printf("vl_h264_cavlc_tables: all coeff_token/total_zeros/run_before "
          "codewords round-trip PASS\n");
   return 0;
}
