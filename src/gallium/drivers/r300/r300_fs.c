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
#include "nir/nir_draw_helpers.h"
#include "compiler/nir_to_rc.h"
#include "compiler/classic/r300_classic_emit.h"
#include "compiler/classic/r300_classic_regalloc.h"
#include "nir.h"
#include "compiler/nir/nir_builder.h"
#include "r300_nir_ssa_cut.h"

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
    state->pstipple = r300->pstipple_draw;
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

        /* Fractional MIN/MAX LOD clamp: the TX unit only clamps on integer
         * levels, so a fractional window lowers into the shader as TXB with
         * an analytic-gradient bias.  SWTCL only -- the ddx/ddy the lowering
         * emits ride the draw-module gradient injection, which HW-TCL parts
         * do not run.  The same fractional predicate widens the sampler's
         * integer window to floor/ceil in r300_create_sampler_state. */
        if (!r300->screen->caps.has_tcl &&
            t->b.target == PIPE_TEXTURE_2D &&
            s->state.min_mip_filter != PIPE_TEX_MIPFILTER_NONE &&
            (s->state.min_lod != floorf(s->state.min_lod) ||
             s->state.max_lod != ceilf(s->state.max_lod))) {
            float minl = CLAMP(s->state.min_lod, 0.0f, 255.0f);
            float maxl = CLAMP(s->state.max_lod, 0.0f, 255.0f);

            state->unit[i].frac_lod_clamp = 1;
            state->unit[i].lod_min_q88 = (unsigned)(minl * 256.0f + 0.5f);
            state->unit[i].lod_max_q88 = (unsigned)(maxl * 256.0f + 0.5f);
            state->unit[i].tex_width = MIN2(t->tex.width0, 0xffff);
            state->unit[i].tex_height = MIN2(t->tex.height0, 0xffff);
        }
    }
}

static void r300_translate_fragment_shader_body(
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
    /* The body, not the wrapper: the failed program's "Too many ALU" error
     * string survives on the shader object, and the wrapper would read it as
     * an invitation to partition the dummy. */
    r300_translate_fragment_shader_body(r300, shader, state);
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

/* Fractional MIN/MAX LOD clamp lowering.  The TX unit clamps mip selection
 * only on integer levels, so a fractional clamp window rewrites every plain
 * 2D fetch on the unit into TXB: the bias is clamp(lod, min, max) - lod with
 * lod computed analytically as 0.5 * log2(max(|ddx(st) * size|^2,
 * |ddy(st) * size|^2)).  The hardware adds the bias to its own lod, so the
 * result lands on the clamp exactly where the shader-computed lod matches
 * the hardware's; the clamp range and texture size arrive as immediates
 * baked into this variant (quantized copies sit in the variant key).  The
 * ddx/ddy emitted here are claimed by the SWTCL derivative lowering that
 * runs next. */
static bool
r300_nir_lower_frac_lod_clamp(nir_shader *s,
                              const struct r300_fragment_program_external_state *state)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(s);
    nir_builder b = nir_builder_create(impl);
    bool progress = false;

    nir_foreach_block(block, impl) {
        nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
                continue;

            nir_tex_instr *tex = nir_instr_as_tex(instr);
            unsigned unit = tex->sampler_index;

            if (unit >= ARRAY_SIZE(state->unit) ||
                !state->unit[unit].frac_lod_clamp ||
                tex->op != nir_texop_tex ||
                tex->sampler_dim != GLSL_SAMPLER_DIM_2D)
                continue;

            int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
            if (ci < 0)
                continue;

            b.cursor = nir_before_instr(instr);

            nir_def *st = nir_trim_vector(&b, tex->src[ci].src.ssa, 2);
            nir_def *size =
                nir_imm_vec2(&b, state->unit[unit].tex_width,
                             state->unit[unit].tex_height);
            nir_def *sx = nir_fmul(&b, nir_ddx(&b, st), size);
            nir_def *sy = nir_fmul(&b, nir_ddy(&b, st), size);
            nir_def *rho2 = nir_fmax(&b, nir_fdot2(&b, sx, sx),
                                     nir_fdot2(&b, sy, sy));
            /* LG2 of a zero gradient (constant coord) must not poison the
             * bias; the floor pins the unclamped lod at a large negative
             * value the clamp then lifts. */
            rho2 = nir_fmax(&b, rho2, nir_imm_float(&b, 1e-10f));
            nir_def *lod = nir_fmul_imm(&b, nir_flog2(&b, rho2), 0.5f);
            nir_def *clamped =
                nir_fclamp(&b, lod,
                           nir_imm_float(&b, state->unit[unit].lod_min_q88 /
                                         256.0f),
                           nir_imm_float(&b, state->unit[unit].lod_max_q88 /
                                         256.0f));

            nir_tex_instr_add_src(tex, nir_tex_src_bias,
                                  nir_fsub(&b, clamped, lod));
            tex->op = nir_texop_txb;
            progress = true;
        }
    }

    return nir_progress(progress, impl, nir_metadata_control_flow);
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

/* Rebuild the differentiated expression with the source varying's load shifted
 * by `shift`; the caller subtracts a quad-hi and quad-lo rebuild to form the
 * 2x2-quad finite difference (exact at a non-smooth point). Scalarized NIR. */
