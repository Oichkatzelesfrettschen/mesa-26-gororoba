/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_nir.h"

#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_atomic.h"

static uint32_t r300_r2vb_fail_position_source_clone;

void
r300_r2vb_test_fail_position_source_clone_once(void)
{
    p_atomic_set(&r300_r2vb_fail_position_source_clone, 1);
}

bool
r300_r2vb_output_store_location(const nir_intrinsic_instr *intr,
                                gl_varying_slot *location)
{
    if (!intr || !location)
        return false;

    switch (intr->intrinsic) {
    case nir_intrinsic_store_deref: {
        nir_variable *out = nir_intrinsic_get_var(intr, 0);
        if (!out || !(out->data.mode & nir_var_shader_out))
            return false;
        *location = (gl_varying_slot)out->data.location;
        return true;
    }
    case nir_intrinsic_store_output: {
        const nir_io_semantics semantics = nir_intrinsic_io_semantics(intr);
        if (!semantics.num_slots || !nir_src_is_const(intr->src[1]))
            return false;

        const uint64_t base = semantics.location;
        const uint64_t offset = nir_src_as_uint(intr->src[1]);
        if (offset >= semantics.num_slots || base >= VARYING_SLOT_MAX ||
            offset >= VARYING_SLOT_MAX - base)
            return false;

        *location = (gl_varying_slot)(base + offset);
        return true;
    }
    default:
        return false;
    }
}

bool
r300_r2vb_output_store_is_input_passthrough(
    const nir_intrinsic_instr *intr)
{
    if (!intr)
        return false;

    const nir_src *value;
    switch (intr->intrinsic) {
    case nir_intrinsic_store_deref:
        value = &intr->src[1];
        break;
    case nir_intrinsic_store_output:
        value = &intr->src[0];
        break;
    default:
        return false;
    }

    nir_intrinsic_instr *load = nir_src_as_intrinsic(*value);
    if (!load)
        return false;
    if (load->intrinsic == nir_intrinsic_load_input)
        return true;
    if (load->intrinsic != nir_intrinsic_load_deref)
        return false;

    nir_variable *src = nir_intrinsic_get_var(load, 0);
    return src && (src->data.mode & nir_var_shader_in);
}

void
r300_r2vb_prune_output_stores(nir_shader *nir, gl_varying_slot target)
{
    if (!nir)
        return;

    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    if (!impl)
        return;

    nir_foreach_block(block, impl) {
        nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            gl_varying_slot location;
            if (r300_r2vb_output_store_location(intr, &location) &&
                location != target)
                nir_instr_remove(instr);
        }
    }
}

void
r300_r2vb_prune_position_only(nir_shader *nir)
{
    if (!nir)
        return;

    r300_r2vb_prune_output_stores(nir, VARYING_SLOT_POS);

    bool progress;
    do {
        progress = nir_opt_dce(nir);
    } while (progress);
    nir_remove_dead_variables(nir, nir_var_shader_in | nir_var_shader_out |
                                      nir_var_mem_push_const,
                              NULL);
}

static int
r300_r2vb_input_order_rank(nir_shader *vs_nir, const nir_variable *target)
{
    if (!vs_nir || !target)
        return -1;

    unsigned target_index = 0;
    unsigned index = 0;
    bool found = false;
    nir_foreach_variable_with_modes(var, vs_nir, nir_var_shader_in) {
        if (var == target) {
            target_index = index;
            found = true;
            break;
        }
        index++;
    }
    if (!found)
        return -1;

    unsigned rank = 0;
    index = 0;
    nir_foreach_variable_with_modes(var, vs_nir, nir_var_shader_in) {
        if (var != target &&
            (var->data.location < target->data.location ||
             (var->data.location == target->data.location &&
              (var->data.location_frac < target->data.location_frac ||
               (var->data.location_frac == target->data.location_frac &&
                index < target_index)))))
            rank++;
        index++;
    }
    return (int)rank;
}

