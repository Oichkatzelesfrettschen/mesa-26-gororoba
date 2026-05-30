/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * r300_r2vb.c -- RS482 render-to-vertex-buffer (R2VB) synthesized-vertex loop.
 *
 * RS482 has no hardware vertex shader (num_vert_fpus = 0), so a normal draw
 * transforms vertices on the CPU through the gallivm SWTCL draw module.  The
 * R2VB idea moves the transform onto the fragment ALU: pass 1 renders the
 * transformed (clip-space) vertices into a GTT buffer through the color buffer,
 * a cache barrier makes them visible, and pass 2 re-ingests that same buffer as
 * the vertex array via an in-IB LOAD_VBPNTR, drawn by the VAP in TCL_BYPASS (the
 * pre-transformed, no-PVS path RS482 already uses for every SWTCL draw).
 *
 * Scope and safety.  The CB-write -> barrier -> vertex-fetch data path is
 * coherency-validated through the steinmarder GL oracle (a fragment-rendered
 * buffer, round-tripped into the vertex array, rasters the correct geometry).
 * What is unmeasured is the gallivm-free direct-VAP draw timing -- and emitting
 * that draw is a raw PM4 submit, which the RS482 hazard policy gates behind the
 * safe-regs evidence bundle.  This function is therefore built and grounded in
 * the driver's own verified emitters but is NOT yet wired to a caller; a live
 * submit waits on the hazard gate.  Constants are the real r300_reg.h values and
 * the barrier matches r300_emit_gpu_flush / the cb_flush_clean sequence in
 * r300_context.c; no speculative fallback defines.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/u_inlines.h"

#include "r300_context.h"
#include "r300_cs.h"
#include "r300_reg.h"
#include "r300_r2vb.h"

