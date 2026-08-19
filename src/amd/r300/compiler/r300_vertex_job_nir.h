/*
 * SPDX-License-Identifier: MIT
 *
 * Semantic vertex front end: lowers an admitted NIR vertex shader to
 * the r300_vertex_job IR and reads an admitted constant-color NIR
 * fragment shader, replacing byte-identity module matching.
 */

#ifndef R300_VERTEX_JOB_NIR_H
#define R300_VERTEX_JOB_NIR_H

#include "r300_vertex_job.h"

#include "compiler/nir/nir.h"
#include "compiler/spirv/nir_spirv.h"

/* The NIR and SPIR-V ingestion options every job-front-end consumer
 * uses, so the driver and the calibration tests prepare shaders through
 * one path.
 */
const nir_shader_compiler_options *r300_vertex_job_nir_options(void);
const struct spirv_to_nir_options *r300_vertex_job_spirv_options(void);

/* Normalizes a freshly ingested shader for the analyzers: function
 * inlining, variable-to-SSA promotion, per-member struct splitting,
 * driver-location assignment (vertex inputs by generic attribute
 * index, the position and color-0 outputs at zero), vec4 IO lowering,
 * copy propagation, and dead-code elimination.  Returns false, leaving
 * *reason naming the refusal, for a shader whose stage or IO the
 * analyzers do not model.
 */
bool r300_vertex_job_nir_normalize(nir_shader *nir, const char **reason);

/* Lowers a normalized vertex shader to the job IR: straight-line vec4
 * code over attribute 0 -- LOAD_INPUT, vec4 constants, MOV, FADD,
 * FMUL, FMAD, DP4 (a scalar dot rejoined only by its own broadcast),
 * and exactly one full store to the position output.  The caller
 * assigns job->input_format_id from the pipeline's vertex-input state
 * and validates the finished job.  Returns false with *reason naming
 * the first inadmissible construct; the job is unspecified on refusal.
 */
bool r300_vertex_job_from_nir(nir_shader *nir, struct r300_vertex_job *job,
                              const char **reason);

/* Reads a normalized fragment shader that stores one compile-time
 * vec4 constant to color output 0 and nothing else; color_bits
 * receives the four component bit patterns.  Returns false with
 * *reason for any other program shape.
 */
bool r300_fragment_nir_constant_color(nir_shader *nir,
                                      uint32_t color_bits[4],
                                      const char **reason);

#endif /* R300_VERTEX_JOB_NIR_H */
