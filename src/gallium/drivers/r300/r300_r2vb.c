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
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

#include "compiler/r300_nir.h"
#include "r300_context.h"
#include "r300_r2vb_clip.h"
#include "r300_state_inlines.h"
#include "r300_cs.h"
#include "r300_emit.h"
#include "r300_fs.h"
#include "r300_nir_ssa_cut.h"
#include "r300_r2vb.h"
#include "r300_r2vb_capture_gate.h"
#include "r300_r2vb_plan.h"
#include "r300_r2vb_telemetry.h"
#include "r300_reg.h"
#include "r300_vs.h"

/* Forward: CPU classify/edge maps and re-ingest run before these helpers. */
static void r2vb_wait_bo(struct r300_context *r300, struct pipe_resource *res);
static bool r2vb_single_cs_enabled(void);

/* Active hardwired clip planes for the oracle: XY always; NEAR/FAR only when
 * the rasterizer keeps depth clipping (draw_update_clip_flags parity). */
static uint8_t
r2vb_active_clip_planes(const struct r300_context *r300)
{
    const struct r300_rs_state *rs =
        r300->rs_state.state ? (const struct r300_rs_state *)r300->rs_state.state
                           : NULL;
    uint8_t m = R300_R2VB_CLIP_RIGHT | R300_R2VB_CLIP_LEFT | R300_R2VB_CLIP_TOP |
                R300_R2VB_CLIP_BOTTOM;
    if (!rs || rs->rs.depth_clip_near)
        m |= R300_R2VB_CLIP_NEAR;
    if (!rs || rs->rs.depth_clip_far)
        m |= R300_R2VB_CLIP_FAR;
    return m;
}

void r300_r2vb_report_bo_identity(struct r300_context *r300, const char *tag,
                                  struct pipe_resource *pr)
{
    struct radeon_winsys *rws = r300->rws;
    struct r300_resource *rr = pr ? r300_resource(pr) : NULL;
    struct pb_buffer_lean *buf = rr ? rr->buf : NULL;
    if (!buf) {
        /* A CPU-shadow or user vertex buffer has no winsys BO until the delivery
         * path uploads it; report the resource pointer and that the BO is not
         * yet materialized so the reader distinguishes it from a missing slot. */
        fprintf(stderr, "%s res=%p buf=none\n", tag, (void *)pr);
        return;
    }
    int reloc = rws->cs_lookup_buffer(&r300->cs, buf);
    /* The radeon DRM winsys leaves buffer_get_flags unset (the vtable slot
     * exists for amdgpu); calling through the NULL pointer faults, so report
     * the flags only when the op is wired and print -1 otherwise. */
    int flags = rws->buffer_get_flags ? (int)rws->buffer_get_flags(buf) : -1;
    fprintf(stderr,
            "%s res=%p buf=%p size=%" PRIu64 " suballoc=%d parent_offset=%u "
            "va=0x%" PRIx64 " domain=0x%x flags=%d reloc_index=%d "
            "parent_bo=not_reachable\n",
            tag, (void *)pr, (void *)buf, (uint64_t)buf->size,
            rws->buffer_is_suballocated(buf),
            rws->buffer_get_reloc_offset(buf),
            rws->buffer_get_virtual_address(buf),
            rws->buffer_get_initial_domain(buf), flags, reloc);
}

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

/* Perspective-divide gate: the transform fragment shader emits window
 * coordinates when R300_R2VB_DIVIDE is set. The clip-space MVP self-test
 * remains the default oracle. The divided result is compared with the CPU
 * reference before delivery. */

static bool r2vb_exec_debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_EXEC_DEBUG");
        if (!e)
            e = getenv("R300_R2VB_ROUTE_DEBUG");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached;
}

static bool r300_r2vb_divide_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_DIVIDE");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached;
}

/* Build the producer FS output vec4 from the four transform DP4 results, shared by
 * every transform-FS builder so the divide is a property of the producer output
 * contract, not of one FS-construction variant.  Divide gate off: the raw
 * clip-space vec4 (the demonstrated MVP path).  Divide gate on: perspective divide
 * then viewport, window.xyz = (clip.xyz / w_clip) * viewport_scale + viewport_bias
 * with w = 1, reproducing the contract r2vb_verify_window_readback checks.  The
 * reciprocal is a native alpha-pipe RCP co-issued with the transform DP4s and the
 * three viewport terms are native MADs, so the divide adds no slot pair over the
 * bare transform.  Geometric clip (04c) precedes this in the collineation domain,
 * so w_clip > 0 in normal use; the 1/32768 guard bounds the FP24 reciprocal
 * defensively.  Reads r300->viewport for the same scale/bias the CPU oracle uses. */
static nir_def *r2vb_divide_position(nir_builder *b, nir_def *pos,
                                     struct r300_context *r300,
                                     enum r300_r2vb_position_space space)
{
    if (space != R300_R2VB_POSITION_WINDOW)
        return pos;
    const struct pipe_viewport_state *vp = &r300->viewport;
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

static nir_def *r2vb_build_producer_output(nir_builder *b, nir_def *comp[4],
                                           struct r300_context *r300,
                                           enum r300_r2vb_position_space space)
{
    return r2vb_divide_position(b, nir_vec4(b, comp[0], comp[1], comp[2], comp[3]),
                                r300, space);
}

/* Clip-classification oracle gate (R300_R2VB_CLIP_CLASSIFY): run the
 * position producer in POSITION_CLIP mode, classify the actual FP24 clip-BO
 * contents against the Draw-parity clip codes, print machine-readable
 * records, and fall back to gallivm without delivering.  Diagnostic only --
 * no route behavior changes under the gate beyond the forced clip-space
 * producer and the fallback. */
static bool r300_r2vb_clip_classify_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_CLIP_CLASSIFY");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached;
}

/* Conservative clip-route action gate (R300_R2VB_CLIP_ROUTE): classify the
 * FP24 clip BO per triangle and act on the whole draw -- every triangle
 * trivially accepted runs the window-space producer and the verbatim-fetch
 * delivery; every triangle trivially rejected consumes the draw with no
 * delivery submit; anything else (partial, mixed, unsafe w, unsupported
 * draw state) falls the whole draw back to gallivm.  No per-triangle
 * splitting -- geometric clipping is edge generation's job. */
static bool r300_r2vb_auto_single_armed(uint32_t *floor_out);

static bool r300_r2vb_clip_route_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_CLIP_ROUTE");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    /* The armed AUTO_SINGLE canary delivers through the clip-route action;
     * the producer path only runs for draws the canary admitted, so the
     * implied action never engages outside the admission contract. */
    return cached || r300_r2vb_auto_single_armed(NULL);
}

/* Edge-generation gate (R300_R2VB_CLIP_EDGE, requires R300_R2VB_CLIP_ROUTE):
 * a PARTIAL or mixed accept/reject draw is rebuilt on the CPU instead of
 * falling back -- accepted triangles pass through verbatim, rejected ones are
 * dropped, and each straddling triangle is Sutherland-Hodgman clipped in
 * clip space against its failing planes with the position INPUT interpolated
 * at each intersection (t = d_out / (d_out - d_in)), then fanned back into a
 * TRIANGLES list.  The producers re-run over the clipped inputs, so every
 * carried output (position, computed varyings) is regenerated on the
 * fragment ALU from the interpolated inputs -- exact for the affine producer
 * class the route gate admits, where output(lerp(in)) == lerp(output(in)).
 * The delivery must contain no passthrough streams: those fetch application
 * buffers by original vertex index, which the rebuilt list invalidates. */
static bool r300_r2vb_clip_edge_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_CLIP_EDGE");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached;
}

/* Split-delivery bypass (R300_R2VB_SPLIT_DELIVERY_BYPASS) declines direct
 * re-ingest after a window-space split producer finishes. The route releases
 * its temporary resources and returns false, so the caller renders the
 * original draw through gallivm. */
static bool r300_r2vb_split_delivery_bypass_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_SPLIT_DELIVERY_BYPASS");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached;
}

/* Fresh clip-copy gate (R300_R2VB_FRESH_CLIP_COPY): after the split producers
 * complete, CPU-copy the verified window-space clip output into a newly
 * allocated buffer that was never a producer render target, and deliver from
 * the fresh buffer.  The copy is two CPU maps and a memcpy -- no extra GPU
 * submission.  The split-produced clip BO carries a render-target reservation
 * history and a color-target-to-vertex-source role transition that the fresh
 * buffer does not; a retiring delivery from the fresh buffer convicts that
 * history, a hanging one convicts the direct-VB delivery draw itself. */
static bool r300_r2vb_fresh_clip_copy_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_FRESH_CLIP_COPY");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached;
}

/* Topology-gather gate (R300_R2VB_TOPOLOGY): a CPU pre-pass resolves
 * TRIANGLE_STRIP, TRIANGLE_FAN, and indexed triangle-family draws (including
 * primitive restart) into a plain triangle-index list before classification,
 * so the clip-route's per-triangle classify and Sutherland-Hodgman rebuild
 * run unchanged over that list.  A gathered list without the clip route to
 * classify and act on it renders nothing useful, so this gate is ANDed with
 * R300_R2VB_CLIP_ROUTE rather than acting alone. */
static bool r300_r2vb_topology_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_TOPOLOGY");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached && r300_r2vb_clip_route_enabled();
}

/* Drop the clip-edge replacement streams after the delivery they were built
 * for (or on any abort past the rebuild).  Idempotent. */
static void r2vb_edge_streams_release(struct r300_context *r300)
{
    if (!r300->r2vb_edge_streams_active)
        return;
    for (unsigned i = 0; i < PIPE_MAX_ATTRIBS; i++) {
        free(r300->r2vb_edge_stream_attr[i]);
        r300->r2vb_edge_stream_attr[i] = NULL;
    }
    r300->r2vb_edge_streams_active = false;
}

/* The process-global divide gate maps onto the explicit producer contract:
 * R300_R2VB_DIVIDE selects the window-space producer, default is raw clip. */
static enum r300_r2vb_position_space r2vb_env_space(void)
{
    return r300_r2vb_divide_enabled() ? R300_R2VB_POSITION_WINDOW
                                      : R300_R2VB_POSITION_CLIP;
}

/* Build (once) the 4-DP4 transform fragment program: gl_FragColor = M * in_attr,
 * where in_attr is the per-slot input vertex delivered as a flat GENERIC input
 * and M's four rows live in FS const file 0 (the route sets them per draw from
 * the transposed MVP).  r300 lowers an FS load_ubo(binding 0) to RC_FILE_CONSTANT
 * (nir_to_rc: lower_uniforms_to_ubo + nir_lower_ubo_vec4 -> ntr_emit_load_ubo),
 * so each fdot4(row, in) compiles to one DP4 reading the const file.  Cached on
 * the context; create_fs_state takes ownership of the NIR and precompiles, so
 * RADEON_DEBUG=fp prints the four-DP4 r300 program at creation -- no submit. */
static void *r300_r2vb_get_transform_fs(struct r300_context *r300,
                                         enum r300_r2vb_position_space space)
{
    if (r300->r2vb_transform_fs[space])
        return r300->r2vb_transform_fs[space];

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
    nir_def *o = r2vb_build_producer_output(&b, comp, r300, space);

    nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out_color");
    out->data.location = FRAG_RESULT_COLOR;
    nir_store_var(&b, out, o, 0xf);

    /* Declare a sized block-0 UBO variable so nir_to_rc's ntr_setup_uniforms
     * counts the four matrix rows as RC_CONSTANT_EXTERNAL.  The compiler sizes
     * the const file from the nir_var_mem_ubo interface declaration; a raw
     * load_ubo with only info.num_ubos set carries no size, so externals_count
     * stays 0 and set_constant_buffer(FRAGMENT, 0) is silently ignored (the FS
     * dots its input against garbage).  Same shape r3v_shape_block0_ubo uses
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
    r300->r2vb_transform_fs[space] =
        r300_create_fs_state_internal(&r300->context, &st,
                                      R300_FS_INPUT_R2VB_FLAT_VERTEX);
    if (space == R300_R2VB_POSITION_WINDOW) {
        for (unsigned i = 0; i < 3; i++) {
            r300->r2vb_transform_fs_vp_scale[i] = r300->viewport.scale[i];
            r300->r2vb_transform_fs_vp_translate[i] = r300->viewport.translate[i];
        }
        r300->r2vb_transform_fs_vp_valid = true;
    }
    return r300->r2vb_transform_fs[space];
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
/* Cap on producer model-attribute inputs (application VS inputs feeding the
 * producer FS).  A quaternion rotation and an octonion square need 2 inputs and
 * are HW-confirmed on RS482; the sedenion (CD-4) product of two distinct elements
 * needs 8 (two 16-component sedenions = 8 FP32x4 velems), which also feeds 8
 * generic interpolators -- exactly the R300 RS texcoord-unit count, the binding
 * limit -- and is HW-confirmed per quarter (each quarter compiles to 41 r300 ALU,
 * inside the 64-slot budget).  The producer VAP_OUT_VTX_FMT / PSC packing and the
 * per-input passthrough varyings all scale with the input count.  Raising the cap
 * only widens the gated MVP-route experiments; the default passthrough/transform
 * paths use 1-2 inputs unchanged. */
#define R300_R2VB_MAX_PRODUCER_INPUTS 8

/* Build (and cache) the producer vertex shader for num_inputs model attributes: it
 * passes the embedded slot position (GENERIC0) through to gl_Position and each
 * model attribute (GENERIC1+a) through to VARYING_SLOT_VAR0+a, so the re-staged
 * producer FS reads the application's a-th input at VAR0+a.  The slot position pins
 * VAP_OUT_VTX_FMT / PSC / RS to the embedded vertex regardless of the application
 * VS, and the varying count matches the inputs the FS reads.  Rebuilt when the
 * count changes; num_inputs is clamped to [1, R300_R2VB_MAX_PRODUCER_INPUTS]. */
static void *r300_r2vb_get_producer_vs(struct r300_context *r300, unsigned num_inputs)
{
    if (num_inputs < 1)
        num_inputs = 1;
    if (num_inputs > R300_R2VB_MAX_PRODUCER_INPUTS)
        num_inputs = R300_R2VB_MAX_PRODUCER_INPUTS;
    if (r300->r2vb_producer_vs && r300->r2vb_producer_vs_inputs == num_inputs)
        return r300->r2vb_producer_vs;
    if (r300->r2vb_producer_vs) {
        r300->context.delete_vs_state(&r300->context, r300->r2vb_producer_vs);
        r300->r2vb_producer_vs = NULL;
    }

    const nir_shader_compiler_options *options =
        r300->screen->screen.nir_options[MESA_SHADER_VERTEX];
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                   "r300 r2vb producer VS");

    nir_variable *in_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                               glsl_vec4_type(), "in_pos");
    in_pos->data.location = VERT_ATTRIB_GENERIC0;
    nir_variable *out_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                                glsl_vec4_type(), "gl_Position");
    out_pos->data.location = VARYING_SLOT_POS;
    nir_store_var(&b, out_pos, nir_load_var(&b, in_pos), 0xf);

    /* One passthrough varying per model attribute, in input order: GENERIC1+a feeds
     * VAR0+a, the slot that the re-staged FS reads as the application's input a. */
    for (unsigned a = 0; a < num_inputs; a++) {
        char name[16];
        snprintf(name, sizeof(name), "in_attr%u", a);
        nir_variable *in_a = nir_variable_create(b.shader, nir_var_shader_in,
                                                 glsl_vec4_type(), name);
        in_a->data.location = VERT_ATTRIB_GENERIC1 + a;
        snprintf(name, sizeof(name), "var%u", a);
        nir_variable *out_a = nir_variable_create(b.shader, nir_var_shader_out,
                                                  glsl_vec4_type(), name);
        out_a->data.location = VARYING_SLOT_VAR0 + a;
        nir_store_var(&b, out_a, nir_load_var(&b, in_a), 0xf);
    }

    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = b.shader; /* create_vs_state takes ownership and precompiles */
    r300->r2vb_producer_vs = r300->context.create_vs_state(&r300->context, &st);
    r300->r2vb_producer_vs_inputs = num_inputs;
    return r300->r2vb_producer_vs;
}

/* Build a transform-FS with the MVP baked in as immediates: out = M * in_attr,
 * where the four rows (already transposed so DP4(row_i, v) = (M*v)_i) are
 * nir_imm_vec4 constants.  Hand-built load_ubo on r300 folds to immediate
 * garbage (externals_count=0, so set_constant_buffer is ignored), so the matrix
 * must travel in the program; the FS is therefore matrix-specific and rebuilt
 * per draw (the caller deletes it).  Returns a pipe FS CSO or NULL. */
static void *r300_r2vb_build_baked_transform_fs(struct r300_context *r300,
                                                const float rows[16],
                                                enum r300_r2vb_position_space space)
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
    /* Build raw clip-space or divided window-space output shared with
     * r300_r2vb_get_transform_fs through r2vb_build_producer_output. */
    nir_def *o = r2vb_build_producer_output(&b, comp, r300, space);
    nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out_color");
    out->data.location = FRAG_RESULT_COLOR;
    nir_store_var(&b, out, o, 0xf);

    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = b.shader;
    return r300_create_fs_state_internal(&r300->context, &st,
                                         R300_FS_INPUT_R2VB_FLAT_VERTEX);
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
 * sizes the const file from that block-0 interface (the same externals path
 * r300_r2vb_get_transform_fs relies on), so the caller loads FS const file 0
 * with the matrix the VS expects (untransposed -- the VS body is column-MAD,
 * reading the matrix columns via load_ubo_vec4, not the DP4-transposed rows).
 *
 * This derives the producer from the real shader instead of hand-building the
 * 4-DP4 transform: the arithmetic, the constant reads, and the output count all
 * come from the VS, and nir_to_rc's stage-aware compile (it lowers I/O and emits
 * both VS and FS varyings) does the rest.  Returns the derived FS NIR (caller
 * owns) or NULL; r300_r2vb_restage_vs_as_fs wraps it into a pipe FS CSO, and
 * the admission oracle compiles the same NIR throwaway to measure emitted
 * slots, so the program the oracle admits is the program the producer runs. */
nir_shader *r300_r2vb_build_restaged_fs_nir(struct r300_context *r300,
                                            nir_shader *vs_nir,
                                                   gl_varying_slot target,
                                                   enum r300_r2vb_position_space space)
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
    nir_variable *ins[PIPE_MAX_ATTRIBS];
    unsigned orig_loc[PIPE_MAX_ATTRIBS], orig_frac[PIPE_MAX_ATTRIBS];
    unsigned n_in = 0;
    nir_foreach_variable_with_modes(var, fs, nir_var_shader_in)
        if (n_in < PIPE_MAX_ATTRIBS) {
            orig_loc[n_in] = var->data.location;
            orig_frac[n_in] = var->data.location_frac;
            ins[n_in++] = var;
        }
    /* Rank by a TOTAL order -- location, then location_frac, then array index --
     * so two inputs that share a location (a packed/component-split attribute)
     * still get distinct ranks and distinct VAR slots, rather than aliasing onto
     * the same slot.  Use the snapshot, not ins[]->data.location, since the remap
     * loop below overwrites it. */
    unsigned rank[PIPE_MAX_ATTRIBS];
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
            if (intr->intrinsic != nir_intrinsic_store_deref)
                continue;
            nir_variable *out = nir_intrinsic_get_var(intr, 0);
            if (out && (out->data.mode & nir_var_shader_out) &&
                out->data.location != target) {
                nir_instr_remove(instr);
            } else if (out && (out->data.mode & nir_var_shader_out) &&
                       out->data.location == target &&
                       target == VARYING_SLOT_POS &&
                       intr->src[1].ssa->num_components == 4) {
                /* Position pass output contract: same divide + viewport as the
                 * built transform-FS producers, applied to the cloned VS's clip
                 * result so every producer variant emits window space under the
                 * divide gate. */
                nir_builder wb = nir_builder_at(nir_before_instr(instr));
                nir_def *win = r2vb_divide_position(&wb, intr->src[1].ssa, r300,
                                                    space);
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
    nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));

    if (getenv("R300_R2VB_VS_DUMP"))
        nir_print_shader(fs, stderr);

    return fs;
}

static void *r300_r2vb_restage_vs_as_fs(struct r300_context *r300, nir_shader *vs_nir,
                                        gl_varying_slot target,
                                        enum r300_r2vb_position_space space)
{
    nir_shader *fs = r300_r2vb_build_restaged_fs_nir(r300, vs_nir, target, space);
    if (!fs)
        return NULL;
    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = fs; /* create_fs_state takes ownership and precompiles */
    return r300_create_fs_state_internal(&r300->context, &st,
                                         R300_FS_INPUT_R2VB_FLAT_VERTEX);
}

/* Find the bound VS's first computed-varying output: a non-position output whose
 * store value is not a straight load of a vertex input.  The multi-pass producer
 * renders such a varying on the fragment ALU.  Returns its gl_varying_slot, or -1
 * when every non-position output is a plain passthrough (nothing to produce). */
int r300_r2vb_first_computed_varying(nir_shader *vs_nir)
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

/* Count the application VS inputs that feed gl_Position -- the number of model
 * attributes the position producer must carry.  Mirror the position re-stage
 * (drop every non-position output store, DCE, then drop the now-dead inputs) on a
 * throwaway clone and count what survives, so the count matches the inputs the
 * re-staged position FS actually reads at VAR0+i.  An MVP or single-input-position
 * VS returns 1 even when the application declares extra (varying-only) inputs. */
unsigned r300_r2vb_count_position_inputs(nir_shader *vs_nir)
{
    nir_shader *tmp = nir_shader_clone(NULL, vs_nir);
    if (!tmp)
        return 1;
    nir_function_impl *impl = nir_shader_get_entrypoint(tmp);
    nir_foreach_block(block, impl) {
        nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
                continue;
            nir_variable *out = nir_intrinsic_get_var(intr, 0);
            if (out && (out->data.mode & nir_var_shader_out) &&
                out->data.location != VARYING_SLOT_POS)
                nir_instr_remove(instr);
        }
    }
    nir_opt_dce(tmp);
    nir_remove_dead_variables(tmp, nir_var_shader_in, NULL);
    unsigned n = 0;
    nir_foreach_variable_with_modes(var, tmp, nir_var_shader_in)
        n++;
    ralloc_free(tmp);
    return n < 1 ? 1 : n;
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
    void *fs = r300_r2vb_get_transform_fs(r300, r2vb_env_space());
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
/* Bind a producer matrix into FS const file 0 without disturbing the
 * application const0 mirror; the producer transaction restores the
 * application binding through r300_r2vb_restore_app_fs_consts. */
static void r2vb_bind_producer_fs_consts(struct r300_context *r300,
                                         const void *values, unsigned size)
{
    struct pipe_constant_buffer cb = {0};
    cb.buffer_size = size;
    cb.user_buffer = values;
    r300->r2vb_fs_const0_producer_bind = true;
    r300->context.set_constant_buffer(&r300->context, MESA_SHADER_FRAGMENT, 0, &cb);
    r300->r2vb_fs_const0_producer_bind = false;
}

/* Rebind the application's fragment const0 after a producer pass overwrote
 * the slot with the transform matrix.  Must run before the post-producer
 * r300_update_derived_state, or the application FS variant / constant remap /
 * inline-uniform specialization is selected while the matrix is still the
 * live constant source. */
static void r300_r2vb_restore_app_fs_consts(struct r300_context *r300)
{
    if (!r300->fs_const0_app.valid)
        return;
    struct pipe_constant_buffer cb = {0};
    cb.buffer = r300->fs_const0_app.buffer;
    cb.user_buffer = r300->fs_const0_app.user_buffer;
    cb.buffer_offset = r300->fs_const0_app.buffer_offset;
    cb.buffer_size = r300->fs_const0_app.buffer_size;
    r300->context.set_constant_buffer(&r300->context, MESA_SHADER_FRAGMENT, 0, &cb);
}

/* Load FS const file 0 with the transpose of a column-major MVP so each DP4 row
 * is a matrix row: DP4(row_i, v) = (M*v)_i, row_i = (col0[i],col1[i],col2[i],col3[i]).
 * set_constant_buffer keeps the pointer, not a copy, so the rows live on the
 * context for the duration of the producer pass. */
static void r300_r2vb_set_transform_consts(struct r300_context *r300, const float *cols)
{
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = 0; j < 4; j++)
            r300->r2vb_mvp_rows[i * 4 + j] = cols[j * 4 + i];
    r2vb_bind_producer_fs_consts(r300, r300->r2vb_mvp_rows, 64);
}

/* Load FS const file 0 with the matrix in the layout the re-staged VS expects --
 * the column-major columns verbatim, not the DP4 transpose -- because the
 * re-staged body reads load_ubo_vec4 base=i (column i) and forms
 * col0*x + col1*y + col2*z + col3*w. */
