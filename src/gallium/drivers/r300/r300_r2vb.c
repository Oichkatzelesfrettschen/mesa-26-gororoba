/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * r300_r2vb.c -- RS482 render-to-vertex-buffer (R2VB) synthesized-vertex loop.
 *
 * RS482 ordinary draws keep num_vert_fpus = 0 and has_tcl = false, so a normal
 * draw transforms vertices on the CPU through the gallivm SWTCL draw module
 * instead of the VAP/PVS hardware vertex-shader route.  The R2VB idea moves the
 * transform onto the fragment ALU: pass 1 renders the
 * transformed (clip-space) vertices into a GTT buffer through the color buffer,
 * a cache barrier makes them visible, and pass 2 re-ingests that same buffer as
 * the vertex array via an in-IB LOAD_VBPNTR, drawn by the VAP in TCL_BYPASS (the
 * pre-transformed path where the VAP fetches already-transformed vertices and
 * does not execute PVS microcode).
 *
 * Scope and safety.  The data path is the driver's ordinary color-buffer write,
 * the cb_flush_clean barrier, a LOAD_VBPNTR vertex fetch, and a TCL_BYPASS draw.
 * The function is built for no-submit capture and explicit raw-submit
 * experiments, but is not wired into ordinary drawing.  Constants are the real
 * r300_reg.h values and the barrier matches r300_emit_gpu_flush / the
 * cb_flush_clean sequence in r300_context.c; no speculative fallback defines.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

#include "r300_context.h"
#include "r300_cs.h"
#include "r300_emit.h"
#include "r300_fs.h"
#include "r300_r2vb.h"
#include "r300_reg.h"
#include "r300_vs.h"

static struct pipe_resource *r2vb_create_selftest_bo(struct r300_context *r300,
                                                     uint32_t width_bytes, uint32_t fill_val)
{
    struct pipe_screen *pscreen = r300->context.screen;
    struct pipe_resource templ = { 0 };
    templ.target = PIPE_BUFFER;
    templ.format = PIPE_FORMAT_R32G32B32A32_FLOAT;
    templ.width0 = width_bytes;
    templ.height0 = 1;
    templ.depth0 = 1;
    templ.array_size = 1;
    /* PIPE_BIND_CUSTOM is load-bearing: force winsys allocation for R2VB. */
    templ.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_CUSTOM;
    templ.usage = PIPE_USAGE_DEFAULT;

    struct pipe_resource *res = pscreen->resource_create(pscreen, &templ);
    if (!res)
        return NULL;

    if (fill_val != 0) {
        struct pipe_transfer *xfer = NULL;
        struct pipe_box box = { .width = width_bytes, .height = 1, .depth = 1 };
        void *map = r300->context.buffer_map(&r300->context, res, 0, PIPE_MAP_WRITE, &box, &xfer);
        if (map) {
            memset(map, fill_val, width_bytes);
            r300->context.buffer_unmap(&r300->context, xfer);
        }
    }
    return res;
}

static void r2vb_report_stage3_readback(struct r300_context *r300, struct pipe_resource *stage3,
                                        uint32_t s3dim)
{
    struct pipe_transfer *rd_xfer = NULL;
    struct pipe_box box = { .width = s3dim * s3dim * 16, .height = 1, .depth = 1 };
    const uint32_t *texels =
        r300->context.buffer_map(&r300->context, stage3, 0, PIPE_MAP_READ, &box, &rd_xfer);
    if (!texels)
        return;

    uint32_t written = 0;
    uint32_t min_x = s3dim, min_y = s3dim, max_x = 0, max_y = 0;
    uint64_t sum_x = 0, sum_y = 0;
    for (uint32_t i = 0; i < s3dim * s3dim; i++) {
        if (texels[i * 4] != 0xffffffffu) {
            uint32_t x = i % s3dim, y = i / s3dim;
            if (x < min_x)
                min_x = x;
            if (y < min_y)
                min_y = y;
            if (x > max_x)
                max_x = x;
            if (y > max_y)
                max_y = y;
            sum_x += x;
            sum_y += y;
            written++;
        }
    }
    if (written)
        fprintf(stderr,
                "r2vb_stage3_readback dim=%ux%u written_texels=%u "
                "bbox=%u,%u,%u,%u centroid=%.1f,%.1f\n",
                s3dim, s3dim, written, min_x, min_y, max_x, max_y, (double)sum_x / written,
                (double)sum_y / written);
    else
        fprintf(stderr,
                "r2vb_stage3_readback dim=%ux%u written_texels=0 "
                "bbox=none centroid=none\n",
                s3dim, s3dim);

    r300->context.buffer_unmap(&r300->context, rd_xfer);
}

static void r2vb_report_bo_a_diagnostic(struct r300_context *r300, struct pipe_resource *res,
                                        uint32_t num_vertices)
{
    struct pipe_transfer *a_xfer = NULL;
    struct pipe_box abox = { .width = num_vertices * 16, .height = 1, .depth = 1 };
    const float *av =
        r300->context.buffer_map(&r300->context, res, 0, PIPE_MAP_READ, &abox, &a_xfer);
    if (!av)
        return;

    uint32_t n = num_vertices < 8 ? num_vertices : 8;
    for (uint32_t i = 0; i < n; i++)
        fprintf(stderr,
                "r2vb_stage1_bo_a slot=%u m0=%.3f m1=%.3f "
                "m2=%.3f m3=%.3f\n",
                i, av[i * 4 + 0], av[i * 4 + 1], av[i * 4 + 2], av[i * 4 + 3]);

    r300->context.buffer_unmap(&r300->context, a_xfer);
}

/* Build (once) the 4-DP4 transform fragment program: gl_FragColor = M * in_attr,
 * where in_attr is the per-slot input vertex delivered as a flat GENERIC input
 * and M's four rows live in FS const file 0 (the route sets them per draw from
 * the transposed MVP).  r300 lowers an FS load_ubo(binding 0) to RC_FILE_CONSTANT
 * (nir_to_rc: lower_uniforms_to_ubo + nir_lower_ubo_vec4 -> ntr_emit_load_ubo),
 * so each fdot4(row, in) compiles to one DP4 reading the const file.  Cached on
 * the context; create_fs_state takes ownership of the NIR and precompiles, so
 * RADEON_DEBUG=fp prints the four-DP4 r300 program at creation -- no submit. */
static void *r300_r2vb_get_transform_fs(struct r300_context *r300)
{
    if (r300->r2vb_transform_fs)
        return r300->r2vb_transform_fs;

    const nir_shader_compiler_options *options =
        r300->screen->screen.nir_options[MESA_SHADER_FRAGMENT];
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, options,
                                                   "r300 r2vb mvp transform FS");

    nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                           glsl_vec4_type(), "in_vtx");
    in->data.location = VARYING_SLOT_VAR0;
    in->data.interpolation = INTERP_MODE_FLAT;
    nir_def *v = nir_load_var(&b, in);

    nir_def *zero = nir_imm_int(&b, 0);
    nir_def *comp[4];
    for (unsigned r = 0; r < 4; r++) {
        /* Matrix row r from FS UBO[0] at byte offset r*16; one DP4 per output. */
        nir_def *row = nir_load_ubo(&b, 4, 32, zero, nir_imm_int(&b, r * 16),
                                    .align_mul = 16, .range = 64);
        comp[r] = nir_fdot(&b, row, v);
    }
    nir_def *o = nir_vec4(&b, comp[0], comp[1], comp[2], comp[3]);

    nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out_color");
    out->data.location = FRAG_RESULT_COLOR;
    nir_store_var(&b, out, o, 0xf);

    /* Declare a sized block-0 UBO variable so nir_to_rc's ntr_setup_uniforms
     * counts the four matrix rows as RC_CONSTANT_EXTERNAL.  The compiler sizes
     * the const file from the nir_var_mem_ubo interface declaration; a raw
     * load_ubo with only info.num_ubos set carries no size, so externals_count
     * stays 0 and set_constant_buffer(FRAGMENT, 0) is silently ignored (the FS
     * dots its input against garbage).  Same shape r300vk_shape_block0_ubo uses
     * for the push-constant UBO: glsl_array_type(vec4, n, 16) under a one-field
     * std430 interface (4 vec4 rows = 64 bytes). */
    b.shader->info.num_ubos = 1;
    const struct glsl_type *ubo_type = glsl_array_type(glsl_vec4_type(), 4, 16);
    nir_variable *ubo = nir_variable_create(b.shader, nir_var_mem_ubo, ubo_type,
                                            "r2vb_mvp_ubo");
    ubo->data.driver_location = 0;
    ubo->data.binding = 0;
    ubo->data.explicit_binding = 1;
    struct glsl_struct_field ubo_field = {
        .type = ubo_type,
        .name = "data",
        .location = -1,
    };
    ubo->interface_type = glsl_interface_type(&ubo_field, 1,
                                              GLSL_INTERFACE_PACKING_STD430, false,
                                              "__r300_r2vb_mvp_ubo");

    if (getenv("R300_R2VB_VS_DUMP"))
        nir_print_shader(b.shader, stderr);

    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = b.shader; /* create_fs_state takes ownership and precompiles */
    r300->r2vb_transform_fs = r300->context.create_fs_state(&r300->context, &st);
    return r300->r2vb_transform_fs;
}

/* Minimal pass-through VS bound for a producer pass: one position input and one
 * generic attribute, two outputs (gl_Position + a single VAR0 varying).  The
 * producer renders points through PRIM_WALK_VERTEX_EMBEDDED at TCL_BYPASS, so this
 * VS never runs -- only its output signature is read by update_derived_state,
 * which sizes VAP_OUT_VTX_FMT, the PSC stream count, and the RS block.  Binding it
 * pins those to the producer's two-vec4 embedded vertex regardless of how many
 * varyings the application VS declares; an application VS with two or more
 * varyings otherwise inflates the PSC and the VAP fetches past the embedded
 * vertex.  Cached on the context. */
static void *r300_r2vb_get_producer_vs(struct r300_context *r300)
{
    if (r300->r2vb_producer_vs)
        return r300->r2vb_producer_vs;

    const nir_shader_compiler_options *options =
        r300->screen->screen.nir_options[MESA_SHADER_VERTEX];
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                   "r300 r2vb producer VS");

    nir_variable *in_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_vec4_type(), "in_pos");
    in_pos->data.location = VERT_ATTRIB_GENERIC0;
    nir_variable *in_attr = nir_variable_create(b.shader, nir_var_shader_in,
                                                glsl_vec4_type(), "in_attr");
    in_attr->data.location = VERT_ATTRIB_GENERIC1;

    nir_variable *out_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_vec4_type(), "gl_Position");
    out_pos->data.location = VARYING_SLOT_POS;
    nir_variable *out_var = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_vec4_type(), "var0");
    out_var->data.location = VARYING_SLOT_VAR0;

    nir_store_var(&b, out_pos, nir_load_var(&b, in_pos), 0xf);
    nir_store_var(&b, out_var, nir_load_var(&b, in_attr), 0xf);

    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = b.shader; /* create_vs_state takes ownership and precompiles */
    r300->r2vb_producer_vs = r300->context.create_vs_state(&r300->context, &st);
    return r300->r2vb_producer_vs;
}

/* Build a transform-FS with the MVP baked in as immediates: out = M * in_attr,
 * where the four rows (already transposed so DP4(row_i, v) = (M*v)_i) are
 * nir_imm_vec4 constants.  Hand-built load_ubo on r300 folds to immediate
 * garbage (externals_count=0, so set_constant_buffer is ignored), so the matrix
 * must travel in the program; the FS is therefore matrix-specific and rebuilt
 * per draw (the caller deletes it).  Returns a pipe FS CSO or NULL. */
static void *r300_r2vb_build_baked_transform_fs(struct r300_context *r300, const float rows[16])
{
    const nir_shader_compiler_options *options =
        r300->screen->screen.nir_options[MESA_SHADER_FRAGMENT];
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, options,
                                                   "r300 r2vb mvp baked transform FS");
    nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                           glsl_vec4_type(), "in_vtx");
    in->data.location = VARYING_SLOT_VAR0;
    in->data.interpolation = INTERP_MODE_FLAT;
    nir_def *v = nir_load_var(&b, in);

    nir_def *comp[4];
    for (unsigned r = 0; r < 4; r++) {
        nir_def *row = nir_imm_vec4(&b, rows[r * 4 + 0], rows[r * 4 + 1],
                                    rows[r * 4 + 2], rows[r * 4 + 3]);
        comp[r] = nir_fdot(&b, row, v);
    }
    nir_def *o = nir_vec4(&b, comp[0], comp[1], comp[2], comp[3]);
    nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out_color");
    out->data.location = FRAG_RESULT_COLOR;
    nir_store_var(&b, out, o, 0xf);

    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = b.shader;
    return r300->context.create_fs_state(&r300->context, &st);
}

