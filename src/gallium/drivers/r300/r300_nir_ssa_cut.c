/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300_nir_ssa_cut.h"

#include "nir_builder.h"
#include "util/macros.h"

/* fneg/fabs fold into RC source modifiers and mov copy-propagates, so a
 * modifier tail over a carried value is free in pass B; carrying the base
 * under the tail lets the tail's instructions stay live in pass B at zero
 * ALU cost while the base's producers dead-code away. */
nir_def *
r300_mp_carry_base(nir_def *def, unsigned depth)
{
    nir_instr *instr = nir_def_instr(def);
    if (depth > 6 || instr->type != nir_instr_type_alu)
        return def;
    nir_alu_instr *alu = nir_instr_as_alu(instr);
    if (alu->op != nir_op_mov && alu->op != nir_op_fneg &&
        alu->op != nir_op_fabs)
        return def;
    return r300_mp_carry_base(alu->src[0].src.ssa, depth + 1);
}

/* Semantic type of a typeless SSA def, resolved through the producer chain:
 * a typed ALU op answers directly; mov/vecN/bcsel recurse into their value
 * sources; anything else (loads, constants) reports invalid and the caller
 * falls back to a consumer vote. */
nir_alu_type
r300_mp_producer_type(nir_def *def, unsigned depth)
{
    if (depth > 6)
        return nir_type_invalid;
    nir_instr *instr = nir_def_instr(def);
    if (instr->type != nir_instr_type_alu)
        return nir_type_invalid;
    nir_alu_instr *alu = nir_instr_as_alu(instr);
    nir_alu_type t =
        nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type);
    if (t == nir_type_float || t == nir_type_int || t == nir_type_uint ||
        t == nir_type_bool)
        return t;
    unsigned first = (alu->op == nir_op_bcsel) ? 1 : 0;
    for (unsigned i = first; i < nir_op_infos[alu->op].num_inputs; i++) {
        nir_alu_type st = r300_mp_producer_type(alu->src[i].src.ssa, depth + 1);
        if (st != nir_type_invalid)
            return st;
    }
    return nir_type_invalid;
}

/* Classify a carried def as float or integer for the pack round trip.  The
 * producer chain decides when it is typed; otherwise the consumers vote by
 * their nir_op_infos input types.  A def consumed both ways is ambiguous and
 * the partition defers rather than guess. */
bool
r300_mp_classify_carry(nir_def *def, enum r300_mp_carry_type *out)
{
    nir_alu_type t = r300_mp_producer_type(def, 0);
    if (t == nir_type_float) {
        *out = R300_MP_CARRY_FLOAT;
        return true;
    }
    if (t == nir_type_int || t == nir_type_uint || t == nir_type_bool) {
        *out = R300_MP_CARRY_INT;
        return true;
    }

    bool as_int = false, as_float = false;
    nir_foreach_use(use, def) {
        if (nir_src_is_if(use)) {
            as_int = true;
            continue;
        }
        nir_instr *ui = nir_src_use_instr(use);
        if (ui->type != nir_instr_type_alu)
            continue;
        nir_alu_instr *ualu = nir_instr_as_alu(ui);
        for (unsigned i = 0; i < nir_op_infos[ualu->op].num_inputs; i++) {
            if (&ualu->src[i].src != use)
                continue;
            nir_alu_type it =
                nir_alu_type_get_base_type(nir_op_infos[ualu->op].input_types[i]);
            if (it == nir_type_float)
                as_float = true;
            else if (it == nir_type_int || it == nir_type_uint ||
                     it == nir_type_bool)
                as_int = true;
            break;
        }
    }
    if (as_int && as_float)
        return false;
    *out = as_int ? R300_MP_CARRY_INT : R300_MP_CARRY_FLOAT;
    return true;
}

/* Collect the carry set for a candidate cut: every ALU or TEX def at or
 * before the cut with a use after it, reduced to its modifier-chain base and
 * deduplicated.  Loads and constants crossing the cut are free (pass B keeps
 * the instruction and re-reads the input).  Returns false when the carry
 * exceeds the scratch budget, a value's type is ambiguous, or nothing
 * crosses at all. */
