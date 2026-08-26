/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration for the native pipeline front end: the reference cell
 * modules admit through the direct SPIR-V word-stream reader by
 * meaning, the admitted jobs execute to exact bytes through the CPU
 * executor, inadmissible modules refuse by shape, and malformed word
 * streams refuse at their bounds.  The admitter is plain C over words,
 * so this test carries no compiler-stack dependency.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/common/r300_vertex_spirv.h"
#include "amd/r300/cpu/r300_cpu_vertex_job.h"

#include "r3v_native_reference_spirv.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS(array) (sizeof(array) / sizeof((array)[0]))

static uint32_t f_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

/* The reference vertex module is the position pass-through, so the
 * admitter must lower it to the identity job and the job must
 * reproduce the gather bytes exactly, hostile bit patterns included.
 */
static void test_reference_vertex_module(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_spirv(
      r3v_reference_vertex_spirv, WORDS(r3v_reference_vertex_spirv),
      "main", &job,
      &reason);
   if (!admitted)
      fprintf(stderr, "reference vertex refusal: %s\n", reason);
   assert(admitted);
   assert(job.instruction_count == 2 && job.constant_count == 0);
   assert(job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT);
   assert(job.instructions[1].opcode == R300_VERTEX_JOB_OP_STORE_POSITION);
   assert(job.instructions[1].src0 == job.instructions[0].dst);

   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_validate(&job) == 0);

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
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 12) ==
          0);
   assert(memcmp(carrier, records, sizeof(records)) == 0);
}

static void test_reference_fragment_module(void)
{
   uint32_t color[4];
   const char *reason = NULL;
   assert(r300_fragment_constant_color_from_spirv(
      r3v_reference_fragment_spirv, WORDS(r3v_reference_fragment_spirv),
      "main",
      color, &reason));
   assert(color[0] == 0 && color[1] == 0x3f800000u && color[2] == 0 &&
          color[3] == 0x3f800000u);

   /* Another constant reads back with its own bits: the admitter
    * reports what the module wrote, and the pipeline's own gate is the
    * FP24 lattice the register encodes.  The red module is the
    * reference module with its composite constant's lanes reordered,
    * located by the OpConstantComposite word (opcode 44, length 7).
    */
   uint32_t red[WORDS(r3v_reference_fragment_spirv)];
   memcpy(red, r3v_reference_fragment_spirv, sizeof(red));
   size_t at = 5;
   bool patched = false;
   while (at < WORDS(red)) {
      const uint32_t len = red[at] >> 16;
      assert(len != 0 && at + len <= WORDS(red));
      if ((red[at] & 0xffffu) == 44) {
         const uint32_t f0 = red[at + 3];
         const uint32_t f1 = red[at + 4];
         red[at + 3] = f1;
         red[at + 4] = f0;
         red[at + 5] = f0;
         red[at + 6] = f1;
         patched = true;
      }
      at += len;
   }
   assert(patched);
   assert(r300_fragment_constant_color_from_spirv(red, WORDS(red),
   "main", color,
                                                  &reason));
   assert(color[0] == 0x3f800000u && color[1] == 0 && color[2] == 0 &&
          color[3] == 0x3f800000u);
}


/* The sampled-texture reader admits the canonical module alone: the
 * other two fragment readers refuse it, the sampled reader refuses the
 * pass-through module, and a descriptor-set or binding literal moved
 * off zero refuses.
 */
