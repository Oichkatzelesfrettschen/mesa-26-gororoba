/*
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_file.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

#include "vl_h264_contract_wire.h"
#include "vl_h264_vld_provider.h"

/*
 * Replay provider: instead of entropy-decoding the bitstream, it reads a
 * pre-serialized per-macroblock contract from the file named by
 * R300_H264_CONTRACT_REPLAY and hands those macroblocks to the back half.  It is
 * the bring-up harness for the FP24 back half -- it lets the GPU reconstruction
 * run on the exact contracts the libavcodec oracle (or a future Mesa-native
 * provider) produced, with no entropy decoder in the loop.  It is dev-only and
 * not a real VAAPI decode path: it ignores the slice NAL and replays the file.
 */

struct vl_h264_replay_priv {
   struct vl_h264_slice_contract slice;
};

static bool
vl_h264_replay_decode_slice(struct vl_h264_vld_provider *provider,
                            const struct pipe_h264_picture_desc *picture,
                            const uint8_t *nal, unsigned nal_size,
                            struct vl_h264_slice_contract *out)
{
   const struct vl_h264_replay_priv *priv = provider->priv;
   uint32_t count;

   /* The replay file stands in for the entropy decode, so the live slice NAL is
    * deliberately unused. */
   (void) picture;
   (void) nal;
   (void) nal_size;

   /* The provider owns the slice type: carry it from the serialized contract so
    * the frame records P/I rather than the picture's profile. */
   out->slice_type = priv->slice.slice_type;

   /* Copy the replayed macroblocks into the frame contract at their raster
    * index, ignoring any that fall outside the frame the decoder sized. */
   count = priv->slice.num_macroblocks;
   if (count > out->num_macroblocks)
      count = out->num_macroblocks;
   memcpy(out->macroblocks, priv->slice.macroblocks,
          (size_t)count * sizeof(*out->macroblocks));
   return true;
}

static void
vl_h264_replay_destroy(struct vl_h264_vld_provider *provider)
{
   struct vl_h264_replay_priv *priv = provider->priv;

   if (priv) {
      FREE(priv->slice.macroblocks);
      FREE(priv);
   }
   FREE(provider);
}

struct vl_h264_vld_provider *
vl_h264_replay_provider_create(void)
{
   struct vl_h264_vld_provider *provider;
   struct vl_h264_replay_priv *priv;
   const char *path;
   char *bytes;
   size_t size = 0;
   bool ok;

   path = debug_get_option("R300_H264_CONTRACT_REPLAY", NULL);
   if (!path)
      return NULL;

   bytes = os_read_file(path, &size);
   if (!bytes)
      return NULL;

   priv = CALLOC_STRUCT(vl_h264_replay_priv);
   provider = CALLOC_STRUCT(vl_h264_vld_provider);
   if (!priv || !provider) {
      free(bytes);
      FREE(priv);
      FREE(provider);
      return NULL;
   }

   ok = vl_h264_contract_deserialize((const uint8_t *)bytes, size, &priv->slice);
   free(bytes);
   if (!ok) {
      FREE(priv);
      FREE(provider);
      return NULL;
   }

   provider->kind = VL_H264_VLD_PROVIDER_FFMPEG_ORACLE;
   provider->priv = priv;
   provider->decode_slice = vl_h264_replay_decode_slice;
   provider->destroy = vl_h264_replay_destroy;
   return provider;
}