static int
r300_r2vb_input_order_rank_by_identity(nir_shader *vs_nir,
                                       unsigned driver_location,
                                       gl_varying_slot location,
                                       unsigned location_frac)
{
    nir_variable *match = NULL;
    nir_foreach_variable_with_modes(var, vs_nir, nir_var_shader_in) {
        if (var->data.driver_location != driver_location ||
            var->data.location != location ||
            var->data.location_frac != location_frac)
            continue;
        if (match)
            return -1;
        match = var;
    }
    return match ? r300_r2vb_input_order_rank(vs_nir, match) : -1;
}

/* The producer restage owns the survivor set: r300_r2vb_nir_restage_vs_as_fs
 * removes non-position stores, runs DCE, and compacts the inputs before the
 * fragment pass is compiled.  Telemetry uses the same clone reduction so
 * every physical application input feeding gl_Position keeps its measured
 * identity.  Symbol discovery uses (rg --fixed-strings
 * r300_r2vb_nir_restage_vs_as_fs src/amd/r300/compiler/). */
unsigned
r300_r2vb_position_source_scan_list_status(
    nir_shader *vs_nir, struct r300_r2vb_position_source *out,
    unsigned max_sources, bool *transient_failure)
{
    if (transient_failure)
        *transient_failure = false;
    if (!vs_nir || !out || max_sources == 0)
        return 0;
    memset(out, 0, (size_t)max_sources * sizeof(*out));
    if (p_atomic_xchg(&r300_r2vb_fail_position_source_clone, 0)) {
        if (transient_failure)
            *transient_failure = true;
        return 0;
    }
    nir_shader *tmp = nir_shader_clone(NULL, vs_nir);
    if (!tmp) {
        if (transient_failure)
            *transient_failure = true;
        return 0;
    }
    /* The survivors are exactly the inputs feeding gl_Position (the
     * count_position_inputs reduction, retained here as an identity). */
    r300_r2vb_prune_position_only(tmp);
    unsigned count = 0;
    bool overflow = false;
    nir_foreach_variable_with_modes(var, tmp, nir_var_shader_in) {
        if (count == max_sources) {
            overflow = true;
            break;
        }
        int rank = r300_r2vb_input_order_rank_by_identity(
            vs_nir, var->data.driver_location, var->data.location,
            var->data.location_frac);
        if (var->data.driver_location > 255 || rank < 0 || rank > 255) {
            ralloc_free(tmp);
            return 0;
        }
        out[count++] = (struct r300_r2vb_position_source) {
            .app_driver_location = var->data.driver_location,
            .location_rank = (uint8_t)rank,
            .valid = true,
        };
    }
    ralloc_free(tmp);
    if (overflow)
        return 0;

    /* The producer feeds VAR0+a in rank order.  Keep telemetry in the same
     * order so each record names the element consumed by that producer slot. */
    for (unsigned i = 1; i < count; i++) {
        struct r300_r2vb_position_source value = out[i];
        unsigned j = i;
        while (j > 0 && out[j - 1].location_rank > value.location_rank) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = value;
    }
    return count;
}

bool r300_r2vb_position_source_scan_status(
    nir_shader *vs_nir, struct r300_r2vb_position_source *out,
    bool *transient_failure)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out)
        return false;
    unsigned count = r300_r2vb_position_source_scan_list_status(
        vs_nir, out, 1, transient_failure);
    if (count != 1)
        memset(out, 0, sizeof(*out));
    return count == 1;
}

bool r300_r2vb_position_source_scan(nir_shader *vs_nir,
                                    struct r300_r2vb_position_source *out)
{
    return r300_r2vb_position_source_scan_status(vs_nir, out, NULL);
}