static void test_reference_sampled_module(void)
{
   const char *reason = NULL;
   uint32_t color[4];
   assert(r300_fragment_sampled_texture_from_spirv(
      r3v_reference_fragment_sampled_spirv,
      WORDS(r3v_reference_fragment_sampled_spirv), "main", &reason));
   assert(!r300_fragment_constant_color_from_spirv(
      r3v_reference_fragment_sampled_spirv,
      WORDS(r3v_reference_fragment_sampled_spirv), "main", color,
      &reason));
   assert(!r300_fragment_varying_passthrough_from_spirv(
      r3v_reference_fragment_sampled_spirv,
      WORDS(r3v_reference_fragment_sampled_spirv), "main", &reason));
   assert(!r300_fragment_sampled_texture_from_spirv(
      r3v_reference_fragment_varying_spirv,
      WORDS(r3v_reference_fragment_varying_spirv), "main", &reason));

   /* Move each descriptor literal off zero: OpDecorate (opcode 71,
    * length 4) with Binding (33) or DescriptorSet (34). */
   for (uint32_t decor = 33; decor <= 34; decor++) {
      uint32_t bad[WORDS(r3v_reference_fragment_sampled_spirv)];
      memcpy(bad, r3v_reference_fragment_sampled_spirv, sizeof(bad));
      size_t at = 5;
      bool patched = false;
      while (at < WORDS(bad)) {
         const uint32_t len = bad[at] >> 16;
         assert(len != 0 && at + len <= WORDS(bad));
         if ((bad[at] & 0xffffu) == 71 && len == 4 &&
             bad[at + 2] == decor) {
            bad[at + 3] = 1;
            patched = true;
         }
         at += len;
      }
      assert(patched);
      assert(!r300_fragment_sampled_texture_from_spirv(
         bad, WORDS(bad), "main", &reason));
   }
}

/* The arithmetic reference exercises the constant, FFMA, DP4,
 * function-variable, and broadcast-replicate paths; the executed value
 * is the host model's exact composition.
 */
static void test_reference_arith_module(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_spirv(
      r3v_reference_vertex_arith_spirv,
      WORDS(r3v_reference_vertex_arith_spirv), "main", &job, &reason);
   if (!admitted)
      fprintf(stderr, "reference arith refusal: %s\n", reason);
   assert(admitted);
   assert(job.instruction_count == 5 && job.constant_count == 1);
   assert(job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT);
   assert(job.instructions[1].opcode == R300_VERTEX_JOB_OP_LOAD_CONSTANT);
   assert(job.instructions[2].opcode == R300_VERTEX_JOB_OP_FFMA);
   assert(job.instructions[3].opcode == R300_VERTEX_JOB_OP_DP4);
   assert(job.instructions[4].opcode == R300_VERTEX_JOB_OP_STORE_POSITION);

   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   const float in[4] = { 1.5f, 3.0f, -1.0f, 0.25f };
   const float k[4] = { 2.0f, -0.5f, 4.0f, 1.0f };
   float lanes[4];
   for (int c = 0; c < 4; c++)
      lanes[c] = fmaf(in[c], k[c], k[c]);
   const float expected =
      ((lanes[0] * k[0] + lanes[1] * k[1]) + lanes[2] * k[2]) +
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
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 1, carrier, 4) ==
          0);
   for (int c = 0; c < 4; c++)
      assert(carrier[c] == f_bits(expected));
}

/* The computed-varying pair: the vertex module lowers to a job storing
 * one varying ahead of the position -- whatever order the module stores
 * its outputs in -- and executes to eight-dword records that carry the
 * reference varying payload over the NDC triangle; the fragment module
 * admits as the pass-through shape alone.
 */
