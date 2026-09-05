/*
 * SPDX-License-Identifier: MIT
 *
 * Result oracle for the public RB2D constant-fill cell: classifies a
 * destination image against the sealed request without touching a
 * Vulkan object, so the same judgment serves the attended silicon run,
 * the drm-shim transport control, and the synthetic fixture matrix.
 */

#ifndef R3V_PUBLIC_RB2D_FILL_ORACLE_H
#define R3V_PUBLIC_RB2D_FILL_ORACLE_H

#include <stdbool.h>
#include <stdint.h>

/* The sealed cell: a 64 KiB destination, the fill interval [12, 5004),
 * pattern 0x11223344, and a 64-byte tail canary. */
#define R3V_PUBLIC_RB2D_FILL_ALLOCATION_BYTES (64u * 1024u)
#define R3V_PUBLIC_RB2D_FILL_OFFSET 12u
#define R3V_PUBLIC_RB2D_FILL_BYTES 4992u
#define R3V_PUBLIC_RB2D_FILL_VALUE 0x11223344u
#define R3V_PUBLIC_RB2D_FILL_TAIL_BYTES 64u

/* Initialization bytes.  The pattern's four bytes (44 33 22 11) and the
 * three canaries are pairwise distinct from the interval sentinel, so a
 * byte's origin is decidable from its value alone. */
#define R3V_PUBLIC_RB2D_FILL_PREFIX_CANARY 0xc1u
#define R3V_PUBLIC_RB2D_FILL_INTERVAL_SENTINEL 0xa5u
#define R3V_PUBLIC_RB2D_FILL_SUFFIX_CANARY 0xc2u
#define R3V_PUBLIC_RB2D_FILL_TAIL_CANARY 0xc3u

struct r3v_public_rb2d_fill_cell {
   uint32_t allocation_bytes;
   uint32_t fill_offset;
   uint32_t fill_bytes;
   uint32_t fill_value;
   uint32_t tail_bytes;
};

/* Outcome classes in verdict order.  CONTROL_PASS is the only zero exit;
 * the three transport classes come from the wrapper that owns the
 * submission, the destination classes from the oracle. */
enum r3v_public_rb2d_fill_outcome {
   R3V_PUBLIC_RB2D_FILL_CONTROL_PASS = 0,
   R3V_PUBLIC_RB2D_FILL_NO_DEVICE_WRITE,
   R3V_PUBLIC_RB2D_FILL_PARTIAL_WRITE,
   R3V_PUBLIC_RB2D_FILL_SHIFTED_WRITE,
   R3V_PUBLIC_RB2D_FILL_PATTERN_MISMATCH,
   R3V_PUBLIC_RB2D_FILL_OUTSIDE_WRITE,
   R3V_PUBLIC_RB2D_FILL_CANARY_CORRUPTION,
   R3V_PUBLIC_RB2D_FILL_SUBMIT_FAILED,
   R3V_PUBLIC_RB2D_FILL_COMPLETION_FAILED,
   R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL,
   R3V_PUBLIC_RB2D_FILL_OUTCOME_COUNT,
};

/* The six decisive predicates, each disabled alone by the calibration
 * matrix; a disabled predicate reads as satisfied.  The changed-byte and
 * changed-dword counts are implied by the six (an interval of pattern
 * dwords with nothing outside it changes exactly fill_bytes), so they are
 * reported values verified on every fixture and re-checked by the
 * wrapper rather than a seventh predicate that could never decide. */
enum r3v_public_rb2d_fill_predicate {
   R3V_PUBLIC_RB2D_FILL_PREDICATE_TAIL_CANARY = 1u << 0,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_ANY_WRITE = 1u << 1,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_SHIFT = 1u << 2,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_OUTSIDE = 1u << 3,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_PATTERN = 1u << 4,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_COMPLETE = 1u << 5,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_ALL = (1u << 6) - 1u,
};

struct r3v_public_rb2d_fill_report {
   enum r3v_public_rb2d_fill_outcome outcome;
   uint32_t changed_bytes;
   uint32_t changed_dwords;
   uint32_t interval_pattern_dwords;
   uint32_t interval_sentinel_dwords;
   uint32_t interval_other_dwords;
   uint32_t outside_changed_bytes;
   uint32_t tail_changed_bytes;
   uint32_t first_changed;
   uint32_t last_changed;
   bool shifted;
   uint32_t shifted_run_start;
};

const struct r3v_public_rb2d_fill_cell *r3v_public_rb2d_fill_sealed_cell(void);

bool r3v_public_rb2d_fill_cell_valid(const struct r3v_public_rb2d_fill_cell *cell);

uint8_t r3v_public_rb2d_fill_initial_byte(
   const struct r3v_public_rb2d_fill_cell *cell, uint32_t index);

void r3v_public_rb2d_fill_initialize(
   const struct r3v_public_rb2d_fill_cell *cell, uint8_t *image);

enum r3v_public_rb2d_fill_outcome r3v_public_rb2d_fill_classify(
   const struct r3v_public_rb2d_fill_cell *cell, const uint8_t *image,
   struct r3v_public_rb2d_fill_report *report);

enum r3v_public_rb2d_fill_outcome r3v_public_rb2d_fill_classify_masked(
   const struct r3v_public_rb2d_fill_cell *cell, const uint8_t *image,
   uint32_t predicates, struct r3v_public_rb2d_fill_report *report);

const char *r3v_public_rb2d_fill_outcome_name(
   enum r3v_public_rb2d_fill_outcome outcome);

const char *r3v_public_rb2d_fill_predicate_name(
   enum r3v_public_rb2d_fill_predicate predicate);

/* Exit status of a wrapper for an outcome: 0 for CONTROL_PASS, 1 for a
 * destination verdict, 2 for an infrastructure refusal, 3 and 4 for the
 * submit and completion transport failures. */
int r3v_public_rb2d_fill_exit_status(enum r3v_public_rb2d_fill_outcome outcome);

#endif