/* Re-stage the bound vertex shader as the producer fragment shader (position
 * only): clone the VS NIR, keep its arithmetic verbatim, and remap only the I/O
 * semantics so the fragment ALU runs it.  Each VS attribute input becomes a flat
 * fragment input at VARYING_SLOT_VAR0+i (the producer feeds one attribute per
 * output slot, flat); gl_Position (VARYING_SLOT_POS) becomes FRAG_RESULT_DATA0
 * (the clip BO).  Stores to any other (varying) output are dropped -- an
 * MVP-class VS passes its varyings through unchanged, so the multi-stream
 * re-ingest reads them from the application buffers -- and the inputs that fed
 * only those stores die in DCE.  The VS reads its matrix from UBO[0]; nir_to_rc
 * sizes the const file from that block-0 interface (the same externals path
 * r300_r2vb_get_transform_fs relies on), so the caller loads FS const file 0
 * with the matrix the VS expects (untransposed -- the VS body is column-MAD,
 * reading the matrix columns via load_ubo_vec4, not the DP4-transposed rows).
 *
 * This derives the producer from the real shader instead of hand-building the
 * 4-DP4 transform: the arithmetic, the constant reads, and the output count all
 * come from the VS, and nir_to_rc's stage-aware compile (it lowers I/O and emits
 * both VS and FS varyings) does the rest.  Returns a pipe FS CSO or NULL. */
static void *r300_r2vb_restage_vs_as_fs(struct r300_context *r300, nir_shader *vs_nir,
                                        gl_varying_slot target)
{
    nir_shader *fs = nir_shader_clone(NULL, vs_nir);
    if (!fs)
        return NULL;
    fs->info.stage = MESA_SHADER_FRAGMENT;

    /* VS vertex attributes -> flat fragment inputs at VAR0+i. */
    unsigned in_idx = 0;
    nir_foreach_variable_with_modes(var, fs, nir_var_shader_in) {
        var->data.location = VARYING_SLOT_VAR0 + in_idx++;
        var->data.location_frac = 0;
        var->data.interpolation = INTERP_MODE_FLAT;
    }

    /* The producer renders one output per pass, so keep only the store to the
     * target output and drop the rest.  Position pass: target = VARYING_SLOT_POS
     * (the dropped varyings come from the application buffers in the re-ingest).
     * Computed-varying pass: target = that varying's slot, and the now-unstored
     * position transform drops in DCE below, leaving just the varying arithmetic. */
    nir_function_impl *impl = nir_shader_get_entrypoint(fs);
    nir_foreach_block(block, impl) {
        nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
                continue;
            nir_variable *out = nir_intrinsic_get_var(intr, 0);
            if (out && (out->data.mode & nir_var_shader_out) &&
                out->data.location != target)
                nir_instr_remove(instr);
        }
    }

    /* Target output -> colour0 (the producer BO this pass writes). */
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
    nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));

    if (getenv("R300_R2VB_VS_DUMP"))
        nir_print_shader(fs, stderr);

    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = fs; /* create_fs_state takes ownership and precompiles */
    return r300->context.create_fs_state(&r300->context, &st);
}

/* Find the bound VS's first computed-varying output: a non-position output whose
 * store value is not a straight load of a vertex input.  The multi-pass producer
 * renders such a varying on the fragment ALU.  Returns its gl_varying_slot, or -1
 * when every non-position output is a plain passthrough (nothing to produce). */
static int r300_r2vb_first_computed_varying(nir_shader *vs_nir)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(vs_nir);
    if (!impl)
        return -1;
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
            if (val && val->intrinsic == nir_intrinsic_load_deref) {
                nir_variable *src = nir_intrinsic_get_var(val, 0);
                if (src && (src->data.mode & nir_var_shader_in))
                    continue; /* passthrough -- the re-ingest reads the app buffer */
            }
            return out->data.location;
        }
    }
    return -1;
}

/* Data-independent producer position stream: one window-space point per output
 * slot at pixel (slot+0.5, 0.5, 0, 1), FP32x4.  Reused across draws, grown when
 * a draw needs more slots.  PIPE_BIND_CUSTOM forces a real winsys BO (the SWTCL
 * path otherwise keeps vertex resources as a CPU shadow with no ->buf, which a
 * LOAD_VBPNTR cannot fetch). */
static struct pipe_resource *r300_r2vb_get_slot_pos_bo(struct r300_context *r300,
                                                       unsigned count)
{
    if (r300->r2vb_slot_pos_bo && r300->r2vb_slot_pos_count >= count)
        return r300->r2vb_slot_pos_bo;

    pipe_resource_reference(&r300->r2vb_slot_pos_bo, NULL);
    unsigned cap = align(count, 64);
    struct pipe_screen *pscreen = r300->context.screen;
    struct pipe_resource templ = {0};
    templ.target = PIPE_BUFFER;
    templ.format = PIPE_FORMAT_R32G32B32A32_FLOAT;
    templ.width0 = cap * 16;
    templ.height0 = 1;
    templ.depth0 = 1;
    templ.array_size = 1;
    templ.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_CUSTOM;
    templ.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *bo = pscreen->resource_create(pscreen, &templ);
    if (!bo)
        return NULL;

    struct pipe_transfer *xfer = NULL;
    struct pipe_box box = { .width = cap * 16, .height = 1, .depth = 1 };
    float *map = r300->context.buffer_map(&r300->context, bo, 0, PIPE_MAP_WRITE, &box, &xfer);
    if (map) {
        for (unsigned s = 0; s < cap; s++) {
            map[s * 4 + 0] = (float)s + 0.5f;
            map[s * 4 + 1] = 0.5f;
            map[s * 4 + 2] = 0.0f;
            map[s * 4 + 3] = 1.0f;
        }
        r300->context.buffer_unmap(&r300->context, xfer);
    }
    r300->r2vb_slot_pos_bo = bo;
    r300->r2vb_slot_pos_count = cap;
    return bo;
}

/* No-submit self-check for the MVP producer components: build the transform-FS
 * (create_fs_state precompiles it, so RADEON_DEBUG=fp prints the 4-DP4 program)
 * and the slot-pixel BO, then read back the first slots to confirm the position
 * sequence.  Pure CPU setup; no draw, no GPU work. */
static void r300_r2vb_mvp_init_selftest(struct r300_context *r300, unsigned count)
{
    void *fs = r300_r2vb_get_transform_fs(r300);
    struct pipe_resource *bo = r300_r2vb_get_slot_pos_bo(r300, count);
    fprintf(stderr, "r2vb_mvp_init transform_fs=%p slot_pos_bo=%p req_count=%u cap=%u\n",
            fs, (void *)bo, count, r300->r2vb_slot_pos_count);
    if (!bo)
        return;
    struct pipe_transfer *xfer = NULL;
    struct pipe_box box = { .width = (count ? count : 1) * 16, .height = 1, .depth = 1 };
    const float *m = r300->context.buffer_map(&r300->context, bo, 0, PIPE_MAP_READ, &box, &xfer);
    if (m) {
        for (unsigned s = 0; s < count && s < 4; s++)
            fprintf(stderr, "  slot[%u] pos=%.1f,%.1f,%.1f,%.1f\n", s, m[s * 4 + 0],
                    m[s * 4 + 1], m[s * 4 + 2], m[s * 4 + 3]);
        r300->context.buffer_unmap(&r300->context, xfer);
    }
}

/* Fixed test matrix for the R2VB transform self-check (column-major): a
 * scale-by-2 + translate-by-5 in x and y, so M*v = (2x+5w, 2y+5w, 2z, w).
 * Distinct enough that a transpose or routing error is visible in the readback. */
static const float r2vb_test_mvp_cols[16] = {
    2, 0, 0, 0,  0, 2, 0, 0,  0, 0, 2, 0,  5, 5, 0, 1,
};
/* set_constant_buffer keeps the pointer, not a copy, so the transposed rows need
 * process-lifetime storage. */
static float r2vb_mvp_rows[16];

/* Load FS const file 0 with the transpose of a column-major MVP so each DP4 row
 * is a matrix row: DP4(row_i, v) = (M*v)_i, row_i = (col0[i],col1[i],col2[i],col3[i]). */
static void r300_r2vb_set_transform_consts(struct r300_context *r300, const float *cols)
{
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = 0; j < 4; j++)
            r2vb_mvp_rows[i * 4 + j] = cols[j * 4 + i];
    struct pipe_constant_buffer cb = {0};
    cb.buffer_size = 64;
    cb.user_buffer = r2vb_mvp_rows;
    r300->context.set_constant_buffer(&r300->context, MESA_SHADER_FRAGMENT, 0, &cb);
}

/* Process-lifetime copy for the re-staged producer: set_constant_buffer keeps the
 * pointer, not a copy. */
static float r2vb_mvp_cols[16];

/* Load FS const file 0 with the matrix in the layout the re-staged VS expects --
 * the column-major columns verbatim, not the DP4 transpose -- because the
 * re-staged body reads load_ubo_vec4 base=i (column i) and forms
 * col0*x + col1*y + col2*z + col3*w. */
static void r300_r2vb_set_transform_consts_raw(struct r300_context *r300, const float *cols)
{
    memcpy(r2vb_mvp_cols, cols, sizeof(r2vb_mvp_cols));
    struct pipe_constant_buffer cb = {0};
    cb.buffer_size = 64;
    cb.user_buffer = r2vb_mvp_cols;
    r300->context.set_constant_buffer(&r300->context, MESA_SHADER_FRAGMENT, 0, &cb);
}

/* No-submit decode of the producer's VAP-stream routing against the bound FS's
 * RS routing.  The producer reuses the inherited VAP_PROG_STREAM_CNTL (which
 * vector each fetched element lands in, DST_VEC_LOC), while the bound transform-
 * FS's rs_block decides which VAP output vector feeds each FS input (rs ip).
 * The attribute reaches the FS iff the stream-1 DST_VEC_LOC equals the vector
 * the FS input[0] reads. */
static void r300_r2vb_dump_xform_routing(struct r300_context *r300)
{
    struct r300_vertex_stream_state *vs =
        (struct r300_vertex_stream_state *)r300->vertex_stream_state.state;
    struct r300_rs_block *rs = (struct r300_rs_block *)r300->rs_block_state.state;
    if (vs) {
        fprintf(stderr, "r2vb_xform_route psc_count=%u\n", vs->count);
        for (unsigned i = 0; i < vs->count && i < 4; i++) {
            uint32_t c = vs->vap_prog_stream_cntl[i];
            for (unsigned e = 0; e < 2; e++) {
                uint32_t f = c >> (e * 16);
                fprintf(stderr, "  psc[%u].e%u data_type=%u dst_vec_loc=%u last=%u\n", i, e,
                        f & 0xf, (f >> 8) & 0x1f, (f >> 13) & 1);
            }
        }
    }
    if (rs) {
        fprintf(stderr,
                "r2vb_xform_route vap_out_vtx_fmt0=0x%08x fmt1=0x%08x rs_count=0x%08x "
                "inst_count=0x%08x\n",
                rs->vap_out_vtx_fmt[0], rs->vap_out_vtx_fmt[1], rs->count, rs->inst_count);
        for (unsigned i = 0; i < 8; i++)
            if (rs->ip[i] || rs->inst[i])
                fprintf(stderr, "  rs[%u] ip=0x%08x inst=0x%08x\n", i, rs->ip[i], rs->inst[i]);
    }
}

/* Read back a producer output BO and compare each slot to a caller-supplied
 * expected value with a per-channel tolerance.  The producer runs on the FP24
 * fragment ALU, so a transform accumulates rounding and wants a loose tolerance;
 * an FP24-exact input through an exact op (e.g. *2.0) stays bit-exact and admits
 * a near-zero tolerance, where any deviation is a plumbing bug, not rounding.
 * tag names the stream in the per-slot dump and the pass line.  Returns the
 * number of slots within tolerance. */
static uint32_t r2vb_verify_bo_readback(struct r300_context *r300, struct pipe_resource *res,
                                        const float (*expected)[4], uint32_t count, float tol,
                                        const char *tag)
{
    struct pipe_transfer *xfer = NULL;
    struct pipe_box box = { .width = count * 16, .height = 1, .depth = 1 };
    const float *got =
        r300->context.buffer_map(&r300->context, res, 0, PIPE_MAP_READ, &box, &xfer);
    if (!got)
        return 0;
    uint32_t pass = 0;
    for (uint32_t s = 0; s < count; s++) {
        const float *e = expected[s];
        const float *g = &got[s * 4];
        bool ok = fabsf(g[0] - e[0]) <= tol && fabsf(g[1] - e[1]) <= tol &&
                  fabsf(g[2] - e[2]) <= tol && fabsf(g[3] - e[3]) <= tol;
        if (ok)
            pass++;
        if (s < 4)
            fprintf(stderr,
                    "  %s[%u] got=%.6f,%.6f,%.6f,%.6f exp=%.6f,%.6f,%.6f,%.6f %s\n", tag, s,
                    g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3], ok ? "OK" : "MISMATCH");
    }
    fprintf(stderr, "r2vb_%s_verify pass=%u/%u tol=%g\n", tag, pass, count, tol);
    r300->context.buffer_unmap(&r300->context, xfer);
    return pass;
}

/* Verify the producer transform BO against the CPU column-major M*model_vert.
 * FP24 fragment ALU, so a loose tolerance, not bit-exactness. */
