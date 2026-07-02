/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 *                Joakim Sindholt <opensource@zhasha.com>
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "util/format/u_format.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "r300_cb.h"
#include "r300_context.h"
#include "r300_emit.h"
#include "r300_screen.h"
#include "r300_fs.h"
#include "r300_reg.h"
#include "r300_texture.h"

#include "compiler/radeon_compiler.h"
#include "compiler/r300_nir.h"
#include "compiler/nir_to_rc.h"
#include "compiler/classic/r300_classic_emit.h"
#include "nir.h"
#include "compiler/nir/nir_builder.h"

static void allocate_hardware_inputs(
    struct r300_fragment_program_compiler * c,
    void (*allocate)(void * data, unsigned input, unsigned hwreg),
    void * mydata)
{
    struct r300_shader_semantics* inputs =
        (struct r300_shader_semantics*)c->UserData;
    int i, reg = 0;

    /* Allocate input registers. */
    for (i = 0; i < ATTR_COLOR_COUNT; i++) {
        if (inputs->color[i] != ATTR_UNUSED) {
            allocate(mydata, inputs->color[i], reg++);
        }
    }
    if (inputs->face != ATTR_UNUSED) {
        allocate(mydata, inputs->face, reg++);
    }
    for (i = 0; i < ATTR_GENERIC_COUNT; i++) {
        if (inputs->generic[i] != ATTR_UNUSED) {
            allocate(mydata, inputs->generic[i], reg++);
        }
    }
    if (inputs->fog != ATTR_UNUSED) {
        allocate(mydata, inputs->fog, reg++);
    }
    if (inputs->wpos != ATTR_UNUSED) {
        allocate(mydata, inputs->wpos, reg++);
    }
}

/* Translate a PIPE_TEX_WRAP value to the rc_wrap_mode the compiler needs.
 * Only the modes the r300 hardware cannot perform natively are non-NONE;
 * clamp variants are handled by the sampler HW and need no shader math. */
static rc_wrap_mode
r300_pipe_wrap_to_rc(unsigned pipe_wrap)
{
    switch (pipe_wrap) {
    case PIPE_TEX_WRAP_REPEAT:
        return RC_WRAP_REPEAT;
    case PIPE_TEX_WRAP_MIRROR_REPEAT:
        return RC_WRAP_MIRRORED_REPEAT;
    case PIPE_TEX_WRAP_MIRROR_CLAMP:
    case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
    case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
        return RC_WRAP_MIRRORED_CLAMP;
    default:
        return RC_WRAP_NONE;
    }
}

/* Pick the wrap mode that is most important to emulate correctly when two
 * axes disagree.  Missing REPEAT on a repeating NPOT texture is the most
 * visible failure; MIRRORED_REPEAT second; MIRRORED_CLAMP third.
 * The compiler applies one mode uniformly to all coordinate channels, so
 * this is a best-effort choice for the mixed-axis case. */
static rc_wrap_mode
r300_npot_wrap_max(rc_wrap_mode a, rc_wrap_mode b)
{
    if (a == RC_WRAP_REPEAT || b == RC_WRAP_REPEAT)
        return RC_WRAP_REPEAT;
    if (a == RC_WRAP_MIRRORED_REPEAT || b == RC_WRAP_MIRRORED_REPEAT)
        return RC_WRAP_MIRRORED_REPEAT;
    if (a == RC_WRAP_MIRRORED_CLAMP || b == RC_WRAP_MIRRORED_CLAMP)
        return RC_WRAP_MIRRORED_CLAMP;
    return RC_WRAP_NONE;
}

void r300_fragment_program_get_external_state(
    struct r300_context* r300,
    struct r300_fragment_program_external_state* state)
{
    struct r300_textures_state *texstate = r300->textures_state.state;
    unsigned i;

    state->alpha_to_one = r300->alpha_to_one && r300->msaa_enable;
    state->sampler_state_count = texstate->sampler_state_count;

    for (i = 0; i < texstate->sampler_state_count; i++) {
        struct r300_sampler_state *s = texstate->sampler_states[i];
        struct r300_sampler_view *v = texstate->sampler_views[i];
        struct r300_resource *t;

        if (!s || !v) {
            continue;
        }

        t = r300_resource(v->base.texture);

        if (s->state.compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE) {
            state->unit[i].compare_mode_enabled = 1;

            /* Fortunately, no need to translate this. */
            state->unit[i].texture_compare_func = s->state.compare_func;
        }

        /* Pass texture swizzling to the compiler, some lowering passes need it. */
        if (state->unit[i].compare_mode_enabled) {
            state->unit[i].texture_swizzle =
                RC_MAKE_SWIZZLE(v->swizzle[0], v->swizzle[1],
                                v->swizzle[2], v->swizzle[3]);
        }

        if (t->tex.is_npot) {
            /* Start with S, which applies to every texture target. */
            rc_wrap_mode mode = r300_pipe_wrap_to_rc(s->state.wrap_s);

            /* Include T for 2-D and 3-D spatial coordinates; include R for
             * volume and cube targets.  For 1D_ARRAY the T coord is an array
             * layer index, not a spatial axis, so skip it there.  Same for
             * wrap_r on 2D_ARRAY.  The compiler emits one wrap mode for all
             * channels, so r300_npot_wrap_max picks the most critical one
             * when the axes disagree. */
            switch (t->b.target) {
            case PIPE_TEXTURE_3D:
            case PIPE_TEXTURE_CUBE:
            case PIPE_TEXTURE_CUBE_ARRAY:
                mode = r300_npot_wrap_max(mode,
                           r300_pipe_wrap_to_rc(s->state.wrap_r));
                FALLTHROUGH;
            case PIPE_TEXTURE_2D:
            case PIPE_TEXTURE_RECT:
            case PIPE_TEXTURE_2D_ARRAY:
                mode = r300_npot_wrap_max(mode,
                           r300_pipe_wrap_to_rc(s->state.wrap_t));
                break;
            default:
                break;
            }

            state->unit[i].wrap_mode = mode;

            if (t->b.target == PIPE_TEXTURE_3D)
                state->unit[i].clamp_and_scale_before_fetch = true;
        }
    }
}

static void r300_translate_fragment_shader(
    struct r300_context* r300,
    struct r300_fragment_shader_code* shader,
    struct pipe_shader_state state);

static void r300_dummy_fragment_shader(
    struct r300_context* r300,
    struct r300_fragment_shader_code* shader)
{
    struct pipe_shader_state state = {};
    const nir_shader_compiler_options *options =
        r300->screen->screen.nir_options[MESA_SHADER_FRAGMENT];