bool r300_r2vb_varying_source_scan(nir_shader *vs_nir, int slot,
                                   struct r300_r2vb_position_source *out)
{
    memset(out, 0, sizeof(*out));
    if (slot < 0)
        return false;
    nir_shader *tmp = nir_shader_clone(NULL, vs_nir);
    if (!tmp)
        return false;
    /* Strip every store except the target varying's, DCE, and drop dead
     * inputs: the survivors are exactly the inputs feeding the varying.
     * The single-model-stream BO-fetch producer feeds one attribute, so
     * exactly one survivor is admissible. */
    nir_function_impl *impl = nir_shader_get_entrypoint(tmp);
    nir_foreach_block(block, impl) {
        nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref &&
                intr->intrinsic != nir_intrinsic_store_output)
                continue;
            gl_varying_slot location;
            if (r300_r2vb_output_store_location(intr, &location) &&
                (int)location != slot)
                nir_instr_remove(instr);
        }
    }
    nir_opt_dce(tmp);
    nir_remove_dead_variables(tmp, nir_var_shader_in, NULL);
    unsigned n = 0;
    int surviving_location = -1;
    unsigned surviving_fraction = 0;
    unsigned driver_location = 0;
    nir_foreach_variable_with_modes(var, tmp, nir_var_shader_in) {
        n++;
        surviving_location = var->data.location;
        surviving_fraction = var->data.location_frac;
        driver_location = var->data.driver_location;
    }
    ralloc_free(tmp);
    if (n != 1)
        return false;
    int rank = r300_r2vb_input_order_rank_by_identity(
        vs_nir, driver_location, (gl_varying_slot)surviving_location,
        surviving_fraction);
    if (driver_location > 255 || rank < 0 || rank > 255)
        return false;
    out->app_driver_location = driver_location;
    out->location_rank = (uint8_t)rank;
    out->valid = true;
    return true;
}

/* Build the producer position output from the raw clip-space vec4, shared by
 * every transform-FS builder so the divide is a property of the producer
 * output contract, not of one FS-construction variant.  divide_to_window
 * clear: the raw clip-space vec4 (the demonstrated MVP path).  Set:
 * perspective divide then viewport, window.xyz = (clip.xyz / w_clip) *
 * viewport_scale + viewport_bias with w = 1, reproducing the contract
 * r2vb_verify_window_readback checks.  The reciprocal is a native alpha-pipe
 * RCP co-issued with the transform DP4s and the three viewport terms are
 * native MADs, so the divide adds no slot pair over the bare transform.
 * Geometric clip precedes this in the collineation domain, so w_clip > 0 in
 * normal use; the 1/32768 guard bounds the FP24 reciprocal defensively. */
nir_def *
r300_r2vb_nir_divide_position(nir_builder *b, nir_def *pos,
                              const struct r300_r2vb_restage_viewport *vp)
{
    if (!vp || !vp->divide_to_window)
        return pos;
    nir_def *comp[4];
    for (unsigned i = 0; i < 4; i++)
        comp[i] = nir_channel(b, pos, i);
    nir_def *w = comp[3];
    nir_def *guard = nir_imm_float(b, 1.0f / 32768.0f);
    nir_def *rcp_w = nir_bcsel(b, nir_flt(b, nir_fabs(b, w), guard),
                               nir_imm_float(b, 0.0f), nir_frcp(b, w));
    nir_def *win[3];
    for (unsigned i = 0; i < 3; i++) {
        /* NDC * scale + bias as separate fmul + fadd; nir_to_rc has no ffma
         * opcode and fuses the multiply-add into the native MAD itself. */
        nir_def *ndc = nir_fmul(b, comp[i], rcp_w);
        win[i] = nir_fadd(b, nir_fmul(b, ndc, nir_imm_float(b, vp->scale[i])),
                          nir_imm_float(b, vp->translate[i]));
    }
    return nir_vec4(b, win[0], win[1], win[2], nir_imm_float(b, 1.0f));
}