static void r300_r2vb_set_transform_consts_raw(struct r300_context *r300, const float *cols)
{
    memcpy(r300->r2vb_mvp_cols, cols, sizeof(r300->r2vb_mvp_cols));
    r2vb_bind_producer_fs_consts(r300, r300->r2vb_mvp_cols, 64);
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

/* Clip-classification oracle: map the producer BO and classify the
 * ACTUAL FP24 clip-space results the fragment ALU wrote.  A CPU FP32 recompute
 * is deliberately not used as the classification input: near a plane, FP24 and
 * host FP32 can round to opposite sides, and the hardware-produced value is
 * the route's authoritative input.  Guard-band k = 0.5 matches the r300/Draw
 * XY guard-band configuration; the near plane follows the context's
 * clip_halfz.  Triangle records are emitted for TRIANGLES topology; every
 * other topology gets per-vertex records only.
 *
 * The return value is the whole-draw verdict for the conservative route
 * action: ACCEPT only when every classified triangle is trivially accepted,
 * REJECT only when every one is trivially rejected, FALLBACK for everything
 * else -- partial, mixed accept/reject, unsafe w, a non-TRIANGLES topology
 * (no triangle records), or an unmappable BO.  FALLBACK is the safe default:
 * it hands the whole draw to gallivm unchanged. */
enum r2vb_clip_route_verdict {
    R2VB_CLIP_ROUTE_ACCEPT,
    R2VB_CLIP_ROUTE_REJECT,
    R2VB_CLIP_ROUTE_FALLBACK,
};

static enum r2vb_clip_route_verdict
r2vb_clip_classify_readback(struct r300_context *r300,
                            struct pipe_resource *res,
                            uint32_t count, enum mesa_prim mode)
{
    struct pipe_transfer *xfer = NULL;
    struct pipe_box box = {
        .width = (count ? count : 1) * 16, .height = 1, .depth = 1 };
    /* Flush first: single-CS producer may leave the write still in the active
     * cmdbuf, so wait alone would observe an un-submitted BO. */
    r300->context.flush(&r300->context, NULL, 0);
    r2vb_wait_bo(r300, res);
    const float *got =
        r300->context.buffer_map(&r300->context, res, 0, PIPE_MAP_READ, &box, &xfer);
    if (!got) {
        fprintf(stderr, "r2vb_clip map failed\n");
        return R2VB_CLIP_ROUTE_FALLBACK;
    }
    const bool half_z = r300->clip_halfz;
    const float k = R300_R2VB_CLIP_GUARD_K;
    const uint8_t planes = r2vb_active_clip_planes(r300);
    static int clip_log = -1;
    if (clip_log < 0) {
        const char *e = getenv("R300_R2VB_CLIP_LOG");
        clip_log = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }

    for (uint32_t v = 0; v < count; v++) {
        const float *g = &got[v * 4];
        if (clip_log)
            fprintf(stderr,
                    "r2vb_clip vertex=%u pos=%.6f,%.6f,%.6f,%.6f mask=0x%02x%s\n",
                    v, g[0], g[1], g[2], g[3],
                    r300_r2vb_clipcode_planes(g, k, half_z, planes),
                    (r300_r2vb_clip_nonfinite(g) || r300_r2vb_w_unsafe(g[3]))
                        ? " unsafe" : "");
    }

    uint32_t n_accept = 0, n_reject = 0, n_partial = 0, n_fallback = 0;
    if (mode == MESA_PRIM_TRIANGLES) {
        for (uint32_t t = 0; t * 3 + 2 < count; t++) {
            uint8_t om = 0, am = 0;
            enum r300_r2vb_tri_class c = r300_r2vb_classify_triangle_planes(
                &got[(t * 3 + 0) * 4], &got[(t * 3 + 1) * 4],
                &got[(t * 3 + 2) * 4], k, half_z, planes, &om, &am);
            static const char *cname[] = { "accept", "reject", "partial",
                                           "fallback_w" };
            if (clip_log)
                fprintf(stderr, "r2vb_clip prim=%u or=0x%02x and=0x%02x class=%s\n",
                        t, om, am, cname[c]);
            switch (c) {
            case R300_R2VB_TRI_ACCEPT: n_accept++; break;
            case R300_R2VB_TRI_REJECT: n_reject++; break;
            case R300_R2VB_TRI_PARTIAL: n_partial++; break;
            case R300_R2VB_TRI_FALLBACK: n_fallback++; break;
            }
        }
    }
    fprintf(stderr,
            "r2vb_clip summary mode=%u half_z=%d k=%.2f accept=%u reject=%u "
            "partial=%u fallback=%u\n",
            (unsigned)mode, half_z, k, n_accept, n_reject, n_partial,
            n_fallback);
    r300->context.buffer_unmap(&r300->context, xfer);

    if (n_fallback || n_partial || (n_accept && n_reject) ||
        (n_accept == 0 && n_reject == 0))
        return R2VB_CLIP_ROUTE_FALLBACK;
    return n_reject ? R2VB_CLIP_ROUTE_REJECT : R2VB_CLIP_ROUTE_ACCEPT;
}

/* Edge generation: rebuild a TRIANGLES vertex list from the FP24 clip BO and
 * the single position-input model array.  Accepted triangles copy their three
 * model inputs verbatim, rejected triangles are dropped, and each PARTIAL
 * triangle is clipped in clip space against its failing planes with the model
 * input carried through every intersection blend, then fanned.  Positions come
 * from the hardware-produced BO (the route's authoritative classification
 * input); the interpolated payload is the model INPUT, valid because the
 * admitted producer class is affine: M * lerp(in) == lerp(M * in), so the
 * window-space producer re-run over the clipped inputs lands each new vertex
 * exactly on the clipped edge.  Returns the new vertex count, 0 when nothing
 * survives (the draw is consumed), or -1 when a triangle is undividable or
 * the rebuilt list would exceed the producer's vertex ceiling (fall back). */
static int32_t
r2vb_clip_build_clipped_list(struct r300_context *r300,
                             struct pipe_resource *res, uint32_t count,
                             const float (*model)[4], unsigned n_extra,
                             float (*const *extras)[4],
                             float (**out_model)[4], float (**out_extras)[4])
{
    if (out_model)
        *out_model = NULL;
    if (out_extras)
        *out_extras = NULL;
    if (1 + n_extra > R300_R2VB_CLIP_MAX_ATTRS)
        return -1;
    struct pipe_transfer *xfer = NULL;
    struct pipe_box box = {
        .width = (count ? count : 1) * 16, .height = 1, .depth = 1 };
    /* Flush first: single-CS producer may leave the write still in the active
     * cmdbuf, so wait alone would observe an un-submitted BO. */
    r300->context.flush(&r300->context, NULL, 0);
    r2vb_wait_bo(r300, res);
    const float *got =
        r300->context.buffer_map(&r300->context, res, 0, PIPE_MAP_READ, &box, &xfer);
    if (!got)
        return -1;
    const bool half_z = r300->clip_halfz;
    const float k = R300_R2VB_CLIP_GUARD_K;
    const uint8_t planes = r2vb_active_clip_planes(r300);
    const unsigned num_attrs = 1 + n_extra;

    /* One triangle clipped against six planes fans into at most
     * R300_R2VB_CLIP_MAX_POLY - 2 triangles. */
    const uint32_t max_out = (count / 3) * (R300_R2VB_CLIP_MAX_POLY - 2) * 3;
    float (*na[R300_R2VB_CLIP_MAX_ATTRS])[4] = { NULL };
    bool oom = false;
    for (unsigned a = 0; a < num_attrs; a++)
        if (!(na[a] = malloc((size_t)max_out * sizeof(*na[a]))))
            oom = true;
    if (oom) {
        for (unsigned a = 0; a < num_attrs; a++)
            free(na[a]);
        r300->context.buffer_unmap(&r300->context, xfer);
        return -1;
    }

    uint32_t n_out = 0;
    bool bail = false;
    for (uint32_t t = 0; t * 3 + 2 < count && !bail; t++) {
        const uint32_t vi[3] = { t * 3, t * 3 + 1, t * 3 + 2 };
        uint8_t om = 0, am = 0;
        enum r300_r2vb_tri_class c = r300_r2vb_classify_triangle_planes(
            &got[vi[0] * 4], &got[vi[1] * 4], &got[vi[2] * 4], k, half_z,
            planes, &om, &am);
        switch (c) {
        case R300_R2VB_TRI_FALLBACK:
            bail = true;
            break;
        case R300_R2VB_TRI_REJECT:
            break;
        case R300_R2VB_TRI_ACCEPT:
            for (int i = 0; i < 3; i++) {
                memcpy(na[0][n_out + i], model[vi[i]], sizeof(na[0][0]));
                for (unsigned j = 0; j < n_extra; j++)
                    memcpy(na[1 + j][n_out + i], extras[j][vi[i]],
                           sizeof(na[0][0]));
            }
            n_out += 3;
            break;
        case R300_R2VB_TRI_PARTIAL: {
            struct r300_r2vb_clip_vertex in[3], poly[R300_R2VB_CLIP_MAX_POLY];
            for (int i = 0; i < 3; i++) {
                memcpy(in[i].clip, &got[vi[i] * 4], sizeof(in[i].clip));
                memcpy(in[i].attr[0], model[vi[i]], sizeof(in[i].attr[0]));
                for (unsigned j = 0; j < n_extra; j++)
                    memcpy(in[i].attr[1 + j], extras[j][vi[i]],
                           sizeof(in[i].attr[0]));
            }
            unsigned np =
                r300_r2vb_clip_triangle(in, num_attrs, om & planes, k, half_z,
                                        poly);
            static int edge_log = -1;
            if (edge_log < 0) {
                const char *e = getenv("R300_R2VB_CLIP_LOG");
                edge_log = (e && strcmp(e, "1") == 0) ? 1 : 0;
            }
            if (edge_log)
                fprintf(stderr, "r2vb_clip_edge prim=%u or=0x%02x poly=%u\n",
                        t, om, np);
            /* Empty polygon after multi-plane clip: drop the triangle only.
             * Whole-draw fallback would force gallivm when CLIP_EDGE should
             * keep the surviving accepted triangles. */
            if (np == 0)
                break;
            for (unsigned i = 1; i + 1 < np; i++) {
                const unsigned pv[3] = { 0, i, i + 1 };
                for (int q = 0; q < 3; q++)
                    for (unsigned a = 0; a < num_attrs; a++)
                        memcpy(na[a][n_out + q], poly[pv[q]].attr[a],
                               sizeof(na[0][0]));
                n_out += 3;
            }
            break;
        }
        }
    }
    r300->context.buffer_unmap(&r300->context, xfer);

    if (bail || n_out > 4096 || n_out == 0) {
        for (unsigned a = 0; a < num_attrs; a++)
            free(na[a]);
        if (!bail && n_out == 0) {
            *out_model = NULL;
            return 0;
        }
        return -1;
    }
    *out_model = na[0];
    for (unsigned j = 0; j < n_extra; j++)
        out_extras[j] = na[1 + j];
    return (int32_t)n_out;
}

/* Read one application vertex element as CPU vec4 floats (missing components
 * default to the (0,0,0,1) vertex identity), the same read the model loop
 * performs for the position input.  Only float32 source formats are read --
 * the clip-edge interpolation blends in the float domain, and delivering a
 * blended value in a narrower application format would quantize it twice.
 * indices[i] names the source vertex row for output slot i -- the shared
 * index-resolution table r2vb_topology_gather_indices builds for a
 * topology-gathered draw, or the identity draw->start+i sequence for a plain
 * one -- so this reader always walks the same vertices the position
 * model-gather used. */
/* Checked size_t add/mul: false when the result would wrap. */
static bool
r2vb_size_add(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static bool
r2vb_size_mul(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b)
        return false;
    *out = a * b;
    return true;
}

/* True when base + index * stride + format_size fits the bound resource.
 * User vertex buffers have no size in the pipe ABI; they cannot be extent-
 * checked here and remain the caller's contract.  Each term is accumulated
 * with wrap-checked arithmetic so a large index or offset fails closed. */
static bool
r2vb_velem_index_in_bounds(struct r300_context *r300, unsigned e, uint32_t index)
{
    if (e >= r300->velems->count || !r300->velems->velem[e].src_stride)
        return false;
    struct pipe_vertex_element *pe = &r300->velems->velem[e];
    if (pe->vertex_buffer_index >= r300->nr_vertex_buffers)
        return false;
    struct pipe_vertex_buffer *vb =
        &r300->vertex_buffer[pe->vertex_buffer_index];
    if (vb->is_user_buffer)
        return true;
    if (!vb->buffer.resource)
        return false;
    size_t row, base, need;
    if (!r2vb_size_mul((size_t)index, pe->src_stride, &row))
        return false;
    if (!r2vb_size_add((size_t)vb->buffer_offset, pe->src_offset, &base))
        return false;
    if (!r2vb_size_add(base, row, &need))
        return false;
    if (!r2vb_size_add(need, util_format_get_blocksize(pe->src_format), &need))
        return false;
    return need <= (size_t)vb->buffer.resource->width0;
}

static float (*
r2vb_read_velem_floats(struct r300_context *r300, unsigned e,
                       const uint32_t *indices, uint32_t count))[4]
{
    if (e >= r300->velems->count)
        return NULL;
    struct pipe_vertex_element *pe = &r300->velems->velem[e];
    switch (pe->src_format) {
    case PIPE_FORMAT_R32_FLOAT:
    case PIPE_FORMAT_R32G32_FLOAT:
    case PIPE_FORMAT_R32G32B32_FLOAT:
    case PIPE_FORMAT_R32G32B32A32_FLOAT:
        break;
    default:
        return NULL;
    }
    struct pipe_vertex_buffer *vb = &r300->vertex_buffer[pe->vertex_buffer_index];
    const uint8_t *base = NULL;
    if (vb->is_user_buffer)
        base = vb->buffer.user;
    else if (vb->buffer.resource)
        base = r300_resource(vb->buffer.resource)->malloced_buffer;
    if (!base || !pe->src_stride)
        return NULL;
    base += vb->buffer_offset + pe->src_offset;
    unsigned comps = util_format_get_nr_components(pe->src_format);
    float (*out)[4] = malloc((size_t)count * sizeof(*out));
    if (!out)
        return NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (!r2vb_velem_index_in_bounds(r300, e, indices[i])) {
            free(out);
            return NULL;
        }
        const float *v = (const float *)(base + (size_t)indices[i] * pe->src_stride);
        out[i][0] = v[0];
        out[i][1] = comps > 1 ? v[1] : 0.0f;
        out[i][2] = comps > 2 ? v[2] : 0.0f;
        out[i][3] = comps > 3 ? v[3] : 1.0f;
    }
    return out;
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

/* Known-answer self-test for perspective divide and viewport mapping. The window
 * result is fixed by the arithmetic, rather than by agreement with the shader,
 * so a shared sign or scale bug cannot pass. clip = (4, -2, 1, 2) under a unit
 * viewport divides to (2, -1, 0.5) and carries w = 1. A sub-threshold w_clip
 * collapses the reciprocal to 0 through the FP24 infinity guard. The test mirrors
 * the divide emitted by r300_r2vb_build_baked_transform_fs. */
static bool r2vb_divide_oracle_selftest(void)
{
    const float clip[4] = { 4.0f, -2.0f, 1.0f, 2.0f };
    float rcp = (fabsf(clip[3]) < 1.0f / 32768.0f) ? 0.0f : 1.0f / clip[3];
    float win[3];
    for (int i = 0; i < 3; i++)
        win[i] = clip[i] * rcp; /* unit viewport: scale = 1, bias = 0 */
    const float wtiny = 1e-6f;
    float rcp_tiny = (fabsf(wtiny) < 1.0f / 32768.0f) ? 0.0f : 1.0f / wtiny;
    bool ok = fabsf(win[0] - 2.0f) < 1e-5f && fabsf(win[1] + 1.0f) < 1e-5f &&
              fabsf(win[2] - 0.5f) < 1e-5f && rcp_tiny == 0.0f;
    fprintf(stderr,
            "r2vb_divide_oracle_selftest=%s (4,-2,1,2)/w->win(%.3f,%.3f,%.3f) tiny-w-guard=%s\n",
            ok ? "PASS" : "FAIL", win[0], win[1], win[2], rcp_tiny == 0.0f ? "0" : "BAD");
    return ok;
}

/* Verify the producer BO against the CPU divide + viewport reference: clip =
 * M(cols) * model_vert, then window.xyz = (clip.xyz / w_clip) * viewport_scale +
 * viewport_bias with w = 1, reproducing the FS's own guard and w output rather than
 * a naive draw-style w = 1/w_clip.  Reads r300->viewport for the same scale/bias
 * the FS bakes.  FP24 fragment ALU plus a divide and three MADs, so the same loose
 * 0.05 tolerance as the bare transform. */
static void r2vb_verify_window_readback(struct r300_context *r300, struct pipe_resource *res,
                                        const float (*model)[4], uint32_t count, const float *cols)
{
    const struct pipe_viewport_state *vp = &r300->viewport;
    float (*exp)[4] = malloc((size_t)count * sizeof(*exp));
    if (!exp)
        return;
    for (uint32_t s = 0; s < count; s++) {
        float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                clip[i] += cols[j * 4 + i] * model[s][j];
        float rcp = (fabsf(clip[3]) < 1.0f / 32768.0f) ? 0.0f : 1.0f / clip[3];
        for (int i = 0; i < 3; i++)
            exp[s][i] = clip[i] * rcp * vp->scale[i] + vp->translate[i];
        exp[s][3] = 1.0f;
    }
    r2vb_verify_bo_readback(r300, res, exp, count, 0.05f, "divide");
    free(exp);
}

/* Rotate a 3-vector by a UNIT quaternion q = (x, y, z, w), the Cayley-Dickson
 * product v' = q v q* expanded to its rotation-matrix form.  The doubling product
 * (a,b)(c,d) = (ac - d*b, da + bc*) with gamma = -1 gives the Hamilton quaternion
 * algebra; carrying the sandwich q (0,v) q* through and collecting terms yields the
 * standard orthogonal matrix below, which is pure multiply-add -- no normalize, no
 * reciprocal-sqrt -- so it maps cleanly onto the FP24 fragment ALU.  q must already
 * be unit; a non-unit q gives a scaled (non-orthogonal) map, so the caller feeds a
 * pre-normalized quaternion and the shader matches term for term. */
static void r2vb_quat_rotate_unit(const float q[4], const float v[3], float out[3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = (1.0f - 2.0f * (y * y + z * z)) * v[0] + 2.0f * (x * y - w * z) * v[1] +
             2.0f * (x * z + w * y) * v[2];
    out[1] = 2.0f * (x * y + w * z) * v[0] + (1.0f - 2.0f * (x * x + z * z)) * v[1] +
             2.0f * (y * z - w * x) * v[2];
    out[2] = 2.0f * (x * z - w * y) * v[0] + 2.0f * (y * z + w * x) * v[1] +
             (1.0f - 2.0f * (x * x + y * y)) * v[2];
}

/* Known-answer self-test: anchor the oracle on rotations whose result is fixed by
 * the convention, NOT by agreeing with the shader, so a shared sign/transpose bug
 * cannot pass green.  Identity (0,0,0,1) must leave (1,0,0) put; a +90 deg rotation
 * about +z, q = (0, 0, sin45, cos45), must send (1,0,0) -> (0,1,0) (a transpose or
 * conjugate-sign error sends it to (0,-1,0)).  Returns true iff both hold. */
static bool r2vb_quat_oracle_selftest(void)
{
    const float s = 0.70710678f; /* sin(45 deg) = cos(45 deg) */
    const float idq[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float zq[4] = { 0.0f, 0.0f, s, s };
    const float ex[3] = { 1.0f, 0.0f, 0.0f };
    float o1[3], o2[3];
    r2vb_quat_rotate_unit(idq, ex, o1); /* expect (1,0,0) */
    r2vb_quat_rotate_unit(zq, ex, o2);  /* expect (0,1,0) */
    bool ok = fabsf(o1[0] - 1.0f) < 1e-5f && fabsf(o1[1]) < 1e-5f && fabsf(o1[2]) < 1e-5f &&
              fabsf(o2[0]) < 1e-5f && fabsf(o2[1] - 1.0f) < 1e-5f && fabsf(o2[2]) < 1e-5f;
    fprintf(stderr,
            "r2vb_quat_oracle_selftest=%s identity->(%.3f,%.3f,%.3f) z90*(1,0,0)->(%.3f,%.3f,%.3f)\n",
            ok ? "PASS" : "FAIL", o1[0], o1[1], o1[2], o2[0], o2[1], o2[2]);
    return ok;
}

/* Cayley-Dickson product of two n-dim elements (n a power of two), scalar-first:
 * (a,b)(c,d) = (a c - conj(d) b, d a + b conj(c)); base n==1 is real mult.  Used
 * at n=16 for the sedenion (CD-4) product oracle.  conj negates all but [0]. */
static void r2vb_cd_mul(const float *a, const float *b, float *o, int n)
{
    /* Temporaries are fixed at 16 components (sedenion).  n must be a power
     * of two in [1, 16] or the half-size recursion overruns the stack arrays. */
    assert(n >= 1 && n <= 16 && (n & (n - 1)) == 0);
    if (n == 1) { o[0] = a[0] * b[0]; return; }
    int h = n / 2;
    const float *A = a, *B = a + h, *C = b, *D = b + h;
    /* cj[0..h-1] is written before each use.  Zero-initialize the full array so
     * the unread tail cj[h..15] stays a defined zero for static analysis. */
    float ac[16], db[16], da[16], bc[16], cj[16] = {0};
    r2vb_cd_mul(A, C, ac, h);
    for (int i = 0; i < h; i++) cj[i] = (i == 0) ? D[i] : -D[i]; /* conj(D) */
    r2vb_cd_mul(cj, B, db, h);
    r2vb_cd_mul(D, A, da, h);
    for (int i = 0; i < h; i++) cj[i] = (i == 0) ? C[i] : -C[i]; /* conj(C) */
    r2vb_cd_mul(B, cj, bc, h);
    for (int i = 0; i < h; i++) { o[i] = ac[i] - db[i]; o[i + h] = da[i] + bc[i]; }
}

/* Sedenion (CD-4) product a*b, the first non-division-algebra level: it has zero
 * divisors (nonzero a, b with a*b = 0).  The CPU reference for the genuine
 * frontier product the fragment ALU computes from two 16-component inputs. */
static void r2vb_sedenion_mul(const float a[16], const float b[16], float out[16])
{
    r2vb_cd_mul(a, b, out, 16);
}

/* Known-answer self-test anchored on the algebra: every imaginary unit squares to
 * -1, and the explicit zero divisor (e1 + e10)(e5 + e14) = 0 -- two nonzero
 * sedenions whose product is exactly zero, impossible in CD <= 3 (composition is
 * |a b| = |a| |b|, so a, b != 0 would force a*b != 0 there). */
static bool r2vb_sed_oracle_selftest(void)
{
    bool ok = true;
    for (int k = 1; k < 16 && ok; k++) {
        float e[16] = { 0 }, sq[16];
        e[k] = 1.0f;
        r2vb_sedenion_mul(e, e, sq);
        ok = ok && fabsf(sq[0] + 1.0f) < 1e-5f;
        for (int i = 1; i < 16; i++)
            ok = ok && fabsf(sq[i]) < 1e-5f;
    }
    float a[16] = { 0 }, b[16] = { 0 }, p[16];
    a[1] = 1.0f; a[10] = 1.0f;
    b[5] = 1.0f; b[14] = 1.0f;
    r2vb_sedenion_mul(a, b, p);
    float pn = 0.0f;
    for (int i = 0; i < 16; i++) pn += p[i] * p[i];
    ok = ok && pn < 1e-8f; /* zero divisor: |a*b| = 0 with |a|,|b| != 0 */
    fprintf(stderr, "r2vb_sed_oracle_selftest=%s zero_divisor|ab|^2=%.3e\n",
            ok ? "PASS" : "FAIL", pn);
    return ok;
}

/* Producer half (stage 1 + the cb_flush_clean barrier): render one synthesized
 * vertex per output slot into output_gart_bo through the bound fragment program,
 * then make the writes visible to the VAP.  Factored out of the combined loop so
 * the route-exec MVP path can run it under the normal draw flow (where
 * prepare_for_rendering has emitted the transform-FS state) and then re-ingest
 * with a different (application) FS, rather than the single-FS combined loop. */
/* Calibration decode of the immediate producer's draw-adjacent logical
 * state, gated on R300_R2VB_IMMD_STATE=1.  The IMMD draw interprets its
 * embedded vertices through the INHERITED programmable-stream and RS
 * atoms, so the authoritative record is those bound atoms plus the
 * registers the emit path writes itself.  One key=value line per
 * register keeps the record machine-reducible into the
 * producer-immediate-logical-state calibration artifact. */
static void
r300_r2vb_dump_immd_state(struct r300_context *r300, uint32_t num_vertices,
                          unsigned num_attrs, uint32_t vtx_dwords,
                          uint32_t output_pitch, bool transform_mode)
{
    const char *gate = getenv("R300_R2VB_IMMD_STATE");
    if (!gate || strcmp(gate, "1") != 0)
        return;
    struct r300_vertex_stream_state *vs =
        (struct r300_vertex_stream_state *)r300->vertex_stream_state.state;
    struct r300_rs_block *rs =
        (struct r300_rs_block *)r300->rs_block_state.state;
    struct r300_viewport_state *vp =
        (struct r300_viewport_state *)r300->viewport_state.state;
    fprintf(stderr,
            "r2vb_immd_state begin num_vertices=%u num_attrs=%u "
            "transform_mode=%u vap_vtx_size=%u vf_max=%u output_pitch=%u\n",
            num_vertices, num_attrs, transform_mode ? 1 : 0, vtx_dwords,
            num_vertices - 1, output_pitch);
    if (vs) {
        fprintf(stderr, "r2vb_immd_state psc_count=%u\n", vs->count);
        for (unsigned i = 0; i < 8; i++)
            fprintf(stderr,
                    "r2vb_immd_state prog_stream_cntl_%u=0x%08x "
                    "prog_stream_cntl_ext_%u=0x%08x\n",
                    i, vs->vap_prog_stream_cntl[i], i,
                    vs->vap_prog_stream_cntl_ext[i]);
    }
    if (rs) {
        fprintf(stderr,
                "r2vb_immd_state vtx_state_cntl=0x%08x vsm_vtx_assm=0x%08x "
                "out_vtx_fmt0=0x%08x out_vtx_fmt1=0x%08x gb_enable=0x%08x "
                "rs_count=0x%08x rs_inst_count=0x%08x\n",
                rs->vap_vtx_state_cntl, rs->vap_vsm_vtx_assm,
                rs->vap_out_vtx_fmt[0], rs->vap_out_vtx_fmt[1], rs->gb_enable,
                rs->count, rs->inst_count);
        for (unsigned i = 0; i < 8; i++)
            fprintf(stderr,
                    "r2vb_immd_state rs_ip_%u=0x%08x rs_inst_%u=0x%08x\n", i,
                    rs->ip[i], i, rs->inst[i]);
    }
    if (vp)
        fprintf(stderr,
                "r2vb_immd_state vport_xscale=%a vport_xoffset=%a "
                "vport_yscale=%a vport_yoffset=%a vport_zscale=%a "
                "vport_zoffset=%a inherited_vte_control=0x%08x\n",
                vp->xscale, vp->xoffset, vp->yscale, vp->yoffset, vp->zscale,
                vp->zoffset, vp->vte_control);
    /* Registers this emit path writes directly, restated as data so the
     * artifact stands without the source open. */
    fprintf(stderr,
            "r2vb_immd_state emitted_vte_cntl=0x%08x emitted_clip_cntl=0x%08x "
            "emitted_us_out_fmt=%s scissor_tl=1440,1440 "
            "scissor_br=%u,%u end\n",
            (uint32_t)(R300_VTX_XY_FMT | R300_VTX_Z_FMT),
            (uint32_t)R300_CLIP_DISABLE,
            transform_mode ? "c4_32_fp_rgba" : "c4_32_fp_bgra",
            num_vertices + 1440 - 1, 1 + 1440 - 1);
}

/* Producer output-target prologue, shared by the embedded-immediate draw and
 * the BO-fetch transaction: raw-retarget the color target to the producer BO
 * behind the cb_flush_clean barrier, then pin the point-raster and VAP mode
 * registers the producer draw depends on.  Everything from the destination
 * cache flush through VAP_VTE_CNTL lives here, so the two producer forms
 * share one output-ordering contract instead of drifting apart. */
static void r2vb_emit_producer_target_prologue(struct r300_context *r300,
                                               struct r300_resource *output_gart_bo,
                                               uint32_t output_gart_bo_offset,
                                               uint32_t num_vertices,
                                               bool transform_mode)
{
    CS_LOCALS(r300);
    uint32_t output_pitch = align(num_vertices, 2);

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

    /* ZB_CNTL through VAP_VTE_CNTL: 31 fixed dwords plus the per-matching
     * viewport-constant wpos override. */
    BEGIN_CS(31 + (int)r2vb_vp_override_dwords);

    OUT_CS_REG(R300_ZB_CNTL, 0);
    OUT_CS_REG_SEQ(R300_SC_SCISSORS_TL, 2);
    OUT_CS((1440 << R300_SCISSORS_X_SHIFT) | (1440 << R300_SCISSORS_Y_SHIFT));
    OUT_CS(((num_vertices + 1440 - 1) << R300_SCISSORS_X_SHIFT) |
           ((1 + 1440 - 1) << R300_SCISSORS_Y_SHIFT));
    /* Flush the OUTGOING color buffer before retargeting RB3D_COLOROFFSET0.
     * Every framebuffer change in the driver pairs with the gpu_flush atom's
     * cb_flush_clean (r300_mark_fb_state_dirty marks gpu_flush dirty), which
     * pushes dirty RB3D destination-cache tiles to memory while the old
     * COLOROFFSET is still programmed.  A raw retarget without that barrier
     * drops the application surface's cached pixels -- observed as a prior
     * blitter clear-quad losing every pixel the later draw does not rewrite. */
    OUT_CS_REG(R300_ZB_ZCACHE_CTLSTAT,
               R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
    OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT,
               R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
    OUT_CS_REG(RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
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
    /* GA point size is in sixths of a pixel (the blitter encodes dimension*6 into
     * R300_GA_POINT_SIZE, see r300_render.c), so 6 == 1 px.  R300_R2VB_POINT_SIZE
     * overrides the rasterized size only for single-vertex producer probes: a
     * multi-vertex producer packs attributes as a point stream, and a wide
     * GA_POINT_SIZE would rasterize each vertex as a large splat instead of
     * a 1-px sample.  Unset (default 1) emits 6/6. */
    {
        static int r2vb_point_px = -1;
        if (r2vb_point_px < 0) {
            const char *e = getenv("R300_R2VB_POINT_SIZE");
            long px = e ? strtol(e, NULL, 0) : 1;
            /* ps6 = px*6 is packed into a 16-bit GA_POINT_SIZE field per axis. */
            r2vb_point_px = (px > 0 && px <= 65535 / 6) ? (int)px : 1;
        }
        int px = (num_vertices == 1) ? r2vb_point_px : 1;
        uint32_t ps6 = (uint32_t)px * 6;
        OUT_CS_REG(R300_GA_POINT_SIZE, (ps6 << R300_POINTSIZE_Y_SHIFT) |
                                           (ps6 << R300_POINTSIZE_X_SHIFT));
        OUT_CS_REG(R300_GA_POINT_MINMAX, (6 << R300_GA_POINT_MINMAX_MIN_SHIFT) |
                                             (ps6 << R300_GA_POINT_MINMAX_MAX_SHIFT));
    }
    OUT_CS_REG(R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
    OUT_CS_REG(R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
    END_CS;
}

/* Producer cache-publication tail, shared by both producer forms: push the
 * color write out of the ZB/RB3D caches, wait 3D idle-clean, and sync the
 * VAP so a later vertex fetch of the same BO cannot read stale vertex-cache
 * content (R300_R2VB_BARRIER neuters parts for timing bisection). */
static void r2vb_emit_producer_order_tail(struct r300_context *r300)
{
    CS_LOCALS(r300);
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
    BEGIN_CS(8);
    OUT_CS_REG(R300_ZB_ZCACHE_CTLSTAT, r2vb_zb);
    OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT, r2vb_rb);
    OUT_CS_REG(RADEON_WAIT_UNTIL, r2vb_wait);
    /* Sync the VAP/vertex-fetch engine.  The cache flushes above push the
     * producer's color write out of the RB3D/Z caches to memory, but they do
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

static void r300_r2vb_emit_producer(struct r300_context *r300,
                                    struct r300_resource *output_gart_bo,
                                    uint32_t output_gart_bo_offset, uint32_t num_vertices,
                                    const float (*vertex_attrs)[4], unsigned num_attrs,
                                    bool transform_mode)
{
    CS_LOCALS(r300);
    uint32_t output_pitch = align(num_vertices, 2);

    /* Embedded vertex = slot position + num_attrs model attributes, each FP32x4, so
     * VAP_VTX_SIZE and the DRAW_IMMD body are 4*(1+num_attrs) dwords per vertex.
     * The passthrough producer (transform_mode == false) copies one attribute, so
     * it always feeds a single attribute. */
    if (num_attrs < 1)
        num_attrs = 1;
    if (num_attrs > R300_R2VB_MAX_PRODUCER_INPUTS)
        num_attrs = R300_R2VB_MAX_PRODUCER_INPUTS;
    if (!transform_mode)
        num_attrs = 1;
    uint32_t vtx_dwords = 4 * (1 + num_attrs);

    r300_r2vb_dump_immd_state(r300, num_vertices, num_attrs, vtx_dwords,
                              output_pitch, transform_mode);

    r300->rws->cs_add_buffer(&r300->cs, output_gart_bo->buf,
                             RADEON_USAGE_READWRITE | RADEON_USAGE_SYNCHRONIZED |
                                 RADEON_PRIO_COLOR_BUFFER,
                             RADEON_DOMAIN_GTT);

    r2vb_emit_producer_target_prologue(r300, output_gart_bo,
                                       output_gart_bo_offset, num_vertices,
                                       transform_mode);

    /* VTX_SIZE + VF_MAX + the DRAW_IMMD header pair, then the embedded body. */
    BEGIN_CS(6 + (int)num_vertices * (int)vtx_dwords);
    OUT_CS_REG(R300_VAP_VTX_SIZE, vtx_dwords);
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_IMMD_2, num_vertices * vtx_dwords);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | (num_vertices << 16) |
           R300_VAP_VF_CNTL__PRIM_POINTS);
    for (uint32_t pv = 0; pv < num_vertices; pv++) {
        OUT_CS_32F((float)pv + 0.5f);
        OUT_CS_32F(0.5f);
        OUT_CS_32F(0.0f);
        OUT_CS_32F(1.0f);
        if (transform_mode) {
            /* One straight FP32x4 per model attribute, in input order: the producer
             * VS routes embedded attribute a to VAR0+a, the re-staged FS's input a.
             * vertex_attrs is laid out num_attrs per vertex (vertex_attrs[pv*num_attrs+a]),
             * which collapses to vertex_attrs[pv] for the single-attribute callers. */
            for (uint32_t a = 0; a < num_attrs; a++) {
                const float *att = vertex_attrs[pv * num_attrs + a];
                OUT_CS_32F(att[0]);
                OUT_CS_32F(att[1]);
                OUT_CS_32F(att[2]);
                OUT_CS_32F(att[3]);
            }
        } else {
            OUT_CS_32F(vertex_attrs[pv][2]);
            OUT_CS_32F(vertex_attrs[pv][1]);
            OUT_CS_32F(vertex_attrs[pv][0]);
            OUT_CS_32F(vertex_attrs[pv][3]);
        }
    }
    END_CS;

    r2vb_emit_producer_order_tail(r300);
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
                            vertex_attrs, 1, transform_mode);

    /* C0 baseline cell gate (R300_PTSIZE_C0=1): write VAP_OUTPUT_VTX_FMT_0/1
     * (0x2090/0x2094) explicitly on the re-ingest so PT_SIZE_PRESENT (bit 16 of
     * 0x2090) does not inherit from the upstream real SWTCL draw.  The only other
     * 0x2090 write site in the driver is r300_emit_vap_output_state, so without
     * this cell the re-ingest runs with whatever PT_SIZE_PRESENT the trigger draw
     * last left set.  When it is left set the GA expects a per-vertex psize output
     * vector that the single-FLOAT_4 re-ingest never produces; the output-vector
     * layout the GA reads stops matching what the producer wrote, position moves,
     * and a 16-point readback bbox spills to the origin (~110 texels, not 16
     * single texels).  C0 holds position-only output with PT_SIZE_PRESENT and the
     * color/texcoord components cleared, so the GA falls back to GA_POINT_SIZE for
     * the rasterized size.  C0 owns the VAP_OUTPUT_VTX_FMT subset that C1c and C1d
     * deliberately leave alone, keeping the cells non-overlapping.  Gate-off keeps
     * the path byte-identical. */
    static int ptsize_c0 = -1;
    if (ptsize_c0 < 0) {
        const char *e = getenv("R300_PTSIZE_C0");
        ptsize_c0 = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }

    /* C1a cell gate (R300_PTSIZE_C1A=1): write identity values to
     * SE_VPORT_X/Y/ZSCALE/OFFSET (0x1d98..0x1dac) on the re-ingest so the
     * VAP viewport state does not inherit non-identity values from previous
     * IBs.  The Stage 0.5 capture-class decode established that NEITHER the
     * SE_VPORT_*SCALE/OFFSET (0x1d98..0x1dac) NOR the VAP_VPORT_*SCALE/OFFSET
     * (0x2098..0x20ac) registers are ever written in the canonical Vulkan
     * trigger's selftest IB -- their value at DRAW_VBUF_2 is whatever the
     * GPU register file held before the IB started.  The producer's
     * VAP_VTE_CNTL = R300_VTX_XY_FMT | R300_VTX_Z_FMT clears VPORT_*_ENA at
     * the BITS level.  Inherited non-identity viewport state was the leading
     * register hypothesis for the bbox-to-origin POINTS smear, but silicon
     * measurement falsified it: cross-process pollution of these registers
     * does not reproduce the smear, and the carrier is a cold-cycle-clearable
     * transient GPU state, not a register a normal render can write.  This
     * write is therefore hygiene -- it makes the re-ingest's viewport state
     * explicit rather than inherited -- not a proven smear fix.
     * The direct-VB R2VB re-ingest path writes the same identity viewport
     * state before its VTE setup.
     * Gate-off keeps the path byte-identical. */
    static int ptsize_c1a = -1;
    if (ptsize_c1a < 0) {
        const char *e = getenv("R300_PTSIZE_C1A");
        ptsize_c1a = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }

    /* R300_R2VB_VTE_W0_FMT=1 writes VAP_VTE_CNTL explicitly on the
     * re-ingest with R300_VTX_W0_FMT (bit 10) set, in addition to the
     * R300_VTX_XY_FMT and R300_VTX_Z_FMT bits the producer writes. The
     * producer value (0x300) tells the VAP that X, Y, and Z
     * arrive in window-coordinate space and bypass the viewport transform; W
     * still flows through perspective divide.  The re-ingest reads vertices
     * the producer wrote in window space with W = 1.0, so the divide-by-W path
     * should be a no-op when applied -- but the inherited W viewport bias from
     * upstream draws is then applied AFTER the divide on a near-1 W, producing
     * a Y-channel collapse that matches the asymmetric crushing signature
     * (X centroid 28.3 ~ predicted 32; Y centroid 6.2 << predicted 32).
     * Setting R300_VTX_W0_FMT (giving VAP_VTE_CNTL = 0x700) tells the VAP that
     * W is also in window space and bypasses the divide entirely. The
     * direct-VB R2VB re-ingest path uses the same VTE W0 format. Gate-off
     * keeps the path byte-identical. */
    static int r2vb_vte_w0_fmt = -1;
    if (r2vb_vte_w0_fmt < 0) {
        /* R300_PTSIZE_C1B is the retired name for the same gate. */
        const char *e = getenv("R300_R2VB_VTE_W0_FMT");
        if (!e)
            e = getenv("R300_PTSIZE_C1B");
        r2vb_vte_w0_fmt = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }

    /* C1c cell gate (R300_PTSIZE_C1C=1): re-emit the VAP_VTX_STATE_CNTL +
     * VAP_VSM_VTX_ASSM atom (0x2180..0x2184) with values that match the
     * re-ingest's single-FLOAT_4 position-only input stream, instead of
     * inheriting from the application's prior VS state-emit at
     * r300_emit.c:903-905.  Stage 0.5 observed VTX_STATE_CNTL = 0x00005555
     * and VSM_VTX_ASSM = 0x00000401 (POS | TC0) inherited from the
     * application's "position + tex0" layout; the re-ingest has no tex0,
     * so the inherited TC0 bit causes the GA to expect a texcoord output
     * vector that the FLOAT_4-only PSC does not produce.  C1c writes:
     *   VTX_STATE_CNTL = 0 (default)
     *   VSM_VTX_ASSM   = R300_INPUT_CNTL_POS (POS only)
     * Gate-off keeps the path byte-identical. */
    static int ptsize_c1c = -1;
    if (ptsize_c1c < 0) {
        const char *e = getenv("R300_PTSIZE_C1C");
        ptsize_c1c = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }

    /* C2/C3/C4 independently gate register classes the trigger IB leaves
     * unwritten. Each writes a known-good identity or zero value.
     *   C2: SU_POLY_OFFSET_FRONT/BACK_SCALE/OFFSET (0x42A4..0x42B0) = 0
     *       (clear inherited polygon-offset that shifts Z and indirectly XY)
     *   C3: VAP_PROG_STREAM_CNTL_1..7 (0x2154..0x216C) = 0
     *       (clear inherited multi-stream config that confuses the VAP)
     *   C4: GA_FOG_SCALE (0x4294), GA_FOG_OFFSET (0x4298),
     *       GA_TRIANGLE_STIPPLE (0x4214) = 0
     *       (clear inherited fog state and stipple bias) */
    static int ptsize_c2 = -1;
    if (ptsize_c2 < 0) {
        const char *e = getenv("R300_PTSIZE_C2");
        ptsize_c2 = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    static int ptsize_c3 = -1;
    if (ptsize_c3 < 0) {
        const char *e = getenv("R300_PTSIZE_C3");
        ptsize_c3 = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    static int ptsize_c4 = -1;
    if (ptsize_c4 < 0) {
        const char *e = getenv("R300_PTSIZE_C4");
        ptsize_c4 = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }

    /* C1d cell gate (R300_PTSIZE_C1D=1): re-assert the rasterizer interpolator
     * block (GB_ENABLE + RS_IP/RS_COUNT/RS_INST) on the re-ingest so the
     * fragment-input routing does not inherit a stale routing from the trigger
     * draw.  Stage 0.5 observed the RS_IP/RS_INST/RS_COUNT atoms in the trigger
     * region of the IB -- the re-ingest changes the VAP output layout (single
     * FLOAT_4 position) but does not re-emit the RS routing, so the GA samples
     * the trigger FS's expected inputs from VAP outputs the re-ingest does not
     * produce.  This is a COLOR-routing concern, orthogonal to the bbox-to-
     * origin position smear (a cold-cycle-clearable transient-GPU-state
     * Heisenbug, carrier not a writable register -- silicon-measured); the readback
     * oracle counts coverage, so a wrong-routed color still counts.  The
     * re-emit is the genuinely-rs-specific subset of r300_emit_rs_block_state;
     * the VAP_OUTPUT_VTX_FMT subset is owned by C0 and the VAP_VTX_STATE_CNTL/
     * VSM_VTX_ASSM subset by C1c.  Gate-off keeps the path byte-identical. */
    static int ptsize_c1d = -1;
    if (ptsize_c1d < 0) {
        const char *e = getenv("R300_PTSIZE_C1D");
        ptsize_c1d = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    struct r300_rs_block *c1d_rs =
        ptsize_c1d ? (struct r300_rs_block *)r300->rs_block_state.state : NULL;
    /* RS_IP and RS_INST tables share the same length (inst_count + 1). */
    unsigned c1d_rs_count = c1d_rs ? (c1d_rs->inst_count & R300_RS_INST_COUNT_MASK) + 1 : 0;

    /* Stage 3 -- re-ingest output_gart_bo as the vertex array and draw it.  The
     * optional observe redirect (stage3_color_bo) adds nine dwords.  Each C1/C
     * cell adds an independently-gated count: C0 3 (SEQ-of-2 + header), C1a 7,
     * C1b 2, C1c 3, C2 5 (SEQ-of-4 + header), C3 8 (SEQ-of-7 + header), C4 6 (3
     * single writes, non-contiguous), C1d 7 + 2*rs_count (the rasterizer block,
     * variable). */
    BEGIN_CS((stage3_color_bo ? 26 : 17)
             + (ptsize_c0  ? 3 : 0)
             + (ptsize_c1a ? 7 : 0)
             + (r2vb_vte_w0_fmt ? 2 : 0)
             + (ptsize_c1c ? 3 : 0)
             + (ptsize_c2  ? 5 : 0)
             + (ptsize_c3  ? 8 : 0)
             + (ptsize_c4  ? 6 : 0)
             /* C1d rasterizer block: GB_ENABLE(2) + RS_IP SEQ(1 header + count) +
              * RS_COUNT(3) + RS_INST SEQ(1 header + count) = 7 + 2 * count. */
             + (c1d_rs ? 7 + 2 * c1d_rs_count : 0));

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

    if (ptsize_c0) {
        /* Explicit position-only output vector format.  POS_PRESENT (bit 0) set;
         * PT_SIZE_PRESENT (bit 16) and every COLOR_*_PRESENT bit cleared in 0x2090;
         * VAP_OUTPUT_VTX_FMT_1 (0x2094) cleared so no texcoord components either.
         * Same SEQ-of-2 form r300_emit_vap_output_state uses. */
        OUT_CS_REG_SEQ(R300_VAP_OUTPUT_VTX_FMT_0, 2);
        OUT_CS(R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
        OUT_CS(0);
    }

    if (ptsize_c1a) {
        /* The direct-VB R2VB re-ingest path programs an identity viewport:
         * scale = 1.0 and offset = 0.0 on X, Y, and Z through
         * SE_VPORT_XSCALE..SE_VPORT_ZOFFSET (0x1d98..0x1dac). */
        OUT_CS_REG_SEQ(R300_SE_VPORT_XSCALE, 6);
        OUT_CS_32F(1.0f);
        OUT_CS_32F(0.0f);
        OUT_CS_32F(1.0f);
        OUT_CS_32F(0.0f);
        OUT_CS_32F(1.0f);
        OUT_CS_32F(0.0f);
    }

    if (r2vb_vte_w0_fmt) {
        /* Re-write VAP_VTE_CNTL with W0_FMT in addition to XY_FMT and Z_FMT
         * so the VAP treats W as already in window space and does NOT apply a
         * perspective divide on the re-ingest path. */
        OUT_CS_REG(R300_VAP_VTE_CNTL,
                   R300_VTX_XY_FMT | R300_VTX_Z_FMT | R300_VTX_W0_FMT);
    }

    if (ptsize_c1c) {
        /* Clean VAP_VTX_STATE_CNTL + VAP_VSM_VTX_ASSM for a position-only
         * single-stream re-ingest.  Same SEQ-of-2 form as r300_emit.c:903. */
        OUT_CS_REG_SEQ(R300_VAP_VTX_STATE_CNTL, 2);
        OUT_CS(0);
        OUT_CS(R300_INPUT_CNTL_POS);
    }

    if (ptsize_c2) {
        /* C2: SU_POLY_OFFSET_FRONT/BACK_SCALE/OFFSET cleared.  SEQ-of-4
         * starting at 0x42A4. */
        OUT_CS_REG_SEQ(R300_SU_POLY_OFFSET_FRONT_SCALE, 4);
        OUT_CS(0);
        OUT_CS(0);
        OUT_CS(0);
        OUT_CS(0);
    }

    if (ptsize_c3) {
        /* C3: VAP_PROG_STREAM_CNTL_1..7 cleared.  SEQ-of-7 starting at 0x2154. */
        OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_1, 7);
        OUT_CS(0);
        OUT_CS(0);
        OUT_CS(0);
        OUT_CS(0);
        OUT_CS(0);
        OUT_CS(0);
        OUT_CS(0);
    }

    if (ptsize_c4) {
        /* C4: GA_TRIANGLE_STIPPLE + GA_FOG_SCALE + GA_FOG_OFFSET cleared.
         * Three separate writes because the registers are not contiguous. */
        OUT_CS_REG(R300_GA_TRIANGLE_STIPPLE, 0);
        OUT_CS_REG(R300_GA_FOG_SCALE, 0);
        OUT_CS_REG(R300_GA_FOG_OFFSET, 0);
    }

    if (c1d_rs) {
        /* C1d: re-assert GB_ENABLE + RS_IP/RS_COUNT/RS_INST from the derived
         * rs_block_state, mirroring r300_emit_rs_block_state's rs-specific tail.
         * RS482/RS480 is not r500, so the R300_RS_* register set applies.  The
         * VAP_OUTPUT_VTX_FMT (C0) and VAP_VTX_STATE_CNTL/VSM_VTX_ASSM (C1c)
         * subsets of the atom are deliberately NOT re-emitted here to keep the
         * cells non-overlapping. */
        OUT_CS_REG_SEQ(R300_GB_ENABLE, 1);
        OUT_CS(c1d_rs->gb_enable);
        OUT_CS_REG_SEQ(R300_RS_IP_0, c1d_rs_count);
        OUT_CS_TABLE(c1d_rs->ip, c1d_rs_count);
        OUT_CS_REG_SEQ(R300_RS_COUNT, 2);
        OUT_CS(c1d_rs->count);
        OUT_CS(c1d_rs->inst_count);
        OUT_CS_REG_SEQ(R300_RS_INST_0, c1d_rs_count);
        OUT_CS_TABLE(c1d_rs->inst, c1d_rs_count);
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
     * It does NOT address the POINTS re-ingest bbox-to-origin smear.  That
     * smear is a Heisenbug: it appears only on a register-polluted GPU and
     * clears on a cold power cycle, and silicon measurement shows the carrier
     * is a cold-cycle-clearable transient GPU state, not a writable register
     * (cross-process register pollution and a power_profile clock sweep are
     * clean negatives, and the only remaining induction vector wedges the
     * northbridge).  Three register hypotheses had near-zero effect on it --
     * GA_POINT_SIZE, GA_POINT_MINMAX, and this VAP_VTX_SIZE -- consistent with
     * the carrier not being a register.  The stage-1 producer's own PRIM_POINTS
     * rasterize correctly, so the smear is specific to the re-ingest draw on a
     * polluted GPU; POINTS is off the mesh-draw critical path. */
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
 * hybrid-TCL experiment surface; an exact R300_R2VB_TIMING value picks the
 * transport mode.  Both variables are required:
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
        /* center + 4 ring corners.  A 5-vertex fan assembles n-2 = 3 triangles
         * (center,c1,c2), (center,c2,c3), (center,c3,c4) -- it does NOT close back
         * to c1, so it fills three of the four center-anchored quadrants and
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
    enum r300_r2vb_selftest_action action;
    bool do_submit;
    bool nowait;
    bool observe;
    bool xform;
    uint32_t num_vertices;
    uint32_t s3dim;
    const char *prim_name;
};

static void r2vb_get_selftest_config(struct r2vb_selftest_config *cfg,
                                     bool from_flush, bool already_fired,
                                     bool query_active)
{
    const char *hb_tcl = getenv("R300_HB_TCL");
    const char *mode = getenv("R300_R2VB_TIMING");
    const char *raw_submit_accepted = getenv("R300_RAW_SUBMIT_ACCEPTED");

    memset(cfg, 0, sizeof(*cfg));
    cfg->action = r300_r2vb_select_selftest_action(
        hb_tcl, mode, raw_submit_accepted, from_flush, already_fired,
        query_active);
    if (cfg->action == R300_R2VB_SELFTEST_DECLINE) {
        if (from_flush && !already_fired &&
            r300_r2vb_option_is(hb_tcl, "1") &&
            r300_r2vb_option_is(mode, "submit") &&
            !r300_r2vb_option_is(raw_submit_accepted, "1"))
            fprintf(stderr,
                    "r2vb selftest: submit mode needs "
                    "R300_RAW_SUBMIT_ACCEPTED=1\n");
        return;
    }

    cfg->do_submit = cfg->action == R300_R2VB_SELFTEST_SUBMIT;
    /* NOWAIT: submit and hand the fence back to the caller (r300_flush's out
     * param -> r3v's Vulkan fence) instead of waiting via the raw winsys
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
    r2vb_get_selftest_config(&cfg, from_flush, fired,
                             r300->query_current != NULL);

    if (cfg.action == R300_R2VB_SELFTEST_DECLINE)
        return false;

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
        void *xfs = r300_r2vb_get_transform_fs(r300, r2vb_env_space());
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
    if (cfg.do_submit)
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
        /* Submit and hand the fence to the caller's out param (becomes r3v's
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
    if (cfg.xform && cfg.do_submit) {
        if (r300_r2vb_divide_enabled()) {
            /* The transform fragment shader divides, so verify against the
             * window-space reference after the pure-CPU divide self-test. */
            if (r2vb_divide_oracle_selftest())
                r2vb_verify_window_readback(r300, res, attrs, cfg.num_vertices,
                                            r2vb_test_mvp_cols);
        } else {
            r2vb_verify_xform_readback(r300, res, attrs, cfg.num_vertices,
                                       r2vb_test_mvp_cols);
        }
    }

    /* Restore the application fragment shader and its const0 binding the
     * transform producer displaced. */
    if (cfg.xform && saved_fs) {
        r300->context.bind_fs_state(&r300->context, saved_fs);
        r300_r2vb_restore_app_fs_consts(r300);
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

/* No-submit NIR-shape probe for the vertex transform. It reports the bound VS
 * instruction profile so r300_vs_is_mvp matches the NIR state compiled by r300.
 * The matcher keys on that same nir_shader rather than a nir_to_tgsi round trip.
 * The probe reports an ALU and intrinsic histogram, the instruction count, and
 * the full nir_print_shader once when R300_R2VB_VS_DUMP is set. */
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
static bool r300_r2vb_nir_is_mvp_folded(nir_shader *nir)
{
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
     * case a later nir pass vectorizes differently, pure data movement, and
     * integer address math that NIR emits when indexing load_ubo_vec4 for the
     * matrix rows (iadd/imul/ushr chains).  Affine edge rebuild needs the
     * position transform to stay M*lerp(in)==lerp(M*in); UBO index arithmetic
     * does not break that.  Float ops outside the transform set (trig, div,
     * comparisons, ...) stay outside an MVP. */
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
                case nir_op_ffma:
                case nir_op_ffma_weak: n_ffma++; break;
                case nir_op_fmul: n_fmul++; break;
                case nir_op_fadd: n_fadd++; break;
                case nir_op_mov:
                case nir_op_vec4:
                case nir_op_vec3:
                case nir_op_vec2:
                    break; /* pure data movement, allowed */
                case nir_op_iadd:
                case nir_op_isub:
                case nir_op_imul:
                case nir_op_ushr:
                case nir_op_ishr:
                case nir_op_ishl:
                case nir_op_iand:
                case nir_op_ior:
                case nir_op_ixor:
                case nir_op_inot:
                case nir_op_ineg: {
                    /* Address math for matrix UBO rows must be constant-folded
                     * SSA (load_const only). Per-vertex matrix selection would
                     * still be affine per vertex but breaks the edge rebuild
                     * M*lerp(in)==lerp(M*in) assumption across vertices. */
                    nir_alu_instr *alu = nir_instr_as_alu(instr);
                    unsigned n_src = nir_op_infos[alu->op].num_inputs;
                    for (unsigned s = 0; s < n_src; s++) {
                        if (!nir_src_is_const(alu->src[s].src))
                            return false;
                    }
                    break;
                }
                default:
                    return false; /* arithmetic outside an MVP transform */
                }
                break;
            case nir_instr_type_intrinsic: {
                nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                switch (intr->intrinsic) {
                case nir_intrinsic_load_deref: {
                    nir_variable *var = nir_intrinsic_get_var(intr, 0);
                    if (!var)
                        return false;
                    /* gl_nir_lower_buffers stores the selected UBO block in
                     * driver_location.  The executor mirrors only block 0. */
                    if ((var->data.mode & nir_var_mem_ubo) &&
                        var->data.driver_location != 0)
                        return false;
                    break;
                }
                case nir_intrinsic_store_deref:
                case nir_intrinsic_load_input:
                case nir_intrinsic_store_output:
                case nir_intrinsic_load_constant:
                    break;
                case nir_intrinsic_load_ubo:
                case nir_intrinsic_load_ubo_vec4:
                    /* The MVP executor reads only the VS UBO[0] shadow.  Both
                     * the block and byte/vec4 offset must be constant after
                     * folding, and the block must select that shadow. */
                    if (!nir_src_is_const(intr->src[0]) ||
                        nir_src_as_uint(intr->src[0]) != 0 ||
                        !nir_src_is_const(intr->src[1]))
                        return false;
                    break;
                case nir_intrinsic_load_push_constant:
                    if (!nir_src_is_const(intr->src[0]))
                        return false;
                    break;
                default:
                    return false; /* texturing, atomics, ... not an MVP */
                }
                break;
            }
            default:
                return false;
            }
        }
    }
    /* mat4 * vec4 == c0*x + c1*y + c2*z + c3*w: 4 fmul + 3 fadd in the observed
     * column-MAD form; accept the fdot4 or ffma/ffma_weak chain forms too.
     * n_ffma counts both ffma and ffma_weak (NIR may emit either for the
     * same column-MAD shape). */
    bool has_transform = (n_fmul >= 4 && n_fadd >= 3) || (n_dot4 >= 4) || (n_ffma >= 3);
    return has_transform;
}

bool r300_r2vb_nir_is_mvp(nir_shader *nir)
{
    if (!nir)
        return false;

    nir_shader *clone = nir_shader_clone(NULL, nir);
    if (!clone)
        return false;

    bool progress;
    do {
        progress = false;
        progress |= nir_opt_constant_folding(clone);
        progress |= nir_opt_dce(clone);
    } while (progress);

    bool is_mvp = r300_r2vb_nir_is_mvp_folded(clone);
    ralloc_free(clone);
    return is_mvp;
}

static bool r300_vs_is_mvp(struct r300_context *r300)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return false;

    return r300_r2vb_nir_is_mvp(vs->state.ir.nir);
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
    case nir_op_fmad: case nir_op_ffma: case nir_op_ffma_weak:
    case nir_op_fdot2: case nir_op_fdot3: case nir_op_fdot4:
    case nir_op_fdot2_replicated: case nir_op_fdot3_replicated:
    case nir_op_fdot4_replicated:
    case nir_op_fmin: case nir_op_fmax:
    case nir_op_ffract: case nir_op_ffloor: case nir_op_fround_even:
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
static bool r300_vs_nir_is_fragment_aluable(nir_shader *nir,
                                            bool allow_computed_varying)
{
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

    /* Structural admissibility only: op set, intrinsic set, output shape.  The
     * ALU budget is NOT judged here -- scalar-NIR instruction counts over-reject
     * dense kernels the vectorizing backend packs far smaller (the CD-4 sedenion
     * product quarter has scalar NIR > 64 yet compiles to 41 r300 ALU slots).
     * r300_r2vb_producer_fits_budget measures the derived producer FS against
     * the real emit ceiling instead. */
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            switch (instr->type) {
            case nir_instr_type_load_const:
            case nir_instr_type_deref:
                break;
            case nir_instr_type_alu:
                if (!r300_nir_op_is_fragment_aluable(nir_instr_as_alu(instr)->op))
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

    /* Position-input arity.  A computed varying is produced from the first input
     * only (the varying producer feeds a single attribute), so a shader that
     * produces a computed varying stays single-input position and keeps the
     * original restriction: every non-first input must appear solely as a
     * passthrough varying source, leaving velem[0] (the first input) as the only one
     * feeding computation.  Without a computed varying, position may read up to
     * R300_R2VB_MAX_PRODUCER_INPUTS inputs -- the multi-input position path -- which
     * the producer feeds at VAR0+a in input order.  The two are kept orthogonal so a
     * failure localizes to one mechanism. */
    if (allow_computed_varying && r300_r2vb_first_computed_varying(nir) >= 0) {
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

    /* Multi-input position (no computed varying): bound the inputs feeding position
     * to what the producer's embedded vertex and the VAP output vectors carry. */
    if (r300_r2vb_count_position_inputs(nir) > R300_R2VB_MAX_PRODUCER_INPUTS)
        return false;
    return true;
}

/* The 04f admission oracle: derive the producer FS the restage route would
 * run for the bound VS and compile it into a throwaway code object, admitting
 * on the ACTUAL emitted ALU slot count against the backend's emit ceiling
 * (r300_fs_measure_nir_admission).  This replaces the scalar-NIR count proxy,
 * which over-rejected dense kernels the vectorizing backend packs far
 * smaller.  Both producer passes must fit: the position pass, and -- when a
 * computed varying is admitted -- that varying's pass.  The measured NIR is
 * built by the same r300_r2vb_build_restaged_fs_nir the delivery path uses
 * and preprocessed with the same r300_optimize_nir create_fs_state runs, so
 * the measured program is the program the producer compiles at delivery.
 * The verdict is memoized on the VS (immutable NIR, process-constant env
 * gates), so the throwaway compile runs once per VS, not per draw. */
/* Budget-escape gate (R300_R2VB_BUDGET_ESCAPE): "spill1" arms the single-vec4
 * carry-BO producer split for an over-budget position pass.  Any other value is
 * ignored with one note, matching the R300_HB_R400_US gate pattern; unset keeps
 * the split off and the over-budget verdict collapses to a plain reject, so the
 * route is byte-identical to the pre-split path. */
static bool r300_r2vb_budget_escape_enabled(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("R300_R2VB_BUDGET_ESCAPE");
        if (!e)
            mode = 0;
        else if (strcmp(e, "spill1") == 0)
            mode = 1;
        else {
            fprintf(stderr,
                    "r300: ignoring R300_R2VB_BUDGET_ESCAPE=%s; use spill1 "
                    "(single FP32 vec4 carry-BO producer split)\n", e);
            mode = 0;
        }
    }
    return mode == 1;
}

static enum r300_fs_admission
r300_r2vb_measure_pass(struct r300_context *r300, nir_shader *vs_nir,
                       gl_varying_slot target, const char *pass_name,
                       enum r300_r2vb_position_space space,
                       unsigned *out_alu)
{
    /* WINDOW includes frcp + viewport MADs; CLIP does not.  Admission is
     * keyed by space so a CLIP-route classify pass cannot inherit a WINDOW
     * force-split memo (or the reverse). */
    nir_shader *fs = r300_r2vb_build_restaged_fs_nir(r300, vs_nir, target,
                                                     space);
    if (fs == NULL)
        return R300_FS_ADMIT_REJECT;
    r300_optimize_nir(fs, r300->screen);
    unsigned alu_len = 0;
    enum r300_fs_admission adm =
        r300_fs_measure_nir_admission(r300, fs, &alu_len,
                                      R300_FS_INPUT_R2VB_FLAT_VERTEX, NULL);
    ralloc_free(fs);
    if (out_alu)
        *out_alu = alu_len;
    if (getenv("R300_R2VB_EXEC_DEBUG"))
        fprintf(stderr,
                "r2vb_admission pass=%s space=%s verdict=%s emitted_alu=%u\n",
                pass_name,
                space == R300_R2VB_POSITION_WINDOW ? "window" : "clip",
                adm == R300_FS_ADMIT_FITS ? "fits" :
                adm == R300_FS_ADMIT_OVER_ALU_BUDGET ? "over_alu_budget"
                                                     : "reject",
                alu_len);
    return adm;
}

static bool r300_r2vb_measure_pass_fits(struct r300_context *r300,
                                        nir_shader *vs_nir,
                                        gl_varying_slot target,
                                        const char *pass_name,
                                        enum r300_r2vb_position_space space)
{
    return r300_r2vb_measure_pass(r300, vs_nir, target, pass_name, space,
                                  NULL) == R300_FS_ADMIT_FITS;
}

/* Compact carry-composition string for the EXEC_DEBUG trace: one letter per
 * typed FP32 transport (f float, i signed, u unsigned, b boolean). */
static void r300_r2vb_carry_types_str(const struct r300_mp_partition *p,
                                      char *buf, size_t len)
{
    unsigned n = 0;
    for (unsigned i = 0; i < p->num_bases && n + 1 < len; i++) {
        switch (p->r2vb_transport[i]) {
        case R300_MP_R2VB_SINT:
            buf[n++] = 'i';
            break;
        case R300_MP_R2VB_UINT:
            buf[n++] = 'u';
            break;
        case R300_MP_R2VB_BOOL1:
        case R300_MP_R2VB_BOOL32:
            buf[n++] = 'b';
            break;
        default:
            buf[n++] = 'f';
            break;
        }
    }
    buf[n] = '\0';
}

/* Attempt the single-vec4 carry-BO split on the over-budget position pass:
 * derive the same optimized position producer FS the oracle measured, rank a
 * single-vec4 cut, build the two FP32 halves, and admit only when both compile
 * under the emit ceiling.  Every failure (no admissible cut, either half over
 * budget or rejected, a construction failure) declines and the caller falls
 * back to a plain reject exactly as the pre-split route does. */
static bool r300_r2vb_split_admitted(struct r300_context *r300,
                                     nir_shader *vs_nir, unsigned num_in,
                                     enum r300_r2vb_position_space space)
{
    /* The pass-B producer draw feeds num_in model attributes plus the carry,
     * so the split is deliverable only within the producer's input ceiling. */
    if (num_in + 1 > R300_R2VB_MAX_PRODUCER_INPUTS)
        return false;

    nir_shader *pos = r300_r2vb_build_restaged_fs_nir(r300, vs_nir,
                                                      VARYING_SLOT_POS, space);
    if (pos == NULL)
        return false;
    r300_optimize_nir(pos, r300->screen);

    struct r300_mp_partition part;
    bool admitted = false;
    if (r300_mp_find_vec4_cut(pos, &part)) {
        nir_shader *pass_a = r300_mp_build_carry_pass_a(pos, &part);
        nir_shader *pass_b = r300_mp_build_pos_pass_b(pos, &part, num_in);
        if (pass_a && pass_b) {
            r300_optimize_nir(pass_a, r300->screen);
            r300_optimize_nir(pass_b, r300->screen);
            unsigned la = 0, lb = 0;
            enum r300_fs_admission aa =
                r300_fs_measure_nir_admission(r300, pass_a, &la,
                                              R300_FS_INPUT_R2VB_FLAT_VERTEX,
                                              NULL);
            enum r300_fs_admission ab =
                r300_fs_measure_nir_admission(r300, pass_b, &lb,
                                              R300_FS_INPUT_R2VB_FLAT_VERTEX,
                                              NULL);
            admitted = aa == R300_FS_ADMIT_FITS && ab == R300_FS_ADMIT_FITS;
            if (getenv("R300_R2VB_EXEC_DEBUG")) {
                char types[R300_MP_MAX_CARRY_COMPS + 1];
                r300_r2vb_carry_types_str(&part, types, sizeof(types));
                fprintf(stderr,
                        "r2vb_split cut=%u carry_bases=%u carry_comps=%u "
                        "carry_types=%s passA_alu=%u passB_alu=%u admitted=%d\n",
                        part.cut_index, part.num_bases, part.total_comps, types,
                        la, lb, admitted);
            }
        }
        if (pass_a)
            ralloc_free(pass_a);
        if (pass_b)
            ralloc_free(pass_b);
    } else if (getenv("R300_R2VB_EXEC_DEBUG")) {
        fprintf(stderr,
                "r2vb_split declined: no exact single-vec4 cut (width, "
                "logical type, or integer range)\n");
    }
    ralloc_free(pos);
    return admitted;
}

/* Shadow decision parity: the producer plan classifies the same (VS,
 * computed-varying mode, position space) cell the admission memo just decided,
 * and its effective decision under the live gates must match.  A plan SPLIT is
 * effective only when the spill1 budget-escape gate is armed; ungated it
 * collapses to the same reject the memo records.  The plan is cached on the
 * VS, so the extra measurement compiles run once per cell.  A mismatch counts
 * on the process-wide divergence counter and prints under
 * R300_R2VB_PLAN_DEBUG=1; the memo stays authoritative and rendering
 * proceeds unchanged. */
bool r300_r2vb_typed_split_gate_value(const char *value)
{
    return value && strcmp(value, "1") == 0;
}

bool r300_r2vb_auto_single_gate_value(const char *value)
{
    return value && strcmp(value, "1") == 0;
}

bool r300_r2vb_slot_grid_gate_value(const char *value)
{
    return value && strcmp(value, "1") == 0;
}

bool r300_r2vb_slot_fetch_gate_value(const char *value)
{
    return value && strcmp(value, "1") == 0;
}

enum r300_r2vb_model_source_kind
r300_r2vb_model_source_classify(bool is_user_buffer, bool has_winsys_bo,
                                bool has_cpu_shadow)
{
    if (is_user_buffer)
        return R300_R2VB_MODEL_UNSUPPORTED;
    /* A resource carrying both backings has no authority contract yet:
     * the generic map path writes the shadow whenever it exists, so the
     * BO may hold stale data.  Decline until a transition or generation
     * contract is proven. */
    if (has_winsys_bo && has_cpu_shadow)
        return R300_R2VB_MODEL_UNSUPPORTED;
    if (has_winsys_bo)
        return R300_R2VB_MODEL_REAL_BO;
    if (has_cpu_shadow)
        return R300_R2VB_MODEL_CPU_SHADOW_UPLOAD;
    return R300_R2VB_MODEL_UNSUPPORTED;
}

/* Materialize one model stream for the BO-fetch producer.  A real BO takes
 * a reference at the start-adjusted application offset with no copy.  A
 * CPU shadow uploads exactly the fetched span -- first record through the
 * last fetched byte -- so the extent proof and the copy coincide, and the
 * descriptor offset is the uploader's; u_upload_unmap flushes the write
 * before the caller emits.  Every routed draw uploads afresh: the shadow
 * carries no write-generation authority that would make reuse safe.
 * Returns false with *out cleared on every decline, so the caller falls
 * back to gallivm with no state to restore. */
void r300_r2vb_model_fetch_fini(struct r300_r2vb_model_fetch *m)
{
    pipe_resource_reference(&m->resource, NULL);
    r300_r2vb_model_fetch_init(m);
}

static bool
r300_r2vb_materialize_model_fetch(struct r300_context *r300,
                                  const struct pipe_vertex_buffer *vb,
                                  const struct pipe_vertex_element *ve,
                                  uint32_t start, uint32_t count,
                                  const struct r300_r2vb_producer_stream *model_stream,
                                  struct r300_r2vb_model_fetch *out)
{
    /* The record must arrive initialized and empty: a reused record still
     * owning a reference would leak it under the entry state below. */
    assert(out->kind == R300_R2VB_MODEL_UNSUPPORTED && !out->resource);
    memset(out, 0, sizeof(*out));
    out->kind = R300_R2VB_MODEL_UNSUPPORTED;
    /* The validated stream record is the single source of the record and
     * stride widths -- re-proved here so the materializer trusts neither
     * its caller nor the builder -- and the bounded stride keeps the
     * byte multiplications from wrapping. */
    if (!model_stream ||
        (model_stream->size_dwords != 3 && model_stream->size_dwords != 4) ||
        model_stream->logical_components != 4 ||
        model_stream->stride_dwords < model_stream->size_dwords ||
        model_stream->stride_dwords > R300_R2VB_VBPNTR_STRIDE_DWORDS_MAX ||
        model_stream->offset_bytes % 4 != 0)
        return false;
    uint32_t record_bytes = model_stream->size_dwords * 4;
    uint32_t stride_bytes = model_stream->stride_dwords * 4;
    if (count == 0 || count >= 65536 || ve->src_stride != stride_bytes)
        return false;
    struct pipe_resource *res =
        vb->is_user_buffer ? NULL : vb->buffer.resource;
    struct r300_resource *rres = res ? r300_resource(res) : NULL;
    enum r300_r2vb_model_source_kind kind = r300_r2vb_model_source_classify(
        vb->is_user_buffer, rres && rres->buf,
        rres && rres->malloced_buffer);
    if (kind == R300_R2VB_MODEL_UNSUPPORTED)
        return false;

    uint64_t source_offset = (uint64_t)vb->buffer_offset + ve->src_offset +
                             (uint64_t)start * stride_bytes;
    /* Offset coherence: the stream builder computed the same start-
     * adjusted offset; a divergence means the two contracts describe
     * different bytes, and the rebased emission stream would then point
     * the GPU at the wrong part of the upload. */
    if (source_offset != model_stream->offset_bytes)
        return false;
    uint64_t source_end = source_offset +
                          (uint64_t)(count - 1) * stride_bytes +
                          record_bytes;
    if (source_end > res->width0 || source_offset > UINT32_MAX ||
        source_end - source_offset > UINT_MAX) {
        r300_r2vb_model_fetch_fini(out);
        return false;
    }

    uint64_t span = source_end - source_offset;
    if (kind == R300_R2VB_MODEL_REAL_BO) {
        pipe_resource_reference(&out->resource, res);
        out->kind = kind;
        out->gpu_offset = (uint32_t)source_offset;
        out->count = count;
        out->stride_dwords = model_stream->stride_dwords;
        out->record_dwords = model_stream->size_dwords;
        out->span_bytes = span;
        return true;
    }

    unsigned upload_offset = 0;
    struct pipe_resource *uploaded = NULL;
    u_upload_data_ref(r300->uploader, 0,
                      (unsigned)(source_end - source_offset), 4,
                      rres->malloced_buffer + source_offset, &upload_offset,
                      &uploaded);
    if (!uploaded) {
        r300_r2vb_model_fetch_fini(out);
        return false;
    }
    u_upload_unmap(r300->uploader);
    out->kind = kind;
    out->resource = uploaded;
    out->gpu_offset = upload_offset;
    out->count = count;
    out->stride_dwords = model_stream->stride_dwords;
    out->record_dwords = model_stream->size_dwords;
    out->span_bytes = span;
    out->uploaded_bytes = span;
    return true;
}

bool r300_r2vb_position_input_mapping_ok(unsigned num_position_inputs,
                                         unsigned app_driver_location,
                                         unsigned location_rank,
                                         unsigned velem_count,
                                         unsigned vertex_buffer_index,
                                         unsigned nr_vertex_buffers,
                                         bool buffer_bound,
                                         enum pipe_format format)
{
    if (num_position_inputs != 1 || velem_count < 1)
        return false;
    /* The first canary restricts the source identity executably: the one
     * position input must sit at application driver location zero and
     * compact to rank zero, so velem[0] is its element by the
     * element-i-feeds-input-i convention.  A shader whose single position
     * source lives at a nonzero original location declines here rather
     * than fetching the wrong element; widening this needs the plan to
     * carry the source's location and rank as data. */
    if (app_driver_location != 0 || location_rank != 0)
        return false;
    if (vertex_buffer_index >= nr_vertex_buffers || !buffer_bound)
        return false;
    return format == PIPE_FORMAT_R32G32B32_FLOAT ||
           format == PIPE_FORMAT_R32G32B32A32_FLOAT;
}

bool r300_r2vb_producer_interface_init(
    const struct r300_r2vb_producer_fetch *fetch,
    unsigned slot_dst_vec_loc, unsigned model_dst_vec_loc,
    struct r300_r2vb_producer_interface *out)
{
    if (!fetch || fetch->streams.num != 2)
        return false;
    /* DST_VEC_LOC is a 5-bit field; a wider value would silently alias
     * another input vector. */
    if (slot_dst_vec_loc > 31 || model_dst_vec_loc > 31 ||
        slot_dst_vec_loc == model_dst_vec_loc)
        return false;
    const struct r300_r2vb_producer_stream *model = &fetch->streams.stream[1];
    enum pipe_format model_format;
    switch (model->size_dwords) {
    case 3:
        model_format = PIPE_FORMAT_R32G32B32_FLOAT;
        break;
    case 4:
        model_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
        break;
    default:
        return false;
    }
    uint16_t slot_type = r300_translate_vertex_data_type(
        PIPE_FORMAT_R32G32B32A32_FLOAT);
    uint16_t model_type = r300_translate_vertex_data_type(model_format);
    if (slot_type == R300_INVALID_FORMAT ||
        model_type == R300_INVALID_FORMAT)
        return false;
    memset(out, 0, sizeof(*out));
    /* Two elements share the first register pair; the model element is
     * the last fetched vector. */
    uint32_t e0 = slot_type | (slot_dst_vec_loc << R300_DST_VEC_LOC_SHIFT);
    uint32_t e1 = model_type |
                  (model_dst_vec_loc << R300_DST_VEC_LOC_SHIFT) |
                  R300_LAST_VEC;
    out->prog_stream_cntl[0] = e0 | (e1 << 16);
    out->prog_stream_cntl_ext[0] =
        (uint32_t)r300_translate_vertex_data_swizzle(
            PIPE_FORMAT_R32G32B32A32_FLOAT) |
        ((uint32_t)r300_translate_vertex_data_swizzle(model_format) << 16);
    out->vap_vtx_size = fetch->vap_vtx_size;
    return true;
}

bool r300_r2vb_position_source_scan(nir_shader *vs_nir,
                                    struct r300_r2vb_position_source *out)
{
    memset(out, 0, sizeof(*out));
    nir_shader *tmp = nir_shader_clone(NULL, vs_nir);
    if (!tmp)
        return false;
    /* Strip every non-position store, DCE, and drop dead inputs: the
     * survivors are exactly the inputs feeding gl_Position (the
     * count_position_inputs reduction, retained here as an identity). */
    nir_function_impl *impl = nir_shader_get_entrypoint(tmp);
    nir_foreach_block(block, impl) {
        nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
                continue;
            nir_variable *o = nir_intrinsic_get_var(intr, 0);
            if (o && (o->data.mode & nir_var_shader_out) &&
                o->data.location != VARYING_SLOT_POS)
                nir_instr_remove(instr);
        }
    }
    nir_opt_dce(tmp);
    nir_remove_dead_variables(tmp, nir_var_shader_in, NULL);
    unsigned n = 0;
    int surviving_location = -1;
    unsigned driver_location = 0;
    nir_foreach_variable_with_modes(var, tmp, nir_var_shader_in) {
        n++;
        surviving_location = var->data.location;
        driver_location = var->data.driver_location;
    }
    ralloc_free(tmp);
    if (n != 1)
        return false;
    /* Rank among the ORIGINAL bound VS inputs in ascending location order:
     * velem[k] feeds the k-th input in that order, so the rank -- not the
     * driver location alone -- names the element.  The bound VS arrives in
     * deref/variable form (r300_optimize_nir does not run nir_lower_io),
     * matching r300_r2vb_input_velem_index. */
    unsigned rank = 0;
    nir_foreach_variable_with_modes(var, vs_nir, nir_var_shader_in)
        if (var->data.location < surviving_location)
            rank++;
    if (driver_location > 255 || rank > 255)
        return false;
    out->app_driver_location = driver_location;
    out->location_rank = rank;
    out->valid = true;
    return true;
}

bool r300_r2vb_producer_fs_input_hwreg(
    const struct r300_shader_semantics *inputs, unsigned *out_hwreg)
{
    /* The first producer contract: one generic model input and nothing
     * else.  A color, FACE, fog, or WPOS input would shift the hardware
     * allocation and route stale rasterizer state into the producer. */
    if (inputs->color[0] != ATTR_UNUSED || inputs->color[1] != ATTR_UNUSED ||
        inputs->face != ATTR_UNUSED || inputs->fog != ATTR_UNUSED ||
        inputs->wpos != ATTR_UNUSED)
        return false;
    if (inputs->generic[0] == ATTR_UNUSED || inputs->num_generic != 1 ||
        inputs->num_total != 1)
        return false;
    for (unsigned i = 1; i < ATTR_GENERIC_COUNT; i++)
        if (inputs->generic[i] != ATTR_UNUSED)
            return false;
    /* Replay allocate_hardware_inputs order (colors, face, generics, fog,
     * WPOS): with everything before the generics unused, the register
     * counter reaches the single generic at zero. */
    *out_hwreg = 0;
    return true;
}

/* RS decode helpers shared by the binding constructor and the contract
 * checker, each proving one layer of the TC0-to-FS routing. */
static bool r2vb_rs_assembly_ok(const struct r300_rs_block *rs)
{
    return rs->vap_vtx_state_cntl == 0x5555 &&
           rs->vap_vsm_vtx_assm ==
               (R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0);
}

static bool r2vb_rs_output_fmt_ok(const struct r300_rs_block *rs)
{
    /* POS plus one 4-component TEX0 vector; fmt1 packs 3 bits per
     * texcoord slot. */
    return rs->vap_out_vtx_fmt[0] == R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT &&
           rs->vap_out_vtx_fmt[1] == 4;
}

static bool r2vb_rs_tc0_components_ok(const struct r300_rs_block *rs)
{
    /* The producer RS program is exactly one 4-component TC0 route: the
     * whole-word values pin the live immediate producer's decode -- IP
     * word 0x00d10000 (TEX_PTR 0, S/T/R/Q from C0..C3) and RS_COUNT
     * 0x00040004 (IT count 4, HIRES enabled).  A spare selector, a
     * nonzero texture pointer, or an inflated interpolator count all
     * fail here instead of hiding behind a component-only decode. */
    uint32_t want_ip = R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
                       R300_RS_SEL_T(R300_RS_SEL_C1) |
                       R300_RS_SEL_R(R300_RS_SEL_C2) |
                       R300_RS_SEL_Q(R300_RS_SEL_C3);
    uint32_t want_count = R300_IT_COUNT(4) | R300_HIRES_EN;
    return rs->ip[0] == want_ip && rs->count == want_count &&
           rs->inst_count == 0;
}

static bool r2vb_rs_writes_fs_reg(const struct r300_rs_block *rs,
                                  unsigned hwreg)
{
    /* Whole-word instruction: TEX_ID 0, TEX_CN_WRITE, TEX_ADDR at the
     * derived register, and every modifier bit clear -- an instruction
     * that writes the right register while referencing another
     * interpolator or carrying stale mode bits fails the equality. */
    return rs->inst[0] == (R300_RS_INST_TEX_ID(0) |
                           R300_RS_INST_TEX_CN_WRITE |
                           R300_RS_INST_TEX_ADDR(hwreg));
}

static bool r2vb_rs_tail_zero(const struct r300_rs_block *rs)
{
    for (unsigned i = 1; i < 8; i++)
        if (rs->ip[i] || rs->inst[i])
            return false;
    return true;
}

bool r300_r2vb_producer_logical_binding_init(
    const struct r300_r2vb_position_source *source,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    unsigned slot_dst_vec_loc, unsigned model_dst_vec_loc,
    struct r300_r2vb_producer_logical_binding *out)
{
    if (!source || !fs_inputs || !rs || !out)
        return false;
    /* The measured source identity, under the canary's executable
     * restriction: location zero, rank zero, so velem[0] is the element. */
    if (!source->valid || source->app_driver_location != 0 ||
        source->location_rank != 0)
        return false;
    unsigned hwreg;
    if (!r300_r2vb_producer_fs_input_hwreg(fs_inputs, &hwreg))
        return false;
    /* The derived RS block must already route TC0 to that register with
     * the expected assembly and output tuple; a binding built against a
     * disagreeing RS would emit a draw whose FS reads a stale input. */
    if (!r2vb_rs_assembly_ok(rs) || !r2vb_rs_output_fmt_ok(rs) ||
        !r2vb_rs_tc0_components_ok(rs) || !r2vb_rs_writes_fs_reg(rs, hwreg) ||
        !r2vb_rs_tail_zero(rs))
        return false;
    if (slot_dst_vec_loc > 31 || model_dst_vec_loc > 31 ||
        slot_dst_vec_loc == model_dst_vec_loc)
        return false;
    memset(out, 0, sizeof(*out));
    out->app_driver_location = source->app_driver_location;
    out->location_rank = source->location_rank;
    out->velem_index = 0;
    out->slot_dst_vec_loc = slot_dst_vec_loc;
    out->model_dst_vec_loc = model_dst_vec_loc;
    out->fs_hw_input_reg = hwreg;
    return true;
}

bool r300_r2vb_producer_logical_binding_from_state(
    const struct r300_r2vb_producer_plan *plan,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    const struct r300_vertex_stream_state *psc,
    struct r300_r2vb_producer_logical_binding *out)
{
    if (!plan || !psc)
        return false;
    /* The derived producer stream state carries exactly one register
     * pair: the slot position vector and the LAST_VEC model vector.
     * The destination locations come from decoding that live word; the
     * calibrated constants then gate them, so a derived state naming
     * other vectors declines instead of silently rebinding. */
    if (psc->count != 1)
        return false;
    uint32_t e0 = psc->vap_prog_stream_cntl[0] & 0xffff;
    uint32_t e1 = psc->vap_prog_stream_cntl[0] >> 16;
    if ((e0 & 0xf) != R300_DATA_TYPE_FLOAT_4)
        return false;
    if ((e1 & 0xf) != R300_DATA_TYPE_FLOAT_4 &&
        (e1 & 0xf) != R300_DATA_TYPE_FLOAT_3)
        return false;
    if ((e0 & R300_LAST_VEC) || !(e1 & R300_LAST_VEC))
        return false;
    for (unsigned i = 1; i < 8; i++)
        if (psc->vap_prog_stream_cntl[i] || psc->vap_prog_stream_cntl_ext[i])
            return false;
    unsigned slot_loc = (e0 >> R300_DST_VEC_LOC_SHIFT) & 0x1f;
    unsigned model_loc = (e1 >> R300_DST_VEC_LOC_SHIFT) & 0x1f;
    if (slot_loc != R300_R2VB_CAL_SLOT_DST_VEC_LOC ||
        model_loc != R300_R2VB_CAL_MODEL_DST_VEC_LOC)
        return false;
    return r300_r2vb_producer_logical_binding_init(
        &plan->position_source, fs_inputs, rs, slot_loc, model_loc, out);
}

/* Decode one packed PSC element pair field-by-field for its MEANING:
 * data type, destination vector, LAST_VEC, and the semantic swizzle --
 * FLOAT_4 reads XYZW identity, FLOAT_3 reads X,Y,Z with W from the
 * constant-one select, both with the full write mask. */
static bool r2vb_psc_element_ok(uint32_t cntl, uint32_t ext, unsigned elem,
                                unsigned record_dwords, unsigned dst_vec_loc,
                                bool want_last)
{
    uint32_t c = (cntl >> (elem * 16)) & 0xffff;
    uint32_t s = (ext >> (elem * 16)) & 0xffff;
    unsigned want_type = record_dwords == 3 ? R300_DATA_TYPE_FLOAT_3
                                            : R300_DATA_TYPE_FLOAT_4;
    if (record_dwords != 3 && record_dwords != 4)
        return false;
    if ((c & 0xf) != want_type)
        return false;
    if (((c >> R300_DST_VEC_LOC_SHIFT) & 0x1f) != dst_vec_loc)
        return false;
    if (((c & R300_LAST_VEC) != 0) != want_last)
        return false;
    unsigned want_w = record_dwords == 3 ? R300_SWIZZLE_SELECT_FP_ONE
                                         : R300_SWIZZLE_SELECT_W;
    return ((s >> R300_SWIZZLE_SELECT_X_SHIFT) & 7) == R300_SWIZZLE_SELECT_X &&
           ((s >> R300_SWIZZLE_SELECT_Y_SHIFT) & 7) == R300_SWIZZLE_SELECT_Y &&
           ((s >> R300_SWIZZLE_SELECT_Z_SHIFT) & 7) == R300_SWIZZLE_SELECT_Z &&
           ((s >> R300_SWIZZLE_SELECT_W_SHIFT) & 7) == want_w &&
           ((s >> R300_WRITE_ENA_SHIFT) & 0xf) == 0xf;
}

unsigned r300_r2vb_producer_binding_check(
    const struct r300_r2vb_producer_fetch *fetch,
    const struct r300_r2vb_producer_interface *psc,
    const struct r300_r2vb_producer_logical_binding *binding,
    const struct r300_rs_block *rs)
{
    unsigned v = 0;
    const struct r300_r2vb_producer_stream *slot = &fetch->streams.stream[0];
    const struct r300_r2vb_producer_stream *model = &fetch->streams.stream[1];
    /* Physical fetch extent: the descriptor record sum is the VAP vertex
     * size, carried unchanged into the PSC object. */
    if (fetch->streams.num != 2 ||
        slot->size_dwords + model->size_dwords != fetch->vap_vtx_size ||
        psc->vap_vtx_size != fetch->vap_vtx_size)
        v |= R300_R2VB_BINDING_FETCH_SIZE;
    /* PSC meaning per element; the swizzle bit reports semantic decode
     * failures, the LAST_VEC bit reports fetch-termination placement. */
    if (!r2vb_psc_element_ok(psc->prog_stream_cntl[0],
                             psc->prog_stream_cntl_ext[0], 0,
                             slot->size_dwords, binding->slot_dst_vec_loc,
                             false) ||
        !r2vb_psc_element_ok(psc->prog_stream_cntl[0],
                             psc->prog_stream_cntl_ext[0], 1,
                             model->size_dwords, binding->model_dst_vec_loc,
                             true))
        v |= R300_R2VB_BINDING_SWIZZLE;
    if ((psc->prog_stream_cntl[0] & (R300_LAST_VEC << 16)) == 0 ||
        (psc->prog_stream_cntl[0] & R300_LAST_VEC) != 0)
        v |= R300_R2VB_BINDING_LAST_VEC;
    if (!r2vb_rs_assembly_ok(rs))
        v |= R300_R2VB_BINDING_VAP_ASSEMBLY;
    if (!r2vb_rs_output_fmt_ok(rs))
        v |= R300_R2VB_BINDING_OUTPUT_FMT;
    if (!r2vb_rs_tc0_components_ok(rs))
        v |= R300_R2VB_BINDING_RS_COMPONENTS;
    if (!r2vb_rs_writes_fs_reg(rs, binding->fs_hw_input_reg))
        v |= R300_R2VB_BINDING_FS_REGISTER;
    for (unsigned i = 1; i < 8; i++)
        if (psc->prog_stream_cntl[i] || psc->prog_stream_cntl_ext[i])
            v |= R300_R2VB_BINDING_TAIL_STATE;
    if (!r2vb_rs_tail_zero(rs))
        v |= R300_R2VB_BINDING_TAIL_STATE;
    return v;
}

bool
r300_r2vb_producer_streams_rebind(const struct r300_r2vb_producer_streams *orig,
                                  const struct r300_r2vb_model_fetch *model,
                                  uint64_t slot_bo_bytes, uint32_t count,
                                  struct r300_r2vb_producer_fetch *out)
{
    if (!orig || !model || model->kind == R300_R2VB_MODEL_UNSUPPORTED ||
        !model->resource)
        return false;
    /* The transaction was materialized for one exact draw: the count,
     * record, and stride must match, and the model stream validates
     * against the bounded end of THIS suballocation, never the backing
     * BO's full width -- the uploader is a suballocator, and adjacent
     * capacity belongs to other transactions. */
    if (count != model->count ||
        orig->stream[1].stride_dwords != model->stride_dwords ||
        orig->stream[1].size_dwords != model->record_dwords)
        return false;
    uint64_t materialized_end = (uint64_t)model->gpu_offset +
                                model->span_bytes;
    if (materialized_end > model->resource->width0)
        return false;
    struct r300_r2vb_producer_streams bound = *orig;
    bound.stream[1].offset_bytes = model->gpu_offset;
    return r300_r2vb_producer_fetch_init(&bound, count, slot_bo_bytes,
                                         materialized_end, out);
}

unsigned r300_r2vb_producer_bo_draw_cs_dwords(void)
{
    /* Fixed emission shape, count-independent: REG_SEQ costs one header
     * plus its values.  PROG_STREAM_CNTL 0..7 and EXT 0..7 (9 + 9, the
     * zeroed tail is EMITTED, clearing stale hardware state), VTX_SIZE
     * (2), VTX_STATE_CNTL (2), VSM_VTX_ASSM (2), OUTPUT_VTX_FMT pair
     * (3), GB_ENABLE (2), RS_IP 0..7 (9), RS_COUNT + RS_INST_COUNT (3),
     * RS_INST 0..7 (9), VF_MAX + VF_MIN pair (3), two-array LOAD_VBPNTR
     * with both NOP-form relocations (header + numarrays + control +
     * two offsets + two 2-dword relocations = 9), DRAW_VBUF_2 (2). */
    return 9 + 9 + 2 + 2 + 2 + 3 + 2 + 9 + 3 + 9 + 3 + 9 + 2;
}

bool r300_r2vb_producer_bo_draw_validate(
    struct r300_context *r300,
    const struct r300_r2vb_producer_plan *plan,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    const struct r300_vertex_stream_state *psc_state,
    const struct pipe_vertex_buffer *vb, const struct pipe_vertex_element *ve,
    unsigned velem_count, unsigned nr_vertex_buffers,
    struct pipe_resource *slot_resource,
    struct pipe_resource *output_resource, uint32_t start, uint32_t count,
    enum r300_r2vb_position_space space,
    struct r300_r2vb_producer_bo_draw *out)
{
    /* Lifecycle: validate consumes an EMPTY transaction only; an owned
     * transaction keeps its references until fini, so validating into it
     * would leak the slot, model, and output storage. */
    if (out->state != R300_R2VB_BO_DRAW_EMPTY || out->slot_resource ||
        out->output_resource || out->model.resource)
        return false;
    /* Transaction-owned gate: the exact opt-in is the first fallible
     * operation, so a gate-off call declines before the model upload. */
    if (!r300_r2vb_slot_fetch_gate_value(getenv("R300_R2VB_SLOT_FETCH")))
        return false;
    if (!plan || !vb || !ve || !slot_resource || !output_resource ||
        !rs || !psc_state)
        return false;
    /* Source identity from the plan's measured record; literals never
     * reach the mapping contract. */
    const struct r300_r2vb_position_source *src = &plan->position_source;
    if (!src->valid)
        return false;
    if (!r300_r2vb_position_input_mapping_ok(
            plan->num_position_inputs, src->app_driver_location,
            src->location_rank, velem_count, ve->vertex_buffer_index,
            nr_vertex_buffers, vb->buffer.resource != NULL, ve->src_format))
        return false;
    if (!r300_r2vb_slot_layout_init(count, false, &out->layout))
        return false;
    struct r300_r2vb_producer_streams streams;
    if (!r300_r2vb_producer_streams_init(vb->buffer_offset, ve->src_offset,
                                         ve->src_stride, ve->src_format,
                                         start, &streams))
        return false;
    if (!r300_r2vb_materialize_model_fetch(r300, vb, ve, start, count,
                                           &streams.stream[1], &out->model))
        return false;
    /* From here every failure releases what materialization took. */
    if (!r300_r2vb_producer_streams_rebind(&streams, &out->model,
                                           slot_resource->width0, count,
                                           &out->fetch))
        goto fail;
    if (!r300_r2vb_producer_logical_binding_from_state(plan, fs_inputs, rs,
                                                       psc_state,
                                                       &out->logical))
        goto fail;
    if (!r300_r2vb_producer_interface_init(&out->fetch,
                                           out->logical.slot_dst_vec_loc,
                                           out->logical.model_dst_vec_loc,
                                           &out->psc))
        goto fail;
    if (r300_r2vb_producer_binding_check(&out->fetch, &out->psc,
                                         &out->logical, rs) != 0)
        goto fail;
    /* Output authority: the one-row producer writes count FP32x4 texels
     * from offset zero of the bound framebuffer color target, whose
     * relocation rides the dirty framebuffer state atom.  Validation
     * proves that identity, the storage extent, the exact producer
     * format, and -- when a winsys buffer backs the resource -- the GTT
     * placement the calibrated delivery observed. */
    {
        const struct pipe_framebuffer_state *fb =
            (const struct pipe_framebuffer_state *)r300->fb_state.state;
        struct r300_resource *outres = r300_resource(output_resource);
        if (!fb || fb->nr_cbufs < 1 ||
            fb->cbufs[0].texture != output_resource)
            goto fail;
        if (!outres->buf && !outres->malloced_buffer)
            goto fail;
        if (outres->buf && outres->domain != RADEON_DOMAIN_GTT)
            goto fail;
        if (output_resource->format != PIPE_FORMAT_R32G32B32A32_FLOAT)
            goto fail;
        if (output_resource->width0 < out->layout.width ||
            output_resource->height0 < out->layout.height)
            goto fail;
        out->output_required_bytes = (uint64_t)count * 16;
        out->output_offset = 0;
        out->output_pitch_pixels = out->layout.pitch_pixels;
    }
    pipe_resource_reference(&out->output_resource, output_resource);
    pipe_resource_reference(&out->slot_resource, slot_resource);
    out->plan = plan;
    out->fs_inputs = fs_inputs;
    out->rs = rs;
    out->psc_state = psc_state;
    out->draw_start = start;
    out->count = count;
    out->space = space;
    out->required_cs_dwords = r300_r2vb_producer_bo_draw_cs_dwords();
    /* Freeze the mutable derived authorities by value; the emitter
     * consumes these copies rather than re-reading context state. */
    out->psc_snapshot = *psc_state;
    out->rs_snapshot = *rs;
    if (r300->viewport_state.state)
        out->viewport_snapshot =
            *(struct r300_viewport_state *)r300->viewport_state.state;
    out->state = R300_R2VB_BO_DRAW_VALIDATED;
    return true;

fail:
    r300_r2vb_model_fetch_fini(&out->model);
    return false;
}

bool r300_r2vb_producer_bo_draw_stage_cs(
    struct r300_context *r300, struct r300_r2vb_producer_bo_draw *txn,
    const struct r300_r2vb_producer_plan *plan,
    const struct r300_shader_semantics *fs_inputs,
    const struct r300_rs_block *rs,
    const struct r300_vertex_stream_state *psc_state)
{
    /* Lifecycle: staging consumes a VALIDATED transaction exactly once. */
    if (txn->state != R300_R2VB_BO_DRAW_VALIDATED)
        return false;
    /* Snapshot recheck: the transaction was validated against exactly
     * these authorities; a rebind between validate and staging makes
     * the transaction stale, and staleness declines rather than emits.
     * The RS block and PSC words are context-derived state updated in
     * place, so the by-value snapshots recheck their contents too. */
    if (txn->plan != plan || txn->fs_inputs != fs_inputs || txn->rs != rs ||
        txn->psc_state != psc_state)
        return false;
    if (memcmp(&txn->psc_snapshot, psc_state, sizeof(*psc_state)) != 0 ||
        memcmp(&txn->rs_snapshot, rs, sizeof(*rs)) != 0)
        return false;
    if (!txn->slot_resource || !txn->model.resource || !txn->output_resource)
        return false;
    struct r300_resource *slot = r300_resource(txn->slot_resource);
    struct r300_resource *model = r300_resource(txn->model.resource);
    struct r300_resource *output_bo = r300_resource(txn->output_resource);
    if (!slot->buf || !model->buf || !output_bo->buf)
        return false;
    /* Capacity first, and capacity only: the reservation may flush and
     * rotate the CS, but it emits no state, so the buffer list built
     * below always binds to the final command stream. */
    r300_r2vb_reserve_bo_draw_cs(r300, txn->required_cs_dwords);
    /* Complete-list validation: every buffer the draw touches -- the
     * ordinary dirty-state resources and the three producer BOs --
     * enters the list before cs_validate.  A validation failure flushes
     * and drops the additions, so the retry re-adds the complete
     * population; a second failure declines with no register written. */
    bool retried = false;
retry:
    r300_add_state_buffers(r300, false, NULL);
    r300->rws->cs_add_buffer(&r300->cs, slot->buf,
                             RADEON_USAGE_READ | RADEON_USAGE_SYNCHRONIZED,
                             slot->domain);
    r300->rws->cs_add_buffer(&r300->cs, model->buf,
                             RADEON_USAGE_READ | RADEON_USAGE_SYNCHRONIZED,
                             model->domain);
    r300->rws->cs_add_buffer(&r300->cs, output_bo->buf,
                             RADEON_USAGE_READWRITE |
                                 RADEON_USAGE_SYNCHRONIZED |
                                 RADEON_PRIO_COLOR_BUFFER,
                             output_bo->domain);
    if (!r300->rws->cs_validate(&r300->cs)) {
        if (retried)
            return false;
        retried = true;
        goto retry;
    }
    /* The validation may have flushed; re-prove the derived-state words
     * against the snapshot before the transaction turns READY. */
    if (memcmp(&txn->psc_snapshot, psc_state, sizeof(*psc_state)) != 0 ||
        memcmp(&txn->rs_snapshot, rs, sizeof(*rs)) != 0)
        return false;
    /* Relocation indices only after the final CS holds every buffer. */
    txn->slot_reloc_index = r300->rws->cs_lookup_buffer(&r300->cs, slot->buf);
    txn->model_reloc_index =
        r300->rws->cs_lookup_buffer(&r300->cs, model->buf);
    txn->output_reloc_index =
        r300->rws->cs_lookup_buffer(&r300->cs, output_bo->buf);
    if (txn->slot_reloc_index < 0 || txn->model_reloc_index < 0 ||
        txn->output_reloc_index < 0)
        return false;
    txn->state = R300_R2VB_BO_DRAW_READY;
    return true;
}

bool r300_r2vb_producer_bo_draw_emit(struct r300_context *r300,
                                     struct r300_r2vb_producer_bo_draw *txn)
{
    /* Lifecycle: emission consumes a READY transaction exactly once. */
    if (txn->state != R300_R2VB_BO_DRAW_READY)
        return false;
    /* Dirty state first.  Staging validated the complete buffer list, so
     * from here every operation completes mechanically; the atoms this
     * writes are the ones the capacity reservation accounted for. */
    r300_emit_dirty_state(r300);
    const struct r300_r2vb_producer_stream *slot =
        &txn->fetch.streams.stream[0];
    const struct r300_r2vb_producer_stream *model =
        &txn->fetch.streams.stream[1];
    CS_LOCALS(r300);
    unsigned start_cdw = cs_copy->current.cdw;
    BEGIN_CS(r300_r2vb_producer_bo_draw_cs_dwords());
    /* The full stream range, zeroed tail included: a stale
     * PROG_STREAM_CNTL word from an inherited draw would add a phantom
     * fetch, so the emitter clears all eight pairs. */
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_0, 8);
    OUT_CS_TABLE(txn->psc_snapshot.vap_prog_stream_cntl, 8);
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_EXT_0, 8);
    OUT_CS_TABLE(txn->psc_snapshot.vap_prog_stream_cntl_ext, 8);
    OUT_CS_REG(R300_VAP_VTX_SIZE, txn->fetch.streams.fetch_dwords);
    OUT_CS_REG(R300_VAP_VTX_STATE_CNTL, txn->rs_snapshot.vap_vtx_state_cntl);
    OUT_CS_REG(R300_VAP_VSM_VTX_ASSM, txn->rs_snapshot.vap_vsm_vtx_assm);
    OUT_CS_REG_SEQ(R300_VAP_OUTPUT_VTX_FMT_0, 2);
    OUT_CS_TABLE(txn->rs_snapshot.vap_out_vtx_fmt, 2);
    OUT_CS_REG(R300_GB_ENABLE, txn->rs_snapshot.gb_enable);
    OUT_CS_REG_SEQ(R300_RS_IP_0, 8);
    OUT_CS_TABLE(txn->rs_snapshot.ip, 8);
    OUT_CS_REG_SEQ(R300_RS_COUNT, 2);
    OUT_CS(txn->rs_snapshot.count);
    OUT_CS(txn->rs_snapshot.inst_count);
    OUT_CS_REG_SEQ(R300_RS_INST_0, 8);
    OUT_CS_TABLE(txn->rs_snapshot.inst, 8);
    /* VAP_VF_MAX_VTX_INDX clamps every fetched index; a stale lower
     * bound would fold high indices onto a low vertex.  MAX (0x2134)
     * and MIN (0x2138) are register-adjacent, MAX first. */
    OUT_CS_REG_SEQ(R300_VAP_VF_MAX_VTX_INDX, 2);
    OUT_CS(txn->fetch.vf_max);
    OUT_CS(txn->fetch.vf_min);
    /* Two-array LOAD_VBPNTR: slot positions then the model span, both
     * offsets rebased by the kernel through the NOP-form relocations
     * whose indices staging captured from the final validated CS.  The
     * VBPNTR control macros take bytes and store dwords. */
    OUT_CS_PKT3(R300_PACKET3_3D_LOAD_VBPNTR, 3);
    OUT_CS(2 | R300_VC_FORCE_PREFETCH);
    OUT_CS(R300_VBPNTR_SIZE0(slot->size_dwords * 4) |
           R300_VBPNTR_STRIDE0(slot->stride_dwords * 4) |
           R300_VBPNTR_SIZE1(model->size_dwords * 4) |
           R300_VBPNTR_STRIDE1(model->stride_dwords * 4));
    OUT_CS(slot->offset_bytes);
    OUT_CS(model->offset_bytes);
    OUT_CS(0xc0001000); /* PKT3_NOP -- the relocation form LOAD_VBPNTR expects */
    OUT_CS((unsigned)txn->slot_reloc_index * 4);
    OUT_CS(0xc0001000);
    OUT_CS((unsigned)txn->model_reloc_index * 4);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS((txn->count << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
           R300_VAP_VF_CNTL__PRIM_POINTS |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST);
    END_CS;
    /* The fixed command size is a compile-time fact the capture ladder
     * re-proves by cursor delta; drift here is an emitter defect. */
    assert(cs_copy->current.cdw - start_cdw ==
           r300_r2vb_producer_bo_draw_cs_dwords());
    (void)start_cdw;
    txn->state = R300_R2VB_BO_DRAW_EMITTED;
    return true;
}

void r300_r2vb_producer_bo_draw_init(struct r300_r2vb_producer_bo_draw *txn)
{
    memset(txn, 0, sizeof(*txn));
    txn->slot_reloc_index = -1;
    txn->model_reloc_index = -1;
    txn->output_reloc_index = -1;
}

void r300_r2vb_producer_bo_draw_fini(struct r300_r2vb_producer_bo_draw *txn)
{
    pipe_resource_reference(&txn->slot_resource, NULL);
    pipe_resource_reference(&txn->output_resource, NULL);
    pipe_resource_reference(&txn->model.resource, NULL);
    memset(txn, 0, sizeof(*txn));
    txn->slot_reloc_index = -1;
    txn->model_reloc_index = -1;
    txn->output_reloc_index = -1;
}

/* No-submit B0-B4 capture of the shipped producer BO-fetch draw.  Fires
 * once from a real flush under the transaction's own exact gate
 * (R300_R2VB_SLOT_FETCH=1) plus R300_R2VB_BO_DRAW=capture.  Builds the
 * calibrated fixture -- the derived stream state is produced through the
 * same streams/fetch/interface helpers the immediate producer uses, so
 * it round-trips from_state by construction -- allocates real GTT slot,
 * model, and output BOs, installs a framebuffer whose cbufs[0] is the
 * output target (validate requires that identity; left unmarked so
 * emit_dirty_state does not touch it), then for each of the five widths
 * runs the real validate -> stage_cs -> emit through the live winsys and
 * flushes RADEON_FLUSH_NOOP.  R300_TRACE captures each IB before
 * DRM_RADEON_CS, so the run carries zero submission and the decoded
 * custom range proves the 64-dword size is count-independent except the
 * VF_MAX_VTX_INDX and DRAW_VBUF_2 count words. */
bool r300_r2vb_bo_draw_capture_selftest(struct r300_context *r300,
                                        bool from_flush)
{
    static bool fired = false;
    if (!from_flush || fired)
        return false;
    if (!r300_r2vb_slot_fetch_gate_value(getenv("R300_R2VB_SLOT_FETCH")))
        return false;
    if (!r300_r2vb_option_is(getenv("R300_R2VB_BO_DRAW"), "capture"))
        return false;
    fired = true;

    static const uint32_t widths[5] = { 3, 2048, 2049, 4095, 4096 };
    const uint32_t max_w = 4096;

    /* Calibrated producer fixture: single-generic FS, the exact RS words
     * from the immediate-producer decode, and one measured position
     * input at source location and rank zero. */
    struct r300_shader_semantics fs;
    r300_shader_semantics_reset(&fs);
    fs.generic[0] = 0;
    fs.num_generic = 1;
    fs.num_total = 1;

    struct r300_rs_block rs;
    memset(&rs, 0, sizeof(rs));
    rs.vap_vtx_state_cntl = 0x5555;
    rs.vap_vsm_vtx_assm = R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0;
    rs.vap_out_vtx_fmt[0] = R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT;
    rs.vap_out_vtx_fmt[1] = 4;
    rs.ip[0] = R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
               R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
               R300_RS_SEL_Q(R300_RS_SEL_C3);
    rs.count = R300_IT_COUNT(4) | R300_HIRES_EN;
    rs.inst[0] = R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                 R300_RS_INST_TEX_ADDR(0);

    struct r300_r2vb_producer_plan plan;
    memset(&plan, 0, sizeof(plan));
    plan.num_position_inputs = 1;
    plan.position_source.app_driver_location = 0;
    plan.position_source.location_rank = 0;
    plan.position_source.valid = true;

    /* The derived stream state the immediate producer would build for a
     * FLOAT_3 model at model destination vector 6 and slot at 0. */
    struct r300_r2vb_producer_streams st;
    struct r300_r2vb_producer_fetch ft;
    struct r300_r2vb_producer_interface it;
    if (!r300_r2vb_producer_streams_init(0, 0, 12, PIPE_FORMAT_R32G32B32_FLOAT,
                                         0, &st) ||
        !r300_r2vb_producer_fetch_init(&st, max_w, (uint64_t)max_w * 16,
                                       (uint64_t)max_w * 12, &ft) ||
        !r300_r2vb_producer_interface_init(&ft, 0, 6, &it))
        return false;
    struct r300_vertex_stream_state psc;
    memset(&psc, 0, sizeof(psc));
    for (unsigned i = 0; i < 8; i++) {
        psc.vap_prog_stream_cntl[i] = it.prog_stream_cntl[i];
        psc.vap_prog_stream_cntl_ext[i] = it.prog_stream_cntl_ext[i];
    }
    psc.count = 1;

    struct pipe_resource *slot_res =
        r2vb_create_selftest_bo(r300, (uint32_t)max_w * 16, 0);
    struct pipe_resource *model_res =
        r2vb_create_selftest_bo(r300, (uint32_t)max_w * 12, 0);
    struct pipe_resource *out_res =
        r2vb_create_selftest_bo(r300, (uint32_t)max_w * 16, 0);
    if (!slot_res || !model_res || !out_res) {
        pipe_resource_reference(&slot_res, NULL);
        pipe_resource_reference(&model_res, NULL);
        pipe_resource_reference(&out_res, NULL);
        return false;
    }

    /* Install the output as the framebuffer color target so validate's
     * identity check passes; keep the current state and restore it. */
    void *saved_fb = r300->fb_state.state;
    struct pipe_framebuffer_state cap_fb;
    memset(&cap_fb, 0, sizeof(cap_fb));
    cap_fb.width = max_w;
    cap_fb.height = 1;
    cap_fb.nr_cbufs = 1;
    cap_fb.cbufs[0].texture = out_res;
    cap_fb.cbufs[0].format = PIPE_FORMAT_R32G32B32A32_FLOAT;
    r300->fb_state.state = &cap_fb;

    /* Neutralize the application's dirty atoms for the length capture:
     * r300_add_state_buffers reads the parallel r300->fb_cbufs surface
     * array and the aa/textures atoms, which do not match this synthetic
     * framebuffer, and r300_emit_dirty_state would emit them.  Clearing
     * the dirty flags and the dirty-atom span keeps both to the three
     * producer BOs and the custom command range; the state is restored
     * after the capture so the enclosing flush is unaffected. */
    bool saved_fb_dirty = r300->fb_state.dirty;
    bool saved_aa_dirty = r300->aa_state.dirty;
    bool saved_tex_dirty = r300->textures_state.dirty;
    struct r300_atom *saved_first = r300->first_dirty;
    struct r300_atom *saved_last = r300->last_dirty;
    r300->fb_state.dirty = false;
    r300->aa_state.dirty = false;
    r300->textures_state.dirty = false;
    r300->first_dirty = NULL;
    r300->last_dirty = NULL;

    struct pipe_vertex_buffer vb;
    memset(&vb, 0, sizeof(vb));
    vb.buffer_offset = 0;
    vb.buffer.resource = model_res;
    struct pipe_vertex_element ve;
    memset(&ve, 0, sizeof(ve));
    ve.src_offset = 0;
    ve.src_stride = 12;
    ve.src_format = PIPE_FORMAT_R32G32B32_FLOAT;
    ve.vertex_buffer_index = 0;

    for (unsigned w = 0; w < 5; w++) {
        struct r300_r2vb_producer_bo_draw txn;
        r300_r2vb_producer_bo_draw_init(&txn);
        bool ok =
            r300_r2vb_producer_bo_draw_validate(
                r300, &plan, &fs, &rs, &psc, &vb, &ve, 1, 1, slot_res,
                out_res, 0, widths[w], R300_R2VB_POSITION_WINDOW, &txn) &&
            r300_r2vb_producer_bo_draw_stage_cs(r300, &txn, &plan, &fs, &rs,
                                                &psc) &&
            r300_r2vb_producer_bo_draw_emit(r300, &txn);
        fprintf(stderr,
                "r2vb_bo_draw_capture width=%u ok=%d cs_dwords=%u "
                "slot_reloc=%d model_reloc=%d\n",
                widths[w], ok, r300_r2vb_producer_bo_draw_cs_dwords(),
                txn.slot_reloc_index, txn.model_reloc_index);
        r300_r2vb_producer_bo_draw_fini(&txn);
        /* Discard the capture IB before DRM_RADEON_CS. */
        r300->rws->cs_flush(&r300->cs, RADEON_FLUSH_NOOP, NULL);
        if (!ok)
            break;
    }

    r300->fb_state.dirty = saved_fb_dirty;
    r300->aa_state.dirty = saved_aa_dirty;
    r300->textures_state.dirty = saved_tex_dirty;
    r300->first_dirty = saved_first;
    r300->last_dirty = saved_last;
    r300->fb_state.state = saved_fb;
    pipe_resource_reference(&slot_res, NULL);
    pipe_resource_reference(&model_res, NULL);
    pipe_resource_reference(&out_res, NULL);
    fprintf(stderr, "r2vb_bo_draw_capture done (no-submit; RADEON_FLUSH_NOOP)\n");
    return true;
}

/* The materialization helper joins the emission arm with the LOAD_VBPNTR
 * producer draw; until that arm lands the reference below keeps the
 * function in the translation unit's live set for the calibration test. */
bool
r300_r2vb_materialize_model_fetch_for_test(
    struct r300_context *r300, const struct pipe_vertex_buffer *vb,
    const struct pipe_vertex_element *ve, uint32_t start, uint32_t count,
    const struct r300_r2vb_producer_stream *model_stream,
    struct r300_r2vb_model_fetch *out)
{
    return r300_r2vb_materialize_model_fetch(r300, vb, ve, start, count,
                                             model_stream, out);
}

bool r300_r2vb_producer_fetch_init(const struct r300_r2vb_producer_streams *s,
                                   uint32_t count, uint64_t slot_bo_bytes,
                                   uint64_t model_bo_bytes,
                                   struct r300_r2vb_producer_fetch *out)
{
    if (!s || s->num != 2 || count == 0 || count >= 65536)
        return false;
    const struct r300_r2vb_producer_stream *slot = &s->stream[0];
    const struct r300_r2vb_producer_stream *model = &s->stream[1];
    /* The emission object is self-authenticating: it re-proves the whole
     * tuple instead of trusting that the normal builder produced it.  The
     * slot stream is the fixed FP32x4 form at offset zero; the model
     * stream is one of the two admitted physical families; and the fetch
     * total derives from the record widths rather than being copied from
     * a redundant field. */
    if (slot->offset_bytes != 0 || slot->size_dwords != 4 ||
        slot->stride_dwords < slot->size_dwords ||
        slot->logical_components != 4)
        return false;
    if ((model->size_dwords != 3 && model->size_dwords != 4) ||
        model->logical_components != 4)
        return false;
    if (model->offset_bytes % 4 != 0)
        return false;
    if (s->fetch_dwords != slot->size_dwords + model->size_dwords)
        return false;
    /* A stride under one record would overlap fetches; the anti-overlap
     * rule follows the stream's own record width, so the packed FLOAT_3
     * stride of 3 dwords is legal. */
    if (model->stride_dwords < model->size_dwords ||
        model->stride_dwords > R300_R2VB_VBPNTR_STRIDE_DWORDS_MAX ||
        slot->stride_dwords > R300_R2VB_VBPNTR_STRIDE_DWORDS_MAX)
        return false;
    uint64_t slot_end = (uint64_t)slot->offset_bytes +
                        (uint64_t)(count - 1) * slot->stride_dwords * 4 +
                        slot->size_dwords * 4;
    uint64_t model_end = (uint64_t)model->offset_bytes +
                         (uint64_t)(count - 1) * model->stride_dwords * 4 +
                         model->size_dwords * 4;
    if (slot_end > slot_bo_bytes || model_end > model_bo_bytes)
        return false;
    out->streams = *s;
    out->vap_vtx_size = slot->size_dwords + model->size_dwords;
    out->vf_min = 0;
    out->vf_max = count - 1;
    out->slot_required_bytes = slot_end;
    out->model_required_bytes = model_end;
    return true;
}

bool r300_r2vb_producer_streams_init(uint32_t buffer_offset,
                                     uint32_t src_offset,
                                     uint32_t src_stride_bytes,
                                     enum pipe_format format, uint32_t start,
                                     struct r300_r2vb_producer_streams *out)
{
    /* The observed dominant workload feeds a tightly packed FLOAT_3
     * position stream (record 12 bytes); the PSC swizzle synthesizes W
     * from FP_ONE, reproducing the immediate path's (x,y,z) -> (x,y,z,1)
     * convention without padded uploads. */
    uint32_t record_dwords;
    switch (format) {
    case PIPE_FORMAT_R32G32B32A32_FLOAT:
        record_dwords = 4;
        break;
    case PIPE_FORMAT_R32G32B32_FLOAT:
        record_dwords = 3;
        break;
    default:
        return false;
    }
    /* The LOAD_VBPNTR format word carries the stride in dwords, and a
     * stride under one record would overlap fetches. */
    if (src_stride_bytes % 4 != 0 || src_stride_bytes < record_dwords * 4)
        return false;
    uint64_t model_off = (uint64_t)buffer_offset + src_offset +
                         (uint64_t)start * src_stride_bytes;
    if (model_off > UINT32_MAX)
        return false;
    struct r300_r2vb_producer_streams s = {
        .num = 2,
        .stream = {
            /* Slot positions: FP32x4 from slot zero regardless of the
             * draw's start -- the producer always writes slots 0..count-1. */
            { .offset_bytes = 0, .stride_dwords = 4, .size_dwords = 4,
              .logical_components = 4 },
            { .offset_bytes = (uint32_t)model_off,
              .stride_dwords = src_stride_bytes / 4,
              .size_dwords = record_dwords,
              .logical_components = 4 },
        },
    };
    s.fetch_dwords = s.stream[0].size_dwords + s.stream[1].size_dwords;
    *out = s;
    return true;
}

bool r300_r2vb_slot_layout_init(uint32_t count, bool grid_enabled,
                                struct r300_r2vb_slot_layout *out)
{
    /* The 16-bit VAP_VF_MAX_VTX_INDX bounds every re-ingest regardless of
     * producer storage. */
    if (count == 0 || count >= 65536)
        return false;
    struct r300_r2vb_slot_layout l = { .count = count };
    if (count <= 4096) {
        /* The proven one-row shape, byte-for-byte, whether or not the grid
         * gate is armed: the first silicon comparison isolates only the new
         * multirow mechanism. */
        l.width = count;
        l.height = 1;
    } else {
        if (!grid_enabled)
            return false;
        l.width = 2048;
        l.height = (count + 2047u) / 2048u;
    }
    l.pitch_pixels = l.width;
    l.storage_slots = (uint64_t)l.pitch_pixels * l.height;
    l.storage_bytes = l.storage_slots * 16u;
    if (l.storage_slots < count || l.pitch_pixels != l.width)
        return false;
    *out = l;
    return true;
}

/* Strict positive decimal uint32: bare digits only.  Sign, whitespace,
 * trailing characters, overflow, and zero all fail, keeping the canary
 * closed on any malformed floor. */
bool r300_r2vb_auto_single_floor_value(const char *value, uint32_t *floor)
{
    if (!value || !*value)
        return false;
    uint64_t v = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > UINT32_MAX)
            return false;
    }
    if (v == 0)
        return false;
    *floor = (uint32_t)v;
    return true;
}

