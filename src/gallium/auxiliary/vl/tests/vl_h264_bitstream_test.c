/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Stage (a) unit test for the clean-room H.264 bitstream reader: RBSP de-escape
 * (sec 7.4.1), the Exp-Golomb codes ue/se/te (sec 9.1/9.1.1), and the
 * more_rbsp_data end test (sec 7.2).  The vectors are hand-derived from the spec
 * code tables, not from any decoder.  A small MSB-first bit writer builds the
 * Exp-Golomb stream so the reader is checked against an independent encoder.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vl_h264_bitstream.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

/* MSB-first bit writer, independent of the reader under test. */
struct bitwriter {
   uint8_t buf[512];
   unsigned nbits;
};

static void
put_bit(struct bitwriter *w, unsigned bit)
{
   if (bit)
      w->buf[w->nbits >> 3] |= (uint8_t)(0x80u >> (w->nbits & 7));
   w->nbits++;
}

static void
put_bits(struct bitwriter *w, uint32_t value, unsigned n)
{
   for (unsigned i = 0; i < n; i++)
      put_bit(w, (value >> (n - 1 - i)) & 1);
}

/* ue(v): M leading zeros then the (M+1)-bit value codeNum+1 (sec 9.1). */
static void
put_ue(struct bitwriter *w, uint32_t code_num)
{
   uint32_t v1 = code_num + 1;
   unsigned m = 0;
   while ((1u << (m + 1)) <= v1)
      m++;
   for (unsigned i = 0; i < m; i++)
      put_bit(w, 0);
   put_bits(w, v1, m + 1);
}

/* se(v): codeNum = s>0 ? 2s-1 : -2s (sec 9.1.1). */
static void
put_se(struct bitwriter *w, int s)
{
   put_ue(w, s > 0 ? (uint32_t)(2 * s - 1) : (uint32_t)(-2 * s));
}

static int
test_de_escape(void)
{
   /* EBSP with two inserted emulation_prevention_three_byte (0x03) sequences and
    * a final 0x80 rbsp_trailing byte.  De-escaped RBSP is {00 00 01 00 00 02 FF
    * 80}. */
   const uint8_t ebsp[] = {
      0x00, 0x00, 0x03, 0x01,   /* -> 00 00 01 */
      0x00, 0x00, 0x03, 0x02,   /* -> 00 00 02 */
      0xFF,                     /* -> FF */
      0x80,                     /* rbsp_stop_one_bit at the MSB */
   };
   struct vl_h264_reader r;
   CHECK(vl_h264_reader_init(&r, ebsp, sizeof(ebsp)));
   CHECK(r.rbsp_size == 8);

   CHECK(vl_h264_u(&r, 8) == 0x00);
   CHECK(vl_h264_u(&r, 8) == 0x00);
   CHECK(vl_h264_u(&r, 8) == 0x01);
   CHECK(vl_h264_u(&r, 8) == 0x00);
   CHECK(vl_h264_u(&r, 8) == 0x00);
   CHECK(vl_h264_u(&r, 8) == 0x02);
   /* Coded data remains (the 0xFF byte) until the trailing 0x80. */
   CHECK(vl_h264_more_rbsp_data(&r));
   CHECK(vl_h264_u(&r, 8) == 0xFF);
   /* Now positioned at the rbsp_stop_one_bit: no coded data left. */
   CHECK(!vl_h264_more_rbsp_data(&r));

   vl_h264_reader_fini(&r);
   return 0;
}

static int
test_exp_golomb(void)
{
   const uint32_t ue_vec[] = { 0, 1, 2, 3, 7, 14, 15, 100 };
   const int se_vec[] = { 0, 1, -1, 2, -2, 3, -3 };
   struct bitwriter w;
   struct vl_h264_reader r;
   unsigned i;

   memset(&w, 0, sizeof(w));

   /* A fixed prefix, then the ue and se sequences, then te(v) cases. */
   put_bits(&w, 5, 3);                 /* u(3) = 101 = 5 */
   for (i = 0; i < sizeof(ue_vec) / sizeof(ue_vec[0]); i++)
      put_ue(&w, ue_vec[i]);
   for (i = 0; i < sizeof(se_vec) / sizeof(se_vec[0]); i++)
      put_se(&w, se_vec[i]);
   put_bit(&w, 0);                     /* te(range==1): 0 -> value 1 */
   put_bit(&w, 1);                     /* te(range==1): 1 -> value 0 */
   put_ue(&w, 9);                      /* te(range>1) decodes as ue(v) */
   put_bit(&w, 1);                     /* rbsp_stop_one_bit */

   CHECK(vl_h264_reader_init(&r, w.buf, (w.nbits + 7) / 8));

   CHECK(vl_h264_u(&r, 3) == 5);
   unsigned before_te_zero = vl_h264_bits_consumed(&r);
   CHECK(vl_h264_te(&r, 0) == 0);
   CHECK(vl_h264_bits_consumed(&r) == before_te_zero);
   CHECK(vl_h264_more_rbsp_data(&r));
   for (i = 0; i < sizeof(ue_vec) / sizeof(ue_vec[0]); i++)
      CHECK(vl_h264_ue(&r) == ue_vec[i]);
   for (i = 0; i < sizeof(se_vec) / sizeof(se_vec[0]); i++)
      CHECK(vl_h264_se(&r) == se_vec[i]);
   CHECK(vl_h264_te(&r, 1) == 1);
   CHECK(vl_h264_te(&r, 1) == 0);
   CHECK(vl_h264_te(&r, 5) == 9);
   /* Everything before the trailing one bit is consumed. */
   CHECK(!vl_h264_more_rbsp_data(&r));

   vl_h264_reader_fini(&r);
   return 0;
}

int
main(void)
{
   if (test_de_escape())
      return 1;
   if (test_exp_golomb())
      return 1;
   printf("vl_h264_bitstream: de-escape + Exp-Golomb + more_rbsp_data PASS\n");
   return 0;
}
