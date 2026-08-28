/*
 * SPDX-License-Identifier: MIT
 *
 * Direct SPIR-V admission for the vertex-job IR.
 */

#include "r3v_vertex_spirv.h"

#include <stdlib.h>
#include <string.h>

/* The opcode and enumerant values below are the SPIR-V specification's
 * numeric assignments (Khronos SPIR-V, section 3: binary form); the
 * reader carries its own table so admission depends on no external
 * header.
 */
enum {
   SPV_MAGIC = 0x07230203u,

   R3V_VERTEX_SPIRV_ID_BOUND_MAX = 4096,

   OP_SOURCE = 3,
   OP_SOURCE_EXTENSION = 4,
   OP_NAME = 5,
   OP_MEMBER_NAME = 6,
   OP_STRING = 7,
   OP_LINE = 8,
   OP_EXTENSION = 10,
   OP_EXT_INST_IMPORT = 11,
   OP_EXT_INST = 12,
   OP_MEMORY_MODEL = 14,
   OP_ENTRY_POINT = 15,
   OP_EXECUTION_MODE = 16,
   OP_CAPABILITY = 17,
   OP_TYPE_VOID = 19,
   OP_TYPE_INT = 21,
   OP_TYPE_FLOAT = 22,
   OP_TYPE_VECTOR = 23,
   OP_TYPE_IMAGE = 25,
   OP_TYPE_SAMPLED_IMAGE = 27,
   OP_TYPE_ARRAY = 28,
   OP_TYPE_STRUCT = 30,
   OP_TYPE_POINTER = 32,
   OP_TYPE_FUNCTION = 33,
   OP_CONSTANT = 43,
   OP_CONSTANT_COMPOSITE = 44,
   OP_FUNCTION = 54,
   OP_FUNCTION_END = 56,
   OP_VARIABLE = 59,
   OP_LOAD = 61,
   OP_STORE = 62,
   OP_ACCESS_CHAIN = 65,
   OP_IN_BOUNDS_ACCESS_CHAIN = 66,
   OP_DECORATE = 71,
   OP_MEMBER_DECORATE = 72,
   OP_VECTOR_SHUFFLE = 79,
   OP_COMPOSITE_CONSTRUCT = 80,
   OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
   OP_CONVERT_S_TO_F = 111,
   OP_F_ADD = 129,
   OP_F_MUL = 133,
   OP_DOT = 148,
   OP_LABEL = 248,
   OP_RETURN = 253,
   OP_NO_LINE = 317,
   OP_MODULE_PROCESSED = 330,

   GLSL_STD_450_FMA = 50,

   CAP_SHADER = 1,
   EXEC_MODEL_VERTEX = 0,
   EXEC_MODEL_FRAGMENT = 4,
   EXEC_MODE_ORIGIN_UPPER_LEFT = 7,
   ADDRESSING_LOGICAL = 0,

   SC_UNIFORM_CONSTANT = 0,
   SC_INPUT = 1,
   SC_OUTPUT = 3,
   SC_FUNCTION = 7,

   DECOR_RELAXED_PRECISION = 0,
   DECOR_BLOCK = 2,
   DECOR_BUILTIN = 11,
   DECOR_LOCATION = 30,
   DECOR_BINDING = 33,
   DECOR_DESCRIPTOR_SET = 34,

   IMAGE_DIM_2D = 1,
   IMAGE_FORMAT_UNKNOWN = 0,

   BUILTIN_POSITION = 0,
   BUILTIN_POINT_SIZE = 1,
   BUILTIN_CLIP_DISTANCE = 3,
   BUILTIN_CULL_DISTANCE = 4,
   BUILTIN_VERTEX_INDEX = 42,
   BUILTIN_INSTANCE_INDEX = 43,
};

/* Per-result-id record: the module walk classifies every id once, and
 * the body walk reads the classifications back.  The kinds cover
 * exactly the admitted grammar.
 */
enum id_kind {
   ID_UNSET = 0,
   ID_TYPE_VOID,
   ID_TYPE_FLOAT32,
   ID_TYPE_VEC4,
   ID_TYPE_INT32,
   /* Fixed-length float arrays: legal only as gl_PerVertex tail
    * members, so the kind carries no structure. */
   ID_TYPE_FLOAT_ARRAY,
   ID_TYPE_STRUCT_PERVERTEX,
   ID_TYPE_POINTER,
   ID_TYPE_FUNCTION,
   ID_CONST_FLOAT,
   ID_CONST_INT,
   ID_CONST_VEC4,
   ID_EXT_IMPORT_GLSL,
   /* A vertex module's located vec4 input: the attribute slot the
    * location names rides in a. */
   ID_VAR_INPUT_ATTRIBUTE,
   ID_VAR_OUTPUT_PERVERTEX,
   ID_VAR_OUTPUT_POS_DIRECT,
   /* The vertex module's location-0 vec4 output, the one varying the
    * carrier record carries beside the position. */
   ID_VAR_OUTPUT_VARYING,
   ID_VAR_OUTPUT_COLOR,
   /* The fragment module's location-0 vec4 input, the interpolated
    * varying. */
   ID_VAR_INPUT_VARYING,
   ID_VAR_FUNCTION,
   ID_FUNCTION,
   ID_LABEL,
   /* Body values. */
   ID_PTR_POSITION,
   ID_VAL_VEC4,
   ID_VAL_SCALAR_BROADCAST,
   /* The fragment module's loaded varying: the only value the
    * pass-through program stores. */
   ID_VAL_VARYING,
   /* A vertex input variable carrying the VertexIndex or InstanceIndex
    * builtin as a 32-bit int (a = enum r300_vertex_job_system_value),
    * and the int broadcast its load leaves in a job temp (a), which
    * reaches the vec4 straight line only through OpConvertSToF. */
   ID_VAR_INPUT_SYSTEM_VALUE,
   ID_VAL_INT_BROADCAST,
   /* The sampled fragment shape's ids: the 2D sampled-image types, the
    * two-component float vector the coordinate shuffle produces, the
    * set-0 binding-0 combined image sampler variable, and the loaded
    * image, coordinate, and sampled texel values.
    */
   ID_TYPE_IMAGE_2D,
   ID_TYPE_SAMPLED_IMAGE,
   ID_TYPE_VEC2,
   ID_VAR_SAMPLER,
   ID_VAL_SAMPLED_IMAGE,
   ID_VAL_COORD_VEC2,
   ID_VAL_SAMPLED_TEXEL,
};