/* Re-stage the bound vertex shader as the producer fragment shader (position
 * only): clone the VS NIR, keep its arithmetic verbatim, and remap only the I/O
 * semantics so the fragment ALU runs it.  Each VS attribute input becomes a flat
 * fragment input at VARYING_SLOT_VAR0+i (the producer feeds one attribute per
 * output slot, flat); gl_Position (VARYING_SLOT_POS) becomes FRAG_RESULT_DATA0
 * (the clip BO).  Stores to any other (varying) output are dropped -- an
 * MVP-class VS passes its varyings through unchanged, so the per-output
 * re-ingest reads them from the application buffers -- and the inputs that fed
 * only those stores die in DCE.  The VS reads its matrix from UBO[0]; nir_to_rc
 * sizes the const file from that block-0 interface, so the caller loads FS
 * const file 0 with the matrix the VS expects (untransposed -- the VS body is
 * column-MAD, reading the matrix columns via load_ubo_vec4, not the
 * DP4-transposed rows).
 *
 * This derives the producer from the real shader instead of hand-building the
 * 4-DP4 transform: the arithmetic, the constant reads, and the output count all
 * come from the VS, and nir_to_rc's stage-aware compile (it lowers I/O and emits
 * both VS and FS varyings) does the rest.  Returns the derived FS NIR (caller
 * owns) or NULL; the caller wraps it into its FS object, and the admission
 * oracle compiles the same NIR throwaway to measure emitted slots, so the
 * program the oracle admits is the program the producer runs. */
