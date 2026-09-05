/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the RB2D fill oracle on synthetic destination images.
 * The oracle never sees a Vulkan object here: each fixture is the
 * initialized image with one named mutation, its expected class and
 * counts pinned; then every predicate is disabled alone and at least one
 * fixture must change class, which proves the predicate decides.  The
 * exact-output image is the oracle's synthetic positive fixture; the host
 * known-bad (a CPU store over the protected mapping) lives in the
 * transport checks, because it proves host exclusion, not classification.
 */

#include "r3v_public_rb2d_fill_oracle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFFSET R3V_PUBLIC_RB2D_FILL_OFFSET
#define BYTES R3V_PUBLIC_RB2D_FILL_BYTES
#define END (OFFSET + BYTES)
#define ALLOC R3V_PUBLIC_RB2D_FILL_ALLOCATION_BYTES

static void
put_dword(uint8_t *image, uint32_t at, uint32_t value)
{
   memcpy(image + at, &value, sizeof(value));
}

static void
fill_interval(uint8_t *image, uint32_t start, uint32_t bytes, uint32_t value)
{
   for (uint32_t at = start; at < start + bytes; at += 4)
      put_dword(image, at, value);
}

static void
mutate_exact(uint8_t *image)
{
   fill_interval(image, OFFSET, BYTES, R3V_PUBLIC_RB2D_FILL_VALUE);
}

static void
mutate_unchanged(uint8_t *image)
{
   (void)image;
}

static void
mutate_missing_dword(uint8_t *image)
{
   mutate_exact(image);
   put_dword(image, OFFSET + 2000, 0x01010101u * R3V_PUBLIC_RB2D_FILL_INTERVAL_SENTINEL);
}

static void
mutate_extra_dword(uint8_t *image)
{
   mutate_exact(image);
   put_dword(image, END, R3V_PUBLIC_RB2D_FILL_VALUE);
}

static void
mutate_shifted(uint8_t *image)
{
   fill_interval(image, OFFSET + 4, BYTES, R3V_PUBLIC_RB2D_FILL_VALUE);
}

static void
mutate_byte_swapped(uint8_t *image)
{
   fill_interval(image, OFFSET, BYTES, 0x44332211u);
}

static void
mutate_prefix(uint8_t *image)
{
   mutate_exact(image);
   image[3] = 0x00;
}

static void
mutate_suffix(uint8_t *image)
{
   mutate_exact(image);
   image[END + 1000] = 0x00;
}

static void
mutate_tail(uint8_t *image)
{
   mutate_exact(image);
   image[ALLOC - 1] = 0x00;
}

static void
mutate_partial_dword(uint8_t *image)
{
   mutate_exact(image);
   image[OFFSET + 3000 + 1] = R3V_PUBLIC_RB2D_FILL_INTERVAL_SENTINEL;
}

/* A missing dword and a byte outside the interval: the earlier
 * predicate in the order names the class. */
static void
mutate_missing_and_suffix(uint8_t *image)
{
   mutate_missing_dword(image);
   image[END + 8] = 0x00;
}

struct fixture {
   const char *name;
   void (*mutate)(uint8_t *image);
   enum r3v_public_rb2d_fill_outcome expected;
   uint32_t changed_bytes;
   uint32_t changed_dwords;
};

static const struct fixture fixtures[] = {
   {"exact_expected_output", mutate_exact, R3V_PUBLIC_RB2D_FILL_CONTROL_PASS,
    BYTES, BYTES / 4},
   {"completely_unchanged", mutate_unchanged,
    R3V_PUBLIC_RB2D_FILL_NO_DEVICE_WRITE, 0, 0},
   {"one_missing_dword", mutate_missing_dword,
    R3V_PUBLIC_RB2D_FILL_PARTIAL_WRITE, BYTES - 4, BYTES / 4 - 1},
   {"one_extra_dword_after_interval", mutate_extra_dword,
    R3V_PUBLIC_RB2D_FILL_OUTSIDE_WRITE, BYTES + 4, BYTES / 4 + 1},
   {"region_shifted_by_four", mutate_shifted,
    R3V_PUBLIC_RB2D_FILL_SHIFTED_WRITE, BYTES, BYTES / 4},
   {"byte_swapped_pattern", mutate_byte_swapped,
    R3V_PUBLIC_RB2D_FILL_PATTERN_MISMATCH, BYTES, BYTES / 4},
   {"prefix_corruption", mutate_prefix, R3V_PUBLIC_RB2D_FILL_OUTSIDE_WRITE,
    BYTES + 1, BYTES / 4 + 1},
   {"suffix_corruption", mutate_suffix, R3V_PUBLIC_RB2D_FILL_OUTSIDE_WRITE,
    BYTES + 1, BYTES / 4 + 1},
   {"tail_canary_corruption", mutate_tail,
    R3V_PUBLIC_RB2D_FILL_CANARY_CORRUPTION, BYTES + 1, BYTES / 4 + 1},
   {"one_partially_changed_dword", mutate_partial_dword,
    R3V_PUBLIC_RB2D_FILL_PATTERN_MISMATCH, BYTES - 1, BYTES / 4},
   {"missing_dword_and_suffix_byte", mutate_missing_and_suffix,
    R3V_PUBLIC_RB2D_FILL_OUTSIDE_WRITE, BYTES - 3, BYTES / 4},
};

