/**********************************************************
 * Copyright 2009-2011 VMware, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *********************************************************
 * Authors:
 * Zack Rusin <zackr-at-vmware-dot-com>
 */
#include "xa_priv.h"

#include "util/format/u_formats.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "util/u_memory.h"

#include "compiler/nir/nir_builder.h"

#include "cso_cache/cso_context.h"
#include "cso_cache/cso_hash.h"

/* Vertex shader:
 * IN[0]    = vertex pos
 * IN[1]    = src tex coord | solid fill color
 * IN[2]    = mask tex coord
 * IN[3]    = dst tex coord
 * CONST[0] = (2/dst_width, 2/dst_height, 1, 1)
 * CONST[1] = (-1, -1, 0, 0)
 *
 * OUT[0]   = vertex pos
 * OUT[1]   = src tex coord
 * OUT[2]   = mask tex coord
 * OUT[3]   = dst tex coord
 */

/* Fragment shader. Samplers are allocated when needed.
 * SAMP[0]  = sampler for first texture (src or mask if src is solid)
 * SAMP[1]  = sampler for second texture (mask or none)
 * IN[0]    = first texture coordinates if present
 * IN[1]    = second texture coordinates if present
 * CONST[0] = Solid color (src if src solid or mask if mask solid
 *            or src in mask if both solid).
 *
 * OUT[0] = color
 */

struct xa_shaders {
    struct xa_context *r;

    struct cso_hash vs_hash;
    struct cso_hash fs_hash;
};

/* The XA shaders read their parameters from constant buffer 0, written per draw
 * by renderer_set_constants(); each parameter is the vec4 at index slot of
 * UBO block 0. */
static nir_def *
load_const(nir_builder *b, unsigned slot)
{
    /* Standard byte-addressed load_ubo; the byte offset is slot * 16 for vec4
     * alignment.  Backends that use nir_to_rc (r300) run nir_lower_ubo_vec4
     * in their compile pipeline, which converts this to load_ubo_vec4 before
     * RC translation. */
    return nir_load_ubo(b, 4, 32, nir_imm_int(b, 0), nir_imm_int(b, slot * 16),
                        .align_mul = 16, .align_offset = 0,
                        .range_base = 0, .range = ~0u);
}

static nir_variable *
decl_io(nir_builder *b, nir_variable_mode mode, int location, const char *name)
{
    nir_variable *v = nir_variable_create(b->shader, mode, glsl_vec4_type(), name);
    v->data.location = location;
    return v;
}

/* A GENERIC[idx] fragment-shader texcoord input, trimmed to the xy used for 2D
 * sampling and the in-bounds test. */
static nir_def *
fs_texcoord(nir_builder *b, unsigned idx)
{
    return nir_trim_vector(b,
        nir_load_var(b, decl_io(b, nir_var_shader_in, VARYING_SLOT_VAR0 + idx, "tc")), 2);
}

static nir_deref_instr *
decl_sampler(nir_builder *b, unsigned binding)
{
    const struct glsl_type *t =
        glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
    nir_variable *s = nir_variable_create(b->shader, nir_var_uniform, t, "samp");
    s->data.binding = binding;
    return nir_build_deref_var(b, s);
}

static nir_def *
tex2d(nir_builder *b, nir_deref_instr *samp, nir_def *coord)
{
    return nir_tex(b, coord, .texture_deref = samp, .sampler_deref = samp);
}

/* nir_to_rc / nir_to_tgsi size the external constant file from a declared UBO
 * variable's interface_type (ntr_setup_uniforms reads
 * glsl_get_explicit_size(var->interface_type)), not from load_ubo usage.  The
 * builder reads constants by raw vec4 slot, so without this declaration ubo_size
 * is 0, ntr_add_constants allocates no RC_CONSTANT_EXTERNAL, r300's
 * externals_count stays 0, and the driver binds zero constants -- the fragment
 * shader reads the solid colour and the YUV->RGB matrix as 0.  Declare the
 * default UBO sized to the slots the shader reads, mirroring
 * nir_lower_uniforms_to_ubo.  The vertex shader does not need this: nir_to_tgsi
 * feeds the SW-TCL draw module, which reads the bound constant buffer directly. */
