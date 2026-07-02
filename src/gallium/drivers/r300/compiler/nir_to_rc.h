/*
 * Copyright © 2014 Broadcom
 * SPDX-License-Identifier: MIT
 */

#ifndef NIR_TO_RC_H
#define NIR_TO_RC_H

#include <stdbool.h>
#include "compiler/nir/nir.h"
#include "pipe/p_defines.h"
#include "r300_fs.h"
#include "r300_shader_semantics.h"
#include "r300_vs.h"

#include "compiler/radeon_program_constants.h"

struct nir_shader;
struct pipe_screen;
struct r300_fragment_program_external_state;
struct radeon_compiler;
union r300_shader_code {
   struct r300_fragment_shader_code *f;
   struct r300_vertex_shader_code *v;
};

void
nir_to_rc(struct nir_shader *s, struct pipe_screen *screen,
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

#endif /* NIR_TO_RC_H */