    /* Make a simple fragment shader which outputs (0, 0, 0, 1). */
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                   options, "r300 dummy FS");
    nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out_color");
    out->data.location = FRAG_RESULT_COLOR;
    nir_store_var(&b, out, nir_imm_vec4(&b, 0, 0, 0, 1), 0xf);

    shader->dummy = true;
    state.type = PIPE_SHADER_IR_NIR;
    state.ir.nir = b.shader;
    r300_translate_fragment_shader(r300, shader, state);
    ralloc_free(state.ir.nir);
}

static void r300_emit_fs_code_to_buffer(
    struct r300_context *r300,
    struct r300_fragment_shader_code *shader)
{
    struct rX00_fragment_program_code *generic_code = &shader->code;
    unsigned imm_count = shader->immediates_count;
    unsigned imm_first = shader->externals_count;
    unsigned imm_end = generic_code->constants.Count;
    struct rc_constant *constants = generic_code->constants.Constants;
    unsigned i;
    CB_LOCALS;

    if (r300->screen->caps.is_r500) {
        struct r500_fragment_program_code *code = &generic_code->code.r500;

        shader->cb_code_size = 19 +
                               ((code->inst_end + 1) * 6) +
                               imm_count * 7 +
                               code->int_constant_count * 2;

        NEW_CB(shader->cb_code, shader->cb_code_size);
        if (r300->screen->options.ieeemath)
            OUT_CB_REG(R500_US_CONFIG, R500_ZERO_TIMES_ANYTHING_EQUALS_ZERO_DEFAULT);
        else
            OUT_CB_REG(R500_US_CONFIG, R500_ZERO_TIMES_ANYTHING_EQUALS_ZERO_LEGACY);
        OUT_CB_REG(R500_US_PIXSIZE, code->max_temp_idx);
        OUT_CB_REG(R500_US_FC_CTRL, code->us_fc_ctrl);
        for(i = 0; i < code->int_constant_count; i++){
                OUT_CB_REG(R500_US_FC_INT_CONST_0 + (i * 4),
                                                code->int_constants[i]);
        }
        OUT_CB_REG(R500_US_CODE_RANGE,
                   R500_US_CODE_RANGE_ADDR(0) | R500_US_CODE_RANGE_SIZE(code->inst_end));
        OUT_CB_REG(R500_US_CODE_OFFSET, 0);
        OUT_CB_REG(R500_US_CODE_ADDR,
                   R500_US_CODE_START_ADDR(0) | R500_US_CODE_END_ADDR(code->inst_end));

        OUT_CB_REG(R500_GA_US_VECTOR_INDEX, R500_GA_US_VECTOR_INDEX_TYPE_INSTR);
        OUT_CB_ONE_REG(R500_GA_US_VECTOR_DATA, (code->inst_end + 1) * 6);
        for (i = 0; i <= code->inst_end; i++) {
            OUT_CB(code->inst[i].inst0);
            OUT_CB(code->inst[i].inst1);
            OUT_CB(code->inst[i].inst2);
            OUT_CB(code->inst[i].inst3);
            OUT_CB(code->inst[i].inst4);
            OUT_CB(code->inst[i].inst5);
        }

        /* Emit immediates. */
        if (imm_count) {
            for(i = imm_first; i < imm_end; ++i) {
                if (constants[i].Type == RC_CONSTANT_IMMEDIATE) {
                    const float *data = constants[i].u.Immediate;

                    OUT_CB_REG(R500_GA_US_VECTOR_INDEX,
                               R500_GA_US_VECTOR_INDEX_TYPE_CONST |
                               (i & R500_GA_US_VECTOR_INDEX_MASK));
                    OUT_CB_ONE_REG(R500_GA_US_VECTOR_DATA, 4);
                    OUT_CB_TABLE(data, 4);
                }
            }
        }
    } else { /* r300 */
        struct r300_fragment_program_code *code = &generic_code->code.r300;
        /* The HB_R400_US route makes an RS48x part emit the R400 US register
         * set without reclassifying the chip; both flags select the same
         * R400-class emission here. */
        const bool us_r400 = r300->screen->caps.is_r400 ||
                             r300->screen->caps.hb_r400_us;
        unsigned int alu_length = code->alu.length;
        unsigned int alu_iterations = ((alu_length - 1) / 64) + 1;
        unsigned int tex_length = code->tex.length;
        unsigned int tex_iterations =
            tex_length > 0 ? ((tex_length - 1) / 32) + 1 : 0;
        unsigned int iterations =
            alu_iterations > tex_iterations ? alu_iterations : tex_iterations;
        unsigned int bank = 0;

        shader->cb_code_size = 15 +
            /* R400_US_CODE_BANK */
            (us_r400 ? 2 * (iterations + 1): 0) +
            /* R400_US_CODE_EXT */
            (us_r400 ? 2 : 0) +
            /* R300_US_ALU_{RGB,ALPHA}_{INST,ADDR}_0, R400_US_ALU_EXT_ADDR_0 */
            (code->r390_mode ? (5 * alu_iterations) : 4) +
            /* R400_US_ALU_EXT_ADDR_[0-63] */
            (code->r390_mode ? (code->alu.length) : 0) +
            /* R300_US_ALU_{RGB,ALPHA}_{INST,ADDR}_0 */
            code->alu.length * 4 +
            /* R300_US_TEX_INST_0, R300_US_TEX_INST_[0-31] */
            (code->tex.length > 0 ? code->tex.length + tex_iterations : 0) +
            imm_count * 5;

        NEW_CB(shader->cb_code, shader->cb_code_size);

        OUT_CB_REG(R300_US_CONFIG, code->config);
        OUT_CB_REG(R300_US_PIXSIZE, code->pixsize);
        OUT_CB_REG(R300_US_CODE_OFFSET, code->code_offset);

        if (code->r390_mode) {
            OUT_CB_REG(R400_US_CODE_EXT, code->r400_code_offset_ext);
        } else if (us_r400) {
            /* This register appears to affect shaders even if r390_mode is
             * disabled, so it needs to be set to 0 for shaders that
             * don't use r390_mode. */
            OUT_CB_REG(R400_US_CODE_EXT, 0);
        }

        OUT_CB_REG_SEQ(R300_US_CODE_ADDR_0, 4);
        OUT_CB_TABLE(code->code_addr, 4);

        do {
            unsigned int bank_alu_length = (alu_length < 64 ? alu_length : 64);
            unsigned int bank_alu_offset = bank * 64;
            unsigned int bank_tex_length = (tex_length < 32 ? tex_length : 32);
            unsigned int bank_tex_offset = bank * 32;

            if (us_r400) {
                OUT_CB_REG(R400_US_CODE_BANK, code->r390_mode ?
                                (bank << R400_BANK_SHIFT) | R400_R390_MODE_ENABLE : 0);//2
            }

            if (bank_alu_length > 0) {
                OUT_CB_REG_SEQ(R300_US_ALU_RGB_INST_0, bank_alu_length);
                for (i = 0; i < bank_alu_length; i++)
                    OUT_CB(code->alu.inst[i + bank_alu_offset].rgb_inst);

                OUT_CB_REG_SEQ(R300_US_ALU_RGB_ADDR_0, bank_alu_length);
                for (i = 0; i < bank_alu_length; i++)
                    OUT_CB(code->alu.inst[i + bank_alu_offset].rgb_addr);

                OUT_CB_REG_SEQ(R300_US_ALU_ALPHA_INST_0, bank_alu_length);
                for (i = 0; i < bank_alu_length; i++)
                    OUT_CB(code->alu.inst[i + bank_alu_offset].alpha_inst);

                OUT_CB_REG_SEQ(R300_US_ALU_ALPHA_ADDR_0, bank_alu_length);
                for (i = 0; i < bank_alu_length; i++)
                    OUT_CB(code->alu.inst[i + bank_alu_offset].alpha_addr);

                if (code->r390_mode) {
                    OUT_CB_REG_SEQ(R400_US_ALU_EXT_ADDR_0, bank_alu_length);
                    for (i = 0; i < bank_alu_length; i++)
                        OUT_CB(code->alu.inst[i + bank_alu_offset].r400_ext_addr);
                }
            }

            if (bank_tex_length > 0) {
                OUT_CB_REG_SEQ(R300_US_TEX_INST_0, bank_tex_length);
                OUT_CB_TABLE(code->tex.inst + bank_tex_offset, bank_tex_length);
            }

            alu_length -= bank_alu_length;
            tex_length -= bank_tex_length;
            bank++;
        } while(code->r390_mode && (alu_length > 0 || tex_length > 0));

        /* R400_US_CODE_BANK needs to be reset to 0, otherwise some shaders
         * will be rendered incorrectly. */
        if (us_r400) {
            OUT_CB_REG(R400_US_CODE_BANK,
                code->r390_mode ? R400_R390_MODE_ENABLE : 0);
        }

        /* Emit immediates. */
        if (imm_count) {
            for(i = imm_first; i < imm_end; ++i) {
                if (constants[i].Type == RC_CONSTANT_IMMEDIATE) {
                    const float *data = constants[i].u.Immediate;

                    OUT_CB_REG_SEQ(R300_PFS_PARAM_0_X + i * 16, 4);
                    OUT_CB(pack_float24(data[0]));
                    OUT_CB(pack_float24(data[1]));
                    OUT_CB(pack_float24(data[2]));
                    OUT_CB(pack_float24(data[3]));
                }
            }
        }
    }

