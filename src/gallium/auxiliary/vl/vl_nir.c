/*
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
   /* nir_builder shaders must gather info and have IO var locations assigned
    * before being consumed: nir_to_tgsi (SW-TCL VS) and nir_to_rc (FS) both
    * key TGSI register / interpolator indices off var->data.driver_location,
    * which they read but never assign.  Without this every input/output
    * collapses onto slot 0 (no POSITION export, all texcoords on interp 0). */
   nir_shader_gather_info(b->shader, nir_shader_get_entrypoint(b->shader));
   nir_assign_io_var_locations(b->shader, nir_var_shader_in);
   nir_assign_io_var_locations(b->shader, nir_var_shader_out);

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
      nir_variable *in_tc = nir_variable_create(b.shader, nir_var_shader_in,
                                                glsl_vec4_type(), "tc_in");
      in_tc->data.location = VERT_ATTRIB_GENERIC0 + i + 1;
      nir_variable *out_tc = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_vec4_type(), "tc_out");
      out_tc->data.location = VARYING_SLOT_VAR0 + i;
      nir_store_var(&b, out_tc, nir_load_var(&b, in_tc), 0xf);
   }

   return vl_nir_vs_finish(&b, pipe);
}

void
vl_nir_fs_begin_opts(struct vl_nir_fs *fs,
                     const nir_shader_compiler_options *options,
                     unsigned num_tc, const char *name)
{
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
vl_nir_fs_begin(struct vl_nir_fs *fs, struct pipe_context *pipe,
                unsigned num_tc, const char *name)
{
   vl_nir_fs_begin_opts(fs, pipe->screen->nir_options[MESA_SHADER_FRAGMENT],
                        num_tc, name);
}

void
vl_nir_sampler(struct vl_nir_fs *fs, unsigned s, enum glsl_sampler_dim dim)
{
   vl_nir_sampler_array(fs, s, dim, false);
}

void
vl_nir_sampler_array(struct vl_nir_fs *fs, unsigned s,
                     enum glsl_sampler_dim dim, bool is_array)
{
   const struct glsl_type *sampler_type =
      glsl_sampler_type(dim, false, is_array, GLSL_TYPE_FLOAT);
   nir_variable *samp = nir_variable_create(fs->b.shader, nir_var_uniform,
                                           sampler_type, "samp");
   samp->data.binding = s;
   BITSET_SET(fs->b.shader->info.textures_used, s);
   BITSET_SET(fs->b.shader->info.samplers_used, s);
   fs->samp[s] = nir_build_deref_var(&fs->b, samp);
}

nir_def *
vl_nir_tex(struct vl_nir_fs *fs, unsigned s, nir_def *coord)
{
   return nir_tex(&fs->b, coord,
                  .texture_deref = fs->samp[s], .sampler_deref = fs->samp[s]);
}

nir_shader *
vl_nir_fs_finish_nir(struct vl_nir_fs *fs, nir_def *color)
{
   nir_store_var(&fs->b, fs->out_color, color, 0xf);
   /* Lower deref-combined samplers to indexed tex.  vl_nir_sampler marks
    * textures_used/samplers_used when declaring each binding: nir_lower_samplers
    * only writes tex instruction indices, while SoA NIR backends such as
    * llvmpipe key sampler static state on BITSET_LAST_BIT(info.textures_used). */
   NIR_PASS(_, fs->b.shader, nir_lower_samplers);
   nir_shader_gather_info(fs->b.shader, nir_shader_get_entrypoint(fs->b.shader));
   nir_assign_io_var_locations(fs->b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(fs->b.shader, nir_var_shader_out);
   return fs->b.shader;
}

void *
vl_nir_fs_finish(struct vl_nir_fs *fs, struct pipe_context *pipe,
                 nir_def *color)
{
   nir_shader *shader = vl_nir_fs_finish_nir(fs, color);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, shader, true);

   struct pipe_shader_state state = {0};
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = shader;
   return pipe->create_fs_state(pipe, &state);
}
