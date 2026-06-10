/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_dp4_fs_nir.h"

#include "compiler/nir/nir_builder.h"

/* Pure-NIR DP4 fragment program (no ureg/TGSI -- r300g's nir_to_rc consumes the
 * NIR directly).  Samples in_a, in_b at the fullscreen texcoord and writes
 * dot(a,b) to the RB3D color export; the dot lowers to the US DP4 instruction,
 * byte-exact for <=7-bit (quantized) operands (4*127^2=64516<2^17, hardware-
 * confirmed).  Built with the same nir_builder pattern as the vl_nir FS helpers
 * (compiler/nir/nir_builder.h); gather_info + assign_io_var_locations run here
 * because nir_to_rc keys interpolators off driver_location. */
nir_shader *
r300vk_build_dp4_fs_nir(const nir_shader_compiler_options *opts,
                        unsigned components)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r300vk_dp4");

   nir_variable *in_tc = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
   in_tc->data.location = VARYING_SLOT_VAR0;
   /* The varying is a vec4, but a 2D sampler takes a 2-component (s,t)
    * coordinate.  nir_tex asserts coord_components equals the sampler
    * dimension's coordinate count, so trim the load to xy before building
    * the texture instruction. */
   nir_def *coord = nir_trim_vector(&b, nir_load_var(&b, in_tc), 2);

   nir_def *tex[2];
   for (unsigned s = 0; s < 2; s++) {
      nir_variable *samp = nir_variable_create(
         b.shader, nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
         "samp");
      samp->data.binding = s;
      nir_deref_instr *d = nir_build_deref_var(&b, samp);
      tex[s] = nir_tex(&b, coord, .texture_deref = d, .sampler_deref = d);
   }

   unsigned c = components ? components : 4;
   /* tex[s] is a vec4 sample; trim to the kernel's dot width (2/3/4).
    * nir_trim_vector returns the def unchanged when c == 4, so the
    * full-width DP4 case adds no extract.  Trimming via the helper (rather
    * than nir_channels(BITFIELD_MASK(c))) avoids feeding the macro's ~0u
    * branch into nir_component_mask_t, which would be a u16 truncation. */
   nir_def *a  = nir_trim_vector(&b, tex[0], c);
   nir_def *bb = nir_trim_vector(&b, tex[1], c);
   nir_def *dot = nir_fdot(&b, a, bb);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   /* R300 has no FP32 render target (hardware-confirmed: an FP32 color FBO is
    * incomplete), so the scalar dot cannot be written as an IEEE-754 float.
    * Carry it as a 3-byte little-endian integer in RGBA8: r=dot%256,
    * g=(dot/256)%256, b=(dot/65536)%256.  The dot is an integer <= 2^17 for
    * <=7-bit (quantized) operands and stays exact through this FP24 encode
    * (every intermediate <= 2^24).  The dispatch-replay's UNORM8 RT round-trips
    * each byte and copies them to the kernel's uint output SSBO.  This is the
    * same encode the surfaceless-EGL dp4 probe proved 6/6 byte-exact. */
   nir_def *fl256 = nir_ffloor(&b, nir_fmul_imm(&b, dot, 1.0 / 256.0));
   nir_def *enc_r = nir_fsub(&b, dot, nir_fmul_imm(&b, fl256, 256.0));
   nir_def *enc_g = nir_fsub(&b, fl256,
      nir_fmul_imm(&b, nir_ffloor(&b, nir_fmul_imm(&b, fl256, 1.0 / 256.0)), 256.0));
   nir_def *flh = nir_ffloor(&b, nir_fmul_imm(&b, dot, 1.0 / 65536.0));
   nir_def *enc_b = nir_fsub(&b, flh,
      nir_fmul_imm(&b, nir_ffloor(&b, nir_fmul_imm(&b, flh, 1.0 / 256.0)), 256.0));
   nir_def *enc = nir_vec4(&b,
      nir_fmul_imm(&b, enc_r, 1.0 / 255.0), nir_fmul_imm(&b, enc_g, 1.0 / 255.0),
      nir_fmul_imm(&b, enc_b, 1.0 / 255.0), nir_imm_float(&b, 0.0));
   nir_store_var(&b, out, enc, 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   return b.shader;
}