    OUT_CB_REG(R300_FG_DEPTH_SRC, shader->fg_depth_src);
    OUT_CB_REG(R300_US_W_FMT, shader->us_out_w);
    END_CB;
}

/* Trace a derivative intrinsic's source back to the fragment-shader input
 * variable it differentiates. The chain is short (the shader reads a varying
 * and takes its dFdx/dFdy, sometimes through a swizzle/mov), so walk through ALU
 * sources until a load_deref of a shader_in variable is found. */
static nir_variable *
r300_deriv_source_input_var(nir_def *def)
{
    nir_instr *parent = nir_def_instr(def);

    if (parent->type == nir_instr_type_intrinsic) {
        nir_intrinsic_instr *load = nir_instr_as_intrinsic(parent);
        if (load->intrinsic == nir_intrinsic_load_deref) {
            nir_deref_instr *deref = nir_src_as_deref(load->src[0]);
            nir_variable *var =
                deref ? nir_deref_instr_get_variable(deref) : NULL;
            if (var && var->data.mode == nir_var_shader_in)
                return var;
        }
    } else if (parent->type == nir_instr_type_alu) {
        nir_alu_instr *alu = nir_instr_as_alu(parent);
        for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
            nir_variable *var = r300_deriv_source_input_var(alu->src[i].src.ssa);
            if (var)
                return var;
        }
    }
    return NULL;
}

/* The six fragment derivative intrinsics the SWTCL lowering rewrites, split by
 * screen axis (ddx vs ddy, each in coarse/fine/plain forms). */
static bool
r300_is_ddx_intrinsic(nir_intrinsic_op op)
{
    return op == nir_intrinsic_ddx || op == nir_intrinsic_ddx_fine ||
           op == nir_intrinsic_ddx_coarse;
}

static bool
r300_is_ddy_intrinsic(nir_intrinsic_op op)
{
    return op == nir_intrinsic_ddy || op == nir_intrinsic_ddy_fine ||
           op == nir_intrinsic_ddy_coarse;
}

static bool
r300_is_deriv_intrinsic(nir_intrinsic_op op)
{
    return r300_is_ddx_intrinsic(op) || r300_is_ddy_intrinsic(op);
}

/* Collect every derivative intrinsic in impl into derivs and confirm they all
 * differentiate the same shader-input varying.  Returns that common source
 * variable, or NULL if there are none, one is not traceable to an input, or two
 * differentiate different inputs -- all cases the caller leaves to the stub. */
static nir_variable *
r300_collect_swtcl_derivatives(nir_function_impl *impl, struct util_dynarray *derivs)
{
    nir_variable *src_var = NULL;
    nir_foreach_block (block, impl) {
        nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (!r300_is_deriv_intrinsic(intr->intrinsic))
                continue;
            nir_variable *var = r300_deriv_source_input_var(intr->src[0].ssa);
            if (!var || (src_var && src_var != var))
                return NULL;
            src_var = var;
            util_dynarray_append(derivs, intr);
        }
    }
    return src_var;
}

/* SWTCL analytic-derivative lowering for R300-class parts, which have no
 * dFdx/dFdy hardware (radeonStubDeriv otherwise turns them into MOV 0, zeroing
 * any normal built as normalize(cross(dFdx(pos), dFdy(pos)))). Rewrite the
 * derivatives of one varying to read two synthesized GENERIC inputs that the
 * draw module fills with the per-triangle analytic gradient
 * (draw_enable_derivative_injection -> inject_screen_gradient_info). Records the
 * differentiated varying's generic index and the two gradient generic indices
 * on the shader (post-fixup numbering, since ntr_fixup_varying_slots and the
 * draw module's nir_to_tgsi apply the same VARn += 9 shift). Returns true when a
 * single differentiated varying was lowered; leaves multi-varying or non-VARn
 * cases to the stub. */