static nir_def *
r300_deriv_build_shifted(nir_builder *b, nir_def *def,
                         nir_variable *src_var, nir_def *shift)
{
    nir_instr *parent = nir_def_instr(def);
    if (parent->type == nir_instr_type_load_const)
        return def;
    if (parent->type == nir_instr_type_intrinsic) {
        nir_intrinsic_instr *load = nir_instr_as_intrinsic(parent);
        if (load->intrinsic == nir_intrinsic_load_deref) {
            nir_deref_instr *deref = nir_src_as_deref(load->src[0]);
            nir_variable *var = deref ? nir_deref_instr_get_variable(deref) : NULL;
            if (var == src_var)
                return nir_fadd(b, def, nir_trim_vector(b, shift, def->num_components));
            return NULL;
        }
        switch (load->intrinsic) {
        case nir_intrinsic_load_ubo:
        case nir_intrinsic_load_ubo_vec4:
        case nir_intrinsic_load_uniform:
            return def;
        default: break;
        }
        return NULL;
    }
    if (parent->type != nir_instr_type_alu)
        return NULL;
    nir_alu_instr *alu = nir_instr_as_alu(parent);
    if (nir_op_is_vec(alu->op)) {
        nir_def *comps[NIR_MAX_VEC_COMPONENTS];
        for (unsigned i = 0; i < def->num_components; i++) {
            nir_def *si = r300_deriv_build_shifted(b, alu->src[i].src.ssa, src_var, shift);
            if (!si) return NULL;
            comps[i] = nir_channel(b, si, alu->src[i].swizzle[0]);
        }
        return nir_vec(b, comps, def->num_components);
    }
    const unsigned n = nir_op_infos[alu->op].num_inputs;
    nir_def *sr[4];
    for (unsigned i = 0; i < n && i < 4; i++) {
        nir_def *si = r300_deriv_build_shifted(b, alu->src[i].src.ssa, src_var, shift);
        if (!si) return NULL;
        unsigned sw[4] = {0};
        for (unsigned c = 0; c < def->num_components; c++)
            sw[c] = alu->src[i].swizzle[c];
        sr[i] = nir_swizzle(b, si, sw, def->num_components);
    }
    switch (alu->op) {
    case nir_op_mov:  return sr[0];
    case nir_op_fneg: return nir_fneg(b, sr[0]);
    case nir_op_fadd: return nir_fadd(b, sr[0], sr[1]);
    case nir_op_fmul: return nir_fmul(b, sr[0], sr[1]);
    case nir_op_ffma: return nir_ffma(b, sr[0], sr[1], sr[2]);
    case nir_op_fabs: return nir_fabs(b, sr[0]);
    case nir_op_fsign: return nir_fsign(b, sr[0]);
    default: return NULL;
    }
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

    /* Three source classes are analytically differentiable here. VARn user
     * varyings and TEXn fixed-function texcoords both map onto generic slots
     * the draw module fills with the per-triangle gradient
     * (ntr_fixup_varying_slots and the draw module's nir_to_tgsi agree on
     * the numbering: VARn -> n + 9, TEXn -> n). gl_FragCoord
     * (VARYING_SLOT_POS) needs no injection: its window-space xy gradient
     * is the compile-time constant dFdx=(1,0), dFdy=(0,1). The face range
     * does not map. */
    const bool is_pos = src_var && src_var->data.location == VARYING_SLOT_POS;
    const bool is_tex = src_var &&
                        src_var->data.location >= VARYING_SLOT_TEX0 &&
                        src_var->data.location <= VARYING_SLOT_TEX7;
    if (!src_var || util_dynarray_num_elements(&derivs, nir_intrinsic_instr *) == 0 ||
        (!is_pos && !is_tex &&
         (src_var->data.location < VARYING_SLOT_VAR0 ||
          src_var->data.location > VARYING_SLOT_VAR31))) {
        util_dynarray_fini(&derivs);
        return false;
    }

    /* Injection slots for the VARn path; the gl_FragCoord path leaves them -1
     * so r300_draw_vbo does not enable draw-module gradient injection. */
    int src_generic = -1, ddx_generic = -1, ddy_generic = -1;
    nir_variable *ddx_var = NULL, *ddy_var = NULL;

    if (!is_pos) {
        /* Post-fixup r300 generic indices: VARn -> n + 9, TEXn -> n.
         * Reserve the two highest generic slots (30, 31) for the
         * gradients. */
        src_generic = is_tex ?
            (int)(src_var->data.location - VARYING_SLOT_TEX0) :
            (int)(src_var->data.location - VARYING_SLOT_VAR0) + 9;
        ddx_generic = 30;
        ddy_generic = 31;

        ddx_var = nir_variable_create(
            s, nir_var_shader_in, glsl_vec4_type(), "r300_deriv_ddx");
        ddy_var = nir_variable_create(
            s, nir_var_shader_in, glsl_vec4_type(), "r300_deriv_ddy");
        ddx_var->data.location = VARYING_SLOT_VAR0 + (ddx_generic - 9);
        ddy_var->data.location = VARYING_SLOT_VAR0 + (ddy_generic - 9);

        /* nir_lower_io (run later inside nir_to_rc) bases each load_input on the
         * variable's driver_location, so give the new inputs unique slots past
         * the existing ones. */
        unsigned max_drv = 0;
        nir_foreach_shader_in_variable (var, s) {
            if (var == ddx_var || var == ddy_var)
                continue;
            max_drv = MAX2(max_drv, var->data.driver_location +
                                        glsl_count_attribute_slots(var->type, false));
        }
        /* The quad-parity anchor reads gl_FragCoord, so nir_to_rc adds a wpos
         * input and places it at the next free driver_location (max_drv). Skip
         * that slot for the gradient generics: nir_to_rc's input_index_map is
         * keyed by driver_location, so a gradient generic sharing max_drv with
         * wpos aliases it and reads the wrong interpolated input instead of the
         * draw-injected gradient. */
        ddx_var->data.driver_location = max_drv + 1;
        ddy_var->data.driver_location = max_drv + 2;
    }

    const unsigned num_derivs =
        util_dynarray_num_elements(&derivs, nir_intrinsic_instr *);

    nir_builder b = nir_builder_create(impl);

    /* First build the analytic gradient for every derivative; if any expression
     * is not differentiable, leave the whole shader to the stub rather than
     * emit a partially-correct result. */
    const unsigned nd = num_derivs;
    nir_def **results = calloc(nd, sizeof(*results));
    bool all_ok = results != NULL;
    unsigned di = 0;
    util_dynarray_foreach (&derivs, nir_intrinsic_instr *, intrp) {
        nir_intrinsic_instr *intr = *intrp;
        bool is_ddx = r300_is_ddx_intrinsic(intr->intrinsic);
        b.cursor = nir_before_instr(&intr->instr);
        /* Seed d(src_var)/d(screen axis): the injected per-triangle gradient
         * for a VARn, or the constant window-space gradient for gl_FragCoord.
         * gl_FragCoord z (depth) and w (1/wclip) are left zero, so a shader
         * that differentiates gl_FragCoord.zw is not covered by this path. */
        nir_def *seed;
        if (is_pos)
            seed = is_ddx ? nir_imm_vec4(&b, 1, 0, 0, 0)
                          : nir_imm_vec4(&b, 0, 1, 0, 0);
        else
            seed = nir_load_var(&b, is_ddx ? ddx_var : ddy_var);
        nir_def *coord = nir_channel(&b, nir_load_frag_coord(&b), is_ddx ? 0 : 1);
        nir_def *half = nir_fmul_imm(&b, nir_ffloor(&b, coord), 0.5);
        nir_def *is_odd = nir_fneu(&b, half, nir_ffloor(&b, half));
        nir_def *zero = nir_imm_zero(&b, seed->num_components, seed->bit_size);
        nir_def *shift_hi = nir_bcsel(&b, is_odd, zero, seed);
        nir_def *shift_lo = nir_bcsel(&b, is_odd, nir_fneg(&b, seed), zero);
        nir_def *hi = all_ok ?
            r300_deriv_build_shifted(&b, intr->src[0].ssa, src_var, shift_hi) : NULL;
        nir_def *lo = hi ?
            r300_deriv_build_shifted(&b, intr->src[0].ssa, src_var, shift_lo) : NULL;
        nir_def *res = lo ? nir_fsub(&b, hi, lo) : NULL;
        if (!res)
            all_ok = false;
        results[di++] = res;
    }

    if (!all_ok) {
        free(results);
        util_dynarray_fini(&derivs);
        return false;
    }

    di = 0;
    util_dynarray_foreach (&derivs, nir_intrinsic_instr *, intrp) {
        nir_intrinsic_instr *intr = *intrp;
        nir_def_rewrite_uses(&intr->def, results[di++]);
        nir_instr_remove(&intr->instr);
    }
    free(results);
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

/* ---- >64-ALU FS multipass phase 2: the NIR DAG partition ----
 *
 * A fragment program that the RC backend rejects at the R300_PFS_MAX_ALU_INST
 * ceiling splits into two passes at a cut through its single-block SSA DAG.
 * Pass A computes everything up to the cut and writes the values crossing it
 * (the CARRY) into scratch render targets; pass B reloads the carry from those
 * targets sampled at the fragment position and finishes the program.  Each
 * carried scalar component travels as a hi/lo byte pair in two RGBA8 channels:
 * v = (x + BIAS) * SCALE spans [0, 2^16) for x in (-64, 64) at 1/512
 * resolution, and every packing intermediate stays inside the FP24
 * integer-exact window (|v| < 2^17), so the pair is exact through an 8-bit
 * unorm channel.  Two components per scratch target across the four-MRT
 * ceiling bounds the carry at eight components.
 *
 * Admission authority is the compile itself: the split is attempted only
 * after the unsplit program failed at the emit ceiling, and it is adopted
 * only when BOTH halves compile clean through the RC backend.  The NIR
 * instruction-count estimate is refused as an authority because the pair
 * scheduler deflates vec4 chains: the three RS482 capacity cases estimate
 * 127-130 NIR ALU yet emit 69-86 RC slots. */

#define R300_MP_MAX_SCRATCH     4
#define R300_MP_C16_BIAS        64.0
#define R300_MP_C16_SCALE       512.0

/* Flatten the carried bases into per-component float scalars, in base order.
 * Integer-carried bases pass through i2f32 so the fixed-point pack below sees
 * a plain float in the window. */
static unsigned
r300_mp_flatten_carries(nir_builder *b, const struct r300_mp_partition *p,
                        nir_def **scalars)
{
    unsigned n = 0;
    for (unsigned i = 0; i < p->num_bases; i++) {
        nir_def *v = p->bases[i];
        if (p->base_type[i] == R300_MP_CARRY_INT)
            v = nir_i2f32(b, v);
        else if (p->base_type[i] == R300_MP_CARRY_BOOL1)
            v = nir_b2f32(b, v);
        for (unsigned c = 0; c < p->bases[i]->num_components; c++)
            scalars[n++] = nir_channel(b, v, c);
    }
    return n;
}

#define R300_MP_C8_BIAS 128.0

/* Channel layout for the multipass carry encoding, derived once from the
 * partition and consumed identically by the packer (pass A) and the
 * unpacker (pass B): a bool1 or int-typed carry scalar is exact as a
 * single UNORM8 byte (bool: the b2f32 0.0/1.0 value written directly;
 * small int: biased by 128 so any integer in [-128, 127] round-trips
 * through byte = round(x) + 128), while a float-typed carry keeps the
 * wider hi/lo 16-bit fixed-point pair.  Byte-tier scalars pack four to an
 * RGBA8 scratch target instead of two, and their pack/unpack is a single
 * add-clamp-floor instead of the float tier's floor/floor/sub ladder, so
 * a frontier that is mostly bool/int (the common case: loop counters,
 * comparison results, small array indices) both fits in fewer scratch
 * targets and costs a fraction of the ALU per carried component.  Byte
 * targets are ordered before float16 targets; both sides compute this
 * split from the same r300_mp_partition, so a writer/reader mismatch is
 * structurally impossible. */
struct r300_mp_layout {
    unsigned n;
    enum r300_mp_carry_type stype[R300_MP_MAX_CARRY_COMPS];
    unsigned order[R300_MP_MAX_CARRY_COMPS];
    unsigned num_byte;
    unsigned num_float;
    unsigned num_byte_rts;
    unsigned num_float_rts;
    unsigned num_rts;
};

static void
r300_mp_build_layout(const struct r300_mp_partition *p,
                     struct r300_mp_layout *L)
{
    L->n = 0;
    for (unsigned i = 0; i < p->num_bases; i++)
        for (unsigned c = 0; c < p->bases[i]->num_components; c++)
            L->stype[L->n++] = p->base_type[i];

    L->num_byte = 0;
    for (unsigned i = 0; i < L->n; i++)
        if (L->stype[i] != R300_MP_CARRY_FLOAT)
            L->order[L->num_byte++] = i;
    L->num_float = 0;
    for (unsigned i = 0; i < L->n; i++)
        if (L->stype[i] == R300_MP_CARRY_FLOAT)
            L->order[L->num_byte + L->num_float++] = i;

    L->num_byte_rts = DIV_ROUND_UP(L->num_byte, 4);
    L->num_float_rts = DIV_ROUND_UP(L->num_float, 2);
    L->num_rts = L->num_byte_rts + L->num_float_rts;
}

/* Pack the flattened carry scalars into L->num_rts RGBA8 scratch colours
 * per r300_mp_layout: byte tier first (up to four scalars per target),
 * then float16 tier (the hi/lo pair this replaces for float-typed
 * carries).  All arithmetic stays in the FP24 integer-exact window; a
 * byte-tier carry outside [-128, 127] and a float16-tier carry outside
 * (-BIAS, BIAS) both clamp at the window edge. */
static void
r300_mp_pack_carries(nir_builder *b, nir_def **scalars,
                     const struct r300_mp_layout *L, nir_def **rt_colors)
{
    for (unsigned k = 0; k < L->num_byte_rts; k++) {
        nir_def *chan[4];
        for (unsigned c = 0; c < 4; c++) {
            unsigned si = k * 4 + c;
            if (si >= L->num_byte) {
                chan[c] = nir_imm_float(b, 0.0f);
                continue;
            }
            unsigned fi = L->order[si];
            nir_def *x = scalars[fi];
            if (L->stype[fi] == R300_MP_CARRY_BOOL1) {
                chan[c] = x;
            } else {
                nir_def *v = nir_fmin(b, nir_fmax(b,
                    nir_fadd_imm(b, x, R300_MP_C8_BIAS), nir_imm_float(b, 0.0f)),
                    nir_imm_float(b, 255.0f));
                chan[c] = nir_fmul_imm(b,
                    nir_ffloor(b, nir_fadd_imm(b, v, 0.5f)), 1.0 / 255.0);
            }
        }
        rt_colors[k] = nir_vec4(b, chan[0], chan[1], chan[2], chan[3]);
    }
    for (unsigned k = 0; k < L->num_float_rts; k++) {
        unsigned sa = L->num_byte + 2 * k, sc = sa + 1;
        nir_def *xa = scalars[L->order[sa]];
        nir_def *xc = (sc < L->n) ? scalars[L->order[sc]] : nir_imm_float(b, 0.0f);
        nir_def *va = nir_fmul_imm(b,
            nir_fadd_imm(b, xa, R300_MP_C16_BIAS), R300_MP_C16_SCALE);
        nir_def *vc = nir_fmul_imm(b,
            nir_fadd_imm(b, xc, R300_MP_C16_BIAS), R300_MP_C16_SCALE);
        va = nir_fmin(b, nir_fmax(b, va, nir_imm_float(b, 0.0f)),
                     nir_imm_float(b, 65535.0f));
        vc = nir_fmin(b, nir_fmax(b, vc, nir_imm_float(b, 0.0f)),
                     nir_imm_float(b, 65535.0f));
        nir_def *vaf = nir_ffloor(b, va), *vcf = nir_ffloor(b, vc);
        nir_def *hia = nir_ffloor(b, nir_fmul_imm(b, vaf, 1.0 / 256.0));
        nir_def *loa = nir_fsub(b, vaf, nir_fmul_imm(b, hia, 256.0));
        nir_def *hic = nir_ffloor(b, nir_fmul_imm(b, vcf, 1.0 / 256.0));
        nir_def *loc = nir_fsub(b, vcf, nir_fmul_imm(b, hic, 256.0));
        nir_def *rgba = nir_vec4(b, hia, loa, hic, loc);
        rt_colors[L->num_byte_rts + k] = nir_fmul_imm(b, rgba, 1.0 / 255.0);
    }
}

/* Reverse r300_mp_pack_carries.  rt_texs holds one sampled RGBA8 value per
 * scratch target, in the same byte-tier-then-float16-tier order the
 * packer used; scalars_out is filled back at each scalar's original
 * flatten index so the base-order reconstruction in emit_pass_b is
 * unchanged. */
static void
r300_mp_unpack_carries(nir_builder *b, nir_def **rt_texs,
                       const struct r300_mp_layout *L, nir_def **scalars_out)
{
    for (unsigned k = 0; k < L->num_byte_rts; k++) {
        nir_def *rgba = rt_texs[k];
        nir_def *bytes = nir_ffloor(b,
            nir_fadd_imm(b, nir_fmul_imm(b, rgba, 255.0f), 0.5f));
        for (unsigned c = 0; c < 4; c++) {
            unsigned si = k * 4 + c;
            if (si >= L->num_byte)
                continue;
            unsigned fi = L->order[si];
            if (L->stype[fi] == R300_MP_CARRY_BOOL1)
                scalars_out[fi] = nir_channel(b, rgba, c);
            else
                scalars_out[fi] = nir_fadd_imm(b, nir_channel(b, bytes, c),
                                               -R300_MP_C8_BIAS);
        }
    }
    for (unsigned k = 0; k < L->num_float_rts; k++) {
        nir_def *rgba = rt_texs[L->num_byte_rts + k];
        nir_def *bytes = nir_fround_even(b, nir_fmul_imm(b, rgba, 255.0));
        nir_def *v0 = nir_fadd(b,
            nir_fmul_imm(b, nir_channel(b, bytes, 0), 256.0),
            nir_channel(b, bytes, 1));
        nir_def *v1 = nir_fadd(b,
            nir_fmul_imm(b, nir_channel(b, bytes, 2), 256.0),
            nir_channel(b, bytes, 3));
        nir_def *s0 = nir_fadd_imm(b, nir_fmul_imm(b, v0, 1.0 / R300_MP_C16_SCALE),
                                   -R300_MP_C16_BIAS);
        nir_def *s1 = nir_fadd_imm(b, nir_fmul_imm(b, v1, 1.0 / R300_MP_C16_SCALE),
                                   -R300_MP_C16_BIAS);
        unsigned sa = L->num_byte + 2 * k, sc = sa + 1;
        scalars_out[L->order[sa]] = s0;
        if (sc < L->n)
            scalars_out[L->order[sc]] = s1;
    }
}

/* Decompose a deferred partition's shape so the cut criterion is visible from
 * the gate log: the control-flow structure (block count), the raw ALU/TEX mass,
 * and -- for a single-block program -- every SSA def crossing the naive budget
 * cut, with opcode and component count.  Measured on the three RS482 capacity
 * cases, this shows the deferral cause is frontier width, not control flow:
 * every case is single-block at this point (ifs bcsel-flatten upstream) with
 * frontiers of one vec4, four vec4s (an fmul/fabs/fneg tail over two bases),
 * and seven mixed-type defs of fifteen components. */
static void
r300_nir_fs_report_defer_shape(nir_shader *nir, unsigned per_pass_budget)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    unsigned blocks = 0, alu = 0, tex = 0;

    nir_foreach_block(block, impl) {
        blocks++;
        nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu)
                alu++;
            else if (instr->type == nir_instr_type_tex)
                tex++;
        }
    }
    fprintf(stderr, "r300 FS multipass defer shape: blocks=%u alu=%u tex=%u\n",
            blocks, alu, tex);
    if (!exec_list_is_singular(&impl->body))
        return;

    nir_block *block = nir_start_block(impl);
    unsigned idx = 0;
    nir_foreach_instr(instr, block)
        instr->index = idx++;

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
        return;

    nir_foreach_instr(instr, block) {
        if (instr->index > cut->index)
            break;
        if (instr->type != nir_instr_type_alu)
            continue;
        nir_alu_instr *alu_instr = nir_instr_as_alu(instr);
        nir_def *def = &alu_instr->def;
        nir_foreach_use(use, def) {
            if (nir_src_is_if(use))
                continue;
            if (nir_src_use_instr(use)->index > cut->index) {
                fprintf(stderr,
                        "r300 FS multipass defer frontier: idx=%u op=%s "
                        "comps=%u\n",
                        instr->index, nir_op_infos[alu_instr->op].name,
                        def->num_components);
                break;
            }
        }
    }
}

