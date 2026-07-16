/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_plan.h"

#include <string.h>

#include "nir.h"

#include "r300_context.h"
#include "compiler/r300_nir.h"
#include "r300_screen.h"
#include "r300_vs.h"

static void
plan_observe(struct r300_r2vb_producer_plan *plan,
             enum r300_r2vb_plan_reason reason)
{
    plan->observed_reason_mask |= 1ull << reason;
}

/* The primary reason is the lowest-numbered observed class; the enum declares
 * the classes in precedence order, so a walk that saw both a range decline and
 * a later half-compile failure reports the range decline, never CARRY_WIDTH
 * merely because the first-ranked candidate happened to be wide. */
static enum r300_r2vb_plan_reason
plan_primary_reason(uint64_t mask)
{
    for (unsigned r = 1; r < R300_R2VB_PLAN_REASON_COUNT; r++)
        if (mask & (1ull << r))
            return (enum r300_r2vb_plan_reason)r;
    return R300_R2VB_PLAN_OK;
}

/* Whole-program typed-source scan.  Signed markers dominate unsigned, and
 * either dominates boolean, so the class reads the strictest admission
 * constraint present.  Equality compares and boolean logic mark the bool
 * class; their integer operands mark their own class at the producing op. */
static void
plan_scan_typed_source(nir_shader *nir, struct r300_r2vb_producer_plan *plan)
{
    bool sint = false, uns = false, boolean = false;
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
                continue;
            switch (nir_instr_as_alu(instr)->op) {
            case nir_op_f2i32: case nir_op_i2f32:
            case nir_op_imin: case nir_op_imax:
            case nir_op_ineg: case nir_op_iabs:
            case nir_op_iadd: case nir_op_isub: case nir_op_imul:
            case nir_op_ishl: case nir_op_ishr:
            case nir_op_ilt: case nir_op_ige:
                sint = true;
                break;
            case nir_op_f2u32: case nir_op_u2f32:
            case nir_op_umin: case nir_op_umax:
            case nir_op_ushr:
            case nir_op_ult: case nir_op_uge:
                uns = true;
                break;
            case nir_op_flt: case nir_op_fge:
            case nir_op_feq: case nir_op_fneu:
            case nir_op_ieq: case nir_op_ine:
            case nir_op_b2f32: case nir_op_b2i32:
            case nir_op_bcsel:
            case nir_op_inot: case nir_op_iand:
            case nir_op_ior: case nir_op_ixor:
                boolean = true;
                break;
            default:
                break;
            }
        }
    }
    plan->has_typed_source = sint || uns || boolean;
    plan->typed_source_class =
        sint ? R300_R2VB_TYPED_SOURCE_SINT :
        uns ? R300_R2VB_TYPED_SOURCE_UINT :
        boolean ? R300_R2VB_TYPED_SOURCE_BOOL : R300_R2VB_TYPED_SOURCE_NONE;
}

/* Pre-lowering shape validation: only the structural facts that survive the
 * fragment backend's lowering -- single-block control flow, plain I/O and
 * uniform/UBO intrinsics, a gl_Position output, and a bounded set of
 * position-feeding inputs.  Varying discipline (input-fed passthroughs, the
 * computed-varying arity rule) belongs to the cv=1 plan alone; the cv=0
 * cell predicts the position-pass admission memo.  ALU-lowering capability
 * is the backend's verdict on the restaged FS (a float-only op whitelist
 * here is exactly the reject that made the typed split unreachable through
 * the production route). */
