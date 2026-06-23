/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "util/macros.h"
#include "util/vl_vlc.h"

#include "vl_h264_cavlc.h"
#include "vl_h264_cavlc_tables.h"

/*
 * Clean-room from ITU-T H.264 sec 9.2.  The codeword tables are generated from
 * the spec; this is the matcher and the context selection over them.
 */

/* The longest CAVLC codeword in any table is 16 bits (coeff_token, Table 9-5). */
#define VL_H264_CAVLC_MAX_CODE_BITS 16

/*
 * Bounded prefix matcher: the tables are prefix-free, so the first entry whose
 * top len bits equal the next len bits of the stream is the match.  Peeks a
 * fixed window (never wider than the 32-bit vl_vlc_peekbits result) and tests
 * each entry; consumes only the matched codeword.
 */
static bool
cavlc_match(struct vl_h264_reader *reader, const struct vl_h264_vlc_table *table,
            int *value)
{
   vl_vlc_fillbits(&reader->vlc);
   unsigned valid = vl_vlc_valid_bits(&reader->vlc);
   unsigned take = valid < VL_H264_CAVLC_MAX_CODE_BITS
                 ? valid : VL_H264_CAVLC_MAX_CODE_BITS;
   if (take == 0)
      return false;
   unsigned window = vl_vlc_peekbits(&reader->vlc, take);

   for (unsigned i = 0; i < table->count; i++) {
      unsigned len = table->entries[i].len;
      if (len > take)
         continue;
      if ((window >> (take - len)) == table->entries[i].code) {
         vl_vlc_eatbits(&reader->vlc, len);
         *value = table->entries[i].value;
         return true;
      }
   }
   return false;
}

bool
vl_h264_cavlc_coeff_token(struct vl_h264_reader *reader, int nc,
                          unsigned *total_coeff, unsigned *trailing_ones)
{
   /* nC >= 8 is a 6-bit fixed-length code (Table 9-5, "8 <= nC" column): the
    * codeword 000011 is the empty block, otherwise TotalCoeff-1 is the top four
    * bits and TrailingOnes the low two. */
   if (nc >= 8) {
      unsigned code = vl_h264_u(reader, 6);
      if (code == 3) {
         *total_coeff = 0;
         *trailing_ones = 0;
         return true;
      }
      *total_coeff = (code >> 2) + 1;
      *trailing_ones = code & 3;
      return *trailing_ones <= *total_coeff && *trailing_ones <= 3;
   }

   const struct vl_h264_vlc_table *table;
   if (nc == -1)
      table = &vl_h264_coeff_token_tables[3]; /* chroma DC */
   else if (nc < 2)
      table = &vl_h264_coeff_token_tables[0];
   else if (nc < 4)
      table = &vl_h264_coeff_token_tables[1];
   else
      table = &vl_h264_coeff_token_tables[2]; /* 4 <= nC < 8 */

   int value;
   if (!cavlc_match(reader, table, &value))
      return false;
   *total_coeff = (unsigned)value >> 2;
   *trailing_ones = (unsigned)value & 3;
   return true;
}

bool
vl_h264_cavlc_total_zeros(struct vl_h264_reader *reader, unsigned total_coeff,
                          unsigned max_num_coeff, unsigned *total_zeros)
{
   const struct vl_h264_vlc_table *table;
   if (max_num_coeff == 4) {
      if (total_coeff >= ARRAY_SIZE(vl_h264_total_zeros_chroma))
         return false;
      table = &vl_h264_total_zeros_chroma[total_coeff];
   } else {
      if (total_coeff >= ARRAY_SIZE(vl_h264_total_zeros_4x4))
         return false;
      table = &vl_h264_total_zeros_4x4[total_coeff];
   }

   int value;
   if (!cavlc_match(reader, table, &value))
      return false;
   *total_zeros = (unsigned)value;
   return true;
}

bool
vl_h264_cavlc_run_before(struct vl_h264_reader *reader, unsigned zeros_left,
                         unsigned *run_before)
{
   /* zeros_left selects the table; everything above six shares one table. */
   unsigned index = zeros_left > 6 ? 7 : zeros_left;
   const struct vl_h264_vlc_table *table = &vl_h264_run_before[index];

   int value;
   if (!cavlc_match(reader, table, &value))
      return false;
   *run_before = (unsigned)value;
   return true;
}