const char *
r300_r2vb_auto_single_reason_str(enum r300_r2vb_auto_single_reason reason)
{
    static const char *names[R300_R2VB_AUTO_SINGLE_REASON_COUNT] = {
        "ok",
        "indexed",
        "instanced",
        "unsupported_primitive",
        "count_ceiling",
        "frontface",
        "clip_planes",
        "fs_external_constants",
        "plan_not_ready",
        "plan_not_single",
        "typed_source",
        "input_shape",
        "delivery_cell",
        "below_vertex_floor",
    };
    return reason < R300_R2VB_AUTO_SINGLE_REASON_COUNT ? names[reason]
                                                       : "unknown";
}

/* One delivery cell of the plain route: READY SINGLE untyped one-input. */
static bool
r2vb_auto_single_cell_ok(const struct r300_r2vb_producer_plan *plan,
                         enum r300_r2vb_auto_single_reason *reason)
{
    if (!plan || plan->status != R300_R2VB_PLAN_READY) {
        *reason = R300_R2VB_AUTO_SINGLE_PLAN_NOT_READY;
        return false;
    }
    if (plan->action != R300_R2VB_PLAN_SINGLE) {
        *reason = R300_R2VB_AUTO_SINGLE_PLAN_NOT_SINGLE;
        return false;
    }
    if (plan->has_typed_source) {
        *reason = R300_R2VB_AUTO_SINGLE_TYPED_SOURCE;
        return false;
    }
    if (plan->num_position_inputs != 1) {
        *reason = R300_R2VB_AUTO_SINGLE_INPUT_SHAPE;
        return false;
    }
    return true;
}