static bool
plan_scan_structure(nir_shader *nir, bool allow_computed_varying,
                    struct r300_r2vb_producer_plan *plan)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    if (!impl) {
        plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
        return false;
    }
    if (!exec_list_is_singular(&impl->body)) {
        plan_observe(plan, R300_R2VB_PLAN_CONTROL_FLOW);
        return false;
    }

    /* A uniform interface is optional: production admission delivers
     * uniform-free producers (a passthrough VS transforms inputs alone), and
     * requiring one here diverged the shadow plan from the memo on exactly
     * those shaders on RS482. */
    bool has_pos_out = false;
    nir_foreach_variable_in_shader(var, nir) {
        if ((var->data.mode & nir_var_shader_out) &&
            var->data.location == VARYING_SLOT_POS)
            has_pos_out = true;
    }
    if (!has_pos_out) {
        plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
        return false;
    }

    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_load_const:
            case nir_instr_type_deref:
            case nir_instr_type_alu:
                break;
            case nir_instr_type_intrinsic:
                switch (nir_instr_as_intrinsic(instr)->intrinsic) {
                case nir_intrinsic_load_deref:
                case nir_intrinsic_store_deref:
                case nir_intrinsic_load_input:
                case nir_intrinsic_store_output:
                case nir_intrinsic_load_ubo:
                case nir_intrinsic_load_ubo_vec4:
                case nir_intrinsic_load_push_constant:
                case nir_intrinsic_load_constant:
                    break;
                default:
                    plan_observe(plan, R300_R2VB_PLAN_INTRINSIC);
                    return false;
                }
                break;
            default:
                /* texturing, jumps */
                plan_observe(plan, instr->type == nir_instr_type_jump
                                       ? R300_R2VB_PLAN_CONTROL_FLOW
                                       : R300_R2VB_PLAN_INTRINSIC);
                return false;
            }
        }
    }

    /* Non-position outputs are the cv=1 plan's surface.  The cv=0 cell
     * predicts the position-pass admission memo, which the clip route
     * consults directly for its position leg (r300_r2vb_producer_fits_budget
     * measures the restaged position producer alone); varyings ride the
     * passthrough re-ingest or the cv=1 varying producer, so a computed
     * varying leaves the cv=0 cell untouched.  The RS482 shadow-parity
     * corpus caught the cv=0 cell rejecting io_shape on the spill1 reference
     * producer's computed varying while the memo recorded the position pass
     * FITS. */
    if (allow_computed_varying) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type != nir_instr_type_intrinsic)
                    continue;
                nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                if (intr->intrinsic != nir_intrinsic_store_deref)
                    continue;
                nir_variable *out = nir_intrinsic_get_var(intr, 0);
                if (!out || !(out->data.mode & nir_var_shader_out) ||
                    out->data.location == VARYING_SLOT_POS)
                    continue;
                nir_intrinsic_instr *val = nir_src_as_intrinsic(intr->src[1]);
                if (!val || val->intrinsic != nir_intrinsic_load_deref)
                    continue; /* computed: the varying producer renders it */
                nir_variable *src = nir_intrinsic_get_var(val, 0);
                if (!src || !(src->data.mode & nir_var_shader_in)) {
                    plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                    return false;
                }
            }
        }

        /* The production arity rule (r300_vs_nir_is_fragment_aluable): with
         * a computed varying present, the varying producer feeds a single
         * attribute, so every non-first input appears solely as a
         * passthrough varying source. */
        if (r300_r2vb_first_computed_varying(nir) >= 0) {
            nir_variable *first_in = NULL;
            nir_foreach_variable_with_modes(v, nir, nir_var_shader_in) {
                first_in = v;
                break;
            }
            nir_foreach_block(block, impl) {
                nir_foreach_instr(instr, block) {
                    if (instr->type != nir_instr_type_intrinsic)
                        continue;
                    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                    if (intr->intrinsic != nir_intrinsic_load_deref)
                        continue;
                    nir_variable *in = nir_intrinsic_get_var(intr, 0);
                    if (!in || !(in->data.mode & nir_var_shader_in) ||
                        in == first_in)
                        continue;
                    nir_foreach_use(use, &intr->def) {
                        if (nir_src_is_if(use)) {
                            plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                            return false;
                        }
                        nir_instr *cons = nir_src_use_instr(use);
                        if (cons->type != nir_instr_type_intrinsic) {
                            plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                            return false;
                        }
                        nir_intrinsic_instr *ci = nir_instr_as_intrinsic(cons);
                        if (ci->intrinsic != nir_intrinsic_store_deref) {
                            plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                            return false;
                        }
                        nir_variable *o = nir_intrinsic_get_var(ci, 0);
                        if (!o || !(o->data.mode & nir_var_shader_out) ||
                            o->data.location == VARYING_SLOT_POS) {
                            plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                            return false;
                        }
                    }
                }
            }
        }
    }

    plan->num_position_inputs = r300_r2vb_count_position_inputs(nir);
    if (plan->num_position_inputs > R300_R2VB_MAX_PRODUCER_INPUTS) {
        plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
        return false;
    }
    return true;
}

