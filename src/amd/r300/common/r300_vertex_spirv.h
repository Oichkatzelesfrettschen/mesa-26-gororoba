/*
 * SPDX-License-Identifier: MIT
 *
 * Direct SPIR-V admission for the vertex-job IR: a fail-closed
 * word-stream reader that recognizes the admitted module shapes from
 * SPIR-V itself, with no intermediate compiler representation.  The
 * admitted grammar is an opcode allowlist plus a symbolic walk of the
 * straight-line entry function, so every unrecognized construct
 * refuses by name before any job field publishes.
 */

#ifndef R300_VERTEX_SPIRV_H
#define R300_VERTEX_SPIRV_H

#include "r300_vertex_job.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lowers an admitted SPIR-V vertex module to the job IR.  The admitted
 * subset is straight-line vec4 code over one location-0 vec4 float
 * input: loads of that input, vec4 float composite constants, FAdd,
 * FMul, GLSL.std.450 Fma (fused), Dot rejoined only by its own
 * four-way replicate, and exactly one full store of the Position
 * builtin as the program's result.  Descriptors, push constants,
 * control flow, non-32-bit types, and every opcode outside the
 * recognized grammar refuse with *reason naming the construct; the job
 * leaves with input_format_id unassigned and is unspecified on
 * refusal.
 */
bool r300_vertex_job_from_spirv(const uint32_t *words, size_t word_count,
                                struct r300_vertex_job *job,
                                const char **reason);

/* Reads an admitted SPIR-V fragment module back as its one constant
 * color: a straight-line Fragment entry function whose single store
 * writes a vec4 float composite constant to the location-0 output.
 * color_bits receives the four 32-bit lane patterns verbatim; any
 * other module shape refuses with *reason naming the construct.
 */
bool r300_fragment_constant_color_from_spirv(const uint32_t *words,
                                             size_t word_count,
                                             uint32_t color_bits[4],
                                             const char **reason);

#endif /* R300_VERTEX_SPIRV_H */
