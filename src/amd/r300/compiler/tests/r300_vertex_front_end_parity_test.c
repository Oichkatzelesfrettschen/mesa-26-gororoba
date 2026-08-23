/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration and parity for the two vertex front ends over one job
 * IR: the NIR front end (compiler/r300_vertex_job_nir.c, the Gallium
 * consumer's path) and the direct SPIR-V admitter
 * (common/r300_vertex_spirv.c, the native ICD's path) must agree on
 * the reference modules -- identical jobs, identical color bits, and
 * identical refusals -- and the NIR front end's builder-made
 * arithmetic shapes must lower to the job IR and execute to exact
 * values.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/common/r300_vertex_spirv.h"
#include "amd/r300/compiler/r300_vertex_job_nir.h"
#include "amd/r300/cpu/r300_cpu_vertex_job.h"
#include "compiler/spirv/spirv_info.h"

#include "r3v_native_reference_spirv.h"

#include "compiler/nir/nir_builder.h"
#include "util/mesa-blake3.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Retained parity manifest: one BLAKE3 row per (shape, field) for the
 * shapes both live front ends accept -- the SPIR-V bytes, the canonical
 * job where the two jobs are bit-identical, the executed carrier bytes,
 * and the fragment color bits.  The arithmetic vertex shape binds no job
 * row: its two front ends may select instructions differently and parity
 * there is value equality of the carriers. */
#define MANIFEST_MAX_ROWS 16

struct manifest_row {
   const char *shape;
   const char *field;
   char digest[BLAKE3_HEX_LEN];
};

static struct manifest_row manifest_rows[MANIFEST_MAX_ROWS];
static unsigned manifest_row_count;
static bool inject_divergent_job;

static void record_digest(const char *shape, const char *field,
                          const void *data, size_t size)
{
   assert(manifest_row_count < MANIFEST_MAX_ROWS);
   blake3_hash hash;
   _mesa_blake3_compute(data, size, hash);
   struct manifest_row *row = &manifest_rows[manifest_row_count++];
   row->shape = shape;
   row->field = field;
   _mesa_blake3_format(row->digest, hash);
}

static void put_u32(uint8_t **cursor, uint32_t value)
{
   (*cursor)[0] = value & 0xff;
   (*cursor)[1] = (value >> 8) & 0xff;
   (*cursor)[2] = (value >> 16) & 0xff;
   (*cursor)[3] = (value >> 24) & 0xff;
   *cursor += 4;
}

/* Canonical job bytes: every field in declaration order, little-endian,
 * every attribute slot's format, only the live instruction and constant
 * prefixes, no padding. */
static void record_job_digest(const char *shape, const char *field,
                              const struct r300_vertex_job *job)
{
   uint8_t bytes[4 * R300_VERTEX_JOB_MAX_INPUTS + 4 +
                 R300_VERTEX_JOB_MAX_INSTRUCTIONS * 5 + 4 +
                 R300_VERTEX_JOB_MAX_CONSTANTS * 16];
   uint8_t *cursor = bytes;
   for (uint32_t slot = 0; slot < R300_VERTEX_JOB_MAX_INPUTS; slot++)
      put_u32(&cursor, (uint32_t)job->input_format_ids[slot]);
   put_u32(&cursor, job->instruction_count);
   for (uint32_t i = 0; i < job->instruction_count; i++) {
      const struct r300_vertex_job_instruction *in = &job->instructions[i];
      *cursor++ = in->opcode;
      *cursor++ = in->dst;
      *cursor++ = in->src0;
      *cursor++ = in->src1;
      *cursor++ = in->src2;
   }
   put_u32(&cursor, job->constant_count);
   for (uint32_t c = 0; c < job->constant_count; c++)
      for (unsigned k = 0; k < 4; k++)
         put_u32(&cursor, job->constants[c][k]);
   record_digest(shape, field, bytes, (size_t)(cursor - bytes));
}

static void record_words_digest(const char *shape, const char *field,
                                const uint32_t *words, size_t count)
{
   uint8_t *bytes = malloc(count * 4);
   assert(bytes != NULL);
   uint8_t *cursor = bytes;
   for (size_t i = 0; i < count; i++)
      put_u32(&cursor, words[i]);
   record_digest(shape, field, bytes, count * 4);
   free(bytes);
}

static uint32_t f_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

static nir_shader *
ingest(const uint32_t *words, size_t bytes, mesa_shader_stage stage)
{
   nir_shader *nir =
      r300_vertex_job_spirv_to_nir(words, bytes / 4, stage, "main");
   assert(nir != NULL);
   const char *reason = NULL;
   bool normalized = r300_vertex_job_nir_normalize(nir, &reason);
   assert(normalized);
   return nir;
}

/* The reference vertex module is the position pass-through, so the
 * semantic front end must lower it to the identity job and the job
 * must reproduce the gather bytes exactly. */
static void test_reference_vertex_module(void)
{
   nir_shader *nir = ingest(r3v_reference_vertex_spirv,
                            sizeof(r3v_reference_vertex_spirv),
                            MESA_SHADER_VERTEX);
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_nir(nir, &job, &reason);
   if (!admitted) {
      fprintf(stderr, "reference vertex refusal: %s\n", reason);
      nir_print_shader(nir, stderr);
   }
   ralloc_free(nir);
   assert(admitted);
   assert(job.instruction_count == 2 && job.constant_count == 0);
   assert(job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT);
   assert(job.instructions[1].opcode == R300_VERTEX_JOB_OP_STORE_POSITION);
   assert(job.instructions[1].src0 == job.instructions[0].dst);

   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   int rc = r300_cpu_vertex_job_validate(&job);
   assert(rc == 0);

   const uint32_t records[3][4] = {
      { f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f) },
      { 0x7fc00123u, 0x80000000u, f_bits(-5.0f), f_bits(0.5f) },
      { f_bits(9.0f), f_bits(8.0f), f_bits(7.0f), f_bits(6.0f) },
   };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)records,
      .stride = 16,
      .size_bytes = sizeof(records),
   };
   uint32_t carrier[12];
   rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 12);
   assert(rc == 0);
   assert(memcmp(carrier, records, sizeof(records)) == 0);
}