static void r2vb_verify_xform_readback(struct r300_context *r300, struct pipe_resource *res,
                                       const float (*model)[4], uint32_t count, const float *cols)
{
    float (*exp)[4] = malloc((size_t)count * sizeof(*exp));
    if (!exp)
        return;
    for (uint32_t s = 0; s < count; s++)
        for (int i = 0; i < 4; i++) {
            exp[s][i] = 0.0f;
            for (int j = 0; j < 4; j++)
                exp[s][i] += cols[j * 4 + i] * model[s][j];
        }
    r2vb_verify_bo_readback(r300, res, exp, count, 0.05f, "xform");
    free(exp);
}

/* Producer half (stage 1 + the cb_flush_clean barrier): render one synthesized
 * vertex per output slot into output_gart_bo through the bound fragment program,
 * then make the writes visible to the VAP.  Factored out of the combined loop so
 * the route-exec MVP path can run it under the normal draw flow (where
 * prepare_for_rendering has emitted the transform-FS state) and then re-ingest
 * with a different (application) FS, rather than the single-FS combined loop. */
static void r300_r2vb_emit_producer(struct r300_context *r300,
                                    struct r300_resource *output_gart_bo,
                                    uint32_t output_gart_bo_offset, uint32_t num_vertices,
                                    const float (*vertex_attrs)[4], bool transform_mode)
{
    CS_LOCALS(r300);
    uint32_t output_pitch = align(num_vertices, 2);

    r300->rws->cs_add_buffer(&r300->cs, output_gart_bo->buf,
                             RADEON_USAGE_READWRITE | RADEON_USAGE_SYNCHRONIZED |
                                 RADEON_PRIO_COLOR_BUFFER,
                             RADEON_DOMAIN_GTT);

    struct r300_fragment_shader *r2vb_fs = r300_fs(r300);
    struct rc_constant_list *r2vb_consts =
        r2vb_fs && r2vb_fs->shader ? &r2vb_fs->shader->code.constants : NULL;
    UNUSED unsigned r2vb_vp_override_dwords = 0;
    if (r2vb_consts) {
        for (unsigned i = 0; i < r2vb_consts->Count; i++) {
            unsigned t = r2vb_consts->Constants[i].Type;
            unsigned s = r2vb_consts->Constants[i].u.State[0];
            if (t == RC_CONSTANT_STATE &&
                (s == RC_STATE_R300_VIEWPORT_SCALE ||
                 s == RC_STATE_R300_VIEWPORT_OFFSET))
                r2vb_vp_override_dwords += 5;
        }
    }

    /* Stage 1 = 47 dwords + stage 2 (barrier) = 8, minus the 16-dword
     * triangle-vs-two-float4-points geometry delta, plus num_vertices*8 embedded
     * vertex dwords and the per-viewport-constant wpos override.  The barrier is
     * 8 (not 6) dwords because it includes the VAP_PVS_STATE_FLUSH engine sync. */
    BEGIN_CS(55 + r2vb_vp_override_dwords + (int)num_vertices * 8 - 16);

    OUT_CS_REG(R300_ZB_CNTL, 0);
    OUT_CS_REG_SEQ(R300_SC_SCISSORS_TL, 2);
    OUT_CS((1440 << R300_SCISSORS_X_SHIFT) | (1440 << R300_SCISSORS_Y_SHIFT));
    OUT_CS(((num_vertices + 1440 - 1) << R300_SCISSORS_X_SHIFT) |
           ((1 + 1440 - 1) << R300_SCISSORS_Y_SHIFT));
    OUT_CS_REG(R300_RB3D_COLOROFFSET0, output_gart_bo_offset);
    OUT_CS_RELOC(output_gart_bo);
    OUT_CS_REG(R300_RB3D_COLORPITCH0, output_pitch | R300_COLOR_FORMAT_ARGB32323232);
    /* RGBA identity select for the transform producer (FS outputs (x,y,z,w)
     * directly); BGRA for the passthrough producer (copies a pre-swizzled
     * (z,y,x,w) attribute).  Both target the ARGB32323232 BO. */
    OUT_CS_REG(R300_US_OUT_FMT_0, transform_mode
                   ? (R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_R | R300_C1_SEL_G |
                      R300_C2_SEL_B | R300_C3_SEL_A)
                   : (R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_B | R300_C1_SEL_G |
                      R300_C2_SEL_R | R300_C3_SEL_A));
    /* Identity wpos for a gl_FragCoord-based passthrough FS (no-op for a
     * transform FS, whose constants are matrix externals, not viewport state). */
    if (r2vb_consts) {
        for (unsigned i = 0; i < r2vb_consts->Count; i++) {
            const struct rc_constant *c = &r2vb_consts->Constants[i];
            float v;
            if (c->Type != RC_CONSTANT_STATE)
                continue;
            if (c->u.State[0] == RC_STATE_R300_VIEWPORT_SCALE)
                v = 1.0f;
            else if (c->u.State[0] == RC_STATE_R300_VIEWPORT_OFFSET)
                v = 0.0f;
            else
                continue;
            OUT_CS_REG_SEQ(R300_PFS_PARAM_0_X + i * 16, 4);
            OUT_CS(pack_float24(v));
            OUT_CS(pack_float24(v));
            OUT_CS(pack_float24(v));
            OUT_CS(pack_float24(v));
        }
    }
    OUT_CS_REG(R300_SU_CULL_MODE, 0);
    OUT_CS_REG(R300_SC_CLIP_RULE, 0xFFFF);
    OUT_CS_REG(R300_GA_POINT_SIZE, (6 << R300_POINTSIZE_Y_SHIFT) |
                                       (6 << R300_POINTSIZE_X_SHIFT));
    OUT_CS_REG(R300_GA_POINT_MINMAX, (6 << R300_GA_POINT_MINMAX_MIN_SHIFT) |
                                         (6 << R300_GA_POINT_MINMAX_MAX_SHIFT));
    OUT_CS_REG(R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
    OUT_CS_REG(R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
    OUT_CS_REG(R300_VAP_VTX_SIZE, 8);
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_IMMD_2, num_vertices * 8);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | (num_vertices << 16) |
           R300_VAP_VF_CNTL__PRIM_POINTS);
    for (uint32_t pv = 0; pv < num_vertices; pv++) {
        OUT_CS_32F((float)pv + 0.5f);
        OUT_CS_32F(0.5f);
        OUT_CS_32F(0.0f);
        OUT_CS_32F(1.0f);
        if (transform_mode) {
            OUT_CS_32F(vertex_attrs[pv][0]);
            OUT_CS_32F(vertex_attrs[pv][1]);
            OUT_CS_32F(vertex_attrs[pv][2]);
            OUT_CS_32F(vertex_attrs[pv][3]);
        } else {
            OUT_CS_32F(vertex_attrs[pv][2]);
            OUT_CS_32F(vertex_attrs[pv][1]);
            OUT_CS_32F(vertex_attrs[pv][0]);
            OUT_CS_32F(vertex_attrs[pv][3]);
        }
    }

    /* Stage 2 -- cb_flush_clean barrier (R300_R2VB_BARRIER neuters parts for
     * timing bisection). */
    const char *r2vb_bar = getenv("R300_R2VB_BARRIER");
    uint32_t r2vb_zb = (r2vb_bar && strstr(r2vb_bar, "nozb"))
                           ? 0
                           : (R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                              R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
    uint32_t r2vb_rb = (r2vb_bar && strstr(r2vb_bar, "norb"))
                           ? 0
                           : (R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                              R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
    uint32_t r2vb_wait = (r2vb_bar && strstr(r2vb_bar, "nowait")) ? 0 : RADEON_WAIT_3D_IDLECLEAN;
    OUT_CS_REG(R300_ZB_ZCACHE_CTLSTAT, r2vb_zb);
    OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT, r2vb_rb);
    OUT_CS_REG(RADEON_WAIT_UNTIL, r2vb_wait);
    /* Sync the VAP/vertex-fetch engine.  The cache flushes above push the
     * producer's colour write out of the RB3D/Z caches to memory, but they do
     * NOT touch the vertex cache: the R2VB re-ingest fetches this same BO as a
     * vertex stream, and the VAP can return STALE vertices the vertex cache kept
     * from an earlier fetch of the recycled GART page.  Observed as a
     * non-deterministic stale read -- the producer's transform is correct in the
     * BO (XFORM_VERIFY reads it back exact), yet ~50% of re-ingest draws
     * rasterize the previous draw's vertices.  Writing zero to
     * VAP_PVS_STATE_FLUSH_REG synchronizes the engine and clears that stale state
     * before the re-ingest's LOAD_VBPNTR, making the route deterministic. */
    OUT_CS_REG(R300_VAP_PVS_STATE_FLUSH_REG, 0x0);

    END_CS;
}

void r300_emit_rs482_r2vb_compute_loop(struct r300_context *r300,
                                       struct r300_resource *output_gart_bo,
                                       uint32_t output_gart_bo_offset, uint32_t num_vertices,
                                       const float (*vertex_attrs)[4], uint32_t reingest_vf_prim,
                                       struct r300_resource *stage3_color_bo, uint32_t stage3_width,
                                       uint32_t stage3_height, bool transform_mode)
{
    CS_LOCALS(r300);

    assert(num_vertices > 0 && num_vertices <= 65535);
    assert(vertex_attrs != NULL);
    assert(r300->screen->caps.num_vert_fpus == 0);
    assert(!r300->screen->caps.has_tcl);
    assert(!stage3_color_bo || (stage3_width > 0 && stage3_height > 0));

    if (num_vertices == 0 || num_vertices > 65535 || !vertex_attrs)
        return;
    if (r300->screen->caps.has_tcl || r300->screen->caps.num_vert_fpus != 0)
        return;
    if (stage3_color_bo && (stage3_width == 0 || stage3_height == 0))
        return;

    /* FP32x4 linear color targets need an even-pixel pitch; the scissor remains
     * the logical vertex count so the padding pixel is not rendered. */
    uint32_t stage3_pitch = align(stage3_width, 2);

    /* Stage-3 observation target (optional): a separate 2D BO so the re-ingest
     * draw renders there, leaving the stage-1 vertex data in output_gart_bo
     * intact for readback.  NULL keeps the single-BO loop. */
    if (stage3_color_bo)
        r300->rws->cs_add_buffer(&r300->cs, stage3_color_bo->buf,
                                 RADEON_USAGE_READWRITE | RADEON_USAGE_SYNCHRONIZED |
                                     RADEON_PRIO_COLOR_BUFFER,
                                 RADEON_DOMAIN_GTT);

    /* Producer: stage 1 (render one synthesized vertex per slot through the
     * bound fragment program) + the cb_flush_clean barrier, into output_gart_bo. */
    r300_r2vb_emit_producer(r300, output_gart_bo, output_gart_bo_offset, num_vertices,
                            vertex_attrs, transform_mode);

    /* Stage 3 -- re-ingest output_gart_bo as the vertex array and draw it.  The
     * optional observe redirect (stage3_color_bo) adds nine dwords. */
    BEGIN_CS(stage3_color_bo ? 26 : 17);

    /* Stage-3 observation redirect.  Point the color buffer at the separate 2D
     * target and scissor to its extent so the re-ingested draw rasterizes there,
     * leaving output_gart_bo's stage-1 vertex data intact for comparison.  The
     * scissor follows stage3_height so the CS validator's pitch * cpp * maxy
     * color-size bound fits stage3_color_bo (same SC_SCISSORS_BR encoding as
     * stage 1, here for the full 2D extent rather than one row). */
    if (stage3_color_bo) {
        OUT_CS_REG(R300_RB3D_COLOROFFSET0, 0);
        OUT_CS_RELOC(stage3_color_bo);
        OUT_CS_REG(R300_RB3D_COLORPITCH0, stage3_pitch | R300_COLOR_FORMAT_ARGB32323232);
        OUT_CS_REG_SEQ(R300_SC_SCISSORS_TL, 2);
        OUT_CS((1440 << R300_SCISSORS_X_SHIFT) | (1440 << R300_SCISSORS_Y_SHIFT));
        OUT_CS(((stage3_width + 1440 - 1) << R300_SCISSORS_X_SHIFT) |
               ((stage3_height + 1440 - 1) << R300_SCISSORS_Y_SHIFT));
    }

    /* Stage 3 -- re-ingest the GTT buffer as the vertex array and draw it.
     *
     * Declare one FP32x4 input stream with an explicit identity XYZW swizzle and
     * all-component write enable (the PSC default swizzle is not XYZW, so a prior
     * draw's PROG_STREAM_CNTL_EXT could otherwise reinterpret the vec4).  Then
     * bind the same GTT BO the CB wrote, mirroring r300_emit_vertex_arrays_swtcl
     * exactly: PKT3 COUNT=3, (num_arrays | force-prefetch), the format word
     * size|(stride<<8) with size and stride in DWORDS (4 for FP32x4), the offset,
     * the reserved zero dword, then the NOP-form relocation.  RS482 sets
     * R300_VAP_TCL_BYPASS unconditionally (r300_state.c), so the VAP rasters
     * these pre-transformed vertices without invoking the (absent) PVS. */
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_0, 1);
    OUT_CS(R300_DATA_TYPE_FLOAT_4 | R300_LAST_VEC);
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_EXT_0, 1);
    OUT_CS((R300_SWIZZLE_SELECT_X << R300_SWIZZLE_SELECT_X_SHIFT) |
           (R300_SWIZZLE_SELECT_Y << R300_SWIZZLE_SELECT_Y_SHIFT) |
           (R300_SWIZZLE_SELECT_Z << R300_SWIZZLE_SELECT_Z_SHIFT) |
           (R300_SWIZZLE_SELECT_W << R300_SWIZZLE_SELECT_W_SHIFT) | (0xf << R300_WRITE_ENA_SHIFT));
    /* Reset the output vertex size to one FP32x4 stream.  Stage 1's producer set
     * VAP_VTX_SIZE = 8 for its two streams (position + attribute); the re-ingest
     * declares a single FP32x4 stream, so the correct size is 4.  The inherited 8
     * is a latent stride mismatch -- it makes the VAP treat each vertex as eight
     * dwords (four real position dwords plus four read past the vertex).  The
     * filled and line topologies consume only position (the first four dwords) and
     * were pixel-exact even with the stale 8, so this is correctness hygiene that
     * does not change their footprint.
     *
     * It does NOT fix the POINTS re-ingest, which still smears (measured: a
     * 16-point readback stays at ~110 texels with the bounding box reaching the
     * origin, not 16 single texels). Three register hypotheses have near-zero
     * effect on it -- GA_POINT_SIZE, GA_POINT_MINMAX, and this VAP_VTX_SIZE -- so
     * the r300 point-rasterization path in TCL_BYPASS sizes/places these points by
     * a mechanism not yet identified. The stage-1 producer's own PRIM_POINTS
     * rasterize correctly, so the fault is specific to the re-ingest draw. Left as
     * a separate investigation; POINTS is off the mesh-draw critical path. */
    OUT_CS_REG(R300_VAP_VTX_SIZE, 4);
    /* Re-assert the vertex-index bound for the re-ingest draw.  VAP_VF_MAX_VTX_
     * INDX clamps every fetched index; a stale lower bound (from an inherited
     * draw or a smaller producer) would fold high-index vertices onto a low one
     * and rasterize a degenerate set.  The re-ingest draws all num_vertices GTT
     * rows, so bound it to the actual highest index. */
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_LOAD_VBPNTR, 3);
    OUT_CS(1 | R300_VC_FORCE_PREFETCH);
    OUT_CS(4 | (4 << 8));
    OUT_CS(output_gart_bo_offset);
    OUT_CS(0);
    OUT_CS(0xc0001000); /* PKT3_NOP -- the relocation form LOAD_VBPNTR expects */
    OUT_CS(r300->rws->cs_lookup_buffer(&r300->cs, output_gart_bo->buf) * 4);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS((num_vertices << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) | reingest_vf_prim |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST);

    END_CS;
}

