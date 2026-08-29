/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_pm4_builder.h"

#include "r300_reg.h"

#include <errno.h>
#include <string.h>

void
r300_pm4_builder_init(struct r300_pm4_builder *b, uint32_t *words,
                      uint32_t capacity)
{
   b->words = words;
   b->capacity = words != NULL ? capacity : 0;
   b->count = 0;
   b->error = words == NULL ? -EINVAL : 0;
}

bool
r300_pm4_builder_reserve(struct r300_pm4_builder *b, uint32_t needed)
{
   if (b->error != 0)
      return false;
   if (needed > b->capacity - b->count) {
      b->error = -ENOSPC;
      return false;
   }
   return true;
}

/* Admit a payload run and reserve it with its header.  The header's 14-bit
 * field carries payload_count - 1, so only 1..R300_PM4_MAX_RUN encode; the
 * bound also keeps payload_count + 1 exact, so the reservation stays the
 * whole gate and the copy below it takes the payload count as written.
 */
static bool
reserve_run(struct r300_pm4_builder *b, uint32_t payload_count)
{
   if (b->error != 0)
      return false;
   if (payload_count == 0 || payload_count > R300_PM4_MAX_RUN) {
      b->error = -EINVAL;
      return false;
   }
   return r300_pm4_builder_reserve(b, payload_count + 1);
}

void
r300_pm4_dword(struct r300_pm4_builder *b, uint32_t value)
{
   if (!r300_pm4_builder_reserve(b, 1))
      return;
   b->words[b->count++] = value;
}

void
r300_pm4_packet0(struct r300_pm4_builder *b, uint32_t reg,
                 const uint32_t *payload, uint32_t count)
{
   if (b->error != 0)
      return;
   if (payload == NULL) {
      b->error = -EINVAL;
      return;
   }
   if (!reserve_run(b, count))
      return;

   b->words[b->count++] = CP_PACKET0(reg, count - 1);
   memcpy(&b->words[b->count], payload, count * sizeof(*payload));
   b->count += count;
}

void
r300_pm4_reg(struct r300_pm4_builder *b, uint32_t reg, uint32_t value)
{
   r300_pm4_packet0(b, reg, &value, 1);
}

void
r300_pm4_packet3(struct r300_pm4_builder *b, uint32_t opcode,
                 const uint32_t *payload, uint32_t count)
{
   if (b->error != 0)
      return;
   /* A type-3 header names count - 1 following payload dwords, so an empty
    * payload has no encoding; the relocation NOP is the count-field-zero,
    * one-payload-dword case.
    */
   if (payload == NULL) {
      b->error = -EINVAL;
      return;
   }
   if (!reserve_run(b, count))
      return;

   b->words[b->count++] = CP_PACKET3(opcode, count - 1);
   memcpy(&b->words[b->count], payload, count * sizeof(*payload));
   b->count += count;
}

void
r300_pm4_block(struct r300_pm4_builder *b, const uint32_t *words,
               uint32_t count)
{
   if (b->error != 0)
      return;
   if (count == 0)
      return;
   if (words == NULL) {
      b->error = -EINVAL;
      return;
   }
   if (!r300_pm4_builder_reserve(b, count))
      return;

   memcpy(&b->words[b->count], words, count * sizeof(*words));
   b->count += count;
}

uint32_t
r300_pm4_reloc_nop(struct r300_pm4_builder *b, uint32_t payload)
{
   /* CP_PACKET3(..., 0) encodes one payload dword after the type-3 header. */
   if (!r300_pm4_builder_reserve(b, 2))
      return R300_PM4_NO_INDEX;

   b->words[b->count++] = CP_PACKET3(R300_PM4_PACKET3_NOP, 0);
   const uint32_t index = b->count;
   b->words[b->count++] = payload;
   return index;
}

void
r300_pm4_emit_vertex_index_range(struct r300_pm4_builder *b,
                                 uint32_t min_index, uint32_t max_index)
{
   if (b->error != 0)
      return;
   if (min_index > max_index || max_index > R300_PM4_VTX_INDX_LIMIT) {
      b->error = -EINVAL;
      return;
   }
   /* The run starts at the maximum register because 0x2134 precedes 0x2138. */
   const uint32_t payload[2] = {max_index, min_index};
   r300_pm4_packet0(b, R300_VAP_VF_MAX_VTX_INDX, payload, 2);
}

void
r300_pm4_emit_immediate_points(struct r300_pm4_builder *b,
                               uint32_t num_vertices, uint32_t vtx_dwords,
                               const uint32_t *vertex_dwords)
{
   if (b->error != 0)
      return;
   /* The VAP_VF_CNTL vertex count occupies bits 16-31 and the PACKET3
    * count field carries body_dwords - 1 in 14 bits, where the body is
    * VAP_VF_CNTL plus the vertex payload.
    */
   const uint64_t body_dwords =
      1u + (uint64_t)num_vertices * (uint64_t)vtx_dwords;
   if (num_vertices == 0 || num_vertices > R300_PM4_VTX_COUNT_LIMIT ||
       vtx_dwords == 0 || body_dwords - 1 > 0x3fffu ||
       vertex_dwords == NULL) {
      b->error = -EINVAL;
      return;
   }
   /* Reserve the complete seven-dword prefix and embedded vertex body before
    * storing any part of this multi-packet draw.  A short destination must
    * leave both the count and every destination word unchanged, matching the
    * builder's all-or-nothing operation contract.
    */
   const uint32_t payload_dwords = (uint32_t)(body_dwords - 1);
   if (!r300_pm4_builder_reserve(b, payload_dwords + 7u))
      return;
   r300_pm4_reg(b, R300_VAP_VTX_SIZE, vtx_dwords);
   r300_pm4_emit_vertex_index_range(b, 0, num_vertices - 1);
   r300_pm4_dword(b, CP_PACKET3(R300_PACKET3_3D_DRAW_IMMD_2,
                                (uint32_t)body_dwords - 1));
   r300_pm4_dword(b, R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED |
                        (num_vertices << 16) |
                        R300_VAP_VF_CNTL__PRIM_POINTS);
   r300_pm4_block(b, vertex_dwords, num_vertices * vtx_dwords);
}

int
r300_pm4_builder_finish(const struct r300_pm4_builder *b, uint32_t *out_count)
{
   *out_count = b->error == 0 ? b->count : 0;
   return b->error;
}