static void test_reference_fragment_module(void)
{
   nir_shader *nir = ingest(r3v_reference_fragment_spirv,
                            sizeof(r3v_reference_fragment_spirv),
                            MESA_SHADER_FRAGMENT);
   uint32_t color[4];
   const char *reason = NULL;
   bool admitted = r300_fragment_nir_constant_color(nir, color, &reason);
   ralloc_free(nir);
   assert(admitted);
   assert(color[0] == 0 && color[1] == 0x3f800000u && color[2] == 0 &&
          color[3] == 0x3f800000u);
}

static nir_builder builder_for(mesa_shader_stage stage, const char *name)
{
   return nir_builder_init_simple_shader(stage, r300_vertex_job_nir_options(),
                                         "%s", name);
}

static nir_def *load_attribute(nir_builder *b, unsigned base)
{
   return nir_load_input(b, 4, 32, nir_imm_int(b, 0), .base = base,
                         .component = 0,
                         .dest_type = nir_type_float32,
                         .io_semantics.location = VERT_ATTRIB_GENERIC0,
                         .io_semantics.num_slots = 1);
}

static void store_position(nir_builder *b, nir_def *value)
{
   nir_store_output(b, value, nir_imm_int(b, 0), .base = 0, .component = 0,
                    .write_mask = 0xf, .src_type = nir_type_float32,
                    .io_semantics.location = VARYING_SLOT_POS,
                    .io_semantics.num_slots = 1);
}

static bool vertex_admits(nir_shader *shader);

/* Strong ffma into a broadcast DP4: the analyzer must produce the exact
 * opcode sequence and the interpreter the exact host-model values,
 * with the twice-used constant deduplicated. */
