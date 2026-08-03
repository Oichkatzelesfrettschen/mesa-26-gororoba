/*
 * SPDX-License-Identifier: MIT
 *
 * Exact-index oracle for the neutral Draw output-location overlay.
 *
 * draw_find_shader_output_location() addresses a shader output by
 * gl_varying_slot, and draw_output_location_semantic() is the translation it
 * performs before searching the TGSI-keyed shader info.  Consumers that moved
 * from the semantic-pair entry point to the location entry point must observe
 * the identical output index, so this harness pins each translation against
 * the literal (semantic, index) pair the caller previously wrote.
 *
 * The generic range is the case that can silently shift: under
 * needs_texcoord_semantic the VAR range and the TEX range share one GENERIC
 * index space with different offsets.  Every draw stage scans its NIR with
 * nir_tgsi_scan_shader(nir, info, true), so the overlay pins that flag set and
 * VARYING_SLOT_VAR0 + n resolves to (GENERIC, n) over the whole range.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pipe/p_shader_tokens.h"
#include "draw/draw_context.h"

static unsigned failures;

static void
check(gl_varying_slot location, const char *location_name,
      enum tgsi_semantic expect_name, unsigned expect_index)
{
   enum tgsi_semantic name;
   unsigned index;

   draw_output_location_semantic(location, &name, &index);

   if (name != expect_name || index != expect_index) {
      fprintf(stderr,
              "FAIL %s: got (%u, %u), expected (%u, %u)\n",
              location_name, name, index, expect_name, expect_index);
      failures++;
   }
}

#define CHECK(location, name, index) check((location), #location, (name), (index))

int
main(void)
{
   /* The r300 SW-TCL vertex-layout closure: every location its emit and
    * lookup sites address, against the semantic pair each site held before
    * the overlay. */
   CHECK(VARYING_SLOT_POS,  TGSI_SEMANTIC_POSITION, 0);
   CHECK(VARYING_SLOT_PSIZ, TGSI_SEMANTIC_PSIZE,    0);
   CHECK(VARYING_SLOT_COL0, TGSI_SEMANTIC_COLOR,    0);
   CHECK(VARYING_SLOT_COL1, TGSI_SEMANTIC_COLOR,    1);
   CHECK(VARYING_SLOT_BFC0, TGSI_SEMANTIC_BCOLOR,   0);
   CHECK(VARYING_SLOT_BFC1, TGSI_SEMANTIC_BCOLOR,   1);
   CHECK(VARYING_SLOT_FACE, TGSI_SEMANTIC_FACE,     0);
   CHECK(VARYING_SLOT_PNTC, TGSI_SEMANTIC_PCOORD,   0);
   CHECK(VARYING_SLOT_FOGC, TGSI_SEMANTIC_FOG,      0);

   /* Color and back-face color are addressed as COL0 + i and BFC0 + i, which
    * holds only while the two slots stay adjacent. */
   if (VARYING_SLOT_COL1 != VARYING_SLOT_COL0 + 1 ||
       VARYING_SLOT_BFC1 != VARYING_SLOT_BFC0 + 1) {
      fprintf(stderr, "FAIL: color slot pairs are not adjacent\n");
      failures++;
   }

   /* The generic range, over the full VAR space a caller can address. */
   for (unsigned i = 0; i <= VARYING_SLOT_VAR31 - VARYING_SLOT_VAR0; i++) {
      enum tgsi_semantic name;
      unsigned index;

      draw_output_location_semantic(VARYING_SLOT_VAR0 + i, &name, &index);

      if (name != TGSI_SEMANTIC_GENERIC || index != i) {
         fprintf(stderr,
                 "FAIL VARYING_SLOT_VAR0 + %u: got (%u, %u), "
                 "expected (%u, %u)\n",
                 i, name, index, TGSI_SEMANTIC_GENERIC, i);
         failures++;
      }
   }

   /* The texcoord range shares the GENERIC space with the VAR range under a
    * different offset, so a caller that addresses TEXn must see TEXCOORD
    * rather than a GENERIC index colliding with VAR0. */
   for (unsigned i = 0; i <= VARYING_SLOT_TEX7 - VARYING_SLOT_TEX0; i++) {
      enum tgsi_semantic name;
      unsigned index;

      draw_output_location_semantic(VARYING_SLOT_TEX0 + i, &name, &index);

      if (name != TGSI_SEMANTIC_TEXCOORD || index != i) {
         fprintf(stderr,
                 "FAIL VARYING_SLOT_TEX0 + %u: got (%u, %u), "
                 "expected (%u, %u)\n",
                 i, name, index, TGSI_SEMANTIC_TEXCOORD, i);
         failures++;
      }
   }

   printf("%u failure(s)\n", failures);
   return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
