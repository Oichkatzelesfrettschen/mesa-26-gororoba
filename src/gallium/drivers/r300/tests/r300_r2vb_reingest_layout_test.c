/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Host corpus for the R2VB per-output re-ingest stream layout
 * (r300_r2vb_reingest_stream_layout).
 *
 * The delivery draw at TCL_BYPASS must fetch one vertex element per VS OUTPUT
 * vector: VAP_OUTPUT_VTX_FMT and the RS program are built from vertex_info (the
 * output tuple), and the VAP consumes VAP_VTX_SIZE dwords per vertex from the
 * fetched arrays.  A delivery that binds elements per DISTINCT SOURCE instead
 * -- two passthrough varyings fed by one application vector yielding one shared
 * element -- under-feeds the advertised tuple, and the GA stalls waiting for
 * vertex dwords that never arrive.  This unit pins the output-driven
 * enumeration: two outputs sourced from one input produce two streams carrying
 * the same src_velem; component-packed inputs share their driver_location and
 * therefore their physical src_velem; distinct sources keep distinct
 * src_velem values; the
 * computed varying maps by slot; streams sort into r300 PSC/VAP output-vector
 * order regardless of store order; every delivered output is one full FP32x4
 * vector; and a varying that is neither the computed slot nor a direct input
 * load is refused, not guessed.
 */

#include <stdio.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_r2vb.h"

static int failures;

#define CHECK(cond, name)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL %s: %s\n", name, #cond);                      \
         failures++;                                                         \
      }                                                                      \
   } while (0)

static const nir_shader_compiler_options vs_options = {0};

static void
check_full_fp32x4_shape(const struct r300_r2vb_reingest_stream *streams,
                        int count, const char *name)
{
   for (int i = 0; i < count; i++) {
      char label[128];
      snprintf(label, sizeof(label), "%s stream %d is full FP32x4", name, i);
      CHECK(streams[i].components == 4 && streams[i].bit_size == 32 &&
               streams[i].write_mask == 0xf,
            label);
   }
}

struct vs_io {
   nir_variable *in_pos;
   nir_variable *in_attr0;
   nir_variable *in_attr1;
   nir_variable *out_pos;
   nir_variable *out_var0;
   nir_variable *out_var1;
};

/* Two application inputs (position rank 0, attribute rank 1) and the three
 * output variables the cases store into; deref-form NIR, the shape the bound
 * VS arrives in (r300_optimize_nir does not run nir_lower_io). */
static nir_builder
vs_shell(struct vs_io *io, bool second_attr, const char *name)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &vs_options, "%s", name);
   io->in_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                    glsl_vec4_type(), "inPos");
   io->in_pos->data.location = VERT_ATTRIB_POS;
   io->in_pos->data.driver_location = 0;
   io->in_attr0 = nir_variable_create(b.shader, nir_var_shader_in,
                                      glsl_vec4_type(), "inAttr0");
   io->in_attr0->data.location = VERT_ATTRIB_GENERIC0;
   io->in_attr0->data.driver_location = 1;
   io->in_attr1 = NULL;
   if (second_attr) {
      io->in_attr1 = nir_variable_create(b.shader, nir_var_shader_in,
                                         glsl_vec4_type(), "inAttr1");
      io->in_attr1->data.location = VERT_ATTRIB_GENERIC1;
      io->in_attr1->data.driver_location = 2;
   }
   io->out_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                     glsl_vec4_type(), "outPos");
   io->out_pos->data.location = VARYING_SLOT_POS;
   io->out_var0 = nir_variable_create(b.shader, nir_var_shader_out,
                                      glsl_vec4_type(), "vColor");
   io->out_var0->data.location = VARYING_SLOT_VAR0;
   io->out_var1 = nir_variable_create(b.shader, nir_var_shader_out,
                                      glsl_vec4_type(), "vAttr");
   io->out_var1->data.location = VARYING_SLOT_VAR1;
   return b;
}

/* The recur-family delivery shape: position computed from inPos, both varyings
 * direct passthroughs of the ONE attribute input.  The layout must produce two
 * passthrough streams that both record src_velem 1 -- two output vectors, one
 * source -- so the fetch dword sum (3 x FP32x4 = 12) matches the output tuple
 * instead of collapsing to the 8-dword distinct-source set. */