static void
declare_const_ubo(nir_shader *s, unsigned num_slots)
{
    if (!num_slots)
        return;

    const struct glsl_type *type =
        glsl_array_type(glsl_vec4_type(), num_slots, 16);
    nir_variable *ubo =
        nir_variable_create(s, nir_var_mem_ubo, type, "xa_const_block");
    ubo->data.binding = 0;
    ubo->data.explicit_binding = 1;

    struct glsl_struct_field field = {
        .type = type,
        .name = "data",
        .location = -1,
    };
    ubo->interface_type =
        glsl_interface_type(&field, 1, GLSL_INTERFACE_PACKING_STD430, false,
                            "xa_const_interface");
    s->info.num_ubos = 1;
    s->info.first_ubo_is_default_ubo = true;
}

/* nir_to_tgsi (SW-TCL VS) and nir_to_rc (FS) key TGSI register / interpolator
 * indices off var->data.driver_location, which they read but never assign; the
 * gather_info + assign_io_var_locations pass below assigns it, else every IO
 * collapses onto slot 0.  finalize_nir is a screen hook r300 leaves NULL.
 *
 * nir_shader_gather_info counts sampler uniforms into info.num_textures but
 * does NOT set info.textures_used or info.samplers_used -- those are normally
 * populated by gl_nir_lower_samplers_as_deref in the GLSL pipeline.  Gallium
 * drivers (including llvmpipe) read these bitsets in make_variant_key to size
 * the sampler region of the shader variant key.  A zero count leaves the key
 * region un-zeroed on the stack, giving garbage format values and an assertion
 * failure in lp_build_sample_soa.  Populate the bitsets here for every sampler
 * uniform variable, mirroring gl_nir_lower_samplers_as_deref. */
static void
finalize(struct pipe_context *pipe, nir_builder *b, unsigned num_const_slots)
{
    declare_const_ubo(b->shader, num_const_slots);

    nir_shader_gather_info(b->shader, nir_shader_get_entrypoint(b->shader));
    nir_assign_io_var_locations(b->shader, nir_var_shader_in);
    nir_assign_io_var_locations(b->shader, nir_var_shader_out);

    nir_foreach_uniform_variable(var, b->shader) {
        if (glsl_type_is_sampler(glsl_without_array(var->type))) {
            unsigned count = MAX2(glsl_type_get_sampler_count(var->type), 1);
            BITSET_SET_COUNT(b->shader->info.textures_used, var->data.binding, count);
            BITSET_SET_COUNT(b->shader->info.samplers_used, var->data.binding, count);
        }
    }

    if (pipe->screen->finalize_nir)
        pipe->screen->finalize_nir(pipe->screen, b->shader, true);
}