/* Gated self-test for the RS482 HB_TCL umbrella.  R300_HB_TCL=1 names the
 * hybrid-TCL experiment surface; R300_R2VB_TIMING picks the transport mode:
 *   capture -- emit the loop and flush with RADEON_FLUSH_NOOP, so the IB is
 *              captured by R300_TRACE and never reaches DRM_RADEON_CS.  The
 *              packets can be decoded and verified with zero hardware risk.
 *              This is the structural preflight.
 *   submit  -- a real flush with a bounded fence wait, timed; additionally
 *              requires R300_RAW_SUBMIT_ACCEPTED=1.  This is the hazard-gated
 *              measurement that decides whether R2VB beats the gallivm CPU
 *              baseline; a draw the CS validator passes can still hang
 *              reset-less silicon.
 * Both modes fire only from r300_flush (from_flush), where a real draw has
 * already left its framebuffer, fragment program, and SU/RS setup in this CS.
 * The loop deliberately does not emit the fragment microcode (the compiler's
 * job), so it needs that state already present; firing at context create would
 * append the loop to an empty CS with no shader bound.  The capture and the
 * submit therefore decode and time the same composed IB.  It fires once per
 * process and returns true when it consumed the CS, so the caller skips its own
 * flush.  R300_R2VB_PRIM selects the re-ingest topology and its canonical shape
 * (points|lines|line_strip|line_loop|triangles|triangle_strip|triangle_fan;
 * default triangles); R300_R2VB_NVERTS scales the POINTS shape's count, clamped
 * below 2^16 (the SWTCL VAP NUM_VERTICES field width). */
/* A canonical re-ingest shape: the window-space vertices the producer writes and
 * the topology the stage-3 draw assembles from them, with a predicted footprint
 * the framebuffer oracle confirms.  Every shape lives in the [10,54] window-coord
 * box so a 64-wide stage-3 readback captures it whole.  This exercises the
 * generalized producer -- arbitrary vertex data and arbitrary topology -- rather
 * than the single baked triangle the proof started from. */
#define R2VB_MAX_SHAPE_VERTS 64
struct r2vb_shape {
    uint32_t num_vertices;
    uint32_t vf_prim; /* R300_VAP_VF_CNTL__PRIM_* */
    float attrs[R2VB_MAX_SHAPE_VERTS][4];
    const char *prim_name;
    char expect[160];
};

/* Fill one window-space vertex row (x, y in pixels; z = 0.5, w = 1). */
static void r2vb_set_vert(float row[4], float x, float y)
{
    row[0] = x;
    row[1] = y;
    row[2] = 0.5f;
    row[3] = 1.0f;
}

/* Build the shape for prim_name (default "triangles"); pts_count sizes the only
 * count-scalable topology, POINTS.  The corners of a 44x44 box at (10,10) anchor
 * every filled/outline shape so the predicted footprint is a clean function of
 * the topology.  Returns false for an unknown primitive name. */
static bool r2vb_build_shape(const char *prim_name, uint32_t pts_count, struct r2vb_shape *s)
{
    const float x0 = 10.0f, y0 = 10.0f, x1 = 54.0f, y1 = 54.0f; /* 44x44 box */
    s->prim_name = prim_name;

    if (strcmp(prim_name, "triangles") == 0) {
        s->vf_prim = R300_VAP_VF_CNTL__PRIM_TRIANGLES;
        s->num_vertices = 3;
        r2vb_set_vert(s->attrs[0], x0, y0);
        r2vb_set_vert(s->attrs[1], x1, y0);
        r2vb_set_vert(s->attrs[2], 32.0f, y1);
        snprintf(s->expect, sizeof s->expect,
                 "expect filled triangle: written~968 (0.5*44*44) bbox~10,10,53,52");
        return true;
    }
    if (strcmp(prim_name, "triangle_strip") == 0) {
        /* TL,TR,BL,BR -> two triangles tiling the full 44x44 quad. */
        s->vf_prim = R300_VAP_VF_CNTL__PRIM_TRIANGLE_STRIP;
        s->num_vertices = 4;
        r2vb_set_vert(s->attrs[0], x0, y0);
        r2vb_set_vert(s->attrs[1], x1, y0);
        r2vb_set_vert(s->attrs[2], x0, y1);
        r2vb_set_vert(s->attrs[3], x1, y1);
        snprintf(s->expect, sizeof s->expect,
                 "expect filled quad: written~1936 (44*44) bbox~10,10,53,53");
        return true;
    }
    if (strcmp(prim_name, "triangle_fan") == 0) {
        /* centre + 4 ring corners.  A 5-vertex fan assembles n-2 = 3 triangles
         * (centre,c1,c2), (centre,c2,c3), (centre,c3,c4) -- it does NOT close back
         * to c1, so it fills three of the four centre-anchored quadrants and
         * leaves the c4->c1 wedge open.  That is correct fan topology, not a
         * defect; the footprint is ~3/4 of the 44x44 quad. */
        s->vf_prim = R300_VAP_VF_CNTL__PRIM_TRIANGLE_FAN;
        s->num_vertices = 5;
        r2vb_set_vert(s->attrs[0], 32.0f, 32.0f);
        r2vb_set_vert(s->attrs[1], x0, y0);
        r2vb_set_vert(s->attrs[2], x1, y0);
        r2vb_set_vert(s->attrs[3], x1, y1);
        r2vb_set_vert(s->attrs[4], x0, y1);
        snprintf(s->expect, sizeof s->expect,
                 "expect open 3-triangle fan (left wedge open): written~1452 bbox~10,10,53,53");
        return true;
    }
    if (strcmp(prim_name, "line_loop") == 0) {
        s->vf_prim = R300_VAP_VF_CNTL__PRIM_LINE_LOOP;
        s->num_vertices = 4;
        r2vb_set_vert(s->attrs[0], x0, y0);
        r2vb_set_vert(s->attrs[1], x1, y0);
        r2vb_set_vert(s->attrs[2], x1, y1);
        r2vb_set_vert(s->attrs[3], x0, y1);
        snprintf(s->expect, sizeof s->expect,
                 "expect rect outline: written~176 (perimeter 4*44) bbox~10,10,53,53");
        return true;
    }
    if (strcmp(prim_name, "line_strip") == 0) {
        s->vf_prim = R300_VAP_VF_CNTL__PRIM_LINE_STRIP;
        s->num_vertices = 4;
        r2vb_set_vert(s->attrs[0], x0, y0);
        r2vb_set_vert(s->attrs[1], x1, y0);
        r2vb_set_vert(s->attrs[2], x1, y1);
        r2vb_set_vert(s->attrs[3], x0, y1);
        snprintf(s->expect, sizeof s->expect,
                 "expect open polyline (3 segments) bbox~10,10,53,53");
        return true;
    }
    if (strcmp(prim_name, "lines") == 0) {
        /* two independent horizontal segments. */
        s->vf_prim = R300_VAP_VF_CNTL__PRIM_LINES;
        s->num_vertices = 4;
        r2vb_set_vert(s->attrs[0], x0, y0);
        r2vb_set_vert(s->attrs[1], x1, y0);
        r2vb_set_vert(s->attrs[2], x0, y1);
        r2vb_set_vert(s->attrs[3], x1, y1);
        snprintf(s->expect, sizeof s->expect,
                 "expect 2 horizontal segments (top+bottom) bbox~10,10,53,53");
        return true;
    }
    if (strcmp(prim_name, "points") == 0) {
        /* N points spaced along the box diagonal; the count-scalable case.  Keep
         * adjacent points >= 1 px apart so each lands on its own texel. */
        uint32_t n = pts_count < 2 ? 2 : pts_count;
        if (n > 45)
            n = 45; /* 44/(45-1)=1.0 px spacing floor */
        s->vf_prim = R300_VAP_VF_CNTL__PRIM_POINTS;
        s->num_vertices = n;
        float step = (x1 - x0) / (float)(n - 1);
        for (uint32_t i = 0; i < n; i++)
            r2vb_set_vert(s->attrs[i], x0 + step * (float)i, y0 + step * (float)i);
        snprintf(s->expect, sizeof s->expect,
                 "%u points on diagonal; stage-1 BO exact, stage-3 POINTS raster "
                 "OPEN (3 register hypotheses falsified)", n);
        return true;
    }
    return false;
}

struct r2vb_selftest_config {
    bool enabled;
    bool do_submit;
    bool nowait;
    bool observe;
    bool xform;
    uint32_t num_vertices;
    uint32_t s3dim;
    const char *prim_name;
};

static void r2vb_get_selftest_config(struct r2vb_selftest_config *cfg)
{
    const char *hb_tcl = getenv("R300_HB_TCL");
    const char *mode = getenv("R300_R2VB_TIMING");
    cfg->enabled = (hb_tcl && strcmp(hb_tcl, "1") == 0) || (mode != NULL);
    if (!cfg->enabled)
        return;

    cfg->do_submit = (mode && strcmp(mode, "submit") == 0);
    /* NOWAIT: submit and hand the fence back to the caller (r300_flush's out
     * param -> r300vk's Vulkan fence) instead of waiting via the raw winsys
     * BO-wait poll, so the GPU completion is timed through the fast fence path. */
    const char *nw = getenv("R300_R2VB_NOWAIT");
    cfg->nowait = cfg->do_submit && nw && strcmp(nw, "1") == 0;

    /* R300_R2VB_PRIM selects the re-ingest topology and its canonical shape
     * (default triangles -- the proven baseline).  R300_R2VB_NVERTS scales only
     * the POINTS shape; the filled/outline shapes fix their own vertex count. */
    cfg->prim_name = getenv("R300_R2VB_PRIM");
    if (!cfg->prim_name)
        cfg->prim_name = "triangles";

    cfg->num_vertices = 16;
    const char *nv = getenv("R300_R2VB_NVERTS");
    if (nv) {
        long v = strtol(nv, NULL, 0);
        if (v > 0 && v < 65536)
            cfg->num_vertices = (uint32_t)v;
    }

    /* R300_R2VB_XFORM=1: run the producer through the 4-DP4 MVP transform-FS
     * (the shape verts are model-space input; a fixed test matrix transforms
     * them) and verify the output BO holds M*v, rather than copying the
     * attribute through a passthrough FS. */
    const char *xf = getenv("R300_R2VB_XFORM");
    cfg->xform = (xf && strcmp(xf, "1") == 0);

    const char *obs = getenv("R300_R2VB_STAGE3_OBSERVE");
    cfg->observe = (obs && strcmp(obs, "1") == 0);
    cfg->s3dim = 64;
    if (cfg->observe) {
        const char *sd = getenv("R300_R2VB_STAGE3_DIM");
        if (sd) {
            long d = strtol(sd, NULL, 0);
            if (d > 0 && d <= 2048)
                cfg->s3dim = (uint32_t)d;
        }
        cfg->s3dim = align(cfg->s3dim, 2);
    }
}

