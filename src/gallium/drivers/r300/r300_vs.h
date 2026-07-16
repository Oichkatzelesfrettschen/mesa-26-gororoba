/*
 * Copyright 2009 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_VS_H
#define R300_VS_H

#include "pipe/p_state.h"
#include "compiler/radeon_code.h"

#include "r300_context.h"
#include "r300_shader_semantics.h"

struct r300_context;

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

struct r300_vertex_shader {
    /* Parent class */
    struct pipe_shader_state state;

    /* Currently-bound vertex shader. */
    struct r300_vertex_shader_code *shader;

    /* List of the same shaders compiled with different states. */
    struct r300_vertex_shader_code *first;

    /* SWTCL-specific. */
    void *draw_vs;

    /* R2VB producer admission memo, indexed by
     * [allow_computed_varying][position_space].  CLIP and WINDOW producers
     * differ by the divide sequence, so a split/fits verdict for one space
     * does not transfer to the other.  0 = unmeasured, 1 = fits, 2 = reject,
     * 3 = split admitted. */
    uint8_t r2vb_admission[2][2];

    /* R2VB producer plan cache, same [allow_computed_varying][position_space]
     * key as the memo plus a per-slot viewport window key (the window
     * producer bakes viewport scale/translate as immediates).  Owned here;
     * released by r300_r2vb_plan_cache_release on delete. */
    struct r300_r2vb_producer_plan *r2vb_plan[2][2];
    uint32_t r2vb_plan_window_key[2][2];
};

void r300_translate_vertex_shader(struct r300_context *r300,
                                  struct r300_vertex_shader *vs);

void r300_draw_init_vertex_shader(struct r300_context *r300,
                                  struct r300_vertex_shader *vs);

#endif /* R300_VS_H */
