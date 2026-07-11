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
struct pipe_fence_handle;
struct pipe_draw_info;
struct pipe_draw_start_count_bias;

/* Verdict from the simple-draw-class classifier: whether a draw is a candidate
 * for the fragment-ALU R2VB vertex route, or the reason it is not. */
enum r300_r2vb_verdict {
    R2VB_ROUTE_PASSTHROUGH = 0, /* simple class AND identity VS: re-ingest the app
                                 * vertex buffer directly at TCL_BYPASS, no transform */
    R2VB_ROUTE_CANDIDATE,     /* simple class, non-identity VS: needs the fragment-ALU
                               * transform producer before it can execute */
    R2VB_REJECT_HW_TCL,       /* has_tcl / num_vert_fpus != 0: not the SWTCL part */
    R2VB_REJECT_INDEXED,      /* indexed draw: producer indexes one slot per vertex */
    R2VB_REJECT_INSTANCED,    /* instance_count != 1 */
    R2VB_REJECT_COUNT,        /* 0 or >= 65536 (VAP_VF_MAX_VTX_INDX is 16-bit) */
    R2VB_REJECT_PRIM,         /* topology outside the proven set */
    R2VB_REJECT_FRONTFACE,    /* FS reads gl_FrontFacing: the TCL_BYPASS re-ingest
                               * skips the draw module, so the CPU face-injection
                               * stage never runs -- the two paths are exclusive */
    R2VB_VERDICT_COUNT
};

/* Classify one draw for the R2VB route. Pure inspection of info + draw + caps;
 * no side effects. */
enum r300_r2vb_verdict r300_r2vb_classify_draw(struct r300_context *r300,
                                               const struct pipe_draw_info *info,
                                               const struct pipe_draw_start_count_bias *draw);

/* Gated routing decision for r300_swtcl_draw_vbo.  Returns true only if the draw
 * should be executed via the R2VB route instead of the gallivm draw module.
 * Enabled by R300_R2VB_ROUTE=1; classifies + tallies each draw and logs the
 * verdict distribution.  Route EXECUTION (the fragment-ALU producer that turns a
 * real VS + vertex arrays into the GART vertex buffer) is a later increment, so
 * this currently returns false even for candidates -- a zero-risk classifier with
 * a gallivm fallback. */
bool r300_r2vb_route_draw(struct r300_context *r300,
                          const struct pipe_draw_info *info,
                          const struct pipe_draw_start_count_bias *draw);

/* MVP route: gate (R300_R2VB_MVP_EXEC) + exec.  route_mvp returns true for an
 * MVP-shape candidate draw under the opt-in; exec_mvp_draw runs the fragment-ALU
 * MVP transform producer (gl_Position = M * in_pos) under the normal draw flow
 * and returns false (gallivm fallback) until the re-ingest half is built. */
bool r300_r2vb_route_mvp(struct r300_context *r300,
                         const struct pipe_draw_info *info,
                         const struct pipe_draw_start_count_bias *draw);
bool r300_r2vb_exec_mvp_draw(struct r300_context *r300,
                             const struct pipe_draw_info *info,
                             const struct pipe_draw_start_count_bias *draw);

/* The passthrough direct-VB re-ingest (defined in r300_render.c): re-ingest the
 * bound velems/vertex_buffers at TCL_BYPASS with the application FS and the HW
 * viewport transform.  The MVP route reuses it for the re-ingest by redirecting
 * only the position element to the producer's clip-space BO. */
bool r300_r2vb_exec_passthrough_draw(struct r300_context *r300,
                                     const struct pipe_draw_info *info,
                                     const struct pipe_draw_start_count_bias *draw);