/* Recompute the partition inside a clone.  nir_shader_clone preserves
 * instruction order, so re-running the deterministic collection at the same
 * cut index yields the clone's copies of the carried bases; the shape must
 * match the original decision exactly or the split is abandoned. */
static bool
r300_mp_recollect(nir_shader *sh, const struct r300_mp_partition *want,
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

/* Emit pass A: everything up to the cut, with the program's own outputs
 * replaced by the tier-encoded carry written across the scratch colour
 * targets r300_mp_build_layout allocates (FRAG_RESULT_DATA0..3).  The
 * original output variables and
 * every store to them are dropped -- a leftover gl_FragColor variable would
 * turn write_all back on and broadcast one value over the per-MRT carries. */
static nir_shader *
r300_nir_fs_emit_pass_a(nir_shader *src, const struct r300_mp_partition *part,
                        unsigned *out_num_rts)
{
    nir_shader *a = nir_shader_clone(NULL, src);
    struct r300_mp_partition p;
    if (!r300_mp_recollect(a, part, &p)) {
        ralloc_free(a);
        return NULL;
    }

    nir_function_impl *impl = nir_shader_get_entrypoint(a);
    nir_block *block = nir_start_block(impl);

    /* Inherit the colour output's variable metadata (precision, interpolation
     * defaults) for the carry outputs before the originals go away. */
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

    /* Drop everything strictly after the cut.  Users go before their defs
     * (reverse order) so no removal ever leaves a live use behind. */
    nir_foreach_instr_reverse_safe(instr, block) {
        if (instr->index > p.cut_index)
            nir_instr_remove(instr);
    }

    /* Drop the remaining pre-cut output stores and, once storeless, the
     * output variables themselves. */
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

    struct r300_mp_layout layout;
    r300_mp_build_layout(&p, &layout);
    if (layout.num_rts > R300_MP_MAX_SCRATCH) {
        ralloc_free(a);
        return NULL;
    }

    nir_builder b = nir_builder_at(nir_after_block(block));
    nir_def *scalars[R300_MP_MAX_CARRY_COMPS];
    r300_mp_flatten_carries(&b, &p, scalars);
    nir_def *rt_colors[R300_MP_MAX_SCRATCH];
    r300_mp_pack_carries(&b, scalars, &layout, rt_colors);
    unsigned num_rts = layout.num_rts;

    for (unsigned k = 0; k < num_rts; k++) {
        char name[24];
        snprintf(name, sizeof(name), "r300_mp_carry%u", k);
        nir_variable *out = nir_variable_create(a, nir_var_shader_out,
                                                glsl_vec4_type(), name);
        out->data = proto_data;
        out->data.location = FRAG_RESULT_DATA0 + k;
        /* One vec4 slot per carry target: unique bases whichever io-lowering
         * keys on driver_location rather than the semantic location. */
        out->data.driver_location = k;
        nir_store_var(&b, out, rt_colors[k], 0xf);
        a->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DATA0 + k);
    }

    nir_index_ssa_defs(impl);
    nir_progress(true, impl, nir_metadata_none);
    nir_validate_shader(a, "r300 FS multipass pass A");
    *out_num_rts = num_rts;
    return a;
}

