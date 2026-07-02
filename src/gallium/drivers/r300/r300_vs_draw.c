/*
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * Copyright 2026 Pavel Ondračka <pavel.ondracka@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/* Vertex shader setup for the swtcl (draw-based) path.
 *
 * NIR transforms applied before handing to the draw module:
 * 1) Secondary color output requires primary color — insert zero primary if absent.
 * 2) Any back-face color requires all 4 color outputs — insert zeros for missing ones.
 * 3) Append a generic output containing a copy of gl_Position, used as WPOS
 *    by the hardware fragment shader.
 */

#include "r300_vs.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "compiler/nir_to_rc.h"
#include "compiler/r300_nir.h"

#include "draw/draw_context.h"

static nir_variable *
r300_draw_find_shader_out(nir_shader *nir, unsigned location)
{
    nir_foreach_shader_out_variable(var, nir) {
        if (var->data.location == location)
            return var;
    }
    return NULL;
}

/* Add a missing output variable and write zeros to it. */
static void
r300_draw_add_zero_output(nir_shader *nir, nir_builder *b, unsigned location,
                          const char *name)
{
    nir_variable *var = nir_variable_create(nir, nir_var_shader_out,
                                            glsl_vec4_type(), name);
    var->data.location = location;
    var->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
    nir_store_var(b, var, nir_imm_zero(b, 4, 32), 0xf);
}

/* Ensure that the color output layout satisfies the r300 hardware rules:
 *   - COL1 (secondary front color) requires COL0
 *   - Any back-face color requires all four color outputs (COL0/COL1/BFC0/BFC1) */
static bool
r300_nir_add_missing_color_outputs(nir_shader *nir)
{
    bool color_used[2] = {false, false};
    bool bcolor_used[2] = {false, false};

    nir_foreach_shader_out_variable(var, nir) {
        switch (var->data.location) {
        case VARYING_SLOT_COL0: color_used[0] = true; break;
        case VARYING_SLOT_COL1: color_used[1] = true; break;
        case VARYING_SLOT_BFC0: bcolor_used[0] = true; break;
        case VARYING_SLOT_BFC1: bcolor_used[1] = true; break;
        default: break;
        }
    }

    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    nir_builder b = nir_builder_create(impl);
    b.cursor = nir_after_impl(impl);
    bool progress = false;

    if (color_used[1] && !color_used[0]) {
        r300_draw_add_zero_output(nir, &b, VARYING_SLOT_COL0, "gl_FrontColor");
        color_used[0] = true;
        progress = true;
    }

    if (bcolor_used[0] || bcolor_used[1]) {
        if (!color_used[0]) {
            r300_draw_add_zero_output(nir, &b, VARYING_SLOT_COL0, "gl_FrontColor");
            progress = true;
        }
        if (!color_used[1]) {
            r300_draw_add_zero_output(nir, &b, VARYING_SLOT_COL1, "gl_FrontSecondaryColor");
            progress = true;
        }
        if (!bcolor_used[0]) {
            r300_draw_add_zero_output(nir, &b, VARYING_SLOT_BFC0, "gl_BackColor");
            progress = true;
        }
        if (!bcolor_used[1]) {
            r300_draw_add_zero_output(nir, &b, VARYING_SLOT_BFC1, "gl_BackSecondaryColor");
            progress = true;
        }
    }

    return nir_progress(progress, impl, nir_metadata_control_flow);
}

/* Assign driver_location to the variable at the given slot (if present)
 * and update the outputs field. */
static void
r300_draw_assign_output(nir_shader *nir, unsigned location, int *field,
                        unsigned *driver_location)
{
    nir_variable *var = r300_draw_find_shader_out(nir, location);
    if (!var)
        return;
    *field = *driver_location;
    var->data.driver_location = (*driver_location)++;
}

/* Build vs->outputs from the (transformed) NIR and assign driver_locations.
 * The WPOS output is identified by wpos_var and placed last. */
static void
r300_draw_fill_vs_outputs(nir_shader *nir, nir_variable *wpos_var,
                          struct r300_vertex_shader_code *vs)
{
    struct r300_shader_semantics *outputs = &vs->outputs;
    unsigned driver_location = 0;

    r300_shader_semantics_reset(outputs);

    r300_draw_assign_output(nir, VARYING_SLOT_POS, &outputs->pos, &driver_location);
    r300_draw_assign_output(nir, VARYING_SLOT_PSIZ, &outputs->psize, &driver_location);
    r300_draw_assign_output(nir, VARYING_SLOT_COL0, &outputs->color[0], &driver_location);
    r300_draw_assign_output(nir, VARYING_SLOT_COL1, &outputs->color[1], &driver_location);
    r300_draw_assign_output(nir, VARYING_SLOT_BFC0, &outputs->bcolor[0], &driver_location);
    r300_draw_assign_output(nir, VARYING_SLOT_BFC1, &outputs->bcolor[1], &driver_location);

