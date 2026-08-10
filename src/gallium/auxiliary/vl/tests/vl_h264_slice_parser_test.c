/*
 * SPDX-License-Identifier: MIT
 */

/*
 * The clean-room slice-header parser test parses the IDR slice of a real
 * Constrained-Baseline QCIF clip and asserts every retained field against
 * the values an independent ffmpeg trace_headers decode reports for the same
 * NAL.  The SPS/PPS are the clip's, transcribed from that trace; the slice NAL
 * (h264_cb_idr_slice.bin) is the clip's first IDR NAL with its one-byte header.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_video_state.h"

#include "vl_h264_slice_parser.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

/* SPS/PPS of the fixture clip, from its ffmpeg trace_headers decode. */
static void
fill_fixture_sps_pps(struct pipe_h264_sps *sps, struct pipe_h264_pps *pps)
{
   memset(sps, 0, sizeof(*sps));
   sps->chroma_format_idc = 1;                  /* 4:2:0 */
   sps->log2_max_frame_num_minus4 = 0;          /* frame_num is 4 bits */
   sps->pic_order_cnt_type = 2;                 /* no POC in the slice header */
   sps->frame_mbs_only_flag = 1;
   sps->max_num_ref_frames = 1;
   sps->pic_width_in_mbs_minus1 = 10;           /* 11 MBs (176 px) */
   sps->pic_height_in_mbs_minus1 = 8;           /* 9 MBs (144 px) */

   memset(pps, 0, sizeof(*pps));
   pps->sps = sps;
   pps->entropy_coding_mode_flag = 0;           /* CAVLC */
   pps->num_ref_idx_l0_default_active_minus1 = 0;
   pps->pic_init_qp_minus26 = -3;               /* base QP 23 */
   pps->deblocking_filter_control_present_flag = 1;
}

