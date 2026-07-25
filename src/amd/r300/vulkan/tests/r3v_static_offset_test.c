/*
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for r3v constant-file offset representability.
 */

#include <stdio.h>
#include <stdbool.h>

#include "../r3v_pipeline.c"

static unsigned failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         failures++;                      \
      }                                   \
   } while (0)

static struct pipe_screen *
fake_r300_screen(struct r300_screen *screen)
{
   screen->caps.has_tcl = false;
   screen->caps.is_r500 = false;
   screen->caps.is_r400 = false;
   return &screen->screen;
}

static nir_shader *
push_const_load_shader(unsigned offset, unsigned components)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                     "r3v static push offset");

   nir_load_push_constant(&b, components, 32, nir_imm_int(&b, offset),
                          .base = 0, .range = 128, .align_mul = 4,
                          .align_offset = offset & 3u);

   return b.shader;
}

static void
check_push_const(unsigned offset, unsigned components, bool expected,
                 const char *name)
{
   nir_shader *shader = push_const_load_shader(offset, components);
   bool ok = r3v_nir_push_const_shape_ok(NULL, shader);
   CHECK(ok == expected, name);
   ralloc_free(shader);
}

static void
check_straddle_flag_is_explicit(void)
{
   nir_shader *shader = push_const_load_shader(12, 2);
   bool ok = r3v_nir_offsets_static(NULL, shader,
                                       nir_intrinsic_load_push_constant, 0,
                                       false);
   CHECK(ok, "straddle=false accepts a constant offset crossing a slot");
   ralloc_free(shader);
}

static nir_variable *
add_block0_ubo(nir_shader *shader, unsigned size_bytes, const char *name)
{
   const struct glsl_type *ubo_type = r3v_block0_ubo_type(size_bytes);
   nir_variable *ubo =
      nir_variable_create(shader, nir_var_mem_ubo, ubo_type, name);

   ubo->data.driver_location = 0;
   ubo->interface_type = r3v_block0_ubo_interface_type(ubo_type);
   return ubo;
}

static unsigned
block0_ubo_count(nir_shader *shader)
{
   unsigned count = 0;

   nir_foreach_variable_with_modes(var, shader, nir_var_mem_ubo) {
      if (var->data.driver_location == 0)
         count++;
   }

   return count;
}

static unsigned
block0_ubo_interface_size(nir_shader *shader)
{
   nir_variable *ubo = r3v_find_block0_ubo(shader);

   return ubo ? r3v_ubo_interface_size(ubo) : 0;
}

static void
check_block0_ubo_declaration(void)
{
   nir_shader *shader = push_const_load_shader(0, 4);
   r3v_declare_block0_ubo(shader, 128);
   CHECK(block0_ubo_count(shader) == 1,
         "block-0 UBO declaration creates one compiler-visible block");
   CHECK(block0_ubo_interface_size(shader) >= 128,
         "block-0 UBO declaration covers the push-constant window");
   ralloc_free(shader);

   shader = push_const_load_shader(0, 4);
   add_block0_ubo(shader, 16, "app_ubo0");
   r3v_declare_block0_ubo(shader, 128);
   CHECK(block0_ubo_count(shader) == 1,
         "block-0 UBO declaration reuses a prior app block");
   CHECK(block0_ubo_interface_size(shader) >= 128,
         "reused block-0 UBO declaration expands to the push window");
   ralloc_free(shader);
}

static nir_shader *
vertex_texture_shader(bool live_texture)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                     "r3v vertex texture gate");

   nir_variable *sampler = nir_variable_create(
      b.shader, nir_var_uniform,
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
      "samp");
   nir_deref_instr *sampler_deref = nir_build_deref_var(&b, sampler);
   nir_def *coord = nir_imm_vec2(&b, 0.0f, 0.0f);
   nir_def *tex =
      nir_tex(&b, coord, .texture_deref = sampler_deref,
              .sampler_deref = sampler_deref);

   nir_variable *pos =
      nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(),
                          "gl_Position");
   pos->data.location = VARYING_SLOT_POS;
   pos->data.driver_location = 0;

   nir_store_var(&b, pos,
                 live_texture ? tex : nir_imm_vec4(&b, 0.0f, 0.0f, 0.0f, 1.0f),
                 0xf);

   return b.shader;
}

static void
check_vertex_texture_gate(void)
{
   struct r300_screen screen = {0};
   struct pipe_screen *pscreen = fake_r300_screen(&screen);

   nir_shader *dead_tex = vertex_texture_shader(false);
   CHECK(!r3v_nir_uses_live_texture_after_r300_opt(pscreen, dead_tex),
         "dead vertex texture is accepted after r300 NIR DCE");
   ralloc_free(dead_tex);

   nir_shader *live_tex = vertex_texture_shader(true);
   CHECK(r3v_nir_uses_live_texture_after_r300_opt(pscreen, live_tex),
         "live vertex texture remains rejected after r300 NIR DCE");
   ralloc_free(live_tex);
}

static void
check_vs_input_span(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                     "r3v VS input span");
   nir_variable *input1 =
      nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(),
                          "input1");
   nir_variable *input0 =
      nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(),
                          "input0");
   input1->data.location = VERT_ATTRIB_GENERIC1;
   input0->data.location = VERT_ATTRIB_GENERIC0;

   CHECK(r3v_assign_vs_input_locations(b.shader),
         "VS input locations fit the Gallium attribute span");
   CHECK(input0->data.driver_location == 0 &&
         input1->data.driver_location == 1,
         "VS input locations map to their AOS rows");
   CHECK(b.shader->num_inputs == 2,
         "VS metadata publishes the two-row input span");
   ralloc_free(b.shader);

   b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                      "r3v invalid VS input span");
   nir_variable *invalid =
      nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(),
                          "invalid");
   invalid->data.location = VERT_ATTRIB_GENERIC0 + PIPE_MAX_ATTRIBS;
   CHECK(!r3v_assign_vs_input_locations(b.shader),
         "VS input locations outside the Gallium span are rejected");
   ralloc_free(b.shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   check_push_const(0, 4, true,
                    "vec4 push load at slot start fits one constant slot");
   check_push_const(4, 3, true,
                    "vec3 push load ending on slot boundary is accepted");
   check_push_const(4, 4, false,
                    "vec4 push load at byte 4 crosses one constant slot");
   check_push_const(12, 1, true,
                    "scalar push load at final slot component is accepted");
   check_push_const(12, 2, false,
                    "vec2 push load at final slot component is rejected");
   check_straddle_flag_is_explicit();
   check_block0_ubo_declaration();
   check_vertex_texture_gate();
   check_vs_input_span();

   glsl_type_singleton_decref();
   return failures ? 1 : 0;
}
