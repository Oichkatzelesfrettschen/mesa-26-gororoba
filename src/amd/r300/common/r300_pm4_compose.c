/* SPDX-License-Identifier: MIT */

#include "r300_pm4_compose.h"

#include "r300_reg.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

int
r300_pm4_compose(struct r300_pm4_builder *b,
                 const struct r300_pm4_fragment *fragments,
                 uint32_t fragment_count,
                 const struct r300_pm4_role_map *roles,
                 struct r300_pm4_composition *out)
{
   if (b == NULL || fragments == NULL || roles == NULL || out == NULL)
      return -EINVAL;
   if (fragment_count == 0 ||
       fragment_count > (uint32_t)(sizeof(out->fragment_start) /
                                   sizeof(out->fragment_start[0])))
      return -EINVAL;

   /* First pass: validate everything and sum the sizes, so the copy
    * below never starts on a composition that cannot finish.
    */
   uint64_t total = 0;
   uint32_t reloc_total = 0;
   uint32_t writer_chunk_indices[R300_PM4_COMPOSE_MAX_RELOCS];
   uint32_t writer_count = 0;
   for (uint32_t f = 0; f < fragment_count; f++) {
      const struct r300_pm4_fragment *frag = &fragments[f];
      if (frag->dwords == NULL || frag->dword_count == 0)
         return -EINVAL;
      if (frag->reloc_count > 0 && frag->relocs == NULL)
         return -EINVAL;
      if (frag->reloc_count >
          R300_PM4_COMPOSE_MAX_RELOCS - reloc_total)
         return -E2BIG;
      total += frag->dword_count;
      if (total > UINT32_MAX)
         return -EOVERFLOW;
      for (uint32_t r = 0; r < frag->reloc_count; r++) {
         const struct r300_pm4_reloc_site *site = &frag->relocs[r];
         /* The payload sits behind its NOP header inside the fragment. */
         if (site->dword_index == 0 ||
             site->dword_index >= frag->dword_count)
            return -EINVAL;
         if (frag->dwords[site->dword_index - 1] !=
             (CP_PACKET3(R300_PM4_PACKET3_NOP, 0)))
            return -EINVAL;
         for (uint32_t previous = 0; previous < r; previous++) {
            if (frag->relocs[previous].dword_index == site->dword_index)
               return -EEXIST;
         }
         if ((unsigned)site->role >= 4)
            return -EINVAL;
         const int32_t chunk_index = roles->chunk_index[site->role];
         if (chunk_index < 0)
            return -ENOENT;
         if ((uint32_t)chunk_index > UINT32_MAX / 4u)
            return -EOVERFLOW;
         /* A resolved destination with two writers has no single owner. */
         if (site->write_domain != 0) {
            for (uint32_t previous = 0; previous < writer_count;
                 previous++) {
               if (writer_chunk_indices[previous] == (uint32_t)chunk_index)
                  return -EEXIST;
            }
            writer_chunk_indices[writer_count++] = (uint32_t)chunk_index;
         }
         reloc_total++;
      }
   }

   if (!r300_pm4_builder_reserve(b, (uint32_t)total))
      return b->error;

   memset(out, 0, sizeof(*out));
   out->fragment_count = fragment_count;
   for (uint32_t f = 0; f < fragment_count; f++) {
      const struct r300_pm4_fragment *frag = &fragments[f];
      const uint32_t base = b->count;
      out->fragment_start[f] = base;
      r300_pm4_block(b, frag->dwords, frag->dword_count);
      for (uint32_t r = 0; r < frag->reloc_count; r++) {
         const struct r300_pm4_reloc_site *site = &frag->relocs[r];
         const uint32_t parent_index = base + site->dword_index;
         /* The payload is the relocation-chunk dword index the role map
          * binds, whatever placeholder the fragment carried.
          */
         b->words[parent_index] =
            (uint32_t)roles->chunk_index[site->role] * 4u;
         struct r300_pm4_composed_reloc *dst =
            &out->relocs[out->reloc_count++];
         dst->ib_index = parent_index;
         dst->role = site->role;
         dst->read_domains = site->read_domains;
         dst->write_domain = site->write_domain;
      }
   }
   return b->error;
}
