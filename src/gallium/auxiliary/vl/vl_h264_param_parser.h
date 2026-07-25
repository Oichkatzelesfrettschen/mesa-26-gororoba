/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Sequence and picture parameter set parsing for the Mesa-native H.264 front
 * end.  A real decoder reads its dimensions, reference count, entropy mode, and
 * QP offsets from the bitstream's SPS and PPS (ITU-T H.264 sec 7.3.2.1, 7.3.2.2)
 * rather than assuming them.  Parsing the headers makes the front end and its
 * tests track the clip instead of carrying brittle hand-set parameters -- the
 * chroma_qp_index_offset that a hand-set value once got wrong is read straight
 * from the picture parameter set here.
 *
 * Scope is the Baseline / Constrained Baseline streams the r300 path targets: a
 * single slice group, no high-profile scaling lists.  The parser rejects an
 * SPS or PPS that uses those features instead of misreading it.
 */

#ifndef vl_h264_param_parser_h
#define vl_h264_param_parser_h

#include "pipe/p_video_state.h"

#include "vl_h264_bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse a sequence parameter set RBSP (the NAL payload after the one-byte
 * header) into sps.  Returns false on a truncated stream or an unsupported
 * feature (separate colour planes, scaling lists, multiple slice groups).
 */
bool vl_h264_parse_sps(struct vl_h264_reader *reader, struct pipe_h264_sps *sps);

/*
 * Parse a picture parameter set RBSP into pps; sps is the already-parsed
 * sequence parameter set the picture parameter set refers to, stored in
 * pps->sps.  Returns false on a truncated stream or an unsupported feature.
 */
bool vl_h264_parse_pps(struct vl_h264_reader *reader, struct pipe_h264_sps *sps,
                       struct pipe_h264_pps *pps);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_param_parser_h */
