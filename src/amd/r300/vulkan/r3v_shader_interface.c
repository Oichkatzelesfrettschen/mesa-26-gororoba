/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_shader_interface.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SPIR-V 1.0 words the reader classifies: opcodes, storage classes,
 * execution models, decorations, and built-ins by their specification
 * numbers.  Everything outside this table is skipped, since the
 * interface is a decoration-and-declaration fact that the function
 * bodies cannot change.
 */
enum {
   OP_ENTRY_POINT = 15,
   OP_TYPE_INT = 21,
   OP_TYPE_FLOAT = 22,
   OP_TYPE_VECTOR = 23,
   OP_TYPE_ARRAY = 28,
   OP_TYPE_STRUCT = 30,
   OP_TYPE_POINTER = 32,
   OP_CONSTANT = 43,
   OP_VARIABLE = 59,
   OP_DECORATE = 71,
   OP_MEMBER_DECORATE = 72,

   STORAGE_INPUT = 1,
   STORAGE_OUTPUT = 3,

   EXEC_MODEL_VERTEX = 0,
   EXEC_MODEL_FRAGMENT = 4,

   DECOR_BUILTIN = 11,
   DECOR_NOPERSPECTIVE = 13,
   DECOR_FLAT = 14,
   DECOR_CENTROID = 16,
   DECOR_SAMPLE = 17,
   DECOR_LOCATION = 30,
   DECOR_COMPONENT = 31,

   BUILTIN_POSITION = 0,
   BUILTIN_POINT_SIZE = 1,
   BUILTIN_CLIP_DISTANCE = 3,
   BUILTIN_CULL_DISTANCE = 4,
   BUILTIN_VERTEX_INDEX = 42,
   BUILTIN_INSTANCE_INDEX = 43,
};

enum id_kind {
   ID_UNSET = 0,
   ID_TYPE_SCALAR,   /* a: r3v_shader_interface_scalar */
   ID_TYPE_VECTOR,   /* a: scalar, b: width */
   ID_TYPE_ARRAY,    /* a: element type id, b: length */
   ID_TYPE_STRUCT,   /* a: member count, members[] */
   ID_TYPE_POINTER,  /* a: storage class, b: pointee type id */
   ID_TYPE_OTHER,
   ID_CONST_INT,     /* a: value */
   ID_VARIABLE,      /* a: storage class, b: pointer type id */
};

#define MAX_STRUCT_MEMBERS 8u

/* The decorations one id or one struct member can carry. */
struct decor {
   bool has_location, has_component, has_builtin;
   uint32_t location, component, builtin;
   bool flat, noperspective;
   bool centroid_or_sample;
};

struct id_info {
   uint8_t kind;
   uint32_t a, b;
   uint32_t members[MAX_STRUCT_MEMBERS];
   struct decor decor;
   struct decor member_decor[MAX_STRUCT_MEMBERS];
};

#define MAX_IDS 1024u
#define MAX_INTERFACE_IDS 32u

