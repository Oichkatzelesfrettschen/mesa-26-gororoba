/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_plan.h"

#include <string.h>

#include "nir.h"
#include "util/u_atomic.h"

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

/* Typed-source scan over one restaged producer candidate.  The position
 * candidate owns the cv=0 typed class; a cv=1 varying candidate is scanned
 * separately before its single-pass verdict is admitted.  The integer cases
 * follow nir_lower_int_to_float, located with `rg --fixed-strings
 * nir_lower_int_to_float src/compiler/nir/`.  Signed markers dominate
 * unsigned, and either dominates boolean, so the class reads the strictest
 * admission constraint present.  Equality compares and boolean logic mark
 * the bool class; their integer operands mark their own class at the
 * producing op. */
static void
plan_merge_typed_source_class(
    struct r300_r2vb_producer_plan *plan,
    enum r300_r2vb_typed_source_class source_class)
{
    if (source_class == R300_R2VB_TYPED_SOURCE_NONE)
        return;

    plan->has_typed_source = true;
    if (plan->typed_source_class == R300_R2VB_TYPED_SOURCE_SINT ||
        source_class == R300_R2VB_TYPED_SOURCE_SINT) {
        plan->typed_source_class = R300_R2VB_TYPED_SOURCE_SINT;
    } else if (plan->typed_source_class == R300_R2VB_TYPED_SOURCE_UINT ||
               source_class == R300_R2VB_TYPED_SOURCE_UINT) {
        plan->typed_source_class = R300_R2VB_TYPED_SOURCE_UINT;
    } else {
        plan->typed_source_class = R300_R2VB_TYPED_SOURCE_BOOL;
    }
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
            case nir_op_idiv: case nir_op_irem: case nir_op_imod:
            case nir_op_ishl: case nir_op_ishr:
            case nir_op_ilt: case nir_op_ige:
            case nir_op_i32csel_gt: case nir_op_i32csel_ge:
                sint = true;
                break;
            case nir_op_f2u32: case nir_op_u2f32:
            case nir_op_umin: case nir_op_umax:
            case nir_op_udiv: case nir_op_umod:
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
            case nir_op_ball_iequal2: case nir_op_ball_iequal3:
            case nir_op_ball_iequal4:
            case nir_op_bany_inequal2: case nir_op_bany_inequal3:
            case nir_op_bany_inequal4:
                boolean = true;
                break;
            default:
                break;
            }
        }
    }
    plan->has_typed_source = false;
    plan->typed_source_class = R300_R2VB_TYPED_SOURCE_NONE;
    if (sint)
        plan_merge_typed_source_class(plan, R300_R2VB_TYPED_SOURCE_SINT);
    if (uns)
        plan_merge_typed_source_class(plan, R300_R2VB_TYPED_SOURCE_UINT);
    if (boolean)
        plan_merge_typed_source_class(plan, R300_R2VB_TYPED_SOURCE_BOOL);
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
                    struct r300_r2vb_producer_plan *plan,
                    bool *transient_failure)
{
    if (transient_failure)
        *transient_failure = false;
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
                case nir_intrinsic_load_ubo:
                case nir_intrinsic_load_ubo_vec4:
                case nir_intrinsic_load_push_constant:
                case nir_intrinsic_load_constant:
                    break;
                case nir_intrinsic_store_output: {
                    gl_varying_slot location;
                    if (!r300_r2vb_output_store_location(
                            nir_instr_as_intrinsic(instr), &location)) {
                        plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                        return false;
                    }
                    (void)location;
                    break;
                }
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
                if (intr->intrinsic != nir_intrinsic_store_deref &&
                    intr->intrinsic != nir_intrinsic_store_output)
                    continue;
                gl_varying_slot location;
                if (!r300_r2vb_output_store_location(intr, &location) ||
                    location == VARYING_SLOT_POS)
                    continue;
                if (intr->intrinsic == nir_intrinsic_store_output)
                    continue; /* computed store_output is the cv=1 producer */
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
         * a computed varying present, the varying producer runs the
         * single-model-stream BO-fetch transaction, so the cell keeps
         * single-input position, exactly one computed varying, and a
         * varying fed by exactly one application input (any rank; the
         * restage compaction presents it at VAR0). */
        int cv = r300_r2vb_first_computed_varying(nir);
        if (cv >= 0) {
            unsigned n_computed = 0;
            nir_foreach_block(block, impl) {
                nir_foreach_instr(instr, block) {
                    if (instr->type != nir_instr_type_intrinsic)
                        continue;
                    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                    if (intr->intrinsic != nir_intrinsic_store_deref &&
                        intr->intrinsic != nir_intrinsic_store_output)
                        continue;
                    gl_varying_slot location;
                    if (!r300_r2vb_output_store_location(intr, &location) ||
                        location == VARYING_SLOT_POS)
                        continue;
                    if (r300_r2vb_output_store_is_input_passthrough(intr))
                        continue;
                    n_computed++;
                }
            }
            unsigned num_position_inputs =
                r300_r2vb_count_position_inputs(nir);
            if (!num_position_inputs) {
                plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
                if (transient_failure)
                    *transient_failure = true;
                return false;
            }
            struct r300_r2vb_position_source vsrc;
            if (n_computed != 1 || num_position_inputs != 1 ||
                !r300_r2vb_varying_source_scan(nir, cv, &vsrc)) {
                plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                return false;
            }
        }
    }

    plan->num_position_inputs = r300_r2vb_count_position_inputs(nir);
    if (!plan->num_position_inputs) {
        plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
        if (transient_failure)
            *transient_failure = true;
        return false;
    }
    /* Retain the measured source identity alongside the count, so the
     * BO-fetch route passes plan data -- never literals -- into the
     * position-mapping contract.  A semantic scan decline leaves the source
     * record invalid for the later route-specific admission checks while the
     * structural plan continues; a clone allocation failure remains transient
     * and stays out of the cache. */
    bool source_transient = false;
    if (!r300_r2vb_position_source_scan_status(
            nir, &plan->position_source, &source_transient)) {
        if (source_transient) {
            plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
            if (transient_failure)
                *transient_failure = true;
        }
        if (source_transient)
            return false;
    }
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
                  nir_shader **keep_nir, bool *transient_failure)
{
    if (transient_failure)
        *transient_failure = false;
    nir_shader *fs =
        r300_r2vb_build_restaged_fs_nir(r300, vs_nir, target, space);
    if (!fs) {
        if (transient_failure)
            *transient_failure = true;
        return R300_FS_ADMIT_REJECT;
    }
    r300_optimize_nir(fs, r300->screen);
    enum r300_fs_admission adm = r300_fs_measure_nir_admission(
        r300, fs, NULL, R300_FS_INPUT_R2VB_FLAT_VERTEX, out_cost);
    if (keep_nir)
        *keep_nir = fs;
    else
        ralloc_free(fs);
    return adm;
}