static void *
create_vs(struct pipe_context *pipe, unsigned vs_traits)
{
    const nir_shader_compiler_options *opt =
        pipe->screen->nir_options[MESA_SHADER_VERTEX];
    nir_builder b =
        nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opt, "xa_vs");
    bool is_composite = (vs_traits & VS_COMPOSITE) != 0;
    bool has_mask = (vs_traits & VS_MASK) != 0;
    bool is_yuv = (vs_traits & VS_YUV) != 0;
    bool is_src_src = (vs_traits & VS_SRC_SRC) != 0;
    bool is_mask_src = (vs_traits & VS_MASK_SRC) != 0;
    unsigned input_slot = 0;

    /* CONST[0] = (2/dst_w, 2/dst_h, 1, 1), CONST[1] = (-1, -1, 0, 0): the
     * pixel-to-clip affine.  o_pos = pos * const0 + const1. */
    nir_def *pos = nir_load_var(&b,
        decl_io(&b, nir_var_shader_in, VERT_ATTRIB_GENERIC0 + input_slot++, "vpos"));
    nir_store_var(&b, decl_io(&b, nir_var_shader_out, VARYING_SLOT_POS, "opos"),
                  nir_ffma_weak(&b, pos, load_const(&b, 0), load_const(&b, 1)), 0xf);

    if (is_yuv) {
        nir_def *tc = nir_load_var(&b,
            decl_io(&b, nir_var_shader_in, VERT_ATTRIB_GENERIC0 + input_slot++, "vyuv"));
        nir_store_var(&b, decl_io(&b, nir_var_shader_out, VARYING_SLOT_VAR0, "oyuv"), tc, 0xf);
    }

    if (is_composite) {
        if (!is_src_src || (has_mask && !is_mask_src)) {
            nir_def *tc = nir_load_var(&b,
                decl_io(&b, nir_var_shader_in, VERT_ATTRIB_GENERIC0 + input_slot++, "vsrc"));
            nir_store_var(&b, decl_io(&b, nir_var_shader_out, VARYING_SLOT_VAR0, "osrc"), tc, 0xf);
        }
        if (!is_src_src && (has_mask && !is_mask_src)) {
            nir_def *tc = nir_load_var(&b,
                decl_io(&b, nir_var_shader_in, VERT_ATTRIB_GENERIC0 + input_slot++, "vmask"));
            nir_store_var(&b, decl_io(&b, nir_var_shader_out, VARYING_SLOT_VAR1, "omask"), tc, 0xf);
        }
    }

    finalize(pipe, &b, 0);
    struct pipe_shader_state state = {0};
    state.type = PIPE_SHADER_IR_NIR;
    state.ir.nir = b.shader;
    return pipe->create_vs_state(pipe, &state);
}

static void *
create_yuv_shader(struct pipe_context *pipe)
{
    const nir_shader_compiler_options *opt =
        pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
    nir_builder b =
        nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opt, "xa_yuv_fs");

    nir_def *pos = fs_texcoord(&b, 0);
    nir_def *y = nir_channel(&b, tex2d(&b, decl_sampler(&b, 0), pos), 0);
    nir_def *u = nir_channel(&b, tex2d(&b, decl_sampler(&b, 1), pos), 0);
    nir_def *v = nir_channel(&b, tex2d(&b, decl_sampler(&b, 2), pos), 0);

    /* RGB = matrow3 (bias) + y.x*matrow0 + u.x*matrow1 + v.x*matrow2: the
     * YUV->RGB affine, rows in const buffer 0 (BT.601, written by xa_yuv.c). */
    nir_def *rgb = load_const(&b, 3);
    rgb = nir_ffma_weak(&b, nir_replicate(&b, y, 4), load_const(&b, 0), rgb);
    rgb = nir_ffma_weak(&b, nir_replicate(&b, u, 4), load_const(&b, 1), rgb);
    rgb = nir_ffma_weak(&b, nir_replicate(&b, v, 4), load_const(&b, 2), rgb);

    nir_store_var(&b, decl_io(&b, nir_var_shader_out, FRAG_RESULT_COLOR, "col"), rgb, 0xf);
    finalize(pipe, &b, 4);
    struct pipe_shader_state state = {0};
    state.type = PIPE_SHADER_IR_NIR;
    state.ir.nir = b.shader;
    return pipe->create_fs_state(pipe, &state);
}

/* src * mask, respecting mask-luminance (mask in .x not .w) and component-alpha
 * (per-channel rather than broadcast-alpha) modes. */
static nir_def *
src_in_mask(nir_builder *b, nir_def *src, nir_def *mask,
            bool mask_luminance, bool component_alpha)
{
    if (mask_luminance) {
        if (component_alpha)
            return nir_vector_insert_imm(b, src,
                nir_fmul(b, nir_channel(b, src, 3), nir_channel(b, mask, 0)), 3);
        return nir_fmul(b, src, nir_replicate(b, nir_channel(b, mask, 0), 4));
    }
    if (!component_alpha)
        return nir_fmul(b, src, nir_replicate(b, nir_channel(b, mask, 3), 4));
    return nir_fmul(b, src, mask);
}

