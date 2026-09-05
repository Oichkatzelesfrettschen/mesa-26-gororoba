/* SPDX-License-Identifier: MIT */

#include "r300_rb2d_contract_evidence.h"

#include <errno.h>
#include <string.h>

/* One row per route contract.  V1 carries the sealed attended CONTROL_PASS
 * of the public vkCmdFillBuffer route, which emitted one window through one
 * relocation site; its artifact is the same retained bundle the 256-byte
 * ARGB8888 pitch row names, because one run produced both facts.  V2 carries
 * the attended CONTROL_PASS of the multiwindow cell: one 2 MiB interval on
 * the receipted 256-byte carrier decomposed into two rebased windows through
 * two relocation sites, 58 dwords, and the strict-2d parser accepted the
 * stream with every interval dword written, every canary intact, and an
 * empty dmesg delta.  The route stays PRECOMMITTED because a contract
 * receipt is not a route receipt: the dense carrier and the chooser verdict
 * are outstanding.
 */
static const struct r300_rb2d_contract_evidence rows[] = {
   { R300_RB2D_CONTRACT_CONST_FILL_V1,
     R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT, 1u, 1u,
     "steinmarder-r300 src/re/r300/results/"
     "r3v-native-rb2d-const-fill-public-route-receipt-"
     "vostro1000_rs485m_5974-strict-2d-cs" },
   { R300_RB2D_CONTRACT_CONST_FILL_V2,
     R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT, 2u, 2u,
     "steinmarder-r300 src/re/r300/results/"
     "r3v-native-rb2d-const-fill-v2-multiwindow-receipt-"
     "vostro1000_rs485m_5974-strict-2d-cs" },
};

const char *
r300_rb2d_contract_evidence_state_name(
   enum r300_rb2d_contract_evidence_state s)
{
   static const char *const names[R300_RB2D_CONTRACT_EVIDENCE_STATE_COUNT] = {
      "planned", "host-model", "kernel-replay", "silicon-receipt",
   };
   return (unsigned)s < R300_RB2D_CONTRACT_EVIDENCE_STATE_COUNT ? names[s]
                                                                : NULL;
}

const struct r300_rb2d_contract_evidence *
r300_rb2d_contract_evidence_rows(uint32_t *count_out)
{
   if (count_out != NULL)
      *count_out = (uint32_t)(sizeof(rows) / sizeof(rows[0]));
   return rows;
}

const struct r300_rb2d_contract_evidence *
r300_rb2d_contract_evidence_find_in(
   const struct r300_rb2d_contract_evidence *table, uint32_t count,
   enum r300_rb2d_contract contract)
{
   if (table == NULL)
      return NULL;
   for (uint32_t i = 0; i < count; i++) {
      if (table[i].contract == contract)
         return &table[i];
   }
   return NULL;
}

const struct r300_rb2d_contract_evidence *
r300_rb2d_contract_evidence_find(enum r300_rb2d_contract contract)
{
   return r300_rb2d_contract_evidence_find_in(
      rows, (uint32_t)(sizeof(rows) / sizeof(rows[0])), contract);
}

bool
r300_rb2d_contract_admitted(enum r300_rb2d_contract contract,
                            uint32_t window_count, uint32_t reloc_sites,
                            enum r300_rb2d_contract_evidence_state at_least)
{
   const struct r300_rb2d_contract_evidence *row =
      r300_rb2d_contract_evidence_find(contract);
   return row != NULL && row->state >= at_least &&
          window_count <= row->max_windows_receipted &&
          reloc_sites <= row->max_reloc_sites_receipted;
}

int
r300_rb2d_contract_evidence_rows_valid(
   const struct r300_rb2d_contract_evidence *table, uint32_t count)
{
   if (table == NULL || count != (uint32_t)R300_RB2D_CONTRACT_COUNT)
      return -EINVAL;

   for (uint32_t i = 0; i < count; i++) {
      const struct r300_rb2d_contract_evidence *r = &table[i];
      if (r->contract != (enum r300_rb2d_contract)i)
         return -EINVAL;
      if ((unsigned)r->state >= R300_RB2D_CONTRACT_EVIDENCE_STATE_COUNT)
         return -EINVAL;
      if (r->artifact == NULL || r->artifact[0] == '\0')
         return -EINVAL;
      if ((r->state == R300_RB2D_CONTRACT_EVIDENCE_PLANNED) !=
          (strcmp(r->artifact, "planned") == 0))
         return -EINVAL;
      if (r->state == R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT &&
          (r->max_windows_receipted == 0u || r->max_reloc_sites_receipted == 0u))
         return -EINVAL;
      if (r->max_reloc_sites_receipted != r->max_windows_receipted)
         return -EINVAL;
   }
   return 0;
}

int
r300_rb2d_contract_evidence_self_check(void)
{
   return r300_rb2d_contract_evidence_rows_valid(
      rows, (uint32_t)(sizeof(rows) / sizeof(rows[0])));
}