enum r300_r2vb_constant_source_contract
r300_r2vb_constant_source_scan(nir_shader *producer,
                               uint32_t *required_bytes)
{
    if (required_bytes)
        *required_bytes = 0;
    if (!producer)
        return R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED;

    uint64_t maximum_end = 0;
    bool external_load = false;
    nir_foreach_function_impl(impl, producer) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type != nir_instr_type_intrinsic)
                    continue;
                nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                if (intr->intrinsic == nir_intrinsic_load_deref) {
                    nir_variable *var = nir_intrinsic_get_var(intr, 0);
                    if (var &&
                        (var->data.mode &
                         (nir_var_uniform | nir_var_mem_ubo |
                          nir_var_mem_push_const |
                          nir_var_mem_constant)))
                        return R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED;
                    continue;
                }
                if (intr->intrinsic == nir_intrinsic_load_push_constant ||
                    intr->intrinsic == nir_intrinsic_load_constant ||
                    intr->intrinsic == nir_intrinsic_load_uniform ||
                    intr->intrinsic ==
                        nir_intrinsic_load_ubo_uniform_block_intel ||
                    intr->intrinsic ==
                        nir_intrinsic_load_global_constant_uniform_block_intel ||
                    intr->intrinsic == nir_intrinsic_load_push_data_intel ||
                    intr->intrinsic == nir_intrinsic_load_push_constant_zink)
                    return R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED;
                if (intr->intrinsic != nir_intrinsic_load_ubo &&
                    intr->intrinsic != nir_intrinsic_load_ubo_vec4)
                    continue;

                external_load = true;
                if (!nir_src_is_const(intr->src[0]) ||
                    nir_src_as_uint(intr->src[0]) != 0 ||
                    !nir_src_is_const(intr->src[1]) ||
                    intr->def.bit_size == 0 ||
                    intr->def.bit_size % 8 != 0)
                    return R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED;

                const uint64_t component_bytes = intr->def.bit_size / 8;
                const uint64_t component =
                    nir_intrinsic_has_component(intr)
                        ? nir_intrinsic_component(intr)
                        : 0;
                uint64_t offset = nir_src_as_uint(intr->src[1]);
                if (intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
                    const uint64_t base =
                        nir_intrinsic_has_base(intr)
                            ? nir_intrinsic_base(intr)
                            : 0;
                    if (component + intr->num_components >
                        16 / component_bytes)
                        return R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED;
                    offset = (base + offset) * 16 +
                             component * component_bytes;
                } else {
                    if (nir_intrinsic_has_base(intr))
                        offset += nir_intrinsic_base(intr);
                    offset += component * component_bytes;
                }
                /* The scan window covers the varying producer's second
                 * matrix at bytes 64..127; the position-pass policy keeps
                 * its own 64-byte ceiling on the reported size. */
                const uint64_t bytes =
                    (uint64_t)intr->num_components * component_bytes;
                if (offset > 128 || bytes > 128 - offset)
                    return R300_R2VB_CONSTANT_SOURCE_UNSUPPORTED;
                maximum_end = MAX2(maximum_end, offset + bytes);
            }
        }
    }

    if (!external_load)
        return R300_R2VB_CONSTANT_SOURCE_NONE;
    if (required_bytes)
        *required_bytes = (uint32_t)maximum_end;
    return R300_R2VB_CONSTANT_SOURCE_UBO0_PREFIX64;
}

