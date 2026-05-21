/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * terakan_pm4_ib_dump.h -- env-gated PM4 IB JSONL dump.
 *
 * Emits the dword array Terakan submits to the kernel via
 * `drmCommandWriteRead(DRM_RADEON_CS, ...)` as JSON Lines to
 * terakan_pm4_ib_<pid>.jsonl in the process temporary directory.
 * Diagnostic tooling compares these submitted command-stream bytes
 * with descriptor-object dumps, libdrm ioctl envelopes, and
 * kernel-side observer captures.
 *
 * Gate: env TERAKAN_DEBUG_DUMP_IB with any truthy value via
 * util/u_debug.h's `debug_get_bool_option`.  This reuses the
 * existing stderr-dump gate at the queue-submit callsite so a single
 * env variable controls both stderr text output and the JSONL file
 * output.  An explicit `TERAKAN_DEBUG_DUMP_IB_JSONL_DISABLE=1`
 * strict gate suppresses the file output specifically (e.g. when
 * the operator wants stderr only because the temporary directory is
 * on read-only media).
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
 * The full IB is emitted rather than capped because the kernel-side
 * observer records complete post-validator IB bytes.  Capping here
 * would make equality checks ambiguous.  Per-line size is bounded by
 * the submitted IB length.
 */

#ifndef TERAKAN_PM4_IB_DUMP_H
#define TERAKAN_PM4_IB_DUMP_H

#include <stdbool.h>
#include <stdint.h>

/* Cached cheap enable check; identical semantics to the existing
 * stderr-side gate (debug_get_bool_option), with one additional
 * level of caching for the file-output disable knob. */
bool terakan_pm4_ib_dump_active(void);

/* Emit one pm4_ib_cs_submission JSONL row.  Safe to call with
 * any ring id + any non-NULL dword pointer.  No-op when the gate
 * is off.  Caller should already have its own enable check from
 * the existing TERAKAN_DEBUG_DUMP_IB stderr block to avoid a
 * second strncmp; this function does an internal gate check for
 * defense in depth. */
void terakan_pm4_ib_dump_cs_submission(unsigned ring,
                                   uint32_t const *ib_dwords,
                                   uint32_t ib_length_dwords);

#endif /* TERAKAN_PM4_IB_DUMP_H */