enum r300_r2vb_auto_single_reason
r300_r2vb_auto_single_policy(const struct r300_r2vb_producer_plan *clip_plan,
                             const struct r300_r2vb_producer_plan *window_plan,
                             const struct r300_r2vb_auto_single_draw *d,
                             uint32_t floor)
{
    /* Route-support shape first: the clip-route delivery acts on a plain
     * whole-triangle list only. */
    if (d->index_size != 0)
        return R300_R2VB_AUTO_SINGLE_INDEXED;
    if (d->instance_count != 1)
        return R300_R2VB_AUTO_SINGLE_INSTANCED;
    if (d->mode != MESA_PRIM_TRIANGLES || d->count % 3 != 0)
        return R300_R2VB_AUTO_SINGLE_UNSUPPORTED_PRIMITIVE;
    /* The producer's slot-pixel stream renders one point per output slot on
     * a single row (r300_r2vb_get_slot_pos_bo), so a deliverable draw tops
     * out at the producer's 4096-slot ceiling -- far below the 16-bit
     * re-ingest index limit.  An admitted count past this ceiling would
     * decline inside the producer and fall back to gallivm after the
     * decision token already read "execute", so the policy holds the real
     * ceiling; raising it is the producer's 2D slot-layout reshape. */
    if (d->count == 0 || d->count > 4096)
        return R300_R2VB_AUTO_SINGLE_COUNT_CEILING;
    if (d->fs_reads_face)
        return R300_R2VB_AUTO_SINGLE_FRONTFACE;
    if (d->clip_planes_enabled)
        return R300_R2VB_AUTO_SINGLE_CLIP_PLANES;
    /* The producer pass overwrites FS constant file 0. */
    if (d->fs_reads_external_constants)
        return R300_R2VB_AUTO_SINGLE_FS_EXTERNAL_CONSTANTS;
    /* Delivery cells of the plain route: cv=0 clip classifies, cv=0 window
     * delivers on accept.  A window-cell failure past a good clip cell is
     * the delivery-cell decline. */
    enum r300_r2vb_auto_single_reason reason;
    if (!r2vb_auto_single_cell_ok(clip_plan, &reason))
        return reason;
    if (!r2vb_auto_single_cell_ok(window_plan, &reason))
        return R300_R2VB_AUTO_SINGLE_DELIVERY_CELL;
    /* The floor separates the amortizing large-draw class from the tiny-draw
     * tail that gallivm serves better; instance_count == 1 above, so count
     * is the submitted vertex total. */
    if (d->count < floor)
        return R300_R2VB_AUTO_SINGLE_BELOW_VERTEX_FLOOR;
    return R300_R2VB_AUTO_SINGLE_OK;
}

