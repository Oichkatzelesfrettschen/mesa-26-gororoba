/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_H
#define R300_R2VB_H

#include <stdbool.h>
#include <stdint.h>

struct r300_context;
struct r300_resource;

/* Emit the RS482 render-to-vertex-buffer (R2VB) synthesized-vertex loop into the
 * current command stream.  The caller binds the r300 fragment-shader state (the
 * "vertex compute" program) and the pass-1 geometry through the normal r300
 * state path BEFORE calling this; see the contract comment in r300_r2vb.c.
 * output_gart_bo is a GTT buffer that pass 1 renders into as an FP32x4 color
 * target and pass 2 re-ingests as the vertex array via an in-IB LOAD_VBPNTR,
 * consumed by the VAP in TCL_BYPASS.
 * A future hazard-gated PVS-bank proof lane would only replace that stage-1
 * producer; the barrier and the re-ingest/oracle half stay the same.
 *
 * stage3_color_bo is an optional separate 2D target: when non-NULL the stage-3
 * re-ingested draw renders into it (stage3_width x stage3_height) instead of
 * overwriting output_gart_bo, so a CPU readback of stage3_color_bo evidences the
 * VAP fetch (stage 3) rather than only the stage-1 render.  Pass NULL,0,0 for the
 * legacy single-BO loop.
 *
 * This sequence issues raw PM4 vertex/draw packets; on the RS482 hazard lane a
 * live submit is gated behind the safe-regs evidence bundle.  The CB-write ->
 * barrier -> vertex-fetch data path is coherency-validated through the GL
 * oracle; the gallivm-free direct-VAP timing is the remaining hazard-gated
 * measurement. */
void r300_emit_rs482_r2vb_compute_loop(struct r300_context *r300,
                                       struct r300_resource *output_gart_bo,
                                       uint32_t output_gart_bo_offset,
                                       uint32_t num_vertices,
                                       struct r300_resource *stage3_color_bo,
                                       uint32_t stage3_width,
                                       uint32_t stage3_height);

/* Gated self-test (R300_R2VB_TIMING=capture|submit), fired once from r300_flush
 * with from_flush=true so the loop appends to a CS a real draw has populated.
 * capture mode NOOP-flushes so the IB is R300_TRACE-captured without a DRM
 * submit; submit mode times a real flush and additionally requires
 * R300_RAW_SUBMIT_ACCEPTED=1.  Returns true when it consumed the CS (the caller
 * then skips its own flush); no-op returning false when R300_R2VB_TIMING is
 * unset, from_flush is false, or it has already fired. */
bool r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300,
                                           bool from_flush);

#endif