static void
test_duplicate_source_outputs(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, false, "dup_source");
   nir_def *p = nir_load_var(&b, io.in_pos);
   nir_store_var(&b, io.out_pos, nir_fadd(&b, p, p), 0xf);
   nir_def *a = nir_load_var(&b, io.in_attr0);
   nir_store_var(&b, io.out_var0, a, 0xf);
   nir_store_var(&b, io.out_var1, a, 0xf);

   struct r300_r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == 3, "dup-source stream count is the output count");
   if (n == 3) {
      check_full_fp32x4_shape(s, n, "dup-source");
      CHECK(s[0].kind == R2VB_STREAM_POS && s[0].slot == VARYING_SLOT_POS,
            "dup-source stream 0 is position");
      CHECK(s[1].kind == R2VB_STREAM_PASSTHROUGH && s[1].src_velem == 1,
            "dup-source stream 1 fetches app velem 1");
      CHECK(s[2].kind == R2VB_STREAM_PASSTHROUGH && s[2].src_velem == 1,
            "dup-source stream 2 fetches the SAME app velem 1");
      /* All three sources are FP32x4 in this corpus, so the reconstructed
       * fetch is 3 x 4 = 12 dwords -- the VAP_VTX_SIZE the 12-dword
       * OUTPUT_VTX_FMT tuple (position + two 4-component texcoords)
       * requires. */
      CHECK(n * 4 == 12, "dup-source FP32x4 fetch dword sum is 12");
   }
   ralloc_free(b.shader);
}

/* Component-packed inputs occupy one application vertex element even though
 * their location fractions distinguish the semantic lanes.  The layout must
 * carry that shared physical element to every passthrough output. */
static void
test_component_packed_source_outputs(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, true, "component_packed_source");
   io.in_attr1->data.location = VERT_ATTRIB_GENERIC0;
   io.in_attr0->data.location_frac = 0;
   io.in_attr1->data.location_frac = 1;
   io.in_attr1->data.driver_location = io.in_attr0->data.driver_location;

   nir_store_var(&b, io.out_pos, nir_load_var(&b, io.in_pos), 0xf);
   nir_store_var(&b, io.out_var0, nir_load_var(&b, io.in_attr0), 0xf);
   nir_store_var(&b, io.out_var1, nir_load_var(&b, io.in_attr1), 0xf);

   struct r300_r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == 3, "component-packed stream count");
   if (n == 3) {
      CHECK(s[1].kind == R2VB_STREAM_PASSTHROUGH &&
               s[2].kind == R2VB_STREAM_PASSTHROUGH,
            "component-packed streams are passthroughs");
      CHECK(s[1].src_velem == 1 && s[2].src_velem == 1,
            "component-packed streams share their physical app velem");
   }
   ralloc_free(b.shader);
}

/* Distinct passthrough sources keep distinct velem ranks. */
static void
test_distinct_source_outputs(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, true, "distinct_source");
   nir_def *p = nir_load_var(&b, io.in_pos);
   nir_store_var(&b, io.out_pos, nir_fadd(&b, p, p), 0xf);
   nir_store_var(&b, io.out_var0, nir_load_var(&b, io.in_attr0), 0xf);
   nir_store_var(&b, io.out_var1, nir_load_var(&b, io.in_attr1), 0xf);

   struct r300_r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == 3, "distinct-source stream count");
   if (n == 3) {
      check_full_fp32x4_shape(s, n, "distinct-source");
      CHECK(s[1].kind == R2VB_STREAM_PASSTHROUGH && s[1].src_velem == 1,
            "distinct-source stream 1 keeps velem rank 1");
      CHECK(s[2].kind == R2VB_STREAM_PASSTHROUGH && s[2].src_velem == 2,
            "distinct-source stream 2 keeps velem rank 2");
   }
   ralloc_free(b.shader);
}

/* Position + one computed varying + one passthrough: the computed slot maps to
 * R2VB_STREAM_COMPUTED (the producer BO), the passthrough keeps its app velem,
 * matching the standing computed-varying delivery. */
static void
test_computed_varying_mapping(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, false, "computed_varying");
   nir_def *p = nir_load_var(&b, io.in_pos);
   nir_store_var(&b, io.out_pos, nir_fadd(&b, p, p), 0xf);
   nir_store_var(&b, io.out_var0, nir_fmul_imm(&b, p, 2.0), 0xf);
   nir_store_var(&b, io.out_var1, nir_load_var(&b, io.in_attr0), 0xf);

   struct r300_r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, VARYING_SLOT_VAR0, s, 8);
   CHECK(n == 3, "computed-varying stream count");
   if (n == 3) {
      check_full_fp32x4_shape(s, n, "computed-varying");
      CHECK(s[0].kind == R2VB_STREAM_POS, "computed-varying stream 0 is position");
      CHECK(s[1].kind == R2VB_STREAM_COMPUTED && s[1].slot == VARYING_SLOT_VAR0,
            "computed-varying stream 1 is the producer BO");
      CHECK(s[2].kind == R2VB_STREAM_PASSTHROUGH && s[2].src_velem == 1,
            "computed-varying stream 2 is the app passthrough");
   }
   ralloc_free(b.shader);
}