    for (unsigned g = 0; g < ATTR_GENERIC_COUNT; g++) {
        unsigned loc = VARYING_SLOT_VAR0 + g;
        nir_variable *var = r300_draw_find_shader_out(nir, loc);
        if (!var || var == wpos_var)
            continue;
        outputs->generic[g] = driver_location;
        outputs->num_generic++;
        var->data.driver_location = driver_location++;
    }

    r300_draw_assign_output(nir, VARYING_SLOT_FOGC, &outputs->fog, &driver_location);

    /* Draw still needs these non-rasterized outputs in its shader info. */
    nir_variable *edge_var = r300_draw_find_shader_out(nir, VARYING_SLOT_EDGE);
    if (edge_var)
        edge_var->data.driver_location = driver_location++;
    nir_variable *clip_vertex_var =
        r300_draw_find_shader_out(nir, VARYING_SLOT_CLIP_VERTEX);
    if (clip_vertex_var)
        clip_vertex_var->data.driver_location = driver_location++;

    if (wpos_var) {
        outputs->wpos = driver_location;
        wpos_var->data.driver_location = driver_location;
    }
}

/* nir_lower_bool_to_float asserts on vector comparisons; scalarize the
 * bool-producing comparison ops (and the ball/bany reductions that hide
 * them) so the lowering sees only scalar compares. */
static bool
r300_draw_scalarize_bool_cmp_cb(const nir_instr *instr, const void *data)
{
    if (instr->type != nir_instr_type_alu)
        return false;
    const nir_alu_instr *alu = nir_instr_as_alu(instr);
    switch (alu->op) {
    case nir_op_flt:
    case nir_op_fge:
    case nir_op_feq:
    case nir_op_fneu:
        return alu->def.num_components > 1;
    case nir_op_ball_fequal2:
    case nir_op_ball_fequal3:
    case nir_op_ball_fequal4:
    case nir_op_bany_fnequal2:
    case nir_op_bany_fnequal3:
    case nir_op_bany_fnequal4:
    case nir_op_ball_iequal2:
    case nir_op_ball_iequal3:
    case nir_op_ball_iequal4:
    case nir_op_bany_inequal2:
    case nir_op_bany_inequal3:
    case nir_op_bany_inequal4:
        return true;
    default:
        return false;
    }
}

/* nir_lower_int_to_float float-encodes integer-typed constants (its
 * nir_gather_types walk includes intrinsic sources, so a load_ubo_vec4
 * slot offset 1 becomes the bits of 1.0f), and consumers in that world
 * decode them heuristically -- nir_to_rc's ntr_src_as_uint treats any
 * value >= fui(1.0) as an encoded float.  nir_to_tgsi inside draw does
 * no such decode, so re-encode the offsets as integers at the boundary
 * or every uniform read past slot 0 indexes garbage. */
static bool
r300_nir_decode_float_ubo_offsets(nir_shader *nir)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    nir_builder b = nir_builder_create(impl);
    bool progress = false;

    nir_foreach_block (block, impl) {
        nir_foreach_instr_safe (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_ubo &&
                intr->intrinsic != nir_intrinsic_load_ubo_vec4)
                continue;
            for (unsigned si = 0; si < 2; si++) {
                if (!nir_src_is_const(intr->src[si]))
                    continue;
                const uint32_t v = nir_src_as_uint(intr->src[si]);
                if (v < fui(1.0))
                    continue;
                b.cursor = nir_before_instr(instr);
                nir_src_rewrite(&intr->src[si],
                                nir_imm_int(&b, (int)uif(v)));
                progress = true;
            }
        }
    }

    return nir_progress(progress, impl, nir_metadata_control_flow);
}

/* Rebuild boolean branch conditions after bool-to-float lowering: the
 * lowering rewrites bool defs to 0.0/1.0 floats without touching nir_if
 * sources, so a float-typed condition reaches structured control flow and
 * later select flattening (nir_opt_peephole_select inside nir_to_tgsi)
 * builds a bcsel whose condition width fails nir_validate. */
static bool
r300_nir_fixup_float_if_conditions(nir_shader *nir)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    nir_builder b = nir_builder_create(impl);
    bool progress = false;

    nir_foreach_block (block, impl) {
        nir_if *nif = nir_block_get_following_if(block);
        if (!nif || nif->condition.ssa->bit_size == 1)
            continue;
        b.cursor = nir_before_cf_node(&nif->cf_node);
        nir_src_rewrite(&nif->condition,
                        nir_fneu_imm(&b, nif->condition.ssa, 0.0));
        progress = true;
    }

    return nir_progress(progress, impl, nir_metadata_control_flow);
}

