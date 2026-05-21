/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * terakan_pm4_ib_dump.h -- env-gated final PM4 IB capture.
 *
 * Emits the dword array Terakan submits to the kernel via
 * `drmCommandWriteRead(DRM_RADEON_CS, ...)` as JSON Lines to
 * /tmp/terakan_pm4_ib_<pid>.jsonl.  These are the final-IB (A1)
 * bytes in the descriptor-object/final-IB/libdrm-envelope/
 * post-validator-IB byte-path ontology used to localise
 * wrong-result-vs-byte-preserved verdicts in
 * dEQP-VK.glsl.texture_gather cube int gather cases on Palm.
 *
 * Gate: env TERAKAN_DEBUG_DUMP_IB with any truthy value via
 * util/u_debug.h's `debug_get_bool_option`.  This reuses the
 * existing stderr-dump gate at the same callsite (see
 * winsys/drm_radeon/terakan_queue_drm_radeon.c) so a single env
 * variable controls both stderr text output AND the JSONL file
 * output.  An explicit `TERAKAN_DEBUG_DUMP_IB_JSONL_DISABLE=1`
 * strict gate suppresses the file output specifically (e.g. when
 * the operator wants stderr only because /tmp is on read-only
 * media).
 *
 * Output schema (one JSON object per submission):
 *   {
 *     "event":             "pm4_ib_cs_submission",
 *     "ts_nsec":           <CLOCK_MONOTONIC>,
 *     "pid":               <getpid>,
 *     "tid":               <gettid>,
 *     "ring":              <ring id>,
 *     "ib_length_dw":      <length in dwords>,
 *     "ib_crc32":          "0x<crc32 over IB dwords>",
 *     "ib_dwords":         ["0x<dword>", ... (full IB)]
 *   }
 *
 * The full IB is emitted (NOT capped at 64 dwords) because the
 * paired Y.2 C-side ib_post_validate event also emits the full
 * IB; bounding A1 below C's bound would defeat the byte-equality
 * comparison.  Per-line size is bounded by the user's IB length.
 */

#ifndef TERAKAN_PM4_IB_DUMP_H
#define TERAKAN_PM4_IB_DUMP_H

#include <stdbool.h>
#include <stdint.h>

/* Cached cheap enable check; identical semantics to the existing
 * stderr-side gate (debug_get_bool_option), with one additional
 * level of caching for the file-output disable knob. */
bool terakan_pm4_ib_dump_active(void);

/* Emit one a1_cs_submission JSONL row.  Safe to call with
 * any ring id + any non-NULL dword pointer.  No-op when the gate
 * is off.  Caller should already have its own enable check from
 * the existing TERAKAN_DEBUG_DUMP_IB stderr block to avoid a
 * second strncmp; this function does an internal gate check for
 * defense in depth. */
void terakan_pm4_ib_dump_cs_submission(unsigned ring,
                                   uint32_t const *ib_dwords,
                                   uint32_t ib_length_dwords);

#endif /* TERAKAN_PM4_IB_DUMP_H */