static void test_reference_varying_modules(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_spirv(
      r3v_reference_vertex_varying_spirv,
      WORDS(r3v_reference_vertex_varying_spirv), "main", &job, &reason);
   if (!admitted)
      fprintf(stderr, "reference varying refusal: %s\n", reason);
   assert(admitted);
   assert(r300_vertex_job_has_varying(&job));
   assert(r300_vertex_job_record_dwords(&job) == 8);
   assert(job.instructions[job.instruction_count - 1].opcode ==
          R300_VERTEX_JOB_OP_STORE_POSITION);
   unsigned varying_stores = 0;
   for (uint32_t i = 0; i < job.instruction_count; i++)
      varying_stores +=
         job.instructions[i].opcode == R300_VERTEX_JOB_OP_STORE_VARYING;
   assert(varying_stores == 1);
   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   /* The NDC reference triangle; the job leaves the position in clip
    * space (the route transforms it) and computes the tint. */
   const float ndc[12] = { -0.75f, -0.75f, 0.0f, 1.0f, 0.75f, -0.75f,
                           0.0f,   1.0f,   0.0f, 0.75f, 0.0f, 1.0f };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)ndc,
      .stride = 16,
      .size_bytes = sizeof(ndc),
   };
   uint32_t carrier[24];
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 24) ==
          0);
   for (unsigned v = 0; v < 3; v++) {
      assert(memcmp(&carrier[v * 8], &ndc[v * 4], 16) == 0);
      for (unsigned c = 0; c < 4; c++) {
         static const float scale[4] = { 0.5f, 0.5f, 0.0f, 0.0f };
         static const float bias[4] = { 0.5f, 0.5f, 0.25f, 1.0f };
         assert(carrier[v * 8 + 4 + c] ==
                f_bits(fmaf(ndc[v * 4 + c], scale[c], bias[c])));
      }
   }

   assert(r300_fragment_varying_passthrough_from_spirv(
      r3v_reference_fragment_varying_spirv,
      WORDS(r3v_reference_fragment_varying_spirv), "main", &reason));
   /* Each fragment admitter refuses the other's shape. */
   uint32_t color[4];
   assert(!r300_fragment_constant_color_from_spirv(
      r3v_reference_fragment_varying_spirv,
      WORDS(r3v_reference_fragment_varying_spirv), "main", color, &reason));
   assert(strcmp(reason, "fragment shader reads an input") == 0);
   assert(!r300_fragment_varying_passthrough_from_spirv(
      r3v_reference_fragment_spirv, WORDS(r3v_reference_fragment_spirv),
      "main", &reason));
   assert(strcmp(reason,
                 "fragment program outside the varying pass-through") == 0);
   /* The position-only modules keep lowering to varying-free jobs. */
   assert(r300_vertex_job_from_spirv(r3v_reference_vertex_spirv,
                                     WORDS(r3v_reference_vertex_spirv),
                                     "main", &job, &reason));
   assert(!r300_vertex_job_has_varying(&job));
}

/* Known-bads on the varying vertex module: the varying store removed
 * (the declared output left unwritten), and the varying store doubled.
 * Both mutate the generated words at the OpStore whose pointer is the
 * varying variable, located by the Location decoration on an Output
 * variable.
 */
static void test_varying_module_known_bads(void)
{
   enum { OP_DECORATE = 71, OP_VARIABLE = 59, OP_STORE = 62,
          DECOR_LOCATION = 30, SC_OUTPUT = 3 };
   const uint32_t *m = r3v_reference_vertex_varying_spirv;
   const size_t n = WORDS(r3v_reference_vertex_varying_spirv);
   uint32_t varying_id = 0, store_at = 0, store_len = 0;
   for (size_t at = 5; at < n;) {
      const uint32_t len = m[at] >> 16, op = m[at] & 0xffffu;
      assert(len != 0 && at + len <= n);
      if (op == OP_VARIABLE && m[at + 3] == SC_OUTPUT) {
         /* A Location-decorated Output variable is the varying. */
         for (size_t d = 5; d < n;) {
            const uint32_t dlen = m[d] >> 16, dop = m[d] & 0xffffu;
            if (dop == OP_DECORATE && m[d + 1] == m[at + 2] &&
                m[d + 2] == DECOR_LOCATION)
               varying_id = m[at + 2];
            d += dlen;
         }
      }
      if (op == OP_STORE && varying_id != 0 && m[at + 1] == varying_id) {
         store_at = (uint32_t)at;
         store_len = len;
      }
      at += len;
   }
   assert(varying_id != 0 && store_at != 0 && store_len == 3);

   struct r300_vertex_job job;
   const char *reason = NULL;
   /* Removed: the words after the store close the gap. */
   uint32_t removed[WORDS(r3v_reference_vertex_varying_spirv)];
   memcpy(removed, m, store_at * 4);
   memcpy(removed + store_at, m + store_at + store_len,
          (n - store_at - store_len) * 4);
   assert(!r300_vertex_job_from_spirv(removed, n - store_len, "main", &job,
                                      &reason));
   assert(strcmp(reason, "vertex varying output left unwritten") == 0);
   /* Doubled: the store repeated in place. */
   uint32_t doubled[WORDS(r3v_reference_vertex_varying_spirv) + 3];
   memcpy(doubled, m, (store_at + store_len) * 4);
   memcpy(doubled + store_at + store_len, m + store_at, store_len * 4);
   memcpy(doubled + store_at + 2 * store_len, m + store_at + store_len,
          (n - store_at - store_len) * 4);
   assert(!r300_vertex_job_from_spirv(doubled, n + store_len, "main", &job,
                                      &reason));
   assert(strcmp(reason, "second varying store") == 0);
}

