/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_H
#define R300_R2VB_H

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
 *
 * This sequence issues raw PM4 vertex/draw packets; on the RS482 hazard lane a
 * live submit is gated behind the safe-regs evidence bundle, so the function is
 * built but not yet wired to a caller.  The CB-write -> barrier -> vertex-fetch
 * data path is coherency-validated through the GL oracle; the gallivm-free
 * direct-VAP timing is the remaining hazard-gated measurement. */
void r300_emit_rs482_r2vb_compute_loop(struct r300_context *r300,
                                       struct r300_resource *output_gart_bo,
                                       uint32_t output_gart_bo_offset,
                                       uint32_t num_vertices);

/* Gated Phase-4 self-test (R300_R2VB_TIMING=capture|submit), called once from
 * r300_create_context.  capture mode NOOP-flushes so the IB is R300_TRACE-
 * captured without a DRM submit; submit mode times a real flush and additionally
 * requires R300_RAW_SUBMIT_ACCEPTED=1.  No-op when R300_R2VB_TIMING is unset. */
void r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300);

#endif
