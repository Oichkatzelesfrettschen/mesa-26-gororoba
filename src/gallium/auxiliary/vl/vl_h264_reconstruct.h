/*
 * SPDX-License-Identifier: MIT
 */

#ifndef vl_h264_reconstruct_h
#define vl_h264_reconstruct_h

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct nir_shader;
struct nir_shader_compiler_options;

/* H.264 sample reconstruction: the decoded sample is the predicted sample plus
 * the inverse-transform residual, clipped to [0, 255] (ITU-T H.264 sec 8.5.x
 * picture construction, Clip1Y).  This FP24 fragment program reads the
 * prediction at sampler binding 0 and the residual at binding 1 at the same
 * position and writes Clip1(pred + residual).  The prediction comes from the
 * motion-compensation programs (vl_h264_mc, vl_h264_chroma) for an inter block;
 * the residual from the inverse-transform program (vl_h264_idct).  This is the
 * step that joins the two halves of an inter macroblock into decoded pixels.
 *
 * Contract: both planes are sampler bindings 0 (prediction) and 1 (residual),
 * NEAREST; a varying at VARYING_SLOT_VAR0 carries the [0,1] sample position. */
void *vl_h264_reconstruct_create_fs(struct pipe_context *pipe);

/* The same kernel as raw NIR for the r300 compile-budget gate. */
struct nir_shader *
vl_h264_reconstruct_nir(const struct nir_shader_compiler_options *options);

#ifdef __cplusplus
}
#endif

#endif /* vl_h264_reconstruct_h */