/* One admission measurement of a producer pass: build the restaged FS, run
 * the same preprocessing create_fs_state runs, compile it throwaway.  Keeps
 * the optimized NIR when keep_nir is non-NULL (the caller owns it); frees it
 * otherwise. */
static enum r300_fs_admission
plan_measure_pass(struct r300_context *r300, nir_shader *vs_nir,
                  gl_varying_slot target,
                  enum r300_r2vb_position_space space,
                  struct r300_fs_admission_cost *out_cost,
                  nir_shader **keep_nir)
{
    nir_shader *fs =
        r300_r2vb_build_restaged_fs_nir(r300, vs_nir, target, space);
    if (!fs)
        return R300_FS_ADMIT_REJECT;
    r300_optimize_nir(fs, r300->screen);
    enum r300_fs_admission adm = r300_fs_measure_nir_admission(
        r300, fs, NULL, R300_FS_INPUT_R2VB_FLAT_VERTEX, out_cost);
    if (keep_nir)
        *keep_nir = fs;
    else
        ralloc_free(fs);
    return adm;
}

/* One evaluated split candidate: the partition plus both admitted halves'
 * resource vectors. */
struct plan_split_result {
    struct r300_mp_partition part;
    struct r300_fs_admission_cost a;
    struct r300_fs_admission_cost b;
};

/* Deterministic success ordering: fewer carry components, then the lower
 * bottleneck half (max of the two ALU counts), then total ALU, then the
 * temporary high-water, then constant pressure, then the earlier cut index.
 * Every criterion is a measured or structural fact, so the selection is
 * stable across runs. */
static bool
plan_split_better(const struct plan_split_result *n,
                  const struct plan_split_result *best)
{
    if (n->part.total_comps != best->part.total_comps)
        return n->part.total_comps < best->part.total_comps;
    unsigned n_max = MAX2(n->a.alu, n->b.alu);
    unsigned b_max = MAX2(best->a.alu, best->b.alu);
    if (n_max != b_max)
        return n_max < b_max;
    if (n->a.alu + n->b.alu != best->a.alu + best->b.alu)
        return n->a.alu + n->b.alu < best->a.alu + best->b.alu;
    unsigned n_tmp = MAX2(n->a.temps, n->b.temps);
    unsigned b_tmp = MAX2(best->a.temps, best->b.temps);
    if (n_tmp != b_tmp)
        return n_tmp < b_tmp;
    unsigned n_cst = MAX2(n->a.consts, n->b.consts);
    unsigned b_cst = MAX2(best->a.consts, best->b.consts);
    if (n_cst != b_cst)
        return n_cst < b_cst;
    return n->part.cut_index < best->part.cut_index;
}

/* Walk every ranked cut candidate of the optimized position producer,
 * evaluate each fully, and select the best success by the deterministic
 * ordering above.  Every failure class seen along the walk lands in the
 * reason mask. */
static bool
plan_walk_split_candidates(struct r300_context *r300, nir_shader *pos,
                           struct r300_r2vb_producer_plan *plan)
{
    struct r300_mp_partition cands[R300_MP_MAX_CANDIDATES];
    unsigned n = r300_mp_find_cuts(pos, cands, R300_MP_MAX_CANDIDATES);
    if (!n) {
        plan_observe(plan, R300_R2VB_PLAN_NO_EXACT_CUT);
        return false;
    }

