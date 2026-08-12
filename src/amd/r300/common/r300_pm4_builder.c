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
   /* Linux radeon consumes this BO reference through
    * radeon_cs_packet_next_reloc(), which parses a type-3 NOP and reads its
    * payload as a relocation-chunk dword index.  Symbol discovery uses
    * `rg --fixed-strings radeon_cs_packet_next_reloc
    * drivers/gpu/drm/radeon/` in the Linux kernel source tree.
    */
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