/* Texture fetch with optional BGR->RGB swizzle, forced alpha=1, and
 * clamp-to-border kill (coord outside [0,1) -> 0). */
static nir_def *
xrender_tex(nir_builder *b, nir_def *coord, nir_deref_instr *samp,
            bool repeat_none, bool swizzle, bool set_alpha)
{
    nir_def *texel = tex2d(b, samp, coord);
    if (swizzle)
        texel = nir_swizzle(b, texel, (unsigned[]){2, 1, 0, 3}, 4);
    if (set_alpha)
        texel = nir_vector_insert_imm(b, texel, nir_imm_float(b, 1.0f), 3);
    if (repeat_none) {
        nir_def *cx = nir_channel(b, coord, 0), *cy = nir_channel(b, coord, 1);
        nir_def *zero = nir_imm_float(b, 0.0f), *one = nir_imm_float(b, 1.0f);
        /* in = (0<cx) && (cx<1) && (0<cy) && (cy<1), as float 0/1 (SGT/SLT->MIN). */
        nir_def *inx = nir_fmin(b, nir_b2f32(b, nir_flt(b, zero, cx)),
                                   nir_b2f32(b, nir_flt(b, cx, one)));
        nir_def *iny = nir_fmin(b, nir_b2f32(b, nir_flt(b, zero, cy)),
                                   nir_b2f32(b, nir_flt(b, cy, one)));
        texel = nir_fmul(b, texel, nir_replicate(b, nir_fmin(b, inx, iny), 4));
    }
    return texel;
}

/* Either a solid color from the next constant slot (is_src), or a sampled and
 * xrender_tex-processed texture from the next sampler/texcoord pair. */
static nir_def *
read_input(nir_builder *b, bool repeat_none, bool swizzle, bool set_alpha,
           bool is_src, unsigned *cur_constant, unsigned *cur_sampler)
{
    if (is_src)
        return load_const(b, (*cur_constant)++);

    unsigned idx = (*cur_sampler)++;
    return xrender_tex(b, fs_texcoord(b, idx), decl_sampler(b, idx),
                       repeat_none, swizzle, set_alpha);
}

static void *
create_fs(struct pipe_context *pipe, unsigned fs_traits)
{
    bool has_mask = (fs_traits & FS_MASK) != 0;
    bool is_yuv = (fs_traits & FS_YUV) != 0;
    bool src_repeat_none = (fs_traits & FS_SRC_REPEAT_NONE) != 0;
    bool mask_repeat_none = (fs_traits & FS_MASK_REPEAT_NONE) != 0;
    bool src_swizzle = (fs_traits & FS_SRC_SWIZZLE_RGB) != 0;
    bool mask_swizzle = (fs_traits & FS_MASK_SWIZZLE_RGB) != 0;
    bool src_set_alpha = (fs_traits & FS_SRC_SET_ALPHA) != 0;
    bool mask_set_alpha = (fs_traits & FS_MASK_SET_ALPHA) != 0;
    bool src_luminance = (fs_traits & FS_SRC_LUMINANCE) != 0;
    bool mask_luminance = (fs_traits & FS_MASK_LUMINANCE) != 0;
    bool dst_luminance = (fs_traits & FS_DST_LUMINANCE) != 0;
    bool is_src_src = (fs_traits & FS_SRC_SRC) != 0;
    bool is_mask_src = (fs_traits & FS_MASK_SRC) != 0;
    bool component_alpha = (fs_traits & FS_CA) != 0;
    unsigned cur_sampler = 0, cur_constant = 0;

    if (is_yuv)
        return create_yuv_shader(pipe);

    const nir_shader_compiler_options *opt =
        pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
    nir_builder b =
        nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opt, "xa_fs");

    nir_def *src = read_input(&b, src_repeat_none, src_swizzle, src_set_alpha,
                              is_src_src, &cur_constant, &cur_sampler);

    /* L8 source: collapse the luma (src.x) into (0, 0, 0, luma). */
    if (src_luminance)
        src = nir_vec4(&b, nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f),
                       nir_imm_float(&b, 0.0f), nir_channel(&b, src, 0));

    nir_def *result = src;
    if (has_mask) {
        nir_def *mask = read_input(&b, mask_repeat_none, mask_swizzle,
                                   mask_set_alpha, is_mask_src,
                                   &cur_constant, &cur_sampler);
        result = src_in_mask(&b, src, mask, mask_luminance, component_alpha);
    }

    /* L8 destination: the alpha/luma channel is what the L8 surface stores. */
    if (dst_luminance)
        result = nir_replicate(&b, nir_channel(&b, result, 3), 4);

    nir_store_var(&b, decl_io(&b, nir_var_shader_out, FRAG_RESULT_COLOR, "col"), result, 0xf);
    finalize(pipe, &b, cur_constant);
    struct pipe_shader_state state = {0};
    state.type = PIPE_SHADER_IR_NIR;
    state.ir.nir = b.shader;
    return pipe->create_fs_state(pipe, &state);
}

