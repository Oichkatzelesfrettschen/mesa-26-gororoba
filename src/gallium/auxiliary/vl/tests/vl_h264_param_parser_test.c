/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Parse the fixture clip's own sequence and picture parameter sets and check the
 * decoder parameters against the values the clip actually carries.  The point is
 * that the front end reads these from the bitstream rather than carrying them by
 * hand: chroma_qp_index_offset is -2 here, and a hand-set 0 once mis-scaled every
 * chroma coefficient.  Arguments: the SPS NAL and the PPS NAL.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "pipe/p_video_state.h"

#include "vl_h264_param_parser.h"

#define CHECK(cond) do {                                                     \
   if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      return 1;                                                              \
   }                                                                         \
} while (0)

static uint8_t *
read_file(const char *path, long *size)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   *size = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint8_t *data = malloc(*size);
   if (data && fread(data, 1, *size, f) != (size_t)*size) {
      free(data);
      data = NULL;
   }
   fclose(f);
   return data;
}

int
main(int argc, char **argv)
{
   struct pipe_h264_sps sps;
   struct pipe_h264_pps pps;
   struct vl_h264_reader reader;
   uint8_t *sps_nal, *pps_nal;
   long sps_size, pps_size;

   CHECK(argc > 2);
   sps_nal = read_file(argv[1], &sps_size);
   CHECK(sps_nal && sps_size > 1);
   pps_nal = read_file(argv[2], &pps_size);
   CHECK(pps_nal && pps_size > 1);

   /* The RBSP is the NAL payload after the one-byte header. */
   CHECK(vl_h264_reader_init(&reader, sps_nal + 1, (unsigned)(sps_size - 1)));
   CHECK(vl_h264_parse_sps(&reader, &sps));
   vl_h264_reader_fini(&reader);

   CHECK(vl_h264_reader_init(&reader, pps_nal + 1, (unsigned)(pps_size - 1)));
   CHECK(vl_h264_parse_pps(&reader, &sps, &pps));
   vl_h264_reader_fini(&reader);

   /* QCIF is 11x9 macroblocks, progressive, single reference, 4:2:0. */
   CHECK(sps.pic_width_in_mbs_minus1 + 1 == 11);
   CHECK(sps.pic_height_in_mbs_minus1 + 1 == 9);
   CHECK(sps.frame_mbs_only_flag == 1);
   CHECK(sps.max_num_ref_frames == 1);
   CHECK(sps.chroma_format_idc == 1);
   CHECK(sps.pic_order_cnt_type == 2);

   /* CAVLC, single reference, and the QP parameters the chroma path depends on. */
   CHECK(pps.entropy_coding_mode_flag == 0);
   CHECK(pps.num_ref_idx_l0_default_active_minus1 == 0);
   CHECK(pps.pic_init_qp_minus26 == -3);
   CHECK(pps.chroma_qp_index_offset == -2);
   CHECK(pps.second_chroma_qp_index_offset == -2);
   CHECK(pps.deblocking_filter_control_present_flag == 1);

   free(sps_nal);
   free(pps_nal);
   printf("vl_h264_param_parser: SPS and PPS parsed from the bitstream, decoder "
          "parameters match the clip (chroma_qp_index_offset -2) PASS\n");
   return 0;
}
