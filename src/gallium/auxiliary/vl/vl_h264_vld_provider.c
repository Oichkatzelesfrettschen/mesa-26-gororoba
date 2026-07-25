/*
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "util/macros.h"
#include "util/u_debug.h"

#include "vl_h264_vld_provider.h"

/*
 * The Mesa-native CAVLC entropy decoder is the active provider and is always
 * available.  CABAC is unimplemented, so that kind stays unavailable.  The FFMPEG
 * oracle kind is backed by the replay provider, available only when
 * R300_H264_CONTRACT_REPLAY names a serialized contract.
 */
bool
vl_h264_vld_provider_available(enum vl_h264_vld_provider_kind kind)
{
   switch (kind) {
   case VL_H264_VLD_PROVIDER_MESA_CAVLC:
      /* The clean-room CAVLC front end is built into libgalliumvl and carries no
       * external state, so it is always available. */
      return true;
   case VL_H264_VLD_PROVIDER_FFMPEG_ORACLE:
      return debug_get_option("R300_H264_CONTRACT_REPLAY", NULL) != NULL;
   case VL_H264_VLD_PROVIDER_MESA_CABAC:
   default:
      return false;
   }
}

struct vl_h264_vld_provider *
vl_h264_vld_provider_create(enum vl_h264_vld_provider_kind kind)
{
   switch (kind) {
   case VL_H264_VLD_PROVIDER_MESA_CAVLC:
      return vl_h264_cavlc_provider_create();
   case VL_H264_VLD_PROVIDER_FFMPEG_ORACLE:
      return vl_h264_replay_provider_create();
   case VL_H264_VLD_PROVIDER_MESA_CABAC:
   default:
      return NULL;
   }
}

/* The provider preference order.  The replay is an explicit developer override:
 * it is available only when R300_H264_CONTRACT_REPLAY names a contract, so
 * listing it first lets that override win when set and fall through to the
 * clean-room CAVLC front end otherwise. */
static const enum vl_h264_vld_provider_kind provider_preference[] = {
   VL_H264_VLD_PROVIDER_FFMPEG_ORACLE,
   VL_H264_VLD_PROVIDER_MESA_CAVLC,
   VL_H264_VLD_PROVIDER_MESA_CABAC,
};

bool
vl_h264_vld_provider_any_available(void)
{
   for (unsigned i = 0; i < ARRAY_SIZE(provider_preference); i++)
      if (vl_h264_vld_provider_available(provider_preference[i]))
         return true;
   return false;
}

struct vl_h264_vld_provider *
vl_h264_vld_provider_create_available(void)
{
   for (unsigned i = 0; i < ARRAY_SIZE(provider_preference); i++) {
      struct vl_h264_vld_provider *provider =
         vl_h264_vld_provider_create(provider_preference[i]);
      if (provider)
         return provider;
   }
   return NULL;
}