static bool
r300_nir_lower_derivatives_swtcl(nir_shader *s,
                                 struct r300_fragment_shader_code *shader)
{
    if (s->info.stage != MESA_SHADER_FRAGMENT)
        return false;

    nir_function_impl *impl = nir_shader_get_entrypoint(s);

    /* Collect the derivative intrinsics and confirm they all differentiate the
     * same input varying. */
    struct util_dynarray derivs;
    util_dynarray_init(&derivs, NULL);
    nir_variable *src_var = r300_collect_swtcl_derivatives(impl, &derivs);

    /* Only the VARn user-varying range maps cleanly onto a spare generic slot
     * the draw module can fill; the position/face/texcoord ranges do not. */
    if (!src_var || util_dynarray_num_elements(&derivs, nir_intrinsic_instr *) == 0 ||
        src_var->data.location < VARYING_SLOT_VAR0 ||
        src_var->data.location > VARYING_SLOT_VAR31) {
        util_dynarray_fini(&derivs);
        return false;
    }

    /* Post-fixup r300 generic indices: VARn -> n + 9. Reserve the two highest
     * generic slots (30, 31) for the gradients; their pre-fixup VARn locations
     * are 21 and 22. */
    const int src_generic = (src_var->data.location - VARYING_SLOT_VAR0) + 9;
    const int ddx_generic = 30;
    const int ddy_generic = 31;

    nir_variable *ddx_var = nir_variable_create(
        s, nir_var_shader_in, glsl_vec4_type(), "r300_deriv_ddx");
    nir_variable *ddy_var = nir_variable_create(
        s, nir_var_shader_in, glsl_vec4_type(), "r300_deriv_ddy");
    ddx_var->data.location = VARYING_SLOT_VAR0 + (ddx_generic - 9);
    ddy_var->data.location = VARYING_SLOT_VAR0 + (ddy_generic - 9);

    /* nir_lower_io (run later inside nir_to_rc) bases each load_input on the
     * variable's driver_location, so give the new inputs unique slots past the
     * existing ones. */
    unsigned max_drv = 0;
    nir_foreach_shader_in_variable (var, s) {
        if (var == ddx_var || var == ddy_var)
            continue;
        max_drv = MAX2(max_drv, var->data.driver_location +
                                    glsl_count_attribute_slots(var->type, false));
    }
    ddx_var->data.driver_location = max_drv;
    ddy_var->data.driver_location = max_drv + 1;

    const unsigned num_derivs =
        util_dynarray_num_elements(&derivs, nir_intrinsic_instr *);

    nir_builder b = nir_builder_create(impl);
    util_dynarray_foreach (&derivs, nir_intrinsic_instr *, intrp) {
        nir_intrinsic_instr *intr = *intrp;
        bool is_ddx = r300_is_ddx_intrinsic(intr->intrinsic);

        b.cursor = nir_before_instr(&intr->instr);
        nir_def *grad = nir_load_var(&b, is_ddx ? ddx_var : ddy_var);
        nir_def *res = nir_trim_vector(&b, grad, intr->def.num_components);
        nir_def_rewrite_uses(&intr->def, res);
        nir_instr_remove(&intr->instr);
    }
    util_dynarray_fini(&derivs);

    nir_progress(true, impl, nir_metadata_control_flow);

    shader->deriv_src_generic = src_generic;
    shader->deriv_ddx_generic = ddx_generic;
    shader->deriv_ddy_generic = ddy_generic;

    if (getenv("R300_DERIV_DEBUG"))
        fprintf(stderr, "r300 deriv: NIR lowered %u derivative(s); "
                "src_generic=%d ddx_generic=%d ddy_generic=%d\n",
                num_derivs, src_generic, ddx_generic, ddy_generic);
    return true;
}

/* Estimate the paired-ALU instruction count a fragment program will need at the
 * non-r500 hardware (R300_PFS_MAX_ALU_INST budget), from NIR, before nir_to_rc.
 * The r300 fragment ALU issues an RGB op and an alpha op per paired-ALU slot, so
 * the scalar/vector NIR ALU instruction count roughly maps to slots after the
 * RC pair scheduler packs them.  This is intentionally an OVER-estimate: count
 * every ALU and texture instruction in the entrypoint and scale by 1.09 (the
 * NIR->RC inflation measured in the R400_US sweep) so the multipass split is
 * triggered slightly early rather than late.  nir_to_rc's exact rc_recompute_ips
 * count and the emit-time ceiling remain the authority; this estimate only
 * decides whether to attempt the (phase 2) partition. */
static unsigned
r300_nir_fs_estimate_alu_pairs(nir_shader *nir)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    unsigned alu = 0, tex = 0;

    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu)
                alu++;
            else if (instr->type == nir_instr_type_tex)
                tex++;
        }
    }

    /* TEX instructions consume a texture-instruction slot, not an ALU slot, but
     * each typically anchors a small ALU coord/result sequence already counted in
     * alu; include a token weight so a texture-heavy shader is not under-counted. */
    return (unsigned)(((uint64_t)alu * 109) / 100) + tex;
}

/* Multipass auto-partition phase 2a: for a STRAIGHT-LINE fragment program (single
 * basic block, no control flow), find the cut point and the carry frontier the
 * NIR DAG split will need.  Walk the entrypoint's instructions in order, tracking
 * a running paired-ALU estimate; the cut is the first instruction past which the
 * estimate reaches the per-pass budget (target ~56 of the 64-slot envelope so the
 * NIR->RC inflation never pushes a pass over).  The FRONTIER is the set of ALU
 * results defined at or before the cut and used after it: those values cannot be
 * recomputed in the next pass (unlike load_const / a varying / a uniform, which
 * both passes can re-read), so they must be carried through the scratch render
 * target.  Returns the cut instruction (NULL when no split is needed or the shader
 * is not straight-line) and the frontier width in *out_frontier.  Phase 2b emits
 * the two sub-shaders from this; this analysis is wired into the gated decision so
 * the cut and carry cost are reported and verifiable before the emit lands. */
