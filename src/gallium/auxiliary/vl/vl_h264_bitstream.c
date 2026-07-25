/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "util/u_math.h"
#include "util/u_memory.h"

#include "vl_h264_bitstream.h"

/*
 * Clean-room from ITU-T H.264.  The de-escape is sec 7.4.1, the Exp-Golomb codes
 * are sec 9.1/9.1.1, and the rbsp end-of-data test is sec 7.2.  No third-party
 * decoder source was consulted.
 */

bool
vl_h264_reader_init(struct vl_h264_reader *reader,
                    const uint8_t *ebsp, unsigned ebsp_size)
{
   memset(reader, 0, sizeof(*reader));

   if (!ebsp || ebsp_size == 0)
      return false;

   /* The de-escaped RBSP is never longer than the EBSP. */
   reader->rbsp = MALLOC(ebsp_size);
   if (!reader->rbsp)
      return false;

   /* Strip emulation_prevention_three_byte: a 0x03 following two 0x00 bytes is
    * an inserted byte and is dropped, after which the zero run restarts so a
    * genuine 0x03 right behind it survives (sec 7.4.1). */
   unsigned out = 0, zeros = 0;
   for (unsigned i = 0; i < ebsp_size; i++) {
      uint8_t byte = ebsp[i];
      if (zeros >= 2 && byte == 0x03) {
         zeros = 0;
         continue;
      }
      reader->rbsp[out++] = byte;
      zeros = (byte == 0) ? zeros + 1 : 0;
   }
   reader->rbsp_size = out;
   reader->total_bits = out * 8;
   reader->overrun = false;

   /* The rbsp_stop_one_bit is the last set bit in the RBSP; coded data ends
    * there and the remaining bits are alignment padding (sec 7.2). */
   reader->stop_bit_pos = 0;
   for (unsigned i = out; i-- > 0;) {
      uint8_t byte = reader->rbsp[i];
      if (byte) {
         unsigned lsb = 0;
         while (!((byte >> lsb) & 1))
            lsb++;
         reader->stop_bit_pos = i * 8 + (7 - lsb);
         break;
      }
   }

   reader->inputs[0] = reader->rbsp;
   reader->sizes[0] = reader->rbsp_size;
   vl_vlc_init(&reader->vlc, 1, reader->inputs, reader->sizes);
   return true;
}

void
vl_h264_reader_fini(struct vl_h264_reader *reader)
{
   if (reader && reader->rbsp) {
      FREE(reader->rbsp);
      reader->rbsp = NULL;
   }
}

unsigned
vl_h264_u(struct vl_h264_reader *reader, unsigned num_bits)
{
   if (num_bits == 0)
      return 0;

   vl_vlc_fillbits(&reader->vlc);
   /* A truncated or malformed stream can ask for more bits than remain; reading
    * them would assert in vl_vlc or return trailing zeros as data.  Flag the
    * overrun and return zero so callers can reject the stream. */
   if (vl_vlc_valid_bits(&reader->vlc) < num_bits) {
      reader->overrun = true;
      return 0;
   }
   return vl_vlc_get_uimsbf(&reader->vlc, num_bits);
}

bool
vl_h264_overrun(const struct vl_h264_reader *reader)
{
   return reader->overrun;
}

unsigned
vl_h264_leading_zeros(struct vl_h264_reader *reader)
{
   unsigned count = 0;

   for (;;) {
      vl_vlc_fillbits(&reader->vlc);

      /* vl_vlc can hold more than 32 valid bits in its 64-bit buffer, but
       * vl_vlc_peekbits returns a 32-bit value, so never peek a wider window. */
      unsigned valid = vl_vlc_valid_bits(&reader->vlc);
      if (valid == 0)
         return count; /* RBSP exhausted; only on a malformed stream */
      unsigned take = valid < 32 ? valid : 32;

      /* The valid bits sit at the top of the buffer; peek them right-aligned. */
      unsigned window = vl_vlc_peekbits(&reader->vlc, take);
      if (window == 0) {
         /* Every peeked bit is zero: count them and refill for more. */
         count += take;
         vl_vlc_eatbits(&reader->vlc, take);
         continue;
      }

      /* Zeros above the highest set bit are the run; consume them and the
       * terminating one bit. */
      unsigned highest = util_logbase2(window);
      unsigned zeros = take - 1 - highest;
      vl_vlc_eatbits(&reader->vlc, zeros + 1);
      return count + zeros;
   }
}

unsigned
vl_h264_ue(struct vl_h264_reader *reader)
{
   unsigned leading_zeros = vl_h264_leading_zeros(reader);
   if (leading_zeros == 0)
      return 0;

   /* A run of 32 or more zeros cannot encode a valid codeNum; clamp rather than
    * shift by the bit width. */
   if (leading_zeros >= 32)
      return UINT32_MAX;

   return ((1u << leading_zeros) - 1) + vl_h264_u(reader, leading_zeros);
}

int
vl_h264_se(struct vl_h264_reader *reader)
{
   unsigned code_num = vl_h264_ue(reader);

   /* sec 9.1.1: (-1)^(codeNum+1) * Ceil(codeNum / 2). */
   return (code_num & 1) ? (int)((code_num + 1) >> 1)
                         : -(int)(code_num >> 1);
}

unsigned
vl_h264_te(struct vl_h264_reader *reader, unsigned range)
{
   /* sec 9.1.1: a cMax of 1 is a single inverted bit; otherwise ue(v). */
   if (range == 1)
      return 1u - vl_h264_u(reader, 1);
   return vl_h264_ue(reader);
}

unsigned
vl_h264_bits_consumed(struct vl_h264_reader *reader)
{
   return reader->total_bits - vl_vlc_bits_left(&reader->vlc);
}

bool
vl_h264_more_rbsp_data(struct vl_h264_reader *reader)
{
   return vl_h264_bits_consumed(reader) < reader->stop_bit_pos;
}