/* Emit pass B: the whole program with every carried base rewritten to the
 * value unpacked from the scratch targets pass A wrote.  The bases' producer
 * chains lose their uses and dead-code away inside the compile, leaving the
 * scratch reads plus the post-cut half; the modifier tails (fneg/fabs/mov)
 * over the carries stay live and fold into RC source modifiers for free.
 *
 * Each scratch is sampled through a RECT sampler at the fragment's window
 * coordinate: the compiler inserts the RC_STATE_R300_TEXRECT_FACTOR scale
 * (1/width, 1/height of the bound scratch) the driver fills, so no separate
 * uniform is needed; the draw orchestration allocates the scratch as
 * PIPE_TEXTURE_RECT and binds it at the matching sampler unit.  A program
 * that itself samples the displaced low units is not admitted here (the
 * carry samplers claim units 0..N-1); none of the capacity class uses
 * textures at all. */
static nir_shader *
r300_nir_fs_emit_pass_b(nir_shader *src, const struct r300_mp_partition *part)
{
    nir_shader *bsh = nir_shader_clone(NULL, src);
    struct r300_mp_partition p;
    if (!r300_mp_recollect(bsh, part, &p)) {
        ralloc_free(bsh);
        return NULL;
    }

    /* The carry samplers displace units 0..N-1; a program with its own
     * texture instructions would need the reserved-high-unit refinement. */
    nir_function_impl *impl = nir_shader_get_entrypoint(bsh);
    nir_block *block = nir_start_block(impl);
    nir_foreach_instr(instr, block) {
        if (instr->type == nir_instr_type_tex) {
            ralloc_free(bsh);
            return NULL;
        }
    }

    struct r300_mp_layout layout;
    r300_mp_build_layout(&p, &layout);
    if (layout.num_rts > R300_MP_MAX_SCRATCH) {
        ralloc_free(bsh);
        return NULL;
    }
    unsigned num_rts = layout.num_rts;
    nir_builder b = nir_builder_at(nir_before_block(block));
    nir_def *coord = nir_trim_vector(&b, nir_load_frag_coord(&b), 2);

    nir_def *rt_texs[R300_MP_MAX_SCRATCH];
    for (unsigned k = 0; k < num_rts; k++) {
        char name[24];
        snprintf(name, sizeof(name), "r300_mp_scratch%u", k);
        nir_variable *samp = nir_variable_create(
            bsh, nir_var_uniform,
            glsl_sampler_type(GLSL_SAMPLER_DIM_RECT, false, false,
                              GLSL_TYPE_FLOAT),
            name);
        samp->data.binding = k;
        nir_deref_instr *deref = nir_build_deref_var(&b, samp);
        rt_texs[k] = nir_tex(&b, coord, .texture_deref = deref,
                            .sampler_deref = deref);
    }
    nir_def *scalars[R300_MP_MAX_CARRY_COMPS];
    r300_mp_unpack_carries(&b, rt_texs, &layout, scalars);

    unsigned comp = 0;
    for (unsigned i = 0; i < p.num_bases; i++) {
        nir_def *base = p.bases[i];
        nir_def *value = nir_vec(&b, &scalars[comp], base->num_components);
        comp += base->num_components;
        if (p.base_type[i] == R300_MP_CARRY_INT)
            value = nir_f2i32(&b, value);
        else if (p.base_type[i] == R300_MP_CARRY_BOOL1)
            value = nir_fneu(&b, value, nir_imm_float(&b, 0.0f));
        nir_def_rewrite_uses(base, value);
    }

    nir_index_ssa_defs(impl);
    nir_progress(true, impl, nir_metadata_none);
    nir_validate_shader(bsh, "r300 FS multipass pass B");
    return bsh;
}