static nir_instr *
r300_nir_fs_analyze_cut(nir_shader *nir, unsigned per_pass_budget,
                        unsigned *out_frontier)
{
    *out_frontier = 0;
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);

    /* Straight-line only for now: a single block with the entry/exit pair. */
    if (!exec_list_is_singular(&impl->body))
        return NULL;
    nir_block *block = nir_start_block(impl);

    /* Index every instruction so a use's position relative to the cut is a simple
     * comparison. */
    unsigned idx = 0;
    nir_foreach_instr(instr, block)
        instr->index = idx++;

    /* Find the cut: the instruction at which the running inflated ALU estimate
     * first reaches the per-pass budget. */
    nir_instr *cut = NULL;
    unsigned raw = 0;
    nir_foreach_instr(instr, block) {
        if (instr->type == nir_instr_type_alu ||
            instr->type == nir_instr_type_tex)
            raw++;
        if (((uint64_t)raw * 109) / 100 >= per_pass_budget) {
            cut = instr;
            break;
        }
    }
    if (!cut)
        return NULL;

    /* Frontier: ALU defs at or before the cut with a use after it. */
    unsigned frontier = 0;
    nir_foreach_instr(instr, block) {
        if (instr->index > cut->index)
            break;
        if (instr->type != nir_instr_type_alu)
            continue;
        nir_def *def = &nir_instr_as_alu(instr)->def;
        bool crosses = false;
        nir_foreach_use(use, def) {
            if (nir_src_is_if(use))
                continue;
            if (nir_src_use_instr(use)->index > cut->index) {
                crosses = true;
                break;
            }
        }
        if (crosses)
            frontier++;
    }

    *out_frontier = frontier;
    return cut;
}

/* Pack a scalar FP24-integer-window carry into an RGBA8 colour byte-exact so an
 * 8-bit scratch render target preserves it for pass B.  r300's fragment ALU is
 * float-only, so the integer is split with float arithmetic: byte_k =
 * floor(x / 256^k) - 256 * floor(x / 256^(k+1)), scaled to a unorm channel.  This
 * is exact while the carry stays in the FP24 integer-exact window |x| < 2^17
 * (r300-fp-format-lut-and-fp24-exact-window), which is the shape the straight-line
 * integer-accumulator partition produces.  Pass B reverses it: byte_k =
 * round(channel_k * 255), x = sum_k byte_k * 256^k.  This pack/unpack pair is the
 * shared keystone of the FS partition and the multipass programmable-blend. */
/* The carry is mapped to a non-negative fixed-point integer before the byte
 * split so a signed, fractional carry survives: v = (carry + BIAS) * SCALE.
 * BIAS recentres the signed range onto [0, 2*BIAS); SCALE adds fractional bits.
 * The product must stay inside the FP24 integer-exact window (< 2^17) for the
 * floor/byte arithmetic to be lossless, so 2*BIAS*SCALE == 2^17: carry in
 * (-64, 64) with 1/1024 (~10-bit) resolution.  A carry outside that range clamps
 * at the window edge -- the residual HW precision limit of an 8-bit-per-channel
 * RT plus float-only ALU; a full-range encoding needs the log-float limb. */
#define R300_MP_CARRY_BIAS  64.0
#define R300_MP_CARRY_SCALE 1024.0

static nir_def *
r300_nir_fs_pack_carry_rgba8(nir_builder *b, nir_def *carry)
{
    nir_def *v = nir_fmul_imm(b,
        nir_fadd_imm(b, carry, R300_MP_CARRY_BIAS), R300_MP_CARRY_SCALE);
    nir_def *x = nir_ffloor(b, nir_fmax(b, v, nir_imm_float(b, 0.0f)));
    nir_def *bytes[4];
    for (unsigned k = 0; k < 4; k++) {
        nir_def *shifted = (k == 0)
            ? x
            : nir_ffloor(b, nir_fmul_imm(b, x, 1.0 / (double)(1u << (8 * k))));
        nir_def *hi = nir_ffloor(b, nir_fmul_imm(b, shifted, 1.0 / 256.0));
        bytes[k] = nir_fsub(b, shifted, nir_fmul_imm(b, hi, 256.0));
    }
    return nir_fmul_imm(b, nir_vec4(b, bytes[0], bytes[1], bytes[2], bytes[3]),
                        1.0 / 255.0);
}

/* Reverse r300_nir_fs_pack_carry_rgba8: recover the scalar carry from an RGBA8
 * scratch sample.  byte_k = round(channel_k * 255), v = sum_k byte_k * 256^k,
 * carry = v / SCALE - BIAS. */
static nir_def *
r300_nir_fs_unpack_carry_rgba8(nir_builder *b, nir_def *rgba)
{
    nir_def *v = nir_imm_float(b, 0.0f);
    for (unsigned k = 0; k < 4; k++) {
        nir_def *byte = nir_fround_even(
            b, nir_fmul_imm(b, nir_channel(b, rgba, k), 255.0));
        v = nir_fadd(b, v, nir_fmul_imm(b, byte, (double)(1u << (8 * k))));
    }
    return nir_fadd_imm(b, nir_fmul_imm(b, v, 1.0 / R300_MP_CARRY_SCALE),
                        -R300_MP_CARRY_BIAS);
}

/* Phase 2b: emit pass A of a straight-line single-frontier split.  Clone the
 * fragment shader, find the same cut and the one carry value crossing it, drop
 * every instruction after the cut (including the original colour store), and
 * write the byte-packed carry to the colour output so a scratch render target
 * captures it for pass B.  Returns the truncated clone, or NULL when the straight-line
 * single-frontier shape does not hold (the caller keeps the single-pass compile
 * and the existing >64-ALU emit error).
 *
 * The carry is written raw here; pass B reads it back through the scratch render
 * target.  r300 has only 8-bit-per-channel render targets, so a float carry that
 * leaves the FP24 integer-exact window (|x| <= 2^17, the window measured in
 * r300-fp-format-lut-and-fp24-exact-window) must be byte-packed across the four
 * RGBA8 channels rather than stored as a clamped colour -- that pack/unpack pair
 * is the shared keystone this and the multipass programmable-blend both need, and
 * is wired in the orchestration phase (P3) alongside the scratch-RT bind. */