void
r300_draw_init_vertex_shader(struct r300_context *r300,
                             struct r300_vertex_shader *vs)
{
    /* Clone the NIR and apply the +9 varying shift to align VS outputs with
     * FS inputs (which get the same shift in nir_to_rc). */
    nir_shader *nir = nir_shader_clone(NULL, vs->state.ir.nir);
    ntr_fixup_varying_slots(nir, nir_var_shader_out);

    NIR_PASS(_, nir, r300_nir_add_missing_color_outputs);
    nir_variable *wpos_var = NULL;
    NIR_PASS(_, nir, r300_nir_add_wpos, &wpos_var);

    /* RS480 has no hardware TCL, so the vertex shader runs in the gallium draw
     * module's TGSI interpreter.  A dynamic index into a temporary array makes
     * the interpreter walk a per-element index path that does not finish in
     * bounded time (dEQP-GLES2.functional.shaders.indexing.tmp_array.*_dyn hangs
     * the SW vertex shader in tgsi_exec).  Lower indirect temp derefs to
     * if-ladders so the interpreted VS only ever sees static register indices --
     * the fragment path already does this in nir_to_rc. */
    NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
             nir_var_function_temp, UINT32_MAX);

    /* A dynamically indexed vertex-input array (dEQP vertex_input.max_attributes
     * reads in_attr[i] across a loop) reaches the interpreted VS as an indirect
     * shader_in deref.  nir_to_tgsi only lowers indirect shader_in derefs for the
     * fragment stage; for every other stage its nir_lower_io converts the deref
     * to an indirect load_input that the later nir_lower_indirect_derefs cannot
     * touch, so the indirect input survives to emit and ntt leaves the selected
     * value in TGSI_FILE_NULL and aborts.  Lower it to the same if/else selection
     * trees here, before draw hands the shader to nir_to_tgsi, so the interpreter
     * only ever sees constant input indices.  The native GL path is unaffected:
     * st/mesa already lowers shader_in indirects up front, leaving nothing to do. */
    NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
             nir_var_shader_in, UINT32_MAX);

    /* Mesa stores GL integer uniforms converted to float
     * (uniform_int_float in uniform_query.cpp) because the fragment caps
     * make NativeIntegers false for the whole context, but the draw
     * module advertises integer support, so an int-preserving vertex
     * shader reads float bit patterns as integers: a uniform-bounded
     * loop sees 4.0f as 1082130432 and iterates for hours, and an ivec2
     * uniform converts to garbage
     * (dEQP-GLES2.functional.shaders.indexing.*dynamic_loop*_vertex,
     * shaders.linkage.uniform_struct_partial_ivec2_*).  Lower the
     * interpreted shader to the float domain -- the same
     * nir_lower_int_to_float + nir_lower_bool_to_float prep the HW-TCL
     * vertex path runs in nir_to_rc -- so consumption matches storage.
     * GLSL 1.20 and ES 1.00 have no bitwise operators, so nothing this
     * path receives needs the bitwise-to-arith guard.  This runs after
     * the indirect-deref lowerings because those build integer
     * comparison ladders of their own. */
    NIR_PASS(_, nir, nir_lower_int_to_float);
    NIR_PASS(_, nir, nir_opt_copy_prop);
    NIR_PASS(_, nir, nir_lower_alu_to_scalar, r300_draw_scalarize_bool_cmp_cb,
             NULL);
    NIR_PASS(_, nir, nir_lower_bool_to_float, false);
    /* nir_lower_bool_to_float converts instructions and phis but leaves
     * nir_if conditions alone -- sufficient for the flattened HW-TCL
     * vertex path, but the interpreted shader keeps its loops and
     * branches, so every condition needs the boolean rebuilt from the
     * 0.0/1.0 float. */
    NIR_PASS(_, nir, r300_nir_fixup_float_if_conditions);
    NIR_PASS(_, nir, r300_nir_decode_float_ubo_offsets);
    NIR_PASS(_, nir, nir_opt_copy_prop);
    NIR_PASS(_, nir, nir_opt_dce);

    /* Fill in the r300 rasterizer outputs and assign driver locations. */
    r300_draw_fill_vs_outputs(nir, wpos_var, vs->shader);

    /* Hand the transformed NIR off to the draw module. */
    struct pipe_shader_state new_vs = {
        .type = PIPE_SHADER_IR_NIR,
        .ir.nir = nir,
    };
    vs->draw_vs = draw_create_vertex_shader(r300->draw, &new_vs);
}