/* Emit both halves for one ranked cut candidate.  Returns false when the
 * candidate's shape does not re-collect in the clones. */
static bool
r300_nir_fs_partition(nir_shader *nir, const struct r300_mp_partition *part,
                      nir_shader **pass_a, nir_shader **pass_b,
                      unsigned *num_scratch)
{
    *pass_a = r300_nir_fs_emit_pass_a(nir, part, num_scratch);
    if (!*pass_a)
        return false;
    *pass_b = r300_nir_fs_emit_pass_b(nir, part);
    if (!*pass_b) {
        ralloc_free(*pass_a);
        *pass_a = NULL;
        return false;
    }
    return true;
}

/* The state tracker pushes inlinable-uniform values straight from uniform
 * storage, where GL integer uniforms live float-converted (NativeIntegers
 * is false, uniform_int_float storage).  The NIR consuming them at this
 * point still carries native integer ops -- int lowering runs later in the
 * front end -- so an integer-consumed uniform must decode back from its
 * float encoding before nir_inline_uniforms folds it, or the inlined bit
 * pattern (fui(1) = 0x3f800000) reads as a huge integer: a constant-folded
 * array index then clamps out of bounds to the last element and a loop
 * bound exceeds the unroll cap.  Classify each inlinable dword by its
 * load's consumers: any use whose nir_op_infos input type is int- or
 * uint-based marks the dword integer-consumed. */