/* NIR's store_output form carries the destination slot in IO semantics plus a
 * constant offset, the value in src[0], and the component mask in intrinsic
 * indices (src/compiler/nir/nir_intrinsics.py; rg --fixed-strings
 * 'store("output"' src/compiler/nir/nir_intrinsics.py).  The re-ingest layout
 * resolves these fields before it classifies the stream. */
static void
test_lowered_store_output_mapping(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, false, "lowered_store_output");
   nir_store_var(&b, io.out_pos, nir_load_var(&b, io.in_pos), 0xf);

   nir_def *input = nir_load_input(
      &b, 4, 32, nir_imm_int(&b, 1),
      .base = 1,
      .io_semantics.location = VERT_ATTRIB_GENERIC0);
   nir_store_output(&b, input, nir_imm_int(&b, 0),
                    .io_semantics = {
                       .location = VARYING_SLOT_VAR0,
                       .num_slots = 1,
                    });

   nir_def *components[] = {
      nir_channel(&b, nir_load_var(&b, io.in_pos), 0),
      nir_channel(&b, nir_load_var(&b, io.in_pos), 1),
   };
   nir_intrinsic_instr *computed = nir_store_output(
      &b, nir_vec(&b, components, ARRAY_SIZE(components)), nir_imm_int(&b, 1),
      .io_semantics = {
         .location = VARYING_SLOT_VAR0,
         .num_slots = 2,
      });
   nir_intrinsic_set_component(computed, 1);
   nir_intrinsic_set_write_mask(computed, 0x3);

   struct r300_r2vb_reingest_stream streams[8];
   int n = r300_r2vb_reingest_stream_layout(
      b.shader, VARYING_SLOT_VAR1, streams, ARRAY_SIZE(streams));
   CHECK(n == 3, "lowered store_output stream count");
   if (n == 3) {
      CHECK(streams[0].kind == R2VB_STREAM_POS &&
               streams[0].slot == VARYING_SLOT_POS,
            "lowered store_output position remains first");
      CHECK(streams[1].kind == R2VB_STREAM_PASSTHROUGH &&
               streams[1].slot == VARYING_SLOT_VAR0 &&
               streams[1].src_velem == 2 && streams[1].components == 4 &&
               streams[1].write_mask == 0xf,
            "lowered store_output value maps to its input velem");
      CHECK(streams[2].kind == R2VB_STREAM_COMPUTED &&
               streams[2].slot == VARYING_SLOT_VAR1 &&
               streams[2].components == 2 && streams[2].write_mask == 0x6,
            "lowered store_output offset and component mask map to the computed slot");
   }
   ralloc_free(b.shader);
}

/* Stores encountered in non-PSC order sort into PSC/VAP output-vector order:
 * stream i must drive VAP output vector i whatever order the VS wrote them. */
static void
test_store_order_independence(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, false, "store_order");
   nir_def *a = nir_load_var(&b, io.in_attr0);
   nir_store_var(&b, io.out_var1, a, 0xf);
   nir_store_var(&b, io.out_var0, a, 0xf);
   nir_def *p = nir_load_var(&b, io.in_pos);
   nir_store_var(&b, io.out_pos, nir_fadd(&b, p, p), 0xf);

   struct r300_r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == 3, "store-order stream count");
   if (n == 3) {
      check_full_fp32x4_shape(s, n, "store-order");
      CHECK(s[0].slot == VARYING_SLOT_POS, "store-order position sorts first");
      CHECK(s[1].slot == VARYING_SLOT_VAR0 && s[2].slot == VARYING_SLOT_VAR1,
            "store-order varyings sort ascending");
   }
   ralloc_free(b.shader);
}

/* A non-position varying that is neither the computed slot nor a direct input
 * load has no fetchable source at TCL_BYPASS; the layout refuses (-1) so the
 * caller declines the submit instead of drawing a mismatched tuple. */