int
main(int argc, char **argv)
{
   struct pipe_h264_sps sps;
   struct pipe_h264_pps pps;
   struct pipe_h264_picture_desc pic;
   struct vl_h264_slice_header sh;
   struct vl_h264_reader reader;
   uint8_t *nal;
   long nal_size;
   FILE *f;

   CHECK(argc > 1);
   f = fopen(argv[1], "rb");
   CHECK(f != NULL);
   fseek(f, 0, SEEK_END);
   nal_size = ftell(f);
   fseek(f, 0, SEEK_SET);
   CHECK(nal_size > 1);
   nal = malloc(nal_size);
   CHECK(nal != NULL);
   CHECK(fread(nal, 1, nal_size, f) == (size_t)nal_size);
   fclose(f);

   /* The asset starts at the one-byte NAL header (the provider strips the start
    * code).  nal_ref_idc and nal_unit_type come from that byte; the reader runs
    * over the EBSP after it. */
   unsigned nal_ref_idc = (nal[0] >> 5) & 3;
   unsigned nal_unit_type = nal[0] & 0x1f;
   CHECK(nal_ref_idc == 3);
   CHECK(nal_unit_type == 5);   /* IDR slice */

   fill_fixture_sps_pps(&sps, &pps);
   memset(&pic, 0, sizeof(pic));
   pic.pps = &pps;

   CHECK(vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, nal_ref_idc, nal_unit_type,
                                    &sh));

   /* Every value below is the ffmpeg trace_headers ground truth for this NAL. */
   CHECK(sh.first_mb_in_slice == 0);
   CHECK(sh.slice_type == VL_H264_SLICE_I);   /* raw 7 -> 7 % 5 == 2 (I) */
   CHECK(sh.idr);
   CHECK(sh.pic_parameter_set_id == 0);
   CHECK(sh.frame_num == 0);
   CHECK(sh.idr_pic_id == 0);
   CHECK(sh.slice_qp == 27);                  /* 26 + (-3) + 4 */
   CHECK(sh.disable_deblocking_filter_idc == 0);
   CHECK(sh.slice_alpha_c0_offset_div2 == 0);
   CHECK(sh.slice_beta_offset_div2 == 0);
   CHECK(sh.num_ref_idx_l0_active == 1);

   vl_h264_reader_fini(&reader);

   /* These non-IDR I-slice controls encode the exact SPS boundary widths.  The
    * first has frame_num width 16 and pic_order_cnt_type 2.  The second has
    * frame_num width 4, pic_order_cnt_type 0, and POC-LSB width 16. */
   static const uint8_t max_frame_num_slice[] = { 0xb8, 0x00, 0x06 };
   static const uint8_t max_poc_lsb_slice[] = { 0xb8, 0x00, 0x00, 0x60 };
   static const struct {
      const uint8_t *slice;
      unsigned slice_size;
      uint8_t log2_max_frame_num_minus4;
      uint8_t pic_order_cnt_type;
      uint8_t log2_max_pic_order_cnt_lsb_minus4;
   } valid_sps_cases[] = {
      { max_frame_num_slice, sizeof(max_frame_num_slice), 12, 2, 0 },
      { max_poc_lsb_slice, sizeof(max_poc_lsb_slice), 0, 0, 12 },
   };
   for (unsigned i = 0;
        i < sizeof(valid_sps_cases) / sizeof(valid_sps_cases[0]); i++) {
      fill_fixture_sps_pps(&sps, &pps);
      pps.deblocking_filter_control_present_flag = 0;
      sps.log2_max_frame_num_minus4 =
         valid_sps_cases[i].log2_max_frame_num_minus4;
      sps.pic_order_cnt_type = valid_sps_cases[i].pic_order_cnt_type;
      sps.log2_max_pic_order_cnt_lsb_minus4 =
         valid_sps_cases[i].log2_max_pic_order_cnt_lsb_minus4;
      pic.pps = &pps;

      CHECK(vl_h264_reader_init(&reader, valid_sps_cases[i].slice,
                                valid_sps_cases[i].slice_size));
      CHECK(vl_h264_parse_slice_header(&reader, &pic, 0, 1, &sh));
      CHECK(sh.first_mb_in_slice == 0);
      CHECK(sh.slice_type == VL_H264_SLICE_I);
      CHECK(!sh.idr);
      CHECK(sh.pic_parameter_set_id == 0);
      CHECK(sh.frame_num == 0);
      CHECK(sh.slice_qp == 23);
      CHECK(sh.num_reorder_l0 == 0);
      CHECK(!vl_h264_overrun(&reader));
      vl_h264_reader_fini(&reader);
   }

   /* ITU-T H.264 sec 7.4.2.1.1 bounds these SPS fields at 12.  Each invalid
    * value is rejected before the parser consumes any slice-header bits. */
   static const struct {
      const uint8_t *slice;
      unsigned slice_size;
      uint8_t log2_max_frame_num_minus4;
      uint8_t pic_order_cnt_type;
      uint8_t log2_max_pic_order_cnt_lsb_minus4;
   } invalid_sps_cases[] = {
      { max_frame_num_slice, sizeof(max_frame_num_slice), 13, 2, 0 },
      { max_poc_lsb_slice, sizeof(max_poc_lsb_slice), 0, 3, 0 },
      { max_poc_lsb_slice, sizeof(max_poc_lsb_slice), 0, 0, 13 },
   };
   for (unsigned i = 0;
        i < sizeof(invalid_sps_cases) / sizeof(invalid_sps_cases[0]); i++) {
      fill_fixture_sps_pps(&sps, &pps);
      pps.deblocking_filter_control_present_flag = 0;
      sps.log2_max_frame_num_minus4 =
         invalid_sps_cases[i].log2_max_frame_num_minus4;
      sps.pic_order_cnt_type = invalid_sps_cases[i].pic_order_cnt_type;
      sps.log2_max_pic_order_cnt_lsb_minus4 =
         invalid_sps_cases[i].log2_max_pic_order_cnt_lsb_minus4;
      pic.pps = &pps;

      CHECK(vl_h264_reader_init(&reader, invalid_sps_cases[i].slice,
                                invalid_sps_cases[i].slice_size));
      CHECK(!vl_h264_parse_slice_header(&reader, &pic, 0, 1, &sh));
      CHECK(vl_h264_bits_consumed(&reader) == 0);
      CHECK(!vl_h264_overrun(&reader));
      vl_h264_reader_fini(&reader);
   }

   /* The P-slice bits are 1|1|1|0000|0|1|00100|1|1, where the fields are
    * first_mb_in_slice, slice_type, pps_id, frame_num, override flag,
    * ref_pic_list_modification_flag_l0, idc=3, slice_qp_delta, and the
    * rbsp_stop_one_bit.  This control has a valid list terminator. */
   static const uint8_t ref_list_terminator[] = { 0xe0, 0x93 };
   fill_fixture_sps_pps(&sps, &pps);
   pps.deblocking_filter_control_present_flag = 0;
   pic.pps = &pps;
   CHECK(vl_h264_reader_init(&reader, ref_list_terminator,
                             sizeof(ref_list_terminator)));
   CHECK(vl_h264_parse_slice_header(&reader, &pic, 0, 1, &sh));
   CHECK(sh.slice_type == VL_H264_SLICE_P);
   CHECK(sh.num_reorder_l0 == 0);
   CHECK(vl_h264_bits_consumed(&reader) == 15);
   CHECK(!vl_h264_overrun(&reader));
   vl_h264_reader_fini(&reader);

   /* The invalid P-slice fixture encodes idc=4 followed by idc=3.  The
    * parser rejects the invalid modification ID after consuming its code. */
   static const uint8_t invalid_ref_list_idc[] = { 0xe0, 0x94, 0x98 };
   CHECK(vl_h264_reader_init(&reader, invalid_ref_list_idc,
                             sizeof(invalid_ref_list_idc)));
   CHECK(!vl_h264_parse_slice_header(&reader, &pic, 0, 1, &sh));
   CHECK(vl_h264_bits_consumed(&reader) == 14);
   CHECK(!vl_h264_overrun(&reader));
   vl_h264_reader_fini(&reader);

   /* The malformed P-slice fixture ends after
    * ref_pic_list_modification_flag_l0.  The stop bit follows that flag, so
    * sec 7.3.3.1's required idc=3 is absent.
    * The parser rejects the missing terminator at the RBSP boundary instead of
    * reading alignment bits indefinitely. */
   static const uint8_t missing_ref_list_terminator[] = { 0xe0, 0xc0 };
   CHECK(vl_h264_reader_init(&reader, missing_ref_list_terminator,
                             sizeof(missing_ref_list_terminator)));
   CHECK(!vl_h264_parse_slice_header(&reader, &pic, 0, 1, &sh));
   CHECK(vl_h264_bits_consumed(&reader) == 9);
   CHECK(!vl_h264_overrun(&reader));
   vl_h264_reader_fini(&reader);

   /* The CAVLC-only parser rejects a CABAC PPS before consuming slice syntax. */
   pps.entropy_coding_mode_flag = 1;
   CHECK(vl_h264_reader_init(&reader, nal + 1, (unsigned)(nal_size - 1)));
   CHECK(!vl_h264_parse_slice_header(&reader, &pic, nal_ref_idc, nal_unit_type,
                                     &sh));
   CHECK(vl_h264_bits_consumed(&reader) == 0);

   vl_h264_reader_fini(&reader);
   free(nal);
   printf("vl_h264_slice_parser: IDR ground truth and malformed-input bounds PASS\n");
   return 0;
}