struct reader {
   struct id_info ids[MAX_IDS];
   uint32_t bound;
   const char **reason;
   struct r3v_shader_interface *out;
   uint32_t interface_ids[MAX_INTERFACE_IDS];
   uint32_t interface_count;
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

static struct id_info *define(struct reader *r, uint32_t id)
{
   struct id_info *entry = info(r, id);
   if (entry == NULL || entry->kind != ID_UNSET)
      return NULL;
   return entry;
}

static bool apply_decoration(struct reader *r, struct decor *d,
                             const uint32_t *operand, uint32_t operands)
{
   switch (operand[0]) {
   case DECOR_LOCATION:
      if (operands != 2)
         return refuse(r, "malformed interface decoration");
      d->has_location = true;
      d->location = operand[1];
      return true;
   case DECOR_COMPONENT:
      if (operands != 2)
         return refuse(r, "malformed interface decoration");
      d->has_component = true;
      d->component = operand[1];
      return true;
   case DECOR_BUILTIN:
      if (operands != 2)
         return refuse(r, "malformed interface decoration");
      d->has_builtin = true;
      d->builtin = operand[1];
      return true;
   case DECOR_FLAT:
      d->flat = true;
      return true;
   case DECOR_NOPERSPECTIVE:
      d->noperspective = true;
      return true;
   case DECOR_CENTROID:
   case DECOR_SAMPLE:
      d->centroid_or_sample = true;
      return true;
   default:
      /* Block, RelaxedPrecision, Binding, DescriptorSet, and the rest
       * carry no interface fact. */
      return true;
   }
}

/* The interpolation a decoration set names, refusing the pair Vulkan
 * forbids on one object and the auxiliary qualifiers the pipeline
 * carries no sampling mode for. */
static bool decor_interpolation(struct reader *r, const struct decor *d,
                                struct r3v_shader_interface_location *loc)
{
   if (d->flat && d->noperspective)
      return refuse(r, "Flat and NoPerspective on one interface location");
   if (d->centroid_or_sample)
      return refuse(r, "Centroid or Sample interpolation outside the "
                       "admitted grammar");
   loc->interpolation_declared = d->flat || d->noperspective;
   loc->interpolation = d->flat ? R3V_SHADER_INTERFACE_FLAT
                      : d->noperspective ? R3V_SHADER_INTERFACE_NOPERSPECTIVE
                      : R3V_SHADER_INTERFACE_SMOOTH;
   return true;
}

/* Places one scalar or vector at a location of one direction; the
 * mask is the declared width shifted by the Component decoration, and
 * a second object landing on an already-claimed component refuses. */
static bool place_location(struct reader *r,
                           struct r3v_shader_interface_location *table,
                           uint32_t location, uint32_t component,
                           const struct id_info *type,
                           const struct decor *d, bool interpolated)
{
   uint32_t scalar, width;
   if (type->kind == ID_TYPE_SCALAR) {
      scalar = type->a;
      width = 1;
   } else if (type->kind == ID_TYPE_VECTOR) {
      scalar = type->a;
      width = type->b;
   } else {
      return refuse(r, "interface location outside a 32-bit scalar or "
                       "vector");
   }
   if (location >= R3V_SHADER_INTERFACE_MAX_LOCATIONS)
      return refuse(r, "interface location outside the 16-location budget");
   if (component + width > 4)
      return refuse(r, "interface component span outside the four "
                       "components of a location");
   uint32_t mask = ((1u << width) - 1u) << component;
   struct r3v_shader_interface_location *loc = &table[location];
   if (loc->present) {
      if ((loc->component_mask & mask) != 0)
         return refuse(r, "two interface objects claim one component");
      if (loc->scalar != scalar)
         return refuse(r, "two scalar kinds share one interface location");
      /* Two objects split the four components: both are separately
       * qualified, so the location keeps one interpolation only when
       * they agree. */
      struct r3v_shader_interface_location other = { 0 };
      if (!decor_interpolation(r, d, &other))
         return false;
      if (interpolated && other.interpolation != loc->interpolation)
         return refuse(r, "interpolation qualifiers conflict across the "
                          "components of one location");
      loc->component_mask |= mask;
      loc->width = (uint8_t)(loc->width + width);
      loc->interpolation_declared |= other.interpolation_declared;
      return true;
   }
   loc->present = true;
   loc->scalar = (uint8_t)scalar;
   loc->width = (uint8_t)width;
   loc->component_mask = (uint8_t)mask;
   if (interpolated)
      return decor_interpolation(r, d, loc);
   if (d->flat || d->noperspective || d->centroid_or_sample)
      return refuse(r, "interpolation qualifier on a boundary that "
                       "carries none");
   loc->interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   loc->interpolation_declared = false;
   return true;
}

/* One gl_PerVertex member: Position is the vec4 the vertex stage
 * declares, ClipDistance and CullDistance are float arrays whose
 * lengths count against the shared budget, PointSize is carried along
 * by GLSL and records nothing. */
static bool builtin_member(struct reader *r, uint32_t builtin,
                           const struct id_info *type, bool vertex_output)
{
   struct r3v_shader_interface *out = r->out;
   switch (builtin) {
   case BUILTIN_POSITION:
      if (!vertex_output || type->kind != ID_TYPE_VECTOR || type->b != 4 ||
          type->a != R3V_SHADER_INTERFACE_SCALAR_FLOAT32)
         return refuse(r, "Position outside a vertex vec4 output");
      out->position_declared = true;
      return true;
   case BUILTIN_POINT_SIZE:
      return vertex_output ? true
                           : refuse(r, "PointSize outside a vertex output");
   case BUILTIN_CLIP_DISTANCE:
   case BUILTIN_CULL_DISTANCE: {
      if (!vertex_output || type->kind != ID_TYPE_ARRAY)
         return refuse(r, "ClipDistance or CullDistance outside a vertex "
                          "float array output");
      const struct id_info *element = info(r, type->a);
      if (element == NULL || element->kind != ID_TYPE_SCALAR ||
          element->a != R3V_SHADER_INTERFACE_SCALAR_FLOAT32)
         return refuse(r, "ClipDistance or CullDistance outside a vertex "
                          "float array output");
      uint32_t count = type->b;
      if (builtin == BUILTIN_CLIP_DISTANCE)
         out->clip_distance_count = (uint8_t)count;
      else
         out->cull_distance_count = (uint8_t)count;
      if ((uint32_t)out->clip_distance_count +
             (uint32_t)out->cull_distance_count >
          R3V_SHADER_INTERFACE_MAX_CLIP_CULL)
         return refuse(r, "ClipDistance plus CullDistance outside the "
                          "combined budget of 8");
      return true;
   }
   default:
      return refuse(r, "built-in outside the admitted interface");
   }
}

static bool classify_variable(struct reader *r, uint32_t id)
{
   struct r3v_shader_interface *out = r->out;
   const struct id_info *var = info(r, id);
   if (var == NULL || var->kind != ID_VARIABLE)
      return refuse(r, "entry point interface names a non-variable");
   const struct id_info *ptr = info(r, var->b);
   if (ptr == NULL || ptr->kind != ID_TYPE_POINTER || ptr->a != var->a)
      return refuse(r, "interface variable outside an Input or Output "
                       "pointer");
   const struct id_info *type = info(r, ptr->b);
   if (type == NULL)
      return refuse(r, "interface variable of an undefined type");
   bool is_output = var->a == STORAGE_OUTPUT;
   bool vertex = out->stage == R3V_SHADER_INTERFACE_STAGE_VERTEX;
   /* The interpolated boundary is the vertex output / fragment input
    * edge; vertex inputs and fragment outputs carry no qualifier. */
   bool interpolated = vertex == is_output;
   struct r3v_shader_interface_location *table =
      is_output ? out->outputs : out->inputs;
   const struct decor *d = &var->decor;

   if (d->has_builtin) {
      if (vertex && !is_output &&
          (d->builtin == BUILTIN_VERTEX_INDEX ||
           d->builtin == BUILTIN_INSTANCE_INDEX))
         return true;
      return builtin_member(r, d->builtin, type, vertex && is_output);
   }

   if (type->kind == ID_TYPE_STRUCT) {
      bool builtin_block = false, located_block = false;
      for (uint32_t m = 0; m < type->a; m++) {
         if (type->member_decor[m].has_builtin)
            builtin_block = true;
         else
            located_block = true;
      }
      if (builtin_block && located_block)
         return refuse(r, "interface block mixes built-in and located "
                          "members");
      if (builtin_block) {
         for (uint32_t m = 0; m < type->a; m++) {
            const struct id_info *member = info(r, type->members[m]);
            if (member == NULL)
               return refuse(r, "interface block member of an undefined "
                                "type");
            if (!builtin_member(r, type->member_decor[m].builtin, member,
                                vertex && is_output))
               return false;
         }
         return true;
      }
      /* A located block: the variable's Location seeds the first member
       * and each member without its own Location takes the next one. */
      if (!d->has_location)
         return refuse(r, "interface block without a Location");
      uint32_t next = d->location;
      for (uint32_t m = 0; m < type->a; m++) {
         const struct decor *md = &type->member_decor[m];
         const struct id_info *member = info(r, type->members[m]);
         if (member == NULL)
            return refuse(r, "interface block member of an undefined "
                             "type");
         uint32_t location = md->has_location ? md->location : next;
         struct decor merged = *md;
         merged.flat |= d->flat;
         merged.noperspective |= d->noperspective;
         merged.centroid_or_sample |= d->centroid_or_sample;
         if (!place_location(r, table, location,
                             md->has_component ? md->component : 0,
                             member, &merged, interpolated))
            return false;
         next = location + 1;
      }
      return true;
   }

   if (!d->has_location)
      return refuse(r, "interface variable without a Location");
   if (type->kind == ID_TYPE_ARRAY) {
      const struct id_info *element = info(r, type->a);
      if (element == NULL)
         return refuse(r, "interface array of an undefined element type");
      for (uint32_t i = 0; i < type->b; i++) {
         if (!place_location(r, table, d->location + i,
                             d->has_component ? d->component : 0, element,
                             d, interpolated))
            return false;
      }
      return true;
   }
   return place_location(r, table, d->location,
                         d->has_component ? d->component : 0, type, d,
                         interpolated);
}

static bool read_module(struct reader *r, const uint32_t *words,
                        size_t word_count, const char *entry_name,
                        enum r3v_shader_interface_stage stage);

bool r3v_shader_interface_from_spirv(const uint32_t *words,
                                     size_t word_count,
                                     const char *entry_name,
                                     enum r3v_shader_interface_stage stage,
                                     struct r3v_shader_interface *out,
                                     const char **reason)
{
   memset(out, 0, sizeof(*out));
   out->stage = (uint8_t)stage;
   if (words == NULL || word_count < 5 || words[0] != 0x07230203u) {
      *reason = "module outside the SPIR-V header";
      return false;
   }
   if (words[3] == 0 || words[3] > MAX_IDS) {
      *reason = "id bound outside the interface reader's table";
      return false;
   }
   /* Pipeline creation runs on any thread, so the reader's id table is
    * per call. */
   struct reader *r = calloc(1, sizeof(*r));
   if (r == NULL) {
      *reason = "interface reader allocation failed";
      return false;
   }
   r->reason = reason;
   r->out = out;
   r->bound = words[3];
   bool admitted = read_module(r, words, word_count, entry_name, stage);
   free(r);
   return admitted;
}

static bool read_module(struct reader *r, const uint32_t *words,
                        size_t word_count, const char *entry_name,
                        enum r3v_shader_interface_stage stage)
{
   uint32_t exec_model = stage == R3V_SHADER_INTERFACE_STAGE_VERTEX
                            ? EXEC_MODEL_VERTEX : EXEC_MODEL_FRAGMENT;
   bool entry_seen = false;

