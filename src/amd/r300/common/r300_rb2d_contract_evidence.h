/*
 * SPDX-License-Identifier: MIT
 *
 * Contract-evidence registry for the RB2D route contracts.  Two authorities
 * govern one legalization and answer different questions.  The pitch
 * registry says what a carrier -- a (pitch, format, usage) triple -- has
 * been exercised at; this registry says what a contract's own stream shape
 * has been exercised at, in windows and relocation sites.  A 256-byte
 * ARGB8888 carrier holding a silicon receipt states nothing about a stream
 * that rebases the destination twice, so admission reads both tables and a
 * request that clears one still refuses on the other.
 *
 * A row names the contract, the highest class that exercised it, the window
 * and relocation-site counts that class reached, and the retained artifact
 * that carries it.  Promotion is one data-row edit naming an already
 * retained receipt.
 *
 * The classes mirror the pitch registry's ladder value for value and stay a
 * separate type, so a carrier receipt never stands in for a contract
 * receipt.
 */

#ifndef R300_RB2D_CONTRACT_EVIDENCE_H
#define R300_RB2D_CONTRACT_EVIDENCE_H

#include <stdbool.h>
#include <stdint.h>

/* The two route contracts.  V1 is the qualified public fill: the 256-byte
 * ARGB8888 carrier, one window, at most three rectangles, and the exact
 * 38-dword stream the silicon receipt retains for the attended cell.  V2
 * admits any carrier the evidence registry admits, several rebased windows
 * in one stream, and one relocation site per window; it is qualified
 * separately and the route selects it only under its own receipt. */
enum r300_rb2d_contract {
   R300_RB2D_CONTRACT_CONST_FILL_V1 = 0,
   R300_RB2D_CONTRACT_CONST_FILL_V2,
   R300_RB2D_CONTRACT_COUNT,
};

/* Lowest to highest: PLANNED (a shape nothing outside this tree has run),
 * HOST_MODEL (the legalizer and the window checker admit it), KERNEL_REPLAY
 * (the CS-tracker replay accepts the emitted stream), SILICON_RECEIPT (a
 * sealed attended run on the RS485M specimen read the fill back).  Execution
 * admits SILICON_RECEIPT alone. */
enum r300_rb2d_contract_evidence_state {
   R300_RB2D_CONTRACT_EVIDENCE_PLANNED = 0,
   R300_RB2D_CONTRACT_EVIDENCE_HOST_MODEL,
   R300_RB2D_CONTRACT_EVIDENCE_KERNEL_REPLAY,
   R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT,
   R300_RB2D_CONTRACT_EVIDENCE_STATE_COUNT,
};

struct r300_rb2d_contract_evidence {
   enum r300_rb2d_contract contract;
   enum r300_rb2d_contract_evidence_state state;
   /* The widest stream the state reached: windows the run emitted and the
    * relocation sites it carried.  A legalization wider than either is
    * outside what ran, whatever the carrier's own evidence says. */
   uint32_t max_windows_receipted;
   uint32_t max_reloc_sites_receipted;
   /* The retained bundle or test that carries the state, named so a reader
    * can open it; "planned" for a shape nothing has run. */
   const char *artifact;
};

const char *r300_rb2d_contract_evidence_state_name(
   enum r300_rb2d_contract_evidence_state s);

/* The registry, in contract order. */
const struct r300_rb2d_contract_evidence *
r300_rb2d_contract_evidence_rows(uint32_t *count_out);

/* The row for one contract, or NULL when the registry carries none. */
const struct r300_rb2d_contract_evidence *
r300_rb2d_contract_evidence_find(enum r300_rb2d_contract contract);

const struct r300_rb2d_contract_evidence *
r300_rb2d_contract_evidence_find_in(
   const struct r300_rb2d_contract_evidence *table, uint32_t count,
   enum r300_rb2d_contract contract);

/* Whether a stream of window_count windows and reloc_sites relocation sites
 * stays inside what the contract's evidence reached: the row's state is at
 * least at_least and both counts are at or under the receipted maxima.  All
 * three hold together, so a receipted contract still refuses a stream wider
 * than the run that receipted it. */
bool r300_rb2d_contract_admitted(
   enum r300_rb2d_contract contract, uint32_t window_count,
   uint32_t reloc_sites, enum r300_rb2d_contract_evidence_state at_least);

/* Registry self-consistency over a caller's table, so a test calibrates the
 * checker on a mutated copy: every contract carries exactly one row, rows
 * stand in contract order, each state is inside its ladder, each row names
 * an artifact, "planned" and the PLANNED state imply each other, a
 * SILICON_RECEIPT row receipts at least one window and one site because a
 * run emitted a stream, and the site count equals the window count because
 * r300_rb2d_legalize_emit binds the destination once per window.  Returns 0
 * or -EINVAL. */
int r300_rb2d_contract_evidence_rows_valid(
   const struct r300_rb2d_contract_evidence *table, uint32_t count);

/* The shipped registry held to the rules above. */
int r300_rb2d_contract_evidence_self_check(void);

#endif /* R300_RB2D_CONTRACT_EVIDENCE_H */
