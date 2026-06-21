/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"

#include "compiler/nir/nir_builder.h"

#include "vl_nir.h"
#include "vl_h264_mc.h"

/* The H.264 six-tap half-pel FIR weights (ITU-T H.264 sec 8.4.2.2.1), applied
 * to the six integer samples centered on the half-pel position. */
static const float luma_6tap[6] = { 1.0f, -5.0f, 20.0f, 20.0f, -5.0f, 1.0f };

/* Arithmetic right shift on the shiftless FP24 fragment ALU: z >> k is
 * floor(z * 2^-k), realized as scale, FRC, subtract.  FRC floors toward minus
 * infinity, so this matches a two's-complement arithmetic shift; the half-pel
 * accumulator peaks at 10710, well under the FP24 integer-exact ceiling 2^17,
 * so the shift is bit-exact. */
static nir_def *
fp24_floor_shift(nir_builder *b, nir_def *value, unsigned shift)
{
   nir_def *scaled = nir_fmul(b, value,
                              nir_imm_float(b, 1.0f / (float)(1u << shift)));
   return nir_fadd(b, scaled, nir_fneg(b, nir_ffract(b, scaled)));
}

/* The six-tap MAC, normalize, and clip shared by the two axes.  base is the
 * fragment's sample coordinate; step_vec is the per-tap stride (the texel step
 * along the filtered axis, zero on the other).  Every product and partial sum
 * is an exact integer under 2^17 (the accumulator peaks at 255*42 = 10710,
 * since the -5 taps cannot see a negative unsigned sample), so MAD fusion of a
 * multiply and add is value-identical and the result matches the integer spec
 * exactly. */
static nir_def *
halfpel_filter(struct vl_nir_fs *fs, nir_def *base, nir_def *step_vec)
{
   nir_builder *b = &fs->b;
   nir_def *acc = nir_imm_float(b, 0.0f);

   for (unsigned tap = 0; tap < 6; ++tap) {
      nir_def *offset =
         nir_fmul(b, step_vec, nir_imm_float(b, (float)tap - 2.0f));
      nir_def *coord = nir_fadd(b, base, offset);
      nir_def *sample = nir_channel(b, vl_nir_tex(fs, 0, coord), 0);
      acc = nir_fadd(b, acc,
                     nir_fmul(b, sample, nir_imm_float(b, luma_6tap[tap])));
   }

   /* (acc + 16) >> 5, then Clip1Y to [0, 255]. */
   nir_def *normalized =
      fp24_floor_shift(b, nir_fadd(b, acc, nir_imm_float(b, 16.0f)), 5);
   nir_def *clipped = nir_fmin(b, nir_fmax(b, normalized,
                                           nir_imm_float(b, 0.0f)),
                               nir_imm_float(b, 255.0f));
   return nir_replicate(b, clipped, 4);
}

/* The sample base is texcoord.xy; the per-tap stride is the horizontal texel
 * step (.z) for position b and the vertical step (.w) for position h, with the
 * other lane zero so every tap stays on the fragment's own row or column. */
static nir_def *
build_halfpel_color(struct vl_nir_fs *fs, bool horizontal)
{
   nir_builder *b = &fs->b;
   nir_def *tc = fs->texcoord[0];
   nir_def *base = nir_vec2(b, nir_channel(b, tc, 0), nir_channel(b, tc, 1));
   nir_def *step_vec = horizontal
      ? nir_vec2(b, nir_channel(b, tc, 2), nir_imm_float(b, 0.0f))
      : nir_vec2(b, nir_imm_float(b, 0.0f), nir_channel(b, tc, 3));
   return halfpel_filter(fs, base, step_vec);
}

void *
vl_h264_mc_create_halfpel_h_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_mc_halfpel_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_halfpel_color(&fs, true));
}

void *
vl_h264_mc_create_halfpel_v_fs(struct pipe_context *pipe)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin(&fs, pipe, 1, "vl:h264_mc_halfpel_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish(&fs, pipe, build_halfpel_color(&fs, false));
}

/* NIR-only entry points for the r300 compile-budget gate: the same half-pel
 * kernels built with explicit options and no live screen. */
nir_shader *
vl_h264_mc_halfpel_h_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 1, "vl:h264_mc_halfpel_h_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_halfpel_color(&fs, true));
}

nir_shader *
vl_h264_mc_halfpel_v_nir(const nir_shader_compiler_options *options)
{
   struct vl_nir_fs fs;
   vl_nir_fs_begin_opts(&fs, options, 1, "vl:h264_mc_halfpel_v_fs");
   vl_nir_sampler(&fs, 0, GLSL_SAMPLER_DIM_2D);
   return vl_nir_fs_finish_nir(&fs, build_halfpel_color(&fs, false));
}