bool r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300, bool from_flush,
                                           unsigned flush_flags,
                                           struct pipe_fence_handle **out_fence)
{
    static bool fired = false;
    struct r2vb_selftest_config cfg;
    r2vb_get_selftest_config(&cfg);

    if (!cfg.enabled || !from_flush || fired)
        return false;

    if (cfg.do_submit) {
        const char *gate = getenv("R300_RAW_SUBMIT_ACCEPTED");
        if (!gate || strcmp(gate, "1") != 0) {
            fprintf(stderr, "r2vb selftest: submit mode needs R300_RAW_SUBMIT_ACCEPTED=1\n");
            return false;
        }
    }

    /* Select the producer vertices and re-ingest topology.  prim=throughput is a
     * timing path: it generates num_vertices clustered vertices on the heap (tiny
     * degenerate triangles to minimise rasterisation, isolating the transform +
     * fetch + submit cost) and skips the stage-3 readback, so the timer reflects
     * the direct-VAP path at scale rather than a readable picture.  The producer
     * embeds vertices in a 3D_DRAW_IMMD packet, so the whole loop -- base
     * registers plus num_vertices * 8 vertex dwords -- must fit one IB.  Near the
     * 14-bit PKT3 size limit the IB overflows (a 2047-vertex run faulted), so cap
     * throughput N at 1024; reaching larger N is the vertex-array-producer
     * increment, not embedded IMMD.  Otherwise build a canonical shape; an unknown
     * prim is a hard error. */
    const float (*attrs)[4];
    uint32_t vf_prim, nverts;
    float (*heap_attrs)[4] = NULL;
    struct r2vb_shape shape;
    if (strcmp(cfg.prim_name, "throughput") == 0) {
        nverts = cfg.num_vertices > 1024 ? 1024 : cfg.num_vertices;
        heap_attrs = malloc((size_t)nverts * sizeof(*heap_attrs));
        if (!heap_attrs)
            return false;
        for (uint32_t i = 0; i < nverts; i++) {
            heap_attrs[i][0] = 10.0f + (float)(i & 3);
            heap_attrs[i][1] = 10.0f + (float)(i & 3);
            heap_attrs[i][2] = 0.5f;
            heap_attrs[i][3] = 1.0f;
        }
        attrs = heap_attrs;
        vf_prim = R300_VAP_VF_CNTL__PRIM_TRIANGLES;
        cfg.observe = false;
        fprintf(stderr, "r2vb_shape prim=throughput nverts=%u (tiny tris, no stage3)\n", nverts);
    } else {
        if (!r2vb_build_shape(cfg.prim_name, cfg.num_vertices, &shape)) {
            fprintf(stderr, "r2vb selftest: unknown R300_R2VB_PRIM=%s (want points|lines|line_strip|"
                            "line_loop|triangles|triangle_strip|triangle_fan|throughput)\n",
                    cfg.prim_name);
            return false;
        }
        nverts = shape.num_vertices;
        attrs = shape.attrs;
        vf_prim = shape.vf_prim;
        fprintf(stderr, "r2vb_shape prim=%s nverts=%u %s\n", shape.prim_name, nverts, shape.expect);
    }
    cfg.num_vertices = nverts;

    struct pipe_resource *res = r2vb_create_selftest_bo(r300, align(cfg.num_vertices, 2) * 16, 0);
    if (!res) {
        free(heap_attrs);
        return false;
    }

    struct pipe_resource *stage3 = NULL;
    if (cfg.observe) {
        stage3 = r2vb_create_selftest_bo(r300, cfg.s3dim * cfg.s3dim * 16, 0xff);
        if (!stage3) {
            pipe_resource_reference(&res, NULL);
            free(heap_attrs);
            return false;
        }
    }

    fired = true;

    /* Transform mode: run the producer through the 4-DP4 MVP transform-FS instead
     * of the trigger draw's passthrough FS.  Bind it, load the transposed test
     * matrix into FS const file 0, recompute derived (RS) state for its single
     * input, and emit that state into the IB -- the hand-rolled loop inherits FS
     * state from the trigger draw, which here is the wrong (app) FS.  Restored
     * after. */
    void *saved_fs = NULL;
    if (cfg.xform) {
        void *xfs = r300_r2vb_get_transform_fs(r300);
        if (xfs) {
            saved_fs = r300->fs.state;
            r300->context.bind_fs_state(&r300->context, xfs);
            r300_r2vb_set_transform_consts(r300, r2vb_test_mvp_cols);
            r300_update_derived_state(r300);
            /* Reserve CS space and emit the transform-FS US code + const file +
             * RS routing through the real prepare path (not raw emit_dirty_state,
             * which left an empty IB -> RS4xx zero_ib).  Reserve generously for
             * the producer the compute loop emits next. */
            bool prepared = r300_r2vb_prepare_states(r300, 1024);
            fprintf(stderr, "r2vb_xform bound transform-FS + transposed test MVP prepared=%d\n",
                    prepared);
            r300_r2vb_dump_xform_routing(r300);
        }
    }

    r300_emit_rs482_r2vb_compute_loop(r300, r300_resource(res), 0, cfg.num_vertices, attrs,
                                      vf_prim, stage3 ? r300_resource(stage3) : NULL,
                                      cfg.s3dim, cfg.s3dim, cfg.xform);

    r300_emit_hyperz_end(r300);
    r300_emit_query_end(r300);
    {
        CS_LOCALS(r300);
        BEGIN_CS(3);
        OUT_CS_REG_SEQ(R300_GB_MSPOS0, 2);
        OUT_CS(0x66666666);
        OUT_CS(0x6666666);
        END_CS;
    }

    if (cfg.nowait) {
        /* Submit and hand the fence to the caller's out param (becomes r300vk's
         * Vulkan fence); do NOT wait here.  The application's vkWaitForFences then
         * times GPU completion through the fast fence path.  A fence cannot signal
         * before the GPU retires the work, so if that wait is sub-millisecond the
         * R2VB GPU work is genuinely fast and the earlier ~505 ms was purely the
         * raw winsys BO-wait poll. */
        int flush_rc = r300->rws->cs_flush(&r300->cs, flush_flags, out_fence);
        fprintf(stderr,
                "r2vb_nowait_submit nverts=%u flush_rc=%d gave_fence=%d "
                "(GPU completion timed by app vkWaitForFences) hb_vert_fpu=%u\n",
                cfg.num_vertices, flush_rc, out_fence && *out_fence ? 1 : 0,
                r300->screen->caps.num_vert_fpus);
        free(heap_attrs);
        pipe_resource_reference(&stage3, NULL);
        pipe_resource_reference(&res, NULL);
        return true;
    }

    if (cfg.do_submit) {
        struct pipe_fence_handle *fence = NULL;
        struct timespec t0, t1, t2, t3;
        bool signalled = false;
        /* Three-way split to localise the per-submit cost.  cs_flush only ENQUEUES
         * the IB to the radeon threaded-submit queue and returns; cs_sync_flush
         * blocks until that worker has issued the DRM_RADEON_CS ioctl; fence_wait
         * blocks until the GPU retires the fence BO.  So enqueue_ms is CPU-side
         * bookkeeping, submit_ms is the kernel submit + BO pin, and gpu_ms is the
         * actual GPU execution -- the number that should scale with vertex work. */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int flush_rc = r300->rws->cs_flush(&r300->cs, 0, &fence);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        r300->rws->cs_sync_flush(&r300->cs);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        if (fence) {
            signalled = r300->rws->fence_wait(r300->rws, fence, (uint64_t)5 * 1000 * 1000 * 1000);
            r300->rws->fence_reference(r300->rws, &fence, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &t3);
        double enqueue_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        double submit_ms = (double)(t2.tv_sec - t1.tv_sec) * 1e3 + (double)(t2.tv_nsec - t1.tv_nsec) / 1e6;
        double gpu_ms = (double)(t3.tv_sec - t2.tv_sec) * 1e3 + (double)(t3.tv_nsec - t2.tv_nsec) / 1e6;
        double total_ms = enqueue_ms + submit_ms + gpu_ms;
        double mvps = gpu_ms > 0.0 ? (double)cfg.num_vertices / (gpu_ms * 1e3) : 0.0;
        fprintf(stderr,
                "r2vb_direct_vap_timing nverts=%u total_ms=%.4f enqueue_ms=%.4f submit_ms=%.4f "
                "gpu_ms=%.4f gpu_Mvps=%.3f flush_rc=%d signalled=%d hb_vert_fpu=%u\n",
                cfg.num_vertices, total_ms, enqueue_ms, submit_ms, gpu_ms, mvps, flush_rc,
                signalled, r300->screen->caps.num_vert_fpus);
    } else {
        r300->rws->cs_flush(&r300->cs, RADEON_FLUSH_NOOP, NULL);
        fprintf(stderr,
                "r2vb_capture nverts=%u (no-submit; RADEON_FLUSH_NOOP) "
                "hb_tcl=1 hb_vert_fpu=%u\n",
                cfg.num_vertices, r300->screen->caps.num_vert_fpus);
    }

    if (stage3)
        r2vb_report_stage3_readback(r300, stage3, cfg.s3dim);

    if (cfg.observe)
        r2vb_report_bo_a_diagnostic(r300, res, cfg.num_vertices);

    /* Transform verify: the producer output BO should hold M*model for each slot.
     * res keeps the stage-1 data when stage 3 rendered into the separate observe
     * BO; without observe, stage 3 has overwritten res, so the comparison is only
     * meaningful with R300_R2VB_STAGE3_OBSERVE=1. */
    if (cfg.xform && cfg.do_submit)
        r2vb_verify_xform_readback(r300, res, attrs, cfg.num_vertices, r2vb_test_mvp_cols);

    /* Restore the application fragment shader the transform producer displaced. */
    if (cfg.xform && saved_fs) {
        r300->context.bind_fs_state(&r300->context, saved_fs);
        r300_update_derived_state(r300);
    }

    pipe_resource_reference(&stage3, NULL);
    pipe_resource_reference(&res, NULL);
    free(heap_attrs);
    return true;
}

/* Simple-draw-class classifier for the fragment-ALU R2VB vertex route.  The
 * route replaces the gallivm CPU vertex transform on RS482 (num_vert_fpus == 0)
 * for draws the proven producer + TCL_BYPASS re-ingest can express.  This is the
 * structural gate; the vertex transform itself (compiling the bound VS onto the
 * fragment ALU) is the open follow-on, so a CANDIDATE verdict means "structurally
 * eligible", not "executable yet". */
/* A bound vertex shader is a passthrough when every output is a verbatim copy of
 * an input with no arithmetic, so the re-ingest can feed the application's vertex
 * array straight to the VAP under TCL_BYPASS with no transform.  r300 keeps the
 * VS in NIR live across both routes (r300_create_vs_state), so inspect it: allow
 * only copies (mov), constants, and the input/output IO intrinsics; any real ALU,
 * texture, or control flow means a transform the fragment-ALU producer must run.
 * Conservative -- it flags only direct copies, so a vec4(in.xyz, 1.0) style VS is
 * treated as non-passthrough and falls to the producer path rather than risking a
 * mis-routed direct re-ingest. */
static bool r300_vs_is_passthrough(struct r300_context *r300)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return false;

    nir_shader *nir = vs->state.ir.nir;
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    if (!impl)
        return false;

    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_load_const:
            case nir_instr_type_deref:
                break;
            case nir_instr_type_alu:
                if (nir_instr_as_alu(instr)->op != nir_op_mov)
                    return false;
                break;
            case nir_instr_type_intrinsic:
                switch (nir_instr_as_intrinsic(instr)->intrinsic) {
                case nir_intrinsic_load_input:
                case nir_intrinsic_load_deref:
                case nir_intrinsic_store_output:
                case nir_intrinsic_store_deref:
                    break;
                default:
                    return false;
                }
                break;
            default:
                return false; /* tex, jump, phi, ... */
            }
        }
    }
    return true;
}

/* No-submit shape probe for the de-TGSI vertex transform: dump the bound VS's
 * NIR instruction profile so the MVP-shape matcher (r300_vs_is_mvp) is written
 * against the real nir_to_tgsi output rather than an assumed shape (falsifier
 * F2 in the MVP-transform finding).  Reports a histogram of ALU ops and
 * intrinsics plus the total instruction count, then the full nir_print_shader.
 * Fires once, gated by R300_R2VB_VS_DUMP; pure CPU, no CS emit. */