static nir_shader *
r300_nir_fs_emit_pass_a(nir_shader *src, unsigned per_pass_budget)
{
    nir_shader *a = nir_shader_clone(NULL, src);
    unsigned frontier = 0;
    nir_instr *cut = r300_nir_fs_analyze_cut(a, per_pass_budget, &frontier);
    if (!cut || frontier != 1) {
        ralloc_free(a);
        return NULL;
    }

    nir_function_impl *impl = nir_shader_get_entrypoint(a);
    nir_block *block = nir_start_block(impl);

    /* The single carry def: the ALU result at/before the cut used after it. */
    nir_def *carry = NULL;
    nir_foreach_instr(instr, block) {
        if (instr->index > cut->index)
            break;
        if (instr->type != nir_instr_type_alu)
            continue;
        nir_def *def = &nir_instr_as_alu(instr)->def;
        nir_foreach_use(use, def) {
            if (nir_src_is_if(use))
                continue;
            if (nir_src_use_instr(use)->index > cut->index) {
                carry = def;
                break;
            }
        }
        if (carry)
            break;
    }
    /* The byte-pack carries one scalar through the four RGBA8 channels, so a
     * multi-component carry does not fit; fall back to the single-pass compile. */
    if (!carry || carry->num_components != 1) {
        ralloc_free(a);
        return NULL;
    }

    nir_variable *out = NULL;
    nir_foreach_shader_out_variable(var, a) {
        if (var->data.location == FRAG_RESULT_COLOR ||
            var->data.location == FRAG_RESULT_DATA0) {
            out = var;
            break;
        }
    }
    if (!out) {
        ralloc_free(a);
        return NULL;
    }

    /* Drop everything strictly after the cut, including the original colour
     * store; the carry and its producers (index <= cut) stay. */
    nir_foreach_instr_safe(instr, block) {
        if (instr->index > cut->index)
            nir_instr_remove(instr);
    }

    /* Byte-pack the scalar carry into the RGBA8 colour output so the 8-bit scratch
     * render target preserves it exactly for pass B to reload and unpack. */
    nir_builder b = nir_builder_at(nir_after_block(block));
    nir_store_var(&b, out, r300_nir_fs_pack_carry_rgba8(&b, carry), 0xf);

    nir_index_ssa_defs(impl);
    nir_progress(true, impl, nir_metadata_none);
    nir_validate_shader(a, "r300 FS multipass pass A");
    return a;
}

/* Phase 2b: emit pass B, the second sub-shader.  Clone the fragment shader, find
 * the same cut and scalar carry, then read the carry back from the scratch render
 * target that pass A wrote: sample a 2D sampler, unpack the RGBA8 bytes into the
 * carry value, and rewrite every use of the original carry to that value.  The
 * carry's producers (the instructions up to the cut) lose all uses and are
 * removed by the dead-code pass inside nir_to_rc, leaving only the scratch read
 * plus the post-cut instructions -- the second half of the program.
 *
 * The orchestration phase (P3) binds the scratch image at sampler unit 0 and
 * drives the fullscreen pass; the sample coordinate convention (here the
 * normalized fragment position) is matched there.  Returns the rewritten clone or
 * NULL when the straight-line single-frontier shape does not hold. */
static nir_shader *
r300_nir_fs_emit_pass_b(nir_shader *src, unsigned per_pass_budget)
{
    nir_shader *bsh = nir_shader_clone(NULL, src);
    unsigned frontier = 0;
    nir_instr *cut = r300_nir_fs_analyze_cut(bsh, per_pass_budget, &frontier);
    if (!cut || frontier != 1) {
        ralloc_free(bsh);
        return NULL;
    }

    nir_function_impl *impl = nir_shader_get_entrypoint(bsh);
    nir_block *block = nir_start_block(impl);

    nir_def *carry = NULL;
    nir_foreach_instr(instr, block) {
        if (instr->index > cut->index)
            break;
        if (instr->type != nir_instr_type_alu)
            continue;
        nir_def *def = &nir_instr_as_alu(instr)->def;
        nir_foreach_use(use, def) {
            if (nir_src_is_if(use))
                continue;
            if (nir_src_use_instr(use)->index > cut->index) {
                carry = def;
                break;
            }
        }
        if (carry)
            break;
    }
    if (!carry || carry->num_components != 1) {
        ralloc_free(bsh);
        return NULL;
    }

    /* Sample the scratch target at the top of the block, before any use of the
     * carry, and unpack the byte-packed value pass A wrote.  A RECT sampler reads
     * at the fragment's window coordinate directly: the r300 compiler inserts the
     * RC_STATE_R300_TEXRECT_FACTOR scale (1/width, 1/height of the bound scratch)
     * the driver fills, so gl_FragCoord normalizes to the matching scratch texel
     * with no separate uniform.  The draw orchestration allocates the scratch as
     * PIPE_TEXTURE_RECT to match. */
    nir_builder b = nir_builder_at(nir_before_block(block));
    nir_variable *samp = nir_variable_create(
        bsh, nir_var_uniform,
        glsl_sampler_type(GLSL_SAMPLER_DIM_RECT, false, false, GLSL_TYPE_FLOAT),
        "r300_mp_scratch");
    samp->data.binding = 0;
    nir_deref_instr *deref = nir_build_deref_var(&b, samp);
    nir_def *coord = nir_trim_vector(&b, nir_load_frag_coord(&b), 2);
    nir_def *rgba = nir_tex(&b, coord, .texture_deref = deref,
                            .sampler_deref = deref);
    nir_def *value = r300_nir_fs_unpack_carry_rgba8(&b, rgba);

    nir_def_rewrite_uses(carry, value);

    nir_index_ssa_defs(impl);
    nir_progress(true, impl, nir_metadata_none);
    nir_validate_shader(bsh, "r300 FS multipass pass B");
    return bsh;
}

static void r300_translate_fragment_shader(
    struct r300_context* r300,
    struct r300_fragment_shader_code* shader,
    struct pipe_shader_state state)
{
    struct r300_fragment_program_compiler compiler;
    int face;
    unsigned i;
    union r300_shader_code code;
    code.f = shader;

    r300_shader_semantics_reset(&shader->inputs);

    shader->deriv_src_generic = -1;
    shader->deriv_ddx_generic = -1;
    shader->deriv_ddy_generic = -1;

    /* gl_FragColor (vs. gl_FragData[0]) makes the FS write the same value
     * to all bound color buffers. */
    shader->write_all = false;
    nir_foreach_shader_out_variable(var, state.ir.nir) {
        if (var->data.location == FRAG_RESULT_COLOR) {
            shader->write_all = true;
            break;
        }
    }

    /* Setup the compiler. */
    memset(&compiler, 0, sizeof(compiler));
    rc_init(&compiler.Base, &r300->fs_regalloc_state);
    DBG_ON(r300, DBG_FP) ? compiler.Base.Debug |= RC_DBG_LOG : 0;

