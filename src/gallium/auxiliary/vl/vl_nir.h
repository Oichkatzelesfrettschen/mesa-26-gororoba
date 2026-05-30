/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_nir_h
#define vl_nir_h

#include "compiler/nir/nir_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;

/* Shared pure-NIR construction for the g3dvl shader decode kernels.  Every
 * decode kernel renders full-screen quads that sample one or more planes and
 * combine them with fragment-ALU MACs.  The shader skeleton repeats across all
 * of them -- texcoord varyings in, combined samplers, one color out -- and the
 * kernels differ only in the dataflow between.  These helpers carry the
 * skeleton so each kernel writes only its first-principles math, and they emit
 * NIR directly: r300 advertises PIPE_SHADER_IR_NIR and lowers through nir_to_rc,
 * so no TGSI intermediate is built. */

#define VL_NIR_MAX_TC   4
#define VL_NIR_MAX_SAMP 8

struct vl_nir_fs {
   nir_builder       b;
   nir_variable     *out_color;
   nir_def          *texcoord[VL_NIR_MAX_TC];
   nir_deref_instr  *samp[VL_NIR_MAX_SAMP];
};

/* Vertex passthrough: input attribute 0 (position) is copied to the clip-space
 * POSITION output and to num_tc GENERIC texcoord outputs.  The vl quad carries
 * texcoord == position, so a single MOV feeds both. */
void *vl_nir_vs_passthrough(struct pipe_context *pipe, unsigned num_tc,
                            const char *name);

/* Finalize a hand-built vertex shader and create_vs_state.  For kernels whose
 * vertex stage is not a passthrough (e.g. zscan computes scan addresses): the
 * caller builds the body, this wraps the driver handoff. */
void *vl_nir_vs_finish(nir_builder *b, struct pipe_context *pipe);

/* Begin a fragment shader: declare num_tc texcoord-in varyings (loaded into
 * fs->texcoord[]) and the color output.  Add samplers with vl_nir_sampler. */
void vl_nir_fs_begin(struct vl_nir_fs *fs, struct pipe_context *pipe,
                     unsigned num_tc, const char *name);

/* Declare a combined sampler at binding s with dimensionality dim
 * (GLSL_SAMPLER_DIM_2D, _3D, ...); the deref is stored in fs->samp[s]. */
void vl_nir_sampler(struct vl_nir_fs *fs, unsigned s,
                    enum glsl_sampler_dim dim);

/* Sample sampler s at coord; coord component count must match the sampler dim
 * (vec2 for 2D, vec3 for 3D). */
nir_def *vl_nir_tex(struct vl_nir_fs *fs, unsigned s, nir_def *coord);

/* Store color to the output, finalize the NIR, and create_fs_state.  Returns
 * the driver shader handle. */
void *vl_nir_fs_finish(struct vl_nir_fs *fs, struct pipe_context *pipe,
                       nir_def *color);

#ifdef __cplusplus
}
#endif

#endif /* vl_nir_h */