static bool
r300_inlinable_comp_is_int(nir_def *def, unsigned comp, unsigned depth)
{
    if (depth > 8)
        return false;
    nir_foreach_use(use, def) {
        if (nir_src_is_if(use))
            continue;
        nir_instr *parent = nir_src_use_instr(use);
        if (parent->type == nir_instr_type_intrinsic) {
            /* An offset operand of another constant-buffer load is an
             * integer consumer (a folded dynamic index). */
            nir_intrinsic_instr *pi = nir_instr_as_intrinsic(parent);
            if ((pi->intrinsic == nir_intrinsic_load_ubo ||
                 pi->intrinsic == nir_intrinsic_load_ubo_vec4) &&
                use == &pi->src[1])
                return true;
            continue;
        }
        if (parent->type != nir_instr_type_alu)
            continue;
        nir_alu_instr *alu = nir_instr_as_alu(parent);
        for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
            if (&alu->src[i].src != use)
                continue;
            const unsigned width = nir_ssa_alu_instr_src_components(alu, i);
            for (unsigned c = 0; c < width; c++) {
                if (alu->src[i].swizzle[c] != comp)
                    continue;
                /* Typeless movers pass the component through; follow it
                 * to the real consumer. */
                if (alu->op == nir_op_mov ||
                    (alu->op == nir_op_bcsel && i > 0) ||
                    (alu->op == nir_op_b32csel && i > 0)) {
                    if (r300_inlinable_comp_is_int(&alu->def, c, depth + 1))
                        return true;
                    continue;
                }
                if (nir_op_is_vec(alu->op)) {
                    if (r300_inlinable_comp_is_int(&alu->def, i, depth + 1))
                        return true;
                    continue;
                }
                const nir_alu_type t = nir_alu_type_get_base_type(
                    nir_op_infos[alu->op].input_types[i]);
                if (t == nir_type_int || t == nir_type_uint)
                    return true;
            }
        }
    }
    return false;
}

static bool
r300_inlinable_dw_is_int(nir_shader *nir, unsigned dw_offset)
{
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type != nir_instr_type_intrinsic)
                    continue;
                nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                unsigned base_dw;
                if (intr->intrinsic == nir_intrinsic_load_ubo) {
                    if (!nir_src_is_const(intr->src[1]))
                        continue;
                    base_dw = nir_src_as_uint(intr->src[1]) / 4;
                } else if (intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
                    /* nir_lower_ubo_vec4 counts vec4 slots split across
                     * the base index, the offset source, and a starting
                     * component. */
                    if (!nir_src_is_const(intr->src[1]))
                        continue;
                    base_dw = (nir_intrinsic_base(intr) +
                               nir_src_as_uint(intr->src[1])) * 4 +
                              nir_intrinsic_component(intr);
                } else {
                    continue;
                }
                if (!nir_src_is_const(intr->src[0]) ||
                    nir_src_as_uint(intr->src[0]) != 0)
                    continue;
                if (dw_offset < base_dw ||
                    dw_offset >= base_dw + intr->def.num_components)
                    continue;
                if (r300_inlinable_comp_is_int(&intr->def,
                                               dw_offset - base_dw, 0))
                    return true;
            }
        }
    }
    return false;
}

static void
r300_decode_inlinable_values(nir_shader *nir, unsigned n,
                             const uint32_t *raw, const uint16_t *offsets,
                             uint32_t *out)
{
    for (unsigned i = 0; i < n; i++) {
        out[i] = raw[i];
        if (r300_inlinable_dw_is_int(nir, offsets[i]))
            out[i] = (uint32_t)(int32_t)uif(raw[i]);
    }
}

static void r300_translate_fragment_shader_body(
    struct r300_context* r300,
    struct r300_fragment_shader_code* shader,
    struct pipe_shader_state state)
{
    struct r300_fragment_program_compiler compiler;
    int face;
    unsigned i;
    union r300_shader_code code;
    code.f = shader;
    /* A classic-front-end program can exceed backend budgets nir_to_rc's
     * shapes stay under (temporaries, ALU slots, constants); retrying the
     * whole translation with the classic gate closed keeps gate-on failure
     * behavior a subset of gate-off instead of substituting the dummy
     * shader. */
    bool allow_classic = true;

retry:
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

    /* Polygon-stipple variant: rewrite the program to sample the 32x32
     * driver-owned stipple texture at the window position and discard masked
     * fragments.  The pass binds the sampler right past the program's own
     * bindings -- the same unit r300_merge_textures_and_samplers splices the
     * stipple texture into for stippled draws. */
    if (shader->compare_state.pstipple) {
        unsigned stipple_unit = 0;
        NIR_PASS(_, clone, nir_lower_pstipple_fs, &stipple_unit, 0, true,
                 nir_type_bool1);
        nir_shader_gather_info(clone, nir_shader_get_entrypoint(clone));
    }

