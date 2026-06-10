/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_DP4_FS_NIR_H
#define R300VK_DP4_FS_NIR_H

#include "compiler/nir/nir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the DP4 compute-as-raster fragment program as standalone NIR: sample
 * two 2D-sampler inputs at the fullscreen texcoord, dot the first `components`
 * channels (2, 3, or 4; 0 means 4), and encode the integer dot into RGBA8 as a
 * 3-byte little-endian value.  The caller runs screen->finalize_nir and
 * create_fs_state.
 *
 * Split out of r300vk_synthesize_dp4_fs so a build-time test can validate the
 * shader shape without a pipe_context -- in particular that the 2D sampler
 * receives a 2-component coordinate, the invariant nir_build_tex_struct
 * asserts and whose violation aborted every DP4 compute pipeline create on an
 * asserts-enabled build. */
nir_shader *r300vk_build_dp4_fs_nir(const nir_shader_compiler_options *opts,
                                    unsigned components);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_DP4_FS_NIR_H */