/* Walk ranked cuts in the typed diagnostic plan.  The legacy admission and
 * execution path keeps its first transport-valid cut in r300_mp_find_vec4_cut;
 * this plan owns the later-candidate choice because typed execution rebuilds
 * the selected partition from the cached candidate.  The first transport-valid
 * result stays in legacy_split_admitted, so a later fit does not widen the
 * legacy memo.  Symbol discovery uses (rg --fixed-strings
 * r300_mp_find_vec4_cut src/gallium/drivers/r300/).  The failure mask covers
 * every candidate examined through the selected fit or final decline. */
static bool
plan_walk_split_candidates(struct r300_context *r300, nir_shader *pos,
                           struct r300_r2vb_producer_plan *plan,
                           bool *transient_failure)
{
    if (transient_failure)
        *transient_failure = false;
    struct r300_mp_partition cands[R300_MP_MAX_CANDIDATES];
    unsigned n = r300_mp_find_cuts(pos, cands, R300_MP_MAX_CANDIDATES);
    if (!n) {
        plan_observe(plan, R300_R2VB_PLAN_NO_EXACT_CUT);
        return false;
    }

    struct hash_table *range_ht = _mesa_pointer_hash_table_create(NULL);
    if (!range_ht) {
        plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
        if (transient_failure)
            *transient_failure = true;
        return false;
    }
    bool first_transport_candidate_seen = false;
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

        bool first_transport_candidate = !first_transport_candidate_seen;
        first_transport_candidate_seen = true;
        nir_shader *pass_a = r300_mp_build_carry_pass_a(pos, &cands[i]);
        nir_shader *pass_b =
            r300_mp_build_pos_pass_b(pos, &cands[i],
                                     plan->num_position_inputs);
        if (!pass_a || !pass_b) {
            plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
            if (pass_a)
                ralloc_free(pass_a);
            if (pass_b)
                ralloc_free(pass_b);
            if (transient_failure)
                *transient_failure = true;
            _mesa_hash_table_destroy(range_ht, NULL);
            return false;
        }
        r300_optimize_nir(pass_a, r300->screen);
        r300_optimize_nir(pass_b, r300->screen);
        struct r300_fs_admission_cost pass_a_cost;
        struct r300_fs_admission_cost pass_b_cost;
        enum r300_fs_admission aa = r300_fs_measure_nir_admission(
            r300, pass_a, NULL, R300_FS_INPUT_R2VB_FLAT_VERTEX, &pass_a_cost);
        enum r300_fs_admission ab = r300_fs_measure_nir_admission(
            r300, pass_b, NULL, R300_FS_INPUT_R2VB_FLAT_VERTEX, &pass_b_cost);
        if (first_transport_candidate)
            plan->legacy_split_admitted =
                aa == R300_FS_ADMIT_FITS && ab == R300_FS_ADMIT_FITS;
        if (aa == R300_FS_ADMIT_FITS && ab == R300_FS_ADMIT_FITS) {
            plan->partition = cands[i];
            plan->pass_a_cost = pass_a_cost;
            plan->pass_b_cost = pass_b_cost;
            ralloc_free(pass_a);
            ralloc_free(pass_b);
            _mesa_hash_table_destroy(range_ht, NULL);
            return true;
        }
        if (aa != R300_FS_ADMIT_FITS)
            plan_observe(plan, R300_R2VB_PLAN_PASS_A);
        if (ab != R300_FS_ADMIT_FITS)
            plan_observe(plan, R300_R2VB_PLAN_PASS_B);
        ralloc_free(pass_a);
        ralloc_free(pass_b);
        /* A viable transport cut can fail one fragment admission half while a
         * later ranked cut fits.  Keep walking so typed execution retains the
         * later plan and the diagnostic mask records every observed class. */
        continue;
    }
    _mesa_hash_table_destroy(range_ht, NULL);
    return false;
}

