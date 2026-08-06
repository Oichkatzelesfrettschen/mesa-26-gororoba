/* SPDX-License-Identifier: MIT */

#include "r300_pm4_builder.h"

#include "r300_reg.h"

#include <errno.h>
#include <string.h>

/* The radeon CS grammar for a BO reference: a type-3 NOP whose one payload
 * dword indexes the relocation chunk in dword units.
 */
#define R300_PM4_PACKET3_NOP 0x00001000

void
r300_pm4_builder_init(struct r300_pm4_builder *b, uint32_t *words,
                      uint32_t capacity)
{
   b->words = words;
   b->capacity = words != NULL ? capacity : 0;
   b->count = 0;
   b->error = words == NULL && capacity != 0 ? -EINVAL : 0;
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
   /* The header carries count - 1, so a run of zero registers has no
    * encoding.
    */
   if (count == 0 || payload == NULL) {
      b->error = -EINVAL;
      return;
   }
   if (!r300_pm4_builder_reserve(b, count + 1))
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
   /* A type-3 header carries count - 1 for a payload and encodes an empty
    * payload as zero, so only a null pointer with a nonzero count is
    * malformed.
    */
   if (count != 0 && payload == NULL) {
      b->error = -EINVAL;
      return;
   }
   if (!r300_pm4_builder_reserve(b, count + 1))
      return;

   b->words[b->count++] = CP_PACKET3(opcode, count == 0 ? 0 : count - 1);
   if (count != 0) {
      memcpy(&b->words[b->count], payload, count * sizeof(*payload));
      b->count += count;
   }
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
   if (!r300_pm4_builder_reserve(b, 2))
      return R300_PM4_NO_INDEX;

   b->words[b->count++] = CP_PACKET3(R300_PM4_PACKET3_NOP, 0);
   const uint32_t index = b->count;
   b->words[b->count++] = payload;
   return index;
}

int
r300_pm4_builder_finish(const struct r300_pm4_builder *b, uint32_t *out_count)
{
   *out_count = b->error == 0 ? b->count : 0;
   return b->error;
}
