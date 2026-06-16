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
                                       const float (*vertex_attrs)[4], uint32_t reingest_vf_prim,
                                       struct r300_resource *stage3_color_bo, uint32_t stage3_width,
                                       uint32_t stage3_height)
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
    /* Consumed only by the BEGIN_CS size assert (MESA_DEBUG builds); marked
     * UNUSED so a no-assert build does not warn on the dead accumulator. */
    UNUSED unsigned r2vb_vp_override_dwords = 0;
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

    /* 70 dwords for the single-BO loop: stage 1 = 47, stage 2 = 6, stage 3 = 17.
     * OUT_CS_REG and OUT_CS_RELOC each emit two dwords; OUT_CS_REG_SEQ(reg,N)
     * emits one header plus its N values; the LOAD_VBPNTR body is seven dwords;
     * the stage-1 3D_DRAW_IMMD body is one VF_CNTL dword plus three FP32x4
     * vertices (twelve dwords); SU_CULL_MODE, SC_CLIP_RULE, GA_POINT_SIZE, and
     * GA_POINT_MINMAX add eight dwords; the stage-3 VAP_VTX_SIZE reset and
     * VF_MAX_VTX_INDX re-assert add four.  The stage-3
     * color-target switch adds nine dwords (COLOROFFSET0 + reloc + COLORPITCH0 +
     * the SC_SCISSORS pair).  The identity-wpos override adds five dwords per
     * viewport state constant the bound FS carries (zero for a passthrough FS).
     * The producer reuses the trigger draw's PSC (no one-stream override) and
     * embeds num_vertices two-float4 points (num_vertices*8 dwords) where the base
     * counts assumed three one-float4 triangle vertices (twelve dwords) plus a
     * four-dword PSC override: net (num_vertices*8 - 16). */
    BEGIN_CS((stage3_color_bo ? 79 : 70) + r2vb_vp_override_dwords +
             (int)num_vertices * 8 - 16);

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
    /* Pin the fixed point size to one pixel (both registers pack 1/6-pixel units,
     * so size 1 = 6).  This keeps the stage-1 producer's own PRIM_POINTS a
     * deterministic one pixel instead of the size inherited from the trigger draw,
     * so the slot-to-pixel mapping stays exact.  It does NOT fix the stage-3
     * POINTS re-ingest (that remains an open item; see the VAP_VTX_SIZE note in
     * stage 3).  Both are don't-cares for the line and triangle topologies. */
    OUT_CS_REG(R300_GA_POINT_SIZE, (6 << R300_POINTSIZE_Y_SHIFT) |
                                       (6 << R300_POINTSIZE_X_SHIFT));
    OUT_CS_REG(R300_GA_POINT_MINMAX, (6 << R300_GA_POINT_MINMAX_MIN_SHIFT) |
                                         (6 << R300_GA_POINT_MINMAX_MAX_SHIFT));
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
    /* Mechanism A -- flat per-point attribute producer.  Each point carries TWO
     * FP32x4 streams: stream 0 = position (the slot's pixel, window coords);
     * stream 1 = the synthesized vertex, delivered to the fragment program as a
     * FLAT generic attribute.  The bound fragment program is a passthrough that
     * writes that attribute, so each slot's pixel gets its own vertex with no
     * gl_FragCoord dependency (gl_FragCoord was non-deterministic across point
     * size in the raw stage-1).  Do NOT override VAP_PROG_STREAM_CNTL: its
     * DST_VEC_LOC routing derives from the bound VS's output map
     * (r300_state_derived.c), so reusing the trigger draw's stream state is what
     * lands stream 1 on the FS generic input the passthrough reads.  Only the
     * vertex size grows to eight dwords (two float4). */
    OUT_CS_REG(R300_VAP_VTX_SIZE, 8);
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_IMMD_2, num_vertices * 8);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | (num_vertices << 16) |
           R300_VAP_VF_CNTL__PRIM_POINTS);
    for (uint32_t pv = 0; pv < num_vertices; pv++) {
        /* Stream 0: position = slot pv's pixel centre (window coords, w = 1).
         * One producer point per output slot indexes one BO row, regardless of
         * the topology the re-ingest draw later assembles from those rows. */
        OUT_CS_32F((float)pv + 0.5f);
        OUT_CS_32F(0.5f);
        OUT_CS_32F(0.0f);
        OUT_CS_32F(1.0f);
        /* Stream 1: the caller's window-space vertex for this slot (flat
         * attribute).  The passthrough fragment program writes it to the BO row.
         * The US_OUT_FMT C4_32_FP BGRA channel-select stores the fragment as
         * memory=(o.b,o.g,o.r,o.a), and stage 2 re-ingests memory order as the
         * vertex (x,y,z,w).  So the attribute is pre-swizzled to (z,y,x,w):
         * o.r=z, o.g=y, o.b=x, o.a=w -> memory=(x,y,z,w).  Emitting (x,y,z,w)
         * directly would store x in memory[2] and z in memory[0], collapsing
         * every re-ingested vertex's X to z. */
        OUT_CS_32F(vertex_attrs[pv][2]);   /* o.r = z */
        OUT_CS_32F(vertex_attrs[pv][1]);   /* o.g = y */
        OUT_CS_32F(vertex_attrs[pv][0]);   /* o.b = x */
        OUT_CS_32F(vertex_attrs[pv][3]);   /* o.a = w */
    }

    /* Stage 2 -- the full cb_flush_clean barrier (r300_context.c / r300_emit_
     * gpu_flush).  Flush+free the ZB zcache and the RB3D dstcache tags, then halt
     * the CP microengine until the 3D engine is idle and the caches are evicted,
     * so the VAP reads the freshly written GTT data and not stale memory.  Both
     * the ZB and RB3D flushes are part of the verified sequence; emitting only
     * the RB3D half would be a subset, not the driver's full barrier.
     *
     * R300_R2VB_BARRIER neuters individual components for timing bisection (the
     * dword count is unchanged -- a neutered register just gets a no-op value).
     * Substrings "nozb", "norb", "nowait" zero the ZB flush, RB3D flush, and the
     * WAIT_UNTIL idle-clean respectively.  This breaks CB->VAP coherency, so it is
     * valid ONLY for the no-readback throughput shape, to find which part of the
     * barrier carries the fixed per-submit GPU cost. */
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
    r300_emit_rs482_r2vb_compute_loop(r300, r300_resource(res), 0, cfg.num_vertices, attrs,
                                      vf_prim, stage3 ? r300_resource(stage3) : NULL,
                                      cfg.s3dim, cfg.s3dim);

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
    if (!enabled)
        return false;

    static unsigned tally[R2VB_VERDICT_COUNT];
    static unsigned total;
    enum r300_r2vb_verdict v = r300_r2vb_classify_draw(r300, info, draw);
    tally[v]++;
    total++;
    /* Periodic verdict distribution so a real workload shows how much of its draw
     * stream is route-eligible without per-draw log spam. */
    if (total == 1 || (total & 511u) == 0)
        fprintf(stderr,
                "r2vb_route_tally total=%u candidate=%u hw_tcl=%u indexed=%u "
                "instanced=%u count=%u prim=%u\n",
                total, tally[R2VB_ROUTE_CANDIDATE], tally[R2VB_REJECT_HW_TCL],
                tally[R2VB_REJECT_INDEXED], tally[R2VB_REJECT_INSTANCED],
                tally[R2VB_REJECT_COUNT], tally[R2VB_REJECT_PRIM]);

    /* Route EXECUTION is the deferred increment (the fragment-ALU producer that
     * turns this draw's VS + vertex arrays into the GART vertex buffer).  Until it
     * exists, every draw -- candidate or not -- falls back to gallivm, so the gate
     * is a zero-risk classifier. */
    return false;
}