static void r300_vs_dump_nir_shape(struct r300_context *r300)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir) {
        fprintf(stderr, "r2vb_vs_dump: no NIR (type=%d)\n", vs ? vs->state.type : -1);
        return;
    }
    nir_shader *nir = vs->state.ir.nir;
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    if (!impl)
        return;

    unsigned alu_hist[nir_num_opcodes] = {0};
    unsigned intr_hist[nir_num_intrinsics] = {0};
    unsigned n_alu = 0, n_intr = 0, n_tex = 0, n_const = 0, n_deref = 0, n_other = 0;
    unsigned n_load_input = 0, n_store_output = 0, n_load_ubo = 0,
             n_load_push = 0, n_load_const_intr = 0;
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_alu:
                alu_hist[nir_instr_as_alu(instr)->op]++; n_alu++; break;
            case nir_instr_type_intrinsic: {
                n_intr++;
                intr_hist[nir_instr_as_intrinsic(instr)->intrinsic]++;
                switch (nir_instr_as_intrinsic(instr)->intrinsic) {
                case nir_intrinsic_load_input: n_load_input++; break;
                case nir_intrinsic_store_output: n_store_output++; break;
                case nir_intrinsic_load_ubo: n_load_ubo++; break;
                case nir_intrinsic_load_push_constant: n_load_push++; break;
                case nir_intrinsic_load_constant: n_load_const_intr++; break;
                default: break;
                }
                break;
            }
            case nir_instr_type_tex: n_tex++; break;
            case nir_instr_type_load_const: n_const++; break;
            case nir_instr_type_deref: n_deref++; break;
            default: n_other++; break;
            }
        }
    }
    unsigned n_var_in = 0, n_var_out = 0, n_var_uniform = 0;
    nir_foreach_variable_in_shader(var, nir) {
        if (var->data.mode & nir_var_shader_in) n_var_in++;
        if (var->data.mode & nir_var_shader_out) n_var_out++;
        if (var->data.mode &
            (nir_var_mem_ubo | nir_var_mem_push_const | nir_var_uniform))
            n_var_uniform++;
    }
    fprintf(stderr,
            "r2vb_vs_dump name=%s alu=%u intr=%u tex=%u const=%u deref=%u other=%u "
            "| load_input=%u store_output=%u load_ubo=%u load_push=%u load_const=%u "
            "| var_in=%u var_out=%u var_uniform=%u\n",
            nir->info.name ? nir->info.name : "?", n_alu, n_intr, n_tex, n_const,
            n_deref, n_other, n_load_input, n_store_output, n_load_ubo, n_load_push,
            n_load_const_intr, n_var_in, n_var_out, n_var_uniform);
    for (unsigned op = 0; op < nir_num_opcodes; op++)
        if (alu_hist[op])
            fprintf(stderr, "  alu_op %-16s x%u\n", nir_op_infos[op].name, alu_hist[op]);
    for (unsigned in = 0; in < nir_num_intrinsics; in++)
        if (intr_hist[in])
            fprintf(stderr, "  intr   %-28s x%u\n", nir_intrinsic_infos[in].name, intr_hist[in]);

    /* Extract the MVP the route would feed the transform-FS: the matrix is the
     * four load_ubo_vec4 rows of VS UBO[0], stashed as the SWTCL VS constant
     * shadow (r300_set_constant_buffer).  Print it as four vec4s; GLSL stores
     * mat4 column-major, so row r here is column r (offset r*16 bytes), which is
     * what load_ubo_vec4 row r reads. */
    if (r300->swtcl_vs_const0_ptr && r300->swtcl_vs_const0_size >= 64) {
        const float *m = (const float *)r300->swtcl_vs_const0_ptr;
        fprintf(stderr, "r2vb_vs_dump mvp_ubo0 size=%u\n", r300->swtcl_vs_const0_size);
        for (unsigned r = 0; r < 4; r++)
            fprintf(stderr, "  mvp_col%u %.4f %.4f %.4f %.4f\n", r, m[r * 4 + 0],
                    m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    } else {
        fprintf(stderr, "r2vb_vs_dump mvp_ubo0 NOT_BOUND ptr=%p size=%u\n",
                r300->swtcl_vs_const0_ptr, r300->swtcl_vs_const0_size);
    }
    nir_print_shader(nir, stderr);
}

/* MVP-only matcher: a VS whose position output is exactly mat4 * input, the
 * matrix from a uniform/push-constant, plus pass-through of the other inputs.
 * The NIR reaching r300_vs is in DEREF form (R300_R2VB_VS_DUMP, 2026-06-16):
 * IO and the matrix load are load_deref/store_deref through deref chains, NOT
 * load_input/load_ubo intrinsics, and mat4*vec4 lowers to 4 fmul + 3 fadd
 * (vectorized column-MAD), NOT fdot4.  So key the in/out/uniform existence on
 * variable MODES (present in both deref and lowered form) and the transform on
 * the ALU histogram; reject any op or intrinsic outside the transform set. */
static bool r300_vs_is_mvp(struct r300_context *r300)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return false;
    nir_shader *nir = vs->state.ir.nir;
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    if (!impl)
        return false;

    /* A matrix source, an input vertex, and a position output must all exist as
     * declared variables. */
    bool has_in = false, has_out = false, has_uniform = false;
    nir_foreach_variable_in_shader(var, nir) {
        if (var->data.mode & nir_var_shader_in) has_in = true;
        if (var->data.mode & nir_var_shader_out) has_out = true;
        if (var->data.mode &
            (nir_var_mem_ubo | nir_var_mem_push_const | nir_var_uniform))
            has_uniform = true;
    }
    if (!has_in || !has_out || !has_uniform)
        return false;

    /* Allowed ALU: the column-MAD transform (fmul/fadd), the dot/fma variants in
     * case a later nir pass vectorizes differently, and pure data movement.  Any
     * other arithmetic (trig, div, comparisons, ...) is outside an MVP. */
    unsigned n_dot4 = 0, n_ffma = 0, n_fmul = 0, n_fadd = 0;
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_load_const:
            case nir_instr_type_deref:
                break;
            case nir_instr_type_alu:
                switch (nir_instr_as_alu(instr)->op) {
                case nir_op_fdot4: n_dot4++; break;
                case nir_op_ffma: n_ffma++; break;
                case nir_op_fmul: n_fmul++; break;
                case nir_op_fadd: n_fadd++; break;
                case nir_op_mov:
                case nir_op_vec4:
                case nir_op_vec3:
                case nir_op_vec2: break; /* pure data movement, allowed */
                default:
                    return false; /* arithmetic outside an MVP transform */
                }
                break;
            case nir_instr_type_intrinsic:
                switch (nir_instr_as_intrinsic(instr)->intrinsic) {
                case nir_intrinsic_load_deref:
                case nir_intrinsic_store_deref:
                case nir_intrinsic_load_input:
                case nir_intrinsic_store_output:
                case nir_intrinsic_load_ubo:
                case nir_intrinsic_load_ubo_vec4: /* the matrix: one per row */
                case nir_intrinsic_load_push_constant:
                case nir_intrinsic_load_constant:
                    break;
                default:
                    return false; /* texturing, atomics, ... not an MVP */
                }
                break;
            default:
                return false;
            }
        }
    }
    /* mat4 * vec4 == c0*x + c1*y + c2*z + c3*w: 4 fmul + 3 fadd in the observed
     * column-MAD form; accept the fdot4 or ffma-chain forms too. */
    bool has_transform = (n_fmul >= 4 && n_fadd >= 3) || (n_dot4 >= 4) || (n_ffma >= 3);
    return has_transform;
}

/* The float-ALU op set nir_to_rc emits after r300 lowering (compiler_isa.tsv
 * nir_to_rc_direct rows + the dot/fma vector forms a later NIR pass may leave).
 * An op outside this set cannot run on the fragment ALU, so a VS using it stays
 * on gallivm. */
static bool r300_nir_op_is_fragment_aluable(nir_op op)
{
    switch (op) {
    case nir_op_mov:
    case nir_op_vec2: case nir_op_vec3: case nir_op_vec4:
    case nir_op_fadd: case nir_op_fsub: case nir_op_fmul:
    case nir_op_fmad: case nir_op_ffma:
    case nir_op_fdot2: case nir_op_fdot3: case nir_op_fdot4:
    case nir_op_fdot2_replicated: case nir_op_fdot3_replicated:
    case nir_op_fdot4_replicated:
    case nir_op_fmin: case nir_op_fmax:
    case nir_op_ffract: case nir_op_fround_even:
    case nir_op_frcp: case nir_op_frsq:
    case nir_op_fexp2: case nir_op_flog2: case nir_op_fpow:
    case nir_op_fsin: case nir_op_fcos:
    case nir_op_fabs: case nir_op_fneg: case nir_op_fsat:
    case nir_op_slt: case nir_op_sge: case nir_op_seq: case nir_op_sne:
    case nir_op_fcsel: case nir_op_fcsel_gt: case nir_op_fcsel_ge:
        return true;
    default:
        return false;
    }
}

/* Generalize r300_vs_is_mvp to any straight-line vertex shader the re-staging
 * route (R300_R2VB_RESTAGE) can run on the fragment ALU.  Accept iff: the shader
 * is a single basic block (R300/R400 have no fragment control flow); every ALU op
 * is fragment-aluable and the count stays within the 64-instruction ceiling;
 * every intrinsic is plain I/O or a uniform/UBO load; a gl_Position output
 * exists; and at most one vertex attribute feeds computation -- the first one
 * (velem[0]), since the producer feeds one attribute per output slot and the
 * re-stager maps inputs in declaration order.
 *
 * A non-position output is a straight passthrough of a vertex input (the
 * re-ingest supplies it from the application buffer), or -- when
 * allow_computed_varying -- a fragment-aluable function the multi-pass producer
 * renders into its own BO.  Either way the non-first-input scan below rejects a
 * varying computed from any input but the first (the producer feeds velem[0]
 * only): such an input feeds an ALU op, whose consumer is not a passthrough
 * store, so the scan fails.  A second computed attribute or a non-leading
 * position attribute still needs the multi-input producer and is rejected here
 * (the draw falls back to gallivm). */
static bool r300_vs_is_fragment_aluable(struct r300_context *r300,
                                        bool allow_computed_varying)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return false;
    nir_shader *nir = vs->state.ir.nir;
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    if (!impl)
        return false;

    /* Straight-line only: a single basic block, no if/loop. */
    if (!exec_list_is_singular(&impl->body))
        return false;

    bool has_uniform = false, has_pos_out = false;
    nir_foreach_variable_in_shader(var, nir) {
        if (var->data.mode &
            (nir_var_mem_ubo | nir_var_mem_push_const | nir_var_uniform))
            has_uniform = true;
        if ((var->data.mode & nir_var_shader_out) &&
            var->data.location == VARYING_SLOT_POS)
            has_pos_out = true;
    }
    if (!has_uniform || !has_pos_out)
        return false;

    unsigned alu_count = 0;
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_load_const:
            case nir_instr_type_deref:
                break;
            case nir_instr_type_alu:
                if (!r300_nir_op_is_fragment_aluable(nir_instr_as_alu(instr)->op))
                    return false;
                if (++alu_count > 64)
                    return false;
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
                    return false;
                }
                break;
            default:
                return false; /* texturing, jumps, ... */
            }
        }
    }

    /* Every non-position output is a direct passthrough of a vertex input, or --
     * under allow_computed_varying -- a value the producer computes.  A computed
     * varying has a store value that is not a straight input load; the multi-pass
     * producer renders it on the fragment ALU into its own BO. */
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
            if (!val || val->intrinsic != nir_intrinsic_load_deref) {
                if (allow_computed_varying)
                    continue; /* the multi-pass producer renders this varying */
                return false;
            }
            nir_variable *src = nir_intrinsic_get_var(val, 0);
            if (!src || !(src->data.mode & nir_var_shader_in))
                return false;
        }
    }

    /* The producer feeds velem[0] to the re-staged FS's VAR0, and the re-stager
     * assigns VAR0 to the first-declared input.  So only that first input may
     * feed computation; every other input must appear solely as a passthrough
     * varying source.  (velem[0] == the first input holds for the position-first
     * shapes this targets; arbitrary input ordering and a second computed
     * attribute need the multi-input producer and are rejected here.) */
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
            if (!in || !(in->data.mode & nir_var_shader_in) || in == first_in)
                continue;
            /* A non-first input: its every use must be a passthrough varying store. */
            nir_foreach_use(use, &intr->def) {
                if (nir_src_is_if(use))
                    return false;
                nir_instr *cons = nir_src_use_instr(use);
                if (cons->type != nir_instr_type_intrinsic)
                    return false;
                nir_intrinsic_instr *ci = nir_instr_as_intrinsic(cons);
                if (ci->intrinsic != nir_intrinsic_store_deref)
                    return false;
                nir_variable *o = nir_intrinsic_get_var(ci, 0);
                if (!o || !(o->data.mode & nir_var_shader_out) ||
                    o->data.location == VARYING_SLOT_POS)
                    return false;
            }
        }
    }
    return true;
}

