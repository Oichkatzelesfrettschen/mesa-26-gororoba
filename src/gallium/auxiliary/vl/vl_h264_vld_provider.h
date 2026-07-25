/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Swappable CPU variable-length-decode provider for the r300 FP24 H.264 decoder.
 *
 * H.264 has no fixed-function entropy decode on R300-class hardware, so the
 * serial CAVLC/CABAC pass runs on the CPU and the GPU does only the
 * fixed-function back half (inverse transform, motion compensation, deblock).
 * A provider is the CPU half: it takes one slice's raw NAL bytes plus the
 * picture parameters and emits per-macroblock contract records
 * (vl_h264_mb_contract) the back half consumes.  The decoder talks only to this
 * interface, so the entropy front end is replaceable without touching the GPU
 * path -- a libavcodec oracle for bring-up, then clean-room Mesa-native CAVLC
 * and CABAC.
 *
 * The factory returns NULL when no working provider exists for the requested
 * kind.  That is deliberate: the decoder advertises H.264 only when a provider
 * initialises, so a build with no usable entropy decoder stays fail-closed
 * rather than accepting frames it cannot decode.
 */

#ifndef vl_h264_vld_provider_h
#define vl_h264_vld_provider_h

#include <stdbool.h>
#include <stdint.h>

#include "vl_h264_mb_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_h264_picture_desc;

struct vl_h264_vld_provider {
   enum vl_h264_vld_provider_kind kind;
   void *priv;

   /*
    * Entropy-decode one slice.  nal points at the slice's raw Annex B NAL bytes
    * (the VA frontend prepends the start code), nal_size is their length, and
    * picture carries the active SPS/PPS and slice parameters.  The decoded
    * macroblocks are written into out->macroblocks at their raster index
    * (mb_y * width_in_mbs + mb_x); out is sized for the whole frame by the
    * decoder before the first slice.  Returns true when the slice decoded.
    */
   bool (*decode_slice)(struct vl_h264_vld_provider *provider,
                        const struct pipe_h264_picture_desc *picture,
                        const uint8_t *nal, unsigned nal_size,
                        struct vl_h264_slice_contract *out);

   void (*destroy)(struct vl_h264_vld_provider *provider);
};

/*
 * Whether a working provider exists for kind, without constructing it.  The
 * driver advertises H.264 only when this is true, so a build with no usable
 * entropy decoder never reports a codec it cannot create.  Cheap enough to call
 * from the screen's get_video_param.
 */
bool
vl_h264_vld_provider_available(enum vl_h264_vld_provider_kind kind);

/*
 * Create the provider for kind, or NULL when none is available in this build.
 * The Mesa-native CAVLC/CABAC providers are not implemented yet; the FFMPEG
 * oracle kind is backed by the replay provider, which exists only when
 * R300_H264_CONTRACT_REPLAY names a serialized contract.
 */
struct vl_h264_vld_provider *
vl_h264_vld_provider_create(enum vl_h264_vld_provider_kind kind);

/*
 * Replay provider constructor: reads the serialized contract named by
 * R300_H264_CONTRACT_REPLAY and replays it instead of decoding the bitstream.
 * Returns NULL when the environment is unset or the file is missing or malformed.
 * Bring-up only -- it feeds the back half real contracts with no entropy decoder.
 */
struct vl_h264_vld_provider *
vl_h264_replay_provider_create(void);

/*
 * Whether any provider is available.  The clean-room CAVLC front end is always
 * available, so this is true whenever H.264 is built; the screen gates H.264 on
 * it.
 */
bool
vl_h264_vld_provider_any_available(void);

/*
 * Clean-room Mesa-native CAVLC provider constructor: decodes a slice's NAL bytes
 * into the macroblock contract directly.  It carries no external state and is the
 * default provider when no replay override is set.
 */
struct vl_h264_vld_provider *
vl_h264_cavlc_provider_create(void);

/*
 * Create the most preferred available provider: the R300_H264_CONTRACT_REPLAY
 * override when it names a contract, otherwise the clean-room CAVLC front end, or
 * NULL when none is available.  The decoder uses this rather than naming a kind.
 */
struct vl_h264_vld_provider *
vl_h264_vld_provider_create_available(void);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_vld_provider_h */