/* The fragment program shapes the two fragment admitters accept: one
 * constant color, or the location-0 varying passed through unchanged.
 * The vertex admitter carries neither.
 */
enum fragment_shape {
   FRAGMENT_SHAPE_NONE = 0,
   FRAGMENT_SHAPE_CONSTANT_COLOR,
   FRAGMENT_SHAPE_VARYING_PASSTHROUGH,
   /* The location-0 varying's xy sampling the set-0 binding-0 combined
    * image sampler, stored to the location-0 output. */
   FRAGMENT_SHAPE_SAMPLED_TEXTURE,
};

struct id_info {
   uint8_t kind;
   /* ID_TYPE_POINTER: storage class (a), pointee type id (b).
    * ID_CONST_FLOAT / ID_CONST_INT: the 32-bit pattern or value (a).
    * ID_CONST_VEC4: lane patterns (vec), materialized flag (b), temp
    * (c).  ID_VAL_VEC4 / ID_VAL_SCALAR_BROADCAST: the job temp (a).
    * ID_VAR_FUNCTION: the value id the last recognized store left in
    * the variable (b).
    */
   uint32_t a;
   uint32_t b;
   uint32_t c;
   uint32_t vec[4];
   /* Decorations, collected before definitions are read. */
   bool has_location, has_builtin, has_m0_builtin;
   bool has_binding, has_descriptor_set;
   uint32_t location, builtin, m0_builtin;
   uint32_t binding, descriptor_set;
};

struct reader {
   const uint32_t *words;
   size_t count;
   struct id_info *ids;
   uint32_t bound;
   struct r300_vertex_job *job;
   uint32_t next_temp;
   const char **reason;
   const char *entry_name;
};

static bool refuse(struct reader *r, const char *why)
{
   *r->reason = why;
   return false;
}

static struct id_info *info(struct reader *r, uint32_t id)
{
   return id != 0 && id < r->bound ? &r->ids[id] : NULL;
}

/* A fresh result id: inside the bound and not yet defined, so a module
 * redefining an id refuses instead of overwriting the classification.
 */
static struct id_info *define(struct reader *r, uint32_t id)
{
   struct id_info *entry = info(r, id);
   if (entry == NULL || entry->kind != ID_UNSET)
      return NULL;
   return entry;
}

static bool id_is(struct reader *r, uint32_t id, enum id_kind kind)
{
   const struct id_info *entry = info(r, id);
   return entry != NULL && entry->kind == kind;
}

static bool const_int_is(struct reader *r, uint32_t id, uint32_t value)
{
   const struct id_info *entry = info(r, id);
   return entry != NULL && entry->kind == ID_CONST_INT &&
          entry->a == value;
}

static bool emit(struct reader *r, uint8_t opcode, uint8_t dst,
                 uint8_t src0, uint8_t src1, uint8_t src2)
{
   struct r300_vertex_job *job = r->job;
   if (job->instruction_count >= R300_VERTEX_JOB_MAX_INSTRUCTIONS)
      return refuse(r, "vertex program exceeds the instruction budget");
   job->instructions[job->instruction_count++] =
      (struct r300_vertex_job_instruction){ opcode, dst, src0, src1, src2 };
   return true;
}

static bool new_temp(struct reader *r, uint8_t *temp)
{
   if (r->next_temp >= R300_VERTEX_JOB_MAX_TEMPS)
      return refuse(r, "vertex program exceeds the 16-temp file");
   *temp = (uint8_t)r->next_temp++;
   return true;
}

/* Resolves a value id to the temp that carries it as a vec4, emitting
 * the deduplicated LOAD_CONSTANT the first time a composite constant
 * is consumed.  A Dot result reaches an operand only through its own
 * four-way replicate, so a broadcast scalar refuses here.
 */
static bool value_temp(struct reader *r, uint32_t id, uint8_t *temp)
{
   struct id_info *entry = info(r, id);
   if (entry == NULL)
      return refuse(r, "operand outside the bound");
   switch (entry->kind) {
   case ID_VAL_VEC4:
      *temp = (uint8_t)entry->a;
      return true;
   case ID_CONST_VEC4: {
      if (entry->b != 0) {
         *temp = (uint8_t)entry->c;
         return true;
      }
      struct r300_vertex_job *job = r->job;
      uint32_t slot = job->constant_count;
      for (uint32_t c = 0; c < job->constant_count; c++) {
         if (memcmp(job->constants[c], entry->vec,
                    sizeof(entry->vec)) == 0) {
            slot = c;
            break;
         }
      }
      if (slot == job->constant_count) {
         if (slot >= R300_VERTEX_JOB_MAX_CONSTANTS)
            return refuse(r, "vertex program exceeds the constant file");
         memcpy(job->constants[slot], entry->vec, sizeof(entry->vec));
         job->constant_count++;
      }
      uint8_t dst;
      if (!new_temp(r, &dst))
         return false;
      if (!emit(r, R300_VERTEX_JOB_OP_LOAD_CONSTANT, dst, (uint8_t)slot,
                0, 0))
         return false;
      entry->b = 1;
      entry->c = dst;
      *temp = dst;
      return true;
   }
   default:
      return refuse(r, "operand outside the admitted straight line");
   }
}