    struct hash_table *range_ht = _mesa_pointer_hash_table_create(NULL);
    struct plan_split_result best;
    bool have_best = false;
    for (unsigned i = 0; i < n; i++) {
        if (cands[i].total_comps > 4) {
            plan_observe(plan, R300_R2VB_PLAN_CARRY_WIDTH);
            continue;
        }
        enum r300_mp_transport_decline decline;
        if (!r300_mp_select_r2vb_transport(pos, &cands[i], range_ht,
                                           &decline)) {
            switch (decline) {
            case R300_MP_TRANSPORT_MIXED_SIGNEDNESS:
                plan_observe(plan, R300_R2VB_PLAN_MIXED_SIGNEDNESS);
                break;
            case R300_MP_TRANSPORT_SIGNED_RANGE:
                plan_observe(plan, R300_R2VB_PLAN_SIGNED_RANGE);
                break;
            case R300_MP_TRANSPORT_UNSIGNED_RANGE:
                plan_observe(plan, R300_R2VB_PLAN_UNSIGNED_RANGE);
                break;
            default:
                plan_observe(plan, R300_R2VB_PLAN_NO_EXACT_CUT);
                break;
            }
            continue;
        }

        nir_shader *pass_a = r300_mp_build_carry_pass_a(pos, &cands[i]);
        nir_shader *pass_b =
            r300_mp_build_pos_pass_b(pos, &cands[i],
                                     plan->num_position_inputs);
        if (!pass_a || !pass_b) {
            plan_observe(plan, !pass_a ? R300_R2VB_PLAN_PASS_A
                                       : R300_R2VB_PLAN_PASS_B);
            if (pass_a)
                ralloc_free(pass_a);
            if (pass_b)
                ralloc_free(pass_b);
            continue;
        }
        r300_optimize_nir(pass_a, r300->screen);
        r300_optimize_nir(pass_b, r300->screen);
        struct plan_split_result res = { .part = cands[i] };
        enum r300_fs_admission aa = r300_fs_measure_nir_admission(
            r300, pass_a, NULL, R300_FS_INPUT_R2VB_FLAT_VERTEX, &res.a);
        enum r300_fs_admission ab = r300_fs_measure_nir_admission(
            r300, pass_b, NULL, R300_FS_INPUT_R2VB_FLAT_VERTEX, &res.b);
        if (aa == R300_FS_ADMIT_FITS && ab == R300_FS_ADMIT_FITS) {
            if (!have_best || plan_split_better(&res, &best)) {
                best = res;
                have_best = true;
            }
        } else {
            if (aa != R300_FS_ADMIT_FITS)
                plan_observe(plan, R300_R2VB_PLAN_PASS_A);
            if (ab != R300_FS_ADMIT_FITS)
                plan_observe(plan, R300_R2VB_PLAN_PASS_B);
        }
        ralloc_free(pass_a);
        ralloc_free(pass_b);
    }
    _mesa_hash_table_destroy(range_ht, NULL);

    if (have_best) {
        plan->partition = best.part;
        plan->pass_a_cost = best.a;
        plan->pass_b_cost = best.b;
    }
    return have_best;
}

