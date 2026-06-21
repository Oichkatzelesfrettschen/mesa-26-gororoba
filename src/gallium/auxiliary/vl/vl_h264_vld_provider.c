/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "util/u_debug.h"

#include "vl_h264_vld_provider.h"

/*
 * The Mesa-native CAVLC and CABAC entropy decoders are not implemented yet, so
 * those kinds stay unavailable.  The FFMPEG oracle kind is backed by the replay
 * provider, which exists only when R300_H264_CONTRACT_REPLAY names a serialized
 * contract.  Reporting availability honestly keeps the decoder from advertising
 * a codec it cannot create: with no provider it stays fail-closed.
 */
bool
vl_h264_vld_provider_available(enum vl_h264_vld_provider_kind kind)
{
   switch (kind) {
   case VL_H264_VLD_PROVIDER_FFMPEG_ORACLE:
      return debug_get_option("R300_H264_CONTRACT_REPLAY", NULL) != NULL;
   case VL_H264_VLD_PROVIDER_MESA_CAVLC:
   case VL_H264_VLD_PROVIDER_MESA_CABAC:
   default:
      return false;
   }
}

struct vl_h264_vld_provider *
vl_h264_vld_provider_create(enum vl_h264_vld_provider_kind kind)
{
   switch (kind) {
   case VL_H264_VLD_PROVIDER_FFMPEG_ORACLE:
      return vl_h264_replay_provider_create();
   case VL_H264_VLD_PROVIDER_MESA_CAVLC:
   case VL_H264_VLD_PROVIDER_MESA_CABAC:
   default:
      return NULL;
   }
}