bool
r300_r2vb_plan_producer(struct r300_context *r300, struct nir_shader *vs_nir,
                        bool allow_computed_varying,
                        enum r300_r2vb_position_space space,
                        struct r300_r2vb_producer_plan *plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->varying_slot = -1;
    plan->key.allow_computed_varying = allow_computed_varying;
    plan->key.space = space;
    plan->key.input_semantics = R300_FS_INPUT_R2VB_FLAT_VERTEX;
    plan->action = R300_R2VB_PLAN_REJECT;
    plan->status = R300_R2VB_PLAN_SEMANTIC_REJECT;

    /* The structural preflight runs on a constant-folded clone of the
     * application VS: a SPIR-V VS reaches the driver with its UBO address
     * arithmetic still literal, and folded literals are structure, not
     * computation.  The typed-source scan runs later, on the restaged
     * position candidate, so it describes the cell's own producer. */
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

    if (!allow_computed_varying)
        r300_r2vb_prune_position_only(folded);

    bool shape_transient = false;
    bool shape_ok = plan_scan_structure(folded, allow_computed_varying, plan,
                                        &shape_transient);
    ralloc_free(folded);
    if (!shape_ok) {
        if (shape_transient) {
            plan->primary_reason = R300_R2VB_PLAN_OUT_OF_MEMORY;
            plan->status = R300_R2VB_PLAN_TRANSIENT_FAILURE;
            return false;
        }
        plan->primary_reason = plan_primary_reason(plan->observed_reason_mask);
        return true;
    }

    /* Position-pass verdict from the emitted-slot admission oracle, on the
     * same restaged and preprocessed program the delivery path compiles. */
    nir_shader *pos = NULL;
    bool position_transient = false;
    enum r300_fs_admission adm = plan_measure_pass(
        r300, vs_nir, VARYING_SLOT_POS, space, &plan->baseline, &pos,
        &position_transient);
    if (position_transient || !pos) {
        plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
        plan->primary_reason = R300_R2VB_PLAN_OUT_OF_MEMORY;
        plan->status = R300_R2VB_PLAN_TRANSIENT_FAILURE;
        return false;
    }
    plan_scan_typed_source(pos, plan);
    plan->constant_source =
        r300_r2vb_constant_source_scan(pos, &plan->constant_bytes);

    bool varying_typed_source = false;
    switch (adm) {
    case R300_FS_ADMIT_FITS:
        if (allow_computed_varying) {
            int dv = r300_r2vb_first_computed_varying(vs_nir);
            if (dv >= 0) {
                nir_shader *vfs = NULL;
                bool varying_transient = false;
                enum r300_fs_admission va = plan_measure_pass(
                    r300, vs_nir, (gl_varying_slot)dv, space, NULL, &vfs,
                    &varying_transient);
                if (varying_transient) {
                    ralloc_free(vfs);
                    ralloc_free(pos);
                    plan_observe(plan, R300_R2VB_PLAN_OUT_OF_MEMORY);
                    plan->primary_reason = R300_R2VB_PLAN_OUT_OF_MEMORY;
                    plan->status = R300_R2VB_PLAN_TRANSIENT_FAILURE;
                    return false;
                }
                if (va != R300_FS_ADMIT_FITS) {
                    ralloc_free(vfs);
                    plan_observe(plan,
                                 va == R300_FS_ADMIT_OVER_ALU_BUDGET
                                     ? R300_R2VB_PLAN_OVER_ALU_NO_SPLIT
                                     : R300_R2VB_PLAN_BACKEND);
                    break;
                }
                /* The cv=1 varying producer is a separately admitted
                 * fragment pass.  Scan its optimized NIR before accepting a
                 * fitting result, because
                 * r300_r2vb_build_restaged_fs_nir, found with
                 * `rg --fixed-strings r300_r2vb_build_restaged_fs_nir
                 * src/gallium/drivers/r300/`, runs restaging DCE and drops
                 * non-target stores, which can remove the only typed
                 * producer from the position candidate. */
                struct r300_r2vb_producer_plan varying_scan = {0};
                plan_scan_typed_source(vfs, &varying_scan);
                if (varying_scan.has_typed_source) {
                    plan_merge_typed_source_class(
                        plan, varying_scan.typed_source_class);
                    plan_observe(plan,
                                 R300_R2VB_PLAN_TYPED_SINGLE_PASS_UNPROVEN);
                    varying_typed_source = true;
                }
                /* The varying pass carries its own admission record: the
                 * single application input feeding it and the UBO0 prefix
                 * its restaged producer reads, both consumed by the
                 * BO-fetch delivery route. */
                if (!r300_r2vb_varying_source_scan(vs_nir, dv,
                                                   &plan->varying_source)) {
                    ralloc_free(vfs);
                    plan_observe(plan, R300_R2VB_PLAN_IO_SHAPE);
                    break;
                }
                plan->varying_slot = dv;
                plan->varying_constant_source = r300_r2vb_constant_source_scan(
                    vfs, &plan->varying_constant_bytes);
                ralloc_free(vfs);
            }
        }
        if (varying_typed_source)
            break;
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
        bool split_transient = false;
        if (plan_walk_split_candidates(r300, pos, plan, &split_transient))
            plan->action = R300_R2VB_PLAN_SPLIT;
        else if (split_transient) {
            ralloc_free(pos);
            plan->primary_reason = R300_R2VB_PLAN_OUT_OF_MEMORY;
            plan->status = R300_R2VB_PLAN_TRANSIENT_FAILURE;
            return false;
        }
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
    /* The admission byte is valid only for the plan key that produced it.
     * A window viewport change therefore reopens the cell before the route
     * can consult a previous verdict. */
    vs->r2vb_admission[cv][sp] = R300_R2VB_ADMIT_UNMEASURED;

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
    p_atomic_inc(&plan_shadow_divergences);
}

uint32_t
r300_r2vb_plan_shadow_divergences(void)
{
    return p_atomic_read(&plan_shadow_divergences);
}

const char *
r300_r2vb_plan_action_str(enum r300_r2vb_plan_action action)
{
    switch (action) {
    case R300_R2VB_PLAN_REJECT:    return "reject";
    case R300_R2VB_PLAN_SINGLE:    return "single";
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