bool
r300_r2vb_plan_producer(struct r300_context *r300, struct nir_shader *vs_nir,
                        bool allow_computed_varying,
                        enum r300_r2vb_position_space space,
                        struct r300_r2vb_producer_plan *plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->key.allow_computed_varying = allow_computed_varying;
    plan->key.space = space;
    plan->key.input_semantics = R300_FS_INPUT_R2VB_FLAT_VERTEX;
    plan->action = R300_R2VB_PLAN_REJECT;
    plan->status = R300_R2VB_PLAN_SEMANTIC_REJECT;

    /* Structural and typed-source scans run on a constant-folded clone: a
     * SPIR-V VS reaches the driver with its UBO address arithmetic still
     * literal, and folded literals are structure, not typed computation. */
    nir_shader *folded = nir_shader_clone(NULL, vs_nir);
    if (!folded) {
        plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
        plan->primary_reason = R300_R2VB_PLAN_OUT_OF_MEMORY;
        plan->status = R300_R2VB_PLAN_TRANSIENT_FAILURE;
        return false;
    }
    bool progress;
    do {
        progress = false;
        progress |= nir_opt_constant_folding(folded);
        progress |= nir_opt_dce(folded);
    } while (progress);

    bool shape_ok = plan_scan_structure(folded, allow_computed_varying, plan);
    if (shape_ok)
        plan_scan_typed_source(folded, plan);
    ralloc_free(folded);
    if (!shape_ok) {
        plan->primary_reason = plan_primary_reason(plan->observed_reason_mask);
        return true;
    }

    /* Position-pass verdict from the emitted-slot admission oracle, on the
     * same restaged and preprocessed program the delivery path compiles. */
    nir_shader *pos = NULL;
    enum r300_fs_admission adm = plan_measure_pass(
        r300, vs_nir, VARYING_SLOT_POS, space, &plan->baseline, &pos);
    if (!pos) {
        plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
        plan->primary_reason = R300_R2VB_PLAN_OUT_OF_MEMORY;
        plan->status = R300_R2VB_PLAN_TRANSIENT_FAILURE;
        return false;
    }

    switch (adm) {
    case R300_FS_ADMIT_FITS:
        if (allow_computed_varying) {
            int dv = r300_r2vb_first_computed_varying(vs_nir);
            if (dv >= 0) {
                enum r300_fs_admission va = plan_measure_pass(
                    r300, vs_nir, (gl_varying_slot)dv, space, NULL, NULL);
                if (va != R300_FS_ADMIT_FITS) {
                    plan_observe(plan,
                                 va == R300_FS_ADMIT_OVER_ALU_BUDGET
                                     ? R300_R2VB_PLAN_OVER_ALU_NO_SPLIT
                                     : R300_R2VB_PLAN_BACKEND);
                    break;
                }
            }
        }
        if (plan->has_typed_source) {
            /* A fitting typed producer would run single-pass without ever
             * passing the carry range/signedness checks, admitting an
             * unbounded or mixed-signedness value; it declines until that
             * single-pass domain is proven. */
            plan_observe(plan, R300_R2VB_PLAN_TYPED_SINGLE_PASS_UNPROVEN);
            break;
        }
        plan->action = R300_R2VB_PLAN_SINGLE;
        break;

    case R300_FS_ADMIT_OVER_ALU_BUDGET:
        if (allow_computed_varying) {
            /* The split serves the single-input position pass only; the
             * computed-varying producer keeps the single-pass rule. */
            plan_observe(plan, R300_R2VB_PLAN_OVER_ALU_NO_SPLIT);
            break;
        }
        if (plan->num_position_inputs + 1 > R300_R2VB_MAX_PRODUCER_INPUTS) {
            /* Pass B feeds every model attribute plus the carry, so the
             * split is deliverable only within the producer input ceiling. */
            plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
            break;
        }
        if (plan_walk_split_candidates(r300, pos, plan))
            plan->action = R300_R2VB_PLAN_SPLIT;
        break;

    case R300_FS_ADMIT_REJECT:
    default:
        plan_observe(plan, R300_R2VB_PLAN_BACKEND);
        break;
    }

    if (plan->action == R300_R2VB_PLAN_REJECT) {
        ralloc_free(pos);
        plan->primary_reason = plan_primary_reason(plan->observed_reason_mask);
    } else {
        plan->candidate = pos;
        plan->primary_reason = R300_R2VB_PLAN_OK;
        plan->status = R300_R2VB_PLAN_READY;
    }
    return true;
}

void
r300_r2vb_plan_release(struct r300_r2vb_producer_plan *plan)
{
    if (plan->candidate)
        ralloc_free(plan->candidate);
    memset(plan, 0, sizeof(*plan));
}

/* Build the key for the bound state.  The struct is zeroed first so the
 * padding bytes compare equal under memcmp. */
static void
plan_build_key(struct r300_context *r300, bool allow_computed_varying,
               enum r300_r2vb_position_space space,
               struct r300_r2vb_plan_key *key)
{
    memset(key, 0, sizeof(*key));
    key->allow_computed_varying = allow_computed_varying;
    key->space = space;
    key->input_semantics = R300_FS_INPUT_R2VB_FLAT_VERTEX;
    if (space == R300_R2VB_POSITION_WINDOW) {
        const struct pipe_viewport_state *vp = &r300->viewport;
        for (unsigned i = 0; i < 3; i++) {
            memcpy(&key->viewport_scale[i], &vp->scale[i], sizeof(uint32_t));
            memcpy(&key->viewport_translate[i], &vp->translate[i],
                   sizeof(uint32_t));
        }
    }
}

