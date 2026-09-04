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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r300_vs.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "amd/r300/compiler/nir_to_rc.h"
#include "amd/r300/compiler/r300_nir.h"

#include "draw/draw_context.h"
#include "draw/draw_vs.h"

static void
r300_draw_reject_vertex_shader(nir_shader *nir,
                               struct r300_vertex_shader *vs,
                               const char *reason)
{
    fprintf(stderr, "r300: %s; rejecting SW-TCL vertex shader\n", reason);
    ralloc_free(nir);
    vs->draw_vs = NULL;

    if (vs->shader) {
        vs->shader->dummy = true;
        free(vs->shader->error);
        vs->shader->error = strdup(reason);
    }
}

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

/* nir_lower_int_to_float converts ALU instructions and constants but
 * leaves intrinsics alone, so an integer system value (gl_InstanceID
 * as load_instance_id) enters the float-domain shader as raw integer
 * bits.  Every converted consumer -- float ALU, and the load_ubo
 * offset rebuild that applies f2i32 before direct Draw execution --
 * then misreads those bits as a float: f2i32 of the denormal bit
 * pattern of integer 1 yields 0, so Pos[gl_InstanceID] collapses to
 * Pos[0] for every instance (piglit arb_draw_instanced-drawarrays
 * draws all instances at the instance-0 offset).  Encode the system
 * value as float at its definition so the float-domain model holds
 * for the whole chain. */
static bool
r300_nir_float_encode_int_sysvals(nir_shader *nir)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    nir_builder b = nir_builder_create(impl);
    bool progress = false;

    nir_foreach_block (block, impl) {
        nir_foreach_instr_safe (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_instance_id:
            case nir_intrinsic_load_vertex_id:
            case nir_intrinsic_load_vertex_id_zero_base:
            case nir_intrinsic_load_base_vertex:
            case nir_intrinsic_load_first_vertex:
            case nir_intrinsic_load_base_instance:
            case nir_intrinsic_load_draw_id:
                break;
            default:
                continue;
            }
            b.cursor = nir_after_instr(instr);
            nir_def *as_float = nir_i2f32(&b, &intr->def);
            nir_def_rewrite_uses_after(&intr->def, as_float);
            progress = true;
        }
    }

    return nir_progress(progress, impl, nir_metadata_control_flow);
}

/* Default (non-R3V_NATIVE_VERTEXID) VertexIndex/InstanceIndex delivery is an
 * R32_FLOAT synthetic stream (r3v_build_velems_cso +
 * r3v_bind_synthetic_identity_stream) read as a float shader_in
 * (make_sysval_input).  The load_deref already carries a genuine float
 * encoding of the index (2 written as 2.0f), so ordering compares against
 * numeric thresholds work without a use-site rewrite.
 *
 * Equality with an integer-typed vertex attribute still needs both sides in
 * the same domain after nir_lower_int_to_float.  Production r3v equality cases
 * that compared raw int bit patterns under the legacy R32_SINT synthetic
 * delivery relied on bit-identity of the index; with R32_FLOAT that contract
 * is intentionally numeric-float equality (attribute side must also be
 * float-encoded or compared as float).  The use-site i2f32 rewrite below
 * remains for int-typed synthetic inputs (unit tests and any non-r3v caller
 * still presenting the legacy shape); float-typed inputs skip it so a genuine
 * 2.0f is never re-encoded as i2f32. */
bool
r300_nir_float_encode_synthetic_sysval_index_uses(nir_shader *nir)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    nir_builder b = nir_builder_create(impl);
    bool progress = false;

    nir_foreach_block_safe (block, impl) {
        nir_foreach_instr_safe (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_deref)
                continue;

            nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
            if (!deref || !nir_deref_mode_is(deref, nir_var_shader_in))
                continue;
            /* Ordinary SPIR-V vertex-attribute variables carry no OpName in
             * these dEQP-VK shaders, so var->name is NULL except the two
             * synthetic sysval inputs make_sysval_input names explicitly. */
            nir_variable *var = nir_deref_instr_get_variable(deref);
            if (!var || !var->name ||
                (strcmp(var->name, "sys_vertex_index") != 0 &&
                 strcmp(var->name, "sys_instance_index") != 0))
                continue;

            /* Float-typed synthetic inputs already carry numeric values. */
            if (glsl_type_is_float(var->type) ||
                glsl_type_is_float_16_32_64(var->type))
                continue;

            /* Int-typed legacy shape: redirect every consumer except raw-bit
             * feq/fneu to a single numeric i2f32 clone.  Read-only walk first:
             * nir_i2f32 consumes intr->def, so building the clone while walking
             * that use list would insert a use into the list under iteration. */
            bool needs_numeric = false;
            nir_foreach_use (use, &intr->def) {
                nir_instr *user = nir_src_use_instr(use);
                if (user->type != nir_instr_type_alu)
                    continue;
                nir_alu_instr *alu = nir_instr_as_alu(user);
                if (alu->op != nir_op_feq && alu->op != nir_op_fneu) {
                    needs_numeric = true;
                    break;
                }
            }
            if (!needs_numeric)
                continue;

            b.cursor = nir_after_instr(instr);
            nir_def *numeric = nir_i2f32(&b, &intr->def);
            nir_instr *numeric_instr = nir_def_instr(numeric);

            nir_foreach_use_safe (use, &intr->def) {
                nir_instr *user = nir_src_use_instr(use);
                if (user == numeric_instr)
                    continue;
                if (user->type != nir_instr_type_alu)
                    continue;
                nir_alu_instr *alu = nir_instr_as_alu(user);
                if (alu->op == nir_op_feq || alu->op == nir_op_fneu)
                    continue;
                nir_src_rewrite(use, numeric);
                progress = true;
            }
        }
    }

    return nir_progress(progress, impl, nir_metadata_control_flow);
}

