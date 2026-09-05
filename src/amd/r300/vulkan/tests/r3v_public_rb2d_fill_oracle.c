/*
 * SPDX-License-Identifier: MIT
 *
 * Result oracle for the public RB2D constant-fill cell.  The verdict is
 * a total order over the destination image: the tail canary, then
 * whether any byte moved, then a displaced copy of the interval, then
 * any byte outside the interval, then the interval's dword content.
 * The first failed predicate names the outcome, so an image that fails
 * several reads as the earliest one and a calibration fixture pins each.
 */

#include "r3v_public_rb2d_fill_oracle.h"

#include <string.h>

static const struct r3v_public_rb2d_fill_cell sealed_cell = {
   .allocation_bytes = R3V_PUBLIC_RB2D_FILL_ALLOCATION_BYTES,
   .fill_offset = R3V_PUBLIC_RB2D_FILL_OFFSET,
   .fill_bytes = R3V_PUBLIC_RB2D_FILL_BYTES,
   .fill_value = R3V_PUBLIC_RB2D_FILL_VALUE,
   .tail_bytes = R3V_PUBLIC_RB2D_FILL_TAIL_BYTES,
};

const struct r3v_public_rb2d_fill_cell *
r3v_public_rb2d_fill_sealed_cell(void)
{
   return &sealed_cell;
}

bool
r3v_public_rb2d_fill_cell_valid(const struct r3v_public_rb2d_fill_cell *cell)
{
   if (cell->allocation_bytes == 0 || cell->fill_bytes == 0 ||
       cell->fill_bytes % 4 != 0 || cell->fill_offset % 4 != 0 ||
       cell->tail_bytes == 0 || cell->tail_bytes >= cell->allocation_bytes)
      return false;
   if (cell->fill_offset > cell->allocation_bytes - cell->tail_bytes ||
       cell->fill_bytes > cell->allocation_bytes - cell->tail_bytes -
                             cell->fill_offset)
      return false;
   /* The pattern's bytes must stay distinct from every initialization
    * byte, or a byte's origin stops being decidable. */
   for (unsigned i = 0; i < 4; i++) {
      const uint8_t b = (uint8_t)(cell->fill_value >> (8 * i));
      if (b == R3V_PUBLIC_RB2D_FILL_PREFIX_CANARY ||
          b == R3V_PUBLIC_RB2D_FILL_INTERVAL_SENTINEL ||
          b == R3V_PUBLIC_RB2D_FILL_SUFFIX_CANARY ||
          b == R3V_PUBLIC_RB2D_FILL_TAIL_CANARY)
         return false;
   }
   return true;
}

uint8_t
r3v_public_rb2d_fill_initial_byte(const struct r3v_public_rb2d_fill_cell *cell,
                                  uint32_t index)
{
   if (index >= cell->allocation_bytes - cell->tail_bytes)
      return R3V_PUBLIC_RB2D_FILL_TAIL_CANARY;
   if (index < cell->fill_offset)
      return R3V_PUBLIC_RB2D_FILL_PREFIX_CANARY;
   if (index < cell->fill_offset + cell->fill_bytes)
      return R3V_PUBLIC_RB2D_FILL_INTERVAL_SENTINEL;
   return R3V_PUBLIC_RB2D_FILL_SUFFIX_CANARY;
}

void
r3v_public_rb2d_fill_initialize(const struct r3v_public_rb2d_fill_cell *cell,
                                uint8_t *image)
{
   for (uint32_t i = 0; i < cell->allocation_bytes; i++)
      image[i] = r3v_public_rb2d_fill_initial_byte(cell, i);
}

static uint32_t
read_dword(const uint8_t *image, uint32_t at)
{
   uint32_t word;
   memcpy(&word, image + at, sizeof(word));
   return word;
}

/* A displaced copy: every changed byte lies in one run of exactly
 * fill_bytes that starts on a dword boundary other than the request's
 * offset, and every dword of the run carries the pattern. */
static bool
find_shifted_run(const struct r3v_public_rb2d_fill_cell *cell,
                 const uint8_t *image, const struct r3v_public_rb2d_fill_report *r,
                 uint32_t *start)
{
   if (r->changed_bytes != cell->fill_bytes || r->tail_changed_bytes != 0)
      return false;
   const uint32_t first = r->first_changed;
   if (r->last_changed + 1 - first != cell->fill_bytes || first % 4 != 0 ||
       first == cell->fill_offset)
      return false;
   for (uint32_t at = first; at < first + cell->fill_bytes; at += 4)
      if (read_dword(image, at) != cell->fill_value)
         return false;
   *start = first;
   return true;
}

enum r3v_public_rb2d_fill_outcome
r3v_public_rb2d_fill_classify_masked(const struct r3v_public_rb2d_fill_cell *cell,
                                     const uint8_t *image, uint32_t predicates,
                                     struct r3v_public_rb2d_fill_report *r)
{
   memset(r, 0, sizeof(*r));
   r->first_changed = UINT32_MAX;
   const uint32_t interval_end = cell->fill_offset + cell->fill_bytes;
   const uint32_t tail_start = cell->allocation_bytes - cell->tail_bytes;
   const uint32_t sentinel_word = 0x01010101u * R3V_PUBLIC_RB2D_FILL_INTERVAL_SENTINEL;

