/*
 * SPDX-License-Identifier: MIT
 *
 * Command-line face of the RB2D fill oracle: emits the initialized
 * destination image and classifies a retained one, so a check script
 * can judge a destination.bin without the application that produced it.
 *
 * Usage: r3v_public_rb2d_fill_oracle_tool --emit-initial <file>
 *        r3v_public_rb2d_fill_oracle_tool --classify <file>
 *
 * --classify prints the report as key=value and exits with the
 * outcome's status (0 CONTROL_PASS alone).
 */

#include "r3v_public_rb2d_fill_oracle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
   const struct r3v_public_rb2d_fill_cell *cell = r3v_public_rb2d_fill_sealed_cell();
   if (argc != 3) {
      fprintf(stderr, "usage: %s --emit-initial|--classify <file>\n", argv[0]);
      return 2;
   }
   uint8_t *image = malloc(cell->allocation_bytes);
   if (image == NULL)
      return 2;
   if (strcmp(argv[1], "--emit-initial") == 0) {
      r3v_public_rb2d_fill_initialize(cell, image);
      FILE *f = fopen(argv[2], "wb");
      if (f == NULL || fwrite(image, 1, cell->allocation_bytes, f) !=
                          cell->allocation_bytes) {
         fprintf(stderr, "cannot write %s\n", argv[2]);
         return 2;
      }
      fclose(f);
      free(image);
      return 0;
   }
   if (strcmp(argv[1], "--classify") != 0) {
      fprintf(stderr, "unknown mode %s\n", argv[1]);
      return 2;
   }
   FILE *f = fopen(argv[2], "rb");
   if (f == NULL) {
      fprintf(stderr, "cannot read %s\n", argv[2]);
      return 2;
   }
   const size_t got = fread(image, 1, cell->allocation_bytes, f);
   const bool exact = got == cell->allocation_bytes && fgetc(f) == EOF;
   fclose(f);
   if (!exact) {
      fprintf(stderr, "%s is not exactly %u bytes\n", argv[2],
              cell->allocation_bytes);
      return 2;
   }
   struct r3v_public_rb2d_fill_report r;
   const enum r3v_public_rb2d_fill_outcome outcome =
      r3v_public_rb2d_fill_classify(cell, image, &r);
   printf("outcome=%s\nchanged_bytes=%u\nchanged_dwords=%u\n"
          "expected_changed_bytes=%u\nexpected_changed_dwords=%u\n"
          "interval_pattern_dwords=%u\ninterval_sentinel_dwords=%u\n"
          "interval_other_dwords=%u\noutside_changed_bytes=%u\n"
          "tail_changed_bytes=%u\nshifted=%d\n",
          r3v_public_rb2d_fill_outcome_name(outcome), r.changed_bytes,
          r.changed_dwords, cell->fill_bytes, cell->fill_bytes / 4,
          r.interval_pattern_dwords, r.interval_sentinel_dwords,
          r.interval_other_dwords, r.outside_changed_bytes,
          r.tail_changed_bytes, r.shifted ? 1 : 0);
   free(image);
   return r3v_public_rb2d_fill_exit_status(outcome);
}
