/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Clean-room H.264 bitstream primitives for the Mesa-native CAVLC front end.
 *
 * The decoder reads an Annex B NAL whose payload is an EBSP: an RBSP with
 * emulation_prevention_three_byte (0x03) inserted after every 0x0000 so a start
 * code never appears inside the payload (ITU-T H.264 sec 7.4.1).  vl_vlc gives
 * fast bit access but has no de-escape and no Exp-Golomb, so this reader strips
 * the 0x03 bytes into an owned RBSP scratch buffer, then layers the ue(v)/se(v)/
 * te(v) Exp-Golomb codes (sec 9.1) and the rbsp end-of-data test (sec 7.2) on
 * top of vl_vlc.  The CAVLC residual decoder builds its variable-length tables on
 * the same reader via the exposed vl_vlc and the leading-zero helper.
 */

#ifndef vl_h264_bitstream_h
#define vl_h264_bitstream_h

#include <stdbool.h>
#include <stdint.h>

#include "util/vl_vlc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vl_h264_reader {
   struct vl_vlc vlc;

   /* De-escaped RBSP, owned.  vl_vlc keeps pointers into the inputs/sizes
    * arrays and advances them, so both live in the reader for its lifetime. */
   uint8_t *rbsp;
   unsigned rbsp_size;
   const void *inputs[1];
   unsigned sizes[1];

   /* Total RBSP bits and the position of the rbsp_stop_one_bit, so
    * more_rbsp_data can tell coded data from the trailing bits (sec 7.2). */
   unsigned total_bits;
   unsigned stop_bit_pos;

   /* Set once a read asks for more bits than the RBSP holds, i.e. the stream is
    * truncated or malformed.  Callers check it to reject the stream instead of
    * decoding trailing zeros as data. */
   bool overrun;
};

/*
 * Initialise the reader over one NAL's EBSP payload (the bytes after the start
 * code and the one-byte NAL header).  De-escapes 0x000003 -> 0x0000 into an owned
 * RBSP buffer and starts vl_vlc over it.  Returns false on allocation failure or
 * an empty payload, leaving the reader safe to fini.
 */
bool vl_h264_reader_init(struct vl_h264_reader *reader,
                         const uint8_t *ebsp, unsigned ebsp_size);

/* Release the owned RBSP scratch.  Safe on a zeroed or failed reader. */
void vl_h264_reader_fini(struct vl_h264_reader *reader);

/* u(n): n-bit unsigned, MSB first (sec 7.2).  n must be 0..32. */
unsigned vl_h264_u(struct vl_h264_reader *reader, unsigned num_bits);

/* ue(v): unsigned Exp-Golomb (sec 9.1). */
unsigned vl_h264_ue(struct vl_h264_reader *reader);

/* se(v): signed Exp-Golomb (sec 9.1.1). */
int vl_h264_se(struct vl_h264_reader *reader);

/* te(v): truncated Exp-Golomb (sec 9.1.1).  range is the syntax element's cMax:
 * for cMax == 1 it is a single inverted bit, otherwise it is ue(v). */
unsigned vl_h264_te(struct vl_h264_reader *reader, unsigned range);

/*
 * more_rbsp_data (sec 7.2): true while coded data remains before the
 * rbsp_stop_one_bit.  The per-macroblock loop ends when this turns false.
 */
bool vl_h264_more_rbsp_data(struct vl_h264_reader *reader);

/* Whether a read has run past the end of the RBSP (a truncated or malformed
 * stream).  Once set it stays set for the reader's lifetime. */
bool vl_h264_overrun(const struct vl_h264_reader *reader);

/* Bits consumed from the RBSP so far; pairs with total_bits for diagnostics. */
unsigned vl_h264_bits_consumed(struct vl_h264_reader *reader);

/*
 * Count and consume a run of zero bits up to the next one bit, returning the run
 * length and leaving the reader positioned just past that one bit.  This is the
 * Exp-Golomb prefix and, used directly, the CAVLC level_prefix (sec 9.2.2.1).
 * Spans vl_vlc refills so a long run is counted correctly.
 */
unsigned vl_h264_leading_zeros(struct vl_h264_reader *reader);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_bitstream_h */
