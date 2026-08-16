/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 *                Joakim Sindholt <opensource@zhasha.com>
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_FS_H
#define R300_FS_H

#include "pipe/p_state.h"
#include "compiler/r300_shader_code.h"
#include "r300_shader_semantics.h"

struct r300_context;


struct r300_fragment_shader {
    /* Parent class */
    struct pipe_shader_state state;

    /* Currently-bound fragment shader. */
    struct r300_fragment_shader_code* shader;

    /* List of the same shaders compiled with different texture-compare
     * states. */
    struct r300_fragment_shader_code* first;

    /* The state tracker's inlinable-uniform dword offsets, snapshotted at
     * create time before any driver NIR pass mutates the shader info.  The
     * values r300_set_inlinable_constants receives are ordered by THIS
     * array (st_atom_constbuf reads its own program info); the variant
     * clone's re-gathered info can differ in count and order, so the
     * inline pairs values with clone offsets by offset identity, never by
     * position. */
    uint16_t st_inlinable_offsets[MAX_INLINABLE_UNIFORMS];
    unsigned st_num_inlinable;

    /* SWTCL (!has_tcl) only: the gallium draw module's copy of this shader.
     * The wide-point stage reads it to find the gl_PointCoord input and
     * generate sprite texcoords for SW-expanded points. NULL on HW-TCL chips. */
    void* draw_fs;

    /* Sampler unit the polygon-stipple variant reads the stipple texture
     * from: the first unit past the program's own sampler bindings, computed
     * from the base NIR at create time with the same walk
     * nir_lower_pstipple_fs uses, so the texture bind and the variant
     * compile agree without the variant existing yet. */
    unsigned pstipple_sampler_unit;

    /* Fragment-input delivery contract, set at create time.  An R2VB re-staged
     * vertex producer records R300_FS_INPUT_R2VB_FLAT_VERTEX so every variant
     * translate skips the f2i/f2u interpolation epsilon; an ordinary fragment
     * shader keeps the interpolated default. */
    enum r300_fs_input_semantics input_semantics;
};

/* Verdict of a throwaway compile measuring a fragment program against the
 * real backend budgets (emit_alu's max_alu_insts ceiling, the temp file, the
 * const file), separating the one failure the multipass/spill escapes can
 * recover -- the ALU emit ceiling -- from every other reject. */
enum r300_fs_admission {
    R300_FS_ADMIT_FITS = 0,
    R300_FS_ADMIT_OVER_ALU_BUDGET,
    R300_FS_ADMIT_REJECT,
};

/* Backend resource vector of one admission measurement: emitted instruction
 * slots, the US_PIXSIZE temporary high-water mark, and the constant-file vec4
 * count.  Zero when the compile died before emission. */
struct r300_fs_admission_cost {
   unsigned alu;
   unsigned temps;
   unsigned consts;
};

enum r300_fs_admission
r300_fs_measure_nir_admission(struct r300_context *r300, struct nir_shader *fs_nir,
                              unsigned *out_alu_len,
                              enum r300_fs_input_semantics input_semantics,
                              struct r300_fs_admission_cost *out_cost);

/* Emitted-program snapshot of one FITS admission measurement.  The R300
 * (non-r500) code block is a flat pure function of the input program --
 * every field derives from the emitted instruction stream and none holds a
 * pointer or address -- so the struct copy is a complete snapshot and two
 * snapshots of the same program compare byte-equal.  The constant list is
 * deep-copied because rc_constant_list owns heap storage the probe reset
 * frees. */
struct r300_fs_admission_program {
   struct r300_fragment_program_code code;
   struct rc_constant *constants;
   unsigned num_constants;
};

/* Measure like r300_fs_measure_nir_admission and additionally snapshot the
 * emitted program on a FITS verdict, for identity comparison against a CSO
 * compile of the same NIR.  R300-class (non-r500) screens only.  The caller
 * releases the snapshot with r300_fs_admission_program_release; a snapshot
 * allocation failure reports R300_FS_ADMIT_REJECT like every other
 * infrastructure failure in the measurement. */
enum r300_fs_admission
r300_fs_measure_nir_admission_program(struct r300_context *r300,
                                      struct nir_shader *fs_nir,
                                      unsigned *out_alu_len,
                                      enum r300_fs_input_semantics input_semantics,
                                      struct r300_fs_admission_cost *out_cost,
                                      struct r300_fs_admission_program *out_program);

void r300_fs_admission_program_release(struct r300_fs_admission_program *program);

/* Create a fragment-shader CSO with an explicit input-delivery contract.  The
 * public create_fs_state pipe callback wraps this with
 * R300_FS_INPUT_INTERPOLATED; an R2VB re-staged producer passes
 * R300_FS_INPUT_R2VB_FLAT_VERTEX so its flat generated inputs skip the f2i/f2u
 * interpolation epsilon. */
void *r300_create_fs_state_internal(struct pipe_context *pipe,
                                    const struct pipe_shader_state *shader,
                                    enum r300_fs_input_semantics input_semantics);

/* Bake the compiled fragment-program code into the shader's cb_code PM4
 * register-write stream.  Reads only screen caps (is_r500, is_r400,
 * hb_r400_us) and options.ieeemath from the context, so a compiler tool can
 * drive it with a zeroed context naming a fake screen. */
void r300_emit_fs_code_to_buffer(struct r300_context *r300,
                                 struct r300_fragment_shader_code *shader);

/* Return TRUE if the shader was switched and should be re-emitted. */
bool r300_pick_fragment_shader(struct r300_context *r300,
                               struct r300_fragment_shader* fs,
                               struct r300_fragment_program_external_state *state);
void r300_fragment_program_get_external_state(struct r300_context *r300,
                                              struct r300_fragment_program_external_state *state);

static inline bool r300_fragment_shader_writes_depth(struct r300_fragment_shader *fs)
{
    if (!fs)
        return false;
    return (fs->shader->code.writes_depth) ? true : false;
}

static inline bool r300_fragment_shader_writes_all(struct r300_fragment_shader *fs)
{
    if (!fs)
        return false;
    return (fs->shader->write_all) ? true : false;
}
#endif /* R300_FS_H */
