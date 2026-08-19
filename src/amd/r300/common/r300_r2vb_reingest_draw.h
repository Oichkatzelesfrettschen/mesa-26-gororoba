/*
 * SPDX-License-Identifier: MIT
 *
 * Neutral re-ingest draw body: binds a GPU-written buffer as the vertex
 * array of a follow-on draw on R300-class silicon, with optional state
 * re-assertions ahead of the fetch.
 */

#ifndef R300_R2VB_REINGEST_DRAW_H
#define R300_R2VB_REINGEST_DRAW_H

#include "r300_pm4_builder.h"

#include <stdbool.h>
#include <stdint.h>

/* Observation redirect: points the color buffer at a separate target and
 * scissors to its extent, so the re-ingested draw rasterizes there and
 * the source buffer's vertex data stays intact for comparison.  The
 * caller supplies finished register words; the pitch word carries the
 * color format, and the scissor words carry the guard-band-offset
 * encoding the emitting driver uses.
 */
struct r300_r2vb_reingest_redirect {
   uint32_t color_pitch_word;
   uint32_t scissor_tl_word;
   uint32_t scissor_br_word;
   uint32_t relocation_payload;
};

/* Rasterizer-interpolator re-assertion.  The re-ingest changes the VAP
 * output layout, and a stale RS routing from the trigger draw would
 * sample fragment inputs the re-ingest does not produce; the block
 * re-asserts GB_ENABLE plus the RS_IP/RS_COUNT/RS_INST tables from the
 * caller's derived rasterizer state.  table_len covers both tables
 * (the hardware pairs them at inst_count + 1 entries).
 */
struct r300_r2vb_reingest_rs_block {
   uint32_t gb_enable;
   uint32_t ip[8];
   uint32_t count_word;
   uint32_t inst_count_word;
   uint32_t inst[8];
   uint32_t table_len;
};

/* Every optional block defaults to absent, which keeps the emission
 * byte-identical to the bare re-ingest tail.  The fixed tail declares
 * one FP32x4 stream through the caller's finished PROG_STREAM_CNTL
 * words, resets VAP_VTX_SIZE to that stream's four dwords, bounds the
 * vertex-index pair to [0, vertex_count - 1], binds the source buffer
 * through the one-array LOAD_VBPNTR with its NOP-form relocation, and
 * issues the vertex-list DRAW_VBUF_2.
 */
struct r300_r2vb_reingest_draw_params {
   const struct r300_r2vb_reingest_redirect *redirect;
   /* VAP_OUTPUT_VTX_FMT pair for a position-only output layout. */
   bool position_only_output;
   uint32_t out_vtx_fmt0;
   uint32_t out_vtx_fmt1;
   /* Identity viewport: scale 1.0 and offset 0.0 on X, Y, and Z. */
   bool viewport_identity;
   /* VAP_VTE_CNTL override declaring X, Y, Z, and W already in window
    * space, so the VAP applies no perspective divide on the re-ingest;
    * vte_restore_word re-writes the caller's VTE selection after the
    * draw so later draws in the same buffer see the original value.
    */
   bool vte_w0;
   uint32_t vte_restore_word;
   /* VAP_VTX_STATE_CNTL + VAP_VSM_VTX_ASSM pair for a position-only
    * single-stream assembly. */
   bool vertex_assembly;
   uint32_t vtx_state_cntl;
   uint32_t vsm_vtx_assm;
   /* Zeroed SU_POLY_OFFSET_FRONT/BACK_SCALE/OFFSET run. */
   bool polygon_offset_clear;
   /* Zeroed VAP_PROG_STREAM_CNTL_1..7 run: a stale word from an
    * inherited draw would add a phantom fetch. */
   bool stream_control_clear;
   /* Zeroed GA_TRIANGLE_STIPPLE, GA_FOG_SCALE, GA_FOG_OFFSET. */
   bool fog_stipple_clear;
   const struct r300_r2vb_reingest_rs_block *rs;
   /* Finished PROG_STREAM_CNTL_0 / _EXT_0 words for the single
    * re-ingested stream. */
   uint32_t stream_cntl_word;
   uint32_t stream_cntl_ext_word;
   uint32_t vertex_count;
   /* R300_VAP_VF_CNTL primitive selector; the walk is VERTEX_LIST. */
   uint32_t vf_prim;
   uint32_t vertex_offset_bytes;
   uint64_t vertex_bo_size_bytes;
   uint32_t vertex_relocation_payload;
};

/* IB dword indices of the relocation payloads.  color is
 * R300_PM4_NO_INDEX when no redirect was requested.
 */
struct r300_r2vb_reingest_draw_relocs {
   uint32_t color_ib_index;
   uint32_t vertex_ib_index;
};

/* Dword total the emission produces for the given gate selection: the
 * 18-dword fixed tail plus each requested block (redirect 9,
 * position-only output 3, viewport identity 7, VTE override plus
 * restore 4, vertex assembly 3, polygon offset 5, stream control 8,
 * fog/stipple 6, rasterizer interpolator 7 + 2 * table_len).  A null
 * params or an RS block whose table_len is outside 1..8 returns 0;
 * every valid emission is at least the 18-dword tail, so 0 names an
 * invalid descriptor and never a real size.
 */
uint32_t r300_r2vb_reingest_draw_dwords(
   const struct r300_r2vb_reingest_draw_params *params);

/* Emits the re-ingest body in block order.  A null builder returns
 * -EINVAL.  Rejects, without writing any dword: a null params or
 * relocs destination, a vertex count of zero or past the 16-bit index
 * registers, a vertex stream whose last fetched byte (offset +
 * 16 * count in 64-bit arithmetic) lies past its BO, and an RS block
 * whose table_len is outside 1..8.  Returns 0, or a negative errno
 * matching the builder's first refusal.
 */
int r300_r2vb_reingest_draw_emit(
   struct r300_pm4_builder *b,
   const struct r300_r2vb_reingest_draw_params *params,
   struct r300_r2vb_reingest_draw_relocs *relocs);

#endif /* R300_R2VB_REINGEST_DRAW_H */