/* Canary arming (both gates cached once): exact "1" plus a valid floor. */
static bool r300_r2vb_auto_single_armed(uint32_t *floor_out)
{
    static int armed = -1;
    static uint32_t floor;
    if (armed < 0) {
        uint32_t f = 0;
        armed = (r300_r2vb_auto_single_gate_value(
                     getenv("R300_R2VB_AUTO_SINGLE")) &&
                 r300_r2vb_auto_single_floor_value(
                     getenv("R300_R2VB_AUTO_SINGLE_MIN_VERTICES"), &f))
                    ? 1
                    : 0;
        floor = f;
    }
    if (floor_out)
        *floor_out = floor;
    return armed == 1;
}

/* Diagnostic typed-split gate (R300_R2VB_TYPED_SPLIT): the exact value 1
 * opens the plan-driven typed route; unset, empty, and every other value
 * keep classification and execution byte-identical to the gate-off path. */
static bool r300_r2vb_typed_split_enabled(void)
{
    static int mode = -1;
    if (mode < 0)
        mode = r300_r2vb_typed_split_gate_value(
                   getenv("R300_R2VB_TYPED_SPLIT"))
                   ? 1
                   : 0;
    return mode == 1;
}

/* The typed diagnostic route validates typed carry transport, so its cells
 * must actually carry a typed value: a producer whose typed computation
 * converts to float before the selected cut leaves a float-only carry, and
 * executing it would prove nothing about SINT/UINT/BOOL transport. */
static bool
r300_r2vb_partition_has_typed_transport(const struct r300_mp_partition *p)
{
    for (unsigned i = 0; i < p->num_bases; i++) {
        switch (p->r2vb_transport[i]) {
        case R300_MP_R2VB_SINT:
        case R300_MP_R2VB_UINT:
        case R300_MP_R2VB_BOOL1:
        case R300_MP_R2VB_BOOL32:
            return true;
        default:
            break;
        }
    }
    return false;
}

const char *
r300_r2vb_typed_split_contract(const struct r300_r2vb_producer_plan *plan,
                               bool allow_computed_varying,
                               enum r300_r2vb_position_space space,
                               unsigned num_position_inputs)
{
    if (allow_computed_varying)
        return "computed_varying_cell";
    if (!plan)
        return "plan_transient";
    if (plan->status != R300_R2VB_PLAN_READY)
        return "plan_not_ready";
    if (plan->action != R300_R2VB_PLAN_SPLIT)
        return "plan_not_split";
    /* The contract defends its own authority: the plan cell must be the one
     * the caller is deciding, so a cache-keying defect surfaces as a decline
     * instead of executing a plan measured for a different cell. */
    if (plan->key.allow_computed_varying || plan->key.space != space)
        return "plan_key_mismatch";
    if (!plan->has_typed_source)
        return "typed_source_absent";
    if (!r300_r2vb_partition_has_typed_transport(&plan->partition))
        return "typed_carry_absent";
    if (plan->key.input_semantics != R300_FS_INPUT_R2VB_FLAT_VERTEX)
        return "input_semantics";
    if (plan->partition.total_comps == 0 || plan->partition.total_comps > 4)
        return "carry_width";
    /* Pass B feeds the planned model-attribute arity; the caller's count must
     * match or the executed program diverges from the measured one. */
    if (num_position_inputs != plan->num_position_inputs)
        return "input_count_mismatch";
    if (!plan->candidate)
        return "candidate_absent";
    return NULL;
}

void
r300_r2vb_typed_split_note_format(const struct r300_r2vb_producer_plan *plan,
                                  enum r300_r2vb_position_space space,
                                  const char *decline, char *buf, size_t len)
{
    static const char *const typed_names[] = { "none", "bool", "sint",
                                               "uint" };
    static_assert(ARRAY_SIZE(typed_names) == R300_R2VB_TYPED_SOURCE_UINT + 1,
                  "typed_names must cover r300_r2vb_typed_source_class");
    /* The carry letters come from the selected r2vb_transport, the same
     * encoding the EXEC_DEBUG split trace prints (f float, i signed,
     * u unsigned, b boolean), so the T3 unsigned boundary cell is
     * distinguishable from signed transport in the token alone. */
    char carry_types[R300_MP_MAX_CARRY_COMPS + 1] = "-";
    unsigned cut = 0, comps = 0;
    if (plan && plan->action == R300_R2VB_PLAN_SPLIT) {
        r300_r2vb_carry_types_str(&plan->partition, carry_types,
                                  sizeof(carry_types));
        cut = plan->partition.cut_index;
        comps = plan->partition.total_comps;
    }
    snprintf(buf, len,
             "r2vb_typed_route gate=1 plan_status=%s plan_action=%s space=%s "
             "typed_source=%s carry_types=%s carry_components=%u cut=%u "
             "passA=%u/%u/%u passB=%u/%u/%u decision=%s decline_reason=%s",
             !plan                                            ? "transient"
             : plan->status == R300_R2VB_PLAN_READY           ? "ready"
             : plan->status == R300_R2VB_PLAN_SEMANTIC_REJECT ? "semantic_reject"
             : plan->status == R300_R2VB_PLAN_POLICY_REJECT   ? "policy_reject"
                                                              : "transient_failure",
             plan ? r300_r2vb_plan_action_str(plan->action) : "-",
             space == R300_R2VB_POSITION_WINDOW ? "window" : "clip",
             plan ? typed_names[plan->typed_source_class] : "-", carry_types,
             comps, cut, plan ? plan->pass_a_cost.alu : 0,
             plan ? plan->pass_a_cost.temps : 0,
             plan ? plan->pass_a_cost.consts : 0,
             plan ? plan->pass_b_cost.alu : 0,
             plan ? plan->pass_b_cost.temps : 0,
             plan ? plan->pass_b_cost.consts : 0,
             decline ? "decline" : "execute", decline ? decline : "none");
}

/* One diagnostic token line per gated typed-route decision, so a gallivm
 * fallback is never mistaken for typed execution.  Prints whenever the typed
 * gate is open, on the cold once-per-cell classification path. */
static void
r300_r2vb_typed_split_note(const struct r300_r2vb_producer_plan *plan,
                           enum r300_r2vb_position_space space,
                           const char *decline)
{
    char line[512];
    r300_r2vb_typed_split_note_format(plan, space, decline, line,
                                      sizeof(line));
    fprintf(stderr, "%s\n", line);
}

enum r300_r2vb_admission_memo
r300_r2vb_plan_effective_admission(const struct r300_r2vb_producer_plan *plan,
                                   enum r300_r2vb_memo_writer writer,
                                   bool budget_escape_enabled,
                                   bool allow_computed_varying,
                                   enum r300_r2vb_position_space space,
                                   unsigned num_position_inputs)
{
    /* The typed diagnostic writer records exactly what its contract admits:
     * the typed gate never widens or narrows the legacy mapping, and the
     * spill1 gate never overrides a typed-contract decline, so the two gates
     * compose deterministically -- a typed-arm cell the contract declines
     * stays a reject (gallivm fallback) even with spill1 armed. */
    if (writer == R300_R2VB_MEMO_WRITER_TYPED_DIAGNOSTIC)
        return r300_r2vb_typed_split_contract(plan, allow_computed_varying,
                                              space, num_position_inputs)
                   ? R300_R2VB_ADMIT_REJECT
                   : R300_R2VB_ADMIT_SPLIT;
    /* Legacy float writer: a SPLIT plan is effective only under the spill1
     * budget-escape gate; ungated it collapses to the same reject the memo
     * records. */
    switch (plan->action) {
    case R300_R2VB_PLAN_SINGLE:
        return R300_R2VB_ADMIT_FITS;
    case R300_R2VB_PLAN_SPLIT:
        return budget_escape_enabled ? R300_R2VB_ADMIT_SPLIT
                                     : R300_R2VB_ADMIT_REJECT;
    default:
        return R300_R2VB_ADMIT_REJECT;
    }
}

static void r300_r2vb_plan_shadow_check(struct r300_context *r300,
                                        bool allow_computed_varying,
                                        enum r300_r2vb_position_space space,
                                        uint8_t memo,
                                        enum r300_r2vb_memo_writer writer)
{
    const struct r300_r2vb_producer_plan *plan =
        r300_r2vb_producer_plan_get(r300, allow_computed_varying, space);
    if (!plan)
        return; /* infrastructure failure; a later call replans */
    /* The memo decision point classifies each cell exactly once, so the
     * standing-route telemetry records here. */
    r300_r2vb_telemetry_note(r300, plan);
    uint8_t effective = r300_r2vb_plan_effective_admission(
        plan, writer, r300_r2vb_budget_escape_enabled(),
        allow_computed_varying, space,
        r300_r2vb_count_position_inputs(r300_vs(r300)->state.ir.nir));
    if (effective != memo) {
        /* A divergence is a planner defect finding, never an application
         * abort: the memo stays authoritative, the counter records the
         * event for the planner test and telemetry, and the print rides an
         * exact opt-in gate. */
        r300_r2vb_plan_note_shadow_divergence();
        static int dbg = -1;
        if (dbg < 0) {
            const char *e = getenv("R300_R2VB_PLAN_DEBUG");
            dbg = (e && strcmp(e, "1") == 0) ? 1 : 0;
        }
        if (dbg)
            fprintf(stderr,
                    "r2vb_plan shadow mismatch: memo=%u plan=%s/%s "
                    "mask=0x%" PRIx64 " space=%s cv=%d\n",
                    memo, r300_r2vb_plan_action_str(plan->action),
                    r300_r2vb_plan_reason_str(plan->primary_reason),
                    plan->observed_reason_mask,
                    space == R300_R2VB_POSITION_WINDOW ? "window" : "clip",
                    allow_computed_varying);
    }
}

static bool r300_r2vb_producer_fits_budget(struct r300_context *r300,
                                           bool allow_computed_varying,
                                           enum r300_r2vb_position_space space)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    unsigned space_i = space == R300_R2VB_POSITION_WINDOW ? 1u : 0u;
    uint8_t *memo = &vs->r2vb_admission[allow_computed_varying ? 1 : 0][space_i];
    if (*memo)
        return *memo != R300_R2VB_ADMIT_REJECT;

    unsigned alu = 0;
    enum r300_fs_admission adm =
        r300_r2vb_measure_pass(r300, vs->state.ir.nir, VARYING_SLOT_POS,
                               "position", space, &alu);
    bool fits = adm == R300_FS_ADMIT_FITS;
    if (fits && allow_computed_varying) {
        int dv = r300_r2vb_first_computed_varying(vs->state.ir.nir);
        if (dv >= 0)
            fits = r300_r2vb_measure_pass_fits(r300, vs->state.ir.nir,
                                               (gl_varying_slot)dv, "varying",
                                               space);
    }
    /* R300_R2VB_FORCE_SPLIT requests the split producer for a fitting position
     * pass when the spill1 gate is enabled. A declined split keeps the normal
     * single-pass admission.  Probe the same `space` the producer will emit. */
    if (fits) {
        static int force_split = -1;
        if (force_split < 0) {
            const char *e = getenv("R300_R2VB_FORCE_SPLIT");
            force_split = (e && strcmp(e, "1") == 0) ? 1 : 0;
        }
        if (force_split && !allow_computed_varying &&
            r300_r2vb_budget_escape_enabled()) {
            unsigned num_in = r300_r2vb_count_position_inputs(vs->state.ir.nir);
            if (r300_r2vb_split_admitted(r300, vs->state.ir.nir, num_in,
                                         space)) {
                fprintf(stderr,
                        "r2vb_force_split under_budget=1 space=%s admitted=1\n",
                        space_i ? "window" : "clip");
                *memo = R300_R2VB_ADMIT_SPLIT;
                return true;
            }
            fprintf(stderr,
                    "r2vb_force_split under_budget=1 space=%s admitted=0 "
                    "(split declined; single-pass kept)\n",
                    space_i ? "window" : "clip");
        }
        *memo = R300_R2VB_ADMIT_FITS;
        r300_r2vb_plan_shadow_check(r300, allow_computed_varying, space, *memo,
                                    R300_R2VB_MEMO_WRITER_LEGACY_FLOAT);
        return true;
    }

    /* Budget escape rides only the ALU-emit ceiling, never a structural reject,
     * and only the single-input-position pass (the computed-varying pass is
     * orthogonal and keeps the single-pass rule).  Gated off, this branch is
     * skipped and the over-budget verdict collapses to a reject, byte-identical
     * to the pre-split route. */
    if (adm == R300_FS_ADMIT_OVER_ALU_BUDGET && !allow_computed_varying &&
        r300_r2vb_budget_escape_enabled()) {
        unsigned num_in = r300_r2vb_count_position_inputs(vs->state.ir.nir);
        if (r300_r2vb_split_admitted(r300, vs->state.ir.nir, num_in, space)) {
            *memo = R300_R2VB_ADMIT_SPLIT;
            r300_r2vb_plan_shadow_check(r300, allow_computed_varying, space,
                                        *memo,
                                        R300_R2VB_MEMO_WRITER_LEGACY_FLOAT);
            return true;
        }
    }
    *memo = R300_R2VB_ADMIT_REJECT;
    r300_r2vb_plan_shadow_check(r300, allow_computed_varying, space, *memo,
                                R300_R2VB_MEMO_WRITER_LEGACY_FLOAT);
    return false;
}

/* Diagnostic typed-split admission: the cached plan is the sole authority --
 * its split walk already proved the typed transport (carry range inside the
 * FP24 window, consistent signedness, one-vec4 crossing set) and compiled
 * both halves under the emit ceiling.  The verdict lands in the same per-VS
 * memo byte the float route reads, so execution reaches the split arm
 * through the established path, and the shadow check keeps auditing the
 * memo against the plan.  A transient plan (allocation) is never memoized;
 * the next request replans. */
static bool r300_r2vb_typed_split_admit(struct r300_context *r300,
                                        enum r300_r2vb_position_space space)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    unsigned space_i = space == R300_R2VB_POSITION_WINDOW ? 1u : 0u;
    uint8_t *memo = &vs->r2vb_admission[0][space_i];
    if (*memo)
        return *memo != R300_R2VB_ADMIT_REJECT;

    const struct r300_r2vb_producer_plan *plan =
        r300_r2vb_producer_plan_get(r300, false, space);
    unsigned num_in = r300_r2vb_count_position_inputs(vs->state.ir.nir);
    const char *decline =
        r300_r2vb_typed_split_contract(plan, false, space, num_in);
    r300_r2vb_typed_split_note(plan, space, decline);
    if (!plan)
        return false;
    *memo = decline ? R300_R2VB_ADMIT_REJECT : R300_R2VB_ADMIT_SPLIT;
    r300_r2vb_plan_shadow_check(r300, false, space, *memo,
                                R300_R2VB_MEMO_WRITER_TYPED_DIAGNOSTIC);
    return *memo != R300_R2VB_ADMIT_REJECT;
}

/* Per-cell producer admission for one position space: the constant-folded
 * structural scan decides the memo writer (float budget oracle for an
 * aluable VS, the typed diagnostic contract for a structurally rejected one),
 * exactly as the classify hook applies it.  A SPIR-V VS reaches the driver
 * with its UBO address arithmetic still literal (iadd/imul/ushr of
 * load_const values) and with ffma kept weak by the gallivm compiler
 * options; both fold or map to fragment-aluable form in the FS compile the
 * restage producer runs, so the scan must see the folded shader, not the
 * raw one.  Real (non-constant) integer arithmetic survives the folding and
 * still rejects.  The space parameter exists because a routed draw admits
 * per space: the clip-route delivery re-runs the producer in window space,
 * and the window cell of the same shader can decline where the clip cell
 * admitted (the window transform's extra ALU, or the typed contract on the
 * window candidate). */
static bool r300_vs_admits_producer(struct r300_context *r300,
                                    bool allow_computed_varying,
                                    enum r300_r2vb_position_space space)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return false;
    nir_shader *clone = nir_shader_clone(NULL, vs->state.ir.nir);
    if (!clone)
        return false;
    bool progress;
    do {
        progress = false;
        progress |= nir_opt_constant_folding(clone);
        progress |= nir_opt_dce(clone);
    } while (progress);
    bool ok = r300_vs_nir_is_fragment_aluable(clone, allow_computed_varying);
    ralloc_free(clone);
    /* Structure admitted; the budget verdict comes from the emitted-slot
     * oracle on the derived producer FS, memoized per VS and position space. */
    if (ok)
        ok = r300_r2vb_producer_fits_budget(r300, allow_computed_varying,
                                            space);
    /* The float whitelist rejects every typed producer before the budget
     * oracle runs; the diagnostic typed gate re-asks the cached plan, whose
     * split walk carries the typed transport admission (range, signedness,
     * one-vec4 carry).  Gated off, the structural reject stands unchanged. */
    else if (!allow_computed_varying && r300_r2vb_typed_split_enabled())
        ok = r300_r2vb_typed_split_admit(r300, space);
    return ok;
}

static bool r300_vs_is_fragment_aluable(struct r300_context *r300,
                                        bool allow_computed_varying)
{
    return r300_vs_admits_producer(r300, allow_computed_varying,
                                   r2vb_env_space());
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
     * linear vertex list; an index buffer would need an index-aware producer.
     * The gated topology gather (R300_R2VB_TOPOLOGY, requires
     * R300_R2VB_CLIP_ROUTE) resolves an indexed triangle-family draw to a
     * plain triangle list on the MVP/clip route only.  The pure passthrough
     * path still emits draw_arrays without resolving indices, so indexed
     * identity-VS draws are rejected below even when topology is enabled. */
    if (info->index_size != 0 && !r300_r2vb_topology_enabled())
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
    /* gl_FrontFacing is delivered by a CPU draw-module stage (the unfilled stage
     * computes the per-triangle face). The re-ingest runs at TCL_BYPASS and skips
     * the draw module entirely, so it cannot carry the face -- decline and let
     * the draw fall back to gallivm. */
    if (r300_fs(r300)->shader->inputs.face != ATTR_UNUSED)
        return R2VB_REJECT_FRONTFACE;
    /* Identity VS: re-ingest the app buffers at TCL_BYPASS only for non-indexed
     * lists/points/lines.  Indexed and strip/fan shapes need topology expand,
     * which lives on the MVP producer path -- reject them from pure
     * passthrough so R300_R2VB_EXEC cannot emit draw_arrays over raw indices. */
    if (r300_vs_is_passthrough(r300)) {
        if (info->index_size != 0)
            return R2VB_REJECT_INDEXED;
        if (info->mode == MESA_PRIM_TRIANGLE_STRIP ||
            info->mode == MESA_PRIM_TRIANGLE_FAN)
            return R2VB_REJECT_PRIM;
        return R2VB_ROUTE_PASSTHROUGH;
    }
    return R2VB_ROUTE_CANDIDATE;
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
    /* Telemetry-only observation: with the route gate closed and the
     * telemetry print gate or retain directory armed, the standing workload
     * still classifies -- the plan cache measures each candidate cell once
     * and the telemetry note records and retains it -- and every draw then
     * declines, so gallivm renders exactly as with both gates closed.  The
     * gates are process-constant, so the observation marker written into the
     * admission memo below is never consulted by the routing path. */
    static int observe = -1;
    if (observe < 0)
        observe = r300_r2vb_telemetry_observation_enabled() ? 1 : 0;
    if (!enabled && !observe)
        return false;

    static unsigned tally[R2VB_VERDICT_COUNT];
    static unsigned total;
    enum r300_r2vb_verdict v = r300_r2vb_classify_draw(r300, info, draw);
    tally[v]++;
    total++;

    /* Workload accounting runs on every candidate draw, not only the
     * once-per-cell classification: the plan lookup is a cache hit after
     * the first draw, and the accumulator turns cell incidence into the
     * dynamic weight (draws, vertices, topology) a route-on policy needs. */
    if (observe && v == R2VB_ROUTE_CANDIDATE) {
        struct r300_vertex_shader *vs = r300_vs(r300);
        if (vs && vs->state.type == PIPE_SHADER_IR_NIR && vs->state.ir.nir) {
            enum r300_r2vb_position_space space = r2vb_env_space();
            unsigned space_i =
                space == R300_R2VB_POSITION_WINDOW ? 1u : 0u;
            uint8_t *memo = &vs->r2vb_admission[0][space_i];
            if (!enabled && *memo == R300_R2VB_ADMIT_UNMEASURED) {
                const struct r300_r2vb_producer_plan *plan =
                    r300_r2vb_producer_plan_get(r300, false, space);
                if (plan) {
                    r300_r2vb_telemetry_note(r300, plan);
                    *memo = R300_R2VB_ADMIT_REJECT;
                }
                /* Observation-only input facts, once per cell measure: the
                 * BO-fetch contract admits by element format, stride, and
                 * resource extent, none of which the retained NIR carries,
                 * so the record decides which format family the fetch path
                 * implements next.  No routing, no GPU change. */
                if (r300->velems && r300->velems->count > 0) {
                    const struct pipe_vertex_element *pe =
                        &r300->velems->velem[0];
                    const struct pipe_vertex_buffer *vb =
                        &r300->vertex_buffer[pe->vertex_buffer_index];
                    struct pipe_resource *res =
                        vb->is_user_buffer ? NULL : vb->buffer.resource;
                    fprintf(stderr,
                            "r2vb_producer_input hash=%s input=0 format=%s"
                            " format_dwords=%u stride_bytes=%u"
                            " buffer_offset=%u src_offset=%u draw_start=%u"
                            " model_offset=%" PRIu64 " resource_width=%u"
                            " bo_materialized=%d\n",
                            r300_r2vb_telemetry_vs_content_hex(r300),
                            util_format_short_name(pe->src_format),
                            util_format_get_blocksize(pe->src_format) / 4,
                            pe->src_stride, vb->buffer_offset,
                            pe->src_offset, draw->start,
                            (uint64_t)vb->buffer_offset + pe->src_offset +
                                (uint64_t)draw->start * pe->src_stride,
                            res ? res->width0 : 0,
                            res && r300_resource(res)->buf ? 1 : 0);
                }
            }
            const struct r300_r2vb_producer_plan *plan =
                r300_r2vb_producer_plan_get(r300, false, space);
            if (plan)
                r300_r2vb_telemetry_draw(r300, plan, info, draw);
        }
    }
    if (!enabled)
        return false;

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
            "passthrough", "candidate", "hw_tcl", "indexed", "instanced", "count",
            "prim", "frontface"};
        fprintf(stderr, "r2vb_route_draw #%u verdict=%s is_mvp=%d vs=%s count=%u mode=%u\n",
                total, vname[v], r300_vs_is_mvp(r300), vsname, draw->count, info->mode);
    }

    /* R300_R2VB_VS_DUMP emits one NIR-shape dump of the bound VS. The MVP
     * matcher reads that bound NIR state rather than a nir_to_tgsi round trip.
     * No submit. */
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
            /* Classifier decision + intermediates for the bound VS, no submit, so a
             * route that engages on hardware can be predicted from the safe dump
             * path instead of a hanging draw. */
            {
                nir_shader *dn = r300_vs(r300)->state.ir.nir;
                const char *vev = getenv("R300_R2VB_VARYING");
                bool av = vev && strcmp(vev, "1") == 0;
                fprintf(stderr,
                        "r2vb_classify_diag first_computed_varying=%d count_pos_inputs=%u "
                        "aluable[varying=%d]=%d aluable[no_varying]=%d\n",
                        r300_r2vb_first_computed_varying(dn),
                        r300_r2vb_count_position_inputs(dn),
                        av, r300_vs_is_fragment_aluable(r300, av),
                        r300_vs_is_fragment_aluable(r300, false));
            }
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
                "indexed=%u instanced=%u count=%u prim=%u frontface=%u\n",
                total, tally[R2VB_ROUTE_PASSTHROUGH], tally[R2VB_ROUTE_CANDIDATE],
                tally[R2VB_REJECT_HW_TCL], tally[R2VB_REJECT_INDEXED],
                tally[R2VB_REJECT_INSTANCED], tally[R2VB_REJECT_COUNT],
                tally[R2VB_REJECT_PRIM], tally[R2VB_REJECT_FRONTFACE]);

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
    uint32_t auto_floor = 0;
    bool auto_armed = r300_r2vb_auto_single_armed(&auto_floor);
    if (!gate && !auto_armed)
        return false;
    if (r300_r2vb_classify_draw(r300, info, draw) != R2VB_ROUTE_CANDIDATE)
        return false;
    /* AUTO_SINGLE canary: the pure policy decides, one token per decision,
     * and the manual gates keep their behavior when the canary declines
     * (both armed together lets the manual battery drive the canary image). */
    if (auto_armed) {
        struct r300_vertex_shader *avs = r300_vs(r300);
        if (!avs || avs->state.type != PIPE_SHADER_IR_NIR ||
            !avs->state.ir.nir)
            return false;
        const struct r300_rs_state *rs =
            (const struct r300_rs_state *)r300->rs_state.state;
        const struct r300_fragment_shader *afs = r300_fs(r300);
        bool fs_ext = false;
        if (afs && afs->shader) {
            const struct rc_constant_list *cl = &afs->shader->code.constants;
            for (unsigned i = 0; i < cl->Count; i++)
                if (cl->Constants[i].Type == RC_CONSTANT_EXTERNAL)
                    fs_ext = true;
        }
        struct r300_r2vb_auto_single_draw d = {
            .mode = info->mode,
            .count = draw->count,
            .instance_count = info->instance_count,
            .index_size = info->index_size,
            .fs_reads_face = afs && afs->shader &&
                             afs->shader->inputs.face != ATTR_UNUSED,
            .clip_planes_enabled = rs && rs->rs.clip_plane_enable != 0,
            .fs_reads_external_constants = fs_ext,
        };
        const struct r300_r2vb_producer_plan *cp =
            r300_r2vb_producer_plan_get(r300, false, R300_R2VB_POSITION_CLIP);
        const struct r300_r2vb_producer_plan *wp =
            r300_r2vb_producer_plan_get(r300, false,
                                        R300_R2VB_POSITION_WINDOW);
        enum r300_r2vb_auto_single_reason reason =
            r300_r2vb_auto_single_policy(cp, wp, &d, auto_floor);
        fprintf(stderr,
                "r2vb_auto_single gate=1 hash=%s submitted_vertices=%" PRIu64
                " threshold=%u topology=%u indexed=%u"
                " plan_clip=%s/%u/%u/%u plan_window=%s/%u/%u/%u"
                " decision=%s reason=%s\n",
                r300_r2vb_telemetry_vs_content_hex(r300),
                (uint64_t)draw->count * info->instance_count, auto_floor,
                info->mode, info->index_size ? 1 : 0,
                cp ? r300_r2vb_plan_action_str(cp->action) : "-",
                cp ? cp->baseline.alu : 0, cp ? cp->baseline.temps : 0,
                cp ? cp->baseline.consts : 0,
                wp ? r300_r2vb_plan_action_str(wp->action) : "-",
                wp ? wp->baseline.alu : 0, wp ? wp->baseline.temps : 0,
                wp ? wp->baseline.consts : 0,
                reason == R300_R2VB_AUTO_SINGLE_OK ? "execute" : "decline",
                r300_r2vb_auto_single_reason_str(reason));
        if (!gate)
            return reason == R300_R2VB_AUTO_SINGLE_OK;
    }
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
     * gallivm rather than rendering the wrong color from the application buffer). */
    static int varying = -1;
    if (varying < 0) {
        const char *e = getenv("R300_R2VB_VARYING");
        varying = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    /* Multi-input position needs the restaged producer (one feed slot per
     * input).  The hand-built MVP FS only reads velem[0]; without RESTAGE a
     * multi-input match would under-feed the transform.  classify_draw can
     * return CANDIDATE for a non-NIR VS (passthrough is false), so require
     * NIR before counting position inputs. */
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return false;
    unsigned npos = r300_r2vb_count_position_inputs(vs->state.ir.nir);
    if (npos > 1 && !restage)
        return false;
    bool al = restage ? r300_vs_is_fragment_aluable(r300, varying) : r300_vs_is_mvp(r300);
    if (getenv("R300_R2VB_EXEC_DEBUG"))
        fprintf(stderr,
                "r2vb_route_mvp gate=%d restage=%d varying=%d aluable=%d "
                "npos=%u count=%u\n",
                gate, restage, varying, al, npos, draw->count);
    return al;
}

