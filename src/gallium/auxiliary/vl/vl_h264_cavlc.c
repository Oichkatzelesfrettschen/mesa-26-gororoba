/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

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

/* One capacity governs all three per-coefficient arrays, so the residual
 * decoder's single bound check covers every write below.  A struct edit that
 * shrinks one array independently stops the build here rather than at a
 * diagnostic on one of the writes. */
static_assert(ARRAY_SIZE(((struct vl_h264_cavlc_block *)0)->level) ==
              VL_H264_CAVLC_MAX_COEFF, "level[] holds one entry per coefficient");
static_assert(ARRAY_SIZE(((struct vl_h264_cavlc_block *)0)->run) ==
              VL_H264_CAVLC_MAX_COEFF, "run[] holds one entry per coefficient");
static_assert(ARRAY_SIZE(((struct vl_h264_cavlc_block *)0)->coeff) ==
              VL_H264_CAVLC_MAX_COEFF, "coeff[] holds one entry per coefficient");

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

/* Decode one non-trailing level and advance suffix_length (sec 9.2.2).
 * first_level biases the magnitude up by one, applied to the first non-trailing
 * level when there were fewer than three trailing ones. */
static int16_t
decode_one_level(struct vl_h264_reader *reader, unsigned *suffix_length,
                 bool first_level)
{
   unsigned level_prefix = vl_h264_leading_zeros(reader);

   unsigned suffix_size = *suffix_length;
   if (level_prefix == 14 && *suffix_length == 0)
      suffix_size = 4;
   else if (level_prefix == 15)
      suffix_size = 12;

   unsigned level_suffix = suffix_size > 0 ? vl_h264_u(reader, suffix_size) : 0;
   int level_code = (int)(level_prefix << *suffix_length) + (int)level_suffix;
   if (level_prefix == 15 && *suffix_length == 0)
      level_code += 15;
   if (first_level)
      level_code += 2;

   /* Even level_code is a positive level, odd is negative; the shifts stay on
    * non-negative operands. */
   int16_t level = ((level_code & 1) == 0)
                 ? (int16_t)((level_code + 2) >> 1)
                 : (int16_t)(-((level_code + 1) >> 1));

   if (*suffix_length == 0)
      *suffix_length = 1;
   int magnitude = level < 0 ? -level : level;
   if (magnitude > (3 << (*suffix_length - 1)) && *suffix_length < 6)
      (*suffix_length)++;
   return level;
}

bool
vl_h264_cavlc_decode_levels(struct vl_h264_reader *reader, unsigned total_coeff,
                            unsigned trailing_ones,
                            int16_t level[VL_H264_CAVLC_MAX_COEFF])
{
   unsigned i = 0;

   /* The trailing ones are a single sign bit each (sec 9.2.2). */
   for (; i < trailing_ones; i++)
      level[i] = vl_h264_u(reader, 1) ? -1 : 1;

   /* suffixLength seeds high for a long run of large levels, low otherwise. */
   unsigned suffix_length = (total_coeff > 10 && trailing_ones < 3) ? 1 : 0;

   for (; i < total_coeff; i++)
      level[i] = decode_one_level(reader, &suffix_length,
                                  i == trailing_ones && trailing_ones < 3);
   return true;
}

bool
vl_h264_cavlc_residual_block(struct vl_h264_reader *reader,
                             unsigned max_num_coeff, int nc,
                             struct vl_h264_cavlc_block *out)
{
   /* level[], run[], and coeff[] hold one entry per coefficient position, so a
    * max_num_coeff above that capacity would let a decoded total_coeff index
    * past the end.  H.264 sec 7.4.5.3.2 caps the value at 16 for every block
    * type, and rejecting a larger one bounds every write below against the
    * array size rather than against the parameter.  The rejection precedes the
    * memset, so a caller that passes an impossible capacity gets its block back
    * unmodified. */
   if (max_num_coeff > VL_H264_CAVLC_MAX_COEFF)
      return false;

   memset(out, 0, sizeof(*out));

   unsigned total_coeff, trailing_ones;
   if (!vl_h264_cavlc_coeff_token(reader, nc, &total_coeff, &trailing_ones))
      return false;
   if (total_coeff > max_num_coeff)
      return false;
   out->total_coeff = total_coeff;
   out->trailing_ones = trailing_ones;
   if (total_coeff == 0)
      return true;                 /* a coded-but-empty block is valid */

   /* total_coeff and trailing_ones stay local after coeff_token, and the
    * capacity check proves total_coeff <= max_num_coeff <=
    * VL_H264_CAVLC_MAX_COEFF.  vl_h264_cavlc_decode_levels receives out->level,
    * and vl_h264_cavlc_total_zeros receives &out->total_zeros; retaining the
    * local total_coeff preserves that bound across both calls for the level[i]
    * and run[i] indices.  The scan-order combine separately checks coeff_num
    * against max_num_coeff before indexing out->coeff. */
   if (!vl_h264_cavlc_decode_levels(reader, total_coeff, trailing_ones,
                                    out->level))
      return false;

   unsigned zeros_left;
   if (total_coeff < max_num_coeff) {
      if (!vl_h264_cavlc_total_zeros(reader, total_coeff, max_num_coeff,
                                     &out->total_zeros))
         return false;
      zeros_left = out->total_zeros;
   } else {
      zeros_left = 0;
   }

   /* Each level but the last carries the run of zeros before it; the last takes
    * whatever zeros remain (sec 7.3.5.3.1). */
   for (unsigned i = 0; i + 1 < total_coeff; i++) {
      unsigned run = 0;
      if (zeros_left > 0) {
         if (!vl_h264_cavlc_run_before(reader, zeros_left, &run))
            return false;
      }
      if (run > zeros_left)
         return false;
      out->run[i] = (uint8_t)run;
      zeros_left -= run;
   }
   out->run[total_coeff - 1] = (uint8_t)zeros_left;

   /* Combine runs and levels into scan order (sec 9.2.4). */
   int coeff_num = -1;
   for (unsigned i = total_coeff; i-- > 0;) {
      coeff_num += out->run[i] + 1;
      if (coeff_num < 0 || coeff_num >= (int)max_num_coeff)
         return false;
      out->coeff[coeff_num] = out->level[i];
   }
   return true;
}
