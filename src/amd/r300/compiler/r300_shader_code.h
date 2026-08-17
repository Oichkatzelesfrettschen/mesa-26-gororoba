/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 *                Joakim Sindholt <opensource@zhasha.com>
 * Copyright 2009 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/* The compiler's immutable output containers: one compiled fragment or
 * vertex program variant, its semantics, and its variant-key metadata.
 * Both front ends receive these from nir_to_rc and the rc backends; the
 * driver-side shader objects that own the variant lists stay in
 * r300_fs.h and r300_vs.h.
 */

#ifndef R300_SHADER_CODE_H
#define R300_SHADER_CODE_H

#include <stdbool.h>
#include <stdint.h>

#include "radeon_code.h"
#include "compiler/shader_info.h"
#include "r300_shader_semantics.h"

struct r300_fragment_shader_code {
    struct rX00_fragment_program_code code;
    struct r300_shader_semantics inputs;

    /* Whether the shader was replaced by a dummy one due to a shader
     * compilation failure. */
    bool dummy;

    /* Numbers of constants for each type. */
    unsigned externals_count;
    unsigned immediates_count;
    unsigned rc_state_count;

    /* Registers for fragment depth output setup. */
    uint32_t fg_depth_src;      /* R300_FG_DEPTH_SRC: 0x4bd8 */
    uint32_t us_out_w;          /* R300_US_W_FMT:     0x46b4 */

    struct r300_fragment_program_external_state compare_state;

    /* Default-block uniform values this variant was specialized with: a
     * uniform used as a loop bound or branch condition is inlined as a constant
     * so the loop can be statically unrolled (R300/R400 have no dynamic control
     * flow). Part of the variant key alongside compare_state; the values come
     * from r300_set_inlinable_constants at draw time. */
    uint32_t inlinable_values[MAX_INLINABLE_UNIFORMS];
    unsigned num_inlinable;

    /* The state tracker's offsets the values above are ordered by, copied
     * from the shader-level snapshot at variant-key time so translate can
     * pair values with the clone's re-gathered offsets by identity. */
    uint16_t st_inlinable_offsets[MAX_INLINABLE_UNIFORMS];
    unsigned st_num_inlinable;

    unsigned cb_code_size;
    uint32_t *cb_code;

    struct r300_fragment_shader_code* next;

    /* >64-ALU FS auto-partition (R300_FS_MULTIPASS): when this code is pass A
     * of a DAG split, multipass_pass_b holds the second pass, which reloads
     * the carry from multipass_num_scratch RGBA8 scratch render targets
     * (sampled as RECT at the fragment position, units 0..N-1) and finishes
     * the program.  NULL for an ordinary single-pass fragment shader.  Each
     * scratch carries two scalar components as FP24-exact hi/lo byte pairs. */
    struct r300_fragment_shader_code* multipass_pass_b;
    unsigned multipass_num_scratch;

    bool write_all;
    bool uses_discard;

    /* SWTCL analytic-derivative emulation (R300-class has no dFdx/dFdy
     * hardware). When the FS reads a screen-space derivative, the NIR lowering
     * pass rewrites dFdx/dFdy of one varying to read two synthesized GENERIC
     * inputs the draw module fills per triangle. These hold the differentiated
     * varying's GENERIC index and the two gradient GENERIC indices, or -1 when
     * the shader uses no derivatives / the part has native derivative HW. */
    int deriv_src_generic;
    int deriv_ddx_generic;
    int deriv_ddy_generic;

    /* Sampler unit nir_lower_pstipple_fs chose for this stipple variant
     * (UINT_MAX when the variant is not a stipple key). */
    unsigned pstipple_lowered_unit;

    /* Error message in case compilation failed. */
    char *error;
};

struct r300_vertex_shader_code {
    /* Parent class */

    unsigned num_inputs;
    struct r300_shader_semantics outputs;

    /* Whether the shader was replaced by a dummy one due to a shader
     * compilation failure. */
    bool dummy;

    bool wpos;

    /* Numbers of constants for each type. */
    unsigned externals_count;
    unsigned immediates_count;

    /* HWTCL-specific.  */
    /* Machine code (if translated) */
    struct r300_vertex_program_code code;

    struct r300_vertex_shader_code *next;

    /* Error message in case compilation failed. */
    char *error;
};

#endif /* R300_SHADER_CODE_H */