/* One admitted gl_PerVertex shape: member 0 is the vec4 Position
 * builtin, and any tail members are the float or float-array builtins
 * a GLSL declaration carries along.
 */
static bool struct_members_admit_pervertex(struct reader *r,
                                           const uint32_t *w, uint32_t len)
{
   if (len < 3 || !id_is(r, w[2], ID_TYPE_VEC4))
      return false;
   for (uint32_t m = 3; m < len; m++) {
      if (!id_is(r, w[m], ID_TYPE_FLOAT32) &&
          !id_is(r, w[m], ID_TYPE_FLOAT_ARRAY))
         return false;
   }
   return true;
}

static bool
admit_module(const uint32_t *words, size_t word_count,
             struct id_info *ids, uint32_t bound, uint32_t exec_model,
             enum fragment_shape shape, const char *entry_name,
             struct r300_vertex_job *job, uint32_t color_bits[4],
             const char **reason)
{
   struct reader reader = {
      .words = words,
      .count = word_count,
      .ids = ids,
      .bound = bound,
      .job = job,
      .reason = reason,
      .entry_name = entry_name,
   };
   struct reader *r = &reader;
   const bool fragment = exec_model == EXEC_MODEL_FRAGMENT;

   bool capability_shader = false;
   bool memory_model_logical = false;
   uint32_t entry_point = 0;
   /* The attribute slots the vertex module declares, one bit per
    * location. */
   uint32_t input_mask = 0;
   uint32_t output_var = 0;
   /* The vertex varying output and the fragment varying input; a
    * module declares each at most once. */
   uint32_t varying_var = 0;

   /* Body state: the entry function's straight-line walk.  The
    * position store is recorded and emitted after the walk, so the job
    * keeps STORE_POSITION as its final instruction whatever order the
    * module stores its outputs in; the varying store emits in place. */
   bool in_function = false;
   bool function_seen = false;
   bool label_seen = false;
   bool returned = false;
   bool stored = false;
   bool varying_stored = false;
   uint8_t position_src = 0;

   size_t at = 5;
   while (at < word_count) {
      const uint32_t first = words[at];
      const uint32_t opcode = first & 0xffffu;
      const uint32_t len = first >> 16;
      if (len == 0 || at + len > word_count) {
         *reason = "instruction overruns the module";
         return false;
      }
      const uint32_t *w = &words[at];
      at += len;

      /* The final output store ends the admitted body: only the
       * return, the function end, and line markers follow it, so a
       * module carrying work after its last store refuses rather than
       * lowering to a job that discards it.  A vertex module that
       * declares a varying has two output stores, and the later of the
       * two is the final one. */
      const bool final_stored =
         stored && (fragment || varying_var == 0 || varying_stored);
      if (final_stored && opcode != OP_RETURN && opcode != OP_FUNCTION_END &&
          opcode != OP_LINE && opcode != OP_NO_LINE)
         return refuse(r, "instruction after the final output store");

      switch (opcode) {
      case OP_SOURCE:
      case OP_SOURCE_EXTENSION:
      case OP_NAME:
      case OP_MEMBER_NAME:
      case OP_STRING:
      case OP_LINE:
      case OP_NO_LINE:
      case OP_MODULE_PROCESSED:
         break;

      case OP_CAPABILITY:
         if (len != 2 || w[1] != CAP_SHADER)
            return refuse(r, "capability outside Shader");
         capability_shader = true;
         break;

      case OP_EXTENSION:
         return refuse(r, "SPIR-V extension outside the admitted grammar");

      case OP_EXT_INST_IMPORT: {
         if (len < 3)
            return refuse(r, "malformed extended-instruction import");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed extended-instruction import");
         /* The GLSL.std.450 set alone supplies an admitted instruction
          * (Fma); any other import still refuses at its use.
          */
         static const char glsl_450[] = "GLSL.std.450";
         const size_t name_words = len - 2;
         if (name_words != sizeof(glsl_450) / 4 + 1 ||
             memcmp(&w[2], glsl_450, sizeof(glsl_450)) != 0)
            return refuse(r,
                          "extended-instruction import outside "
                          "GLSL.std.450");
         entry->kind = ID_EXT_IMPORT_GLSL;
         break;
      }

      case OP_MEMORY_MODEL:
         if (len != 3 || w[1] != ADDRESSING_LOGICAL)
            return refuse(r, "memory model outside Logical addressing");
         memory_model_logical = true;
         break;

      case OP_ENTRY_POINT:
         if (len < 3 || w[1] != exec_model)
            return refuse(r, "entry point outside the requested model");
         if (entry_point != 0)
            return refuse(r, "more than one entry point");
         {
            /* The literal name occupies whole words from w[3] and ends
             * inside the instruction; it binds to the requested entry
             * point byte for byte, so a module whose entry point carries
             * another name refuses before its body is read. */
            const size_t name_words = len - 3;
            const size_t name_bytes = name_words * sizeof(uint32_t);
            const size_t want = strlen(r->entry_name);
            if (name_words == 0 || want + 1 > name_bytes ||
                memcmp(&w[3], r->entry_name, want + 1) != 0)
               return refuse(r, "entry point name outside the request");
         }
         entry_point = w[2];
         break;

      case OP_EXECUTION_MODE:
         if (fragment && len == 3 && w[1] == entry_point &&
             entry_point != 0 && w[2] == EXEC_MODE_ORIGIN_UPPER_LEFT)
            break;
         return refuse(r, "execution mode outside the admitted grammar");

      case OP_DECORATE: {
         if (len < 3)
            return refuse(r, "malformed decoration");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "decoration names an id outside the bound");
         switch (w[2]) {
         case DECOR_LOCATION:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_location = true;
            entry->location = w[3];
            break;
         case DECOR_BUILTIN:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_builtin = true;
            entry->builtin = w[3];
            break;
         case DECOR_BINDING:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_binding = true;
            entry->binding = w[3];
            break;
         case DECOR_DESCRIPTOR_SET:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_descriptor_set = true;
            entry->descriptor_set = w[3];
            break;
         case DECOR_BLOCK:
         case DECOR_RELAXED_PRECISION:
            break;
         default:
            return refuse(r, "decoration outside the admitted grammar");
         }
         break;
      }

      case OP_MEMBER_DECORATE: {
         if (len < 4)
            return refuse(r, "malformed member decoration");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "decoration names an id outside the bound");
         if (w[3] != DECOR_BUILTIN || len != 5)
            return refuse(r,
                          "member decoration outside the builtin block");
         if (w[2] == 0) {
            entry->has_m0_builtin = true;
            entry->m0_builtin = w[4];
         } else if (w[4] != BUILTIN_POINT_SIZE &&
                    w[4] != BUILTIN_CLIP_DISTANCE &&
                    w[4] != BUILTIN_CULL_DISTANCE) {
            return refuse(r,
                          "member builtin outside the gl_PerVertex tail");
         }
         break;
      }

      case OP_TYPE_VOID: {
         if (len != 2)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         entry->kind = ID_TYPE_VOID;
         break;
      }

      case OP_TYPE_FLOAT: {
         if (len != 3)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (w[2] != 32)
            return refuse(r, "float width outside 32 bits");
         entry->kind = ID_TYPE_FLOAT32;
         break;
      }

      case OP_TYPE_INT: {
         if (len != 4)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (w[2] != 32)
            return refuse(r, "integer width outside 32 bits");
         entry->kind = ID_TYPE_INT32;
         break;
      }

      case OP_TYPE_VECTOR: {
         if (len != 4)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (!id_is(r, w[2], ID_TYPE_FLOAT32) ||
             (w[3] != 4 && w[3] != 2))
            return refuse(r, "vector type outside float vec4 or vec2");
         entry->kind = w[3] == 4 ? ID_TYPE_VEC4 : ID_TYPE_VEC2;
         break;
      }

      case OP_TYPE_IMAGE: {
         /* The one admitted image type is the sampled 2D single-sample
          * float image with no declared format, the type the canonical
          * combined-image-sampler declaration produces. */
         if (len != 9)
            return refuse(r, "malformed image type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed image type");
         if (shape != FRAGMENT_SHAPE_SAMPLED_TEXTURE ||
             !id_is(r, w[2], ID_TYPE_FLOAT32) || w[3] != IMAGE_DIM_2D ||
             w[4] != 0 || w[5] != 0 || w[6] != 0 || w[7] != 1 ||
             w[8] != IMAGE_FORMAT_UNKNOWN)
            return refuse(r, "image type outside the sampled 2D float");
         entry->kind = ID_TYPE_IMAGE_2D;
         break;
      }

      case OP_TYPE_SAMPLED_IMAGE: {
         if (len != 3)
            return refuse(r, "malformed sampled-image type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed sampled-image type");
         if (!id_is(r, w[2], ID_TYPE_IMAGE_2D))
            return refuse(r,
                          "sampled-image type outside the 2D float image");
         entry->kind = ID_TYPE_SAMPLED_IMAGE;
         break;
      }

      case OP_TYPE_ARRAY: {
         if (len != 4)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (!id_is(r, w[2], ID_TYPE_FLOAT32) ||
             !id_is(r, w[3], ID_CONST_INT))
            return refuse(r,
                          "array type outside the gl_PerVertex tail");
         entry->kind = ID_TYPE_FLOAT_ARRAY;
         break;
      }

      case OP_TYPE_STRUCT: {
         /* Decorations landed on this id before the definition. */
         if (len < 3)
            return refuse(r, "malformed type");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (entry->kind != ID_UNSET)
            return refuse(r, "result id defined twice");
         if (!entry->has_m0_builtin ||
             entry->m0_builtin != BUILTIN_POSITION ||
             !struct_members_admit_pervertex(r, w, len))
            return refuse(r, "struct type outside gl_PerVertex");
         entry->kind = ID_TYPE_STRUCT_PERVERTEX;
         break;
      }

      case OP_TYPE_POINTER: {
         if (len != 4)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         entry->kind = ID_TYPE_POINTER;
         entry->a = w[2];
         entry->b = w[3];
         break;
      }

      case OP_TYPE_FUNCTION: {
         if (len < 3)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (len != 3 || !id_is(r, w[2], ID_TYPE_VOID))
            return refuse(r, "function type outside void()");
         entry->kind = ID_TYPE_FUNCTION;
         break;
      }

      case OP_CONSTANT: {
         if (len != 4)
            return refuse(r, "malformed constant");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed constant");
         if (id_is(r, w[1], ID_TYPE_FLOAT32))
            entry->kind = ID_CONST_FLOAT;
         else if (id_is(r, w[1], ID_TYPE_INT32))
            entry->kind = ID_CONST_INT;
         else
            return refuse(r, "constant type outside 32-bit scalars");
         entry->a = w[3];
         break;
      }

      case OP_CONSTANT_COMPOSITE: {
         if (len != 7)
            return refuse(r, "malformed composite constant");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed composite constant");
         if (!id_is(r, w[1], ID_TYPE_VEC4))
            return refuse(r, "composite constant outside vec4 float");
         for (uint32_t c = 0; c < 4; c++) {
            const struct id_info *lane = info(r, w[3 + c]);
            if (lane == NULL || lane->kind != ID_CONST_FLOAT)
               return refuse(r,
                             "composite constant outside literal floats");
            entry->vec[c] = lane->a;
         }
         entry->kind = ID_CONST_VEC4;
         break;
      }

      case OP_VARIABLE: {
         if (len != 4)
            return refuse(r, "malformed variable");
         struct id_info *entry = info(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed variable");
         if (entry->kind != ID_UNSET)
            return refuse(r, "result id defined twice");
         const uint32_t storage_class = w[3];
         const struct id_info *ptr = info(r, w[1]);
         if (ptr == NULL || ptr->kind != ID_TYPE_POINTER ||
             ptr->a != storage_class)
            return refuse(r, "variable type outside a matching pointer");
         switch (storage_class) {
         case SC_INPUT:
            if (fragment && shape == FRAGMENT_SHAPE_CONSTANT_COLOR)
               return refuse(r, "fragment shader reads an input");
            if (fragment) {
               if (!id_is(r, ptr->b, ID_TYPE_VEC4) ||
                   !entry->has_location || entry->location != 0)
                  return refuse(r,
                                "fragment input outside varying 0 as vec4");
               if (varying_var != 0)
                  return refuse(r, "more than one fragment input");
               entry->kind = ID_VAR_INPUT_VARYING;
               varying_var = w[2];
               break;
            }
            /* The two draw system values arrive as builtin-decorated
             * 32-bit int inputs; every other vertex input is a located
             * vec4 attribute. */
            if (id_is(r, ptr->b, ID_TYPE_INT32)) {
               if (!entry->has_builtin ||
                   (entry->builtin != BUILTIN_VERTEX_INDEX &&
                    entry->builtin != BUILTIN_INSTANCE_INDEX))
                  return refuse(r,
                                "int vertex input outside the VertexIndex "
                                "and InstanceIndex builtins");
               entry->kind = ID_VAR_INPUT_SYSTEM_VALUE;
               entry->a = entry->builtin == BUILTIN_VERTEX_INDEX
                             ? R300_VERTEX_JOB_SV_VERTEX_INDEX
                             : R300_VERTEX_JOB_SV_INSTANCE_INDEX;
               break;
            }
            if (!id_is(r, ptr->b, ID_TYPE_VEC4) || !entry->has_location)
               return refuse(r, "vertex input outside a located vec4");
            /* One attribute slot per location, inside the slot count
             * the job IR and the published maxVertexInputAttributes
             * share. */
            if (entry->location >= R300_VERTEX_JOB_MAX_INPUTS)
               return refuse(r,
                             "vertex input location beyond the attribute "
                             "slots");
            if (input_mask & (1u << entry->location))
               return refuse(r, "two vertex inputs at one location");
            entry->kind = ID_VAR_INPUT_ATTRIBUTE;
            entry->a = entry->location;
            input_mask |= 1u << entry->location;
            break;
         case SC_OUTPUT:
            if (!fragment && id_is(r, ptr->b, ID_TYPE_VEC4) &&
                !entry->has_builtin && entry->has_location) {
               /* The one varying: location 0, vec4, beside the
                * position output. */
               if (entry->location != 0)
                  return refuse(r, "vertex varying outside location 0");
               if (varying_var != 0)
                  return refuse(r, "more than one vertex varying output");
               entry->kind = ID_VAR_OUTPUT_VARYING;
               varying_var = w[2];
               break;
            }
            if (output_var != 0)
               return refuse(r, "more than one shader output");
            if (fragment) {
               if (!id_is(r, ptr->b, ID_TYPE_VEC4) ||
                   !entry->has_location || entry->location != 0)
                  return refuse(r,
                                "fragment output outside color 0 as vec4");
               entry->kind = ID_VAR_OUTPUT_COLOR;
            } else if (id_is(r, ptr->b, ID_TYPE_STRUCT_PERVERTEX)) {
               entry->kind = ID_VAR_OUTPUT_PERVERTEX;
            } else if (id_is(r, ptr->b, ID_TYPE_VEC4) &&
                       entry->has_builtin &&
                       entry->builtin == BUILTIN_POSITION) {
               entry->kind = ID_VAR_OUTPUT_POS_DIRECT;
            } else {
               return refuse(r,
                             "vertex output outside the Position builtin");
            }
            output_var = w[2];
            break;
         case SC_UNIFORM_CONSTANT:
            /* The sampled shape's one descriptor: the combined image
             * sampler at set 0 binding 0, the binding the pipeline
             * layout and the descriptor write name. */
            if (shape != FRAGMENT_SHAPE_SAMPLED_TEXTURE ||
                !id_is(r, ptr->b, ID_TYPE_SAMPLED_IMAGE) ||
                !entry->has_binding || entry->binding != 0 ||
                !entry->has_descriptor_set || entry->descriptor_set != 0)
               return refuse(r,
                             "uniform constant outside the set-0 "
                             "binding-0 combined image sampler");
            for (uint32_t s = 1; s < bound; s++) {
               if (ids[s].kind == ID_VAR_SAMPLER)
                  return refuse(r, "more than one sampler variable");
            }
            entry->kind = ID_VAR_SAMPLER;
            break;
         case SC_FUNCTION:
            if (!in_function)
               return refuse(r, "function variable outside a function");
            if (!id_is(r, ptr->b, ID_TYPE_VEC4) &&
                !id_is(r, ptr->b, ID_TYPE_FLOAT32))
               return refuse(r,
                             "function variable outside the 32-bit float "
                             "model");
            entry->kind = ID_VAR_FUNCTION;
            entry->b = 0; /* holds no value yet */
            break;
         default:
            return refuse(r, "storage class outside the admitted grammar");
         }
         break;
      }

      case OP_FUNCTION: {
         if (len != 5)
            return refuse(r, "malformed function");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed function");
         if (function_seen)
            return refuse(r, "more than one function");
         if (w[2] != entry_point)
            return refuse(r, "function outside the entry point");
         entry->kind = ID_FUNCTION;
         function_seen = true;
         in_function = true;
         break;
      }

      case OP_LABEL: {
         if (len != 2 || !in_function)
            return refuse(r, "malformed label");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed label");
         if (label_seen)
            return refuse(r, "control flow outside straight-line code");
         entry->kind = ID_LABEL;
         label_seen = true;
         break;
      }

      case OP_ACCESS_CHAIN:
      case OP_IN_BOUNDS_ACCESS_CHAIN: {
         if (len != 5 || !in_function || returned)
            return refuse(r, "malformed access chain");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed access chain");
         const struct id_info *base = info(r, w[3]);
         if (base == NULL || base->kind != ID_VAR_OUTPUT_PERVERTEX ||
             !const_int_is(r, w[4], 0))
            return refuse(r,
                          "access chain outside the Position member");
         entry->kind = ID_PTR_POSITION;
         break;
      }

      case OP_LOAD: {
         if ((len != 4 && len != 5) || !in_function || returned)
            return refuse(r, "malformed load");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed load");
         if (len == 5 && w[4] != 0)
            return refuse(r, "memory operands outside the grammar");
         const struct id_info *ptr = info(r, w[3]);
         if (ptr == NULL)
            return refuse(r, "load outside the bound");
         if (ptr->kind == ID_VAR_INPUT_ATTRIBUTE) {
            if (!id_is(r, w[1], ID_TYPE_VEC4))
               return refuse(r, "load outside one vec4");
            uint8_t dst;
            if (!new_temp(r, &dst) ||
                !emit(r, R300_VERTEX_JOB_OP_LOAD_INPUT, dst,
                      (uint8_t)ptr->a, 0, 0))
               return false;
            entry->kind = ID_VAL_VEC4;
            entry->a = dst;
         } else if (ptr->kind == ID_VAR_INPUT_SYSTEM_VALUE) {
            if (!id_is(r, w[1], ID_TYPE_INT32))
               return refuse(r, "system value loaded outside its int type");
            uint8_t dst;
            if (!new_temp(r, &dst) ||
                !emit(r, R300_VERTEX_JOB_OP_LOAD_SYSTEM_VALUE, dst,
                      (uint8_t)ptr->a, 0, 0))
               return false;
            entry->kind = ID_VAL_INT_BROADCAST;
            entry->a = dst;
         } else if (ptr->kind == ID_VAR_INPUT_VARYING) {
            if (!id_is(r, w[1], ID_TYPE_VEC4))
               return refuse(r, "load outside one vec4");
            entry->kind = ID_VAL_VARYING;
         } else if (ptr->kind == ID_VAR_SAMPLER) {
            if (!id_is(r, w[1], ID_TYPE_SAMPLED_IMAGE))
               return refuse(r,
                             "sampler loaded outside its sampled-image "
                             "type");
            entry->kind = ID_VAL_SAMPLED_IMAGE;
         } else if (ptr->kind == ID_VAR_FUNCTION) {
            /* The variable forwards the value it holds. */
            const struct id_info *held = info(r, ptr->b);
            if (held == NULL ||
                (held->kind != ID_VAL_VEC4 &&
                 held->kind != ID_CONST_VEC4 &&
                 held->kind != ID_VAL_SCALAR_BROADCAST))
               return refuse(r,
                             "function variable read before a "
                             "recognized write");
            *entry = *held;
         } else {
            return refuse(r, "load outside the admitted pointers");
         }
         break;
      }

      case OP_STORE: {
         if ((len != 3 && len != 4) || !in_function || returned)
            return refuse(r, "malformed store");
         if (len == 4 && w[3] != 0)
            return refuse(r, "memory operands outside the grammar");
         struct id_info *ptr = info(r, w[1]);
         const struct id_info *value = info(r, w[2]);
         if (ptr == NULL || value == NULL)
            return refuse(r, "store outside the bound");
         if (ptr->kind == ID_VAR_FUNCTION) {
            if (value->kind != ID_VAL_VEC4 &&
                value->kind != ID_CONST_VEC4 &&
                value->kind != ID_VAL_SCALAR_BROADCAST)
               return refuse(r,
                             "function variable holds a value outside "
                             "the vec4 straight line");
            ptr->b = w[2];
         } else if (!fragment && (ptr->kind == ID_PTR_POSITION ||
                                  ptr->kind == ID_VAR_OUTPUT_POS_DIRECT)) {
            if (stored)
               return refuse(r, "second position store");
            if (!value_temp(r, w[2], &position_src))
               return false;
            stored = true;
         } else if (!fragment && ptr->kind == ID_VAR_OUTPUT_VARYING) {
            if (varying_stored)
               return refuse(r, "second varying store");
            uint8_t src;
            if (!value_temp(r, w[2], &src))
               return false;
            if (!emit(r, R300_VERTEX_JOB_OP_STORE_VARYING, 0, src, 0, 0))
               return false;
            varying_stored = true;
         } else if (fragment && ptr->kind == ID_VAR_OUTPUT_COLOR) {
            if (stored)
               return refuse(r, "second color store");
            if (shape == FRAGMENT_SHAPE_VARYING_PASSTHROUGH) {
               if (value->kind != ID_VAL_VARYING)
                  return refuse(r,
                                "fragment program outside the varying "
                                "pass-through");
            } else if (shape == FRAGMENT_SHAPE_SAMPLED_TEXTURE) {
               if (value->kind != ID_VAL_SAMPLED_TEXEL)
                  return refuse(r,
                                "fragment program outside the sampled "
                                "texel store");
            } else {
               if (value->kind != ID_CONST_VEC4)
                  return refuse(r,
                                "fragment program outside one constant "
                                "color");
               memcpy(color_bits, value->vec, 4 * sizeof(uint32_t));
            }
            stored = true;
         } else {
            return refuse(r, "store outside the admitted pointers");
         }
         break;
      }

      case OP_F_ADD:
      case OP_F_MUL: {
         if (len != 5 || !in_function || returned)
            return refuse(r, "malformed arithmetic");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed arithmetic");
         if (!id_is(r, w[1], ID_TYPE_VEC4))
            return refuse(r, "arithmetic width outside the vec4 model");
         uint8_t src0, src1, dst;
         if (!value_temp(r, w[3], &src0) || !value_temp(r, w[4], &src1) ||
             !new_temp(r, &dst))
            return false;
         if (!emit(r,
                   opcode == OP_F_ADD ? R300_VERTEX_JOB_OP_FADD
                                      : R300_VERTEX_JOB_OP_FMUL,
                   dst, src0, src1, 0))
            return false;
         entry->kind = ID_VAL_VEC4;
         entry->a = dst;
         break;
      }

      case OP_EXT_INST: {
         if (len != 8 || !in_function || returned)
            return refuse(r,
                          "extended instruction outside GLSL.std.450 Fma");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r,
                          "extended instruction outside GLSL.std.450 Fma");
         if (!id_is(r, w[3], ID_EXT_IMPORT_GLSL) ||
             w[4] != GLSL_STD_450_FMA || !id_is(r, w[1], ID_TYPE_VEC4))
            return refuse(r,
                          "extended instruction outside GLSL.std.450 Fma");
         uint8_t src0, src1, src2, dst;
         if (!value_temp(r, w[5], &src0) || !value_temp(r, w[6], &src1) ||
             !value_temp(r, w[7], &src2) || !new_temp(r, &dst))
            return false;
         /* Vulkan GLSL fma() carries fused semantics, so the selection
          * is FFMA's single rounding. */
         if (!emit(r, R300_VERTEX_JOB_OP_FFMA, dst, src0, src1, src2))
            return false;
         entry->kind = ID_VAL_VEC4;
         entry->a = dst;
         break;
      }

      case OP_DOT: {
         if (len != 5 || !in_function || returned)
            return refuse(r, "malformed arithmetic");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed arithmetic");
         if (!id_is(r, w[1], ID_TYPE_FLOAT32))
            return refuse(r, "dot result outside one 32-bit float");
         uint8_t src0, src1, dst;
         if (!value_temp(r, w[3], &src0) || !value_temp(r, w[4], &src1) ||
             !new_temp(r, &dst))
            return false;
         if (!emit(r, R300_VERTEX_JOB_OP_DP4, dst, src0, src1, 0))
            return false;
         entry->kind = ID_VAL_SCALAR_BROADCAST;
         entry->a = dst;
         break;
      }

      case OP_CONVERT_S_TO_F: {
         /* The loaded system value enters the float straight line here
          * alone: the int broadcast converts per lane, and the result
          * is a broadcast scalar the vec4 replicate below rejoins. */
         if (len != 4 || !in_function || returned)
            return refuse(r, "malformed conversion");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed conversion");
         const struct id_info *operand = info(r, w[3]);
         if (!id_is(r, w[1], ID_TYPE_FLOAT32) || operand == NULL ||
             operand->kind != ID_VAL_INT_BROADCAST)
            return refuse(r,
                          "conversion outside a loaded system value to "
                          "float");
         uint8_t dst;
         if (!new_temp(r, &dst) ||
             !emit(r, R300_VERTEX_JOB_OP_CONVERT_S_TO_F, dst,
                   (uint8_t)operand->a, 0, 0))
            return false;
         entry->kind = ID_VAL_SCALAR_BROADCAST;
         entry->a = dst;
         break;
      }

      case OP_VECTOR_SHUFFLE: {
         /* The one admitted shuffle takes the loaded varying's x and y
          * as the vec2 sampling coordinate. */
         if (len != 7 || !in_function || returned)
            return refuse(r, "malformed vector shuffle");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed vector shuffle");
         const struct id_info *src = info(r, w[3]);
         if (shape != FRAGMENT_SHAPE_SAMPLED_TEXTURE ||
             !id_is(r, w[1], ID_TYPE_VEC2) || src == NULL ||
             src->kind != ID_VAL_VARYING || w[4] != w[3] || w[5] != 0 ||
             w[6] != 1)
            return refuse(r,
                          "shuffle outside the varying's xy coordinate");
         entry->kind = ID_VAL_COORD_VEC2;
         break;
      }

      case OP_IMAGE_SAMPLE_IMPLICIT_LOD: {
         if (len != 5 || !in_function || returned)
            return refuse(r, "malformed image sample");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed image sample");
         const struct id_info *img = info(r, w[3]);
         const struct id_info *coord = info(r, w[4]);
         if (shape != FRAGMENT_SHAPE_SAMPLED_TEXTURE ||
             !id_is(r, w[1], ID_TYPE_VEC4) || img == NULL ||
             img->kind != ID_VAL_SAMPLED_IMAGE || coord == NULL ||
             coord->kind != ID_VAL_COORD_VEC2)
            return refuse(r,
                          "sample outside the loaded sampler at the "
                          "varying coordinate");
         entry->kind = ID_VAL_SAMPLED_TEXEL;
         break;
      }

      case OP_COMPOSITE_CONSTRUCT: {
         /* The one admitted vec4 construction is a broadcast scalar's
          * own replicate, which the DP4 temp already carries. */
         if (len != 7 || !in_function || returned)
            return refuse(r, "malformed composite construction");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed composite construction");
         const struct id_info *scalar = info(r, w[3]);
         if (!id_is(r, w[1], ID_TYPE_VEC4) || scalar == NULL ||
             scalar->kind != ID_VAL_SCALAR_BROADCAST || w[4] != w[3] ||
             w[5] != w[3] || w[6] != w[3])
            return refuse(r,
                          "vec4 construction outside a broadcast scalar");
         entry->kind = ID_VAL_VEC4;
         entry->a = scalar->a;
         break;
      }

      case OP_RETURN:
         if (len != 1 || !in_function || !label_seen)
            return refuse(r, "malformed return");
         returned = true;
         break;

      case OP_FUNCTION_END:
         if (len != 1 || !in_function || !returned)
            return refuse(r, "function ends before returning");
         in_function = false;
         break;

      default:
         return refuse(r, "opcode outside the vec4 straight line");
      }
   }

   if (!capability_shader || !memory_model_logical || entry_point == 0)
      return refuse(r, "module preamble outside the admitted grammar");
   if (in_function || !function_seen || !returned)
      return refuse(r, "entry function did not complete");
   if (!stored)
      return refuse(r, fragment ? "missing color-0 store"
                                : "missing position store");
   /* A declared varying the program never writes would reach the
    * interpolator undefined, so the shape refuses instead of lowering
    * to a record with an unwritten vector. */
   if (!fragment && varying_var != 0 && !varying_stored)
      return refuse(r, "vertex varying output left unwritten");
   if (!fragment &&
       !emit(r, R300_VERTEX_JOB_OP_STORE_POSITION, 0, position_src, 0, 0))
      return false;
   return true;
}

