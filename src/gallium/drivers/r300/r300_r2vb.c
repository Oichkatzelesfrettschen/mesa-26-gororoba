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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/u_inlines.h"

#include "r300_context.h"
#include "r300_cs.h"
#include "r300_emit.h"
#include "r300_fs.h"
#include "r300_r2vb.h"
#include "r300_reg.h"
#include "r300_state_inlines.h"

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

void r300_emit_rs482_r2vb_compute_loop(struct r300_context *r300,
                                       struct r300_resource *output_gart_bo,
                                       uint32_t output_gart_bo_offset, uint32_t num_vertices,
                                       struct r300_resource *stage3_color_bo, uint32_t stage3_width,
                                       uint32_t stage3_height)
{
    CS_LOCALS(r300);

    assert(num_vertices > 0 && num_vertices <= 65535);
    assert(r300->screen->caps.num_vert_fpus == 0);
    assert(!r300->screen->caps.has_tcl);
    assert(!stage3_color_bo || (stage3_width > 0 && stage3_height > 0));

    if (num_vertices == 0 || num_vertices > 65535)
        return;
    if (r300->screen->caps.has_tcl || r300->screen->caps.num_vert_fpus != 0)
        return;
    if (stage3_color_bo && (stage3_width == 0 || stage3_height == 0))
        return;

    /* FP32x4 linear color targets need an even-pixel pitch; the scissor remains
     * the logical vertex count so the padding pixel is not rendered. */
    uint32_t output_pitch = align(num_vertices, 2);
    uint32_t stage3_pitch = align(stage3_width, 2);

    /* The output BO is both the pass-1 color target and the pass-2 vertex array,
     * so register it in the command stream once, read+write, before any
     * relocation or lookup -- the radeon winsys returns -1 from cs_lookup_buffer
     * for a BO that was never added, which would emit an invalid relocation. */
    r300->rws->cs_add_buffer(&r300->cs, output_gart_bo->buf,
                             RADEON_USAGE_READWRITE | RADEON_USAGE_SYNCHRONIZED |
                                 RADEON_PRIO_COLOR_BUFFER,
                             RADEON_DOMAIN_GTT);

    /* Stage-3 observation target (optional).  When present, the stage-3 draw
     * renders into this separate 2D BO instead of overwriting the stage-1 vertex
     * data in output_gart_bo, so a CPU readback of stage3_color_bo shows where
     * the re-ingested vertices rasterized -- evidence of the VAP fetch (stage 3),
     * not just the stage-1 render.  NULL keeps the legacy single-BO loop. */
    if (stage3_color_bo)
        r300->rws->cs_add_buffer(&r300->cs, stage3_color_bo->buf,
                                 RADEON_USAGE_READWRITE | RADEON_USAGE_SYNCHRONIZED |
                                     RADEON_PRIO_COLOR_BUFFER,
                                 RADEON_DOMAIN_GTT);

    /* Identity wpos for the window-coord covering triangle.  Stage 1 rasterizes
     * a VTX_XY_FMT (pre-divided window-coord) covering triangle, but the bound
     * fragment program's gl_FragCoord reconstruction (rc_transform_fragment_wpos)
     * computes window = clip.xy/clip.w * VIEWPORT_SCALE + VIEWPORT_OFFSET,
     * re-applying the trigger draw's viewport.  That collapses gl_FragCoord to a
     * constant across the few covered pixels, so every BO slot receives the same
     * synthesized vertex.  Override the two viewport state constants with
     * scale = 1, offset = 0 so the reconstruction is identity and gl_FragCoord
     * is the true window position, indexing one BO slot per pixel.  Immediates
     * precede the state constants, so the physical slots are not fixed: scan the
     * bound FS's constant list for them (the same list r300_emit_fs_rc_constant_
     * state walks).  A wpos-free producer has none and the loop emits no
     * override. */
    struct r300_fragment_shader *r2vb_fs = r300_fs(r300);
    struct rc_constant_list *r2vb_consts =
        r2vb_fs && r2vb_fs->shader ? &r2vb_fs->shader->code.constants : NULL;
    unsigned r2vb_vp_override_dwords = 0;
    if (r2vb_consts) {
        for (unsigned i = 0; i < r2vb_consts->Count; i++) {
            unsigned t = r2vb_consts->Constants[i].Type;
            unsigned s = r2vb_consts->Constants[i].u.State[0];
            if (t == RC_CONSTANT_STATE &&
                (s == RC_STATE_R300_VIEWPORT_SCALE ||
                 s == RC_STATE_R300_VIEWPORT_OFFSET))
                r2vb_vp_override_dwords += 5; /* OUT_CS_REG_SEQ(reg,4) = 5 dwords */
        }
    }

    /* 64 dwords for the single-BO loop: stage 1 = 43, stage 2 = 6, stage 3 = 15.
     * OUT_CS_REG and OUT_CS_RELOC each emit two dwords; OUT_CS_REG_SEQ(reg,N)
     * emits one header plus its N values; the LOAD_VBPNTR body is seven dwords;
     * the stage-1 3D_DRAW_IMMD body is one VF_CNTL dword plus three FP32x4
     * vertices (twelve dwords); SU_CULL_MODE and SC_CLIP_RULE add four dwords;
     * the stage-3 VF_MAX_VTX_INDX re-assert adds two.  The stage-3 color-target
     * switch adds nine dwords (COLOROFFSET0 + reloc + COLORPITCH0 + the
     * SC_SCISSORS pair).  The identity-wpos override adds five dwords per
     * viewport state constant the bound FS carries.  The stage-1 producer draw
     * now embeds num_vertices points (num_vertices*4 dwords) where the base
     * counts assumed three triangle vertices (twelve dwords). */
    BEGIN_CS((stage3_color_bo ? 73 : 64) + r2vb_vp_override_dwords +
             (int)num_vertices * 4 - 12 + 2 /* GA_POINT_SIZE */);

    /* Stage 1 -- render the transformed vertices into the GTT buffer.
     *
     * Point the color buffer at the GTT output BO with an FP32x4 color format so
     * each "pixel" the bound fragment program writes is one vec4 vertex, then
     * rasterize the whole num_vertices x 1 target with a self-supplied covering
     * triangle (below).  Stage 1 owns its geometry: an inherited DRAW_VBUF_2
     * against the caller's vertex array produced zero fragments after the
     * caller's flush, leaving the BO unwritten, so the loop no longer depends on
     * the caller's post-flush vertex-array state.
     *
     * Contract (the build-out boundary, not a stub): the caller still binds the
     * "vertex compute" fragment program through the normal r300 pipe state path,
     * so r300_emit_dirty_state has emitted the US instruction state, the RS
     * interpolator state, and the GA/SU setup into this same IB.  The US microcode
     * lives in the US instruction registers (r300_emit_fs in r300_emit.c), never
     * as a relocatable BO; this function deliberately does not hand-roll the
     * compiler's fragment-shader emit, and the covering triangle's vertex values
     * are ignored by a wpos-only program that writes from gl_FragCoord.
     *
     * A hazard-gated RS482 PVS-bank proof would replace only this producer half.
     * Stages 2 and 3 only consume a clip-space FP32x4 vertex buffer, so the
     * barrier and re-ingest/oracle half stay valid whether stage 1 was
     * fragment-generated or produced by an explicit PVS experiment. */
    /* Disable depth: this color-only vertex render needs no Z test, and the
     * radeon CS validator (r300_cs_track_check) defaults z_enabled true with a
     * NULL z buffer, so a draw with ZB_CNTL R300_Z_ENABLE still set but no depth
     * BO bound is rejected ("No buffer for z buffer").  Clearing R300_Z_ENABLE
     * makes the loop pass the validator regardless of the preceding draw's depth
     * state. */
    OUT_CS_REG(R300_ZB_CNTL, 0);
    /* Scissor to the num_vertices x 1 GTT target.  The CS validator derives the
     * color-buffer bound from SC_SCISSORS_BR (r300_cs_track_check:
     * maxy = (BR_Y >> 13) + 1, less the 1440 R300_SCISSORS_OFFSET on pre-RV515),
     * then rejects the draw if pitch * cpp * maxy exceeds the BO.  The GTT BO is
     * one row of num_vertices FP32x4 texels, so height 1 yields maxy = 1 and the
     * exact-fit bound passes; inheriting the caller's full-height scissor would
     * fail ("Buffer too small for color buffer").  Encoded like
     * r300_emit_scissor_state for the non-r500 path. */
    OUT_CS_REG_SEQ(R300_SC_SCISSORS_TL, 2);
    OUT_CS((1440 << R300_SCISSORS_X_SHIFT) | (1440 << R300_SCISSORS_Y_SHIFT));
    OUT_CS(((num_vertices + 1440 - 1) << R300_SCISSORS_X_SHIFT) |
           ((1 + 1440 - 1) << R300_SCISSORS_Y_SHIFT));
    OUT_CS_REG(R300_RB3D_COLOROFFSET0, output_gart_bo_offset);
    OUT_CS_RELOC(output_gart_bo);
    OUT_CS_REG(R300_RB3D_COLORPITCH0, output_pitch | R300_COLOR_FORMAT_ARGB32323232);
    /* Write the fragment output as FP32x4, not the caller's 8-bit format.  The
     * fragment program's gl_FragColor is cast to the color buffer per
     * US_OUT_FMT_0; inheriting the caller's C4_8 (ARGB8888) format truncates each
     * channel to 8 bits, so an FP32x4 BO reads back ~0.  C4_32_FP with the BGRA
     * channel select (matching the ARGB32323232 memory order) stores each
     * fragment as four 32-bit floats, so the re-ingest reads the synthesized
     * vertex (x,y,z,w) from memory order (b,g,r,a).  Stage 3 inherits this. */
    OUT_CS_REG(R300_US_OUT_FMT_0, R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_B | R300_C1_SEL_G |
                                      R300_C2_SEL_R | R300_C3_SEL_A);
    /* Identity wpos: override the bound FS's VIEWPORT_SCALE (-> 1) and
     * VIEWPORT_OFFSET (-> 0) state constants at their physical slots so
     * gl_FragCoord = the window position the covering triangle rasterizes, one
     * BO slot per pixel.  Non-r500 FS constants are FP24 PFS_PARAM registers at
     * R300_PFS_PARAM_0_X + slot*16; the wpos MAD reads .xyz0 so the w lane is
     * a don't-care. */
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
    /* Re-assert the two GA->RE primitive-reject gates the caller's last draw
     * leaves in an unknown state.  SU_CULL_MODE (0x42B8) is cleared so neither
     * winding is culled -- the covering triangle's winding is fixed here, not by
     * the caller -- and SC_CLIP_RULE (0x43D0) is set to 0xFFFF so every clip
     * combination passes, matching r300_blitter_draw_rectangle, which writes
     * SC_CLIP_RULE on every immediate draw.  Without these, RBBM_STATUS reads
     * VAP_BUSY and GA_BUSY (the vertices transform and the primitive assembles)
     * but RE_BUSY and RB3D_BUSY stay zero: the triangle is rejected at setup, the
     * rasterizer never runs, and the GTT color BO reads back all zero. */
    OUT_CS_REG(R300_SU_CULL_MODE, 0);
    OUT_CS_REG(R300_SC_CLIP_RULE, 0xFFFF);
    /* One-pixel points.  The loop reuses the caller's GA_POINT_SIZE, which is
     * undefined for a caller that drew no points, so an oversized point would
     * spill onto neighbouring slots' pixels and two producer points would
     * collide on a shared pixel (the later point winning, leaving a slot with
     * the wrong synthesized vertex).  Pin size 1.0 so point pv covers exactly
     * pixel pv. */
    OUT_CS_REG(R300_GA_POINT_SIZE,
               (pack_float_16_6x(1.0f) << R300_POINTSIZE_X_SHIFT) |
               (pack_float_16_6x(1.0f) << R300_POINTSIZE_Y_SHIFT));
    /* Self-supplied producer geometry: one POINT per output slot.  Declare one
     * FP32x4 position stream with pre-divided window coordinates (VTX_XY_FMT),
     * then emit num_vertices points at (slot+0.5, 0.5) in-IB via 3D_DRAW_IMMD.
     * A covering triangle was the first attempt, but the bound FS's gl_FragCoord
     * (reconstructed wpos) interpolates FLAT across a triangle, collapsing every
     * fragment to one vertex's window position, so every BO slot received the
     * same synthesized vertex.  One point per slot makes each fragment its own
     * primitive at its own pixel: gl_FragCoord = that point's window position
     * (slot+0.5) even under flat shading, so the wpos producer writes a distinct
     * synthesized vertex per slot.  The embedded vertices need no relocation --
     * they travel in the command stream -- so this draw is independent of any
     * vertex-array BO.
     *
     * Disable clipping (R300_CLIP_DISABLE), as r300_blitter_draw_rectangle does
     * for its immediate-mode draw: with VTX_XY_FMT the vertices are pre-divided
     * window coordinates, so the clipper -- which expects clip space -- would
     * otherwise reject the whole triangle and produce no fragments. */
    OUT_CS_REG(R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
    OUT_CS_REG(R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
    OUT_CS_REG(R300_VAP_VTX_SIZE, 4);
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_0, 1);
    OUT_CS(R300_DATA_TYPE_FLOAT_4 | R300_LAST_VEC);
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_EXT_0, 1);
    OUT_CS((R300_SWIZZLE_SELECT_X << R300_SWIZZLE_SELECT_X_SHIFT) |
           (R300_SWIZZLE_SELECT_Y << R300_SWIZZLE_SELECT_Y_SHIFT) |
           (R300_SWIZZLE_SELECT_Z << R300_SWIZZLE_SELECT_Z_SHIFT) |
           (R300_SWIZZLE_SELECT_W << R300_SWIZZLE_SELECT_W_SHIFT) | (0xf << R300_WRITE_ENA_SHIFT));
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_IMMD_2, num_vertices * 4);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | (num_vertices << 16) |
           R300_VAP_VF_CNTL__PRIM_POINTS);
    for (uint32_t pv = 0; pv < num_vertices; pv++) {
        OUT_CS_32F((float)pv + 0.5f); /* window x = centre of slot pv's pixel */
        OUT_CS_32F(0.5f);             /* window y = row 0 centre */
        OUT_CS_32F(0.0f);             /* z */
        OUT_CS_32F(1.0f);             /* w (VTX_XY_FMT pre-divided, w = 1) */
    }

    /* Stage 2 -- the full cb_flush_clean barrier (r300_context.c / r300_emit_
     * gpu_flush).  Flush+free the ZB zcache and the RB3D dstcache tags, then halt
     * the CP microengine until the 3D engine is idle and the caches are evicted,
     * so the VAP reads the freshly written GTT data and not stale memory.  Both
     * the ZB and RB3D flushes are part of the verified sequence; emitting only
     * the RB3D half would be a subset, not the driver's full barrier. */
    OUT_CS_REG(R300_ZB_ZCACHE_CTLSTAT, R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                                           R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
    OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT, R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                                               R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
    OUT_CS_REG(RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);

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
    /* Re-assert the vertex-index bound for the re-ingest draw.  Stage 1 set
     * VAP_VF_MAX_VTX_INDX to 2 for its three-vertex covering triangle; stage 3
     * draws num_vertices vertices from the GTT array, so an inherited bound of 2
     * clamps every index above 2 and the DRAW_VBUF fetches a degenerate vertex
     * set that rasterizes nothing.  Bound it to the actual highest index. */
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_LOAD_VBPNTR, 3);
    OUT_CS(1 | R300_VC_FORCE_PREFETCH);
    OUT_CS(4 | (4 << 8));
    OUT_CS(output_gart_bo_offset);
    OUT_CS(0);
    OUT_CS(0xc0001000); /* PKT3_NOP -- the relocation form LOAD_VBPNTR expects */
    OUT_CS(r300->rws->cs_lookup_buffer(&r300->cs, output_gart_bo->buf) * 4);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS((num_vertices << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
           R300_VAP_VF_CNTL__PRIM_TRIANGLES | R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST);

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
 * flush.  R300_R2VB_NVERTS sets the count, clamped below 2^16 (the SWTCL VAP
 * NUM_VERTICES field width). */
struct r2vb_selftest_config {
    bool enabled;
    bool do_submit;
    bool observe;
    uint32_t num_vertices;
    uint32_t s3dim;
};

static void r2vb_get_selftest_config(struct r2vb_selftest_config *cfg)
{
    const char *hb_tcl = getenv("R300_HB_TCL");
    const char *mode = getenv("R300_R2VB_TIMING");
    cfg->enabled = (hb_tcl && strcmp(hb_tcl, "1") == 0) || (mode != NULL);
    if (!cfg->enabled)
        return;

    cfg->do_submit = (mode && strcmp(mode, "submit") == 0);

    cfg->num_vertices = 64;
    const char *nv = getenv("R300_R2VB_NVERTS");
    if (nv) {
        long v = strtol(nv, NULL, 0);
        if (v > 0 && v < 65536)
            cfg->num_vertices = (uint32_t)v;
    }

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

bool r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300, bool from_flush)
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

    struct pipe_resource *res = r2vb_create_selftest_bo(r300, align(cfg.num_vertices, 2) * 16, 0);
    if (!res)
        return false;

    struct pipe_resource *stage3 = NULL;
    if (cfg.observe) {
        stage3 = r2vb_create_selftest_bo(r300, cfg.s3dim * cfg.s3dim * 16, 0xff);
        if (!stage3) {
            pipe_resource_reference(&res, NULL);
            return false;
        }
    }

    fired = true;
    r300_emit_rs482_r2vb_compute_loop(r300, r300_resource(res), 0, cfg.num_vertices,
                                      stage3 ? r300_resource(stage3) : NULL, cfg.s3dim, cfg.s3dim);

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

    if (cfg.do_submit) {
        struct pipe_fence_handle *fence = NULL;
        struct timespec t0, t1;
        bool signalled = false;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int flush_rc = r300->rws->cs_flush(&r300->cs, 0, &fence);
        if (fence) {
            signalled = r300->rws->fence_wait(r300->rws, fence, (uint64_t)5 * 1000 * 1000 * 1000);
            r300->rws->fence_reference(r300->rws, &fence, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr,
                "r2vb_direct_vap_timing nverts=%u submit_ms=%.4f "
                "flush_rc=%d signalled=%d hb_tcl=1 hb_vert_fpu=%u\n",
                cfg.num_vertices, ms, flush_rc, signalled, r300->screen->caps.num_vert_fpus);
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

    pipe_resource_reference(&stage3, NULL);
    pipe_resource_reference(&res, NULL);
    return true;
}