    compiler.code = &shader->code;
    compiler.state = shader->compare_state;
    if (!shader->dummy)
        compiler.Base.debug = &r300->context.debug;
    compiler.Base.is_r500 = r300->screen->caps.is_r500;
    /* The R400 instruction envelope (512 ALU/TEX slots + r390_mode code banks)
     * is gated by is_r400 in the compiler.  The HB route opens it on an RS48x
     * part two ways: hb_r400_us_alu_only lifts the ALU/TEX slot count and the
     * code-bank emission while leaving the temp file at the proven R300 size of
     * 32, so a >64-instruction shader that stays within 32 temps exercises code
     * banks without touching the unproven upper temp file; hb_r400_us_envelope
     * additionally raises the temp file to the full R400 64. */
    const bool us_envelope = r300->screen->caps.is_r400 ||
                             r300->screen->caps.hb_r400_us_envelope ||
                             r300->screen->caps.hb_r400_us_alu_only;
    const bool us_full_temps = r300->screen->caps.is_r400 ||
                               r300->screen->caps.hb_r400_us_envelope;
    compiler.Base.is_r400 = us_envelope;
    compiler.Base.disable_optimizations = DBG_ON(r300, DBG_NO_OPT);
    compiler.Base.has_half_swizzles = true;
    compiler.Base.has_presub = true;
    compiler.Base.has_omod = true;
    compiler.Base.max_temp_regs =
        compiler.Base.is_r500 ? 128 : (us_full_temps ? 64 : 32);
    compiler.Base.max_constants = compiler.Base.is_r500 ? 256 : 32;
    compiler.Base.max_alu_insts =
        (compiler.Base.is_r500 || us_envelope) ? 512 : 64;
    compiler.Base.max_tex_insts =
        (compiler.Base.is_r500 || us_envelope) ? 512 : 32;
    compiler.AllocateHwInputs = &allocate_hardware_inputs;
    compiler.UserData = &shader->inputs;

    nir_shader *clone = nir_shader_clone(NULL, state.ir.nir);

    /* Multipass auto-partition, phase 1 (R300_FS_MULTIPASS, default off): estimate
     * the paired-ALU instruction count this fragment program will need at the NIR
     * level, before nir_to_rc, so a future pass can split a program that would
     * overflow R300_PFS_MAX_ALU_INST into a chain of <=budget passes carried
     * through a scratch render target.  The estimate counts NIR ALU + texture
     * instructions and applies the ~1.09x NIR->RC inflation the R400_US sweep
     * measured; nir_to_rc's exact rc_recompute_ips count and the emit-time ceiling
     * (r300_fragprog_emit.c) stay the authority and the backstop, so an estimate
     * error only mis-times the (not-yet-built) split, never miscompiles.  Phase 2
     * replaces this diagnostic with the actual NIR DAG partition. */
    if (!compiler.Base.is_r500 && !us_envelope) {
        static int mp_gate = -1;
        if (mp_gate < 0) {
            const char *e = getenv("R300_FS_MULTIPASS");
            mp_gate = (e && e[0] == '1') ? 1 : 0;
        }
        if (mp_gate) {
            /* The authoritative >64-ALU ceiling is emit_alu in r300_fragprog_emit.c:
             * code->alu.length >= max_alu_insts errors the compile.  This NIR-level
             * estimate runs before nir_to_rc lowers trig/pow/etc, so it UNDERCOUNTS;
             * gating at the raw max_alu_insts would let a shader whose estimate lands
             * under the ceiling still overflow at emit and fall back to the dummy FS.
             * Gate instead at the inflation-safe per-pass capacity (also the per-pass
             * split budget): a shader whose estimate exceeds what one safe pass holds
             * is split at NIR, so the authoritative emit ceiling is never reached for
             * the straight-line single-frontier shapes the partition handles. */
            const unsigned safe_pass_alu = 56;
            unsigned est = r300_nir_fs_estimate_alu_pairs(clone);
            if (est > safe_pass_alu) {
                nir_shader *pass_a = r300_nir_fs_emit_pass_a(clone, safe_pass_alu);
                nir_shader *pass_b = r300_nir_fs_emit_pass_b(clone, safe_pass_alu);
                if (pass_a && pass_b) {
                    /* Compile pass B into a partner code object -- it is below
                     * budget so the recursive translate will not re-split -- store
                     * it for the draw path, and fall through to compile pass A as
                     * this shader's code by swapping the clone the main path
                     * compiles.  The draw path renders pass A to a scratch target,
                     * then pass B (sampling that scratch) to the real attachment. */
                    struct r300_fragment_shader_code *pb =
                        CALLOC_STRUCT(r300_fragment_shader_code);
                    if (pb) {
                        struct pipe_shader_state pb_state = {
                            .type = PIPE_SHADER_IR_NIR, .ir.nir = pass_b };
                        r300_translate_fragment_shader(r300, pb, pb_state);
                        shader->multipass_pass_b = pb;
                        ralloc_free(pass_b);
                        ralloc_free(clone);
                        clone = pass_a;
                        fprintf(stderr,
                                "r300 FS multipass: estimate %u > budget %u; split "
                                "compiled (pass A as FS, pass B partner stored)\n",
                                est, (unsigned)compiler.Base.max_alu_insts);
                    } else {
                        ralloc_free(pass_a);
                        ralloc_free(pass_b);
                    }
                } else {
                    if (pass_a)
                        ralloc_free(pass_a);
                    if (pass_b)
                        ralloc_free(pass_b);
                    fprintf(stderr,
                            "r300 FS multipass: estimate %u > budget %u but not a "
                            "straight-line single-frontier split; partition deferred\n",
                            est, (unsigned)compiler.Base.max_alu_insts);
                }
            }
        }
    }

    /* R300-class parts have no fragment dFdx/dFdy hardware. Rewrite a varying's
     * derivatives to read draw-module-supplied per-triangle gradients instead of
     * letting them lower to MOV 0. Skipped on r500 (native derivatives) and when
     * R300_DERIV_VIA_DRAW=0. The draw injection is enabled per draw in
     * r300_draw_vbo once the shader's recorded generic indices are known. */
    if (!r300->screen->caps.is_r500) {
        static int deriv_gate = -1;
        if (deriv_gate < 0) {
            const char *e = getenv("R300_DERIV_VIA_DRAW");
            deriv_gate = (e && strcmp(e, "0") == 0) ? 0 : 1;
        }
        if (deriv_gate)
            r300_nir_lower_derivatives_swtcl(clone, shader);

        /* Zero any derivative the analytic pass could not claim (multi-varying,
         * non-VARn source, or the gate off).  r300_optimize_nir leaves the
         * derivatives as intrinsics so the analytic pass above can run first;
         * the r300 RC backend has no DDX/DDY emit, so the residue must become
         * MOV 0 here before nir_to_rc. */
        NIR_PASS(_, clone, r300_nir_stub_deriv);
    }