bool
r300_mp_collect(nir_block *block, unsigned cut_index,
                struct r300_mp_partition *p)
{
    p->cut_index = cut_index;
    p->num_bases = 0;
    p->total_comps = 0;
    nir_foreach_instr(instr, block) {
        if (instr->index > cut_index)
            break;
        nir_def *def;
        if (instr->type == nir_instr_type_alu)
            def = &nir_instr_as_alu(instr)->def;
        else if (instr->type == nir_instr_type_tex)
            def = &nir_instr_as_tex(instr)->def;
        else
            continue;
        bool crosses = false;
        nir_foreach_use(use, def) {
            if (nir_src_is_if(use))
                continue;
            if (nir_src_use_instr(use)->index > cut_index) {
                crosses = true;
                break;
            }
        }
        if (!crosses)
            continue;
        nir_def *base = r300_mp_carry_base(def, 0);
        nir_instr *base_instr = nir_def_instr(base);
        if (base_instr->type != nir_instr_type_alu &&
            base_instr->type != nir_instr_type_tex)
            continue;
        bool dup = false;
        for (unsigned i = 0; i < p->num_bases; i++) {
            if (p->bases[i] == base) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (p->num_bases >= R300_MP_MAX_CARRY_COMPS ||
            p->total_comps + base->num_components > R300_MP_MAX_CARRY_COMPS)
            return false;
        if (base->bit_size != 32 && base->bit_size != 1)
            return false;
        enum r300_mp_carry_type ct;
        if (base->bit_size == 1)
            ct = R300_MP_CARRY_BOOL1;
        else if (base_instr->type == nir_instr_type_tex)
            ct = R300_MP_CARRY_FLOAT;
        else if (!r300_mp_classify_carry(base, &ct))
            return false;
        p->bases[p->num_bases] = base;
        p->base_type[p->num_bases] = ct;
        p->num_bases++;
        p->total_comps += base->num_components;
    }
    return p->num_bases > 0;
}

/* Scan every admissible cut position in a single-block program and rank the
 * candidates: smallest carry first, tie-broken toward a balanced ALU split.
 * Any cut whose two halves both compile under the emit ceiling is correct --
 * the scan only orders candidates, the compile decides -- so the caller
 * walks the ranked list until a candidate's halves both compile.  A serial
 * dependence chain may have only a narrow window of feasible cuts around
 * the balance point (each half pays the pack or unpack overhead on top of
 * its share), which is exactly what the multi-candidate walk recovers. */
unsigned
r300_mp_find_cuts(nir_shader *nir, struct r300_mp_partition *cands,
                  unsigned max_cands)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    if (!exec_list_is_singular(&impl->body))
        return 0;
    nir_block *block = nir_start_block(impl);

    unsigned idx = 0, total = 0;
    nir_foreach_instr(instr, block) {
        instr->index = idx++;
        if (instr->type == nir_instr_type_alu ||
            instr->type == nir_instr_type_tex)
            total++;
    }
    if (total < 16)
        return 0;

    unsigned num = 0;
    unsigned costs[R300_MP_MAX_CANDIDATES];
    unsigned raw = 0;
    nir_foreach_instr(instr, block) {
        if (instr->type != nir_instr_type_alu &&
            instr->type != nir_instr_type_tex)
            continue;
        raw++;
        /* A side below a quarter of the mass cannot relieve the other side
         * enough to matter; skip the degenerate edges. */
        if (raw * 4 < total || (total - raw) * 4 < total)
            continue;
        struct r300_mp_partition p;
        if (!r300_mp_collect(block, instr->index, &p))
            continue;
        unsigned balance = (raw * 2 > total) ? raw * 2 - total
                                             : total - raw * 2;
        unsigned cost = p.total_comps * 1024 + balance;
        /* Insertion sort into the ranked candidate list. */
        unsigned pos = num;
        while (pos > 0 && costs[pos - 1] > cost)
            pos--;
        if (pos >= max_cands)
            continue;
        unsigned last = MIN2(num, max_cands - 1);
        for (unsigned j = last; j > pos; j--) {
            cands[j] = cands[j - 1];
            costs[j] = costs[j - 1];
        }
        cands[pos] = p;
        costs[pos] = cost;
        if (num < max_cands)
            num++;
    }
    return num;
}