nir_shader *
r300_r2vb_nir_restage_vs_as_fs(nir_shader *vs_nir, gl_varying_slot target,
                               const struct r300_r2vb_restage_viewport *vp)
{
    nir_shader *fs = nir_shader_clone(NULL, vs_nir);
    if (!fs)
        return NULL;
    fs->info.stage = MESA_SHADER_FRAGMENT;

    /* VS vertex attributes -> flat fragment inputs at VAR0 + location-rank.  The
     * producer feeds model attribute a (velem[a], the a-th input in location order)
     * to VAR0 + a, so the re-staged FS must read input a there.  Rank by location,
     * NOT NIR list order: a multi-input VS whose variable list is not in location
     * order (e.g. the quaternion declared before the position) would otherwise read
     * its inputs swapped -- the producer feeds inPos to VAR0 but the FS, indexed by
     * list order, reads VAR0 as the other input.  Single-input shapes are unaffected
     * (rank 0 either way).  Compute every rank from the original locations before
     * remapping any, so a remap does not perturb a later rank. */
    nir_variable *ins[R300_R2VB_NIR_MAX_INPUTS];
    unsigned orig_loc[R300_R2VB_NIR_MAX_INPUTS];
    unsigned orig_frac[R300_R2VB_NIR_MAX_INPUTS];
    unsigned n_in = 0;
    nir_foreach_variable_with_modes(var, fs, nir_var_shader_in)
        if (n_in < R300_R2VB_NIR_MAX_INPUTS) {
            orig_loc[n_in] = var->data.location;
            orig_frac[n_in] = var->data.location_frac;
            ins[n_in++] = var;
        }
    /* Rank by a TOTAL order -- location, then location_frac, then array index --
     * so two inputs that share a location (a packed/component-split attribute)
     * still get distinct ranks and distinct VAR slots, rather than aliasing onto
     * the same slot.  Use the snapshot, not ins[]->data.location, since the remap
     * loop below overwrites it. */
    unsigned rank[R300_R2VB_NIR_MAX_INPUTS];
    for (unsigned i = 0; i < n_in; i++) {
        rank[i] = 0;
        for (unsigned j = 0; j < n_in; j++)
            if (j != i &&
                (orig_loc[j] < orig_loc[i] ||
                 (orig_loc[j] == orig_loc[i] &&
                  (orig_frac[j] < orig_frac[i] ||
                   (orig_frac[j] == orig_frac[i] && j < i)))))
                rank[i]++;
    }
    for (unsigned i = 0; i < n_in; i++) {
        ins[i]->data.location = VARYING_SLOT_VAR0 + rank[i];
        ins[i]->data.location_frac = 0;
        ins[i]->data.interpolation = INTERP_MODE_FLAT;
    }

    /* The producer renders one output per pass, so keep only the store to the
     * target output and drop the rest.  Position pass: target = VARYING_SLOT_POS
     * (the dropped varyings come from the application buffers in the re-ingest).
     * Computed-varying pass: target = that varying's slot, and the now-unstored
     * position transform drops in DCE below, leaving just the varying arithmetic. */
    nir_function_impl *impl = nir_shader_get_entrypoint(fs);
    /* The planner removes both store_deref and lowered store_output forms;
     * share that target reduction so the compiled producer has the same
     * survivor set as the admission candidate. */
    r300_r2vb_prune_output_stores(fs, target);

    /* The cloned VS carries multiply-add as ffma/ffma_weak (the gallivm VS
     * compiler options keep them fused-weak), but nir_to_rc translates only
     * nir_op_fmad to the native MAD and errors on ffma variants.  Rewrite in
     * place -- same three float sources, same result shape. */
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
                continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op == nir_op_ffma || alu->op == nir_op_ffma_weak)
                alu->op = nir_op_fmad;
        }
    }

    nir_foreach_block(block, impl) {
        nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_output) {
                gl_varying_slot store_location;
                if (!r300_r2vb_output_store_location(intr,
                                                     &store_location) ||
                    store_location != target)
                    continue;
                nir_io_semantics semantics = nir_intrinsic_io_semantics(intr);
                semantics.location = FRAG_RESULT_DATA0;
                semantics.num_slots = 1;
                nir_intrinsic_set_io_semantics(intr, semantics);
                nir_builder rb = nir_builder_at(nir_before_instr(instr));
                nir_src_rewrite(&intr->src[1], nir_imm_int(&rb, 0));
                if (target != VARYING_SLOT_POS &&
                    intr->src[0].ssa->num_components < 4) {
                    /* A partial varying store pads to FP32x4 with zeros so
                     * the producer writes the complete delivery record. */
                    nir_builder pb = nir_builder_at(nir_before_instr(instr));
                    nir_def *src = intr->src[0].ssa;
                    nir_def *zero = nir_imm_float(&pb, 0.0f);
                    const unsigned original_write_mask =
                        nir_intrinsic_write_mask(intr);
                    unsigned component = nir_intrinsic_has_component(intr)
                                            ? nir_intrinsic_component(intr)
                                            : 0;
                    nir_def *comp[4];
                    for (unsigned k = 0; k < 4; k++) {
                        unsigned source_component = k >= component
                                                       ? k - component
                                                       : src->num_components;
                        bool source_written =
                            source_component < src->num_components &&
                            (original_write_mask &
                             BITFIELD_BIT(source_component));
                        comp[k] = source_written
                                      ? nir_channel(&pb, src, source_component)
                                      : zero;
                    }
                    nir_def *padded = nir_vec(&pb, comp, 4);
                    nir_src_rewrite(&intr->src[0], padded);
                    intr->num_components = 4;
                    nir_intrinsic_set_write_mask(intr, 0xf);
                    if (nir_intrinsic_has_component(intr))
                        nir_intrinsic_set_component(intr, 0);
                } else if (target == VARYING_SLOT_POS &&
                           intr->src[0].ssa->num_components == 4) {
                    /* Position output follows the same divide and viewport
                     * contract as the store_deref producer form. */
                    nir_builder wb = nir_builder_at(nir_before_instr(instr));
                    nir_def *win = r300_r2vb_nir_divide_position(
                        &wb, intr->src[0].ssa, vp);
                    if (win != intr->src[0].ssa)
                        nir_src_rewrite(&intr->src[0], win);
                }
                continue;
            }
            if (intr->intrinsic != nir_intrinsic_store_deref)
                continue;
            nir_variable *out = nir_intrinsic_get_var(intr, 0);
            if (out && (out->data.mode & nir_var_shader_out) &&
                out->data.location == target &&
                target != VARYING_SLOT_POS &&
                intr->src[1].ssa->num_components < 4) {
                /* A partial varying store (a scalar lighting varying is the
                 * dominant shape) pads to FP32x4 with zeros so the producer
                 * writes the complete BO record the FLOAT_4 delivery fetch
                 * reads; the FS consumes the components the GLSL varying
                 * defines. */
                nir_builder pb = nir_builder_at(nir_before_instr(instr));
                nir_def *src = intr->src[1].ssa;
                nir_def *zero = nir_imm_float(&pb, 0.0f);
                nir_def *comp[4];
                for (unsigned k = 0; k < 4; k++)
                    comp[k] = k < src->num_components
                                  ? nir_channel(&pb, src, k)
                                  : zero;
                nir_def *padded = nir_vec(&pb, comp, 4);
                out->type = glsl_vec4_type();
                nir_deref_instr *dv = nir_src_as_deref(intr->src[0]);
                dv->type = glsl_vec4_type();
                nir_src_rewrite(&intr->src[1], padded);
                intr->num_components = 4;
                nir_intrinsic_set_write_mask(intr, 0xf);
            } else if (out && (out->data.mode & nir_var_shader_out) &&
                       out->data.location == target &&
                       target == VARYING_SLOT_POS &&
                       intr->src[1].ssa->num_components == 4) {
                /* Position pass output contract: same divide + viewport as the
                 * built transform-FS producers, applied to the cloned VS's clip
                 * result so every producer variant emits window space under the
                 * divide gate. */
                nir_builder wb = nir_builder_at(nir_before_instr(instr));
                nir_def *win = r300_r2vb_nir_divide_position(
                    &wb, intr->src[1].ssa, vp);
                if (win != intr->src[1].ssa)
                    nir_src_rewrite(&intr->src[1], win);
            }
        }
    }

    /* Target output -> color0 (the producer BO this pass writes). */
    nir_foreach_variable_with_modes(var, fs, nir_var_shader_out) {
        if (var->data.location == target) {
            var->data.location = FRAG_RESULT_DATA0;
            var->data.location_frac = 0;
        }
    }

    /* DCE first, THEN drop dead variables.  Removing a store leaves its
     * deref_var (and the load feeding it) as dead instructions;
     * nir_remove_dead_variables counts a deref as a use, so running it before DCE
     * would keep the orphaned varying output -- which then reaches nir_to_rc as a
     * shader_out at a VARYING_SLOT location and trips its
     * `location < FRAG_RESULT_MAX` assert.  DCE clears the dead derefs, then the
     * unreferenced varying outputs, their inputs, and push-constants drop, and a
     * re-gather keeps num_ubos/inputs/outputs consistent for nir_to_rc. */
    nir_opt_dce(fs);
    nir_remove_dead_variables(fs, nir_var_shader_in | nir_var_shader_out |
                                      nir_var_mem_push_const, NULL);

    /* Varying-pass input compaction: the surviving inputs re-rank among
     * themselves onto VAR0.., so a varying computed from a non-first
     * application input (its position input dead after the store drop)
     * still presents the single-generic FS input contract the qualified
     * single-model-stream producer feeds.  The caller feeds the matching
     * attribute via r300_r2vb_varying_source_scan's rank.  The position
     * pass keeps the uncompacted rank-over-all mapping the multi-input
     * producer feeds. */
    if (target != VARYING_SLOT_POS) {
        nir_variable *sur[R300_R2VB_NIR_MAX_INPUTS];
        unsigned n_sur = 0;
        nir_foreach_variable_with_modes(var, fs, nir_var_shader_in)
            if (n_sur < R300_R2VB_NIR_MAX_INPUTS)
                sur[n_sur++] = var;
        for (unsigned i = 0; i < n_sur; i++) {
            unsigned crank = 0;
            for (unsigned j = 0; j < n_sur; j++)
                if (j != i && sur[j]->data.location < sur[i]->data.location)
                    crank++;
            sur[i]->data.location = VARYING_SLOT_VAR0 + crank;
            /* nir_to_rc sizes the input register file from the driver
             * location, so the compaction moves it with the slot; a
             * surviving second-rank input would otherwise report a
             * two-register file and fail the one-input producer FS
             * contract. */
            sur[i]->data.driver_location = crank;
        }
    }
    nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));

    if (getenv("R300_R2VB_VS_DUMP"))
        nir_print_shader(fs, stderr);

    return fs;
}
