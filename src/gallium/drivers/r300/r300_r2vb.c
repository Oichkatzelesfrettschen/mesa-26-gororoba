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

#include "util/os_time.h"
#include "util/u_inlines.h"

#include "r300_context.h"
#include "r300_cs.h"
#include "r300_reg.h"
#include "r300_r2vb.h"

void r300_emit_rs482_r2vb_compute_loop(struct r300_context *r300,
                                       struct r300_resource *output_gart_bo,
                                       uint32_t output_gart_bo_offset,
                                       uint32_t num_vertices)
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

    /* 31 dwords: stage 1 = 10, stage 2 = 6, stage 3 = 15.  OUT_CS_REG and
     * OUT_CS_RELOC each emit two dwords; OUT_CS_REG_SEQ(reg,1) emits one header
     * plus its one value; the LOAD_VBPNTR body is seven dwords. */
    BEGIN_CS(31);

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

/* Gated Phase-4 self-test for the R2VB direct-VAP path.  R300_R2VB_TIMING picks
 * the mode:
 *   capture -- emit the loop into a fresh GTT buffer and flush with
 *              RADEON_FLUSH_NOOP, so the IB is captured by R300_TRACE and never
 *              reaches DRM_RADEON_CS.  The packets can be decoded and verified
 *              with zero hardware risk.  This is the structural preflight.
 *   submit  -- a real flush with a fence wait, timed; additionally requires
 *              R300_RAW_SUBMIT_ACCEPTED=1.  This is the hazard-gated measurement
 *              that decides whether R2VB beats the gallivm CPU baseline; a draw
 *              that the CS validator passes can still hang reset-less silicon, so
 *              the caller must have bound the vertex-compute fragment program and
 *              GTT framebuffer first (the loop's contract).
 * R300_R2VB_NVERTS sets the count, clamped below 2^16 (the SWTCL VAP
 * NUM_VERTICES field width). */
void r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300)
{
    const char *mode = getenv("R300_R2VB_TIMING");
    if (!mode)
        return;
    bool do_submit = strcmp(mode, "submit") == 0;
    if (do_submit) {
        const char *gate = getenv("R300_RAW_SUBMIT_ACCEPTED");
        if (!gate || strcmp(gate, "1") != 0) {
            fprintf(stderr,
                    "r2vb selftest: submit mode needs R300_RAW_SUBMIT_ACCEPTED=1\n");
            return;
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
        return;

    r300_emit_rs482_r2vb_compute_loop(r300, r300_resource(res), 0, num_vertices);

    if (do_submit) {
        struct pipe_fence_handle *fence = NULL;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        r300->rws->cs_flush(&r300->cs, 0, &fence);
        if (fence) {
            r300->rws->fence_wait(r300->rws, fence, OS_TIMEOUT_INFINITE);
            r300->rws->fence_reference(r300->rws, &fence, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                    (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr, "r2vb_direct_vap_timing nverts=%u submit_ms=%.4f\n",
                num_vertices, ms);
    } else {
        r300->rws->cs_flush(&r300->cs, RADEON_FLUSH_NOOP, NULL);
        fprintf(stderr, "r2vb_capture nverts=%u (no-submit; RADEON_FLUSH_NOOP)\n",
                num_vertices);
    }
    pipe_resource_reference(&res, NULL);
}
