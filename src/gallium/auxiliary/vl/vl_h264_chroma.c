/*
 * SPDX-License-Identifier: MIT
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"

#include "compiler/nir/nir_builder.h"

#include "vl_nir.h"
#include "vl_h264_chroma.h"

/* Arithmetic right shift on the shiftless FP24 fragment ALU: z >> k is
 * floor(z * 2^-k) via scale, FRC, subtract.  The bilinear accumulator peaks at
 * 64*255 = 16320, well under the FP24 integer-exact ceiling 2^17, so the shift
 * is bit-exact. */
static nir_def *
fp24_floor_shift(nir_builder *b, nir_def *value, unsigned shift)
{
   nir_def *scaled = nir_fmul(b, value,
                              nir_imm_float(b, 1.0f / (float)(1u << shift)));
   return nir_fadd(b, scaled, nir_fneg(b, nir_ffract(b, scaled)));
}

/* The eighth-pel bilinear blend.  The 2x2 neighborhood is read NEAREST at the
 * sample position (A) and one texel right (B), down (C), and diagonal (D); the
 * four integer weights arrive in the second varying.  Each weighted term is a
 * separate multiply and add, grouped as (wA*A + wB*B) + (wC*C + wD*D) to match
 * the validated FP24 model; every product and partial sum is an exact integer
 * under 2^17. */
static nir_def *
build_chroma_color(struct vl_nir_fs *fs)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *w = fs->texcoord[1];
   nir_def *base = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *step_x = nir_channel(b, tc, 2);
   nir_def *step_y = nir_channel(b, tc, 3);
   nir_def *zero = nir_imm_float(b, 0.0f);

   nir_def *a = nir_channel(b, vl_nir_tex(fs, 0, base), 0);
   nir_def *bb = nir_channel(b,
      vl_nir_tex(fs, 0, nir_fadd(b, base, nir_vec2(b, step_x, zero))), 0);
   nir_def *c = nir_channel(b,
      vl_nir_tex(fs, 0, nir_fadd(b, base, nir_vec2(b, zero, step_y))), 0);
   nir_def *d = nir_channel(b,
      vl_nir_tex(fs, 0, nir_fadd(b, base, nir_vec2(b, step_x, step_y))), 0);

   nir_def *top = nir_fadd(b, nir_fmul(b, nir_channel(b, w, 0), a),
                           nir_fmul(b, nir_channel(b, w, 1), bb));
   nir_def *bot = nir_fadd(b, nir_fmul(b, nir_channel(b, w, 2), c),
                           nir_fmul(b, nir_channel(b, w, 3), d));
   nir_def *acc = nir_fadd(b, top, bot);

   nir_def *result =
      fp24_floor_shift(b, nir_fadd(b, acc, nir_imm_float(b, 32.0f)), 6);
   return nir_replicate(b, result, 4);
}

void *
vl_h264_chroma_create_bilinear_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 2, "vl:h264_chroma_bilinear_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_chroma_color(&fs));
}

nir_shader *
vl_h264_chroma_bilinear_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 2, "vl:h264_chroma_bilinear_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_chroma_color(&fs));
}
