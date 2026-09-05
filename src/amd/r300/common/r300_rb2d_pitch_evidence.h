/*
 * SPDX-License-Identifier: MIT
 *
 * Pitch-evidence registry for the RB2D carriers.  A virtual surface pitch
 * is a driver invention -- a linear Vulkan buffer has no rows -- so which
 * pitches the legalizer may cut a span on is an evidence question, and
 * this table is the one place that answers it.  A row names a pitch, the
 * carrier format, the usage it was exercised under, the highest evidence
 * class that exercised it, and the retained artifact that carries that
 * evidence.  Promotion is one data-row edit naming an already retained
 * receipt; no code path admits a pitch the table withholds.
 *
 * Evidence classes, lowest to highest: PLANNED (a candidate the cost model
 * may rank, exercised by nothing outside this tree), HOST_MODEL (the
 * decomposition and plan checker admit it), KERNEL_REPLAY (the kernel CS
 * tracker replay accepts the emitted stream), SILICON_RECEIPT (a sealed
 * attended run on the RS485M specimen read the fill back).  Execution
 * admits SILICON_RECEIPT alone.
 */

#ifndef R300_RB2D_PITCH_EVIDENCE_H
#define R300_RB2D_PITCH_EVIDENCE_H

#include "r300_rb2d_fill.h"

#include <stdbool.h>
#include <stdint.h>

enum r300_rb2d_pitch_evidence_class {
   R300_RB2D_PITCH_EVIDENCE_PLANNED = 0,
   R300_RB2D_PITCH_EVIDENCE_HOST_MODEL,
   R300_RB2D_PITCH_EVIDENCE_KERNEL_REPLAY,
   R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT,
   R300_RB2D_PITCH_EVIDENCE_CLASS_COUNT,
};

enum r300_rb2d_usage {
   R300_RB2D_USAGE_FILL_BUFFER = 0,
   R300_RB2D_USAGE_COUNT,
};

struct r300_rb2d_pitch_evidence {
   uint32_t pitch_bytes;
   enum r300_rb2d_format format;
   enum r300_rb2d_usage usage;
   enum r300_rb2d_pitch_evidence_class evidence;
   /* The retained bundle or test that carries the evidence class, named
    * so a reader can open it; "planned" for a candidate nothing has run. */
   const char *artifact;
};

const char *
r300_rb2d_pitch_evidence_class_name(enum r300_rb2d_pitch_evidence_class c);

/* The registry, in ascending pitch order within a format. */
const struct r300_rb2d_pitch_evidence *
r300_rb2d_pitch_evidence_rows(uint32_t *count_out);

/* The row for one carrier under one usage, or NULL when the registry
 * carries none: an unregistered pitch has no evidence class at all. */
const struct r300_rb2d_pitch_evidence *
r300_rb2d_pitch_evidence_find(uint32_t pitch_bytes,
                              enum r300_rb2d_format format,
                              enum r300_rb2d_usage usage);

/* Whether a carrier reaches at least the named class under one usage. */
bool r300_rb2d_pitch_admitted(uint32_t pitch_bytes,
                              enum r300_rb2d_format format,
                              enum r300_rb2d_usage usage,
                              enum r300_rb2d_pitch_evidence_class at_least);

/* Registry self-consistency: every row's pitch passes the layout grid, the
 * rows are unique per (pitch, format, usage), ascending within a format,
 * every SILICON_RECEIPT row names an artifact that is not "planned", and
 * the witnessed 256-byte ARGB8888 fill carrier holds SILICON_RECEIPT.
 * Returns 0 or -EINVAL. */
int r300_rb2d_pitch_evidence_self_check(void);

#endif /* R300_RB2D_PITCH_EVIDENCE_H */
