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
 * at reg.  The header encodes count - 1, so a zero count is -EINVAL.
 */
void r300_pm4_packet0(struct r300_pm4_builder *b, uint32_t reg,
                      const uint32_t *payload, uint32_t count);

/* The one-dword PACKET0 case. */
void r300_pm4_reg(struct r300_pm4_builder *b, uint32_t reg, uint32_t value);

/* PACKET3 header plus payload.  The header encodes count - 1, and a
 * zero-payload packet is legal, so count zero writes the header alone.
 */
void r300_pm4_packet3(struct r300_pm4_builder *b, uint32_t opcode,
                      const uint32_t *payload, uint32_t count);

void r300_pm4_block(struct r300_pm4_builder *b, const uint32_t *words,
                    uint32_t count);

/* The relocation form the radeon CS grammar takes: a type-3 NOP whose single
 * payload dword indexes the relocation chunk.  Returns the IB index that
 * payload landed at, which is the site a caller records, or
 * R300_PM4_NO_INDEX when the write refused.
 */
uint32_t r300_pm4_reloc_nop(struct r300_pm4_builder *b, uint32_t payload);

/* Publishes the dword count on success alone.  A builder that refused any
 * operation reports its first error and writes zero, so no caller reads a
 * partially written stream as a complete one.
 */
int r300_pm4_builder_finish(const struct r300_pm4_builder *b,
                            uint32_t *out_count);

#endif /* R300_PM4_BUILDER_H */