   for (uint32_t i = 0; i < cell->allocation_bytes; i++) {
      if (image[i] == r3v_public_rb2d_fill_initial_byte(cell, i))
         continue;
      r->changed_bytes++;
      if (r->first_changed == UINT32_MAX)
         r->first_changed = i;
      r->last_changed = i;
      if (i >= tail_start)
         r->tail_changed_bytes++;
      else if (i < cell->fill_offset || i >= interval_end)
         r->outside_changed_bytes++;
   }
   for (uint32_t at = 0; at + 4 <= cell->allocation_bytes; at += 4) {
      bool changed = false;
      for (uint32_t b = 0; b < 4; b++)
         changed |= image[at + b] != r3v_public_rb2d_fill_initial_byte(cell, at + b);
      r->changed_dwords += changed;
   }
   for (uint32_t at = cell->fill_offset; at < interval_end; at += 4) {
      const uint32_t word = read_dword(image, at);
      if (word == cell->fill_value)
         r->interval_pattern_dwords++;
      else if (word == sentinel_word)
         r->interval_sentinel_dwords++;
      else
         r->interval_other_dwords++;
   }
   r->shifted = find_shifted_run(cell, image, r, &r->shifted_run_start);

#define PREDICATE(bit) ((predicates & R3V_PUBLIC_RB2D_FILL_PREDICATE_##bit) != 0)
   if (PREDICATE(TAIL_CANARY) && r->tail_changed_bytes != 0)
      r->outcome = R3V_PUBLIC_RB2D_FILL_CANARY_CORRUPTION;
   else if (PREDICATE(ANY_WRITE) && r->changed_bytes == 0)
      r->outcome = R3V_PUBLIC_RB2D_FILL_NO_DEVICE_WRITE;
   else if (PREDICATE(SHIFT) && r->shifted)
      r->outcome = R3V_PUBLIC_RB2D_FILL_SHIFTED_WRITE;
   else if (PREDICATE(OUTSIDE) && r->outside_changed_bytes != 0)
      r->outcome = R3V_PUBLIC_RB2D_FILL_OUTSIDE_WRITE;
   else if (PREDICATE(PATTERN) && r->interval_other_dwords != 0)
      r->outcome = R3V_PUBLIC_RB2D_FILL_PATTERN_MISMATCH;
   else if (PREDICATE(COMPLETE) && r->interval_sentinel_dwords != 0)
      r->outcome = R3V_PUBLIC_RB2D_FILL_PARTIAL_WRITE;
   else
      r->outcome = R3V_PUBLIC_RB2D_FILL_CONTROL_PASS;
#undef PREDICATE
   return r->outcome;
}

enum r3v_public_rb2d_fill_outcome
r3v_public_rb2d_fill_classify(const struct r3v_public_rb2d_fill_cell *cell,
                              const uint8_t *image,
                              struct r3v_public_rb2d_fill_report *report)
{
   return r3v_public_rb2d_fill_classify_masked(
      cell, image, R3V_PUBLIC_RB2D_FILL_PREDICATE_ALL, report);
}

const char *
r3v_public_rb2d_fill_outcome_name(enum r3v_public_rb2d_fill_outcome outcome)
{
   static const char *const names[R3V_PUBLIC_RB2D_FILL_OUTCOME_COUNT] = {
      [R3V_PUBLIC_RB2D_FILL_CONTROL_PASS] = "CONTROL_PASS",
      [R3V_PUBLIC_RB2D_FILL_NO_DEVICE_WRITE] = "NO_DEVICE_WRITE",
      [R3V_PUBLIC_RB2D_FILL_PARTIAL_WRITE] = "PARTIAL_WRITE",
      [R3V_PUBLIC_RB2D_FILL_SHIFTED_WRITE] = "SHIFTED_WRITE",
      [R3V_PUBLIC_RB2D_FILL_PATTERN_MISMATCH] = "PATTERN_MISMATCH",
      [R3V_PUBLIC_RB2D_FILL_OUTSIDE_WRITE] = "OUTSIDE_WRITE",
      [R3V_PUBLIC_RB2D_FILL_CANARY_CORRUPTION] = "CANARY_CORRUPTION",
      [R3V_PUBLIC_RB2D_FILL_SUBMIT_FAILED] = "SUBMIT_FAILED",
      [R3V_PUBLIC_RB2D_FILL_COMPLETION_FAILED] = "COMPLETION_FAILED",
      [R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL] = "INFRASTRUCTURE_REFUSAL",
   };
   if ((unsigned)outcome >= R3V_PUBLIC_RB2D_FILL_OUTCOME_COUNT)
      return "(unknown)";
   return names[outcome];
}

const char *
r3v_public_rb2d_fill_predicate_name(enum r3v_public_rb2d_fill_predicate p)
{
   switch (p) {
   case R3V_PUBLIC_RB2D_FILL_PREDICATE_TAIL_CANARY:
      return "tail_canary_unchanged";
   case R3V_PUBLIC_RB2D_FILL_PREDICATE_ANY_WRITE:
      return "any_byte_changed";
   case R3V_PUBLIC_RB2D_FILL_PREDICATE_SHIFT:
      return "no_displaced_interval";
   case R3V_PUBLIC_RB2D_FILL_PREDICATE_OUTSIDE:
      return "no_byte_outside_interval";
   case R3V_PUBLIC_RB2D_FILL_PREDICATE_PATTERN:
      return "interval_dwords_pattern_or_sentinel";
   case R3V_PUBLIC_RB2D_FILL_PREDICATE_COMPLETE:
      return "interval_sentinel_dwords_zero";
   default:
      return "(unknown)";
   }
}

int
r3v_public_rb2d_fill_exit_status(enum r3v_public_rb2d_fill_outcome outcome)
{
   switch (outcome) {
   case R3V_PUBLIC_RB2D_FILL_CONTROL_PASS:
      return 0;
   case R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL:
      return 2;
   case R3V_PUBLIC_RB2D_FILL_SUBMIT_FAILED:
      return 3;
   case R3V_PUBLIC_RB2D_FILL_COMPLETION_FAILED:
      return 4;
   default:
      return 1;
   }
}