enum r300_r2vb_verdict r300_r2vb_classify_draw(struct r300_context *r300,
                                               const struct pipe_draw_info *info,
                                               const struct pipe_draw_start_count_bias *draw)
{
    /* The route is the no-hardware-vertex-shader path: it only makes sense where
     * the part has no VAP vertex FPUs and runs SWTCL. */
    if (r300->screen->caps.has_tcl || r300->screen->caps.num_vert_fpus != 0)
        return R2VB_REJECT_HW_TCL;
    /* The producer rasterizes one point per output slot and the re-ingest draws a
     * linear vertex list; an index buffer would need an index-aware producer. */
    if (info->index_size != 0)
        return R2VB_REJECT_INDEXED;
    if (info->instance_count != 1)
        return R2VB_REJECT_INSTANCED;
    /* VAP_VF_MAX_VTX_INDX is 16-bit, so the re-ingest tops out below 2^16. */
    if (draw->count == 0 || draw->count >= 65536)
        return R2VB_REJECT_COUNT;
    /* Only the topologies proven pixel-exact through the re-ingest (POINTS is in
     * the set structurally; its rasterization is a separate open item). */
    switch (info->mode) {
    case MESA_PRIM_POINTS:
    case MESA_PRIM_LINES:
    case MESA_PRIM_LINE_STRIP:
    case MESA_PRIM_LINE_LOOP:
    case MESA_PRIM_TRIANGLES:
    case MESA_PRIM_TRIANGLE_STRIP:
    case MESA_PRIM_TRIANGLE_FAN:
        break;
    default:
        return R2VB_REJECT_PRIM;
    }
    /* Structurally eligible.  An identity VS needs no transform -- the app vertex
     * buffer can re-ingest directly (PASSTHROUGH); anything else needs the
     * fragment-ALU transform producer first (CANDIDATE). */
    return r300_vs_is_passthrough(r300) ? R2VB_ROUTE_PASSTHROUGH : R2VB_ROUTE_CANDIDATE;
}

