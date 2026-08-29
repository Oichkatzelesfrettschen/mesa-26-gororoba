/*
 * SPDX-License-Identifier: MIT
 *
 * Rebase, payload-rewrite, and rejection controls for the role-based
 * PM4 composer.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG

#include "r300_pm4_compose.h"

#include "r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define NOP_HEADER CP_PACKET3(R300_PM4_PACKET3_NOP, 0)

/* Fragment A: two state dwords, then a carrier-target reference whose
 * placeholder payload the composer must rewrite.  Fragment B: one state
 * dword, a carrier read reference, and a color write reference.
 */
static const uint32_t frag_a_words[] = {
   0x11111111, 0x22222222, NOP_HEADER, 0xdeadbeef,
};
static const struct r300_pm4_reloc_site frag_a_sites[] = {
   {.dword_index = 3,
    .role = R300_R2VB_BO_CARRIER,
    .read_domains = 0,
    .write_domain = 2},
};
static const uint32_t frag_b_words[] = {
   0x33333333, NOP_HEADER, 0xdeadbeef, NOP_HEADER, 0xdeadbeef,
};
static const struct r300_pm4_reloc_site frag_b_sites[] = {
   {.dword_index = 2,
    .role = R300_R2VB_BO_CARRIER,
    .read_domains = 2,
    .write_domain = 0},
   {.dword_index = 4,
    .role = R300_R2VB_BO_COLOR,
    .read_domains = 0,
    .write_domain = 2},
};

static const struct r300_pm4_fragment reference_fragments[2] = {
   {.dwords = frag_a_words,
    .dword_count = 4,
    .relocs = frag_a_sites,
    .reloc_count = 1},
   {.dwords = frag_b_words,
    .dword_count = 5,
    .relocs = frag_b_sites,
    .reloc_count = 2},
};

static const struct r300_pm4_role_map reference_roles = {
   .chunk_index = {[R300_R2VB_BO_SLOT] = -1,
                   [R300_R2VB_BO_MODEL] = -1,
                   [R300_R2VB_BO_CARRIER] = 0,
                   [R300_R2VB_BO_COLOR] = 1},
};

static void
test_compose_rebases_and_rewrites(void)
{
   uint32_t ib[16];
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, ib, 9);
   struct r300_pm4_composition comp;
   assert(r300_pm4_compose(&b, reference_fragments, 2, &reference_roles,
                           &comp) == 0);
   uint32_t count = 0;
   assert(r300_pm4_builder_finish(&b, &count) == 0);
   assert(count == 9);

   assert(comp.fragment_count == 2);
   assert(comp.fragment_start[0] == 0);
   assert(comp.fragment_start[1] == 4);

   /* Fragment bytes survive verbatim except the rewritten payloads. */
   assert(ib[0] == 0x11111111 && ib[1] == 0x22222222);
   assert(ib[2] == NOP_HEADER && ib[4] == 0x33333333);

   assert(comp.reloc_count == 3);
   /* Site 0: fragment A's carrier write at parent index 3, chunk 0. */
   assert(comp.relocs[0].ib_index == 3);
   assert(comp.relocs[0].role == R300_R2VB_BO_CARRIER);
   assert(comp.relocs[0].write_domain == 2);
   assert(ib[3] == 0);
   /* Site 1: fragment B's carrier read rebased to 4 + 2. */
   assert(comp.relocs[1].ib_index == 6);
   assert(comp.relocs[1].read_domains == 2);
   assert(ib[6] == 0);
   /* Site 2: fragment B's color write rebased to 4 + 4, chunk 1. */
   assert(comp.relocs[2].ib_index == 8);
   assert(comp.relocs[2].role == R300_R2VB_BO_COLOR);
   assert(ib[8] == 4);

   /* Same carrier BO, two use sites: use-site identity is preserved
    * rather than collapsed.
    */
   assert(comp.relocs[0].ib_index != comp.relocs[1].ib_index);
}