/* Emit the RS482 render-to-vertex-buffer (R2VB) synthesized-vertex loop into the
 * current command stream.  The caller binds the r300 fragment-shader state (the
 * "vertex compute" program) and the pass-1 geometry through the normal r300
 * state path BEFORE calling this; see the contract comment in r300_r2vb.c.
 * output_gart_bo is a GTT buffer that pass 1 renders into as an FP32x4 color
 * target and pass 2 re-ingests as the vertex array via an in-IB LOAD_VBPNTR,
 * consumed by the VAP in TCL_BYPASS.  The color-buffer pitch is aligned to an
 * even pixel count for 128bpp linear storage, so output_gart_bo must cover
 * align(num_vertices, 2) FP32x4 slots.
 * A PVS-bank proof route would only replace that stage-1 producer; the barrier
 * and re-ingest half stay the same.
 *
 * vertex_attrs is the per-slot window-space vertex the producer writes: one
 * (x, y, z, w) row per output slot, num_vertices rows.  x and y are pre-divided
 * window coordinates (the re-ingest draws under VTX_XY_FMT), z in [0,1], w = 1.
 * The producer stores each row through the BGRA C4_32_FP color path, so it
 * pre-swizzles the row to (z, y, x, w) on emit; the caller passes natural
 * (x, y, z, w).  reingest_vf_prim is the raw VAP_VF_CNTL primitive field for the
 * stage-3 draw (R300_VAP_VF_CNTL__PRIM_*), so the same producer drives any
 * topology -- points, lines, line strips/loops, triangles, strips, and fans --
 * the count and the topology are caller data, not baked into the loop.
 *
 * stage3_color_bo is an optional separate 2D target: when non-NULL the stage-3
 * re-ingested draw renders into it (stage3_width x stage3_height) instead of
 * overwriting output_gart_bo, so a CPU readback of stage3_color_bo evidences the
 * VAP fetch (stage 3) rather than only the stage-1 render.  Its storage must
 * cover align(stage3_width, 2) * stage3_height FP32x4 slots.  Pass NULL,0,0 for
 * the legacy single-BO loop.
 *
 * This sequence issues raw PM4 vertex/draw packets.  Capture mode flushes with
 * RADEON_FLUSH_NOOP so the IB can be decoded without a DRM submit; live submit
 * mode requires explicit R300_RAW_SUBMIT_ACCEPTED consent. */
void r300_emit_rs482_r2vb_compute_loop(struct r300_context *r300,
                                       struct r300_resource *output_gart_bo,
                                       uint32_t output_gart_bo_offset,
                                       uint32_t num_vertices,
                                       const float (*vertex_attrs)[4],
                                       uint32_t reingest_vf_prim,
                                       struct r300_resource *stage3_color_bo,
                                       uint32_t stage3_width,
                                       uint32_t stage3_height,
                                       bool transform_mode);

/* Reserve CS space + emit dirty state via the real prepare_for_rendering path
 * (defined in r300_render.c).  Used by the MVP producer to land a freshly bound
 * transform-FS + its const file in the IB. */
bool r300_r2vb_prepare_states(struct r300_context *r300, unsigned cs_dwords);

/* Emit one machine-greppable key=value line describing a pipe_resource's
 * underlying winsys BO identity: the buffer pointer, byte size, slab
 * suballocation, the reloc offset within its parent slab, virtual address,
 * initial domain, allocation flags, and the current command stream's relocation
 * index.  The radeon winsys does not expose the parent radeon_bo pointer to the
 * driver, so the parent is named only by its reloc offset.  The delivery capture
 * and the split-producer trace share this so their BO-identity lines match. */
void r300_r2vb_report_bo_identity(struct r300_context *r300, const char *tag,
                                  struct pipe_resource *pr);

/* Gated self-test for the RS482 HB_TCL umbrella, fired once from r300_flush with
 * from_flush=true so the loop appends to a CS a real draw has populated.
 * R300_HB_TCL=1 enables the umbrella; R300_R2VB_TIMING=capture|submit selects
 * the mode and defaults to capture when unset.  capture mode NOOP-flushes so
 * the IB is R300_TRACE-captured without a DRM submit; submit mode times a real
 * flush and additionally requires R300_RAW_SUBMIT_ACCEPTED=1.  Returns true
 * when it consumed the CS; no-op returning false when the umbrella is disabled,
 * from_flush is false, or it has already fired. */
bool r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300,
                                           bool from_flush,
                                           unsigned flush_flags,
                                           struct pipe_fence_handle **out_fence);

#endif