/* The two-attribute module: location 0 feeds the position, location 1
 * the varying, so the job reads slots 0 and 1 and executes over one
 * stream per slot; the color stream at its own stride reaches the
 * record's varying vector verbatim.  Known-bads mutate the Location
 * decorations: location 16 lies beyond the slot count, and two inputs
 * at one location collide.
 */
static void test_reference_two_attribute_module(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   const uint32_t *m = r3v_reference_vertex_two_attributes_spirv;
   const size_t n = WORDS(r3v_reference_vertex_two_attributes_spirv);
   bool admitted = r300_vertex_job_from_spirv(m, n, "main", &job, &reason);
   if (!admitted)
      fprintf(stderr, "reference two-attribute refusal: %s\n", reason);
   assert(admitted);
   assert(r300_vertex_job_input_mask(&job) == 0x3u);
   assert(r300_vertex_job_has_varying(&job));
   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   job.input_format_ids[1] = R300_VERTEX_FORMAT_F32_3;
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   const float ndc[12] = { -0.75f, -0.75f, 0.0f, 1.0f, 0.75f, -0.75f,
                           0.0f,   1.0f,   0.0f, 0.75f, 0.0f, 1.0f };
   const float colors[9] = { 0.125f, 0.125f, 0.25f, 0.875f, 0.125f,
                             0.25f,  0.5f,   0.875f, 0.25f };
   const struct r300_vertex_stream streams[2] = {
      { .data = (const uint8_t *)ndc, .stride = 16,
        .size_bytes = sizeof(ndc) },
      { .data = (const uint8_t *)colors, .stride = 12,
        .size_bytes = sizeof(colors) },
   };
   uint32_t carrier[24];
   assert(r300_cpu_vertex_job_execute(&job, streams, 0, 3, carrier, 24) ==
          0);
   for (unsigned v = 0; v < 3; v++) {
      assert(memcmp(&carrier[v * 8], &ndc[v * 4], 16) == 0);
      assert(memcmp(&carrier[v * 8 + 4], &colors[v * 3], 12) == 0);
      assert(carrier[v * 8 + 7] == f_bits(1.0f));
   }

   /* The Location decorations of the two Input variables. */
   enum { OP_DECORATE = 71, OP_VARIABLE = 59, DECOR_LOCATION = 30,
          SC_INPUT = 1 };
   uint32_t location_at[2] = { 0, 0 };
   unsigned located = 0;
   for (size_t at = 5; at < n;) {
      const uint32_t len = m[at] >> 16, op = m[at] & 0xffffu;
      assert(len != 0 && at + len <= n);
      if (op == OP_VARIABLE && m[at + 3] == SC_INPUT) {
         for (size_t d = 5; d < n;) {
            const uint32_t dlen = m[d] >> 16, dop = m[d] & 0xffffu;
            if (dop == OP_DECORATE && m[d + 1] == m[at + 2] &&
                m[d + 2] == DECOR_LOCATION) {
               assert(located < 2);
               location_at[located++] = (uint32_t)d + 3;
            }
            d += dlen;
         }
      }
      at += len;
   }
   assert(located == 2);
   uint32_t mutated[WORDS(r3v_reference_vertex_two_attributes_spirv)];
   memcpy(mutated, m, n * 4);
   /* The color input relocated beyond the slot count. */
   const unsigned color_index = m[location_at[0]] == 1 ? 0 : 1;
   mutated[location_at[color_index]] = R300_VERTEX_JOB_MAX_INPUTS;
   assert(!r300_vertex_job_from_spirv(mutated, n, "main", &job, &reason));
   assert(strcmp(reason,
                 "vertex input location beyond the attribute slots") == 0);
   /* The color input relocated onto the position's location. */
   mutated[location_at[color_index]] = 0;
   assert(!r300_vertex_job_from_spirv(mutated, n, "main", &job, &reason));
   assert(strcmp(reason, "two vertex inputs at one location") == 0);
   /* The slot count itself admits: location 15 is the last slot. */
   mutated[location_at[color_index]] = R300_VERTEX_JOB_MAX_INPUTS - 1;
   assert(r300_vertex_job_from_spirv(mutated, n, "main", &job, &reason));
   assert(r300_vertex_job_input_mask(&job) ==
          (1u | 1u << (R300_VERTEX_JOB_MAX_INPUTS - 1)));
}

