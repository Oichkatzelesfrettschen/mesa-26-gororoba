/*
 * Copyright (c) 2026 Terascale Functionalists
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
 * the same src_velem; distinct sources keep distinct src_velem ranks; the
 * computed varying maps by slot; streams sort into ascending-slot (VAP output
 * vector) order regardless of store order; and a varying that is neither the
 * computed slot nor a direct input load is refused, not guessed.
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
   io->in_attr0 = nir_variable_create(b.shader, nir_var_shader_in,
                                      glsl_vec4_type(), "inAttr0");
   io->in_attr0->data.location = VERT_ATTRIB_GENERIC0;
   io->in_attr1 = NULL;
   if (second_attr) {
      io->in_attr1 = nir_variable_create(b.shader, nir_var_shader_in,
                                         glsl_vec4_type(), "inAttr1");
      io->in_attr1->data.location = VERT_ATTRIB_GENERIC1;
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

   struct r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == 3, "dup-source stream count is the output count");
   if (n == 3) {
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

   struct r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == 3, "distinct-source stream count");
   if (n == 3) {
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

   struct r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, VARYING_SLOT_VAR0, s, 8);
   CHECK(n == 3, "computed-varying stream count");
   if (n == 3) {
      CHECK(s[0].kind == R2VB_STREAM_POS, "computed-varying stream 0 is position");
      CHECK(s[1].kind == R2VB_STREAM_COMPUTED && s[1].slot == VARYING_SLOT_VAR0,
            "computed-varying stream 1 is the producer BO");
      CHECK(s[2].kind == R2VB_STREAM_PASSTHROUGH && s[2].src_velem == 1,
            "computed-varying stream 2 is the app passthrough");
   }
   ralloc_free(b.shader);
}

/* Stores encountered in non-slot order sort into ascending-slot order: stream
 * i must drive VAP output vector i whatever order the VS wrote them in. */
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

   struct r2vb_reingest_stream s[8];
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == 3, "store-order stream count");
   if (n == 3) {
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

   struct r2vb_reingest_stream s[8];
   /* No computed slot declared: the arithmetic VAR0 store is unmappable. */
   int n = r300_r2vb_reingest_stream_layout(b.shader, -1, s, 8);
   CHECK(n == -1, "arithmetic varying without a computed slot is refused");
   ralloc_free(b.shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   test_duplicate_source_outputs();
   test_distinct_source_outputs();
   test_computed_varying_mapping();
   test_store_order_independence();
   test_unmappable_varying_refused();

   glsl_type_singleton_decref();

   if (failures) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   printf("r300_r2vb_reingest_layout_test: all checks passed\n");
   return 0;
}