   size_t pos = 5;
   while (pos < word_count) {
      const uint32_t *w = &words[pos];
      uint32_t len = w[0] >> 16;
      uint32_t op = w[0] & 0xffffu;
      if (len == 0 || pos + len > word_count)
         return refuse(r, "instruction length outside the module");
      switch (op) {
      case OP_ENTRY_POINT: {
         if (len < 4)
            return refuse(r, "malformed entry point");
         if (w[1] != exec_model)
            return refuse(r, "entry point outside the requested model");
         if (entry_seen)
            return refuse(r, "more than one entry point");
         entry_seen = true;
         /* The name literal is NUL-padded to a word boundary. */
         const char *name = (const char *)&w[3];
         size_t name_bytes = (len - 3) * 4, name_len = 0;
         while (name_len < name_bytes && name[name_len] != '\0')
            name_len++;
         if (name_len == name_bytes)
            return refuse(r, "entry point name outside the request");
         size_t name_words = (name_len + 4) / 4;
         if (strlen(entry_name) != name_len ||
             memcmp(name, entry_name, name_len) != 0)
            return refuse(r, "entry point name outside the request");
         for (uint32_t i = 3 + (uint32_t)name_words; i < len; i++) {
            if (r->interface_count >= MAX_INTERFACE_IDS)
               return refuse(r, "entry point interface outside the "
                                "reader's table");
            r->interface_ids[r->interface_count++] = w[i];
         }
         break;
      }
      case OP_DECORATE: {
         if (len < 3)
            return refuse(r, "malformed interface decoration");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "decoration names an id outside the bound");
         if (!apply_decoration(r, &entry->decor, &w[2], len - 2))
            return false;
         break;
      }
      case OP_MEMBER_DECORATE: {
         if (len < 4)
            return refuse(r, "malformed interface decoration");
         struct id_info *entry = info(r, w[1]);
         if (entry == NULL)
            return refuse(r, "decoration names an id outside the bound");
         if (w[2] >= MAX_STRUCT_MEMBERS)
            return refuse(r, "interface block member outside the "
                             "eight-member table");
         if (!apply_decoration(r, &entry->member_decor[w[2]], &w[3],
                               len - 3))
            return false;
         break;
      }
      case OP_TYPE_INT:
      case OP_TYPE_FLOAT: {
         if (len < 3)
            return refuse(r, "malformed interface type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "interface type redefines an id");
         if (w[2] != 32) {
            entry->kind = ID_TYPE_OTHER;
            break;
         }
         entry->kind = ID_TYPE_SCALAR;
         entry->a = op == OP_TYPE_FLOAT ? R3V_SHADER_INTERFACE_SCALAR_FLOAT32
                    : (len >= 4 && w[3] != 0)
                       ? R3V_SHADER_INTERFACE_SCALAR_INT32
                       : R3V_SHADER_INTERFACE_SCALAR_UINT32;
         break;
      }
      case OP_TYPE_VECTOR: {
         if (len != 4)
            return refuse(r, "malformed interface type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "interface type redefines an id");
         const struct id_info *component = info(r, w[2]);
         if (component != NULL && component->kind == ID_TYPE_SCALAR &&
             w[3] >= 2 && w[3] <= 4) {
            entry->kind = ID_TYPE_VECTOR;
            entry->a = component->a;
            entry->b = w[3];
         } else {
            entry->kind = ID_TYPE_OTHER;
         }
         break;
      }
      case OP_TYPE_ARRAY: {
         if (len != 4)
            return refuse(r, "malformed interface type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "interface type redefines an id");
         const struct id_info *length = info(r, w[3]);
         if (length == NULL || length->kind != ID_CONST_INT)
            return refuse(r, "interface array length outside an integer "
                             "constant");
         entry->kind = ID_TYPE_ARRAY;
         entry->a = w[2];
         entry->b = length->a;
         break;
      }
      case OP_TYPE_STRUCT: {
         if (len < 2)
            return refuse(r, "malformed interface type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "interface type redefines an id");
         if (len - 2 > MAX_STRUCT_MEMBERS) {
            entry->kind = ID_TYPE_OTHER;
            break;
         }
         entry->kind = ID_TYPE_STRUCT;
         entry->a = len - 2;
         for (uint32_t m = 0; m < len - 2; m++)
            entry->members[m] = w[2 + m];
         break;
      }
      case OP_TYPE_POINTER: {
         if (len != 4)
            return refuse(r, "malformed interface type");
         struct id_info *entry = define(r, w[1]);
         if (entry == NULL)
            return refuse(r, "interface type redefines an id");
         entry->kind = ID_TYPE_POINTER;
         entry->a = w[2];
         entry->b = w[3];
         break;
      }
      case OP_CONSTANT: {
         if (len < 4)
            return refuse(r, "malformed interface constant");
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "interface constant redefines an id");
         const struct id_info *type = info(r, w[1]);
         if (type != NULL && type->kind == ID_TYPE_SCALAR &&
             type->a != R3V_SHADER_INTERFACE_SCALAR_FLOAT32 && len == 4) {
            entry->kind = ID_CONST_INT;
            entry->a = w[3];
         } else {
            entry->kind = ID_TYPE_OTHER;
         }
         break;
      }
      case OP_VARIABLE: {
         if (len < 4)
            return refuse(r, "malformed interface variable");
         if (w[3] != STORAGE_INPUT && w[3] != STORAGE_OUTPUT)
            break;
         struct id_info *entry = define(r, w[2]);
         if (entry == NULL)
            return refuse(r, "interface variable redefines an id");
         entry->kind = ID_VARIABLE;
         entry->a = w[3];
         entry->b = w[1];
         break;
      }
      default:
         break;
      }
      pos += len;
   }
   if (!entry_seen)
      return refuse(r, "module without the requested entry point");

   for (uint32_t i = 0; i < r->interface_count; i++) {
      if (!classify_variable(r, r->interface_ids[i]))
         return false;
   }
   return true;
}

static const char *scalar_name(uint32_t scalar)
{
   switch (scalar) {
   case R3V_SHADER_INTERFACE_SCALAR_FLOAT32: return "float32";
   case R3V_SHADER_INTERFACE_SCALAR_INT32: return "int32";
   case R3V_SHADER_INTERFACE_SCALAR_UINT32: return "uint32";
   default: return "?";
   }
}

static const char *interpolation_name(uint32_t interpolation)
{
   switch (interpolation) {
   case R3V_SHADER_INTERFACE_SMOOTH: return "smooth";
   case R3V_SHADER_INTERFACE_FLAT: return "flat";
   case R3V_SHADER_INTERFACE_NOPERSPECTIVE: return "noperspective";
   default: return "?";
   }
}

bool r3v_shader_interface_link(const struct r3v_shader_interface *vertex,
                               const struct r3v_shader_interface *fragment,
                               struct r3v_shader_interface_link *out,
                               const char **reason)
{
   memset(out, 0, sizeof(*out));
   if (vertex->stage != R3V_SHADER_INTERFACE_STAGE_VERTEX ||
       fragment->stage != R3V_SHADER_INTERFACE_STAGE_FRAGMENT) {
      *reason = "link outside a vertex and a fragment record";
      return false;
   }
   out->vertex = *vertex;
   out->fragment = *fragment;
   out->clip_distance_count = vertex->clip_distance_count;
   out->cull_distance_count = vertex->cull_distance_count;
   for (uint32_t l = 0; l < R3V_SHADER_INTERFACE_MAX_LOCATIONS; l++) {
      const struct r3v_shader_interface_location *vs = &vertex->outputs[l];
      const struct r3v_shader_interface_location *fs = &fragment->inputs[l];
      if (!vs->present && !fs->present)
         continue;
      if (fs->present && !vs->present) {
         *reason = "fragment input without a vertex output at its location";
         return false;
      }
      if (vs->present && !fs->present) {
         *reason = "vertex output without a fragment consumer";
         return false;
      }
      if (vs->scalar != fs->scalar || vs->width != fs->width ||
          vs->component_mask != fs->component_mask) {
         *reason = "vertex output and fragment input shapes differ";
         return false;
      }
      if (vs->interpolation_declared && fs->interpolation_declared &&
          vs->interpolation != fs->interpolation) {
         *reason = "interpolation qualifiers conflict across the stages";
         return false;
      }
      uint32_t interpolation = fs->interpolation_declared ? fs->interpolation
                             : vs->interpolation_declared ? vs->interpolation
                             : R3V_SHADER_INTERFACE_SMOOTH;
      if (vs->scalar != R3V_SHADER_INTERFACE_SCALAR_FLOAT32 &&
          interpolation != R3V_SHADER_INTERFACE_FLAT) {
         *reason = "integer varying without Flat";
         return false;
      }
      if (vs->scalar != R3V_SHADER_INTERFACE_SCALAR_FLOAT32 ||
          vs->component_mask != 0xf) {
         *reason = "varying outside the float vec4 the vertex carrier "
                   "executes";
         return false;
      }
      struct r3v_shader_interface_varying *v = &out->varyings[l];
      v->present = true;
      v->scalar = vs->scalar;
      v->width = vs->width;
      v->component_mask = vs->component_mask;
      v->interpolation = (uint8_t)interpolation;
      out->varying_mask |= 1u << l;
      if (interpolation == R3V_SHADER_INTERFACE_FLAT)
         out->flat_mask |= 1u << l;
      else if (interpolation == R3V_SHADER_INTERFACE_NOPERSPECTIVE)
         out->noperspective_mask |= 1u << l;
   }
   return true;
}

/* Appends one formatted line, tracking the full length the way snprintf
 * reports it so a truncated buffer still yields the needed size. */
struct sink {
   char *buf;
   size_t capacity;
   size_t length;
};

static void put(struct sink *s, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char *dst = s->length < s->capacity ? s->buf + s->length : NULL;
   size_t room = s->length < s->capacity ? s->capacity - s->length : 0;
   int n = vsnprintf(dst, room, fmt, ap);
   va_end(ap);
   if (n > 0)
      s->length += (size_t)n;
}

static void put_table(struct sink *s, const char *direction,
                      const struct r3v_shader_interface_location *table)
{
   for (uint32_t l = 0; l < R3V_SHADER_INTERFACE_MAX_LOCATIONS; l++) {
      const struct r3v_shader_interface_location *loc = &table[l];
      if (!loc->present)
         continue;
      put(s, "%s[%u]=%sx%u mask=%x interpolation=%s declared=%u\n",
          direction, l, scalar_name(loc->scalar), loc->width,
          loc->component_mask, interpolation_name(loc->interpolation),
          loc->interpolation_declared ? 1u : 0u);
   }
}

static void put_interface(struct sink *s,
                          const struct r3v_shader_interface *in)
{
   put(s, "stage=%s\n",
       in->stage == R3V_SHADER_INTERFACE_STAGE_VERTEX ? "vertex"
                                                       : "fragment");
   put_table(s, "in", in->inputs);
   put_table(s, "out", in->outputs);
   put(s, "position=%u clip=%u cull=%u\n", in->position_declared ? 1u : 0u,
       in->clip_distance_count, in->cull_distance_count);
}

size_t r3v_shader_interface_serialize(const struct r3v_shader_interface *in,
                                      char *buf, size_t capacity)
{
   struct sink s = { buf, capacity, 0 };
   if (capacity != 0)
      buf[0] = '\0';
   put_interface(&s, in);
   if (capacity != 0 && s.length >= capacity)
      buf[capacity - 1] = '\0';
   return s.length;
}

size_t
r3v_shader_interface_link_serialize(const struct r3v_shader_interface_link *in,
                                    char *buf, size_t capacity)
{
   struct sink s = { buf, capacity, 0 };
   if (capacity != 0)
      buf[0] = '\0';
   put_interface(&s, &in->vertex);
   put_interface(&s, &in->fragment);
   for (uint32_t l = 0; l < R3V_SHADER_INTERFACE_MAX_LOCATIONS; l++) {
      const struct r3v_shader_interface_varying *v = &in->varyings[l];
      if (!v->present)
         continue;
      put(&s, "varying[%u]=%sx%u mask=%x interpolation=%s\n", l,
          scalar_name(v->scalar), v->width, v->component_mask,
          interpolation_name(v->interpolation));
   }
   put(&s, "varying_mask=%x flat_mask=%x noperspective_mask=%x "
           "clip=%u cull=%u\n",
       in->varying_mask, in->flat_mask, in->noperspective_mask,
       in->clip_distance_count, in->cull_distance_count);
   if (capacity != 0 && s.length >= capacity)
      buf[capacity - 1] = '\0';
   return s.length;
}
