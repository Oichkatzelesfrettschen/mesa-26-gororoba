/*
 * SPDX-License-Identifier: MIT
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"

#include "compiler/nir/nir_builder.h"

#include "vl_nir.h"
#include "vl_h264_reconstruct.h"

/* Clip1(prediction + residual): read the prediction (binding 0) and the signed
 * residual (binding 1) at the sample position and saturate the sum to [0, 255].
 * The prediction is in [0, 255] and the residual in roughly [-255, 255], so the
 * sum stays far under the FP24 integer-exact ceiling 2^17 and the clip is the
 * MOV_SAT-shaped min/max. */
static nir_def *
build_reconstruct_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = nir_vec2(b, nir_channel(b, fs->texcoord[0], 0),
                          nir_channel(b, fs->texcoord[0], 1));
   nir_def *pred = nir_channel(b, vl_nir_tex(fs, 0, tc), 0);
   nir_def *resid = nir_channel(b, vl_nir_tex(fs, 1, tc), 0);
   nir_def *recon = nir_fmin(b,
      nir_fmax(b, nir_fadd(b, pred, resid), nir_imm_float(b, 0.0f)),
      nir_imm_float(b, 255.0f));
   return nir_replicate(b, recon, 4);
}

void *
vl_h264_reconstruct_create_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_reconstruct_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   vl_nir_sampler(&fs, 1, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_reconstruct_color(&fs));
}

nir_shader *
vl_h264_reconstruct_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 1, "vl:h264_reconstruct_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   vl_nir_sampler(&fs, 1, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_reconstruct_color(&fs));
}