bool
r300_mp_find_vec4_cut(nir_shader *nir, struct r300_mp_partition *out)
{
    struct r300_mp_partition cands[R300_MP_MAX_CANDIDATES];
    unsigned n = r300_mp_find_cuts(nir, cands, R300_MP_MAX_CANDIDATES);
    /* find_cuts ranks smallest carry first, so the first candidate that fits one
     * FP32 vec4 is the narrowest carry available. */
    for (unsigned i = 0; i < n; i++)
        if (cands[i].total_comps <= 4) {
            *out = cands[i];
            return true;
        }
    return false;
}

/* Re-run the deterministic collection inside a clone.  nir_shader_clone keeps
 * instruction order, so the same cut index yields the clone's copies of the
 * carried bases; the shape must match the original decision or the split is
 * abandoned. */
static bool
mp_recollect(nir_shader *sh, const struct r300_mp_partition *want,
             struct r300_mp_partition *got)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(sh);
    if (!exec_list_is_singular(&impl->body))
        return false;
    nir_block *block = nir_start_block(impl);
    unsigned idx = 0;
    nir_foreach_instr(instr, block)
        instr->index = idx++;
    if (!r300_mp_collect(block, want->cut_index, got))
        return false;
    return got->num_bases == want->num_bases &&
           got->total_comps == want->total_comps;
}

nir_shader *
r300_mp_build_carry_pass_a(nir_shader *src, const struct r300_mp_partition *part)
{
    nir_shader *a = nir_shader_clone(NULL, src);
    if (!a)
        return NULL;
    struct r300_mp_partition p;
    if (!mp_recollect(a, part, &p)) {
        ralloc_free(a);
        return NULL;
    }

    nir_function_impl *impl = nir_shader_get_entrypoint(a);
    nir_block *block = nir_start_block(impl);

    /* Inherit the position output's variable metadata for the carry output
     * before the original goes away. */
    nir_variable *proto = NULL;
    nir_foreach_shader_out_variable(var, a) {
        proto = var;
        break;
    }
    if (!proto) {
        ralloc_free(a);
        return NULL;
    }
    struct nir_variable_data proto_data = proto->data;

    /* Drop everything strictly after the cut, users before defs so no removal
     * leaves a live use behind. */
    nir_foreach_instr_reverse_safe(instr, block)
        if (instr->index > p.cut_index)
            nir_instr_remove(instr);

    /* Drop the remaining pre-cut output stores, then the storeless output
     * variables; a leftover position output would keep the position store's
     * dead derefs alive through the re-gather. */
    nir_foreach_instr_reverse_safe(instr, block) {
        if (instr->type != nir_instr_type_intrinsic)
            continue;
        nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
        if (intr->intrinsic != nir_intrinsic_store_deref)
            continue;
        nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
        if (deref && nir_deref_mode_is(deref, nir_var_shader_out))
            nir_instr_remove(instr);
    }
    nir_foreach_instr_reverse_safe(instr, block) {
        if (instr->type == nir_instr_type_deref) {
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (nir_deref_mode_is(deref, nir_var_shader_out) &&
                nir_def_is_unused(&deref->def))
                nir_instr_remove(instr);
        }
    }
    nir_foreach_variable_with_modes_safe(var, a, nir_var_shader_out)
        exec_node_remove(&var->node);
    a->info.outputs_written = 0;

    /* Flatten the carried bases into up to four FP32 scalars and pack them into
     * one vec4.  total_comps <= 4 (the vec4 cut) bounds the flatten at four; the
     * unused lanes carry 0.0. */
    nir_builder b = nir_builder_at(nir_after_block(block));
    nir_def *scalars[4];
    unsigned n = 0;
    for (unsigned i = 0; i < p.num_bases && n < 4; i++) {
        nir_def *v = p.bases[i];
        if (p.base_type[i] == R300_MP_CARRY_INT)
            v = nir_i2f32(&b, v);
        else if (p.base_type[i] == R300_MP_CARRY_BOOL1)
            v = nir_b2f32(&b, v);
        for (unsigned c = 0; c < p.bases[i]->num_components && n < 4; c++)
            scalars[n++] = nir_channel(&b, v, c);
    }
    while (n < 4)
        scalars[n++] = nir_imm_float(&b, 0.0f);
    nir_def *carry = nir_vec4(&b, scalars[0], scalars[1], scalars[2], scalars[3]);

    nir_variable *out = nir_variable_create(a, nir_var_shader_out,
                                            glsl_vec4_type(), "r2vb_carry");
    out->data = proto_data;
    out->data.location = FRAG_RESULT_DATA0;
    out->data.location_frac = 0;
    out->data.driver_location = 0;
    nir_store_var(&b, out, carry, 0xf);
    a->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);

    nir_index_ssa_defs(impl);
    nir_progress(true, impl, nir_metadata_none);
    nir_validate_shader(a, "r300 r2vb carry pass A");
    return a;
}