    /* Specialize this variant on the inlinable-uniform values it was keyed on
     * (r300_pick_fragment_shader): inline the draw-time value and re-run the
     * optimizer so nir_opt_loop_unroll can resolve a now-constant loop bound
     * that r300_create_fs_state deferred (R300/R400 have no dynamic control
     * flow). If the loop still cannot be unrolled the ordinary compile path
     * reports the error for this variant only. */
    if (shader->num_inlinable > 0 && clone->info.num_inlinable_uniforms > 0) {
        /* The pushed values are ordered by the state tracker's offset
         * array; the clone's re-gathered info can differ in count and
         * order, so pair by offset identity.  A clone offset absent from
         * the snapshot has no pushed value and stays a uniform. */
        uint32_t aligned[MAX_INLINABLE_UNIFORMS];
        uint16_t aligned_offs[MAX_INLINABLE_UNIFORMS];
        unsigned n = 0;
        for (unsigned i = 0; i < clone->info.num_inlinable_uniforms; i++) {
            const uint16_t off = clone->info.inlinable_uniform_dw_offsets[i];
            const unsigned lim = MIN2(shader->st_num_inlinable,
                                      shader->num_inlinable);
            for (unsigned j = 0; j < lim; j++) {
                if (shader->st_inlinable_offsets[j] == off) {
                    aligned[n] = shader->inlinable_values[j];
                    aligned_offs[n] = off;
                    n++;
                    break;
                }
            }
        }
        uint32_t decoded[MAX_INLINABLE_UNIFORMS];
        if (n) {
            r300_decode_inlinable_values(clone, n, aligned, aligned_offs,
                                         decoded);
            nir_inline_uniforms(clone, n, decoded, aligned_offs);
        }
        r300_optimize_nir(clone, r300->screen);

        /* nir_opt_loop_unroll refuses a trip count past the FS unroll cap
         * (max_unroll_iterations), so a loop bound larger than that survives
         * the inline. nir_to_rc cannot lower R300/R400 control flow and would
         * spin on it, so fail this variant to the dummy FS rather than let the
         * compile hang. */
        if (!r300->screen->caps.is_r500) {
            char *msg = r300_check_control_flow(clone);
            if (msg) {
                ralloc_free(clone);
                r300_dummy_fragment_shader(r300, shader);
                shader->error = strdup(msg);
                return;
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

    /* Classic front end, default open; R300_USE_CLASSIC_FS=0 opts out.
     * Qualified on RS482 against the full deqp-gles2 functional suite with
     * zero attributable gate-on deltas: every rejection falls back to
     * nir_to_rc by name, non-plain external state never enters selection,
     * and a post-classic backend error retries the whole translation with
     * the gate closed, so failure behavior stays a subset of the nir_to_rc
     * path by construction.  The classic path selects the NIR into
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
        classic_gate = (e && strcmp(e, "0") == 0) ? 0 : 1;
    }
    if (classic_gate && allow_classic) {
        /* Selection carries the sampler-state lowerings itself (shadow
         * compare, RECT normalization, NPOT wrap emulation, 3D
         * clamp-and-scale, alpha-to-one) through the shared nir_to_rc
         * passes and the classic state-constant table, so every external
         * state enters the gate. */
        {
            void *cctx = ralloc_context(NULL);
            nir_shader *cclone = nir_shader_clone(cctx, clone);
            const struct r300_classic_target *ct = r300_classic_target_get(
                compiler.Base.is_r400, compiler.Base.is_r500);
            struct r300_shader_semantics classic_inputs;
            r300_shader_semantics_reset(&classic_inputs);
            struct r300_classic_select_result sel;
            struct r300_classic_regalloc_result ra;
            const char *why = NULL;
            if (!r300_classic_select(cctx, cclone, ct,
                                     &shader->compare_state, 0,
                                     &classic_inputs, &sel) ||
                !sel.program) {
                why = sel.reject_reason ? sel.reject_reason : "selection";
            } else if (!r300_classic_regalloc(cctx, sel.program, &ra) ||
                       !ra.temp_of_ssa) {
                why = ra.reject_reason ? ra.reject_reason : "allocation";
            } else if (!r300_classic_emit(sel.program, &sel.immediates,
                                          &sel.states, &compiler)) {
                why = "emission";
            } else {
                shader->inputs = classic_inputs;
                /* nir_to_rc() sets rc.f->uses_discard from s->info.fs.uses_discard
                 * (gathered before either front end runs); r300_update_ztop reads
                 * shader->uses_discard to disable ZTOP whenever the fragment
                 * shader can kill a pixel (r300_hyperz.c, ZTOP condition 2: texture
                 * kill instructions).  The classic front end emits KIL/KILP
                 * directly into rc_program without going through nir_to_rc, so it
                 * must copy the same NIR gather-info flag or a discarding
                 * classic-compiled shader leaves ZTOP enabled and its early depth
                 * write reaches the Z buffer before the pixel shader can discard
                 * it. */
                shader->uses_discard = clone->info.fs.uses_discard;
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
        if (classic_done) {
            if (DBG_ON(r300, DBG_FP))
                fprintf(stderr, "r300 classic FS backend retry: %s\n",
                        compiler.Base.ErrorMsg ? compiler.Base.ErrorMsg : "");
            rc_destroy(&compiler.Base);
            allow_classic = false;
            goto retry;
        }
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
        if (classic_done) {
            if (DBG_ON(r300, DBG_FP))
                fprintf(stderr, "r300 classic FS backend retry: %s\n",
                        compiler.Base.ErrorMsg ? compiler.Base.ErrorMsg : "");
            free(compiler.code->constants.Constants);
            free(compiler.code->constants_remap_table);
            rc_destroy(&compiler.Base);
            allow_classic = false;
            goto retry;
        }
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
        if (classic_done) {
            allow_classic = false;
            goto retry;
        }
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

/* Free a failed compile's allocations so the same code object can host a
 * fresh translation.  The variant key (compare_state, inlinable values) and
 * the list linkage stay; everything the compile produced goes. */
static void
r300_fs_code_reset(struct r300_fragment_shader_code *shader)
{
    FREE(shader->code.constants_remap_table);
    rc_constants_destroy(&shader->code.constants);
    FREE(shader->cb_code);
    free(shader->error);
    shader->error = NULL;
    shader->cb_code = NULL;
    shader->cb_code_size = 0;
    memset(&shader->code, 0, sizeof(shader->code));
    shader->dummy = false;
}

/* Translate a fragment shader, with the >64-ALU multipass partition as the
 * failure retry (R300_FS_MULTIPASS=1, default off).  The unsplit program
 * compiles first through the ordinary path -- both front ends, the classic
 * backend-retry belt -- so a program that fits never splits.  Only a compile
 * that died at the emit ceiling ("Too many ALU instructions", emit_alu in
 * r300_fragprog_emit.c) attempts the partition, and the split is adopted only
 * when BOTH halves compile clean: pass B into a partner code object the draw
 * path renders second, pass A into this shader's own code.  Any partition or
 * compile failure leaves the ordinary dummy-shader outcome untouched, so the
 * gate can only convert failures into passes, never the reverse. */
static void r300_translate_fragment_shader(
    struct r300_context* r300,
    struct r300_fragment_shader_code* shader,
    struct pipe_shader_state state)
{
    r300_translate_fragment_shader_body(r300, shader, state);

    static int mp_gate = -1;
    if (mp_gate < 0) {
        const char *e = getenv("R300_FS_MULTIPASS");
        mp_gate = (e && e[0] == '1') ? 1 : 0;
    }
    if (!mp_gate)
        return;
    /* The r500 and R400-envelope compilers have a 512-slot budget; the
     * partition exists for the plain R300 64-slot envelope. */
    if (r300->screen->caps.is_r500 || r300->screen->caps.is_r400 ||
        r300->screen->caps.hb_r400_us_envelope ||
        r300->screen->caps.hb_r400_us_alu_only)
        return;
    if (!shader->dummy || !shader->error ||
        !strstr(shader->error, "Too many ALU instructions"))
        return;

    struct r300_mp_partition cands[R300_MP_MAX_CANDIDATES];
    unsigned num_cands = r300_mp_find_cuts(state.ir.nir, cands,
                                           R300_MP_MAX_CANDIDATES);
    if (!num_cands) {
        fprintf(stderr, "r300 FS multipass: emit ceiling hit but no "
                        "admissible cut; compile stays failed\n");
        r300_nir_fs_report_defer_shape(state.ir.nir, 56);
        return;
    }

    /* Walk the ranked candidates: a serial chain may leave only a narrow
     * window where both halves plus their pack/unpack overhead compile, so
     * a candidate whose half rejects just moves the walk to the next cut. */
    for (unsigned ci = 0; ci < num_cands; ci++) {
        nir_shader *pass_a = NULL, *pass_b = NULL;
        unsigned num_scratch = 0;
        if (!r300_nir_fs_partition(state.ir.nir, &cands[ci], &pass_a,
                                   &pass_b, &num_scratch))
            continue;

        /* The partition leaves each half's severed chain in place (pass A's
         * post-cut half, pass B's carried producers); dead-code and
         * re-optimize both so the halves the backend counts are the real
         * halves, not the whole program twice. */
        r300_optimize_nir(pass_a, r300->screen);
        r300_optimize_nir(pass_b, r300->screen);

        /* Pass B first, into the partner code object: its verdict is free
         * to take without disturbing the failed compile this shader still
         * holds. */
        struct r300_fragment_shader_code *pb =
            CALLOC_STRUCT(r300_fragment_shader_code);
        if (!pb) {
            ralloc_free(pass_a);
            ralloc_free(pass_b);
            return;
        }
        pb->compare_state = shader->compare_state;
        memcpy(pb->inlinable_values, shader->inlinable_values,
               sizeof(pb->inlinable_values));
        pb->num_inlinable = shader->num_inlinable;
        memcpy(pb->st_inlinable_offsets, shader->st_inlinable_offsets,
               sizeof(pb->st_inlinable_offsets));
        pb->st_num_inlinable = shader->st_num_inlinable;

        struct pipe_shader_state pb_state = {
            .type = PIPE_SHADER_IR_NIR, .ir.nir = pass_b };
        r300_translate_fragment_shader_body(r300, pb, pb_state);
        ralloc_free(pass_b);
        if (pb->dummy || pb->error) {
            fprintf(stderr, "r300 FS multipass: cut %u pass B rejected "
                            "(%s)\n", cands[ci].cut_index,
                    pb->error ? pb->error : "dummy");
            r300_fs_code_reset(pb);
            FREE(pb);
            ralloc_free(pass_a);
            continue;
        }

        /* Pass A replaces the failed compile in place. */
        r300_fs_code_reset(shader);
        struct pipe_shader_state pa_state = {
            .type = PIPE_SHADER_IR_NIR, .ir.nir = pass_a };
        r300_translate_fragment_shader_body(r300, shader, pa_state);
        ralloc_free(pass_a);
        if (shader->dummy || shader->error) {
            fprintf(stderr, "r300 FS multipass: cut %u pass A rejected "
                            "(%s)\n", cands[ci].cut_index,
                    shader->error ? shader->error : "dummy");
            r300_fs_code_reset(pb);
            FREE(pb);
            continue;
        }

        shader->multipass_pass_b = pb;
        shader->multipass_num_scratch = num_scratch;
        fprintf(stderr, "r300 FS multipass: split admitted at cut %u "
                        "(pass A %u ALU + pass B %u ALU, %u scratch RT%s)\n",
                cands[ci].cut_index,
                shader->code.code.r300.alu.length,
                pb->code.code.r300.alu.length,
                num_scratch, num_scratch > 1 ? "s" : "");
        return;
    }
    fprintf(stderr, "r300 FS multipass: all %u cut candidates rejected; "
                    "compile stays failed\n", num_cands);
}

/* A compiled FS variant is keyed on the texture-compare state and on the
 * inlinable-uniform values it was specialized with, so a shader whose loop
 * bound changes at draw time gets a fresh unroll. */
static bool
r300_fs_variant_matches(const struct r300_fragment_shader_code *code,
                        const struct r300_fragment_program_external_state *state,
                        const struct r300_context *r300)
{
    return memcmp(&code->compare_state, state, sizeof(*state)) == 0 &&
           code->num_inlinable == r300->fs_num_inlinable &&
           memcmp(code->inlinable_values, r300->fs_inlinable_values,
                  r300->fs_num_inlinable * sizeof(uint32_t)) == 0;
}

static void
r300_fs_variant_set_key(struct r300_fragment_shader_code *code,
                        const struct r300_fragment_program_external_state *state,
                        const struct r300_context *r300,
                        const struct r300_fragment_shader *fs)
{
    code->compare_state = *state;
    /* Read each inlinable uniform's draw-time value straight from the
     * bound constant buffer at the offsets snapshotted from the shader's
     * own info: the values the state tracker pushes through
     * set_inlinable_constants are ordered by ITS program-info copy, which
     * diverges from the driver-finalized NIR's re-gathered offsets, and a
     * positional pairing then inlines the wrong uniform.  Sourcing by
     * offset makes the key and the inline self-consistent by
     * construction. */
    const struct r300_constant_buffer *cbuf =
        (struct r300_constant_buffer *)r300->fs_constants.state;
    code->num_inlinable = 0;
    code->st_num_inlinable = fs->st_num_inlinable;
    memcpy(code->st_inlinable_offsets, fs->st_inlinable_offsets,
           fs->st_num_inlinable * sizeof(uint16_t));
    if (cbuf && cbuf->ptr) {
        code->num_inlinable = fs->st_num_inlinable;
        for (unsigned j = 0; j < fs->st_num_inlinable; j++)
            code->inlinable_values[j] =
                cbuf->ptr[fs->st_inlinable_offsets[j]];
    }
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

        r300_fs_variant_set_key(fs->shader, state, r300, fs);
        r300_translate_fragment_shader(r300, fs->shader, fs->state);
        return true;

    } else {
        /* Check if the currently-bound shader has been compiled with the
         * texture-compare state and inlinable-uniform values we need. */
        if (!r300_fs_variant_matches(fs->shader, state, r300)) {
            /* Search for the right shader. */
            ptr = fs->first;
            while (ptr) {
                if (r300_fs_variant_matches(ptr, state, r300)) {
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

            r300_fs_variant_set_key(ptr, state, r300, fs);
            r300_translate_fragment_shader(r300, ptr, fs->state);
            return true;
        }
    }

    return false;
}
