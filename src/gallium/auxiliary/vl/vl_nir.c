/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "compiler/nir/nir_builder.h"

#include "vl_nir.h"

void *
vl_nir_vs_finish(nir_builder *b, struct pipe_context *pipe)
{
   /* finalize_nir is an optional screen hook; r300 lowers NIR inside
    * create_*_state (nir_to_rc) and leaves the hook NULL.  Skip it when absent,
    * the same guard tgsi_to_nir uses. */
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, b->shader, true);

   struct pipe_shader_state state = {0};
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = b->shader;
   return pipe->create_vs_state(pipe, &state);
}

void *
vl_nir_vs_passthrough(struct pipe_context *pipe, unsigned num_tc,
                      const char *name)
{
   const nir_shader_compiler_options *options =
      pipe->screen->nir_options[MESA_SHADER_VERTEX];
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "%s", name);

   nir_variable *in_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                              glsl_vec4_type(), "pos_in");
   in_pos->data.location = VERT_ATTRIB_GENERIC0;
   nir_def *pos = nir_load_var(&b, in_pos);

   nir_variable *out_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                              glsl_vec4_type(), "pos_out");
   out_pos->data.location = VARYING_SLOT_POS;
   nir_store_var(&b, out_pos, pos, 0xf);

   for (unsigned i = 0; i < num_tc; i++) {
      nir_variable *out_tc = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_vec4_type(), "tc_out");
      out_tc->data.location = VARYING_SLOT_VAR0 + i;
      nir_store_var(&b, out_tc, pos, 0xf);
   }

   return vl_nir_vs_finish(&b, pipe);
}

void
vl_nir_fs_begin(struct vl_nir_fs *fs, struct pipe_context *pipe,
                unsigned num_tc, const char *name)
{
   const nir_shader_compiler_options *options =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   fs->b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, options, "%s", name);
   nir_builder *b = &fs->b;

   for (unsigned i = 0; i < num_tc; i++) {
      nir_variable *in_tc = nir_variable_create(b->shader, nir_var_shader_in,
                                               glsl_vec4_type(), "tc");
      in_tc->data.location = VARYING_SLOT_VAR0 + i;
      fs->texcoord[i] = nir_load_var(b, in_tc);
   }

   fs->out_color = nir_variable_create(b->shader, nir_var_shader_out,
                                       glsl_vec4_type(), "color");
   fs->out_color->data.location = FRAG_RESULT_COLOR;
}

void
vl_nir_sampler(struct vl_nir_fs *fs, unsigned s, enum glsl_sampler_dim dim)
{
   const struct glsl_type *sampler_type =
      glsl_sampler_type(dim, false, false, GLSL_TYPE_FLOAT);
   nir_variable *samp = nir_variable_create(fs->b.shader, nir_var_uniform,
                                           sampler_type, "samp");
   samp->data.binding = s;
   fs->samp[s] = nir_build_deref_var(&fs->b, samp);
}

nir_def *
vl_nir_tex(struct vl_nir_fs *fs, unsigned s, nir_def *coord)
{
   return nir_tex(&fs->b, coord,
                  .texture_deref = fs->samp[s], .sampler_deref = fs->samp[s]);
}

void *
vl_nir_fs_finish(struct vl_nir_fs *fs, struct pipe_context *pipe,
                 nir_def *color)
{
   nir_store_var(&fs->b, fs->out_color, color, 0xf);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, fs->b.shader, true);

   struct pipe_shader_state state = {0};
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = fs->b.shader;
   return pipe->create_fs_state(pipe, &state);
}