/* The instance-offset module reads two located vec4 inputs; the
 * instance-index and vertex-index modules load an int builtin, convert
 * it, replicate it, and fma it onto the position.  The jobs execute to
 * the exact bytes the harness arms expect, and the builtin surface
 * refuses by name: a builtin outside the two, a load of the builtin as
 * a float, and a conversion of a non-builtin operand. */
static void test_reference_instance_modules(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_spirv(
      r3v_reference_vertex_instance_offset_spirv,
      WORDS(r3v_reference_vertex_instance_offset_spirv), "main", &job,
      &reason);
   if (!admitted)
      fprintf(stderr, "reference instance-offset refusal: %s\n", reason);
   assert(admitted);
   assert(r300_vertex_job_input_mask(&job) == 0x3u);
   assert(!r300_vertex_job_reads_system_value(
      &job, R300_VERTEX_JOB_SV_INSTANCE_INDEX));

   const uint32_t *m = r3v_reference_vertex_instance_index_spirv;
   const size_t n = WORDS(r3v_reference_vertex_instance_index_spirv);
   admitted = r300_vertex_job_from_spirv(m, n, "main", &job, &reason);
   if (!admitted)
      fprintf(stderr, "reference instance-index refusal: %s\n", reason);
   assert(admitted);
   assert(r300_vertex_job_input_mask(&job) == 0x1u);
   assert(r300_vertex_job_reads_system_value(
      &job, R300_VERTEX_JOB_SV_INSTANCE_INDEX));
   assert(!r300_vertex_job_reads_system_value(
      &job, R300_VERTEX_JOB_SV_VERTEX_INDEX));
   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_validate(&job) == 0);
   const float ndc[12] = { -0.75f, -0.75f, 0.0f, 1.0f, 0.75f, -0.75f,
                           0.0f,   1.0f,   0.0f, 0.75f, 0.0f, 1.0f };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)ndc, .stride = 16, .size_bytes = sizeof(ndc),
   };
   const struct r300_cpu_vertex_draw draw = {
      .vertex_count = 3, .first_instance = 3, .instance_count = 2,
   };
   uint32_t carrier[24];
   assert(r300_cpu_vertex_job_execute_draw(&job, &stream, &draw, carrier,
                                           24) == 0);
   for (unsigned i = 0; i < 2; i++) {
      for (unsigned v = 0; v < 3; v++) {
         const uint32_t *record = &carrier[(i * 3 + v) * 4];
         assert(record[0] == f_bits(ndc[v * 4] + 0.0625f * (float)(3 + i)));
         assert(record[1] == f_bits(ndc[v * 4 + 1]));
         assert(record[3] == f_bits(1.0f));
      }
   }

   const uint32_t *vm = r3v_reference_vertex_vertex_index_spirv;
   const size_t vn = WORDS(r3v_reference_vertex_vertex_index_spirv);
   admitted = r300_vertex_job_from_spirv(vm, vn, "main", &job, &reason);
   if (!admitted)
      fprintf(stderr, "reference vertex-index refusal: %s\n", reason);
   assert(admitted);
   assert(r300_vertex_job_reads_system_value(
      &job, R300_VERTEX_JOB_SV_VERTEX_INDEX));
   job.input_format_ids[0] = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 12) ==
          0);
   for (unsigned v = 0; v < 3; v++)
      assert(carrier[v * 4 + 1] == f_bits(ndc[v * 4 + 1] + 0.0625f * v));

   /* Known-bads over the instance-index module.  The BuiltIn decoration
    * payload, the int load's result type, the conversion, and the
    * position load are located by opcode walk. */
   enum { OP_DECORATE = 71, OP_LOAD = 61, OP_CONVERT_S_TO_F = 111,
          OP_TYPE_FLOAT = 22, DECOR_BUILTIN = 11,
          BUILTIN_INSTANCE_INDEX = 43 };
   size_t builtin_at = 0, int_load_at = 0, convert_at = 0, float_type = 0,
          position_load_at = 0;
   for (size_t at = 5; at < n;) {
      const uint32_t len = m[at] >> 16, op = m[at] & 0xffffu;
      assert(len != 0 && at + len <= n);
      if (op == OP_DECORATE && m[at + 2] == DECOR_BUILTIN &&
          m[at + 3] == BUILTIN_INSTANCE_INDEX)
         builtin_at = at + 3;
      if (op == OP_TYPE_FLOAT)
         float_type = m[at + 1];
      if (op == OP_CONVERT_S_TO_F)
         convert_at = at;
      if (op == OP_LOAD && m[at + 1] != float_type &&
          int_load_at == 0 && convert_at == 0)
         int_load_at = at;
      else if (op == OP_LOAD && convert_at != 0)
         position_load_at = at;
      at += len;
   }
   assert(builtin_at && int_load_at && convert_at && float_type &&
          position_load_at);
   assert(m[convert_at + 3] == m[int_load_at + 2]);
   uint32_t mutated[WORDS(r3v_reference_vertex_instance_index_spirv)];
   /* The builtin renamed outside the two index builtins. */
   memcpy(mutated, m, n * 4);
   mutated[builtin_at] = 5;
   assert(!r300_vertex_job_from_spirv(mutated, n, "main", &job, &reason));
   assert(strcmp(reason, "int vertex input outside the VertexIndex and "
                         "InstanceIndex builtins") == 0);
   /* The builtin renamed to VertexIndex admits as that value. */
   mutated[builtin_at] = 42;
   assert(r300_vertex_job_from_spirv(mutated, n, "main", &job, &reason));
   assert(r300_vertex_job_reads_system_value(
      &job, R300_VERTEX_JOB_SV_VERTEX_INDEX));
   /* The builtin loaded as a float. */
   memcpy(mutated, m, n * 4);
   mutated[int_load_at + 1] = (uint32_t)float_type;
   assert(!r300_vertex_job_from_spirv(mutated, n, "main", &job, &reason));
   assert(strcmp(reason, "system value loaded outside its int type") == 0);
   /* The conversion's operand redirected at the position load's result
    * id: the module loads the position after the conversion, so the id
    * names no defined value there and the operand refuses as outside a
    * loaded system value. */
   memcpy(mutated, m, n * 4);
   mutated[convert_at + 3] = m[position_load_at + 2];
   assert(!r300_vertex_job_from_spirv(mutated, n, "main", &job, &reason));
   assert(strcmp(reason, "conversion outside a loaded system value to "
                         "float") == 0);
}