static void test_arithmetic_lowering(void)
{
   nir_builder b = builder_for(MESA_SHADER_VERTEX, "arith");
   nir_def *input = load_attribute(&b, 0);
   nir_def *coeff = nir_imm_vec4(&b, 2.0f, -0.5f, 4.0f, 1.0f);
   nir_def *fma = nir_ffma(&b, input, coeff, coeff);
   nir_def *dot = nir_fdot4(&b, fma, coeff);
   store_position(&b, nir_vec4(&b, dot, dot, dot, dot));

   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_nir(b.shader, &job, &reason);
   ralloc_free(b.shader);
   assert(admitted);
   assert(job.instruction_count == 5 && job.constant_count == 1);
   assert(job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT);
   assert(job.instructions[1].opcode == R300_VERTEX_JOB_OP_LOAD_CONSTANT);
   assert(job.instructions[2].opcode == R300_VERTEX_JOB_OP_FFMA);
   assert(job.instructions[3].opcode == R300_VERTEX_JOB_OP_DP4);
   assert(job.instructions[4].opcode == R300_VERTEX_JOB_OP_STORE_POSITION);

   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   const float in[4] = { 1.5f, 3.0f, -1.0f, 0.25f };
   const float k[4] = { 2.0f, -0.5f, 4.0f, 1.0f };
   float expected = 0;
   float lanes[4];
   for (int c = 0; c < 4; c++)
      lanes[c] = in[c] * k[c] + k[c];
   expected = ((lanes[0] * k[0] + lanes[1] * k[1]) + lanes[2] * k[2]) +
              lanes[3] * k[3];

   const uint32_t record[4] = {
      f_bits(in[0]), f_bits(in[1]), f_bits(in[2]), f_bits(in[3]),
   };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)record,
      .stride = 16,
      .size_bytes = sizeof(record),
   };
   uint32_t carrier[4];
   int rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 1, carrier, 4);
   assert(rc == 0);
   for (int c = 0; c < 4; c++)
      assert(carrier[c] == f_bits(expected));
}

static void test_multiply_add_semantic_classes(void)
{
   nir_builder b = builder_for(MESA_SHADER_VERTEX, "multiply_add_classes");
   nir_def *input = load_attribute(&b, 0);
   nir_def *coefficient = nir_imm_vec4(&b, 0.5f, 1.0f, 2.0f, 4.0f);
   nir_def *unfused = nir_build_alu3(&b, nir_op_fmad, input, coefficient,
                                     coefficient);
   nir_def *weak = nir_build_alu3(&b, nir_op_ffma_weak, unfused,
                                  coefficient, coefficient);
   nir_def *precise_weak = nir_build_alu3(&b, nir_op_ffma_weak, weak,
                                          coefficient, coefficient);
   nir_def_as_alu(precise_weak)->fp_math_ctrl |= nir_fp_exact;
   nir_def *strong = nir_build_alu3(&b, nir_op_ffma, precise_weak,
                                    coefficient, coefficient);
   store_position(&b, strong);

   struct r300_vertex_job job;
   const char *reason = NULL;
   const bool admitted = r300_vertex_job_from_nir(b.shader, &job, &reason);
   ralloc_free(b.shader);
   assert(admitted);
   assert(job.instruction_count == 7 && job.constant_count == 1);
   assert(job.instructions[2].opcode == R300_VERTEX_JOB_OP_FMAD);
   assert(job.instructions[3].opcode == R300_VERTEX_JOB_OP_FMAD);
   assert(job.instructions[4].opcode == R300_VERTEX_JOB_OP_FFMA);
   assert(job.instructions[5].opcode == R300_VERTEX_JOB_OP_FFMA);
}