static void
test_unmappable_varying_refused(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, false, "unmappable");
   nir_def *p = nir_load_var(&b, io.in_pos);
   nir_store_var(&b, io.out_pos, nir_fadd(&b, p, p), 0xf);
   nir_store_var(&b, io.out_var0, nir_fmul_imm(&b, p, 2.0), 0xf);
   nir_store_var(&b, io.out_var1, nir_load_var(&b, io.in_attr0), 0xf);

   struct r300_r2vb_reingest_stream s[8];
   /* No computed slot declared: the arithmetic VAR0 store is unmappable. */
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == -1, "arithmetic varying without a computed slot is refused");
   ralloc_free(b.shader);
}

static void
test_scalar_point_size_refused(void)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &vs_options,
                                     "scalar_point_size");
   nir_variable *in_pos = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "inPos");
   in_pos->data.location = VERT_ATTRIB_POS;
   nir_variable *out_pos = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "outPos");
   out_pos->data.location = VARYING_SLOT_POS;
   nir_variable *out_psiz = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_float_type(), "outPointSize");
   out_psiz->data.location = VARYING_SLOT_PSIZ;
   nir_store_var(&b, out_pos, nir_load_var(&b, in_pos), 0xf);
   nir_store_var(&b, out_psiz, nir_imm_float(&b, 1.0f), 0x1);

   struct r300_r2vb_reingest_stream streams[8];
   CHECK(r300_r2vb_reingest_stream_layout(
            b.shader, -1, streams, ARRAY_SIZE(streams)) == -1,
         "scalar point-size output is refused");
   ralloc_free(b.shader);
}

static void
test_vec2_output_refused(void)
{
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &vs_options,
                                     "vec2_output");
   nir_variable *in_pos = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "inPos");
   in_pos->data.location = VERT_ATTRIB_POS;
   nir_variable *in_attr = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec2_type(), "inAttr");
   in_attr->data.location = VERT_ATTRIB_GENERIC0;
   nir_variable *out_pos = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "outPos");
   out_pos->data.location = VARYING_SLOT_POS;
   nir_variable *out_var = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec2_type(), "outVar");
   out_var->data.location = VARYING_SLOT_VAR0;
   nir_store_var(&b, out_pos, nir_load_var(&b, in_pos), 0xf);
   nir_store_var(&b, out_var, nir_load_var(&b, in_attr), 0x3);

   struct r300_r2vb_reingest_stream streams[8];
   CHECK(r300_r2vb_reingest_stream_layout(
            b.shader, -1, streams, ARRAY_SIZE(streams)) == -1,
         "vec2 passthrough output is refused");
   ralloc_free(b.shader);
}

static void
test_partial_write_refused(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, false, "partial_write");
   nir_store_var(&b, io.out_pos, nir_load_var(&b, io.in_pos), 0xf);
   nir_store_var(&b, io.out_var0, nir_load_var(&b, io.in_attr0), 0x3);

   struct r300_r2vb_reingest_stream streams[8];
   CHECK(r300_r2vb_reingest_stream_layout(
            b.shader, -1, streams, ARRAY_SIZE(streams)) == -1,
         "partial vec4 output write is refused");
   ralloc_free(b.shader);
}

static void
test_duplicate_output_store_refused(void)
{
   struct vs_io io;
   nir_builder b = vs_shell(&io, false, "duplicate_output_store");
   nir_def *position = nir_load_var(&b, io.in_pos);
   nir_def *attribute = nir_load_var(&b, io.in_attr0);
   nir_store_var(&b, io.out_pos, position, 0xf);
   nir_store_var(&b, io.out_var0, attribute, 0xf);
   nir_store_var(&b, io.out_var0, attribute, 0xf);

   struct r300_r2vb_reingest_stream streams[8];
   CHECK(r300_r2vb_reingest_stream_layout(
            b.shader, -1, streams, ARRAY_SIZE(streams)) == -1,
         "duplicate output-slot stores are refused");
   ralloc_free(b.shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   test_duplicate_source_outputs();
   test_component_packed_source_outputs();
   test_distinct_source_outputs();
   test_computed_varying_mapping();
   test_lowered_store_output_mapping();
   test_store_order_independence();
   test_unmappable_varying_refused();
   test_scalar_point_size_refused();
   test_vec2_output_refused();
   test_partial_write_refused();
   test_duplicate_output_store_refused();

   glsl_type_singleton_decref();

   if (failures) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   printf("r300_r2vb_reingest_layout_test: all checks passed\n");
   return 0;
}