/* Whole-module refusals: each inadmissible module names its construct. */
static void test_module_refusals(void)
{
   struct r300_vertex_job job;
   uint32_t color[4];
   const char *reason = NULL;

   /* The compute modules carry the GLCompute entry model. */
   assert(!r300_vertex_job_from_spirv(r3v_reference_identity_map_spirv,
                                      WORDS(r3v_reference_identity_map_spirv),
                                      "main",
                                      &job, &reason));
   assert(!r300_vertex_job_from_spirv(r3v_reference_scatter_reject_spirv,
                                      WORDS(r3v_reference_scatter_reject_spirv),
                                      "main",
                                      &job, &reason));

   /* Stage crosses: the fragment module refuses as a vertex program
    * and the vertex module as a fragment program. */
   assert(!r300_vertex_job_from_spirv(r3v_reference_fragment_spirv,
                                      WORDS(r3v_reference_fragment_spirv),
                                      "main",
                                      &job, &reason));
   assert(!r300_fragment_constant_color_from_spirv(
      r3v_reference_vertex_spirv, WORDS(r3v_reference_vertex_spirv),
      "main", color,
      &reason));

   /* The fragment path admits constants alone, so the arithmetic
    * module refuses there. */
   assert(!r300_fragment_constant_color_from_spirv(
      r3v_reference_vertex_arith_spirv,
      WORDS(r3v_reference_vertex_arith_spirv), "main", color, &reason));
}