static void test_precise_weak_fma_constant_folding(void)
{
   nir_builder b = builder_for(MESA_SHADER_VERTEX, "precise_weak_constant");
   nir_def *multiplicand =
      nir_imm_vec4(&b, 0x1.001p0f, 0x1.001p0f, 0x1.001p0f, 0x1.001p0f);
   nir_def *addend =
      nir_imm_vec4(&b, -0x1.002p0f, -0x1.002p0f, -0x1.002p0f,
                   -0x1.002p0f);
   nir_def *fma = nir_build_alu3(&b, nir_op_ffma_weak, multiplicand,
                                 multiplicand, addend);
   nir_def_as_alu(fma)->fp_math_ctrl |= nir_fp_exact;
   store_position(&b, fma);

   const char *reason = NULL;
   assert(r300_vertex_job_nir_normalize(b.shader, &reason));
   struct r300_vertex_job job;
   assert(r300_vertex_job_from_nir(b.shader, &job, &reason));
   ralloc_free(b.shader);
   assert(job.instruction_count == 2 && job.constant_count == 1);
   assert(job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_CONSTANT);
   assert(job.instructions[1].opcode == R300_VERTEX_JOB_OP_STORE_POSITION);
   for (uint32_t component = 0; component < 4; component++)
      assert(job.constants[0][component] == 0x33800000u); /* 2^-24 */
}

static void test_spirv_float_control_admission(void)
{
   const struct spirv_to_nir_options *options =
      r300_vertex_job_spirv_options();
   assert(options->capabilities != NULL);
   assert(options->capabilities->Matrix);
   assert(options->capabilities->Shader);
   assert(!options->capabilities->DenormFlushToZero);
   assert(!options->capabilities->DenormPreserve);
   assert(!options->capabilities->FMAKHR);
   assert(!options->capabilities->RoundingModeRTE);
   assert(!options->capabilities->RoundingModeRTZ);
   assert(!options->capabilities->SignedZeroInfNanPreserve);

   uint32_t unsupported_capability[
      ARRAY_SIZE(r3v_reference_vertex_spirv) + 2];
   memcpy(unsupported_capability, r3v_reference_vertex_spirv,
          7 * sizeof(uint32_t));
   unsupported_capability[7] = 0x00020011u; /* OpCapability */
   unsupported_capability[8] = SpvCapabilityDenormPreserve;
   memcpy(&unsupported_capability[9], &r3v_reference_vertex_spirv[7],
          (ARRAY_SIZE(r3v_reference_vertex_spirv) - 7) * sizeof(uint32_t));
   nir_shader *rejected = r300_vertex_job_spirv_to_nir(
      unsupported_capability, ARRAY_SIZE(unsupported_capability),
      MESA_SHADER_VERTEX, "main");
   assert(rejected == NULL);

   nir_builder b = builder_for(MESA_SHADER_VERTEX, "rounding_mode");
   nir_def *input = load_attribute(&b, 0);
   store_position(&b, nir_fadd(&b, input, input));
   b.shader->info.float_controls_execution_mode =
      FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP32;
   assert(!vertex_admits(b.shader));

   b = builder_for(MESA_SHADER_VERTEX, "preservation_mode");
   input = load_attribute(&b, 0);
   nir_def *sum = nir_fadd(&b, input, input);
   nir_def_as_alu(sum)->fp_math_ctrl |= nir_fp_preserve_sz_inf_nan;
   store_position(&b, sum);
   assert(!vertex_admits(b.shader));
}

/* A non-green constant fragment shader reads back with its own bits;
 * the green gate is the pipeline's, not the analyzer's. */
static void test_fragment_constant_readback(void)
{
   nir_builder b = builder_for(MESA_SHADER_FRAGMENT, "red");
   nir_def *red = nir_imm_vec4(&b, 1.0f, 0.0f, 0.0f, 1.0f);
   nir_store_output(&b, red, nir_imm_int(&b, 0), .base = 0, .component = 0,
                    .write_mask = 0xf, .src_type = nir_type_float32,
                    .io_semantics.location = FRAG_RESULT_DATA0,
                    .io_semantics.num_slots = 1);
   uint32_t color[4];
   const char *reason = NULL;
   bool admitted = r300_fragment_nir_constant_color(b.shader, color, &reason);
   ralloc_free(b.shader);
   assert(admitted);
   assert(color[0] == 0x3f800000u && color[1] == 0 && color[2] == 0 &&
          color[3] == 0x3f800000u);
}

