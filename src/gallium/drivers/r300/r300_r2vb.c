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

    /* 24 dwords: stage 1 = 10, stage 2 = 4, stage 3 = 10.  Verify against the
     * OUT_CS_* macro widths if this is ever wired to a live submit; OUT_CS_REG
     * and OUT_CS_RELOC each emit two dwords. */
    BEGIN_CS(24);

    /* Stage 1 -- render the transformed vertices into the GTT buffer.
     *
     * Point the color buffer at the GTT output BO and set an FP32x4 color
     * format so each "pixel" the fragment program writes is one vec4 vertex.
     * The fragment-shader state (the "vertex compute" program) and the pass-1
     * geometry that drives the rasterizer must already be bound by the caller
     * through the normal r300 state path -- this function does not synthesize
     * fragment microcode.
     *
     * TODO: bind the vertex-compute fragment program and pass-1 geometry --
     *       emit the US instruction state through the r300_fragment_program /
     *       r300_emit_fs path (the microcode lives in the US instruction
     *       registers, it is not a relocatable BO).
     *       reason -- a correct fragment-shader emit is the r300 compiler's job
     *       and must not be hand-rolled here.
     *       tracking -- r300_emit_fs in r300_emit.c. */
    OUT_CS_REG(R300_RB3D_COLOROFFSET0, output_gart_bo_offset);
    OUT_CS_RELOC(output_gart_bo);
    OUT_CS_REG(R300_RB3D_COLORPITCH0,
               num_vertices | R300_COLOR_FORMAT_ARGB32323232);
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, num_vertices);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_TRIANGLES |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST);

    /* Stage 2 -- the verified cache barrier (cb_flush_clean in r300_context.c).
     * The CB writes pass through the color cache; flush dirty 3D tags and free
     * them, then halt the CP microengine until the 3D engine is idle and the
     * cache is evicted, so the VAP reads the freshly written GTT data and not
     * stale memory.  This is the same sequence r300_emit_gpu_flush emits. */
    OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT,
               R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
               R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
    OUT_CS_REG(RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);

    /* Stage 3 -- re-ingest the GTT buffer as the vertex array and draw it.
     *
     * Declare one FP32x4 input stream, then bind the same GTT BO the CB wrote as
     * the vertex array.  The LOAD_VBPNTR layout mirrors r300_emit_vertex_arrays_
     * swtcl in r300_emit.c: COUNT=3, then (num_arrays | force-prefetch), then
     * the format word size|(stride<<8) with size and stride in DWORDS (4 for an
     * FP32x4 vertex), then the offset, then the relocated BO.  RS482 sets
     * R300_VAP_TCL_BYPASS unconditionally (r300_state.c), so the VAP rasters
     * these pre-transformed vertices without invoking the (absent) PVS. */
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_0, 1);
    OUT_CS(R300_DATA_TYPE_FLOAT_4 | R300_LAST_VEC);
    OUT_CS_PKT3(R300_PACKET3_3D_LOAD_VBPNTR, 3);
    OUT_CS(1 | R300_VC_FORCE_PREFETCH);
    OUT_CS(4 | (4 << 8));
    OUT_CS(output_gart_bo_offset);
    OUT_CS_RELOC(output_gart_bo);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_TRIANGLES |
           R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST);

    END_CS;
}
