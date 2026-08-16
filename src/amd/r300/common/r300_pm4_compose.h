/*
 * SPDX-License-Identifier: MIT
 *
 * Role-based composer for independently emitted PM4 fragments.
 */

#ifndef R300_PM4_COMPOSE_H
#define R300_PM4_COMPOSE_H

#include "r300_pm4_builder.h"
#include "r300_r2vb_fetch_pass.h"

#include <stdint.h>

/* One relocation site inside a fragment: the dword index of the payload
 * behind a NOP header, the symbolic BO role it resolves through, and
 * the DRM read/write domains the submission's BO table needs for the
 * use.  Two roles resolving to the same BO stay separate uses: use-site
 * identity is what the kernel validates and the evidence retains.
 */
struct r300_pm4_reloc_site {
   uint32_t dword_index;
   enum r300_r2vb_bo_role role;
   uint32_t read_domains;
   uint32_t write_domain;
};

struct r300_pm4_fragment {
   const uint32_t *dwords;
   uint32_t dword_count;
   const struct r300_pm4_reloc_site *relocs;
   uint32_t reloc_count;
};

/* The caller's role map: for each role, the relocation-chunk index the
 * kernel resolves the NOP payload against, or -1 for a role this
 * composition does not bind.
 */
struct r300_pm4_role_map {
   int32_t chunk_index[4];
};

/* One resolved site in the composed parent. */
struct r300_pm4_composed_reloc {
   uint32_t ib_index;
   enum r300_r2vb_bo_role role;
   uint32_t read_domains;
   uint32_t write_domain;
};

#define R300_PM4_COMPOSE_MAX_RELOCS 16u

struct r300_pm4_composition {
   struct r300_pm4_composed_reloc relocs[R300_PM4_COMPOSE_MAX_RELOCS];
   uint32_t reloc_count;
   /* The parent dword index where each fragment begins, in call order;
    * retained evidence splits the stream here when a verdict needs the
    * per-stage bytes.
    */
   uint32_t fragment_start[8];
   uint32_t fragment_count;
};

/* Composes the fragments, in order, into the builder: checked size sum,
 * whole-parent reservation, ordered copy, relocation-index rebase, and
 * NOP payloads rewritten to the role map's chunk index times four.
 * Rejects, before writing any dword: a null or empty fragment list,
 * more fragments than fragment_start holds, a dword-count sum that
 * overflows, a relocation index outside its fragment or not preceded by
 * the NOP header, a role the map leaves unbound, more sites than the
 * composition holds, and two sites writing the same role (a duplicate
 * write target reads as two owners of one destination).  Returns 0 or a
 * negative errno.
 */
int r300_pm4_compose(struct r300_pm4_builder *b,
                     const struct r300_pm4_fragment *fragments,
                     uint32_t fragment_count,
                     const struct r300_pm4_role_map *roles,
                     struct r300_pm4_composition *out);

#endif /* R300_PM4_COMPOSE_H */
