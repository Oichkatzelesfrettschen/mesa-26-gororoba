/*
 * SPDX-License-Identifier: MIT
 *
 * Direct SPIR-V admission for the compute-job IR.
 */

#include "r3v_compute_spirv.h"

#include <stdlib.h>
#include <string.h>

/* The opcode and enumerant values below are the SPIR-V specification's
 * numeric assignments (Khronos SPIR-V, section 3: binary form); the
 * reader carries its own table so admission depends on no external
 * header.
 */
enum {
   SPV_MAGIC = 0x07230203u,

   R3V_COMPUTE_SPIRV_ID_BOUND_MAX = 4096,

   OP_SOURCE = 3,
   OP_SOURCE_EXTENSION = 4,
   OP_NAME = 5,
   OP_MEMBER_NAME = 6,
   OP_STRING = 7,
   OP_LINE = 8,
   OP_EXTENSION = 10,
   OP_EXT_INST_IMPORT = 11,
   OP_MEMORY_MODEL = 14,
   OP_ENTRY_POINT = 15,
   OP_EXECUTION_MODE = 16,
   OP_CAPABILITY = 17,
   OP_TYPE_VOID = 19,
   OP_TYPE_INT = 21,
   OP_TYPE_FLOAT = 22,
   OP_TYPE_VECTOR = 23,
   OP_TYPE_RUNTIME_ARRAY = 29,
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
   OP_NOT = 200,
   OP_LABEL = 248,
   OP_RETURN = 253,
   OP_NO_LINE = 317,
   OP_MODULE_PROCESSED = 330,

   OP_CONTROL_BARRIER = 224,
   OP_MEMORY_BARRIER = 225,
   OP_ATOMIC_FIRST = 227,
   OP_ATOMIC_LAST = 240,

   CAP_SHADER = 1,
   EXEC_MODEL_GLCOMPUTE = 5,
   EXEC_MODE_LOCAL_SIZE = 17,
   ADDRESSING_LOGICAL = 0,

   SC_INPUT = 1,
   SC_UNIFORM = 2,
   SC_WORKGROUP = 4,
   SC_FUNCTION = 7,
   SC_STORAGE_BUFFER = 12,

   DECOR_BLOCK = 2,
   DECOR_BUFFER_BLOCK = 3,
   DECOR_ARRAY_STRIDE = 6,
   DECOR_BUILTIN = 11,
   DECOR_RELAXED_PRECISION = 0,
   DECOR_COHERENT = 23,
   DECOR_NON_WRITABLE = 24,
   DECOR_RESTRICT = 19,
   DECOR_BINDING = 33,
   DECOR_DESCRIPTOR_SET = 34,
   DECOR_OFFSET = 35,

   BUILTIN_WORKGROUP_SIZE = 25,
   BUILTIN_GLOBAL_INVOCATION_ID = 28,
};

/* Per-result-id record: the module walk classifies every id once, and
 * the body walk reads the classifications back.  The kinds cover
 * exactly the admitted grammar.
 */
enum id_kind {
   ID_UNSET = 0,
   ID_TYPE_VOID,
   ID_TYPE_INT32,
   ID_TYPE_UINT3,
   ID_TYPE_RUNTIME_ARRAY,
   ID_TYPE_STRUCT,
   ID_TYPE_POINTER,
   ID_TYPE_FUNCTION,
   ID_CONST_INT,
   ID_CONST_COMPOSITE,
   ID_EXT_IMPORT,
   ID_VAR_GID,
   ID_VAR_SSBO,
   ID_VAR_FUNCTION,
   ID_FUNCTION,
   ID_LABEL,
   /* Body values. */
   ID_PTR_GID_X,
   ID_PTR_SSBO_ELEM,
   ID_VAL_GID_X,
   ID_VAL_LOADED_WORD,
   ID_VAL_COMPLEMENT_WORD,
};

struct id_info {
   uint8_t kind;
   /* ID_TYPE_POINTER: storage class; ID_VAR_*: pointer type id (a),
    * plus decoration state below.  ID_CONST_INT: the value.
    * ID_TYPE_RUNTIME_ARRAY: element type id (a), ArrayStride (b).
    * ID_TYPE_STRUCT: member-0 type id (a), member count (b), member-0
    * Offset (c).  ID_PTR_SSBO_ELEM: the SSBO variable id (a).
    * ID_VAL_LOADED_WORD and ID_VAL_COMPLEMENT_WORD: the SSBO variable
    * id the value came from (a).
    * ID_VAR_FUNCTION: the value kind the last recognized store left in
    * the variable (b).
    */
   uint32_t a;
   uint32_t b;
   uint32_t c;
   /* Decorations, collected before definitions are read. */
   bool has_set, has_binding, has_builtin, has_stride, has_member0_offset;
   uint32_t set, binding, builtin, stride, member0_offset;
};

