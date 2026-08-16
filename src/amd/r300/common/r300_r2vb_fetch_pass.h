/*
 * SPDX-License-Identifier: MIT
 *
 * Neutral two-array BO-fetched R2VB draw body for R300-class silicon.
 */

#ifndef R300_R2VB_FETCH_PASS_H
#define R300_R2VB_FETCH_PASS_H

#include "r300_pm4_builder.h"

#include <stdint.h>

/* Symbolic ownership of a buffer object inside an R2VB transaction.  The
 * emitter reports relocation sites by role; the caller resolves roles to
 * live BOs when it assembles the submission, so the neutral body owns no
 * BO and no relocation-chunk index.
 */
enum r300_r2vb_bo_role {
   R300_R2VB_BO_SLOT,
   R300_R2VB_BO_MODEL,
   R300_R2VB_BO_CARRIER,
   R300_R2VB_BO_COLOR,
};

/* One fetched vertex array.  Sizes and strides are in dwords because the
 * VBPNTR control macros store dword units; offsets and BO bounds are in
 * bytes because the packet's pointer dwords and the kernel's relocation
 * arithmetic are byte-addressed.  relocation_payload is the dword the
 * caller wants in the NOP-form relocation that follows the packet --
 * typically the relocation-chunk index times four.
 */
struct r300_r2vb_fetch_stream {
   enum r300_r2vb_bo_role role;
   uint32_t size_dwords;
   uint32_t stride_dwords;
   uint32_t offset_bytes;
   uint64_t bo_size_bytes;
   uint32_t relocation_payload;
};

/* The caller-owned register state the fixed body writes verbatim.  All
 * eight PROG_STREAM_CNTL pairs are emitted, zeroed tail included: a
 * stale word from an inherited draw would add a phantom fetch.
 */
struct r300_r2vb_fetch_state {
   uint32_t vap_prog_stream_cntl[8];
   uint32_t vap_prog_stream_cntl_ext[8];
   /* VAP_VTX_SIZE: dwords the VAP consumes per vertex. */
   uint32_t fetch_dwords;
   uint32_t vap_vtx_state_cntl;
   uint32_t vap_vsm_vtx_assm;
   uint32_t vap_out_vtx_fmt[2];
   uint32_t gb_enable;
   uint32_t rs_ip[8];
   uint32_t rs_count;
   uint32_t rs_inst_count;
   uint32_t rs_inst[8];
};

struct r300_r2vb_fetch_pass_params {
   const struct r300_r2vb_fetch_state *state;
   /* Exactly two fetched arrays; the packet form is the two-array
    * LOAD_VBPNTR with one control dword.
    */
   struct r300_r2vb_fetch_stream stream[2];
   uint32_t vertex_count;
   /* VAP_VF_CNTL primitive selector, e.g.
    * R300_VAP_VF_CNTL__PRIM_POINTS.  The walk is always VERTEX_LIST.
    */
   uint32_t vf_prim;
};

/* IB dword indices of the two relocation payloads, in stream order, with
 * the roles they resolve through.
 */
struct r300_r2vb_fetch_pass_relocs {
   uint32_t ib_index[2];
   enum r300_r2vb_bo_role role[2];
};

/* The emission is shape-fixed and count-independent: PROG_STREAM_CNTL
 * 0..7 and EXT 0..7 (9 + 9), VTX_SIZE (2), VTX_STATE_CNTL (2),
 * VSM_VTX_ASSM (2), OUTPUT_VTX_FMT pair (3), GB_ENABLE (2), RS_IP 0..7
 * (9), RS_COUNT + RS_INST_COUNT (3), RS_INST 0..7 (9), the
 * VF_MAX/VF_MIN index-range run (3), the two-array LOAD_VBPNTR with
 * both NOP-form relocations (5 + 2 + 2), and DRAW_VBUF_2 (2).
 */
#define R300_R2VB_FETCH_PASS_DWORDS 64u

/* Emits the fixed fetched-draw body.  Rejects, without writing any
 * dword: a null state or reloc destination, a zero vertex count, a
 * vertex count past the 16-bit index registers, a stream whose size or
 * stride exceeds the VBPNTR field width, a size or stride not covering
 * the declared fetch, and a stream whose last fetched byte
 * (offset + stride * (count - 1) + size, in 64-bit arithmetic) lies
 * past its BO.  Returns 0, or a negative errno matching the builder's
 * first refusal.
 */
int r300_r2vb_fetch_pass_emit(struct r300_pm4_builder *b,
                              const struct r300_r2vb_fetch_pass_params *params,
                              struct r300_r2vb_fetch_pass_relocs *relocs);

#endif /* R300_R2VB_FETCH_PASS_H */