bool r300_r2vb_route_draw(struct r300_context *r300,
                          const struct pipe_draw_info *info,
                          const struct pipe_draw_start_count_bias *draw)
{
    /* Gate read once: this runs on every draw, so do not getenv per call. */
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("R300_R2VB_ROUTE");
        enabled = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (!enabled)
        return false;

    static unsigned tally[R2VB_VERDICT_COUNT];
    static unsigned total;
    enum r300_r2vb_verdict v = r300_r2vb_classify_draw(r300, info, draw);
    tally[v]++;
    total++;

    /* Per-draw trace (R300_R2VB_ROUTE_DEBUG=1): one line per draw with the bound
     * VS name, vertex count, and verdict, so the application's draw is visible
     * rather than only the first (often an internal setup/clear) draw. */
    static int dbg = -1;
    if (dbg < 0) {
        const char *e = getenv("R300_R2VB_ROUTE_DEBUG");
        dbg = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (dbg) {
        struct r300_vertex_shader *vs = r300_vs(r300);
        const char *vsname = (vs && vs->state.type == PIPE_SHADER_IR_NIR &&
                              vs->state.ir.nir && vs->state.ir.nir->info.name)
                                 ? vs->state.ir.nir->info.name
                                 : "?";
        static const char *vname[R2VB_VERDICT_COUNT] = {
            "passthrough", "candidate", "hw_tcl", "indexed", "instanced", "count", "prim"};
        fprintf(stderr, "r2vb_route_draw #%u verdict=%s is_mvp=%d vs=%s count=%u mode=%u\n",
                total, vname[v], r300_vs_is_mvp(r300), vsname, draw->count, info->mode);
    }

    /* R300_R2VB_VS_DUMP: one-shot NIR-shape dump of the bound VS so the MVP
     * matcher is pinned to the real nir_to_tgsi output (finding F2).  No submit. */
    static int vsdump = -1;
    if (vsdump < 0) {
        const char *e = getenv("R300_R2VB_VS_DUMP");
        vsdump = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (vsdump && v == R2VB_ROUTE_CANDIDATE) {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            r300_vs_dump_nir_shape(r300);
            /* For an MVP candidate, also build the transform-FS + slot-pixel BO
             * (no submit) so RADEON_DEBUG=fp prints the 4-DP4 program and the BO
             * positions are confirmed before the producer pass is wired. */
            if (r300_vs_is_mvp(r300))
                r300_r2vb_mvp_init_selftest(r300, draw->count);
        }
    }
    /* Periodic verdict distribution so a real workload shows how much of its draw
     * stream is route-eligible without per-draw log spam. */
    if (total == 1 || (total & 511u) == 0)
        fprintf(stderr,
                "r2vb_route_tally total=%u passthrough=%u candidate=%u hw_tcl=%u "
                "indexed=%u instanced=%u count=%u prim=%u\n",
                total, tally[R2VB_ROUTE_PASSTHROUGH], tally[R2VB_ROUTE_CANDIDATE],
                tally[R2VB_REJECT_HW_TCL], tally[R2VB_REJECT_INDEXED],
                tally[R2VB_REJECT_INSTANCED], tally[R2VB_REJECT_COUNT],
                tally[R2VB_REJECT_PRIM]);

    /* Execute only the PASSTHROUGH class, and only under a second opt-in
     * (R300_R2VB_EXEC=1) so classification can run without changing rendering.
     * A passthrough draw's vertices reach r300 already in clip space (the VS is
     * identity), so the caller re-ingests the app vertex buffer directly at
     * TCL_BYPASS, skipping the gallivm draw module.  CANDIDATE (needs the
     * fragment-ALU transform producer) and every reject still fall back to
     * gallivm. */
    static int exec = -1;
    if (exec < 0) {
        const char *e = getenv("R300_R2VB_EXEC");
        exec = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    /* R300_R2VB_INSPECT reaches the exec function too, where it dumps the routing
     * state and falls back -- no submit -- so the no-submit capture does not need
     * the suspected-hang R300_R2VB_EXEC opt-in. */
    static int inspect = -1;
    if (inspect < 0) {
        const char *e = getenv("R300_R2VB_INSPECT");
        inspect = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return (exec || inspect) && v == R2VB_ROUTE_PASSTHROUGH;
}

/* Gate the MVP route on its own opt-in so the passthrough exec stays unaffected.
 * Returns true only for an MVP-shape candidate draw under R300_R2VB_MVP_EXEC. */
bool r300_r2vb_route_mvp(struct r300_context *r300,
                         const struct pipe_draw_info *info,
                         const struct pipe_draw_start_count_bias *draw)
{
    static int gate = -1;
    if (gate < 0) {
        const char *e = getenv("R300_R2VB_MVP_EXEC");
        gate = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (!gate)
        return false;
    if (r300_r2vb_classify_draw(r300, info, draw) != R2VB_ROUTE_CANDIDATE)
        return false;
    /* The re-staging route (R300_R2VB_RESTAGE) derives the producer from the VS,
     * so it runs any fragment-aluable straight-line VS; the hand-built and
     * externals producers only express the exact MVP shape. */
    static int restage = -1;
    if (restage < 0) {
        const char *e = getenv("R300_R2VB_RESTAGE");
        restage = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    /* Computed varyings (a fragment-aluable function of the position input, not a
     * straight passthrough) ride a further opt-in: the route then runs a producer
     * pass per varying into its own BO.  Off by default so the proven
     * passthrough-varying route is unchanged (a computed-varying VS falls back to
     * gallivm rather than rendering the wrong colour from the application buffer). */
    static int varying = -1;
    if (varying < 0) {
        const char *e = getenv("R300_R2VB_VARYING");
        varying = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return restage ? r300_vs_is_fragment_aluable(r300, varying) : r300_vs_is_mvp(r300);
}

/* Route-exec MVP path: run gl_Position = M * in_pos on the fragment ALU.  The
 * producer transforms the application's model-space positions into a clip-space
 * BO through the 4-DP4 transform-FS, emitted under the normal draw flow (where
 * prepare_for_rendering carries the FS/RS/const atoms) -- not the r300_flush
 * self-test, which is reentrant.  R300_R2VB_XFORM_VERIFY flushes and checks the
 * BO holds M*model.  The re-ingest (draw the transformed positions with the
 * application FS) is the remaining step; until then this returns false so the
 * draw falls back to gallivm and the screen stays correct. */
bool r300_r2vb_exec_mvp_draw(struct r300_context *r300,
                             const struct pipe_draw_info *info,
                             const struct pipe_draw_start_count_bias *draw)
{
    if (!r300->swtcl_vs_const0_ptr || r300->swtcl_vs_const0_size < 64)
        return false;
    if (!r300->velems || r300->velems->count == 0)
        return false;
    unsigned count = draw->count;
    if (count == 0 || count > 4096)
        return false;
    const float *cols = (const float *)r300->swtcl_vs_const0_ptr;

    /* Read the model-space positions.  in_pos is velem[0] (location 0) for an
     * MVP VS; honor its buffer, offset, stride, and component count (a vec3
     * position gets w = 1). */
    struct pipe_vertex_element *pe = &r300->velems->velem[0];
    struct pipe_vertex_buffer *vb = &r300->vertex_buffer[pe->vertex_buffer_index];
    const uint8_t *base = NULL;
    if (vb->is_user_buffer)
        base = vb->buffer.user;
    else if (vb->buffer.resource)
        base = r300_resource(vb->buffer.resource)->malloced_buffer;
    if (!base || !pe->src_stride)
        return false;
    base += vb->buffer_offset + pe->src_offset;
    unsigned comps = util_format_get_nr_components(pe->src_format);

    float (*model)[4] = malloc((size_t)count * sizeof(*model));
    if (!model)
        return false;
    for (unsigned i = 0; i < count; i++) {
        const float *v = (const float *)(base + (size_t)(draw->start + i) * pe->src_stride);
        model[i][0] = v[0];
        model[i][1] = comps > 1 ? v[1] : 0.0f;
        model[i][2] = comps > 2 ? v[2] : 0.0f;
        model[i][3] = comps > 3 ? v[3] : 1.0f;
    }

    /* No-submit diagnostic (R300_R2VB_DIAG): dump the matrix this draw reads as
     * cols[] and re-stage both producer targets on the CPU (R300_R2VB_VS_DUMP
     * prints the NIR), then return false so gallivm renders the frame.  Every
     * fact that discriminates a producer-correctness bug is computed before any
     * CS submit, so this path emits no GPU work and cannot wedge the part. */
    if (getenv("R300_R2VB_DIAG")) {
        fprintf(stderr, "r2vb_diag swtcl_vs_const0_size=%u cols=", r300->swtcl_vs_const0_size);
        for (int i = 0; i < 16; i++)
            fprintf(stderr, "%.4f%s", cols[i], i == 15 ? "\n" : ",");
        int dv = r300_r2vb_first_computed_varying(r300_vs(r300)->state.ir.nir);
        fprintf(stderr, "r2vb_diag first_computed_varying=%d\n", dv);
        void *pf = r300_r2vb_restage_vs_as_fs(r300, r300_vs(r300)->state.ir.nir,
                                              VARYING_SLOT_POS);
        if (pf) {
            /* Capture the derived VAP/RS state the producer would inherit for this
             * bound VS, up to (not including) emit_producer -- no CS submit.  Bind
             * the producer VS too (the fix), so the dump shows the routing the
             * producer actually emits; after the fix the 3-output VS's dump must
             * match the 2-output VS's. */
            void *saved_fs = r300->fs.state;
            void *saved_vs = r300->vs_state.state;
            void *pvs = r300_r2vb_get_producer_vs(r300);
            r300->context.bind_fs_state(&r300->context, pf);
            if (pvs)
                r300->context.bind_vs_state(&r300->context, pvs);
            r300_update_derived_state(r300);
            r300_r2vb_dump_xform_routing(r300);
            r300->context.bind_fs_state(&r300->context, saved_fs);
            if (pvs)
                r300->context.bind_vs_state(&r300->context, saved_vs);
            r300_update_derived_state(r300);
            r300->context.delete_fs_state(&r300->context, pf);
        }
        if (dv >= 0) {
            void *vf = r300_r2vb_restage_vs_as_fs(r300, r300_vs(r300)->state.ir.nir,
                                                  (gl_varying_slot)dv);
            if (vf)
                r300->context.delete_fs_state(&r300->context, vf);
        }
        free(model);
        return false;
    }

    struct pipe_resource *clip = r2vb_create_selftest_bo(r300, align(count, 2) * 16, 0);
    if (!clip) {
        free(model);
        return false;
    }

    /* Transform FS.  Default: bake the transposed matrix as immediates -- a
     * matrix-specific FS rebuilt and deleted per draw.  R300_R2VB_MVP_EXTERNALS
     * opts into the cached FS (r300_r2vb_get_transform_fs) that reads the matrix
     * rows from FS constant buffer 0 (now a sized block-0 UBO declaration so
     * nir_to_rc emits the externals), set per draw by r300_r2vb_set_transform_consts
     * (which transposes the column-major cols into DP4 rows).  Externals drops the
     * per-draw recompile and is the const-file form a general VS needs; it stays
     * opt-in behind the proven baked path until the framebuffer oracle confirms
     * it byte-exact.
     *
     * TODO: r300_r2vb_set_transform_consts overwrites FS constant buffer 0, which
     *       the multi-stream re-ingest's application FS also reads.  Restore the
     *       application's bound FS constant buffer before the re-ingest when the
     *       app FS reads uniforms (the validation FS fs_solid reads none, so the
     *       overwrite is invisible to the oracle).  Grounded in
     *       r300_set_constant_buffer and r300->fs_constants. */
    static int externals = -1, restage = -1;
    if (externals < 0) {
        const char *e = getenv("R300_R2VB_MVP_EXTERNALS");
        externals = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (restage < 0) {
        const char *e = getenv("R300_R2VB_RESTAGE");
        restage = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    void *xfs;
    bool xfs_cached = false;
    if (restage) {
        /* Derive the producer from the bound VS itself -- the general fragment-ALU
         * vertex route.  Re-stage its NIR as the FS (position only) and load the
         * matrix it reads from UBO[0] into FS const file 0 untransposed (the VS
         * body is column-MAD).  Built per draw and deleted after, like the baked
         * path, since the FS tracks whatever VS is bound. */
        xfs = r300_r2vb_restage_vs_as_fs(r300, r300_vs(r300)->state.ir.nir, VARYING_SLOT_POS);
        if (xfs)
            r300_r2vb_set_transform_consts_raw(r300, cols);
    } else if (externals) {
        xfs = r300_r2vb_get_transform_fs(r300);
        if (xfs)
            r300_r2vb_set_transform_consts(r300, cols);
        xfs_cached = true;
    } else {
        float rows[16];
        for (unsigned i = 0; i < 4; i++)
            for (unsigned j = 0; j < 4; j++)
                rows[i * 4 + j] = cols[j * 4 + i];
        xfs = r300_r2vb_build_baked_transform_fs(r300, rows);
    }
    if (!xfs) {
        pipe_resource_reference(&clip, NULL);
        free(model);
        return false;
    }

    /* Bind the transform-FS and the minimal producer VS, recompute derived (RS)
     * state for the producer's two-vec4 vertex (not the application VS's varying
     * count), then emit that state and the producer under the normal draw flow.
     * The producer VS pins VAP_OUT_VTX_FMT / PSC / RS to the embedded vertex so
     * the VAP does not fetch past it for an application VS with extra varyings. */
    void *saved_fs = r300->fs.state;
    void *saved_vs = r300->vs_state.state;
    void *pvs = r300_r2vb_get_producer_vs(r300);
    r300->context.bind_fs_state(&r300->context, xfs);
    if (pvs)
        r300->context.bind_vs_state(&r300->context, pvs);
    r300_update_derived_state(r300);
    if (r300_r2vb_prepare_states(r300, 64 + (int)count * 8 + 64))
        r300_r2vb_emit_producer(r300, r300_resource(clip), 0, count, model, true);
    r300->context.bind_fs_state(&r300->context, saved_fs);
    if (pvs)
        r300->context.bind_vs_state(&r300->context, saved_vs);
    r300_update_derived_state(r300);
    if (!xfs_cached)
        r300->context.delete_fs_state(&r300->context, xfs);

    /* Order the producer's transform write before the re-ingest vertex fetch.
     * Default (two-submit, proven byte-exact): submit the producer, then a
     * map-for-read waits for that submit and makes clip coherent.  The radeon
     * winsys pools BOs, so clip can be recycled from a prior VBO; without the
     * wait the VAP fetch reads stale (untransformed) content even though
     * LOAD_VBPNTR points at clip (R300_R2VB_VA_DUMP shows identical array wiring,
     * wrong vs right pixels).  The map is a per-draw CPU stall.
     *
     * R300_R2VB_MVP_SINGLECS runs the producer and re-ingest in one command
     * stream with no flush and no map.  radeon_drm_cs_add_buffer deduplicates:
     * the producer's RADEON_USAGE_READWRITE|COLOR_BUFFER add and the re-ingest's
     * vertex add for clip merge into one relocation, so there is no usage
     * conflict.  The ordering is carried instead by re-asserting the
     * cb_flush_clean + VAP_PVS_STATE_FLUSH barrier adjacent to the re-ingest
     * draw (r2vb_reingest_barrier, emitted in r300_r2vb_exec_passthrough_draw):
     * emit_producer's own barrier precedes prepare_for_rendering's dirty-state
     * re-emit, so it is no longer adjacent to the fetch in the shared stream. */
    static int single_cs = -1;
    if (single_cs < 0) {
        const char *e = getenv("R300_R2VB_MVP_SINGLECS");
        single_cs = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (!single_cs) {
        r300->context.flush(&r300->context, NULL, 0);
        struct pipe_transfer *sxfer = NULL;
        struct pipe_box sbox = { .width = (count ? count : 1) * 16, .height = 1, .depth = 1 };
        void *sm = r300->context.buffer_map(&r300->context, clip, 0, PIPE_MAP_READ, &sbox, &sxfer);
        if (sm)
            r300->context.buffer_unmap(&r300->context, sxfer);
    }
    if (getenv("R300_R2VB_XFORM_VERIFY"))
        r2vb_verify_xform_readback(r300, clip, model, count, cols);

    /* Computed-varying producer pass (R300_R2VB_VARYING): re-stage the bound VS
     * targeting its first computed varying and render that value on the fragment
     * ALU into a dedicated BO, the same proven single-RT producer the position
     * pass uses, fed the same velem[0] attribute.  R300_R2VB_VARYING_VERIFY=<k>
     * reads the BO back against model*k -- an FP24-exact input through an exact op
     * (k a power of two) stays bit-exact, so the tight readback, not the 8-bit
     * colour, is the proof.
     *
     * When a computed varying is produced, RETURN before the re-ingest below.
     * That re-ingest is the unmodified passthrough path: it feeds varyings from
     * the application buffers, which cannot carry a computed varying, and a VS
     * with a computed-varying output mismatches its PSC/VAP setup and hangs the
     * draw (the timeout-kill then poisons the ring).  Skipping it isolates the
     * production + oracle, exactly the #1a scope; wiring the producer BO into the
     * re-ingest is the next increment. */
    static int varying = -1;
    if (varying < 0) {
        const char *e = getenv("R300_R2VB_VARYING");
        varying = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    int vslot = varying ? r300_r2vb_first_computed_varying(r300_vs(r300)->state.ir.nir) : -1;
    if (vslot >= 0) {
        void *vfs = r300_r2vb_restage_vs_as_fs(r300, r300_vs(r300)->state.ir.nir,
                                               (gl_varying_slot)vslot);
        struct pipe_resource *vbo = r2vb_create_selftest_bo(r300, align(count, 2) * 16, 0);
        if (vfs && vbo) {
            void *saved = r300->fs.state;
            void *saved_vs = r300->vs_state.state;
            void *pvs = r300_r2vb_get_producer_vs(r300);
            r300->context.bind_fs_state(&r300->context, vfs);
            if (pvs)
                r300->context.bind_vs_state(&r300->context, pvs);
            r300_update_derived_state(r300);
            if (r300_r2vb_prepare_states(r300, 64 + (int)count * 8 + 64))
                r300_r2vb_emit_producer(r300, r300_resource(vbo), 0, count, model, true);
            r300->context.bind_fs_state(&r300->context, saved);
            if (pvs)
                r300->context.bind_vs_state(&r300->context, saved_vs);
            r300_update_derived_state(r300);
            r300->context.flush(&r300->context, NULL, 0);
            const char *vv = getenv("R300_R2VB_VARYING_VERIFY");
            const char *vmat = getenv("R300_R2VB_VARYING_MAT");
            const char *vtolenv = getenv("R300_R2VB_VARYING_TOL");
            if (vv || vmat) {
                /* Expected = M * inPos (row-major R300_R2VB_VARYING_MAT, 16
                 * floats) for a matrix computed varying, else inPos * factor.  The
                 * matrix case is the lossier check: FP24 error accumulates across
                 * the four mul-adds per row, so the BO readback (not the 8-bit
                 * colour) is the proof, and R300_R2VB_VARYING_TOL sets the FP24
                 * window. */
                float vtol = vtolenv ? (float)atof(vtolenv) : 1e-4f;
                if (vtol <= 0.0f)
                    vtol = 1e-4f;
                float mat[4][4];
                bool have_mat = false;
                if (vmat) {
                    char *p = (char *)vmat;
                    int n = 0;
                    for (; n < 16 && p && *p; n++) {
                        mat[n / 4][n % 4] = (float)strtod(p, &p);
                        while (*p == ',' || *p == ' ')
                            p++;
                    }
                    have_mat = (n == 16);
                }
                float factor = vv ? (float)atof(vv) : 1.0f;
                if (factor == 0.0f)
                    factor = 1.0f;
                float (*vexp)[4] = malloc((size_t)count * sizeof(*vexp));
                if (vexp) {
                    for (unsigned s = 0; s < count; s++)
                        for (int i = 0; i < 4; i++)
                            vexp[s][i] = have_mat
                                ? (mat[i][0] * model[s][0] + mat[i][1] * model[s][1] +
                                   mat[i][2] * model[s][2] + mat[i][3] * model[s][3])
                                : model[s][i] * factor;
                    r2vb_verify_bo_readback(r300, vbo, vexp, count, vtol, "varying");
                    free(vexp);
                }
            }
        }
        if (vfs)
            r300->context.delete_fs_state(&r300->context, vfs);
        pipe_resource_reference(&vbo, NULL);
        pipe_resource_reference(&clip, NULL);
        free(model);
        return true;
    }

    /* Multi-stream re-ingest.  Draw the transformed positions (from clip) plus the
     * model's pass-through attributes (from the application buffers, unchanged by
     * an MVP VS) with the application fragment shader.  Redirect ONLY the position
     * element (velem[0] for an MVP VS) to a fresh clip vertex-buffer slot and
     * reuse the proven passthrough re-ingest: it uploads the other attributes,
     * emits the multi-array LOAD_VBPNTR (velem i -> PSC stream i -> its VS-output
     * vector, so position lands on vec0 and each varying on its own vec), applies
     * the hardware viewport transform, and resets the rasterizer state.  clip is a
     * real winsys BO, used as-is.  Restored after. */
    unsigned clip_slot = r300->nr_vertex_buffers;
    if (clip_slot >= PIPE_MAX_ATTRIBS) {
        pipe_resource_reference(&clip, NULL);
        free(model);
        return false;
    }
    struct pipe_vertex_element saved_pe = r300->velems->velem[0];
    unsigned saved_fmtsz = r300->velems->format_size[0];
    struct pipe_vertex_buffer saved_vb = r300->vertex_buffer[clip_slot];
    unsigned saved_nvb = r300->nr_vertex_buffers;

    r300->velems->velem[0].vertex_buffer_index = clip_slot;
    r300->velems->velem[0].src_offset = 0;
    r300->velems->velem[0].src_stride = 16;
    r300->velems->velem[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
    memset(&r300->vertex_buffer[clip_slot], 0, sizeof(struct pipe_vertex_buffer));
    r300->vertex_buffer[clip_slot].buffer.resource = clip;
    r300->nr_vertex_buffers = clip_slot + 1;

    /* The producer hand-rolled rasterizer/framebuffer/scissor/viewport/ZB
     * registers (CLIP_DISABLE, no-cull, the clip color target, a one-row scissor)
     * outside the atom system, and those persist in GPU registers across the
     * flush.  Mark the owning atoms dirty so the re-ingest's prepare re-emits the
     * application values; otherwise the producer's CLIP_DISABLE rasterizes the
     * triangle far too large (covered ~834 vs 128). */
    r300_mark_atom_dirty(r300, &r300->fb_state);
    r300_mark_atom_dirty(r300, &r300->scissor_state);
    r300_mark_atom_dirty(r300, &r300->viewport_state);
    r300_mark_atom_dirty(r300, &r300->dsa_state);
    r300_mark_atom_dirty(r300, &r300->rs_state);

    if (getenv("R300_R2VB_VA_DUMP"))
        fprintf(stderr, "r2vb_mvp_reingest clip=%p buf=%p clip_slot=%u velem0_vbi=%u nvb=%u\n",
                (void *)clip, (void *)r300_resource(clip)->buf, clip_slot,
                r300->velems->velem[0].vertex_buffer_index, r300->nr_vertex_buffers);
    r300->r2vb_reingest_barrier = single_cs;
    bool ok2 = r300_r2vb_exec_passthrough_draw(r300, info, draw);
    r300->r2vb_reingest_barrier = false;

    r300->velems->velem[0] = saved_pe;
    r300->velems->format_size[0] = saved_fmtsz;
    r300->vertex_buffer[clip_slot] = saved_vb;
    r300->nr_vertex_buffers = saved_nvb;

    pipe_resource_reference(&clip, NULL);
    free(model);
    return ok2;
}
