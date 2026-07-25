/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Wire-format round trip and cross-language byte-equality for the H.264 contract.
 *
 * It builds a deterministic two-macroblock fixture -- one 4x4-transform and one
 * 8x8-transform macroblock with distinctive coefficients, motion vectors, and
 * modes -- serializes it, and checks three things: the C serializer and C
 * deserializer invert each other field for field (including coeff8x8), the
 * serialized bytes equal the golden file the steinmarder Python oracle produced
 * from the same fixture (proving C and Python agree byte for byte, so neither
 * silently drops the 8x8 path), and the deserializer reads the golden back into
 * the fixture.  The fixture builder here and h264_contract_wire_fixture() in the
 * Python oracle must stay identical.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vl_h264_contract_wire.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

#define MB_TYPE_INTRA4X4 0x00000001
#define MB_TYPE_8X8DCT   0x01000000

static void
fill_canonical_fixture(struct vl_h264_mb_contract *mbs,
                       struct vl_h264_slice_contract *slice)
{
   int m, b, k, c;

   for (m = 0; m < 2; m++) {
      struct vl_h264_mb_contract *mb = &mbs[m];

      mb->mb_x = m;
      mb->mb_y = 0;
      mb->slice_type = VL_H264_SLICE_I;
      mb->mb_type = (m == 1) ? (MB_TYPE_INTRA4X4 | MB_TYPE_8X8DCT)
                             : MB_TYPE_INTRA4X4;
      mb->qp_y = 20 + 3 * m;
      mb->qp_cb = 21 + 3 * m;
      mb->qp_cr = 22 + 3 * m;
      mb->transform_8x8 = (m == 1) ? 1 : 0;
      mb->cbp_luma = 0x0f;
      mb->cbp_chroma = 0x02;

      for (b = 0; b < VL_H264_TOTAL_4X4_BLOCKS; b++)
         for (k = 0; k < 16; k++)
            mb->coeff4x4[b][k] = (int16_t)(((m * 101 + b * 16 + k) % 401) - 200);
      for (b = 0; b < VL_H264_LUMA_8X8_BLOCKS; b++)
         for (k = 0; k < 64; k++)
            mb->coeff8x8[b][k] = (int16_t)(((m * 211 + b * 64 + k) % 401) - 200);
      for (b = 0; b < 16; b++) {
         for (c = 0; c < 2; c++) {
            mb->mv_l0[b][c] = (int16_t)(((m * 7 + b * 2 + c) % 33) - 16);
            mb->mv_l1[b][c] = (int16_t)(((m * 5 + b * 2 + c) % 33) - 16);
         }
         mb->ref_l0[b] = (int8_t)(((m + b) % 4) - 1);
         mb->ref_l1[b] = -1;
         mb->intra4x4_pred_mode[b] = (uint8_t)((m + b) % 9);
      }
      mb->intra_chroma_pred_mode = 2;
      mb->disable_deblock_idc = 0;
      mb->slice_alpha_c0_offset_div2 = 1;
      mb->slice_beta_offset_div2 = -1;
   }

   slice->version = VL_H264_MB_CONTRACT_VERSION;
   slice->width = 32;
   slice->height = 16;
   slice->slice_type = VL_H264_SLICE_I;
   slice->provider = VL_H264_VLD_PROVIDER_FFMPEG_ORACLE;
   slice->coeff_contract = VL_H264_COEFF_DEQUANTIZED;
   slice->num_macroblocks = 2;
   slice->macroblocks = mbs;
}

static int
mb_equal(const struct vl_h264_mb_contract *a, const struct vl_h264_mb_contract *b)
{
   return memcmp(a, b, sizeof(*a)) == 0;
}

int
main(int argc, char **argv)
{
   struct vl_h264_mb_contract mbs[2];
   struct vl_h264_slice_contract slice, replay;
   uint8_t buf[2 * VL_H264_CONTRACT_WIRE_MB_BYTES + VL_H264_CONTRACT_WIRE_HEADER_BYTES];
   size_t written;
   unsigned m;

   memset(mbs, 0, sizeof(mbs));
   fill_canonical_fixture(mbs, &slice);

   /* Serialize, and confirm the size matches the documented layout. */
   written = vl_h264_contract_serialize(&slice, buf, sizeof(buf));
   CHECK(written == vl_h264_contract_wire_size(slice.num_macroblocks));
   CHECK(written == sizeof(buf));

   /* C serializer and deserializer invert each other, field for field. */
   CHECK(vl_h264_contract_deserialize(buf, written, &replay));
   CHECK(replay.version == slice.version);
   CHECK(replay.width == slice.width && replay.height == slice.height);
   CHECK(replay.slice_type == slice.slice_type);
   CHECK(replay.provider == slice.provider);
   CHECK(replay.coeff_contract == slice.coeff_contract);
   CHECK(replay.num_macroblocks == slice.num_macroblocks);
   for (m = 0; m < slice.num_macroblocks; m++)
      CHECK(mb_equal(&replay.macroblocks[m], &slice.macroblocks[m]));
   free(replay.macroblocks);

   /* Cross-language: the bytes must equal the golden the Python oracle emitted
    * from the identical fixture, and deserializing the golden must reproduce
    * the fixture. */
   if (argc > 1) {
      FILE *f = fopen(argv[1], "rb");
      uint8_t golden[sizeof(buf)];
      size_t got;

      CHECK(f != NULL);
      got = fread(golden, 1, sizeof(golden), f);
      CHECK(fgetc(f) == EOF);   /* golden is exactly buf-sized, no trailing bytes */
      fclose(f);
      CHECK(got == written);
      CHECK(memcmp(golden, buf, written) == 0);

      CHECK(vl_h264_contract_deserialize(golden, got, &replay));
      CHECK(replay.num_macroblocks == slice.num_macroblocks);
      for (m = 0; m < slice.num_macroblocks; m++)
         CHECK(mb_equal(&replay.macroblocks[m], &slice.macroblocks[m]));
      free(replay.macroblocks);
      printf("vl_h264_contract_wire: round trip + golden cross-language PASS\n");
   } else {
      printf("vl_h264_contract_wire: round trip PASS (no golden given)\n");
   }
   return 0;
}