/* Malformed streams: NULL, short, wrong magic, and every word-boundary
 * truncation refuse rather than read past a bound.
 */
static void test_malformed_streams(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;

   assert(!r300_vertex_job_from_spirv(NULL, 0, "main", &job, &reason));
   assert(!r300_vertex_job_from_spirv(r3v_reference_vertex_spirv, 4,
                                      "main", &job,
                                      &reason));

   uint32_t bad_magic[WORDS(r3v_reference_vertex_spirv)];
   memcpy(bad_magic, r3v_reference_vertex_spirv, sizeof(bad_magic));
   bad_magic[0] ^= 1;
   assert(!r300_vertex_job_from_spirv(bad_magic, WORDS(bad_magic),
   "main", &job,
                                      &reason));

   for (size_t count = 5; count < WORDS(r3v_reference_vertex_spirv);
        count++) {
      assert(!r300_vertex_job_from_spirv(r3v_reference_vertex_spirv, count,
                                         "main",
                                         &job, &reason));
   }
}

/* Entry-point binding: the OpEntryPoint literal binds to the requested
 * name byte for byte, and a NULL request refuses before the module is
 * read.
 */
static void test_entry_name_binding(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   assert(!r300_vertex_job_from_spirv(r3v_reference_vertex_spirv,
                                      WORDS(r3v_reference_vertex_spirv),
                                      "other", &job, &reason));
   assert(strcmp(reason, "entry point name outside the request") == 0);
   assert(!r300_vertex_job_from_spirv(r3v_reference_vertex_spirv,
                                      WORDS(r3v_reference_vertex_spirv),
                                      "mai", &job, &reason));
   assert(strcmp(reason, "entry point name outside the request") == 0);
   assert(!r300_vertex_job_from_spirv(r3v_reference_vertex_spirv,
                                      WORDS(r3v_reference_vertex_spirv),
                                      NULL, &job, &reason));
   uint32_t color[4];
   assert(!r300_fragment_constant_color_from_spirv(
      r3v_reference_fragment_spirv, WORDS(r3v_reference_fragment_spirv),
      "other", color, &reason));
   assert(strcmp(reason, "entry point name outside the request") == 0);
}