void r300_emit_rs482_r2vb_compute_loop(struct r300_context *r300,
                                       struct r300_resource *output_gart_bo,
                                       uint32_t output_gart_bo_offset,
                                       uint32_t num_vertices,
                                       struct r300_resource *stage3_color_bo,
                                       uint32_t stage3_width,
                                       uint32_t stage3_height)
{
    CS_LOCALS(r300);

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

    /* 36 dwords for the single-BO loop: stage 1 = 15, stage 2 = 6, stage 3 = 15.
     * OUT_CS_REG and OUT_CS_RELOC each emit two dwords; OUT_CS_REG_SEQ(reg,N)
     * emits one header plus its N values; the LOAD_VBPNTR body is seven dwords.
     * The stage-3 color-target switch adds nine dwords (COLOROFFSET0 + reloc +
     * COLORPITCH0 + the SC_SCISSORS pair). */
    BEGIN_CS(stage3_color_bo ? 45 : 36);

    /* Stage 1 -- render the transformed vertices into the GTT buffer.
     *
     * Point the color buffer at the GTT output BO with an FP32x4 color format so
     * each "pixel" the fragment program writes is one vec4 vertex, then draw.
     * VAP_VF_MAX_VTX_INDX is an inclusive maximum, so it is count-1, and the draw
     * packet must carry the vertex count in VAP_VF_CNTL NUM_VERTICES like every
     * other r300 vertex-list emitter -- without it the draw requests zero
     * vertices and produces no fragments.
     *
     * Contract (the build-out boundary, not a stub): the caller binds the
     * "vertex compute" fragment program, the GTT framebuffer, and the pass-1
     * geometry through the normal r300 pipe state path before calling this, so
     * r300_emit_dirty_state has already emitted the US instruction state, the RS
     * interpolator state, and the GA/SU setup into this same IB.  The US
     * microcode lives in the US instruction registers (r300_emit_fs in
     * r300_emit.c), never as a relocatable BO; this function deliberately does
     * not hand-roll the compiler's fragment-shader emit. */
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
    OUT_CS_REG(R300_RB3D_COLORPITCH0,
               num_vertices | R300_COLOR_FORMAT_ARGB32323232);
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS((num_vertices << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
           R300_VAP_VF_CNTL__PRIM_TRIANGLES |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST);

    /* Stage 2 -- the full cb_flush_clean barrier (r300_context.c / r300_emit_
     * gpu_flush).  Flush+free the ZB zcache and the RB3D dstcache tags, then halt
     * the CP microengine until the 3D engine is idle and the caches are evicted,
     * so the VAP reads the freshly written GTT data and not stale memory.  Both
     * the ZB and RB3D flushes are part of the verified sequence; emitting only
     * the RB3D half would be a subset, not the driver's full barrier. */
    OUT_CS_REG(R300_ZB_ZCACHE_CTLSTAT,
               R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
               R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
    OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT,
               R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
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
        OUT_CS_REG(R300_RB3D_COLORPITCH0,
                   stage3_width | R300_COLOR_FORMAT_ARGB32323232);
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
           (R300_SWIZZLE_SELECT_W << R300_SWIZZLE_SELECT_W_SHIFT) |
           (0xf << R300_WRITE_ENA_SHIFT));
    OUT_CS_PKT3(R300_PACKET3_3D_LOAD_VBPNTR, 3);
    OUT_CS(1 | R300_VC_FORCE_PREFETCH);
    OUT_CS(4 | (4 << 8));
    OUT_CS(output_gart_bo_offset);
    OUT_CS(0);
    OUT_CS(0xc0001000); /* PKT3_NOP -- the relocation form LOAD_VBPNTR expects */
    OUT_CS(r300->rws->cs_lookup_buffer(&r300->cs, output_gart_bo->buf) * 4);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS((num_vertices << R300_VAP_VF_CNTL__NUM_VERTICES__SHIFT) |
           R300_VAP_VF_CNTL__PRIM_TRIANGLES |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST);

    END_CS;
}

/* Gated self-test for the R2VB direct-VAP path.  R300_R2VB_TIMING picks the
 * mode:
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
bool r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300,
                                           bool from_flush)
{
    static bool fired = false;
    const char *mode = getenv("R300_R2VB_TIMING");
    if (!mode)
        return false;
    bool do_submit = strcmp(mode, "submit") == 0;
    if (!from_flush || fired)
        return false;
    if (do_submit) {
        const char *gate = getenv("R300_RAW_SUBMIT_ACCEPTED");
        if (!gate || strcmp(gate, "1") != 0) {
            fprintf(stderr,
                    "r2vb selftest: submit mode needs R300_RAW_SUBMIT_ACCEPTED=1\n");
            return false;
        }
    }

    uint32_t num_vertices = 64;
    const char *nv = getenv("R300_R2VB_NVERTS");
    if (nv) {
        long v = strtol(nv, NULL, 0);
        if (v > 0 && v < 65536)
            num_vertices = (uint32_t)v;
    }

    struct pipe_screen *pscreen = r300->context.screen;
    struct pipe_resource templ = {0};
    templ.target = PIPE_BUFFER;
    templ.format = PIPE_FORMAT_R8_UNORM;
    templ.width0 = num_vertices * 16;  /* one FP32x4 clip-space vertex per slot */
    templ.height0 = 1;
    templ.depth0 = 1;
    templ.array_size = 1;
    /* PIPE_BIND_CUSTOM is load-bearing: r300_buffer_create puts a plain vertex
     * buffer in RAM (rbuf->buf == NULL) on a no-TCL part like RS482, since SWTCL
     * vertices are CPU-side.  The R2VB path needs a real GART BO the GPU renders
     * to and fetches from, and PIPE_BIND_CUSTOM is the flag r300 uses to force
     * the winsys allocation instead of the RAM path. */
    templ.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_CUSTOM;
    templ.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *res = pscreen->resource_create(pscreen, &templ);
    if (!res)
        return false;

    /* Optional stage-3 observation target: a separate square GART BO the
     * re-ingested draw renders into, so a readback evidences the VAP fetch
     * (stage 3) and not just the stage-1 render.  s3dim defaults to 64 texels
     * per side, FP32x4; R300_R2VB_STAGE3_DIM overrides within the 64 KiB pitch
     * the loop's COLORPITCH0 can encode. */
    struct pipe_resource *stage3 = NULL;
    uint32_t s3dim = 64;
    const char *obs = getenv("R300_R2VB_STAGE3_OBSERVE");
    bool observe = obs && strcmp(obs, "1") == 0;
    if (observe) {
        const char *sd = getenv("R300_R2VB_STAGE3_DIM");
        if (sd) {
            long d = strtol(sd, NULL, 0);
            if (d > 0 && d <= 2048)
                s3dim = (uint32_t)d;
        }
        struct pipe_resource s3t = {0};
        s3t.target = PIPE_BUFFER;
        s3t.format = PIPE_FORMAT_R8_UNORM;
        s3t.width0 = s3dim * s3dim * 16;  /* FP32x4 2D, pitch = s3dim texels */
        s3t.height0 = 1;
        s3t.depth0 = 1;
        s3t.array_size = 1;
        s3t.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_CUSTOM;
        s3t.usage = PIPE_USAGE_DEFAULT;
        stage3 = pscreen->resource_create(pscreen, &s3t);
        if (!stage3) {
            pipe_resource_reference(&res, NULL);
            return false;
        }
        /* Sentinel-fill so a post-submit readback distinguishes rasterized
         * texels (overwritten by stage 3) from untouched ones. */
        struct pipe_transfer *fill_xfer = NULL;
        struct pipe_box box = {.x = 0, .y = 0, .z = 0,
                               .width = s3dim * s3dim * 16, .height = 1, .depth = 1};
        void *fill = r300->context.buffer_map(&r300->context, stage3, 0,
                                              PIPE_MAP_WRITE, &box, &fill_xfer);
        if (fill) {
            memset(fill, 0xff, s3dim * s3dim * 16);  /* -NaN sentinel per float */
            r300->context.buffer_unmap(&r300->context, fill_xfer);
        }
    }

    /* Burn the one-shot only once the GTT BO exists and the loop runs, so a
     * missing env gate or a failed allocation can still fire on a later flush. */
    fired = true;
    r300_emit_rs482_r2vb_compute_loop(r300, r300_resource(res), 0, num_vertices,
                                      stage3 ? r300_resource(stage3) : NULL,
                                      s3dim, s3dim);

    if (do_submit) {
        struct pipe_fence_handle *fence = NULL;
        struct timespec t0, t1;
        bool signalled = false;
        int flush_rc;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        flush_rc = r300->rws->cs_flush(&r300->cs, 0, &fence);
        if (fence) {
            /* Bounded wait, never OS_TIMEOUT_INFINITE: a wedged GPU must not hang
             * the process.  fence_wait returns false on timeout, so signalled=0
             * marks a likely hang for the caller's evidence capture. */
            signalled = r300->rws->fence_wait(r300->rws, fence,
                                              (uint64_t)5 * 1000 * 1000 * 1000);
            r300->rws->fence_reference(r300->rws, &fence, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                    (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr, "r2vb_direct_vap_timing nverts=%u submit_ms=%.4f "
                "flush_rc=%d fence=%d signalled=%d\n",
                num_vertices, ms, flush_rc, fence != NULL, signalled);
    } else {
        r300->rws->cs_flush(&r300->cs, RADEON_FLUSH_NOOP, NULL);
        fprintf(stderr, "r2vb_capture nverts=%u (no-submit; RADEON_FLUSH_NOOP)\n",
                num_vertices);
    }

    /* Read back the stage-3 target.  The fence wait above (submit mode) makes the
     * GPU writes coherent; in capture mode nothing was submitted, so the buffer
     * stays at the sentinel and the written count is zero -- a negative control
     * that the readback path itself is sound.  Count texels whose first float was
     * overwritten (no longer the 0xffffffff sentinel) and report the first one,
     * which is where the re-ingested geometry first rasterized. */
    if (stage3) {
        struct pipe_transfer *rd_xfer = NULL;
        struct pipe_box box = {.x = 0, .y = 0, .z = 0,
                               .width = s3dim * s3dim * 16, .height = 1, .depth = 1};
        const uint32_t *texels = r300->context.buffer_map(&r300->context, stage3, 0,
                                                          PIPE_MAP_READ, &box, &rd_xfer);
        if (texels) {
            /* A texel is "written" when stage 3 overwrote its sentinel.  Track
             * the count and the bounding box / centroid in texel coordinates
             * (x = i % dim, y = i / dim) so the caller can compare the observed
             * coverage against the region its known stage-1 vertices predict --
             * the evidence that stage 3 (the re-ingest) rasterized them. */
            uint32_t written = 0;
            uint32_t min_x = s3dim, min_y = s3dim, max_x = 0, max_y = 0;
            uint64_t sum_x = 0, sum_y = 0;
            for (uint32_t i = 0; i < s3dim * s3dim; i++) {
                if (texels[i * 4] != 0xffffffffu) {
                    uint32_t x = i % s3dim, y = i / s3dim;
                    if (x < min_x) min_x = x;
                    if (y < min_y) min_y = y;
                    if (x > max_x) max_x = x;
                    if (y > max_y) max_y = y;
                    sum_x += x;
                    sum_y += y;
                    written++;
                }
            }
            if (written)
                fprintf(stderr, "r2vb_stage3_readback dim=%ux%u written_texels=%u "
                        "bbox=%u,%u,%u,%u centroid=%.1f,%.1f\n",
                        s3dim, s3dim, written, min_x, min_y, max_x, max_y,
                        (double)sum_x / written, (double)sum_y / written);
            else
                fprintf(stderr, "r2vb_stage3_readback dim=%ux%u written_texels=0 "
                        "bbox=none centroid=none\n", s3dim, s3dim);
            r300->context.buffer_unmap(&r300->context, rd_xfer);
        }
    }

    pipe_resource_reference(&stage3, NULL);
    pipe_resource_reference(&res, NULL);
    return true;
}
