/*
 * SPDX-License-Identifier: MIT
 *
 * The Vulkan shader-interface record: what a vertex or fragment SPIR-V
 * module declares at its stage boundary, read from the module's
 * decorations alone, and the two-stage linkage the graphics pipeline
 * binds.  The record carries the qualifiers the R300 lowering consumes
 * later -- per-location scalar kind, width, component mask, and
 * Smooth/Flat/NoPerspective interpolation, plus the Position,
 * ClipDistance, and CullDistance built-ins -- and serializes
 * canonically so a digest pins every qualifier the record admitted.
 */

#ifndef R3V_SHADER_INTERFACE_H
#define R3V_SHADER_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Locations 0..15 per direction: the vertex job's attribute slot count
 * bounds the input side and the same budget bounds the varying side. */
#define R3V_SHADER_INTERFACE_MAX_LOCATIONS 16u

/* maxCombinedClipAndCullDistances at the Vulkan 1.0 floor; the
 * gl_PerVertex ClipDistance and CullDistance arrays share it. */
#define R3V_SHADER_INTERFACE_MAX_CLIP_CULL 8u

enum r3v_shader_interface_stage {
   R3V_SHADER_INTERFACE_STAGE_VERTEX = 0,
   R3V_SHADER_INTERFACE_STAGE_FRAGMENT,
};

/* The scalar kind of a location's components; every kind is 32 bits
 * wide, so the kind names the interpretation alone. */
enum r3v_shader_interface_scalar {
   R3V_SHADER_INTERFACE_SCALAR_FLOAT32 = 1,
   R3V_SHADER_INTERFACE_SCALAR_INT32,
   R3V_SHADER_INTERFACE_SCALAR_UINT32,
};

/* Vulkan interpolation of a fragment input: Smooth is the undecorated
 * default (perspective-correct), Flat replicates the provoking vertex's
 * value, NoPerspective interpolates linearly in window space. */
enum r3v_shader_interface_interpolation {
   R3V_SHADER_INTERFACE_SMOOTH = 0,
   R3V_SHADER_INTERFACE_FLAT,
   R3V_SHADER_INTERFACE_NOPERSPECTIVE,
};

/* One location of one direction.  component_mask has bit c set for
 * each of the four 32-bit components the location carries; width is
 * the declared vector width and the mask is that width shifted by the
 * Component decoration.  interpolation_declared records whether the
 * module decorated the location; an undeclared location reads Smooth.
 */
struct r3v_shader_interface_location {
   bool present;
   uint8_t scalar;
   uint8_t width;
   uint8_t component_mask;
   uint8_t interpolation;
   bool interpolation_declared;
};

struct r3v_shader_interface {
   uint8_t stage;
   struct r3v_shader_interface_location
      inputs[R3V_SHADER_INTERFACE_MAX_LOCATIONS];
   struct r3v_shader_interface_location
      outputs[R3V_SHADER_INTERFACE_MAX_LOCATIONS];
   /* The vertex stage declares Position through its gl_PerVertex
    * output block; the fragment stage reads none of these. */
   bool position_declared;
   uint8_t clip_distance_count;
   uint8_t cull_distance_count;
};

/* One linked varying: the vertex output and fragment input at one
 * location agree on kind, width, and mask, and the interpolation is
 * the qualifier the pipeline executes. */
struct r3v_shader_interface_varying {
   bool present;
   uint8_t scalar;
   uint8_t width;
   uint8_t component_mask;
   uint8_t interpolation;
};

struct r3v_shader_interface_link {
   struct r3v_shader_interface vertex;
   struct r3v_shader_interface fragment;
   struct r3v_shader_interface_varying
      varyings[R3V_SHADER_INTERFACE_MAX_LOCATIONS];
   /* Bit l set for each linked varying location. */
   uint32_t varying_mask;
   /* Bit l set for each linked varying decorated Flat; a subset of
    * varying_mask. */
   uint32_t flat_mask;
   uint32_t noperspective_mask;
   uint8_t clip_distance_count;
   uint8_t cull_distance_count;
};

/* Reads a module's stage boundary from its entry point's interface
 * list and decorations: every Input and Output variable classifies as
 * a located 32-bit scalar/vector, a located block or array of such
 * spanning consecutive locations (a member without its own Location
 * takes the one after the previous member's), or a gl_PerVertex
 * built-in member.  Refusals name the construct through *reason:
 * Flat and NoPerspective on one object, Centroid or Sample anywhere,
 * a qualifier on a boundary that carries none (vertex inputs, fragment
 * outputs), two objects on one component or two scalar kinds on one
 * location, a location at or past the 16-location budget, a member or
 * element outside a 32-bit scalar/vector (matrices, 64-bit types, and
 * nested arrays), a built-in outside Position, PointSize,
 * ClipDistance, CullDistance, VertexIndex, and InstanceIndex, and a
 * declared ClipDistance plus CullDistance length over the budget
 * (declared length counts, written or not).  The record is
 * unspecified on refusal.
 */
bool r3v_shader_interface_from_spirv(const uint32_t *words,
                                     size_t word_count,
                                     const char *entry_name,
                                     enum r3v_shader_interface_stage stage,
                                     struct r3v_shader_interface *out,
                                     const char **reason);

/* Links a vertex record to a fragment record.  Every fragment input
 * takes a vertex output of the same kind, width, and mask at its
 * location, and every vertex output takes a fragment consumer, so no
 * admitted qualifier is left without the stage that executes it.
 * An interpolation the two stages both declare must agree (a driver
 * policy stricter than the Vulkan interface-matching rules, which read
 * the fragment side alone); the fragment declaration otherwise
 * governs, then the vertex declaration, then Smooth.  An integer
 * varying without Flat refuses by that name; every varying then
 * executes only as a float vec4, the record the vertex carrier writes,
 * so an integer or narrower varying refuses as unexecutable.
 */
bool r3v_shader_interface_link(const struct r3v_shader_interface *vertex,
                               const struct r3v_shader_interface *fragment,
                               struct r3v_shader_interface_link *out,
                               const char **reason);

/* Writes the record as canonical text, one fact per line in location
 * order, and returns the byte count the full text needs (snprintf
 * semantics: the text is truncated to capacity and always
 * NUL-terminated when capacity is nonzero).  Two records serialize
 * identically exactly when every admitted qualifier is equal, so the
 * text's digest pins the record.
 */
size_t r3v_shader_interface_serialize(const struct r3v_shader_interface *in,
                                      char *buf, size_t capacity);
size_t
r3v_shader_interface_link_serialize(const struct r3v_shader_interface_link *in,
                                    char *buf, size_t capacity);

#endif /* R3V_SHADER_INTERFACE_H */
