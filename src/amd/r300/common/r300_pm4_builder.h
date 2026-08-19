/*
 * SPDX-License-Identifier: MIT
 *
 * Capacity-checked PM4 writer for the fixed R300 cells.
 */

#ifndef R300_PM4_BUILDER_H
#define R300_PM4_BUILDER_H

#include <stdbool.h>
#include <stdint.h>

/* A destination the writer never leaves in a half-written state.  Every
 * operation reserves its whole dword run before it stores anything, so a
 * refusal leaves the words written so far intact and the count where it was.
 * error holds the first refusal and every later operation is a no-op, which
 * keeps the reported failure the one that caused the rest.
 */
struct r300_pm4_builder {
   uint32_t *words;
   uint32_t capacity;
   uint32_t count;
   int error;
};

/* The index a reloc payload lands at when the write refuses. */
#define R300_PM4_NO_INDEX UINT32_MAX

/* Both packet headers carry the payload run length as a 14-bit count - 1
 * field, so one to 0x4000 payload dwords have an encoding and no other
 * length does.
 */
#define R300_PM4_MAX_RUN 0x4000u

/* The radeon CS grammar for a BO reference: a type-3 NOP whose single
 * payload dword indexes the relocation chunk in dword units.
 */
#define R300_PM4_PACKET3_NOP 0x00001000

/* A null destination is a malformed builder whatever its capacity, so init
 * records -EINVAL and every operation stays a no-op.
 */
void r300_pm4_builder_init(struct r300_pm4_builder *b, uint32_t *words,
                           uint32_t capacity);

/* Reports whether needed dwords fit, and records -ENOSPC when they do not.
 * The comparison runs as needed > capacity - count, which is exact for every
 * uint32_t: count <= capacity holds from init onward, so the subtraction
 * cannot borrow and no sum can wrap past the capacity it is checked against.
 */
bool r300_pm4_builder_reserve(struct r300_pm4_builder *b, uint32_t needed);

void r300_pm4_dword(struct r300_pm4_builder *b, uint32_t value);

/* PACKET0 header plus payload: a register write run of count dwords starting
 * at reg.  A count outside 1..R300_PM4_MAX_RUN has no header encoding and is
 * -EINVAL.
 */
void r300_pm4_packet0(struct r300_pm4_builder *b, uint32_t reg,
                      const uint32_t *payload, uint32_t count);

/* The one-dword PACKET0 case. */
void r300_pm4_reg(struct r300_pm4_builder *b, uint32_t reg, uint32_t value);

/* PACKET3 header plus payload.  The header names count - 1 following payload
 * dwords, so an empty payload has no encoding: CP_PACKET3(op, 0) tells the
 * parser one payload dword follows.  A count outside 1..R300_PM4_MAX_RUN is
 * -EINVAL.
 */
void r300_pm4_packet3(struct r300_pm4_builder *b, uint32_t opcode,
                      const uint32_t *payload, uint32_t count);

void r300_pm4_block(struct r300_pm4_builder *b, const uint32_t *words,
                    uint32_t count);

/* Linux radeon consumes this BO reference through
 * radeon_cs_packet_next_reloc(), which parses a type-3 NOP and reads its
 * single payload dword as a relocation-chunk dword index.  Symbol discovery
 * uses (rg --fixed-strings radeon_cs_packet_next_reloc
 * drivers/gpu/drm/radeon/) in the Linux kernel source tree.  Returns the IB
 * index that payload landed at, which is the site a caller records, or
 * R300_PM4_NO_INDEX when the write refused.
 */
uint32_t r300_pm4_reloc_nop(struct r300_pm4_builder *b, uint32_t payload);

/* The vertex-fetch index bound registers hold 16-bit indices. */
#define R300_PM4_VTX_INDX_LIMIT 0xffffu

/* One PACKET0 run over the adjacent VAP_VF_MAX_VTX_INDX (0x2134) and
 * VAP_VF_MIN_VTX_INDX (0x2138) pair, maximum first.  Both registers clamp
 * every fetched vertex index, so a draw that writes only the maximum
 * inherits the previous client's minimum and folds low indices onto it;
 * the pair establishes the complete bound.  min_index > max_index or an
 * index above R300_PM4_VTX_INDX_LIMIT is -EINVAL, recorded without
 * writing any dword.
 */
void r300_pm4_emit_vertex_index_range(struct r300_pm4_builder *b,
                                      uint32_t min_index,
                                      uint32_t max_index);

/* Embedded POINTS draw with an inline vertex body: VAP_VTX_SIZE, the
 * complete vertex-index-range pair over [0, num_vertices - 1], the
 * 3D_DRAW_IMMD_2 header, VAP_VF_CNTL declaring an embedded vertex walk
 * of num_vertices points, then num_vertices * vtx_dwords payload words
 * copied verbatim.  The caller owns the vertex facts (slot coordinates,
 * attribute ordering); this helper owns the packet grammar and sizing.
 * num_vertices outside [1, R300_PM4_VTX_INDX_LIMIT], vtx_dwords of
 * zero, a body whose dword count overflows the PACKET3 14-bit field, or
 * a null payload is -EINVAL, recorded without writing any dword.
 */
void r300_pm4_emit_immediate_points(struct r300_pm4_builder *b,
                                    uint32_t num_vertices,
                                    uint32_t vtx_dwords,
                                    const uint32_t *vertex_dwords);

/* Dword total r300_pm4_emit_immediate_points produces for a given
 * geometry: the seven-dword prefix (VTX_SIZE run, index-range run,
 * PACKET3 header, VAP_VF_CNTL) plus the embedded body. */
#define R300_PM4_IMMEDIATE_POINTS_DWORDS(num_vertices, vtx_dwords) \
   (7u + (num_vertices) * (vtx_dwords))

/* Publishes the dword count on success alone.  A builder that refused any
 * operation reports its first error and writes zero, so no caller reads a
 * partially written stream as a complete one.
 */
int r300_pm4_builder_finish(const struct r300_pm4_builder *b,
                            uint32_t *out_count);

#endif /* R300_PM4_BUILDER_H */
