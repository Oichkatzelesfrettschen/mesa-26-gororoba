/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_H
#define R300_R2VB_H

#include <stdbool.h>
#include <stdint.h>

#include "compiler/shader_enums.h"

struct r300_context;
struct r300_resource;
struct r300_r2vb_producer_plan;
struct r300_vertex_shader;
struct pipe_fence_handle;
struct pipe_draw_info;
struct pipe_draw_start_count_bias;
struct pipe_resource;
struct pipe_vertex_element;
struct nir_shader;

/* Runtime source authority for a vertex-buffer record.  A real winsys BO
 * fetches in place.  A CPU-shadow resource uploads its exact fetched span.
 * User pointers and resources with zero or two backing authorities decline. */
enum r300_r2vb_model_source_kind {
    R300_R2VB_MODEL_UNSUPPORTED = 0,
    R300_R2VB_MODEL_REAL_BO,
    R300_R2VB_MODEL_CPU_SHADOW_UPLOAD,
};

enum r300_r2vb_model_source_kind
r300_r2vb_model_source_classify(bool is_user_buffer, bool has_winsys_bo,
                                bool has_cpu_shadow);

/* Delivery-stream admission stays typed so an automatic route never emits
 * producer work for a stream the VAP tuple or application buffer cannot
 * reproduce. */
enum r300_r2vb_delivery_stream_status {
    R300_R2VB_DELIVERY_STREAM_UNCHECKED = 0,
    R300_R2VB_DELIVERY_STREAM_OK,
    R300_R2VB_DELIVERY_STREAM_LAYOUT,
    R300_R2VB_DELIVERY_STREAM_INSTANCE_RATE,
    R300_R2VB_DELIVERY_STREAM_FORMAT,
    R300_R2VB_DELIVERY_STREAM_STRIDE,
    R300_R2VB_DELIVERY_STREAM_BUFFER_BINDING,
    R300_R2VB_DELIVERY_STREAM_SOURCE_CLASS,
    R300_R2VB_DELIVERY_STREAM_SPAN,
    R300_R2VB_DELIVERY_STREAM_CAPACITY,
    R300_R2VB_DELIVERY_STREAM_STATUS_COUNT,
};

enum r300_r2vb_delivery_stream_status
r300_r2vb_delivery_element_preflight(
    const struct pipe_vertex_element *element);

/* One re-ingest vertex stream: a VS output and where TCL_BYPASS fetches its data.
 * Streams follow r300 PSC/VAP output-vector order -- the same order
 * r300_draw_emit_all_attribs and r300_draw_fill_vs_outputs assign -- so stream
 * i drives velem i / PSC stream i / output vec i.  The exact FP32x4 delivery
 * contract admits full vector stores and refuses scalar PSIZ. */
enum r300_r2vb_reingest_kind { R2VB_STREAM_POS, R2VB_STREAM_COMPUTED, R2VB_STREAM_PASSTHROUGH };
struct r300_r2vb_reingest_stream {
    gl_varying_slot slot;
    enum r300_r2vb_reingest_kind kind;
    int src_velem;   /* passthrough: app velem index of the source input; else -1 */
    uint8_t components;
    uint8_t bit_size;
    uint8_t write_mask;
};

/* Enumerate the bound VS's outputs into output-vector order and classify each:
 * position, the one computed varying (slot == computed_slot, fetched from the
 * producer BO), or a passthrough (the store value is a straight load of an input
 * var, fetched from that input's application buffer).  Two outputs may record the
 * same source velem -- each still gets its own stream, because each populates its
 * own VAP output vector.  Returns the stream count, or -1 if a varying is neither
 * the computed one nor a mappable passthrough -- the caller then refuses the
 * submit rather than draw a mismatched layout.  Exported for the host layout
 * unit (r300_r2vb_reingest_layout_test). */
int r300_r2vb_reingest_stream_layout(struct nir_shader *vs, int computed_slot,
                                     struct r300_r2vb_reingest_stream *out, unsigned max);

/* Join the output-vector layout to the current application vertex bindings.
 * Every passthrough output must resolve to one exact FP32x4, per-vertex stream
 * whose complete draw span has one backing authority. */
enum r300_r2vb_delivery_stream_status
r300_r2vb_auto_single_output_streams_preflight(
    struct r300_context *r300, struct nir_shader *vs, int computed_slot,
    const struct pipe_draw_start_count_bias *draw);

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

/* Match the MVP route against a constant-folded clone of the supplied VS NIR.
 * The source shader remains unchanged. */
bool r300_r2vb_nir_is_mvp(struct nir_shader *nir);

