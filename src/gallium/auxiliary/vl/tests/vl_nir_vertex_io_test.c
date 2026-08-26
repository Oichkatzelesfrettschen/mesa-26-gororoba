/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "compiler/nir/nir_builder.h"
#include "util/ralloc.h"

#include "vl_nir.h"

static unsigned failures;

#define CHECK(condition, message)                                               \
   do {                                                                         \
      if (!(condition)) {                                                       \
         fprintf(stderr, "FAIL: %s\n", message);                               \
         failures++;                                                            \
      }                                                                          \
   } while (0)

static void *
capture_vs_state(struct pipe_context *pipe,
                 const struct pipe_shader_state *state)
{
   (void)pipe;
   CHECK(state->type == PIPE_SHADER_IR_NIR, "vertex state carries NIR");
   return state->ir.nir;
}

static nir_variable *
find_input(nir_shader *shader, unsigned location)
{
   nir_foreach_shader_in_variable(var, shader) {
      if (var->data.location == location)
         return var;
   }
   return NULL;
}

static unsigned
count_inputs(nir_shader *shader)
{
   unsigned count = 0;
   nir_foreach_shader_in_variable(var, shader)
      count++;
   return count;
}

static void
check_passthrough(struct pipe_context *pipe, unsigned num_tc,
                  unsigned expected_inputs)
{
   nir_shader *shader = vl_nir_vs_passthrough(pipe, num_tc,
                                               "vl:nir_vertex_io_test");
   CHECK(shader != NULL, "passthrough shader was created");
   if (!shader)
      return;

   CHECK(count_inputs(shader) == expected_inputs,
         "passthrough declares the expected input count");
   CHECK(shader->num_inputs == expected_inputs,
         "passthrough publishes the expected input span");

   for (unsigned slot = 0; slot < expected_inputs; slot++) {
      nir_variable *input = find_input(shader, VERT_ATTRIB_GENERIC0 + slot);
      CHECK(input != NULL, "passthrough input preserves its attribute slot");
      if (input)
         CHECK(input->data.driver_location == slot,
               "passthrough driver location matches its attribute slot");
   }

   ralloc_free(shader);
}

static void
check_sparse_inputs(struct pipe_context *pipe,
                    const nir_shader_compiler_options *options)
{
   nir_builder builder = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, options, "vl:sparse_vertex_io_test");

   nir_variable *input_1 = nir_variable_create(
      builder.shader, nir_var_shader_in, glsl_vec4_type(), "input_1");
   input_1->data.location = VERT_ATTRIB_GENERIC0 + 1;
   nir_variable *input_3 = nir_variable_create(
      builder.shader, nir_var_shader_in, glsl_vec4_type(), "input_3");
   input_3->data.location = VERT_ATTRIB_GENERIC0 + 3;

   nir_variable *output = nir_variable_create(
      builder.shader, nir_var_shader_out, glsl_vec4_type(), "position");
   output->data.location = VARYING_SLOT_POS;
   nir_store_var(&builder, output,
                 nir_fadd(&builder, nir_load_var(&builder, input_1),
                          nir_load_var(&builder, input_3)),
                 0xf);

   nir_shader *shader = vl_nir_vs_finish(&builder, pipe);
   CHECK(shader != NULL, "sparse-input shader was created");
   if (!shader)
      return;

   CHECK(input_1->data.driver_location == 1,
         "attribute 1 remains on driver input 1");
   CHECK(input_3->data.driver_location == 3,
         "attribute 3 remains on driver input 3");
   CHECK(shader->num_inputs == 4,
         "sparse input span includes the highest occupied attribute");

   ralloc_free(shader);
}

int
main(void)
{
   nir_shader_compiler_options options = {0};
   struct pipe_screen screen = {0};
   struct pipe_context pipe = {0};

   screen.nir_options[MESA_SHADER_VERTEX] = &options;
   pipe.screen = &screen;
   pipe.create_vs_state = capture_vs_state;

   check_passthrough(&pipe, 1, 1);
   check_passthrough(&pipe, 2, 3);
   check_sparse_inputs(&pipe, &options);

   if (failures)
      return 1;

   puts("vl_nir vertex input identity PASS");
   return 0;
}