static void
test_compose_rejections(void)
{
   uint32_t ib[16];
   struct r300_pm4_builder b;
   struct r300_pm4_composition comp;

   /* An unbound role refuses before any write. */
   struct r300_pm4_role_map unbound = reference_roles;
   unbound.chunk_index[R300_R2VB_BO_COLOR] = -1;
   memset(ib, 0, sizeof(ib));
   r300_pm4_builder_init(&b, ib, 9);
   assert(r300_pm4_compose(&b, reference_fragments, 2, &unbound, &comp) ==
          -ENOENT);
   assert(b.count == 0 && ib[0] == 0);

   /* A site not behind the NOP header refuses. */
   static const uint32_t bare[] = {0x11111111, 0xdeadbeef};
   static const struct r300_pm4_reloc_site bare_site[] = {
      {.dword_index = 1, .role = R300_R2VB_BO_CARRIER, .write_domain = 2},
   };
   const struct r300_pm4_fragment bad_frag = {
      .dwords = bare, .dword_count = 2, .relocs = bare_site,
      .reloc_count = 1};
   r300_pm4_builder_init(&b, ib, 9);
   assert(r300_pm4_compose(&b, &bad_frag, 1, &reference_roles, &comp) ==
          -EINVAL);

   /* A site index outside its fragment refuses. */
   static const struct r300_pm4_reloc_site oob_site[] = {
      {.dword_index = 4, .role = R300_R2VB_BO_CARRIER, .write_domain = 2},
   };
   const struct r300_pm4_fragment oob_frag = {
      .dwords = frag_a_words, .dword_count = 4, .relocs = oob_site,
      .reloc_count = 1};
   r300_pm4_builder_init(&b, ib, 9);
   assert(r300_pm4_compose(&b, &oob_frag, 1, &reference_roles, &comp) ==
          -EINVAL);

   /* Two writers of one role refuse: a destination has one owner. */
   const struct r300_pm4_fragment twice[2] = {
      reference_fragments[0], reference_fragments[0]};
   r300_pm4_builder_init(&b, ib, 16);
   assert(r300_pm4_compose(&b, twice, 2, &reference_roles, &comp) ==
          -EEXIST);

   /* One dword short takes none of the composition. */
   memset(ib, 0, sizeof(ib));
   r300_pm4_builder_init(&b, ib, 8);
   assert(r300_pm4_compose(&b, reference_fragments, 2, &reference_roles,
                           &comp) == -ENOSPC);
   assert(b.count == 0 && ib[0] == 0);

   /* Duplicate descriptors for one fragment payload refuse before copying. */
   static const struct r300_pm4_reloc_site duplicate_sites[] = {
      {.dword_index = 3,
       .role = R300_R2VB_BO_CARRIER,
       .write_domain = 2},
      {.dword_index = 3,
       .role = R300_R2VB_BO_COLOR,
       .read_domains = 2},
   };
   const struct r300_pm4_fragment duplicate_fragment = {
      .dwords = frag_a_words,
      .dword_count = 4,
      .relocs = duplicate_sites,
      .reloc_count = 2,
   };
   memset(ib, 0, sizeof(ib));
   r300_pm4_builder_init(&b, ib, 4);
   assert(r300_pm4_compose(&b, &duplicate_fragment, 1, &reference_roles,
                           &comp) == -EEXIST);
   assert(b.count == 0 && ib[0] == 0);

   /* Writer ownership follows the resolved chunk, not the symbolic role. */
   struct r300_pm4_role_map aliased_roles = reference_roles;
   aliased_roles.chunk_index[R300_R2VB_BO_COLOR] =
      aliased_roles.chunk_index[R300_R2VB_BO_CARRIER];
   memset(ib, 0, sizeof(ib));
   r300_pm4_builder_init(&b, ib, 9);
   assert(r300_pm4_compose(&b, reference_fragments, 2, &aliased_roles,
                           &comp) == -EEXIST);
   assert(b.count == 0 && ib[0] == 0);

   /* The largest representable four-dword payload index is accepted. */
   struct r300_pm4_role_map boundary_roles = reference_roles;
   boundary_roles.chunk_index[R300_R2VB_BO_CARRIER] =
      (int32_t)(UINT32_MAX / 4u);
   memset(ib, 0, sizeof(ib));
   r300_pm4_builder_init(&b, ib, 4);
   assert(r300_pm4_compose(&b, &reference_fragments[0], 1,
                           &boundary_roles, &comp) == 0);
   assert(ib[3] == UINT32_MAX - 3u);

   /* The next chunk index refuses before the builder receives any dword. */
   boundary_roles.chunk_index[R300_R2VB_BO_CARRIER] =
      (int32_t)(UINT32_MAX / 4u + 1u);
   memset(ib, 0, sizeof(ib));
   r300_pm4_builder_init(&b, ib, 4);
   assert(r300_pm4_compose(&b, &reference_fragments[0], 1,
                           &boundary_roles, &comp) == -EOVERFLOW);
   assert(b.count == 0 && ib[0] == 0);

   /* Zero fragments and a null list refuse. */
   r300_pm4_builder_init(&b, ib, 8);
   assert(r300_pm4_compose(&b, reference_fragments, 0, &reference_roles,
                           &comp) == -EINVAL);
   assert(r300_pm4_compose(&b, NULL, 1, &reference_roles, &comp) ==
          -EINVAL);
}

int
main(void)
{
   test_compose_rebases_and_rewrites();
   test_compose_rejections();
   printf("r300_pm4_compose_test: ownership, overflow, and rejection controls "
          "held\n");
   return 0;
}
