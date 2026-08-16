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
   if (fragments == NULL || roles == NULL || out == NULL)
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
   bool role_written[4] = {false};
   for (uint32_t f = 0; f < fragment_count; f++) {
      const struct r300_pm4_fragment *frag = &fragments[f];
      if (frag->dwords == NULL || frag->dword_count == 0)
         return -EINVAL;
      if (frag->reloc_count > 0 && frag->relocs == NULL)
         return -EINVAL;
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
         if ((unsigned)site->role >= 4)
            return -EINVAL;
         if (roles->chunk_index[site->role] < 0)
            return -ENOENT;
         /* A destination with two writers has no single owner. */
         if (site->write_domain != 0) {
            if (role_written[site->role])
               return -EEXIST;
            role_written[site->role] = true;
         }
         reloc_total++;
         if (reloc_total > R300_PM4_COMPOSE_MAX_RELOCS)
            return -E2BIG;
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
            (uint32_t)roles->chunk_index[site->role] * 4;
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