/* Re-ingest a computed-varying draw by pointing each VS output's vertex stream at
 * its producer BO: position from the clip BO, the computed varying from vbo.  This
 * is the delivery half of the general fragment-ALU vertex route.  The producer
 * pass has already rendered both outputs into their BOs (the BO oracle is the
 * standing proof that vbo holds the right values); here the re-ingest fetches them
 * at TCL_BYPASS instead of falling back to gallivm.
 *
 * The SWTCL layout drives two independent stream sets that must agree: the PSC
 * (one stream per VS output, built from r300->vertex_info) and the LOAD_VBPNTR
 * arrays (one per vertex element, built from r300->velems).  The application
 * velems describe VS inputs, not outputs, so they are rebuilt to one element per
 * output -- velem[0] -> clip, velem[1] -> vbo -- each FP32x4.  The passthrough
 * emit derives each array's LOAD_VBPNTR SIZE from that format, so a wrong count or
 * format is a malformed fetch rather than a silent miss.  Minimal scope (one
 * computed varying, no passthrough) makes every reconstructed element point at a
 * producer BO, so the wiring is exhaustively "each velem -> a producer BO".
 *
 * The invariant -- velems->count == vinfo->num_attribs == 2, the position stream's
 * bound resource is clip, the varying stream's is vbo -- is a HARD submit gate,
 * not a diagnostic print: a velem/PSC mismatch hangs the draw and the timeout-kill
 * poisons the ring, so the submit is refused unless the wiring is the matched case.
 * R300_R2VB_INSPECT dumps the reconstructed routing and skips the submit (the
 * no-submit CS proof); otherwise one gated submit runs.  The full velem set and the
 * two buffer slots are saved and restored so a later draw inherits clean vertex
 * state.
 *
 * The framebuffer raster of the submitted draw stays cold-non-deterministic (the
 * re-ingest VAP-fetch cold hazard).  The wiring is proven instead by the no-submit
 * resource-pointer invariant and the PSC dump, plus the warm IB's LOAD_VBPNTR
 * array 1 binding vbo by relocation index (r300 has no per-BO GPU virtual address;
 * the command stream binds buffers by reloc).  The CS decode (the re-ingest fetches
 * from vbo) and the BO oracle (vbo holds the right values) prove different halves;
 * together they establish the re-ingest fetches correct computed-varying data
 * without depending on the raster. */
/* r300_r2vb_reingest_kind / r300_r2vb_reingest_stream live in r300_r2vb.h so the host
 * layout unit can enumerate against the same types. */

/* The app velem index feeding input var IN: its rank among the VS inputs in
 * ascending location order, because r300 binds velem[k] to the k-th input in that
 * order.  Location rank, not driver_location, because the bound VS arrives in
 * deref/variable form (r300_optimize_nir does not run nir_lower_io). */
static int r300_r2vb_input_velem_index(nir_shader *vs, const nir_variable *in)
{
    int rank = 0;
    nir_foreach_variable_with_modes(v, vs, nir_var_shader_in)
        if (v != in && v->data.location < in->data.location)
            rank++;
    return rank;
}

/* The rank oracle calls the mapper on named variables so the plan's
 * retained position_source and the element mapper stay one convention. */
int r300_r2vb_input_velem_index_for_test(nir_shader *vs,
                                         const nir_variable *in)
{
    return r300_r2vb_input_velem_index(vs, in);
}

/* PSC/VAP output-vector rank for a gl_varying_slot: mirrors
 * r300_draw_emit_all_attribs / r300_draw_fill_vs_outputs (POS, PSIZ, COL*,
 * BFC*, GENERIC*, FOG).  Numeric gl_varying_slot order places PSIZ after
 * colors and FOGC before PSIZ, so an ascending-slot sort miswires streams. */
static int
r300_r2vb_psc_output_rank(gl_varying_slot slot)
{
    if (slot == VARYING_SLOT_POS)
        return 0;
    if (slot == VARYING_SLOT_PSIZ)
        return 1;
    if (slot == VARYING_SLOT_COL0)
        return 2;
    if (slot == VARYING_SLOT_COL1)
        return 3;
    if (slot == VARYING_SLOT_BFC0)
        return 4;
    if (slot == VARYING_SLOT_BFC1)
        return 5;
    if (slot >= VARYING_SLOT_VAR0 && slot < VARYING_SLOT_VAR0 + 32)
        return 6 + (int)(slot - VARYING_SLOT_VAR0);
    if (slot == VARYING_SLOT_FOGC)
        return 6 + 32;
    if (slot == VARYING_SLOT_EDGE)
        return 6 + 33;
    if (slot == VARYING_SLOT_CLIP_VERTEX)
        return 6 + 34;
    /* Unknown fixed-function slots stay after the PSC core, still stable. */
    return 100 + (int)slot;
}

/* Contract comment at the declaration in r300_r2vb.h. */
int r300_r2vb_reingest_stream_layout(nir_shader *vs, int computed_slot,
                                     struct r300_r2vb_reingest_stream *out, unsigned max)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(vs);
    if (!impl)
        return -1;
    unsigned n = 0;
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
                continue;
            nir_variable *o = nir_intrinsic_get_var(intr, 0);
            if (!o || !(o->data.mode & nir_var_shader_out))
                continue;
            if (n >= max)
                return -1;
            struct r300_r2vb_reingest_stream *s = &out[n++];
            s->slot = (gl_varying_slot)o->data.location;
            s->src_velem = -1;
            if (o->data.location == VARYING_SLOT_POS) {
                s->kind = R2VB_STREAM_POS;
            } else if ((int)o->data.location == computed_slot) {
                s->kind = R2VB_STREAM_COMPUTED;
            } else {
                nir_intrinsic_instr *val = nir_src_as_intrinsic(intr->src[1]);
                nir_variable *src = (val && val->intrinsic == nir_intrinsic_load_deref)
                                        ? nir_intrinsic_get_var(val, 0) : NULL;
                if (!src || !(src->data.mode & nir_var_shader_in))
                    return -1; /* neither computed nor a clean input passthrough */
                s->kind = R2VB_STREAM_PASSTHROUGH;
                s->src_velem = r300_r2vb_input_velem_index(vs, src);
            }
        }
    }
    /* Sort into PSC/VAP output-vector order; insertion sort, n is tiny. */
    for (unsigned i = 1; i < n; i++) {
        struct r300_r2vb_reingest_stream key = out[i];
        int j = (int)i - 1;
        while (j >= 0 &&
               r300_r2vb_psc_output_rank(out[j].slot) >
                   r300_r2vb_psc_output_rank(key.slot)) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return (int)n;
}

/* Re-ingest the producer output as a draw whose vertex streams are routed per VS
 * output at TCL_BYPASS: position from the clip BO, the computed varying from vbo,
 * and every passthrough varying from its own application buffer.  The producer has
 * no BO for a passthrough (the application supplies it), so the proof here is
 * routing-correctness, not value-correctness: the no-submit R300_R2VB_INSPECT pass
 * verifies every stream by resource pointer, and the gated submit's CS-decode adds
 * the matching LOAD_VBPNTR relocation per stream.  The framebuffer raster is
 * cold-non-deterministic (the VAP-fetch cold hazard), so it is never the gate.
 * The full velem set and the two new buffer slots are saved and restored. */
/* Re-ingest a producer draw by rebuilding one vertex element per VS OUTPUT, in
 * output-vector order: position from the clip BO, the one computed varying (slot
 * vslot, vbo) if any, and each passthrough varying from its application buffer.
 * vslot < 0 (and vbo NULL) means there is no computed varying -- the position-only
 * and multi-input-position cases, where the extra position INPUTS the producer
 * consumed are simply not outputs and so never appear in the velem set.  Because
 * the velem set is built from outputs, not inputs, velems->count == num_attribs by
 * construction, and the hard invariant refuses anything else -- which is exactly
 * what makes the multi-input-position submit safe: the old path kept the extra
 * input velems as phantom streams (velems > outputs), the malformed fetch that
 * wedged the ring.  The caller decides which classes may submit; this routine only
 * builds the wiring and gates on the invariant. */

/* Re-emit application raster state after a producer hand-wrote registers
 * outside the atom system.  Every delivery decline that follows a successful
 * producer must call this before falling back to gallivm. */
static void
r2vb_redirty_app_raster_state(struct r300_context *r300)
{
    r300_mark_atom_dirty(r300, &r300->fb_state);
    r300_mark_atom_dirty(r300, &r300->scissor_state);
    r300_mark_atom_dirty(r300, &r300->viewport_state);
    r300_mark_atom_dirty(r300, &r300->dsa_state);
    r300_mark_atom_dirty(r300, &r300->rs_state);
}

/* Wait for GPU writes on a BO before a CPU read map.  r300_buffer_transfer_map
 * forces PIPE_MAP_UNSYNCHRONIZED on read maps, so flush alone does not order. */
static void
r2vb_wait_bo(struct r300_context *r300, struct pipe_resource *res)
{
    struct r300_resource *rbuf = r300_resource(res);
    if (rbuf && rbuf->buf)
        r300->rws->buffer_wait(r300->rws, rbuf->buf, OS_TIMEOUT_INFINITE,
                               RADEON_USAGE_WRITE);
}

static bool r300_r2vb_reingest_outputs(struct r300_context *r300,
                                       const struct pipe_draw_info *info,
                                       const struct pipe_draw_start_count_bias *draw,
                                       struct pipe_resource *clip,
                                       struct pipe_resource *vbo, int vslot)
{
    struct r300_vertex_element_state *ve = r300->velems;
    if (!ve || !r300_resource(clip)->buf || (vslot >= 0 && (!vbo || !r300_resource(vbo)->buf))) {
        r2vb_redirty_app_raster_state(r300);
        return false;
    }

    unsigned n_out = r300->vertex_info.num_attribs;

    /* r300 has no per-BO GPU virtual address -- the command stream binds buffers by
     * relocation index -- so BO identity here is the resource pointer, and in the
     * emitted IB the relocation index, not a VA. */
    if (r2vb_exec_debug_enabled())
        fprintf(stderr,
                "r2vb_reingest pre vinfo_num_attribs=%u app_velems_count=%u nvb=%u vslot=%d "
                "clip=%p vbo=%p\n",
                n_out, ve->count, r300->nr_vertex_buffers, vslot, (void *)clip, (void *)vbo);

    struct r300_r2vb_reingest_stream streams[PIPE_MAX_ATTRIBS];
    int n_stream = r300_r2vb_reingest_stream_layout(r300_vs(r300)->state.ir.nir, vslot,
                                                    streams, PIPE_MAX_ATTRIBS);

    unsigned clip_slot = r300->nr_vertex_buffers;
    unsigned vbo_slot = clip_slot + 1;
    /* Clip-edge replacement streams: each passthrough stream carrying an
     * interpolated CPU array gets its own fresh user-buffer slot after the
     * two producer slots. */
    unsigned n_edge = 0;
    if (r300->r2vb_edge_streams_active)
        for (int i = 0; i < n_stream; i++)
            if (streams[i].kind == R2VB_STREAM_PASSTHROUGH &&
                r300->r2vb_edge_stream_attr[i])
                n_edge++;
    if (vbo_slot + n_edge >= PIPE_MAX_ATTRIBS || n_stream < 1 ||
        (unsigned)n_stream != n_out || n_out > PIPE_MAX_ATTRIBS) {
        fprintf(stderr,
                "r2vb_reingest invariant=FAIL (stream_layout=%d vs num_attribs=%u; unmapped or count mismatch)\n",
                n_stream, n_out);
        r2vb_redirty_app_raster_state(r300);
        return false;
    }

    /* Snapshot the velems the rebuild overwrites (the n_out output slots) plus the
     * two new buffer slots: the passthrough emit rewrites format_size[] and
     * vertex_size_dwords, and the count/element grow must not leak.  A passthrough
     * stream's source is an existing application velem (saved here, read for its
     * buffer/offset/stride/format) whose buffer slot stays bound and is not
     * overwritten. */
    unsigned saved_count = ve->count;
    unsigned saved_vsd = ve->vertex_size_dwords;
    unsigned saved_nvb = r300->nr_vertex_buffers;
    unsigned saved_n = MAX2((unsigned)n_out, saved_count);
    struct pipe_vertex_element saved_velem[PIPE_MAX_ATTRIBS];
    unsigned saved_fmtsz[PIPE_MAX_ATTRIBS];
    for (unsigned i = 0; i < saved_n && i < PIPE_MAX_ATTRIBS; i++) {
        saved_velem[i] = ve->velem[i];
        saved_fmtsz[i] = ve->format_size[i];
    }
    unsigned n_fresh = 2 + n_edge;
    struct pipe_vertex_buffer saved_vb[PIPE_MAX_ATTRIBS];
    for (unsigned i = 0; i < n_fresh; i++)
        saved_vb[i] = r300->vertex_buffer[clip_slot + i];

    /* Capture each stream's expected source resource BEFORE the rebuild: position
     * -> clip, computed -> vbo, passthrough -> the app buffer its source velem
     * binds.  The invariant checks the rebuilt velem points at exactly this. */
    /* Bounds-gate every passthrough source before indexing saved_velem[].  A
     * passthrough's src_velem is the source input's location rank; if the VS
     * declares more inputs than the application bound velems for, it can exceed
     * the velem set and the lookup would read past the valid region.  Refuse with
     * a diagnostic rather than fetch garbage. */
    for (int i = 0; i < n_stream; i++)
        if (streams[i].kind == R2VB_STREAM_PASSTHROUGH &&
            (streams[i].src_velem < 0 || (unsigned)streams[i].src_velem >= saved_count)) {
            fprintf(stderr,
                    "r2vb_reingest invariant=FAIL (passthrough stream%d src_velem=%d out of bounds, app_velems=%u)\n",
                    i, streams[i].src_velem, saved_count);
            r2vb_redirty_app_raster_state(r300);
            return false;
        }

    struct pipe_resource *expect[PIPE_MAX_ATTRIBS];
    bool edge_stream[PIPE_MAX_ATTRIBS] = {0};
    for (int i = 0; i < n_stream; i++) {
        if (streams[i].kind == R2VB_STREAM_POS) {
            expect[i] = clip;
        } else if (streams[i].kind == R2VB_STREAM_COMPUTED) {
            expect[i] = vbo;
        } else if (r300->r2vb_edge_streams_active &&
                   r300->r2vb_edge_stream_attr[i]) {
            /* Interpolated replacement: a user buffer carries no resource;
             * the invariant for this stream is the user-pointer identity,
             * checked in the rebuild loop below. */
            expect[i] = NULL;
            edge_stream[i] = true;
        } else {
            struct pipe_vertex_buffer *src_vb =
                &r300->vertex_buffer[saved_velem[streams[i].src_velem].vertex_buffer_index];
            /* User buffers are not pipe_resources; refuse rather than treat
             * a CPU pointer as a BO (the passthrough emit uploads them). */
            if (src_vb->is_user_buffer) {
                fprintf(stderr,
                        "r2vb_reingest decline: passthrough stream%d is a user "
                        "vertex buffer; upload path required\n", i);
                r2vb_redirty_app_raster_state(r300);
                return false;
            }
            expect[i] = src_vb->buffer.resource;
        }
    }

    /* Reconstruct one velem per VS output, in output-vector order.  Position and
     * the computed varying get the two fresh FP32x4 slots (clip, vbo); each
     * passthrough reuses its application velem verbatim so it fetches the original
     * attribute from the application buffer the source input was bound to. */
    ve->count = n_out;
    for (unsigned i = 0; i < n_fresh; i++)
        memset(&r300->vertex_buffer[clip_slot + i], 0,
               sizeof(struct pipe_vertex_buffer));
    r300->vertex_buffer[clip_slot].buffer.resource = clip;
    r300->vertex_buffer[vbo_slot].buffer.resource = vbo;
    r300->nr_vertex_buffers = clip_slot + n_fresh;
    unsigned next_edge_slot = vbo_slot + 1;
    for (int i = 0; i < n_stream; i++) {
        if (streams[i].kind == R2VB_STREAM_POS) {
            ve->velem[i] = (struct pipe_vertex_element){ .vertex_buffer_index = clip_slot,
                .src_offset = 0, .src_stride = 16, .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT };
            ve->format_size[i] = 16;
        } else if (streams[i].kind == R2VB_STREAM_COMPUTED) {
            ve->velem[i] = (struct pipe_vertex_element){ .vertex_buffer_index = vbo_slot,
                .src_offset = 0, .src_stride = 16, .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT };
            ve->format_size[i] = 16;
        } else if (edge_stream[i]) {
            /* Interpolated passthrough replacement: a fresh user-buffer slot
             * over the clip-edge CPU array.  The passthrough emit uploads a
             * velem-referenced user buffer to a BO before the draw. */
            unsigned esl = next_edge_slot++;
            r300->vertex_buffer[esl].is_user_buffer = true;
            r300->vertex_buffer[esl].buffer.user =
                r300->r2vb_edge_stream_attr[i];
            ve->velem[i] = (struct pipe_vertex_element){ .vertex_buffer_index = esl,
                .src_offset = 0, .src_stride = 16, .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT };
            ve->format_size[i] = 16;
        } else {
            ve->velem[i] = saved_velem[streams[i].src_velem];
            ve->format_size[i] = saved_fmtsz[streams[i].src_velem];
        }
    }

    /* The producer hand-rolled rasterizer/framebuffer/scissor/viewport/ZB registers
     * outside the atom system; mark the owners dirty so the re-ingest prepare
     * re-emits the application values (else the producer's CLIP_DISABLE
     * over-rasterizes). */
    r2vb_redirty_app_raster_state(r300);

    /* Post-reconstruction routing dump + the resource-pointer invariant: every
     * stream's rebuilt velem must point at its expected source.  position -> clip
     * and computed -> vbo prove the producer feeds those two; each passthrough ->
     * its application buffer proves the original attribute still flows from the app
     * (no producer BO supplies it). */
    bool invariant = (ve->count == n_out);
    for (int i = 0; i < n_stream; i++) {
        struct pipe_vertex_buffer *svb =
            &r300->vertex_buffer[ve->velem[i].vertex_buffer_index];
        const char *tag = streams[i].kind == R2VB_STREAM_POS ? "pos" :
                          streams[i].kind == R2VB_STREAM_COMPUTED ? "computed" :
                          edge_stream[i] ? "edge" : "passthrough";
        bool hold;
        if (edge_stream[i]) {
            /* User-pointer identity: the stream fetches exactly the
             * interpolated array the clip route built. */
            hold = svb->is_user_buffer &&
                   svb->buffer.user == r300->r2vb_edge_stream_attr[i];
            fprintf(stderr,
                    "r2vb_reingest post stream%d slot=%u kind=%s vbi=%u user=%p expect=%p%s\n",
                    i, streams[i].slot, tag, ve->velem[i].vertex_buffer_index,
                    svb->buffer.user, (void *)r300->r2vb_edge_stream_attr[i],
                    hold ? "" : " MISMATCH");
        } else {
            struct pipe_resource *res = svb->buffer.resource;
            hold = res == expect[i];
            fprintf(stderr,
                    "r2vb_reingest post stream%d slot=%u kind=%s vbi=%u res=%p expect=%p%s\n",
                    i, streams[i].slot, tag, ve->velem[i].vertex_buffer_index,
                    (void *)res, (void *)expect[i], hold ? "" : " MISMATCH");
        }
        if (!hold)
            invariant = false;
    }
    r300_r2vb_dump_xform_routing(r300);
    fprintf(stderr,
            "r2vb_reingest invariant=%s (count==num_attribs==%u && pos->clip && computed->vbo && passthrough->app)\n",
            invariant ? "HOLD" : "FAIL", n_out);

    bool ok = false;
    if (!invariant) {
        fprintf(stderr, "r2vb_reingest refusing submit (invariant FAIL)\n");
    } else if (getenv("R300_R2VB_INSPECT")) {
        fprintf(stderr, "r2vb_reingest no-submit (R300_R2VB_INSPECT): wiring proven, skipping draw\n");
    } else {
        /* Two-submit path needs no adjacent barrier; single-CS mode must keep
         * the re-ingest barrier so the color/VAP flush orders the producer BO. */
        r300->r2vb_reingest_barrier = r2vb_single_cs_enabled();
        /* The delivery coordinate mode derives from the producer's actual
         * output contract: a window-space producer BO fetches verbatim, a
         * clip-space one runs the hardware viewport transform. */
        r300->r2vb_source_window =
            r300->r2vb_produced_space == R300_R2VB_POSITION_WINDOW;
        ok = r300_r2vb_exec_passthrough_draw(r300, info, draw);
        r300->r2vb_source_window = false;
        /* CS-decode correlation: after the emit the producer/app buffers are in the
         * command stream, so print their relocation indices.  The captured IB's
         * LOAD_VBPNTR array i must reference the matching buffer -- the r300-native
         * form of "stream i fetches from its source", since the IB carries reloc
         * indices, not VAs.  Holds whether or not the draw's raster cold-fails. */
        if (ok) {
            int reloc_clip = r300->rws->cs_lookup_buffer(&r300->cs, r300_resource(clip)->buf);
            int reloc_vbo = (vslot >= 0 && vbo)
                                ? r300->rws->cs_lookup_buffer(&r300->cs, r300_resource(vbo)->buf)
                                : -1;
            fprintf(stderr,
                    "r2vb_reingest warm reloc_clip=%d reloc_vbo=%d (computed-varying array must ref reloc_vbo; -1 when no computed varying)\n",
                    reloc_clip, reloc_vbo);
            for (int i = 0; i < n_stream; i++) {
                if (streams[i].kind != R2VB_STREAM_PASSTHROUGH)
                    continue;
                /* A passthrough source is the application vertex buffer, which in
                 * the r3v SWTCL context is a CPU-shadow (or user) buffer with no
                 * winsys BO.  r300_r2vb_exec_passthrough_draw uploads it to a fresh
                 * BO and fetches from that, so a lookup of the ORIGINAL resource is
                 * expected to miss (orig_reloc=-1) -- the routing to the app source
                 * is proven by the INSPECT invariant, and the upload is the same
                 * proven path r300_r2vb_exec_passthrough_draw uses for every
                 * passthrough vertex array.  A real-BO app buffer would instead be
                 * referenced directly and lookup would find it. */
                struct pipe_resource *pr = expect[i];
                int orig_reloc = pr && r300_resource(pr)->buf
                                     ? r300->rws->cs_lookup_buffer(&r300->cs, r300_resource(pr)->buf)
                                     : -1;
                fprintf(stderr,
                        "r2vb_reingest warm passthrough stream%d orig_reloc=%d (CPU-shadow source uploaded to a BO; -1 on the original is expected, routing proven by INSPECT)\n",
                        i, orig_reloc);
            }
        }
    }

    ve->count = saved_count;
    for (unsigned i = 0; i < saved_n && i < PIPE_MAX_ATTRIBS; i++) {
        ve->velem[i] = saved_velem[i];
        ve->format_size[i] = saved_fmtsz[i];
    }
    ve->vertex_size_dwords = saved_vsd;
    for (unsigned i = 0; i < n_fresh; i++)
        r300->vertex_buffer[clip_slot + i] = saved_vb[i];
    r300->nr_vertex_buffers = saved_nvb;
    return ok;
}

static bool r2vb_single_cs_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_R2VB_MVP_SINGLECS");
        cached = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return cached;
}

/* One position-producer pass: build the transform FS for the requested
 * position space (restage / externals / baked, in that precedence), bind it
 * with the minimal producer VS, emit the producer under the normal draw flow,
 * restore the application shaders, and order the BO write against the next
 * consumer.  The producer VS pins VAP_OUT_VTX_FMT / PSC / RS to the embedded
 * vertex so the VAP does not fetch past it for an application VS with extra
 * varyings.
 *
 * Ordering: the default two-submit form (proven byte-exact) flushes the CS and
 * maps the BO for read; the map waits for the submit and makes the BO
 * coherent.  The radeon winsys pools BOs, so the BO can be recycled from a
 * prior VBO; without the wait the VAP fetch reads stale (untransformed)
 * content even though LOAD_VBPNTR points at it.  R300_R2VB_MVP_SINGLECS keeps
 * producer and re-ingest in one command stream with no flush and no map; the
 * ordering is then carried by re-asserting the cb_flush_clean +
 * VAP_PVS_STATE_FLUSH barrier adjacent to the re-ingest draw
 * (r2vb_reingest_barrier in r300_r2vb_exec_passthrough_draw). */
/* One producer emit under the const0 transaction: bind the pass FS and the
 * producer VS, bind the producer matrix, emit the point draw into the target
 * BO, then restore the application shaders and const binding before the
 * post-producer derived-state update (the R2VB-STATE-01 discipline every
 * producer pass follows). */
static bool r300_r2vb_emit_split_pass(struct r300_context *r300, void *pass_fs,
                                      struct pipe_resource *target,
                                      uint32_t count, const float (*attrs)[4],
                                      unsigned num_attrs, const float *cols)
{
    void *saved_fs = r300->fs.state;
    void *saved_vs = r300->vs_state.state;
    void *pvs = r300_r2vb_get_producer_vs(r300, num_attrs);
    r300->context.bind_fs_state(&r300->context, pass_fs);
    if (pvs)
        r300->context.bind_vs_state(&r300->context, pvs);
    r300_r2vb_set_transform_consts_raw(r300, cols);
    r300_update_derived_state(r300);
    bool emitted = false;
    if (r300_r2vb_prepare_states(r300,
                                 64 + (int)count * 4 * (1 + (int)num_attrs) + 64)) {
        r300_r2vb_emit_producer(r300, r300_resource(target), 0, count, attrs,
                                num_attrs, true);
        emitted = true;
    }
    r300->context.bind_fs_state(&r300->context, saved_fs);
    if (pvs)
        r300->context.bind_vs_state(&r300->context, saved_vs);
    r300_r2vb_restore_app_fs_consts(r300);
    r300_update_derived_state(r300);
    return emitted;
}

/* Deliver an admitted budget-escape split: two producer passes whose composition
 * writes the same clip BO the single-pass producer would have.  Pass A renders
 * the cut-crossing carry (one FP32 vec4 per vertex) into a carry BO; the carry
 * then rides a CPU round-trip -- flush, map, read -- and feeds pass B as the
 * num_in+1'th embedded model attribute, which the producer VS routes to the
 * flat input at VARYING_SLOT_VAR0 + num_in that r300_mp_build_pos_pass_b reads
 * (r300_r2vb_emit_producer feeds embedded attribute a to VAR0+a, the same rank
 * scheme the restaged FS inputs use).  Pass B renders the position result into
 * the clip BO.  The CPU round-trip is the deliberate first carry transport: the
 * carry is exact FP32 through the map, and both passes reuse the proven
 * embedded-vertex producer; a streamed vertex-element fetch of the carry BO by
 * the pass-B draw is the follow-on transport that removes the round-trip.  Any
 * failure returns false with the clip BO unwritten so the draw falls back to
 * gallivm; true means pass B rendered into clip. */
static bool r300_r2vb_run_split_producer(struct r300_context *r300,
                                         struct pipe_resource *clip,
                                         uint32_t count, const float (*model)[4],
                                         unsigned num_in, const float *cols,
                                         enum r300_r2vb_position_space space,
                                         const struct r300_r2vb_producer_plan *plan)
{
    /* Plan-driven execution rebuilds pass B at the arity the plan measured,
     * so the caller's draw-path count must equal the planned count or the
     * executed program would diverge from the admitted one.  A mismatch
     * declines to gallivm; the contract's input_count_mismatch arm catches
     * the same divergence at classification. */
    if (plan && num_in != plan->num_position_inputs) {
        if (getenv("R300_R2VB_EXEC_DEBUG"))
            fprintf(stderr,
                    "r2vb_split_producer decline=input_count_mismatch "
                    "num_in=%u planned=%u\n",
                    num_in, plan->num_position_inputs);
        return false;
    }

    /* Pass B carries num_in model attributes plus the carry attribute. */
    if (num_in + 1 > R300_R2VB_MAX_PRODUCER_INPUTS)
        return false;

    struct r300_mp_partition local_part;
    const struct r300_mp_partition *part;
    nir_shader *pass_a, *pass_b;
    if (plan) {
        /* The cached cell owns the canonical candidate NIR and the selected
         * partition, and the pass builders clone their source internally, so
         * the plan survives execution intact and the program that runs is
         * the program the plan measured. */
        part = &plan->partition;
        pass_a = r300_mp_build_carry_pass_a(plan->candidate, part);
        pass_b = r300_mp_build_pos_pass_b(plan->candidate, part,
                                          plan->num_position_inputs);
    } else {
        nir_shader *pos =
            r300_r2vb_build_restaged_fs_nir(r300,
                                            r300_vs(r300)->state.ir.nir,
                                            VARYING_SLOT_POS, space);
        if (pos == NULL)
            return false;
        r300_optimize_nir(pos, r300->screen);
        if (!r300_mp_find_vec4_cut(pos, &local_part)) {
            ralloc_free(pos);
            return false;
        }
        part = &local_part;
        pass_a = r300_mp_build_carry_pass_a(pos, part);
        pass_b = r300_mp_build_pos_pass_b(pos, part, num_in);
        ralloc_free(pos);
    }
    if (!pass_a || !pass_b) {
        if (pass_a)
            ralloc_free(pass_a);
        if (pass_b)
            ralloc_free(pass_b);
        return false;
    }

    if (getenv("R300_R2VB_EXEC_DEBUG")) {
        /* The admission probe clones internally, so measuring here does not
         * consume the halves. */
        unsigned la = 0, lb = 0;
        r300_fs_measure_nir_admission(r300, pass_a, &la,
                                      R300_FS_INPUT_R2VB_FLAT_VERTEX, NULL);
        r300_fs_measure_nir_admission(r300, pass_b, &lb,
                                      R300_FS_INPUT_R2VB_FLAT_VERTEX, NULL);
        fprintf(stderr,
                "r2vb_split_producer cut=%u carry_bases=%u carry_comps=%u "
                "passA_alu=%u passB_alu=%u num_in=%u pass_b_attrs=%u count=%u\n",
                part->cut_index, part->num_bases, part->total_comps, la, lb,
                num_in, num_in + 1, count);
    }

    struct pipe_resource *carry_bo =
        r2vb_create_selftest_bo(r300, align(count, 2) * 16, 0);
    struct pipe_shader_state st_a = {0};
    st_a.type = PIPE_SHADER_IR_NIR;
    st_a.ir.nir = pass_a; /* create_fs_state takes ownership and precompiles */
    void *pa_fs = r300_create_fs_state_internal(&r300->context, &st_a,
                                                R300_FS_INPUT_R2VB_FLAT_VERTEX);
    struct pipe_shader_state st_b = {0};
    st_b.type = PIPE_SHADER_IR_NIR;
    st_b.ir.nir = pass_b;
    void *pb_fs = r300_create_fs_state_internal(&r300->context, &st_b,
                                                R300_FS_INPUT_R2VB_FLAT_VERTEX);
    float (*bmodel)[4] = malloc((size_t)count * (num_in + 1) * sizeof(*bmodel));
    if (!carry_bo || !pa_fs || !pb_fs || !bmodel)
        goto fail;

    /* Pass A: the carry producer, into the carry BO. */
    if (!r300_r2vb_emit_split_pass(r300, pa_fs, carry_bo, count, model, num_in,
                                   cols))
        goto fail;

    /* Carry round-trip: the flush + read map waits for the producer submit and
     * makes the GTT write coherent, the same ordering the BO oracles rely on. */
    r300->context.flush(&r300->context, NULL, 0);
    {
        struct pipe_transfer *xfer = NULL;
        struct pipe_box box = { .width = (int)count * 16, .height = 1, .depth = 1 };
        const float *carry = r300->context.buffer_map(&r300->context, carry_bo, 0,
                                                      PIPE_MAP_READ, &box, &xfer);
        if (!carry)
            goto fail;
        /* Extend the model layout (num_in attributes per vertex, in emit order)
         * to num_in+1: the carry lands after the model attributes, so the
         * producer feeds it to VAR0 + num_in, the pass-B carry input. */
        for (uint32_t pv = 0; pv < count; pv++) {
            for (unsigned a = 0; a < num_in; a++)
                memcpy(bmodel[pv * (num_in + 1) + a], model[pv * num_in + a],
                       sizeof(*bmodel));
            memcpy(bmodel[pv * (num_in + 1) + num_in], &carry[pv * 4],
                   sizeof(*bmodel));
        }
        if (getenv("R300_R2VB_EXEC_DEBUG"))
            fprintf(stderr,
                    "r2vb_split_producer carry[0]=%g,%g,%g,%g\n",
                    carry[0], carry[1], carry[2], carry[3]);
        r300->context.buffer_unmap(&r300->context, xfer);
    }

    /* Pass B: the position remainder, into the clip BO the delivery re-ingests. */
    if (!r300_r2vb_emit_split_pass(r300, pb_fs, clip, count,
                                   (const float (*)[4])bmodel, num_in + 1, cols))
        goto fail;

    /* Order the clip write against the re-ingest, matching the single-pass
     * producer tail: the flush + read map waits for the submit so the VAP does
     * not fetch a recycled GART page's stale content. */
    if (!r2vb_single_cs_enabled()) {
        r300->context.flush(&r300->context, NULL, 0);
        struct pipe_transfer *sxfer = NULL;
        struct pipe_box sbox = { .width = (int)count * 16, .height = 1, .depth = 1 };
        void *sm = r300->context.buffer_map(&r300->context, clip, 0, PIPE_MAP_READ,
                                            &sbox, &sxfer);
        /* The map waits for the producer write.  A failed map means the
         * re-ingest can still fetch a recycled BO's stale contents. */
        if (!sm)
            goto fail;
        r300->context.buffer_unmap(&r300->context, sxfer);
    }

    /* Trace the BO identities the split composition wrote through before the
     * carry BO is dropped: the carry BO pass A rendered and pass B re-read, and
     * the clip BO pass B rendered into (the resource the delivery re-ingests).
     * Capture-side only; the clip write and re-ingest ordering are unchanged. */
    if (getenv("R300_R2VB_EXEC_DEBUG")) {
        r300_r2vb_report_bo_identity(r300, "r2vb_split_producer carry_bo", carry_bo);
        r300_r2vb_report_bo_identity(r300, "r2vb_split_producer clip_bo", clip);
    }

    free(bmodel);
    r300->context.delete_fs_state(&r300->context, pa_fs);
    r300->context.delete_fs_state(&r300->context, pb_fs);
    /* R300_R2VB_SPLIT_KEEPALIVE=1 retains the carry BO through the delivered
     * split draw. The two-slot context ring rotates on the next split and
     * context destruction releases its references. */
    {
        static int keepalive_env = -1;
        if (keepalive_env < 0) {
            const char *e = getenv("R300_R2VB_SPLIT_KEEPALIVE");
            keepalive_env = (e && strcmp(e, "1") == 0) ? 1 : 0;
        }
        if (keepalive_env) {
            pipe_resource_reference(&r300->r2vb_split_keepalive[1], NULL);
            r300->r2vb_split_keepalive[1] = r300->r2vb_split_keepalive[0];
            r300->r2vb_split_keepalive[0] = carry_bo; /* reference moves to the ring */
            carry_bo = NULL;
            fprintf(stderr, "r2vb_split_producer carry_keepalive=parked bo=%p\n",
                    (void *)r300->r2vb_split_keepalive[0]);
        } else {
            pipe_resource_reference(&carry_bo, NULL);
        }
    }
    /* Label the clip BO the delivery re-ingests as split-producer output so the
     * delivery capture can distinguish it from the single-pass producer, and
     * publish the clip BO itself for the capture's position-identity clause. */
    r300->r2vb_producer_kind = R300_R2VB_PRODUCER_SPLIT;
    r300->r2vb_capture_clip = clip;
    return true;

fail:
    free(bmodel);
    if (pa_fs)
        r300->context.delete_fs_state(&r300->context, pa_fs);
    else
        ralloc_free(pass_a);
    if (pb_fs)
        r300->context.delete_fs_state(&r300->context, pb_fs);
    else
        ralloc_free(pass_b);
    pipe_resource_reference(&carry_bo, NULL);
    return false;
}