    /* Classic front end, opt-in via R300_USE_CLASSIC_FS=1 (unset, empty, or
     * any other value stays closed).  The classic path selects the NIR into
     * its own SSA IR, allocates, and emits into the same rc_program object
     * nir_to_rc fills, so everything downstream (face transform,
     * r3xx_compile_fragment_program, the emitters) is shared.  It runs only
     * when the external compare state is plain -- shadow-sampler, wpos, and
     * alpha-to-one lowering live inside nir_to_rc -- and any selection,
     * allocation, or emission reject falls back to nir_to_rc unchanged.
     * Input semantics record into a local table and reach shader->inputs
     * only on full success, so a fallback never leaves a partial record. */
    bool classic_done = false;
    static int classic_gate = -1;
    if (classic_gate < 0) {
        const char *e = getenv("R300_USE_CLASSIC_FS");
        classic_gate = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (classic_gate) {
        static const struct r300_fragment_program_external_state plain_ext;
        if (memcmp(&shader->compare_state, &plain_ext,
                   sizeof(plain_ext)) == 0) {
            void *cctx = ralloc_context(NULL);
            nir_shader *cclone = nir_shader_clone(cctx, clone);
            const struct r300_classic_target *ct = r300_classic_target_get(
                compiler.Base.is_r400, compiler.Base.is_r500);
            struct r300_shader_semantics classic_inputs;
            r300_shader_semantics_reset(&classic_inputs);
            struct r300_classic_select_result sel;
            struct r300_classic_regalloc_result ra;
            const char *why = NULL;
            if (!r300_classic_select(cctx, cclone, ct, 0, &classic_inputs,
                                     &sel) || !sel.program) {
                why = sel.reject_reason ? sel.reject_reason : "selection";
            } else if (!r300_classic_regalloc(cctx, sel.program, &ra) ||
                       !ra.temp_of_ssa) {
                why = ra.reject_reason ? ra.reject_reason : "allocation";
            } else if (!r300_classic_emit(sel.program, &ra, &sel.immediates,
                                          &compiler)) {
                why = "emission";
            } else {
                shader->inputs = classic_inputs;
                classic_done = true;
            }
            /* Both verdicts print under DBG_FP so a gate-on hardware run can
             * prove which front end compiled each shader. */
            if (DBG_ON(r300, DBG_FP)) {
                if (classic_done)
                    fprintf(stderr, "r300 classic FS: compiled\n");
                else
                    fprintf(stderr, "r300 classic FS fallback: %s\n", why);
            }
            ralloc_free(cctx);
        }
    }

    if (!classic_done)
        nir_to_rc(clone, (struct pipe_screen *)r300->screen,
                  shader->compare_state, code, &compiler.Base);

    if (compiler.Base.Error) {
        shader->error = strdup(compiler.Base.ErrorMsg ? compiler.Base.ErrorMsg
                                                      : "Cannot translate shader from NIR.");
        rc_destroy(&compiler.Base);
        r300_dummy_fragment_shader(r300, shader);
        return;
    }

    face = shader->inputs.face;

    if (!r300->screen->caps.is_r500 ||
        compiler.Base.Program.Constants.Count > 200) {
        compiler.Base.remove_unused_constants = true;
    }

    if (face != ATTR_UNUSED) {
        rc_transform_fragment_face(&compiler.Base, face);
    }

    /* Invoke the compiler */
    r3xx_compile_fragment_program(&compiler);

    if (compiler.Base.Error) {
        shader->error = strdup(compiler.Base.ErrorMsg);

        if (shader->dummy) {
            fprintf(stderr, "r300 FP: Cannot compile the dummy shader! "
                    "Giving up...\n");
            abort();
        }

        free(compiler.code->constants.Constants);
        free(compiler.code->constants_remap_table);
        rc_destroy(&compiler.Base);
        r300_dummy_fragment_shader(r300, shader);
        return;
    }

    /* Shaders with zero instructions are invalid,
     * use the dummy shader instead. */
    if (shader->code.code.r500.inst_end == -1) {
        rc_destroy(&compiler.Base);
        r300_dummy_fragment_shader(r300, shader);
        return;
    }

    /* Initialize numbers of constants for each type. */
    shader->externals_count = 0;
    for (i = 0;
         i < shader->code.constants.Count &&
         shader->code.constants.Constants[i].Type == RC_CONSTANT_EXTERNAL; i++) {
        shader->externals_count = i+1;
    }
    shader->immediates_count = 0;
    shader->rc_state_count = 0;

    for (i = shader->externals_count; i < shader->code.constants.Count; i++) {
        switch (shader->code.constants.Constants[i].Type) {
            case RC_CONSTANT_IMMEDIATE:
                ++shader->immediates_count;
                break;
            case RC_CONSTANT_STATE:
                ++shader->rc_state_count;
                break;
            default:
                assert(0);
        }
    }

    /* Setup shader depth output. */
    if (shader->code.writes_depth) {
        shader->fg_depth_src = R300_FG_DEPTH_SRC_SHADER;
        shader->us_out_w = R300_W_FMT_W24 | R300_W_SRC_US;
    } else {
        shader->fg_depth_src = R300_FG_DEPTH_SRC_SCAN;
        shader->us_out_w = R300_W_FMT_W0 | R300_W_SRC_US;
    }

    /* And, finally... */
    rc_destroy(&compiler.Base);

    /* Build the command buffer. */
    r300_emit_fs_code_to_buffer(r300, shader);
}

bool r300_pick_fragment_shader(struct r300_context *r300,
                               struct r300_fragment_shader* fs,
                               struct r300_fragment_program_external_state *state)
{
    struct r300_fragment_shader_code* ptr;

    /* The >64-ALU multipass draw orchestration forces pass B for its second draw;
     * honour the override rather than re-picking pass A by texture-compare state. */
    if (r300->multipass_override_fs) {
        if (fs->shader != r300->multipass_override_fs) {
            fs->shader = r300->multipass_override_fs;
            return true;
        }
        return false;
    }

    if (!fs->first) {
        /* Build the fragment shader for the first time. */
        fs->first = fs->shader = CALLOC_STRUCT(r300_fragment_shader_code);

        memcpy(&fs->shader->compare_state, state, sizeof(*state));
        r300_translate_fragment_shader(r300, fs->shader, fs->state);
        return true;

    } else {
        /* Check if the currently-bound shader has been compiled
         * with the texture-compare state we need. */
        if (memcmp(&fs->shader->compare_state, state, sizeof(*state)) != 0) {
            /* Search for the right shader. */
            ptr = fs->first;
            while (ptr) {
                if (memcmp(&ptr->compare_state, state, sizeof(*state)) == 0) {
                    if (fs->shader != ptr) {
                        fs->shader = ptr;
                        return true;
                    }
                    /* The currently-bound one is OK. */
                    return false;
                }
                ptr = ptr->next;
            }

            /* Not found, gotta compile a new one. */
            ptr = CALLOC_STRUCT(r300_fragment_shader_code);
            ptr->next = fs->first;
            fs->first = fs->shader = ptr;

            memcpy(&ptr->compare_state, state, sizeof(*state));
            r300_translate_fragment_shader(r300, ptr, fs->state);
            return true;
        }
    }

    return false;
}