static bool vertex_admits(nir_shader *shader)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_nir(shader, &job, &reason);
   ralloc_free(shader);
   return admitted;
}

static void test_refusals(void)
{
   /* Control flow. */
   nir_builder b = builder_for(MESA_SHADER_VERTEX, "flow");
   nir_def *input = load_attribute(&b, 0);
   nir_push_if(&b, nir_imm_true(&b));
   nir_pop_if(&b, NULL);
   store_position(&b, input);
   assert(!vertex_admits(b.shader));

   /* Attribute outside slot 0. */
   b = builder_for(MESA_SHADER_VERTEX, "attr1");
   store_position(&b, load_attribute(&b, 1));
   assert(!vertex_admits(b.shader));

   /* Second position store. */
   b = builder_for(MESA_SHADER_VERTEX, "twostore");
   input = load_attribute(&b, 0);
   store_position(&b, input);
   store_position(&b, input);
   assert(!vertex_admits(b.shader));

   /* Non-identity swizzle. */
   b = builder_for(MESA_SHADER_VERTEX, "swizzle");
   input = load_attribute(&b, 0);
   store_position(&b, nir_swizzle(&b, input, (unsigned[]){ 1, 0, 2, 3 }, 4));
   assert(!vertex_admits(b.shader));

   /* A scalar dot stored without its broadcast. */
   b = builder_for(MESA_SHADER_VERTEX, "scalardot");
   input = load_attribute(&b, 0);
   nir_def *dot = nir_fdot4(&b, input, input);
   nir_store_output(&b, dot, nir_imm_int(&b, 0), .base = 0, .component = 0,
                    .write_mask = 0x1, .src_type = nir_type_float32,
                    .io_semantics.location = VARYING_SLOT_POS,
                    .io_semantics.num_slots = 1);
   assert(!vertex_admits(b.shader));

   /* Arithmetic outside the admitted operator set. */
   b = builder_for(MESA_SHADER_VERTEX, "fmax");
   input = load_attribute(&b, 0);
   store_position(&b, nir_fmax(&b, input, input));
   assert(!vertex_admits(b.shader));
}

/* Both front ends lower a reference module against one job contract,
 * so the parity check is bit equality of the finished jobs and of the
 * fragment color bits, plus refusal agreement on the compute modules
 * neither vertex path admits.
 */