/* nir_lower_int_to_float float-encodes integer-typed constants (its
 * nir_gather_types walk includes intrinsic sources, so a load_ubo_vec4
 * slot offset 1 becomes the bits of 1.0f), and consumers in that world
 * decode them heuristically -- nir_to_rc's ntr_src_as_uint treats any
 * value >= fui(1.0) as an encoded float.  The direct Draw executor uses
 * integer byte and vec4 offsets, so re-encode the offsets before handoff. */
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
                if (!nir_src_is_const(intr->src[si])) {
                    /* A dynamic offset chain lives in the float domain after
                     * lowering.  The direct executor consumes integer offset
                     * bits, so 1.0f would address slot 1065353216. */
                    b.cursor = nir_before_instr(instr);
                    nir_src_rewrite(&intr->src[si],
                                    nir_f2i32(&b, intr->src[si].ssa));
                    progress = true;
                    continue;
                }
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
 * the direct executor requires a one-bit structured condition. */
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

    /* RS485M has no hardware TCL, so the vertex shader runs in the direct Draw
     * NIR executor.  Lower indirect temporary derefs to if-ladders because the
     * executor admits SSA, ALU, control flow, and its explicit I/O intrinsics. */
    NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
             nir_var_function_temp, UINT32_MAX);

    /* Normalize dynamically indexed vertex-input arrays to the same if-ladder
     * form used for temporary arrays before the direct-executor admission gate.
     * The GL state tracker normally performs this lowering first. */
    NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
             nir_var_shader_in, UINT32_MAX);

    /* A structured SPIR-V loop with a break nested inside its body keeps a
     * bool function_temp variable (loop_break/loop_continue) live across
     * blocks instead of an SSA phi.  nir_lower_bool_to_float rewrites the
     * false/true constants stored through that deref to 32-bit floats but
     * leaves the variable's glsl_type at bool (1-bit).  Converting eligible
     * variables to SSA before the bool/int-to-float rewrites preserves phi
     * bit sizes and removes derefs the direct executor does not admit. */
    NIR_PASS(_, nir, nir_lower_vars_to_ssa);

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
     * This runs after the indirect-deref lowerings because those build
     * integer comparison ladders of their own. */

    /* GL SW-TCL (GLSL 1.20 / ES 1.00) has no bitwise operators, but this
     * same draw path also lowers r3v's Vulkan SPIR-V, which does.  An
     * unsigned shift or bitwise op then reaches nir_lower_int_to_float,
     * whose default arm asserts its operand is not integer-typed and
     * aborts the process (nir_lower_int_to_float.c lower_alu_instr, a
     * nir_op_ushr abort on the native VertexIndex/InstanceIndex path).
     * Run the fragment path's r300_nir_lower_bitwise_to_arith first: it
     * rewrites the FP24-exact idioms (constant unsigned shift, low-bit
     * mask) to udiv/umod.  Ops outside that set set out_unsupported; the
     * draw path rejects the shader rather than silently dropping them. */
    bool vs_bitwise_unsupported = false;
    NIR_PASS(_, nir, r300_nir_lower_bitwise_to_arith, &vs_bitwise_unsupported);
    if (vs_bitwise_unsupported) {
        r300_draw_reject_vertex_shader(
            nir, vs,
            "SW-TCL VS uses bitwise ops outside the FP24-exact rewrite set");
        return;
    }
    NIR_PASS(_, nir, nir_lower_int_to_float);
    NIR_PASS(_, nir, r300_nir_float_encode_int_sysvals);
    NIR_PASS(_, nir, r300_nir_float_encode_synthetic_sysval_index_uses);
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
    if (!draw_vs_nir_supported(&new_vs)) {
        r300_draw_reject_vertex_shader(
            nir, vs, "SW-TCL VS exceeds direct Draw NIR executor coverage");
        return;
    }
    vs->draw_vs = draw_create_vertex_shader(r300->draw, &new_vs);
}