/* A one-word final instruction (OpTypeVoid with length 1 appended after
 * OpFunctionEnd) refuses on its length before any operand is read, so
 * the reader's last access stays inside the module.
 */
static void test_short_final_instruction(void)
{
   uint32_t words[WORDS(r3v_reference_vertex_spirv) + 1];
   memcpy(words, r3v_reference_vertex_spirv,
          sizeof(r3v_reference_vertex_spirv));
   words[WORDS(r3v_reference_vertex_spirv)] = (1u << 16) | 19u;
   struct r300_vertex_job job;
   const char *reason = NULL;
   assert(!r300_vertex_job_from_spirv(words, WORDS(words), "main", &job,
                                      &reason));
   assert(strcmp(reason, "instruction after the final output store") == 0);
}

/* Work after the final output store: the body's first OpLoad repeated
 * after the OpStore refuses as an instruction after the store rather
 * than lowering to a job that discards it.
 */
static void test_instruction_after_store(void)
{
   enum { OP_LOAD = 61, OP_STORE = 62 };
   const size_t n = WORDS(r3v_reference_vertex_spirv);
   size_t load_at = 0, load_len = 0, store_end = 0;
   for (size_t at = 5; at < n;) {
      const uint32_t len = r3v_reference_vertex_spirv[at] >> 16;
      const uint32_t op = r3v_reference_vertex_spirv[at] & 0xffffu;
      assert(len != 0 && at + len <= n);
      if (op == OP_LOAD && load_at == 0) {
         load_at = at;
         load_len = len;
      }
      if (op == OP_STORE)
         store_end = at + len;
      at += len;
   }
   assert(load_at != 0 && store_end != 0 && store_end < n);
   uint32_t words[WORDS(r3v_reference_vertex_spirv) + 8];
   assert(load_len <= 8);
   memcpy(words, r3v_reference_vertex_spirv, store_end * 4);
   memcpy(words + store_end, r3v_reference_vertex_spirv + load_at,
          load_len * 4);
   memcpy(words + store_end + load_len, r3v_reference_vertex_spirv + store_end,
          (n - store_end) * 4);
   struct r300_vertex_job job;
   const char *reason = NULL;
   assert(!r300_vertex_job_from_spirv(words, n + load_len, "main", &job,
                                      &reason));
   assert(strcmp(reason, "instruction after the final output store") == 0);
}

/* Single-word mutation sweep: every one-word XOR of the identity
 * module either refuses or still lowers to the identity job shape, so
 * no mutation reaches execution with different semantics unnoticed.
 */
static void test_mutation_sweep(void)
{
   uint32_t mutated[WORDS(r3v_reference_vertex_spirv)];
   struct r300_vertex_job job;
   const char *reason = NULL;
   for (size_t word = 0; word < WORDS(mutated); word++) {
      for (uint32_t bit = 0; bit < 32; bit += 7) {
         memcpy(mutated, r3v_reference_vertex_spirv, sizeof(mutated));
         mutated[word] ^= 1u << bit;
         if (!r300_vertex_job_from_spirv(mutated, WORDS(mutated), "main", &job,
                                         &reason))
            continue;
         assert(job.instruction_count == 2);
         assert(job.instructions[0].opcode ==
                R300_VERTEX_JOB_OP_LOAD_INPUT);
         assert(job.instructions[1].opcode ==
                R300_VERTEX_JOB_OP_STORE_POSITION);
      }
   }
}

int main(void)
{
   test_reference_vertex_module();
   test_reference_fragment_module();
   test_reference_sampled_module();
   test_reference_arith_module();
   test_reference_varying_modules();
   test_varying_module_known_bads();
   test_reference_two_attribute_module();
   test_reference_instance_modules();
   test_module_refusals();
   test_malformed_streams();
   test_entry_name_binding();
   test_short_final_instruction();
   test_instruction_after_store();
   test_mutation_sweep();
   printf("r3v_native_pipeline_frontend_test: all cases pass\n");
   return 0;
}