/* First-cell arm selector for the shipped BO-fetch producer draw.  Exact
 * values only: producer_capture3 runs the full real-path transaction and
 * discards the IB with RADEON_FLUSH_NOOP; producer_submit3 submits it and
 * reads the producer BO back.  Any other value keeps the arm closed. */
static int r2vb_bo_draw_producer3_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("R300_R2VB_BO_DRAW");
        if (e && strcmp(e, "producer_capture3") == 0)
            mode = 1;
        else if (e && strcmp(e, "producer_submit3") == 0)
            mode = 2;
        else
            mode = 0;
    }
    return mode;
}

/* Run the shipped LOAD_VBPNTR producer transaction at the real producer call
 * site, for the first three-vertex cell.  The caller has the producer FS and
 * VS bound and the derived state updated; this arm consumes the cached plan,
 * the live derived RS block, the bound producer FS semantics, and the
 * application's position vertex element -- the transaction materializes the
 * model span from that element's real buffer, so the fetch input is the
 * application's own data, not a synthetic fixture.
 *
 * Emission order: the transaction stages the complete buffer list first
 * (capacity reservation, then dirty-state resources plus the three producer
 * BOs, then cs_validate); dirty state and the shared output-target prologue
 * land next, so the raw COLOROFFSET0 retarget and its relocation precede the
 * custom range exactly as the proven immediate producer orders them; the
 * 64-dword custom range and the shared cache-publication tail close the
 * producer.  capture discards the IB with RADEON_FLUSH_NOOP; submit flushes
 * through the normal path, waits the BO, and prints every output record for
 * the off-box oracle.  Either way the caller falls back to gallivm for the
 * visible draw -- the cell tests exactly one new hardware mechanism, the
 * BO-fetched producer input, and delivers nothing. */
static void r2vb_run_bo_fetch_producer3(struct r300_context *r300,
                                        struct pipe_resource *clip,
                                        uint32_t count, uint32_t start,
                                        enum r300_r2vb_position_space space,
                                        bool submit)
{
    const char *why = NULL;
    struct pipe_resource *slot_res = NULL;
    struct r300_r2vb_producer_bo_draw txn;
    r300_r2vb_producer_bo_draw_init(&txn);

    const struct r300_r2vb_producer_plan *plan =
        r300_r2vb_producer_plan_get(r300, false, space);
    struct r300_fragment_shader *pfs = r300_fs(r300);
    const struct r300_rs_block *rs =
        (const struct r300_rs_block *)r300->rs_block_state.state;
    const struct pipe_vertex_element *ve = NULL;
    const struct pipe_vertex_buffer *vb = NULL;

    /* The live-submit half additionally rides the workspace raw-submit
     * consent gate; capture stays reachable without it. */
    if (submit && !r300_r2vb_option_is(getenv("R300_RAW_SUBMIT_ACCEPTED"), "1"))
        why = "raw_submit_gate";
    else if (count != 3)
        why = "count";
    else if (!plan || plan->status != R300_R2VB_PLAN_READY ||
             plan->action != R300_R2VB_PLAN_SINGLE ||
             !plan->position_source.valid)
        why = "plan";
    else if (!pfs || !pfs->shader)
        why = "producer_fs";
    else if (!rs)
        why = "rs_block";
    else if (!r300->velems ||
             plan->position_source.location_rank >= r300->velems->count)
        why = "velems";
    if (!why) {
        ve = &r300->velems->velem[plan->position_source.location_rank];
        if (ve->vertex_buffer_index >= r300->nr_vertex_buffers)
            why = "vb_index";
        else {
            vb = &r300->vertex_buffer[ve->vertex_buffer_index];
            if (!vb->buffer.resource)
                why = "user_buffer";
        }
    }

    /* The transaction PSC is the BO-fetch interface built from the real
     * element parameters at the calibrated destination vectors; the
     * validate-side interface rebuild then re-derives the same words from
     * the materialized streams, so a divergence declines. */
    struct r300_r2vb_producer_streams st;
    struct r300_r2vb_producer_fetch ft;
    struct r300_r2vb_producer_interface it;
    struct r300_vertex_stream_state psc;
    if (!why &&
        (!r300_r2vb_producer_streams_init(vb->buffer_offset, ve->src_offset,
                                          ve->src_stride, ve->src_format,
                                          start, &st) ||
         !r300_r2vb_producer_fetch_init(&st, count, (uint64_t)count * 16,
                                        vb->buffer.resource->width0, &ft) ||
         !r300_r2vb_producer_interface_init(
             &ft, R300_R2VB_CAL_SLOT_DST_VEC_LOC,
             R300_R2VB_CAL_MODEL_DST_VEC_LOC, &it)))
        why = "streams";
    if (!why) {
        memset(&psc, 0, sizeof(psc));
        for (unsigned i = 0; i < 8; i++) {
            psc.vap_prog_stream_cntl[i] = it.prog_stream_cntl[i];
            psc.vap_prog_stream_cntl_ext[i] = it.prog_stream_cntl_ext[i];
        }
        psc.count = 1;
    }

    /* Slot positions: the same one-row pixel centers the immediate producer
     * embeds, written once by the CPU before any GPU use of the BO. */
    if (!why) {
        slot_res = r2vb_create_selftest_bo(r300, count * 16, 0);
        if (!slot_res)
            why = "slot_bo";
    }
    if (!why) {
        struct pipe_transfer *xfer = NULL;
        struct pipe_box box = { .width = (int)(count * 16), .height = 1,
                                .depth = 1 };
        float *slots = r300->context.buffer_map(&r300->context, slot_res, 0,
                                                PIPE_MAP_WRITE, &box, &xfer);
        if (!slots)
            why = "slot_map";
        else {
            for (uint32_t i = 0; i < count; i++) {
                slots[i * 4 + 0] = (float)i + 0.5f;
                slots[i * 4 + 1] = 0.5f;
                slots[i * 4 + 2] = 0.0f;
                slots[i * 4 + 3] = 1.0f;
            }
            r300->context.buffer_unmap(&r300->context, xfer);
        }
    }

    /* Sentinel-fill the producer output so the readback separates written
     * records from untouched storage (the even-pitch padding pixel). */
    if (!why) {
        struct pipe_transfer *xfer = NULL;
        struct pipe_box box = { .width = (int)clip->width0, .height = 1,
                                .depth = 1 };
        void *m = r300->context.buffer_map(&r300->context, clip, 0,
                                           PIPE_MAP_WRITE, &box, &xfer);
        if (m) {
            memset(m, 0xcb, clip->width0);
            r300->context.buffer_unmap(&r300->context, xfer);
        }
    }

    bool ok = false;
    if (!why) {
        /* Reserve + emit dirty state through the real prepare path: shared
         * prologue (31 + wpos override) + 64-dword custom range + 8-dword
         * tail, with margin. */
        if (!r300_r2vb_prepare_states(r300, 192))
            why = "prepare";
    }
    if (!why) {
        /* validate() proves output authority against the bound framebuffer
         * color target; this producer form carries that authority in the
         * shared prologue's raw COLOROFFSET0 retarget + relocation instead
         * of an emitted framebuffer atom, so the identity is stated to the
         * pure-inspection validate through a local framebuffer view and the
         * application state pointer is restored before any emission. */
        void *saved_fb = r300->fb_state.state;
        struct pipe_framebuffer_state pfb;
        memset(&pfb, 0, sizeof(pfb));
        pfb.width = (uint16_t)align(count, 2);
        pfb.height = 1;
        pfb.nr_cbufs = 1;
        pfb.cbufs[0].texture = clip;
        pfb.cbufs[0].format = PIPE_FORMAT_R32G32B32A32_FLOAT;
        r300->fb_state.state = &pfb;
        bool validated = r300_r2vb_producer_bo_draw_validate(
            r300, plan, &pfs->shader->inputs, rs, &psc, vb, ve,
            r300->velems->count, r300->nr_vertex_buffers, slot_res, clip,
            start, count, space, &txn);
        r300->fb_state.state = saved_fb;
        if (!validated)
            why = "validate";
    }
    if (!why && !r300_r2vb_producer_bo_draw_stage_cs(r300, &txn, plan,
                                                     &pfs->shader->inputs, rs,
                                                     &psc))
        why = "stage_cs";
    if (!why) {
        /* Any staging flush re-marked the atoms; land them before the raw
         * prologue so no dirty emission can follow the retarget. */
        r300_emit_dirty_state(r300);
        r2vb_emit_producer_target_prologue(r300, r300_resource(clip), 0,
                                           count, true);
        if (!r300_r2vb_producer_bo_draw_emit(r300, &txn)) {
            why = "emit";
        } else {
            r2vb_emit_producer_order_tail(r300);
            ok = true;
        }
    }

    fprintf(stderr,
            "r2vb_bo_draw_producer3 mode=%s ok=%d why=%s count=%u start=%u "
            "space=%s slot_reloc=%d model_reloc=%d output_reloc=%d\n",
            submit ? "submit" : "capture", ok, why ? why : "-", count, start,
            space == R300_R2VB_POSITION_WINDOW ? "window" : "clip",
            txn.slot_reloc_index, txn.model_reloc_index,
            txn.output_reloc_index);

    if (ok && !submit) {
        /* Discard the producer IB before DRM_RADEON_CS; R300_TRACE has
         * already retained it for the full-path decode. */
        r300->rws->cs_flush(&r300->cs, RADEON_FLUSH_NOOP, NULL);
        fprintf(stderr, "r2vb_bo_draw_producer3 decision=capture "
                        "(no-submit; RADEON_FLUSH_NOOP)\n");
    } else if (ok && submit) {
        fprintf(stderr, "r2vb_bo_draw_producer3 decision=submit\n");
        r300->context.flush(&r300->context, NULL, 0);
        r2vb_wait_bo(r300, clip);
        struct pipe_transfer *xfer = NULL;
        struct pipe_box box = { .width = (int)clip->width0, .height = 1,
                                .depth = 1 };
        const uint32_t *rec = r300->context.buffer_map(
            &r300->context, clip, 0, PIPE_MAP_READ, &box, &xfer);
        if (rec) {
            for (uint32_t i = 0; i < clip->width0 / 16; i++) {
                float f[4];
                memcpy(f, &rec[i * 4], sizeof(f));
                fprintf(stderr,
                        "r2vb_bo_draw_producer3 rec=%u %08x %08x %08x %08x "
                        "(%g %g %g %g)\n",
                        i, rec[i * 4 + 0], rec[i * 4 + 1], rec[i * 4 + 2],
                        rec[i * 4 + 3], f[0], f[1], f[2], f[3]);
            }
            r300->context.buffer_unmap(&r300->context, xfer);
        } else {
            fprintf(stderr, "r2vb_bo_draw_producer3 readback=map_failed\n");
        }
    }

    /* The producer hand-wrote raster registers outside the atom system, and
     * the capture arm discarded a CS carrying emitted state; re-mark the
     * application state either way before the gallivm fallback draw. */
    r2vb_redirty_app_raster_state(r300);
    r300->vertex_arrays_dirty = true;
    r300_r2vb_producer_bo_draw_fini(&txn);
    pipe_resource_reference(&slot_res, NULL);
}

static bool r2vb_run_transform_producer(struct r300_context *r300,
                                        struct pipe_resource *clip,
                                        uint32_t count, const float (*model)[4],
                                        unsigned num_in, const float *cols,
                                        uint32_t start,
                                        enum r300_r2vb_position_space space,
                                        bool restage, bool externals)
{
    void *xfs;
    bool xfs_cached = false;

    if (restage) {
        /* Producer re-entry admission: the clip-route accept action re-runs
         * this producer in window space without passing the classify hook,
         * so the run re-applies the same per-cell admission classify uses --
         * structural scan, budget oracle, and (typed gate) the cached-plan
         * contract -- for the exact space it will emit and in the same
         * computed-varying mode the route classified under.  A declining
         * cell fails the run and the draw falls back to gallivm; the restage
         * arm below therefore only ever builds a producer FS its cell
         * admitted, so an over-budget candidate can no longer dummy-compile
         * into an empty frame reported as success. */
        static int varying_mode = -1;
        if (varying_mode < 0) {
            const char *e = getenv("R300_R2VB_VARYING");
            varying_mode = (e && strcmp(e, "1") == 0) ? 1 : 0;
        }
        if (!r300_vs_admits_producer(r300, varying_mode == 1, space)) {
            if (getenv("R300_R2VB_EXEC_DEBUG"))
                fprintf(stderr,
                        "r2vb_producer decline=cell_admission space=%s\n",
                        space == R300_R2VB_POSITION_WINDOW ? "window"
                                                           : "clip");
            return false;
        }
        /* An admitted budget-escape split delivers through the two-pass carry-BO
         * producer instead of a single over-budget FS.  Guarded by the spill1
         * gate (or, for a plan-admitted typed cell, the typed diagnostic gate)
         * and the per-VS memo, so gated off the branch is never taken. */
        if ((r300_r2vb_budget_escape_enabled() ||
             r300_r2vb_typed_split_enabled()) &&
            r300_vs(r300)->r2vb_admission[0]
                          [space == R300_R2VB_POSITION_WINDOW ? 1 : 0] ==
                R300_R2VB_ADMIT_SPLIT) {
            /* A typed cell executes from the cached plan; under spill1 a
             * float cell keeps the legacy rebuild (the contract declines it
             * on typed_source_absent), so the spill1 path stays
             * byte-identical.  Under the typed gate alone the cached plan is
             * the sole split authority: a cell the contract declines at
             * execution time falls back to gallivm instead of the first-fit
             * rebuild. */
            const struct r300_r2vb_producer_plan *plan = NULL;
            if (r300_r2vb_typed_split_enabled()) {
                const struct r300_r2vb_producer_plan *cell =
                    r300_r2vb_producer_plan_get(r300, false, space);
                if (cell &&
                    !r300_r2vb_typed_split_contract(cell, false, space,
                                                    num_in))
                    plan = cell;
                if (!plan && !r300_r2vb_budget_escape_enabled())
                    return false;
            }
            return r300_r2vb_run_split_producer(r300, clip, count, model, num_in,
                                                cols, space, plan);
        }
    }
    if (restage) {
        /* Derive the producer from the bound VS itself -- the general fragment-ALU
         * vertex route.  Re-stage its NIR as the FS (position only) and load the
         * matrix it reads from UBO[0] into FS const file 0 untransposed (the VS
         * body is column-MAD).  Built per draw and deleted after, like the baked
         * path, since the FS tracks whatever VS is bound. */
        xfs = r300_r2vb_restage_vs_as_fs(r300, r300_vs(r300)->state.ir.nir,
                                         VARYING_SLOT_POS, space);
        if (xfs)
            r300_r2vb_set_transform_consts_raw(r300, cols);
    } else if (externals) {
        xfs = r300_r2vb_get_transform_fs(r300, space);
        if (xfs)
            r300_r2vb_set_transform_consts(r300, cols);
        xfs_cached = true;
    } else {
        float rows[16];
        for (unsigned i = 0; i < 4; i++)
            for (unsigned j = 0; j < 4; j++)
                rows[i * 4 + j] = cols[j * 4 + i];
        xfs = r300_r2vb_build_baked_transform_fs(r300, rows, space);
    }
    if (!xfs)
        return false;

    void *saved_fs = r300->fs.state;
    void *saved_vs = r300->vs_state.state;
    void *pvs = r300_r2vb_get_producer_vs(r300, num_in);
    r300->context.bind_fs_state(&r300->context, xfs);
    if (pvs)
        r300->context.bind_vs_state(&r300->context, pvs);
    r300_update_derived_state(r300);
    /* BO-fetch first-cell arm: with producer_capture3 / producer_submit3
     * armed, the transaction runs here -- producer FS and VS bound, derived
     * state current, real output target in hand -- and the immediate
     * producer is bypassed entirely, so the run carries exactly one
     * producer form.  prepared stays false, so the caller restores the
     * application shaders and falls back to gallivm for the visible draw. */
    int bo3 = r2vb_bo_draw_producer3_mode();
    bool prepared = false;
    if (bo3 != 0 && restage && num_in == 1) {
        r2vb_run_bo_fetch_producer3(r300, clip, count, start, space,
                                    bo3 == 2);
    } else if (bo3 != 0) {
        fprintf(stderr,
                "r2vb_bo_draw_producer3 mode=%s ok=0 why=%s count=%u\n",
                bo3 == 2 ? "submit" : "capture",
                restage ? "num_in" : "restage", count);
    } else {
        prepared = r300_r2vb_prepare_states(
            r300, 64u + count * 4u * (1u + num_in) + 64u);
        if (prepared)
            r300_r2vb_emit_producer(r300, r300_resource(clip), 0, count,
                                    model, num_in, true);
    }
    r300->context.bind_fs_state(&r300->context, saved_fs);
    if (pvs)
        r300->context.bind_vs_state(&r300->context, saved_vs);
    if (restage || externals)
        r300_r2vb_restore_app_fs_consts(r300);
    r300_update_derived_state(r300);
    if (!xfs_cached)
        r300->context.delete_fs_state(&r300->context, xfs);
    if (!prepared) {
        r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
        r300->r2vb_capture_clip = NULL;
        return false;
    }

    if (!r2vb_single_cs_enabled()) {
        r300->context.flush(&r300->context, NULL, 0);
        r2vb_wait_bo(r300, clip);
        struct pipe_transfer *sxfer = NULL;
        struct pipe_box sbox = { .width = (count ? count : 1) * 16, .height = 1, .depth = 1 };
        void *sm = r300->context.buffer_map(&r300->context, clip, 0, PIPE_MAP_READ, &sbox, &sxfer);
        if (!sm) {
            r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
            r300->r2vb_capture_clip = NULL;
            return false;
        }
        r300->context.buffer_unmap(&r300->context, sxfer);
    }
    /* Single over-budget-free producer FS filled the clip BO; label it so the
     * delivery capture distinguishes it from the two-pass split producer, and
     * publish the clip BO itself for the capture's position-identity clause. */
    r300->r2vb_producer_kind = R300_R2VB_PRODUCER_SINGLE;
    r300->r2vb_capture_clip = clip;
    return true;
}

/* Resolve a TRIANGLES / TRIANGLE_STRIP / TRIANGLE_FAN draw, indexed or not,
 * into a plain triangle-index list: gidx holds *out_count source-vertex rows
 * (3 per output triangle, winding preserved), each row in the same domain the
 * non-indexed model-gather loop already reads -- a raw offset into the bound
 * vertex buffers, i.e. draw->start+i for a non-indexed draw or the index
 * buffer's value plus draw->index_bias for an indexed one.  Both the
 * position model-gather and r2vb_read_velem_floats walk this same table when
 * gathered, so position and passthrough attributes cannot diverge.
 *
 * Strip triangle t is (t, t+1, t+2) on an even t and (t+1, t, t+2) on an odd
 * one (preserves the alternating winding); fan triangle t is (0, t+1, t+2).
 * info->primitive_restart splits the source-index sequence at every
 * occurrence of info->restart_index, compared against the RAW index-buffer
 * value before index_bias (the same pre-bias comparison
 * util_prim_restart_convert_to_direct performs; the bias joins at emission),
 * into independent segments, each walked from its own local t=0, so a restart never stitches
 * a triangle across the split; a segment shorter than one full primitive (3
 * indices for TRIANGLES, or the STRIP/FAN minimum) contributes nothing.  A
 * TRIANGLES segment whose length is not a multiple of 3 drops its trailing
 * partial triangle, matching draw_vbo's own primitive-count truncation.
 *
 * Returns false when the topology is not a triangle family, an index or
 * user-buffer source cannot be read, the gathered count would exceed the
 * producer's 4096-vertex ceiling, or the gathered count is 0; *out_idx is
 * caller-owned (free()) and valid only on true. */