struct reader {
   const uint32_t *words;
   size_t count;
   struct id_info *ids;
   uint32_t bound;
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

/* The storage-buffer element layout the executor's 4-byte flattened
 * addressing assumes: the variable points at a struct whose single
 * member is a runtime array of 32-bit integers at ArrayStride 4 and
 * member Offset 0.
 */
static bool ssbo_layout_is_word_array(struct reader *r, uint32_t var_id)
{
   const struct id_info *var = info(r, var_id);
   if (var == NULL)
      return false;
   const struct id_info *ptr = info(r, var->a);
   if (ptr == NULL || ptr->kind != ID_TYPE_POINTER)
      return false;
   const struct id_info *strct = info(r, ptr->b);
   if (strct == NULL || strct->kind != ID_TYPE_STRUCT ||
       strct->b != 1 || strct->c != 0)
      return false;
   const struct id_info *arr = info(r, strct->a);
   return arr != NULL && arr->kind == ID_TYPE_RUNTIME_ARRAY &&
          arr->b == 4 && id_is(r, arr->a, ID_TYPE_INT32);
}

static bool
admit_module(const uint32_t *words, size_t word_count,
             const char *entry_name,
             struct id_info *ids, uint32_t bound,
             struct r300_compute_job *job, const char **reason)
{
   struct reader reader = {
      .words = words,
      .count = word_count,
      .ids = ids,
      .bound = bound,
      .reason = reason,
      .entry_name = entry_name,
   };
   struct reader *r = &reader;

   bool capability_shader = false;
   bool memory_model_logical = false;
   uint32_t entry_point = 0;
   uint32_t local_size[3] = { 0, 0, 0 };

   /* Body state: the entry function's straight-line walk. */
   bool in_function = false;
   bool function_seen = false;
   bool label_seen = false;
   bool returned = false;
   uint32_t load_var = 0;
   uint32_t store_var = 0;
   uint32_t stored_value = 0;
   bool complement_seen = false;
   uint8_t job_op = R300_COMPUTE_JOB_OP_IDENTITY;

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

      /* The kernel's store ends the admitted body: only the return, the
       * function end, and line markers follow it, so a module carrying
       * work after its store refuses rather than lowering to a job that
       * discards it. */
      if (stored_value != 0 && opcode != OP_RETURN &&
          opcode != OP_FUNCTION_END && opcode != OP_LINE &&
          opcode != OP_NO_LINE)
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
         /* The import alone is inert; OpExtInst has no allowlist entry,
          * so an imported instruction still refuses at its use.
          */
         if (len < 3)
            return refuse(r, "malformed extended-instruction import");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed extended-instruction import");
         entry->kind = ID_EXT_IMPORT;
         break;
      }

      case OP_MEMORY_MODEL:
         if (len != 3 || w[1] != ADDRESSING_LOGICAL)
            return refuse(r, "memory model outside Logical addressing");
         memory_model_logical = true;
         break;

      case OP_ENTRY_POINT:
         if (len < 3 || w[1] != EXEC_MODEL_GLCOMPUTE)
            return refuse(r, "entry point outside the GLCompute model");
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
         if (len == 6 && w[2] == EXEC_MODE_LOCAL_SIZE &&
             w[1] == entry_point && entry_point != 0) {
            local_size[0] = w[3];
            local_size[1] = w[4];
            local_size[2] = w[5];
            break;
         }
         return refuse(r, "execution mode outside LocalSize");

