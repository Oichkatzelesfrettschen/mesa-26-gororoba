/*
 * Copyright © 2014 Broadcom
 * SPDX-License-Identifier: MIT
 */

#ifndef NIR_TO_RC_H
#define NIR_TO_RC_H

#include <stdbool.h>
#include "compiler/nir/nir.h"
#include "r300_shader_code.h"
#include "amd/r300/common/r300_shader_semantics.h"

#include "radeon_program_constants.h"

struct nir_shader;
struct r300_capabilities;
struct r300_fragment_program_external_state;
struct radeon_compiler;
union r300_shader_code {
   struct r300_fragment_shader_code *f;
   struct r300_vertex_shader_code *v;
};

void
nir_to_rc(struct nir_shader *s, const struct r300_capabilities *caps,
          struct r300_fragment_program_external_state state,
          union r300_shader_code rc, struct radeon_compiler *compiler);

void
ntr_fixup_varying_slots(struct nir_shader *s, nir_variable_mode mode);

/* Lowers texture projectors a TXP cannot carry (lod, offset, cube, or a
 * 3-coordinate compare), leaving the rest for the backend TXP opcode. */
void
nir_to_rc_lower_txp(struct nir_shader *s);

/* Packs coordinate/comparator/bias/lod/projector into the backend tex
 * source vec4s the RC TEX opcodes consume. */
bool
nir_to_rc_lower_tex(struct nir_shader *s);

/* Loads a driver-updated RC_CONSTANT_STATE vec4 during a NIR lowering;
 * the implementation dedups into its own state table and returns the
 * load_uniform marker whose base indexes that table. */
typedef struct nir_def *(*nir_to_rc_load_state_cb)(void *ctx,
                                                   struct nir_builder *b,
                                                   unsigned rc_state,
                                                   unsigned sampler,
                                                   unsigned num_components);

struct r300_fragment_program_external_state;

/* Rewrites texture coordinates for the sampler states the hardware cannot
 * honor natively: RECT normalization, NPOT wrap emulation, and the 3D
 * clamp-and-scale, each drawing factors through the state loader. */
bool
nir_to_rc_lower_backend_tex(struct nir_shader *s,
                            const struct r300_fragment_program_external_state *fs_state,
                            bool is_r500, nir_to_rc_load_state_cb load_state,
                            void *load_state_ctx);

/* Forces the alpha channel of every color output to one, including
 * FRAG_RESULT_COLOR and partial stores. */
bool
r300_nir_lower_alpha_to_one(struct nir_shader *s);

#endif /* NIR_TO_RC_H */