static bool
admit_words(const uint32_t *words, size_t word_count, uint32_t exec_model,
            enum fragment_shape shape, const char *entry_name,
            struct r300_vertex_job *job, uint32_t color_bits[4],
            const char **reason)
{
   *reason = "unrecognized module";
   if (words == NULL || word_count < 5 || entry_name == NULL)
      return false;
   if (words[0] != SPV_MAGIC) {
      *reason = "SPIR-V magic number absent";
      return false;
   }
   const uint32_t bound = words[3];
   if (bound == 0 || bound > R3V_VERTEX_SPIRV_ID_BOUND_MAX) {
      *reason = "result-id bound outside the admitted module size";
      return false;
   }
   /* The id table scales with the module's declared bound, so it lives
    * on the heap rather than growing the caller's stack by the bound
    * ceiling.
    */
   struct id_info *ids = calloc(bound, sizeof(*ids));
   if (ids == NULL) {
      *reason = "id-table allocation failed";
      return false;
   }
   const bool admitted = admit_module(words, word_count, ids, bound,
                                      exec_model, shape, entry_name, job,
                                      color_bits, reason);
   free(ids);
   return admitted;
}

bool r3v_vertex_job_from_spirv(const uint32_t *words, size_t word_count,
                                const char *entry_name,
                                struct r300_vertex_job *job,
                                const char **reason)
{
   memset(job, 0, sizeof(*job));
   uint32_t unused_color[4];
   if (!admit_words(words, word_count, EXEC_MODEL_VERTEX,
                    FRAGMENT_SHAPE_NONE, entry_name, job, unused_color,
                    reason))
      return false;
   /* The position store emits last by construction; the caller assigns
    * input_format_ids and validates the finished job. */
   return true;
}