static void test_front_end_parity(void)
{
   /* Identity vertex module: bit-identical jobs. */
   nir_shader *nir = ingest(r3v_reference_vertex_spirv,
                            sizeof(r3v_reference_vertex_spirv),
                            MESA_SHADER_VERTEX);
   struct r300_vertex_job nir_job, direct_job;
   memset(&nir_job, 0, sizeof(nir_job));
   const char *reason = NULL;
   assert(r300_vertex_job_from_nir(nir, &nir_job, &reason));
   ralloc_free(nir);
   assert(r300_vertex_job_from_spirv(
      r3v_reference_vertex_spirv,
      sizeof(r3v_reference_vertex_spirv) / 4, "main", &direct_job, &reason));
   assert(memcmp(&nir_job, &direct_job, sizeof(nir_job)) == 0);
   record_words_digest("identity_vertex", "spirv", r3v_reference_vertex_spirv,
                       sizeof(r3v_reference_vertex_spirv) / 4);
   {
      /* The divergent-job known-bad binds a job one opcode away from the
       * one both front ends produced, so the retained manifest row is the
       * single verdict that fails. */
      struct r300_vertex_job manifest_job = direct_job;
      if (inject_divergent_job)
         manifest_job.instructions[0].opcode = R300_VERTEX_JOB_OP_MOV;
      record_job_digest("identity_vertex", "job", &manifest_job);
   }
   {
      struct r300_vertex_job identity = direct_job;
      identity.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
      assert(r300_cpu_vertex_job_validate(&identity) == 0);
      const uint32_t identity_records[3][4] = {
         { f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f) },
         { 0x7fc00123u, 0x80000000u, f_bits(-5.0f), f_bits(0.5f) },
         { f_bits(9.0f), f_bits(8.0f), f_bits(7.0f), f_bits(6.0f) },
      };
      const struct r300_vertex_stream identity_stream = {
         .data = (const uint8_t *)identity_records,
         .stride = 16,
         .size_bytes = sizeof(identity_records),
      };
      uint32_t identity_carrier[12];
      assert(r300_cpu_vertex_job_execute(&identity, &identity_stream, 0, 3,
                                         identity_carrier, 12) == 0);
      record_words_digest("identity_vertex", "carrier", identity_carrier, 12);
   }

   /* Fragment module: identical color bits. */
   nir = ingest(r3v_reference_fragment_spirv,
                sizeof(r3v_reference_fragment_spirv),
                MESA_SHADER_FRAGMENT);
   uint32_t nir_color[4], direct_color[4];
   assert(r300_fragment_nir_constant_color(nir, nir_color, &reason));
   ralloc_free(nir);
   assert(r300_fragment_constant_color_from_spirv(
      r3v_reference_fragment_spirv,
      sizeof(r3v_reference_fragment_spirv) / 4,
      "main", direct_color, &reason));
   assert(memcmp(nir_color, direct_color, sizeof(nir_color)) == 0);
   record_words_digest("constant_fragment", "spirv",
                       r3v_reference_fragment_spirv,
                       sizeof(r3v_reference_fragment_spirv) / 4);
   record_words_digest("constant_fragment", "color", direct_color, 4);

   /* Arithmetic vertex module: NIR's optimizing ingestion and the
    * direct walk may select instructions differently, so parity is
    * value equality of the executed carriers over hostile inputs.
    */
   nir = ingest(r3v_reference_vertex_arith_spirv,
                sizeof(r3v_reference_vertex_arith_spirv),
                MESA_SHADER_VERTEX);
   memset(&nir_job, 0, sizeof(nir_job));
   assert(r300_vertex_job_from_nir(nir, &nir_job, &reason));
   ralloc_free(nir);
   assert(r300_vertex_job_from_spirv(
      r3v_reference_vertex_arith_spirv,
      sizeof(r3v_reference_vertex_arith_spirv) / 4, "main", &direct_job,
      &reason));
   nir_job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   direct_job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_validate(&nir_job) == 0);
   assert(r300_cpu_vertex_job_validate(&direct_job) == 0);
   const uint32_t records[2][4] = {
      { f_bits(1.5f), f_bits(3.0f), f_bits(-1.0f), f_bits(0.25f) },
      { f_bits(-0.0f), f_bits(0.5f), 0x00000001u, f_bits(2.0f) },
   };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)records,
      .stride = 16,
      .size_bytes = sizeof(records),
   };
   uint32_t nir_carrier[8], direct_carrier[8];
   assert(r300_cpu_vertex_job_execute(&nir_job, &stream, 0, 2,
                                      nir_carrier, 8) == 0);
   assert(r300_cpu_vertex_job_execute(&direct_job, &stream, 0, 2,
                                      direct_carrier, 8) == 0);
   assert(memcmp(nir_carrier, direct_carrier, sizeof(nir_carrier)) == 0);
   record_words_digest("arith_vertex", "spirv",
                       r3v_reference_vertex_arith_spirv,
                       sizeof(r3v_reference_vertex_arith_spirv) / 4);
   record_words_digest("arith_vertex", "carrier", direct_carrier, 8);

   /* Refusal agreement: the compute modules admit through neither
    * vertex path. */
   assert(r300_vertex_job_spirv_to_nir(
             r3v_reference_identity_map_spirv,
             sizeof(r3v_reference_identity_map_spirv) / 4,
             MESA_SHADER_VERTEX, "main") == NULL);
   assert(!r300_vertex_job_from_spirv(
      r3v_reference_identity_map_spirv,
      sizeof(r3v_reference_identity_map_spirv) / 4, "main", &direct_job,
      &reason));
   assert(!r300_vertex_job_from_spirv(
      r3v_reference_scatter_reject_spirv,
      sizeof(r3v_reference_scatter_reject_spirv) / 4, "main", &direct_job,
      &reason));
}

