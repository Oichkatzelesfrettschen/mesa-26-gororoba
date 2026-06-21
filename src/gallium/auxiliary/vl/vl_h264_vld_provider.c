/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "vl_h264_vld_provider.h"

/*
 * No CPU variable-length-decode front end is implemented yet.  The Mesa-native
 * CAVLC and CABAC providers and the libavcodec oracle each land separately, and
 * each registers here when it does.  Until then the factory returns NULL, which
 * keeps the decoder from advertising H.264 it cannot actually decode: a build
 * with no usable entropy decoder stays fail-closed rather than accepting frames
 * and producing garbage.
 */
bool
vl_h264_vld_provider_available(enum vl_h264_vld_provider_kind kind)
{
   switch (kind) {
   case VL_H264_VLD_PROVIDER_FFMPEG_ORACLE:
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
   case VL_H264_VLD_PROVIDER_MESA_CAVLC:
   case VL_H264_VLD_PROVIDER_MESA_CABAC:
   default:
      return NULL;
   }
}