struct xa_shaders *
xa_shaders_create(struct xa_context *r)
{
    struct xa_shaders *sc = CALLOC_STRUCT(xa_shaders);

    sc->r = r;
    cso_hash_init(&sc->vs_hash);
    cso_hash_init(&sc->fs_hash);

    return sc;
}

static void
cache_destroy(struct pipe_context *pipe,
	      struct cso_hash *hash, unsigned processor)
{
    struct cso_hash_iter iter = cso_hash_first_node(hash);

    while (!cso_hash_iter_is_null(iter)) {
	void *shader = (void *)cso_hash_iter_data(iter);

	if (processor == MESA_SHADER_FRAGMENT) {
	    pipe->delete_fs_state(pipe, shader);
	} else if (processor == MESA_SHADER_VERTEX) {
	    pipe->delete_vs_state(pipe, shader);
	}
	iter = cso_hash_erase(hash, iter);
    }
    cso_hash_deinit(hash);
}

void
xa_shaders_destroy(struct xa_shaders *sc)
{
    cache_destroy(sc->r->pipe, &sc->vs_hash, MESA_SHADER_VERTEX);
    cache_destroy(sc->r->pipe, &sc->fs_hash, MESA_SHADER_FRAGMENT);

    FREE(sc);
}

static inline void *
shader_from_cache(struct pipe_context *pipe,
		  unsigned type, struct cso_hash *hash, unsigned key)
{
    void *shader = 0;

    struct cso_hash_iter iter = cso_hash_find(hash, key);

    if (cso_hash_iter_is_null(iter)) {
	if (type == MESA_SHADER_VERTEX)
	    shader = create_vs(pipe, key);
	else
	    shader = create_fs(pipe, key);
	cso_hash_insert(hash, key, shader);
    } else
	shader = (void *)cso_hash_iter_data(iter);

    return shader;
}

struct xa_shader
xa_shaders_get(struct xa_shaders *sc, unsigned vs_traits, unsigned fs_traits)
{
    struct xa_shader shader = { NULL, NULL };
    void *vs, *fs;

    vs = shader_from_cache(sc->r->pipe, MESA_SHADER_VERTEX,
			   &sc->vs_hash, vs_traits);
    fs = shader_from_cache(sc->r->pipe, MESA_SHADER_FRAGMENT,
			   &sc->fs_hash, fs_traits);

    assert(vs && fs);
    if (!vs || !fs)
	return shader;

    shader.vs = vs;
    shader.fs = fs;

    return shader;
}