/* Compare the recorded rows with the retained manifest: every recorded
 * row present with the same digest, and no manifest row unrecorded. */
static int check_manifest(const char *path)
{
   FILE *file = fopen(path, "r");
   if (file == NULL) {
      fprintf(stderr, "parity manifest unreadable: %s\n", path);
      return 1;
   }
   unsigned failures = 0;
   bool seen[MANIFEST_MAX_ROWS] = { false };
   char line[256];
   while (fgets(line, sizeof(line), file) != NULL) {
      if (line[0] == '#' || line[0] == '\n')
         continue;
      char shape[64], field[64], digest[BLAKE3_HEX_LEN + 8];
      if (sscanf(line, "%63s %63s %72s", shape, field, digest) != 3) {
         fprintf(stderr, "parity manifest malformed row: %s", line);
         failures++;
         continue;
      }
      bool matched = false;
      for (unsigned i = 0; i < manifest_row_count; i++) {
         const struct manifest_row *row = &manifest_rows[i];
         if (strcmp(row->shape, shape) != 0 || strcmp(row->field, field) != 0)
            continue;
         matched = true;
         seen[i] = true;
         if (strcmp(row->digest, digest) != 0) {
            fprintf(stderr, "parity manifest %s %s: retained %s observed "
                            "%s\n", shape, field, digest, row->digest);
            failures++;
         }
      }
      if (!matched) {
         fprintf(stderr, "parity manifest row %s %s names no recorded "
                         "shape\n", shape, field);
         failures++;
      }
   }
   fclose(file);
   for (unsigned i = 0; i < manifest_row_count; i++) {
      if (!seen[i]) {
         fprintf(stderr, "parity manifest lacks %s %s\n",
                 manifest_rows[i].shape, manifest_rows[i].field);
         failures++;
      }
   }
   return failures != 0;
}

int main(int argc, char **argv)
{
   const char *manifest = NULL;
   bool print_manifest = false;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--print-manifest") == 0)
         print_manifest = true;
      else if (strcmp(argv[i], "--inject-divergent-job") == 0)
         inject_divergent_job = true;
      else if (manifest == NULL)
         manifest = argv[i];
      else {
         fprintf(stderr, "usage: %s [--print-manifest] "
                         "[--inject-divergent-job] [manifest]\n", argv[0]);
         return 2;
      }
   }

   glsl_type_singleton_init_or_ref();
   test_reference_vertex_module();
   test_reference_fragment_module();
   test_arithmetic_lowering();
   test_multiply_add_semantic_classes();
   test_precise_weak_fma_constant_folding();
   test_spirv_float_control_admission();
   test_fragment_constant_readback();
   test_refusals();
   test_front_end_parity();
   glsl_type_singleton_decref();

   if (print_manifest) {
      for (unsigned i = 0; i < manifest_row_count; i++)
         printf("%s %s %s\n", manifest_rows[i].shape, manifest_rows[i].field,
                manifest_rows[i].digest);
      return 0;
   }
   if (manifest != NULL && check_manifest(manifest) != 0) {
      printf("r300_vertex_front_end_parity_test: retained manifest "
             "mismatch\n");
      return 1;
   }
   printf("r300_vertex_front_end_parity_test: all cases pass\n");
   return 0;
}