bool r3v_fragment_constant_color_from_spirv(const uint32_t *words,
                                             size_t word_count,
                                             const char *entry_name,
                                             uint32_t color_bits[4],
                                             const char **reason)
{
   struct r300_vertex_job scratch;
   memset(&scratch, 0, sizeof(scratch));
   return admit_words(words, word_count, EXEC_MODEL_FRAGMENT,
                      FRAGMENT_SHAPE_CONSTANT_COLOR, entry_name, &scratch,
                      color_bits, reason);
}

bool r3v_fragment_varying_passthrough_from_spirv(const uint32_t *words,
                                                  size_t word_count,
                                                  const char *entry_name,
                                                  const char **reason)
{
   struct r300_vertex_job scratch;
   uint32_t unused_color[4];
   memset(&scratch, 0, sizeof(scratch));
   return admit_words(words, word_count, EXEC_MODEL_FRAGMENT,
                      FRAGMENT_SHAPE_VARYING_PASSTHROUGH, entry_name,
                      &scratch, unused_color, reason);
}

bool r3v_fragment_sampled_texture_from_spirv(const uint32_t *words,
                                              size_t word_count,
                                              const char *entry_name,
                                              const char **reason)
{
   struct r300_vertex_job scratch;
   uint32_t unused_color[4];
   memset(&scratch, 0, sizeof(scratch));
   return admit_words(words, word_count, EXEC_MODEL_FRAGMENT,
                      FRAGMENT_SHAPE_SAMPLED_TEXTURE, entry_name,
                      &scratch, unused_color, reason);
}