nir_shader *
r300_mp_build_pos_pass_b(nir_shader *src, const struct r300_mp_partition *part,
                         unsigned num_in)
{
    nir_shader *bsh = nir_shader_clone(NULL, src);
    if (!bsh)
        return NULL;
    struct r300_mp_partition p;
    if (!mp_recollect(bsh, part, &p)) {
        ralloc_free(bsh);
        return NULL;
    }

    nir_function_impl *impl = nir_shader_get_entrypoint(bsh);
    nir_block *block = nir_start_block(impl);

    /* The carry BO arrives as one flat vertex-fed input ranked after the
     * existing model inputs (VAR0..VAR0+num_in-1); the producer draw feeds it
     * from the pass-A carry BO as the num_in+1'th vertex element. */
    nir_builder b = nir_builder_at(nir_before_block(block));
    nir_variable *carry_in = nir_variable_create(bsh, nir_var_shader_in,
                                                 glsl_vec4_type(), "r2vb_carry_in");
    carry_in->data.location = VARYING_SLOT_VAR0 + num_in;
    carry_in->data.location_frac = 0;
    carry_in->data.interpolation = INTERP_MODE_FLAT;
    carry_in->data.driver_location = num_in;
    nir_def *carry = nir_load_var(&b, carry_in);

    /* Reconstitute each base from its carry components in base order (the order
     * the packer flattened them) and rewrite the base's uses.  The pre-cut
     * producers of each base lose their consumers and dead-code away in the
     * caller's optimize pass; the post-cut position half now reads the carry. */
    unsigned comp = 0;
    for (unsigned i = 0; i < p.num_bases; i++) {
        nir_def *base = p.bases[i];
        nir_def *chans[4];
        for (unsigned c = 0; c < base->num_components; c++)
            chans[c] = nir_channel(&b, carry, comp + c);
        comp += base->num_components;
        nir_def *value = nir_vec(&b, chans, base->num_components);
        if (p.base_type[i] == R300_MP_CARRY_INT)
            value = nir_f2i32(&b, value);
        else if (p.base_type[i] == R300_MP_CARRY_BOOL1)
            value = nir_fneu(&b, value, nir_imm_float(&b, 0.0f));
        nir_def_rewrite_uses(base, value);
    }

    bsh->info.inputs_read |= BITFIELD64_BIT(VARYING_SLOT_VAR0 + num_in);
    nir_index_ssa_defs(impl);
    nir_progress(true, impl, nir_metadata_none);
    nir_validate_shader(bsh, "r300 r2vb pos pass B");
    return bsh;
}