static const enum r3v_public_rb2d_fill_predicate predicates[] = {
   R3V_PUBLIC_RB2D_FILL_PREDICATE_TAIL_CANARY,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_ANY_WRITE,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_SHIFT,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_OUTSIDE,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_PATTERN,
   R3V_PUBLIC_RB2D_FILL_PREDICATE_COMPLETE,
};

static int failures;

#define CHECK(condition, ...)                                                 \
   do {                                                                       \
      if (!(condition)) {                                                     \
         fprintf(stderr, "FAIL: ");                                           \
         fprintf(stderr, __VA_ARGS__);                                        \
         fprintf(stderr, "\n");                                               \
         failures++;                                                          \
      }                                                                       \
   } while (0)

int
main(void)
{
   const struct r3v_public_rb2d_fill_cell *cell = r3v_public_rb2d_fill_sealed_cell();
   CHECK(r3v_public_rb2d_fill_cell_valid(cell), "the sealed cell is malformed");
   struct r3v_public_rb2d_fill_cell bad = *cell;
   bad.fill_value = 0x11a52233u;
   CHECK(!r3v_public_rb2d_fill_cell_valid(&bad),
         "a pattern carrying the sentinel byte was admitted");
   bad = *cell;
   bad.fill_bytes = ALLOC;
   CHECK(!r3v_public_rb2d_fill_cell_valid(&bad),
         "a fill reaching the tail canary was admitted");
   bad = *cell;
   bad.name = "";
   CHECK(!r3v_public_rb2d_fill_cell_valid(&bad),
         "a cell with no name was admitted");

   /* The table: the sealed cell is the first row and keeps the five
    * destination values the receipt retains, every row is valid, every
    * name resolves to its own row and is unique, and a name outside the
    * table resolves to nothing. */
   uint32_t cell_count = 0;
   const struct r3v_public_rb2d_fill_cell *cells =
      r3v_public_rb2d_fill_cells(&cell_count);
   CHECK(cell_count >= 2 && cells == cell, "the sealed cell is not row 0");
   CHECK(strcmp(cell->name, "v1_public") == 0 &&
            cell->allocation_bytes == ALLOC && cell->fill_offset == 12 &&
            cell->fill_bytes == 4992 && cell->fill_value == 0x11223344u &&
            cell->tail_bytes == 64,
         "the sealed cell's declared destination moved");
   for (uint32_t i = 0; i < cell_count; i++) {
      CHECK(r3v_public_rb2d_fill_cell_valid(&cells[i]),
            "cell %s is malformed", cells[i].name);
      CHECK(r3v_public_rb2d_fill_cell_by_name(cells[i].name) == &cells[i],
            "cell %s does not resolve to its own row", cells[i].name);
      CHECK(cells[i].expected_relocation_sites ==
               cells[i].expected_window_count,
            "cell %s expects %u sites for %u windows; the emitter binds "
            "the destination once per window",
            cells[i].name, cells[i].expected_relocation_sites,
            cells[i].expected_window_count);
   }
   const struct r3v_public_rb2d_fill_cell *dense =
      r3v_public_rb2d_fill_cell_by_name("dense_16320_carrier");
   CHECK(dense != NULL && dense->pinned_pitch_bytes == 16320u &&
            dense->expected_pitch_bytes == 16320u &&
            dense->expected_window_count == 1u &&
            dense->fill_bytes == 65428u &&
            dense->evidence_scope ==
               R3V_PUBLIC_RB2D_FILL_SCOPE_CARRIER_QUALIFICATION,
         "the dense carrier cell's declaration moved");
   CHECK(r3v_public_rb2d_fill_cell_by_name("no_such_cell") == NULL,
         "an unnamed cell resolved");
   const struct r3v_public_rb2d_fill_cell *windowed =
      r3v_public_rb2d_fill_cell_by_name("v2_multiwindow_256");
   CHECK(windowed != NULL && windowed->allocation_bytes == 2097152u &&
            windowed->fill_offset == 12u &&
            windowed->fill_bytes == 2097012u &&
            windowed->pinned_pitch_bytes == 256u &&
            windowed->expected_pitch_bytes == 256u &&
            windowed->expected_window_count == 2u &&
            windowed->contract == R300_RB2D_CONTRACT_CONST_FILL_V2 &&
            windowed->route_id == R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2,
         "the windowed cell's declaration moved");

   const size_t count = sizeof(fixtures) / sizeof(fixtures[0]);
   uint8_t *image = malloc(ALLOC);
   if (image == NULL)
      return 2;

   /* Every fixture under the full predicate set. */
   for (size_t i = 0; i < count; i++) {
      r3v_public_rb2d_fill_initialize(cell, image);
      fixtures[i].mutate(image);
      struct r3v_public_rb2d_fill_report r;
      const enum r3v_public_rb2d_fill_outcome got =
         r3v_public_rb2d_fill_classify(cell, image, &r);
      printf("%-32s %-18s changed_bytes=%u changed_dwords=%u\n",
             fixtures[i].name, r3v_public_rb2d_fill_outcome_name(got),
             r.changed_bytes, r.changed_dwords);
      CHECK(got == fixtures[i].expected, "%s classified %s, expected %s",
            fixtures[i].name, r3v_public_rb2d_fill_outcome_name(got),
            r3v_public_rb2d_fill_outcome_name(fixtures[i].expected));
      CHECK(r.changed_bytes == fixtures[i].changed_bytes &&
               r.changed_dwords == fixtures[i].changed_dwords,
            "%s reported %u bytes / %u dwords, expected %u / %u",
            fixtures[i].name, r.changed_bytes, r.changed_dwords,
            fixtures[i].changed_bytes, fixtures[i].changed_dwords);
      CHECK(r3v_public_rb2d_fill_exit_status(got) == (got == 0 ? 0 : 1),
            "%s maps to exit %d", fixtures[i].name,
            r3v_public_rb2d_fill_exit_status(got));
   }

   /* Each predicate disabled alone must move at least one fixture off
    * its expected class; a predicate that moves none decides nothing. */
   for (size_t p = 0; p < sizeof(predicates) / sizeof(predicates[0]); p++) {
      const uint32_t mask = R3V_PUBLIC_RB2D_FILL_PREDICATE_ALL & ~predicates[p];
      unsigned moved = 0;
      for (size_t i = 0; i < count; i++) {
         r3v_public_rb2d_fill_initialize(cell, image);
         fixtures[i].mutate(image);
         struct r3v_public_rb2d_fill_report r;
         const enum r3v_public_rb2d_fill_outcome got =
            r3v_public_rb2d_fill_classify_masked(cell, image, mask, &r);
         if (got != fixtures[i].expected) {
            moved++;
            printf("  without %-36s %s -> %s\n",
                   r3v_public_rb2d_fill_predicate_name(predicates[p]),
                   fixtures[i].name, r3v_public_rb2d_fill_outcome_name(got));
         }
      }
      CHECK(moved > 0, "disabling %s moves no fixture; the predicate decides "
                       "nothing",
            r3v_public_rb2d_fill_predicate_name(predicates[p]));
   }
   CHECK(R3V_PUBLIC_RB2D_FILL_PREDICATE_ALL ==
            (1u << (sizeof(predicates) / sizeof(predicates[0]))) - 1u,
         "the predicate table and the ALL mask disagree");

   free(image);
   printf("r3v-public-rb2d-fill-oracle: %s (%zu fixtures, %zu predicates)\n",
          failures == 0 ? "PASS" : "FAIL", count,
          sizeof(predicates) / sizeof(predicates[0]));
   return failures == 0 ? 0 : 1;
}