static bool
r2vb_topology_gather_indices(struct r300_context *r300,
                             const struct pipe_draw_info *info,
                             const struct pipe_draw_start_count_bias *draw,
                             uint32_t **out_idx, uint32_t *out_count)
{
    if (info->mode != MESA_PRIM_TRIANGLES &&
        info->mode != MESA_PRIM_TRIANGLE_STRIP &&
        info->mode != MESA_PRIM_TRIANGLE_FAN)
        return false;

    const uint32_t n_verts = draw->count;
    if (n_verts == 0)
        return false;
    /* Cap source allocation before malloc: gathered output is at most 4096
     * vertices.  TRIANGLES emit one output vertex per source; strip/fan emit
     * at most 3*(n-2), so n > 4096/3+2 cannot fit. */
    const uint32_t cap = 4096;
    if (info->mode == MESA_PRIM_TRIANGLES) {
        if (n_verts > cap)
            return false;
    } else {
        /* strip / fan */
        if (n_verts > (cap / 3u) + 2u)
            return false;
    }

    uint32_t *src = malloc((size_t)n_verts * sizeof(*src));
    if (!src)
        return false;
    if (info->index_size == 0) {
        /* Non-indexed: start + (n_verts-1) must fit uint32_t. */
        if (n_verts > 0 && draw->start > UINT32_MAX - (n_verts - 1u)) {
            free(src);
            return false;
        }
        for (uint32_t i = 0; i < n_verts; i++)
            src[i] = draw->start + i;
    } else {
        const uint8_t *ibase = NULL;
        if (info->has_user_indices)
            ibase = (const uint8_t *)info->index.user;
        else if (info->index.resource)
            ibase = r300_resource(info->index.resource)->malloced_buffer;
        if (!ibase) {
            free(src);
            return false;
        }
        /* Extent-check the index fetch window for BO-backed index buffers.
         * start + n_verts and the byte span use wrap-checked arithmetic. */
        size_t end_idx, byte_off, need;
        if (!r2vb_size_add((size_t)draw->start, n_verts, &end_idx)) {
            free(src);
            return false;
        }
        if (!r2vb_size_mul((size_t)draw->start, info->index_size, &byte_off)) {
            free(src);
            return false;
        }
        if (!info->has_user_indices && info->index.resource) {
            if (!r2vb_size_mul(end_idx, info->index_size, &need) ||
                need > (size_t)info->index.resource->width0) {
                free(src);
                return false;
            }
        }
        ibase += byte_off;
        for (uint32_t i = 0; i < n_verts; i++) {
            uint32_t raw;
            switch (info->index_size) {
            case 1: raw = ibase[i]; break;
            case 2: raw = ((const uint16_t *)ibase)[i]; break;
            case 4: raw = ((const uint32_t *)ibase)[i]; break;
            default:
                free(src);
                return false;
            }
            src[i] = raw;
        }
    }
    /* Bias joins the emitted rows, never the restart comparison above. */
    const int64_t bias = info->index_size != 0 ? draw->index_bias : 0;

    uint32_t *gidx = malloc((size_t)cap * sizeof(*gidx));
    if (!gidx) {
        free(src);
        return false;
    }
    uint32_t n_out = 0;
    bool overflow = false;
    uint32_t seg_start = 0;

    for (uint32_t i = 0; i <= n_verts; i++) {
        bool is_restart = info->primitive_restart && info->index_size != 0 &&
                          i < n_verts && src[i] == info->restart_index;
        if (!is_restart && i < n_verts)
            continue;
        uint32_t seg_len = i - seg_start;
        uint32_t n_tri = seg_len >= 3
                             ? (info->mode == MESA_PRIM_TRIANGLES ? seg_len / 3
                                                                  : seg_len - 2)
                             : 0;
        for (uint32_t t = 0; t < n_tri; t++) {
            uint32_t i0, i1, i2;
            if (info->mode == MESA_PRIM_TRIANGLES) {
                i0 = seg_start + t * 3 + 0;
                i1 = seg_start + t * 3 + 1;
                i2 = seg_start + t * 3 + 2;
            } else if (info->mode == MESA_PRIM_TRIANGLE_STRIP) {
                /* Gathered output is MESA_PRIM_TRIANGLES with first-vertex
                 * provoking.  Even triangles stay (t,t+1,t+2).  Odd triangles
                 * under last-vertex PV rotate to (t+1,t,t+2) so winding of the
                 * strip is preserved and source t+2 remains last.  flatshade_first
                 * keeps source t first while preserving that odd-triangle
                 * winding by rotating (t+1,t,t+2) to (t,t+2,t+1). */
                const struct r300_rs_state *rs =
                    r300->rs_state.state
                        ? (const struct r300_rs_state *)r300->rs_state.state
                        : NULL;
                const bool first_pv = rs && rs->rs.flatshade_first;
                if ((t & 1) == 0) {
                    i0 = seg_start + t + 0;
                    i1 = seg_start + t + 1;
                    i2 = seg_start + t + 2;
                } else if (first_pv) {
                    i0 = seg_start + t + 0;
                    i1 = seg_start + t + 2;
                    i2 = seg_start + t + 1;
                } else {
                    i0 = seg_start + t + 1;
                    i1 = seg_start + t + 0;
                    i2 = seg_start + t + 2;
                }
            } else { /* MESA_PRIM_TRIANGLE_FAN */
                i0 = seg_start;
                i1 = seg_start + t + 1;
                i2 = seg_start + t + 2;
            }
            if (n_out + 3 > cap) {
                overflow = true;
                break;
            }
            gidx[n_out++] = (uint32_t)((int64_t)src[i0] + bias);
            gidx[n_out++] = (uint32_t)((int64_t)src[i1] + bias);
            gidx[n_out++] = (uint32_t)((int64_t)src[i2] + bias);
        }
        if (overflow)
            break;
        seg_start = i + 1; /* skip the restart index itself */
    }
    free(src);

    if (overflow || n_out == 0) {
        free(gidx);
        return false;
    }
    *out_idx = gidx;
    *out_count = n_out;
    return true;
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
    if (getenv("R300_R2VB_EXEC_DEBUG"))
        fprintf(stderr, "r2vb_exec_entry const0=%p size=%u velems=%p vcount=%u draw_count=%u\n",
                r300->swtcl_vs_const0_ptr, r300->swtcl_vs_const0_size,
                (void *)r300->velems, r300->velems ? r300->velems->count : 0, draw->count);
    if (!r300->swtcl_vs_const0_ptr || r300->swtcl_vs_const0_size < 64)
        return false;
    if (!r300->velems || r300->velems->count == 0)
        return false;
    unsigned count = draw->count;
    const float *cols = (const float *)r300->swtcl_vs_const0_ptr;

    /* Read the model-space inputs feeding gl_Position.  num_in is the count of VS
     * inputs the position depends on (an MVP VS reads only velem[0]); a multi-input
     * position VS reads velem[0..num_in-1] in input order.  The producer feeds each
     * input a at VAR0+a, so the re-staged position FS reads input a there.  model is
     * laid out num_in per vertex -- model[i*num_in + a] is vertex i's input a -- and
     * each element honors its buffer/offset/stride and component count (a vec3 gets
     * w = 1).  num_in == 1 reduces to the MVP single-attribute read. */
    unsigned num_in = r300_r2vb_count_position_inputs(r300_vs(r300)->state.ir.nir);
    if (num_in > R300_R2VB_MAX_PRODUCER_INPUTS)
        num_in = R300_R2VB_MAX_PRODUCER_INPUTS;
    if (num_in > r300->velems->count)
        num_in = r300->velems->count;

    /* Topology gather (R300_R2VB_TOPOLOGY, requires R300_R2VB_CLIP_ROUTE):
     * an indexed draw or a TRIANGLE_STRIP/TRIANGLE_FAN topology is resolved
     * to a plain triangle-index list ahead of the model read below, so every
     * consumer further down this function -- the clip-route classify and
     * rebuild, and the eventual re-ingest -- sees a non-indexed TRIANGLES
     * list.  einfo mirrors that decomposition (mode forced to TRIANGLES,
     * index_size to 0) for the delivery calls that still read info->mode.
     * Declines (returns false) for a shape the gather cannot express: a
     * multi-input position (num_in != 1 -- the gather only rebuilds a single
     * position stream), an unreadable index source, or a gathered count past
     * the 4096-vertex producer ceiling.  Byte-identical to the ungathered
     * path when the gate is off: r300_r2vb_topology_enabled() is false, so
     * topo_shape is always false and the original count/index arithmetic
     * below runs unchanged. */
    bool topo_shape = r300_r2vb_topology_enabled() &&
                      (info->index_size != 0 ||
                       info->mode == MESA_PRIM_TRIANGLE_STRIP ||
                       info->mode == MESA_PRIM_TRIANGLE_FAN);
    struct pipe_draw_info topo_info_storage;
    const struct pipe_draw_info *einfo = info;
    uint32_t *topo_idx = NULL;
    bool topo_gathered = false;
    if (topo_shape) {
        if (num_in != 1)
            return false;
        uint32_t gcount = 0;
        if (!r2vb_topology_gather_indices(r300, info, draw, &topo_idx, &gcount))
            return false;
        count = gcount;
        topo_gathered = true;
        topo_info_storage = *info;
        topo_info_storage.mode = MESA_PRIM_TRIANGLES;
        topo_info_storage.index_size = 0;
        topo_info_storage.has_user_indices = false;
        topo_info_storage.primitive_restart = false;
        einfo = &topo_info_storage;
    } else if (count == 0 || count > 4096) {
        return false;
    }

    float (*model)[4] = malloc((size_t)count * num_in * sizeof(*model));
    if (!model) {
        free(topo_idx);
        return false;
    }
    for (unsigned a = 0; a < num_in; a++) {
        struct pipe_vertex_element *pe = &r300->velems->velem[a];
        struct pipe_vertex_buffer *vb = &r300->vertex_buffer[pe->vertex_buffer_index];
        const uint8_t *base = NULL;
        if (vb->is_user_buffer)
            base = vb->buffer.user;
        else if (vb->buffer.resource)
            base = r300_resource(vb->buffer.resource)->malloced_buffer;
        if (!base || !pe->src_stride) {
            free(model);
            free(topo_idx);
            return false;
        }
        base += vb->buffer_offset + pe->src_offset;
        unsigned comps = util_format_get_nr_components(pe->src_format);
        for (unsigned i = 0; i < count; i++) {
            const uint32_t vidx =
                topo_gathered ? topo_idx[i] : draw->start + i;
            if (!r2vb_velem_index_in_bounds(r300, a, vidx)) {
                free(model);
                free(topo_idx);
                return false;
            }
            const float *v =
                (const float *)(base + (size_t)vidx * pe->src_stride);
            model[i * num_in + a][0] = v[0];
            model[i * num_in + a][1] = comps > 1 ? v[1] : 0.0f;
            model[i * num_in + a][2] = comps > 2 ? v[2] : 0.0f;
            model[i * num_in + a][3] = comps > 3 ? v[3] : 1.0f;
        }
    }

    /* Topology gather reorders delivery vertices.  Position comes from the
     * producer BO in that order; passthrough streams still bound to app
     * buffers would fetch the wrong rows.  Gather each passthrough attribute
     * into r2vb_edge_stream_attr (same user-buffer re-ingest path the clip-
     * edge rebuild uses), or decline when a stream cannot be read. */
    if (topo_gathered) {
        struct r300_r2vb_reingest_stream st[PIPE_MAX_ATTRIBS];
        int ns = r300_r2vb_reingest_stream_layout(
            r300_vs(r300)->state.ir.nir, -1, st, PIPE_MAX_ATTRIBS);
        bool has_pt = false;
        for (int i = 0; i < ns; i++)
            if (st[i].kind == R2VB_STREAM_PASSTHROUGH)
                has_pt = true;
        if (has_pt) {
            r2vb_edge_streams_release(r300);
            memset(r300->r2vb_edge_stream_attr, 0,
                   sizeof(r300->r2vb_edge_stream_attr));
            bool ok_pt = ns > 0;
            for (int i = 0; ok_pt && i < ns; i++) {
                if (st[i].kind != R2VB_STREAM_PASSTHROUGH)
                    continue;
                if (st[i].src_velem < 0) {
                    ok_pt = false;
                    break;
                }
                float (*rows)[4] = r2vb_read_velem_floats(
                    r300, (unsigned)st[i].src_velem, topo_idx, count);
                if (!rows) {
                    ok_pt = false;
                    break;
                }
                r300->r2vb_edge_stream_attr[i] = rows;
                r300->r2vb_edge_streams_active = true;
            }
            if (!ok_pt) {
                r2vb_edge_streams_release(r300);
                free(model);
                free(topo_idx);
                return false;
            }
        }
    }
    free(topo_idx);
    topo_idx = NULL;

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
                                              VARYING_SLOT_POS, r2vb_env_space());
        if (pf) {
            /* Capture the derived VAP/RS state the producer would inherit for this
             * bound VS, up to (not including) emit_producer -- no CS submit.  Bind
             * the producer VS too (the fix), so the dump shows the routing the
             * producer actually emits; after the fix the 3-output VS's dump must
             * match the 2-output VS's. */
            void *saved_fs = r300->fs.state;
            void *saved_vs = r300->vs_state.state;
            void *pvs = r300_r2vb_get_producer_vs(r300, num_in);
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
                                                  (gl_varying_slot)dv,
                                                  r2vb_env_space());
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
     * it byte-exact.  Producer const binds bypass the fs_const0_app mirror
     * and the transaction rebinds the application's FS constant buffer 0
     * before the post-producer derived-state update, so the re-ingest's
     * application FS reads its own constants, not the matrix. */
    static int externals = -1, restage = -1;
    if (externals < 0) {
        const char *e = getenv("R300_R2VB_MVP_EXTERNALS");
        externals = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (restage < 0) {
        const char *e = getenv("R300_R2VB_RESTAGE");
        restage = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    /* The canary admits only VS-derived producers, so it runs the restaged
     * builder; the hand-built MVP and externals forms stay manual-gate. */
    if (r300_r2vb_auto_single_armed(NULL))
        restage = 1;
    /* The classification oracle and the clip-route action both need the raw
     * homogeneous result: force the clip-space producer regardless of the
     * divide gate.  The route's accept action re-runs the producer in window
     * space afterwards, so produced_space is reassigned then. */
    enum r300_r2vb_position_space produced_space =
        (r300_r2vb_clip_classify_enabled() || r300_r2vb_clip_route_enabled())
            ? R300_R2VB_POSITION_CLIP
            : r2vb_env_space();
    r300->r2vb_produced_space = produced_space;
    if (!r2vb_run_transform_producer(r300, clip, count, model, num_in, cols,
                                     draw->start, produced_space, restage,
                                     externals)) {
        pipe_resource_reference(&clip, NULL);
        free(model);
        return false;
    }

    if (getenv("R300_R2VB_XFORM_VERIFY") && num_in == 1)
        r2vb_verify_xform_readback(r300, clip, model, count, cols);

    /* Differential oracle: when the divide gate is on, diff the producer BO
     * against the CPU divide + viewport reference.  Every producer variant
     * carries the divide -- the baked and externals builders through
     * r2vb_build_producer_output, the restage builder by wrapping the cloned
     * VS's position store with r2vb_divide_position.  Single-input path only,
     * and only after the pure-CPU self-test confirms the reference math, so a
     * broken oracle cannot green a broken shader.  Reads the same GART clip BO
     * the xform verify maps -- one fragment-ALU producer submit, no VAP/HW-TCL
     * re-ingest. */
    {
        static int divide_verify = -1;
        if (divide_verify < 0) {
            const char *e = getenv("R300_R2VB_DIVIDE_VERIFY");
            divide_verify = (e && strcmp(e, "1") == 0) ? 1 : 0;
        }
        /* Window-space producer + MVP-shaped matrix oracle only; clip-space
         * classify mode and non-MVP restage kernels use a different reference. */
        if (divide_verify && num_in == 1 && r300_r2vb_divide_enabled() &&
            r300->r2vb_produced_space == R300_R2VB_POSITION_WINDOW &&
            r300_vs_is_mvp(r300) && !r300_r2vb_clip_classify_enabled()) {
            if (r2vb_divide_oracle_selftest())
                r2vb_verify_window_readback(r300, clip, model, count, cols);
        }
    }

    /* Clip-classification oracle: classify the FP24 clip BO this producer
     * pass just wrote, then fall back to gallivm without delivering -- the
     * oracle changes no rendering.  The two-submit flush + map above already
     * ordered the producer against this CPU read. */
    if (r300_r2vb_clip_classify_enabled()) {
        r2vb_clip_classify_readback(r300, clip, count, einfo->mode);
        r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
        r300->r2vb_capture_clip = NULL;
        r2vb_redirty_app_raster_state(r300);
        pipe_resource_reference(&clip, NULL);
        free(model);
        return false;
    }

    /* Conservative clip-route action (R300_R2VB_CLIP_ROUTE): whole-draw
     * trivial accept re-runs the producer in window space and delivers
     * through the verbatim-fetch re-ingest; whole-draw trivial reject
     * consumes the draw with no delivery submit; anything else falls the
     * whole draw back to gallivm.  The action is scoped to the exactly
     * provable class: a non-indexed TRIANGLES list with a whole number of
     * triangles, single-input position, no user clip planes, and an
     * application FS that reads no external constants (the producer pass
     * overwrites FS constant file 0).  Indexed and instanced draws are
     * already rejected by r300_r2vb_classify_draw before this path. */
    /* Delivery draw parameters.  The clip-edge rebuild and the topology
     * gather both replace the vertex list, so their delivery draws vertices
     * 0..count-1 of the producer BOs instead of the application's
     * start/count window. */
    struct pipe_draw_start_count_bias rdraw = *draw;
    const struct pipe_draw_start_count_bias *ddraw = draw;
    if (topo_gathered) {
        rdraw.start = 0;
        rdraw.count = count;
        ddraw = &rdraw;
    }

    if (r300_r2vb_clip_route_enabled()) {
        const struct r300_rs_state *rs =
            (const struct r300_rs_state *)r300->rs_state.state;
        const struct r300_fragment_shader *afs = r300_fs(r300);
        bool fs_reads_externals = false;
        if (afs && afs->shader) {
            const struct rc_constant_list *cl = &afs->shader->code.constants;
            for (unsigned i = 0; i < cl->Count; i++)
                if (cl->Constants[i].Type == RC_CONSTANT_EXTERNAL)
                    fs_reads_externals = true;
        }
        const bool supported = einfo->mode == MESA_PRIM_TRIANGLES &&
                               count % 3 == 0 && num_in == 1 &&
                               rs && rs->rs.clip_plane_enable == 0 &&
                               !fs_reads_externals;
        enum r2vb_clip_route_verdict verdict = R2VB_CLIP_ROUTE_FALLBACK;
        if (supported)
            verdict = r2vb_clip_classify_readback(r300, clip, count, einfo->mode);
        const char *action = !supported ? "gallivm"
                           : verdict == R2VB_CLIP_ROUTE_ACCEPT ? "deliver"
                           : verdict == R2VB_CLIP_ROUTE_REJECT ? "consume"
                           : "gallivm";

        /* Edge generation (R300_R2VB_CLIP_EDGE): a non-trivial verdict is
         * rebuilt into a clipped TRIANGLES list instead of falling back.  A
         * rebuilt list is safe only when every delivered stream is position or
         * computed (producer-regenerated for the rebuilt vertices).  A
         * passthrough stream fetches the application buffer by original vertex
         * index, which the rebuilt list invalidates, so its presence declines
         * the clip action unless a CPU-interpolated replacement stream stands
         * in for it. */
        if (supported && verdict == R2VB_CLIP_ROUTE_FALLBACK &&
            r300_r2vb_clip_edge_enabled()) {
            /* Edge rebuild interpolates model inputs and re-runs the producer.
             * That is exact only for affine/MVP position transforms
             * (M*lerp(in) == lerp(M*in)); a non-affine restage op would place
             * generated vertices off the clip plane. */
            if (!r300_vs_is_mvp(r300)) {
                fprintf(stderr,
                        "r2vb_clip_edge decline (non-affine producer)\n");
            } else {
            int vslot_probe = -1;
            const char *ve = getenv("R300_R2VB_VARYING");
            if (ve && strcmp(ve, "1") == 0)
                vslot_probe = r300_r2vb_first_computed_varying(
                    r300_vs(r300)->state.ir.nir);
            struct r300_r2vb_reingest_stream st[PIPE_MAX_ATTRIBS];
            int ns = r300_r2vb_reingest_stream_layout(
                r300_vs(r300)->state.ir.nir, vslot_probe, st, PIPE_MAX_ATTRIBS);
            /* Every passthrough stream's application attribute is read to the
             * CPU so the rebuild can interpolate it; a source the CPU cannot
             * read in the float domain declines the action. */
            unsigned n_extra = 0;
            int extra_stream[R300_R2VB_CLIP_MAX_ATTRS - 1];
            float (*extras[R300_R2VB_CLIP_MAX_ATTRS - 1])[4] = { NULL };
            bool shape_ok = ns >= 1 &&
                            (vslot_probe >= 0 || r300->velems->count == 1);
            /* Fan retriangulation breaks default last-vertex flat PV: the first
             * fan triangle can provoke from an intersection.  Decline when the
             * rasterizer flat-shades and the draw carries any non-position
             * stream (passthrough or computed). */
            if (shape_ok && rs && rs->rs.flatshade &&
                (vslot_probe >= 0 || ns > 1))
                shape_ok = false;
            /* Passthrough attributes are read through the same index table the
             * position model-gather used -- the gathered triangle-index list
             * for a topology-gathered draw, or the identity draw->start+i
             * sequence otherwise -- so a rebuilt vertex's interpolated inputs
             * can never diverge from the position that drove the classify. */
            uint32_t *ridx = NULL;
            if (shape_ok) {
                if (topo_gathered) {
                    uint32_t rcount = 0;
                    if (!r2vb_topology_gather_indices(r300, info, draw, &ridx,
                                                       &rcount) ||
                        rcount != count)
                        shape_ok = false;
                } else {
                    ridx = malloc((size_t)count * sizeof(*ridx));
                    if (!ridx)
                        shape_ok = false;
                    else
                        for (uint32_t i = 0; i < count; i++)
                            ridx[i] = draw->start + i;
                }
            }
            for (int i = 0; shape_ok && i < ns; i++) {
                if (st[i].kind != R2VB_STREAM_PASSTHROUGH)
                    continue;
                if (n_extra >= R300_R2VB_CLIP_MAX_ATTRS - 1 ||
                    st[i].src_velem < 0) {
                    shape_ok = false;
                    break;
                }
                extras[n_extra] = r2vb_read_velem_floats(
                    r300, (unsigned)st[i].src_velem, ridx, count);
                if (!extras[n_extra]) {
                    shape_ok = false;
                    break;
                }
                extra_stream[n_extra++] = i;
            }
            free(ridx);
            /* Interpolated replacements deliver only through the per-output
             * reconstruction; the single-velem tail cannot carry them. */
            if (n_extra > 0 && vslot_probe < 0)
                shape_ok = false;
            if (shape_ok) {
                float (*cmodel)[4] = NULL;
                float (*cextras[R300_R2VB_CLIP_MAX_ATTRS - 1])[4] = { NULL };
                int32_t n2 = r2vb_clip_build_clipped_list(
                    r300, clip, count, (const float (*)[4])model, n_extra,
                    (float (*const *)[4])extras, &cmodel, cextras);
                fprintf(stderr,
                        "r2vb_clip_edge rebuild count=%u extras=%u -> %d\n",
                        count, n_extra, n2);
                if (n2 == 0) {
                    verdict = R2VB_CLIP_ROUTE_REJECT;
                    action = "consume";
                } else if (n2 > 0) {
                    struct pipe_resource *cbo = r2vb_create_selftest_bo(
                        r300, align((uint32_t)n2, 2) * 16, 0);
                    if (cbo) {
                        free(model);
                        model = cmodel;
                        count = (uint32_t)n2;
                        pipe_resource_reference(&clip, NULL);
                        clip = cbo;
                        rdraw.start = 0;
                        rdraw.count = count;
                        ddraw = &rdraw;
                        verdict = R2VB_CLIP_ROUTE_ACCEPT;
                        action = "clip";
                        /* Hand each interpolated passthrough attribute to the
                         * re-ingest, keyed by stream position. */
                        memset(r300->r2vb_edge_stream_attr, 0,
                               sizeof(r300->r2vb_edge_stream_attr));
                        for (unsigned j = 0; j < n_extra; j++)
                            r300->r2vb_edge_stream_attr[extra_stream[j]] =
                                cextras[j];
                        r300->r2vb_edge_streams_active = n_extra > 0;
                    } else {
                        free(cmodel);
                        for (unsigned j = 0; j < n_extra; j++)
                            free(cextras[j]);
                    }
                }
            } else {
                fprintf(stderr,
                        "r2vb_clip_edge decline (stream shape: ns=%d vslot=%d velems=%u)\n",
                        ns, vslot_probe, r300->velems->count);
            }
            for (unsigned j = 0; j < n_extra; j++)
                free(extras[j]);
            } /* affine/MVP edge rebuild */
        }
        {
            static int route_log = -1;
            if (route_log < 0) {
                const char *e = getenv("R300_R2VB_CLIP_LOG");
                route_log = (e && strcmp(e, "1") == 0) ? 1 : 0;
            }
            if (route_log)
                fprintf(stderr, "r2vb_clip_route supported=%d action=%s\n",
                        supported, action);
        }
        if (!supported || verdict == R2VB_CLIP_ROUTE_FALLBACK) {
            pipe_resource_reference(&clip, NULL);
            free(model);
            return false;
        }
        if (verdict == R2VB_CLIP_ROUTE_REJECT) {
            /* Every triangle is trivially outside one clip plane, so the
             * draw's correct image contribution is nothing.  Consuming it
             * with no delivery submit IS the rendering. */
            pipe_resource_reference(&clip, NULL);
            free(model);
            return true;
        }
        /* Trivial accept: every FP24 position is inside the guard band, so
         * the window-space producer's divide is safe and the verbatim fetch
         * is exact.  Classify from a clip-space pass and deliver from a
         * window-space re-run -- the dual producer is the proof-stage form;
         * a single pass exporting both spaces is a later optimization. */
        produced_space = R300_R2VB_POSITION_WINDOW;
        r300->r2vb_produced_space = produced_space;
        if (!r2vb_run_transform_producer(r300, clip, count, model, num_in,
                                         cols, draw->start, produced_space,
                                         restage, externals)) {
            r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
            r300->r2vb_capture_clip = NULL;
            r2vb_redirty_app_raster_state(r300);
            r2vb_edge_streams_release(r300);
            pipe_resource_reference(&clip, NULL);
            free(model);
            return false;
        }
        /* Both producer runs are complete here; the split kind on the context
         * proves the window-space run took the two-pass carry-BO producer, not
         * the single-pass restage.  Bypass before any re-ingest vertex-element
         * mutation: drop the R2VB resources, clear the producer bookkeeping so
         * the stale clip pointer cannot leak into a later capture, and decline
         * so gallivm renders the draw. */
        if (r300_r2vb_split_delivery_bypass_enabled() &&
            r300->r2vb_producer_kind == R300_R2VB_PRODUCER_SPLIT) {
            fprintf(stderr,
                    "r2vb_split_delivery_bypass producers=complete action=gallivm\n");
            r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
            r300->r2vb_capture_clip = NULL;
            r2vb_redirty_app_raster_state(r300);
            r2vb_edge_streams_release(r300);
            pipe_resource_reference(&clip, NULL);
            free(model);
            return false;
        }
        /* Fresh clip-copy: replace the split-produced clip BO with a
         * CPU-written copy in a buffer that was never a render target, then
         * deliver normally.  The run_split_producer tail's flush + read map
         * already ordered the pass-B write, so the read map here returns the
         * final window-space data; the fresh buffer is written before any
         * delivery state is touched and carries no GPU history.  A copy
         * failure falls the draw back to gallivm rather than delivering from
         * the suspect BO with the gate nominally on. */
        if (r300_r2vb_fresh_clip_copy_enabled() &&
            r300->r2vb_producer_kind == R300_R2VB_PRODUCER_SPLIT) {
            struct pipe_resource *fresh =
                r2vb_create_selftest_bo(r300, align(count, 2) * 16, 0);
            bool copied = false;
            r2vb_wait_bo(r300, clip);
            if (fresh) {
                struct pipe_transfer *sx = NULL, *dx = NULL;
                struct pipe_box cbox = { .width = (int)count * 16, .height = 1,
                                         .depth = 1 };
                const void *src = r300->context.buffer_map(&r300->context, clip,
                                                           0, PIPE_MAP_READ,
                                                           &cbox, &sx);
                void *dst = src ? r300->context.buffer_map(&r300->context,
                                                           fresh, 0,
                                                           PIPE_MAP_WRITE,
                                                           &cbox, &dx)
                                : NULL;
                if (src && dst) {
                    memcpy(dst, src, (size_t)count * 16);
                    copied = true;
                }
                if (dst)
                    r300->context.buffer_unmap(&r300->context, dx);
                if (src)
                    r300->context.buffer_unmap(&r300->context, sx);
            }
            if (!copied) {
                fprintf(stderr,
                        "r2vb_fresh_clip_copy copy_failed action=gallivm\n");
                pipe_resource_reference(&fresh, NULL);
                r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
                r300->r2vb_capture_clip = NULL;
                r2vb_edge_streams_release(r300);
                pipe_resource_reference(&clip, NULL);
                free(model);
                return false;
            }
            fprintf(stderr,
                    "r2vb_fresh_clip_copy bytes=%u src=%p dst=%p action=deliver\n",
                    count * 16, (void *)clip, (void *)fresh);
            pipe_resource_reference(&clip, NULL);
            clip = fresh;
            r300->r2vb_capture_clip = clip;
        }
    }

    /* Multi-input position oracle (R300_R2VB_POS_WEIGHTS="w0,w1,...").  The producer
     * feeds input a at VAR0+a, so for a VS whose position is M * sum_a(w_a * input_a)
     * the clip BO must hold that value.  Distinct weights make the check
     * non-commutative: a producer-feed swap of two inputs changes the result and
     * fails the readback, where a symmetric function (e.g. inA+inB) could not.  The
     * FP32 BO readback, not the 8-bit color, is the proof. */
    const char *posw = getenv("R300_R2VB_POS_WEIGHTS");
    if (posw) {
        float w[R300_R2VB_MAX_PRODUCER_INPUTS];
        unsigned nw = 0;
        char *p = (char *)posw;
        for (; *p && nw < num_in; nw++) {
            w[nw] = strtof(p, &p);
            while (*p == ',' || *p == ' ')
                p++;
        }
        for (; nw < num_in; nw++)
            w[nw] = 1.0f;
        float (*pexp)[4] = malloc((size_t)count * sizeof(*pexp));
        if (pexp) {
            for (unsigned s = 0; s < count; s++) {
                float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                for (unsigned a = 0; a < num_in; a++)
                    for (int k = 0; k < 4; k++)
                        acc[k] += w[a] * model[s * num_in + a][k];
                for (int i = 0; i < 4; i++) {
                    pexp[s][i] = 0.0f;
                    for (int j = 0; j < 4; j++)
                        pexp[s][i] += cols[j * 4 + i] * acc[j];
                }
            }
            r2vb_verify_bo_readback(r300, clip, pexp, count, 0.05f, "pos");
            free(pexp);
        }
    }

    /* Cayley-Dickson multi-input position oracle (R300_R2VB_POS_QUAT=1).  The VS
     * computes gl_Position = M * quat_rotate(q, inPos) from TWO inputs -- inPos at
     * velem[0], a unit quaternion q at velem[1] -- a genuinely bilinear transform
     * (products of q's components against inPos), unlike the linear POS_WEIGHTS sum.
     * The producer feeds input a at VAR0+a, so the re-staged FS reads inPos at VAR0
     * and q at VAR1.  The oracle is trusted by construction: a known-answer self-test
     * anchors the rotation convention, then the clip BO must hold M * quat_rotate(q,
     * inPos) per vertex (the FP32 readback, not the 8-bit color, is the proof). */
    if (getenv("R300_R2VB_POS_QUAT") && num_in == 2) {
        /* A failed self-test means the oracle itself is wrong, so it cannot vouch
         * for the producer.  Fail loudly rather than silently skip the readback,
         * which would turn an explicit validation mode into a non-verifying pass. */
        if (!r2vb_quat_oracle_selftest()) {
            fprintf(stderr, "r2vb_posquat_verify ABORT (quaternion oracle self-test failed)\n");
            pipe_resource_reference(&clip, NULL);
            free(model);
            return false;
        }
        float (*qexp)[4] = malloc((size_t)count * sizeof(*qexp));
        if (qexp) {
            for (unsigned s = 0; s < count; s++) {
                const float *vin = model[s * num_in + 0]; /* inPos (xyz, w) */
                const float *q = model[s * num_in + 1];   /* unit quaternion (x,y,z,w) */
                float rotated[3];
                r2vb_quat_rotate_unit(q, vin, rotated);
                float pos4[4] = { rotated[0], rotated[1], rotated[2], vin[3] };
                for (int i = 0; i < 4; i++) {
                    qexp[s][i] = 0.0f;
                    for (int j = 0; j < 4; j++)
                        qexp[s][i] += cols[j * 4 + i] * pos4[j];
                }
            }
            r2vb_verify_bo_readback(r300, clip, qexp, count, 0.05f, "posquat");
            free(qexp);
        }
    }

    /* Sedenion (CD-4) product oracle (R300_R2VB_SED=q0|q1|q2|q3).  The bound VS
     * computes one quarter of the 16-component product a*b for two sedenions a =
     * velem[0..3], b = velem[4..7] (num_in == 8 -- one octonion product per CD
     * level), and writes gl_Position = M * (that quarter).  This is the frontier:
     * CD-4 is the first level with zero divisors (two nonzero elements whose
     * product is zero), the signature the square cannot show (the square stays
     * composition-multiplicative).  Two distinct sedenions need 8 generic
     * interpolators -- exactly the R300 RS texcoord-unit count.  The oracle is
     * anchored by a known-answer self-test (e_i^2=-1 and the explicit zero divisor
     * (e1+e10)(e5+e14)=0), and the FP32 clip readback is the proof; feeding the
     * zero-divisor pair as a vertex makes that quarter read exact zero on silicon. */
    const char *sed = getenv("R300_R2VB_SED");
    if (sed && num_in == 8) {
        /* Exact q0..q3 only; any other value declines rather than defaulting. */
        if (!(sed[0] == 'q' && sed[1] >= '0' && sed[1] <= '3' && sed[2] == '\0')) {
            fprintf(stderr,
                    "r2vb_sed_verify ABORT (R300_R2VB_SED=%s; use exact q0|q1|q2|q3)\n",
                    sed);
            pipe_resource_reference(&clip, NULL);
            free(model);
            return false;
        }
        if (!r2vb_sed_oracle_selftest()) {
            fprintf(stderr, "r2vb_sed_verify ABORT (sedenion oracle self-test failed)\n");
            pipe_resource_reference(&clip, NULL);
            free(model);
            return false;
        }
        int quarter = sed[1] - '0';
        const char *tag = quarter == 0 ? "sed_q0" : quarter == 1 ? "sed_q1"
                        : quarter == 2 ? "sed_q2" : "sed_q3";
        float (*sexp)[4] = malloc((size_t)count * sizeof(*sexp));
        if (sexp) {
            for (unsigned s = 0; s < count; s++) {
                float a16[16], b16[16], prod[16];
                for (int v = 0; v < 4; v++)
                    for (int j = 0; j < 4; j++) {
                        a16[v * 4 + j] = model[s * num_in + v][j];
                        b16[v * 4 + j] = model[s * num_in + 4 + v][j];
                    }
                r2vb_sedenion_mul(a16, b16, prod);
                /* clip holds M * (selected quarter of a*b); cols is column-major M. */
                for (int i = 0; i < 4; i++) {
                    sexp[s][i] = 0.0f;
                    for (int j = 0; j < 4; j++)
                        sexp[s][i] += cols[j * 4 + i] * prod[quarter * 4 + j];
                }
            }
            r2vb_verify_bo_readback(r300, clip, sexp, count, 0.05f, tag);
            free(sexp);
        }
    }

    /* Q16.16 multi-limb MAC dump (R300_R2VB_QMAC=lo|hi).  The bound VS is the lean
     * MAC kernel: it reads two Q16.16 operands a, b and the relevant quarter of the
     * addend c as base-2^4 limbs (a = velem[0..1], b = velem[2..3], c-quarter =
     * velem[4], num_in == 5; the probe feeds c[0..3] for the lo pass and c[4..7] for
     * the hi pass so each half reads 5 contiguous velems), computes the convolution
     * + the >>16 truncation-carry + the +c add on the FP24 ALU, and emits the
     * UN-NORMALISED result columns (cols 0..3 for the lo VS, 4..7 for the hi VS) as
     * gl_Position (the probe pushes an identity matrix, so clip holds the columns
     * verbatim).  The carry chain is NOT on the GPU -- the trivial
     * positional recombine is the inherent limb->integer readback step, done by the
     * host harness, which reassembles value = sum col*16^i across the lo and hi
     * dumps, masks to 32 bits, and compares bit-exact to the int64 oracle TSV.  The
     * columns are small integers (< 2^17), printed exactly. */
    const char *qmac = getenv("R300_R2VB_QMAC");
    if (qmac && num_in == 5) {
        const char *half = (qmac[0] == 'h') ? "hi" : "lo";
        struct pipe_transfer *xfer = NULL;
        struct pipe_box box = { .width = count * 16, .height = 1, .depth = 1 };
        const float *got =
            r300->context.buffer_map(&r300->context, clip, 0, PIPE_MAP_READ, &box, &xfer);
        if (got) {
            for (unsigned s = 0; s < count; s++)
                fprintf(stderr, "r2vb_qmac_dump half=%s vtx=%u cols=%.1f,%.1f,%.1f,%.1f\n",
                        half, s, got[s * 4 + 0], got[s * 4 + 1], got[s * 4 + 2], got[s * 4 + 3]);
            r300->context.buffer_unmap(&r300->context, xfer);
        }
    }

    /* Generic per-vertex producer-output dump (R300_R2VB_DUMP=<tag>).  The bound VS
     * is any fragment-aluable straight-line kernel; this reads the producer clip BO
     * (gl_Position per vertex, computed on the FP24 ALU) and prints the four
     * components per vertex BEFORE any re-ingest.  The host harness recombines /
     * compares against the kernel's CPU oracle.  num_in-agnostic, so the nearest-
     * codeword (num_in 1), D8 lattice (num_in 2, lo/hi quarters), and any other
     * substrate-fit kernel reuse it without a bespoke gate. */
    const char *dump = getenv("R300_R2VB_DUMP");
    if (dump && dump[0]) {
        struct pipe_transfer *dxfer = NULL;
        struct pipe_box dbox = { .width = count * 16, .height = 1, .depth = 1 };
        const float *dg =
            r300->context.buffer_map(&r300->context, clip, 0, PIPE_MAP_READ, &dbox, &dxfer);
        if (dg) {
            for (unsigned s = 0; s < count; s++)
                fprintf(stderr, "r2vb_dump tag=%s vtx=%u v=%.4f,%.4f,%.4f,%.4f\n",
                        dump, s, dg[s * 4 + 0], dg[s * 4 + 1], dg[s * 4 + 2], dg[s * 4 + 3]);
            r300->context.buffer_unmap(&r300->context, dxfer);
        }
    }

    /* Computed-varying producer pass (R300_R2VB_VARYING): re-stage the bound VS
     * targeting its first computed varying and render that value on the fragment
     * ALU into a dedicated BO, the same proven single-RT producer the position
     * pass uses, fed the same velem[0] attribute.  R300_R2VB_VARYING_VERIFY=<k>
     * reads the BO back against model*k -- an FP24-exact input through an exact op
     * (k a power of two) stays bit-exact, so the tight readback, not the 8-bit
     * color, is the proof.
     *
     * When a computed varying is produced, RETURN before the re-ingest below.
     * That re-ingest is the unmodified passthrough path: it feeds varyings from
     * the application buffers, which cannot carry a computed varying, and a VS
     * with a computed-varying output mismatches its PSC/VAP setup and hangs the
     * draw (the timeout-kill then poisons the ring).  Skipping that re-ingest
     * isolates production-plus-oracle from the unmodified passthrough path
     * until the explicit R300_R2VB_REINGEST delivery branch below wires the
     * producer BO into the re-ingest path. */
    static int varying = -1;
    if (varying < 0) {
        const char *e = getenv("R300_R2VB_VARYING");
        varying = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    int vslot = varying ? r300_r2vb_first_computed_varying(r300_vs(r300)->state.ir.nir) : -1;
    if (vslot >= 0) {
        void *vfs = r300_r2vb_restage_vs_as_fs(r300, r300_vs(r300)->state.ir.nir,
                                               (gl_varying_slot)vslot,
                                               produced_space);
        struct pipe_resource *vbo = r2vb_create_selftest_bo(r300, align(count, 2) * 16, 0);
        if (vfs && vbo) {
            void *saved = r300->fs.state;
            void *saved_vs = r300->vs_state.state;
            /* The computed varying depends only on the first input, and a
             * computed-varying VS is kept single-input position (orthogonal to
             * multi-input position), so num_in == 1 and the producer feeds the one
             * attribute model holds at stride 1. */
            void *pvs = r300_r2vb_get_producer_vs(r300, 1);
            r300->context.bind_fs_state(&r300->context, vfs);
            if (pvs)
                r300->context.bind_vs_state(&r300->context, pvs);
            /* The re-staged varying FS reads any matrix through FS const
             * file 0, and the position pass restored the application
             * binding on its way out -- bind the producer matrix for this
             * pass explicitly rather than inheriting whatever is live. */
            r300_r2vb_set_transform_consts_raw(r300, cols);
            r300_update_derived_state(r300);
            bool vprepared =
                r300_r2vb_prepare_states(r300, 64 + (int)count * 8 + 64);
            if (vprepared)
                r300_r2vb_emit_producer(r300, r300_resource(vbo), 0, count, model, 1, true);
            r300->context.bind_fs_state(&r300->context, saved);
            if (pvs)
                r300->context.bind_vs_state(&r300->context, saved_vs);
            r300_r2vb_restore_app_fs_consts(r300);
            r300_update_derived_state(r300);
            if (!vprepared) {
                if (vfs)
                    r300->context.delete_fs_state(&r300->context, vfs);
                r2vb_edge_streams_release(r300);
                pipe_resource_reference(&vbo, NULL);
                pipe_resource_reference(&clip, NULL);
                free(model);
                r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
                r300->r2vb_capture_clip = NULL;
                r2vb_redirty_app_raster_state(r300);
                return false;
            }
            r300->context.flush(&r300->context, NULL, 0);
            const char *vv = getenv("R300_R2VB_VARYING_VERIFY");
            const char *vmat = getenv("R300_R2VB_VARYING_MAT");
            const char *vtolenv = getenv("R300_R2VB_VARYING_TOL");
            if (vv || vmat) {
                /* Expected = M * inPos (row-major R300_R2VB_VARYING_MAT, 16
                 * floats) for a matrix computed varying, else inPos * factor.  The
                 * matrix case is the lossier check: FP24 error accumulates across
                 * the four mul-adds per row, so the BO readback (not the 8-bit
                 * color) is the proof, and R300_R2VB_VARYING_TOL sets the FP24
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

            /* Re-ingest rewiring (R300_R2VB_REINGEST): point each VS output's
             * stream at its producer BO -- position from clip, the computed varying
             * from vbo -- and submit, or under R300_R2VB_INSPECT dump the wiring and
             * skip the submit.  Off by default; the producer pass + BO oracle above
             * remain the standing computed-varying proof, and this adds the
             * delivery-half wiring proof on top without disturbing it. */
            static int reingest = -1;
            if (reingest < 0) {
                const char *e = getenv("R300_R2VB_REINGEST");
                reingest = (e && strcmp(e, "1") == 0) ? 1 : 0;
            }
            /* Single-input position is the proven-safe submit class; a
             * computed-varying VS is kept single-input position upstream, so
             * re-confirm before this delivery submit.  The multi-input-position
             * re-ingest is a separate, explicitly-gated caller below. */
            if (reingest &&
                r300_r2vb_count_position_inputs(r300_vs(r300)->state.ir.nir) == 1)
                r300_r2vb_reingest_outputs(r300, einfo, ddraw, clip, vbo, vslot);
            else if (reingest)
                fprintf(stderr, "r2vb_reingest skip (computed-varying path is single-input position only)\n");
        }
        if (vfs)
            r300->context.delete_fs_state(&r300->context, vfs);
        r2vb_edge_streams_release(r300);
        pipe_resource_reference(&vbo, NULL);
        pipe_resource_reference(&clip, NULL);
        free(model);
        return true;
    }

    /* Multi-input position (num_in >= 2) re-ingest rides the R300_R2VB_REINGEST
     * opt-in, off by default: the producer pass and the clip BO oracle above stand
     * as the proof without a re-ingest draw.  When opted in it routes through the
     * per-output reconstruction below, which the earlier wedge made necessary --
     * the old multi-stream path kept the extra position-input velems as phantom
     * streams (velems > outputs) and that malformed fetch wedged the ring; the
     * reconstruction builds velems from outputs, so velems == outputs and the hard
     * invariant holds.  On RS482 the gated submit then completes and rasterizes,
     * boot-stable. */
    static int mvp_reingest = -1;
    if (mvp_reingest < 0) {
        const char *e = getenv("R300_R2VB_REINGEST");
        mvp_reingest = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    if (num_in >= 2) {
        if (!mvp_reingest) {
            pipe_resource_reference(&clip, NULL);
            free(model);
            return true;
        }
        /* Multi-input-position re-ingest, R300_R2VB_REINGEST opt-in.  Route through
         * the per-output reconstruction with no computed varying (vslot=-1, vbo
         * NULL): the producer already consumed the multiple position inputs, so the
         * VS outputs are position (+ any passthrough varyings) -- the extra inputs
         * are not outputs and do not appear in the velem set.  velems == outputs by
         * construction, so the hard invariant holds where the old multi-stream path,
         * which kept the extra input velems as phantom streams, submitted a
         * velem > output mismatch and wedged the ring.  R300_R2VB_INSPECT dumps the
         * wiring and skips the draw; without it one gated submit runs. */
        bool ok = r300_r2vb_reingest_outputs(r300, einfo, draw, clip, NULL, -1);
        pipe_resource_reference(&clip, NULL);
        free(model);
        return ok;
    }

    /* Per-output re-ingest for the single-input no-computed-varying delivery.
     * A velem[0]-only redirect keeps the application input-element set, so a
     * VS whose outputs outnumber its distinct sources -- two passthrough
     * varyings fed by one application vector -- fetches fewer dwords than
     * VAP_OUTPUT_VTX_FMT advertises: VAP_VTX_SIZE under-feeds the output tuple
     * and the GA waits forever for the missing vertex dwords.  Rebuilding one
     * vertex element per VS output lets two outputs copy the same source velem
     * (same buffer, same offset), so the fetch dword sum equals the output
     * tuple and the proven RS program is untouched. */
    struct r300_r2vb_reingest_stream ostreams[PIPE_MAX_ATTRIBS];
    int n_ostream = r300_r2vb_reingest_stream_layout(
        r300_vs(r300)->state.ir.nir, -1, ostreams, PIPE_MAX_ATTRIBS);
    /* Fetch dwords are derived from each element's src_format, the same
     * align(blocksize, 4) the delivery emit computes for LOAD_VBPNTR SIZE
     * and VAP_VTX_SIZE.  velems->format_size[] is has_tcl-only CSO state
     * and stays zero on SWTCL until that emit fills it. */
    unsigned fetch_dwords = 0;
    for (int i = 0; i < n_ostream; i++)
        fetch_dwords += (ostreams[i].kind == R2VB_STREAM_PASSTHROUGH &&
                         ostreams[i].src_velem >= 0 &&
                         (unsigned)ostreams[i].src_velem < r300->velems->count)
                            ? align(util_format_get_blocksize(
                                        r300->velems->velem[ostreams[i].src_velem]
                                            .src_format), 4) / 4
                            : 4;
    if (r2vb_exec_debug_enabled())
        fprintf(stderr,
                "r2vb_output_reingest mode=per_output outputs=%u app_velems=%u "
                "stream_layout=%d fetch_dwords=%u\n",
                r300->vertex_info.num_attribs, r300->velems->count, n_ostream,
                fetch_dwords);
    bool ok = r300_r2vb_reingest_outputs(r300, einfo, ddraw, clip, NULL, -1);
    if (!ok) {
        r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
        r300->r2vb_capture_clip = NULL;
        r2vb_redirty_app_raster_state(r300);
    }
    r2vb_edge_streams_release(r300);
    pipe_resource_reference(&clip, NULL);
    free(model);
    return ok;
}