/* Gated passthrough decision for r300_swtcl_draw_vbo.  R300_R2VB_ROUTE=1
 * classifies and tallies draws.  R300_R2VB_EXEC=1 admits only the identity-VS
 * passthrough class for direct TCL_BYPASS delivery; every other verdict enters
 * Draw. */
bool r300_r2vb_route_draw(struct r300_context *r300,
                          const struct pipe_draw_info *info,
                          const struct pipe_draw_start_count_bias *draw);

/* MVP route: gate plus execution.  route_mvp admits a fragment-ALU producer
 * candidate under R300_R2VB_MVP_EXEC or the complete AUTO_SINGLE contract.
 * exec_mvp_draw produces, classifies, and either delivers through TCL_BYPASS,
 * consumes a trivial reject, or declines into Draw. */
bool r300_r2vb_route_mvp(struct r300_context *r300,
                         const struct pipe_draw_info *info,
                         const struct pipe_draw_start_count_bias *draw);
bool r300_r2vb_exec_mvp_draw(struct r300_context *r300,
                             const struct pipe_draw_info *info,
                             const struct pipe_draw_start_count_bias *draw);
void r300_r2vb_auto_single_note_outcome(
    struct r300_context *r300,
    const struct pipe_draw_info *info,
    const struct pipe_draw_start_count_bias *draw,
    bool route_executed);

/* Remove telemetry cells owned by a vertex shader before its storage is
 * released.  The ledger deduplicates by shader identity and remains global
 * across contexts, so teardown must retire the pointer identity explicitly. */
void r300_r2vb_telemetry_cell_remove(const struct r300_vertex_shader *vs);

/* Test oracle for the measured application-input ranks used by telemetry.
 * The output order is position source followed by computed-varying source. */
unsigned r300_r2vb_telemetry_source_ranks_for_test(
    const struct r300_r2vb_producer_plan *plan, bool include_varying,
    uint8_t *out_ranks, unsigned max_ranks);

/* Test oracle for the DCE/re-stage survivor list feeding position telemetry.
 * Each result carries the physical application element and its semantic rank;
 * the output is ordered by the producer's VAR0 input order. */
unsigned r300_r2vb_telemetry_position_sources_for_test(
    const struct r300_r2vb_producer_plan *plan, struct nir_shader *vs_nir,
    uint8_t *out_driver_locations,
    uint8_t *out_location_ranks, unsigned max_sources);

/* The passthrough direct-VB re-ingest (defined in r300_render.c): re-ingest the
 * bound velems/vertex_buffers at TCL_BYPASS with the application FS and the HW
 * viewport transform.  The MVP route reuses it for the re-ingest by redirecting
 * only the position element to the producer's clip-space BO. */
bool r300_r2vb_exec_passthrough_draw(struct r300_context *r300,
                                     const struct pipe_draw_info *info,
                                     const struct pipe_draw_start_count_bias *draw);

/* Mark the restored application vertex-array binding for re-emission after
 * a producer delivery temporarily rewires the elements and buffers. */
void r300_r2vb_restore_vertex_array_state(struct r300_context *r300);

/* Compute the exact CPU-shadow prefix a passthrough upload must retain.
 * Every referenced element contributes its final fetched byte; overflow,
 * instance-rate elements, missing strides, and resource overrun decline. */
bool r300_r2vb_vertex_buffer_upload_end(
    const struct pipe_vertex_element *elements, unsigned element_count,
    unsigned vertex_buffer_index, uint32_t buffer_offset, uint32_t start,
    uint32_t count, uint32_t resource_width, uint32_t *upload_end);

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
void r300_r2vb_reserve_bo_draw_cs(struct r300_context *r300,
                                  unsigned cs_dwords);

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
 * from_flush=true so the loop appends to a CS a real draw has populated.  The
 * exact pair R300_HB_TCL=1 and R300_R2VB_TIMING=capture|submit selects the
 * transport.  Capture NOOP-flushes the IB without a DRM submit and declines an
 * active query.  Submit times a real flush and additionally requires
 * R300_RAW_SUBMIT_ACCEPTED=1.  Returns true when it consumed the CS; no-op
 * returning false when admission fails. */
bool r300_emit_rs482_r2vb_capture_selftest(struct r300_context *r300,
                                           bool from_flush,
                                           unsigned flush_flags,
                                           struct pipe_fence_handle **out_fence);

/* No-submit B0-B4 capture of the shipped producer BO-fetch draw across
 * the five widths {3, 2048, 2049, 4095, 4096}.  Gated on
 * R300_R2VB_SLOT_FETCH=1 plus R300_R2VB_BO_DRAW=capture, fires once from
 * flush, and flushes RADEON_FLUSH_NOOP so no IB reaches DRM_RADEON_CS. */
bool r300_r2vb_bo_draw_capture_selftest(struct r300_context *r300,
                                        bool from_flush);

#endif