const struct r300_r2vb_producer_plan *
r300_r2vb_producer_plan_get(struct r300_context *r300,
                            bool allow_computed_varying,
                            enum r300_r2vb_position_space space)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return NULL;

    unsigned cv = allow_computed_varying ? 1 : 0;
    unsigned sp = space == R300_R2VB_POSITION_WINDOW ? 1 : 0;
    struct r300_r2vb_plan_key key;
    plan_build_key(r300, allow_computed_varying, space, &key);

    struct r300_r2vb_producer_plan *slot = vs->r2vb_plan[cv][sp];
    if (slot && memcmp(&slot->key, &key, sizeof(key)) == 0)
        return slot;
    if (slot) {
        r300_r2vb_plan_release(slot);
        FREE(slot);
        vs->r2vb_plan[cv][sp] = NULL;
    }

    struct r300_r2vb_producer_plan *plan = CALLOC_STRUCT(r300_r2vb_producer_plan);
    if (!plan)
        return NULL;
    if (!r300_r2vb_plan_producer(r300, vs->state.ir.nir,
                                 allow_computed_varying, space, plan)) {
        /* TRANSIENT_FAILURE: release without caching so a later call replans
         * instead of pinning a transient verdict as a permanent fallback. */
        r300_r2vb_plan_release(plan);
        FREE(plan);
        return NULL;
    }
    plan->key = key;
    vs->r2vb_plan[cv][sp] = plan;
    return plan;
}

void
r300_r2vb_plan_cache_release(struct r300_vertex_shader *vs)
{
    for (unsigned cv = 0; cv < 2; cv++) {
        for (unsigned sp = 0; sp < 2; sp++) {
            if (vs->r2vb_plan[cv][sp]) {
                r300_r2vb_plan_release(vs->r2vb_plan[cv][sp]);
                FREE(vs->r2vb_plan[cv][sp]);
                vs->r2vb_plan[cv][sp] = NULL;
            }
        }
    }
}

static uint32_t plan_shadow_divergences;

void
r300_r2vb_plan_note_shadow_divergence(void)
{
    plan_shadow_divergences++;
}

uint32_t
r300_r2vb_plan_shadow_divergences(void)
{
    return plan_shadow_divergences;
}

const char *
r300_r2vb_plan_action_str(enum r300_r2vb_plan_action action)
{
    switch (action) {
    case R300_R2VB_PLAN_REJECT:    return "reject";
    case R300_R2VB_PLAN_SINGLE:    return "single";
    case R300_R2VB_PLAN_COMPACTED: return "compacted";
    case R300_R2VB_PLAN_SPLIT:     return "split";
    }
    return "invalid";
}

const char *
r300_r2vb_plan_reason_str(enum r300_r2vb_plan_reason reason)
{
    switch (reason) {
    case R300_R2VB_PLAN_OK:                        return "ok";
    case R300_R2VB_PLAN_OUT_OF_MEMORY:             return "out_of_memory";
    case R300_R2VB_PLAN_CONTROL_FLOW:              return "control_flow";
    case R300_R2VB_PLAN_INTRINSIC:                 return "intrinsic";
    case R300_R2VB_PLAN_IO_SHAPE:                  return "io_shape";
    case R300_R2VB_PLAN_TYPED_SINGLE_PASS_UNPROVEN:
        return "typed_single_pass_unproven";
    case R300_R2VB_PLAN_MIXED_SIGNEDNESS:          return "mixed_signedness";
    case R300_R2VB_PLAN_SIGNED_RANGE:              return "signed_range";
    case R300_R2VB_PLAN_UNSIGNED_RANGE:            return "unsigned_range";
    case R300_R2VB_PLAN_CARRY_WIDTH:               return "carry_width";
    case R300_R2VB_PLAN_PASS_A:                    return "pass_a";
    case R300_R2VB_PLAN_PASS_B:                    return "pass_b";
    case R300_R2VB_PLAN_BACKEND:                   return "backend";
    case R300_R2VB_PLAN_NO_EXACT_CUT:              return "no_exact_cut";
    case R300_R2VB_PLAN_OVER_ALU_NO_SPLIT:         return "over_alu_no_split";
    case R300_R2VB_PLAN_REASON_COUNT:              break;
    }
    return "invalid";
}