      case OP_DECORATE: {
         if (len < 3)
            return refuse(r, "malformed decoration");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "decoration names an id outside the bound");
         switch (w[2]) {
         case DECOR_DESCRIPTOR_SET:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_set = true;
            entry->set = w[3];
            break;
         case DECOR_BINDING:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_binding = true;
            entry->binding = w[3];
            break;
         case DECOR_BUILTIN:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_builtin = true;
            entry->builtin = w[3];
            break;
         case DECOR_ARRAY_STRIDE:
            if (len != 4)
               return refuse(r, "malformed decoration");
            entry->has_stride = true;
            entry->stride = w[3];
            break;
         case DECOR_BLOCK:
         case DECOR_BUFFER_BLOCK:
         case DECOR_NON_WRITABLE:
         case DECOR_COHERENT:
         case DECOR_RESTRICT:
         case DECOR_RELAXED_PRECISION:
            break;
         default:
            return refuse(r, "decoration outside the admitted grammar");
         }
         break;
      }

      case OP_MEMBER_DECORATE:
         if (len < 4)
            return refuse(r, "malformed member decoration");
         switch (w[3]) {
         case DECOR_OFFSET: {
            if (len != 5 || w[2] != 0)
               return refuse(r,
                             "member layout outside the single-member "
                             "word array");
            struct id_info *entry = info(r, w[1]);
            if (entry == NULL)
               return refuse(r,
                             "decoration names an id outside the bound");
            entry->has_member0_offset = true;
            entry->member0_offset = w[4];
            break;
         }
         case DECOR_NON_WRITABLE:
         case DECOR_COHERENT:
         case DECOR_RESTRICT:
            break;
         default:
            return refuse(r,
                          "member decoration outside the admitted grammar");
         }
         break;

      case OP_TYPE_VOID: {
         if (len != 2)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         entry->kind = ID_TYPE_VOID;
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

      case OP_TYPE_FLOAT:
         return refuse(r, "float type outside the elementwise-map subset");

      case OP_TYPE_VECTOR: {
         if (len != 4)
            return refuse(r, "malformed type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (!id_is(r, w[2], ID_TYPE_INT32) || w[3] != 3)
            return refuse(r, "vector type outside the uint3 builtin");
         entry->kind = ID_TYPE_UINT3;
         break;
      }

      case OP_TYPE_RUNTIME_ARRAY: {
         /* The ArrayStride decoration landed on this id before the
          * definition; entry->c == 1 marks it recorded and entry->b
          * carries the stride.
          */
         if (len != 3)
            return refuse(r, "malformed type");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (entry->kind != ID_UNSET)
            return refuse(r, "result id defined twice");
         entry->kind = ID_TYPE_RUNTIME_ARRAY;
         entry->a = w[2];
         entry->b = entry->has_stride ? entry->stride : 0;
         break;
      }

      case OP_TYPE_STRUCT: {
         /* The member-0 Offset decoration landed on this id before the
          * definition; has_builtin marks it recorded and a carries the
          * offset.
          */
         if (len < 2)
            return refuse(r, "malformed type");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "malformed type");
         if (entry->kind != ID_UNSET)
            return refuse(r, "result id defined twice");
         entry->kind = ID_TYPE_STRUCT;
         entry->a = len >= 3 ? w[2] : 0;
         entry->b = len - 2;
         entry->c = entry->has_member0_offset ? entry->member0_offset
                                              : UINT32_MAX;
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
         if (!id_is(r, w[1], ID_TYPE_INT32))
            return refuse(r, "constant type outside 32-bit integers");
         entry->kind = ID_CONST_INT;
         entry->a = w[3];
         break;
      }

      case OP_CONSTANT_COMPOSITE: {
         if (len != 6)
            return refuse(r, "malformed composite constant");
         struct id_info *entry = info(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed composite constant");
         if (entry->kind != ID_UNSET)
            return refuse(r, "result id defined twice");
         if (!id_is(r, w[1], ID_TYPE_UINT3))
            return refuse(r, "composite constant outside uint3");
         const bool is_wg_size = entry->has_builtin &&
                                 entry->builtin == BUILTIN_WORKGROUP_SIZE;
         const struct id_info *x = info(r, w[3]);
         const struct id_info *y = info(r, w[4]);
         const struct id_info *z = info(r, w[5]);
         if (x == NULL || y == NULL || z == NULL ||
             x->kind != ID_CONST_INT || y->kind != ID_CONST_INT ||
             z->kind != ID_CONST_INT)
            return refuse(r, "composite constant outside literal words");
         entry->kind = ID_CONST_COMPOSITE;
         if (is_wg_size) {
            /* The WorkgroupSize builtin constant takes precedence over
             * the LocalSize mode, so the two agree or the module
             * refuses rather than executing at a size the mode belies.
             */
            if (x->a != local_size[0] || y->a != local_size[1] ||
                z->a != local_size[2])
               return refuse(r,
                             "WorkgroupSize constant disagrees with "
                             "LocalSize");
         }
         break;
      }

      case OP_VARIABLE: {
         if (len < 4)
            return refuse(r, "malformed variable");
         struct id_info *entry = info(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed variable");
         if (entry->kind != ID_UNSET)
            return refuse(r, "result id defined twice");
         if (len != 4)
            return refuse(r, "variable initializer outside the grammar");
         const uint32_t storage_class = w[3];
         const struct id_info *ptr = info(r, w[1]);
         if (ptr == NULL || ptr->kind != ID_TYPE_POINTER ||
             ptr->a != storage_class)
            return refuse(r, "variable type outside a matching pointer");
         switch (storage_class) {
         case SC_INPUT:
            if (!entry->has_builtin ||
                entry->builtin != BUILTIN_GLOBAL_INVOCATION_ID ||
                !id_is(r, ptr->b, ID_TYPE_UINT3))
               return refuse(r,
                             "input variable outside gl_GlobalInvocationID");
            entry->kind = ID_VAR_GID;
            entry->a = w[1];
            break;
         case SC_UNIFORM:
         case SC_STORAGE_BUFFER:
            if (!entry->has_set || !entry->has_binding)
               return refuse(r,
                             "storage buffer without set and binding "
                             "decorations");
            if (entry->set != 0)
               return refuse(r,
                             "storage buffer outside descriptor set 0");
            entry->kind = ID_VAR_SSBO;
            entry->a = w[1];
            break;
         case SC_FUNCTION:
            if (!in_function)
               return refuse(r, "function variable outside a function");
            if (!id_is(r, ptr->b, ID_TYPE_INT32))
               return refuse(r,
                             "function variable outside one 32-bit word");
            entry->kind = ID_VAR_FUNCTION;
            entry->a = w[1];
            entry->b = ID_UNSET; /* holds no symbolic value yet */
            break;
         case SC_WORKGROUP:
            return refuse(r, "workgroup shared memory");
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
         if (len < 5 || !in_function || returned)
            return refuse(r, "malformed access chain");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed access chain");
         const struct id_info *base = info(r, w[3]);
         if (base == NULL)
            return refuse(r, "access chain outside the bound");
         if (base->kind == ID_VAR_GID) {
            /* gid.x alone: the flattened index is the x channel. */
            if (len != 5 || !const_int_is(r, w[4], 0))
               return refuse(r,
                             "invocation-id access outside the x channel");
            entry->kind = ID_PTR_GID_X;
         } else if (base->kind == ID_VAR_SSBO) {
            /* member 0, element index: the index must be the loaded
             * invocation id, checked at the load or store that uses
             * this pointer.
             */
            if (len != 6 || !const_int_is(r, w[4], 0))
               return refuse(r,
                             "storage access outside member 0 of the "
                             "word array");
            if (!id_is(r, w[5], ID_VAL_GID_X))
               return refuse(r,
                             "storage address outside the flattened "
                             "invocation index");
            entry->kind = ID_PTR_SSBO_ELEM;
            entry->a = w[3];
         } else {
            return refuse(r, "access chain outside the admitted bases");
         }
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
         struct id_info *ptr = info(r, w[3]);
         if (ptr == NULL)
            return refuse(r, "load outside the bound");
         switch (ptr->kind) {
         case ID_PTR_GID_X:
            entry->kind = ID_VAL_GID_X;
            break;
         case ID_VAR_FUNCTION:
            /* The variable forwards the symbolic value it holds. */
            if (ptr->b == ID_VAL_GID_X)
               entry->kind = ID_VAL_GID_X;
            else
               return refuse(r,
                             "function variable read before a "
                             "recognized write");
            break;
         case ID_PTR_SSBO_ELEM:
            if (load_var != 0)
               return refuse(r, "more than one storage load");
            if (!id_is(r, w[1], ID_TYPE_INT32))
               return refuse(r, "load outside one 32-bit word");
            entry->kind = ID_VAL_LOADED_WORD;
            entry->a = ptr->a;
            load_var = ptr->a;
            break;
         default:
            return refuse(r, "load outside the admitted pointers");
         }
         break;
      }

      case OP_NOT: {
         /* The one arithmetic step the kernel grammar admits: the
          * complement of the word the storage load produced, which the
          * store then consumes.  One store consumes one value, so a
          * second complement would leave a computed word unwritten.
          */
         if (len != 4 || !in_function || returned)
            return refuse(r, "malformed bitwise complement");
         if (complement_seen)
            return refuse(r, "more than one bitwise complement");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "malformed bitwise complement");
         if (!id_is(r, w[1], ID_TYPE_INT32))
            return refuse(r, "bitwise complement outside one 32-bit word");
         const struct id_info *operand = info(r, w[3]);
         if (operand == NULL || operand->kind != ID_VAL_LOADED_WORD)
            return refuse(r, "bitwise complement outside the loaded word");
         entry->kind = ID_VAL_COMPLEMENT_WORD;
         entry->a = operand->a;
         complement_seen = true;
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
            if (value->kind != ID_VAL_GID_X)
               return refuse(r,
                             "function variable holds a value outside "
                             "the invocation index");
            ptr->b = ID_VAL_GID_X;
         } else if (ptr->kind == ID_PTR_SSBO_ELEM) {
            if (store_var != 0)
               return refuse(r, "more than one storage store");
            /* The complement, once computed, is the value the kernel
             * exists to write: a store of the loaded word beside it
             * would leave the complement unread, so the store names
             * whichever value the module's single arithmetic step
             * produced.
             */
            if (value->kind == ID_VAL_LOADED_WORD) {
               if (complement_seen)
                  return refuse(r,
                                "stored value leaves the bitwise "
                                "complement unread");
               job_op = R300_COMPUTE_JOB_OP_IDENTITY;
            } else if (value->kind == ID_VAL_COMPLEMENT_WORD) {
               job_op = R300_COMPUTE_JOB_OP_BITWISE_NOT;
            } else {
               return refuse(r,
                             "stored value outside the loaded word and "
                             "its bitwise complement");
            }
            store_var = ptr->a;
            stored_value = w[2];
         } else {
            return refuse(r, "store outside the admitted pointers");
         }
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

      case OP_CONTROL_BARRIER:
      case OP_MEMORY_BARRIER:
         return refuse(r, "control or memory barrier");

      default:
         if (opcode >= OP_ATOMIC_FIRST && opcode <= OP_ATOMIC_LAST)
            return refuse(r, "storage atomic");
         /* OpSNegate (126) through OpFMod (141): integer and float
          * arithmetic, the scatter module's address math among it.
          */
         if (opcode >= 126 && opcode <= 141)
            return refuse(r, "arithmetic outside the elementwise-map subset");
         return refuse(r, "opcode outside the elementwise-map subset");
      }
   }

   if (!capability_shader || !memory_model_logical || entry_point == 0)
      return refuse(r, "module preamble outside the admitted grammar");
   if (in_function || !function_seen || !returned)
      return refuse(r, "entry function did not complete");
   if (local_size[0] == 0 || local_size[1] == 0 || local_size[2] == 0)
      return refuse(r, "LocalSize execution mode absent");
   if (load_var == 0 || store_var == 0 || stored_value == 0)
      return refuse(r, "kernel lacks the load/store pair");
   if (load_var == store_var)
      return refuse(r, "load and store share one binding");
   if (!ssbo_layout_is_word_array(r, load_var) ||
       !ssbo_layout_is_word_array(r, store_var))
      return refuse(r, "storage layout outside the 4-byte word array");

   const struct id_info *in_var = info(r, load_var);
   const struct id_info *out_var = info(r, store_var);
   if (in_var == NULL || out_var == NULL ||
       in_var->binding == out_var->binding)
      return refuse(r, "load and store share one binding");

   memset(job, 0, sizeof(*job));
   job->op = job_op;
   job->input_binding = in_var->binding;
   job->output_binding = out_var->binding;
   job->local_size[0] = local_size[0];
   job->local_size[1] = local_size[1];
   job->local_size[2] = local_size[2];
   return true;
}

bool r3v_compute_job_from_spirv(const uint32_t *words, size_t word_count,
                                 const char *entry_name,
                                 struct r300_compute_job *job,
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
   if (bound == 0 || bound > R3V_COMPUTE_SPIRV_ID_BOUND_MAX) {
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
   const bool admitted =
      admit_module(words, word_count, entry_name, ids, bound, job, reason);
   free(ids);
   return admitted;
}
