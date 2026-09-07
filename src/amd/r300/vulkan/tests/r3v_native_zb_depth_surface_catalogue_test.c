/*
 * SPDX-License-Identifier: MIT
 *
 * Every selectable depth surface holds the properties the recorder, the
 * host fill, and the queue's frozen-geometry predicate act on without
 * re-testing.  Those consumers took runtime guards before this test
 * existed; the guards were unreachable because the catalogue is fixed,
 * so the invariant moved here, where a third entry joins it through the
 * shared R3V_NATIVE_ZB_DEPTH_SURFACES list rather than by being
 * remembered.
 *
 * The release profiles compile with -DNDEBUG, which discards an assert
 * whole, side effects included.  Undefining it before <assert.h> keeps
 * every verdict here live in every profile.
 */
#undef NDEBUG

#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "amd/r300/common/r300_zb_depth_surface.h"
#include "r3v_native.h"

#include <assert.h>
#include <stdio.h>

struct catalogue_entry {
   enum r3v_native_zb_depth_surface selection;
   const char *name;
};

static const struct catalogue_entry catalogue[] = {
#define R3V_NATIVE_ZB_DEPTH_SURFACE_ENTRY(suffix, descriptor)                 \
   { R3V_NATIVE_ZB_DEPTH_SURFACE_##suffix, #descriptor },
   R3V_NATIVE_ZB_DEPTH_SURFACES(R3V_NATIVE_ZB_DEPTH_SURFACE_ENTRY)
#undef R3V_NATIVE_ZB_DEPTH_SURFACE_ENTRY
};

#define CATALOGUE_COUNT (sizeof(catalogue) / sizeof(catalogue[0]))

int
main(void)
{
   assert(CATALOGUE_COUNT >= 2u);

   for (size_t i = 0; i < CATALOGUE_COUNT; i++) {
      const struct catalogue_entry *entry = &catalogue[i];
      const struct r300_zb_depth_surface *surface =
         r3v_native_zb_depth_surface_descriptor(entry->selection);
      assert(surface != NULL);

      /* The recorder acts on the descriptor's width and geometry without
       * validating it, so validity is established here. */
      assert(r300_zb_depth_surface_check(surface) == 0);

      /* The host fill writes units of this width into a four-byte stack
       * object and divides the allocation by it. */
      assert(surface->bytes_per_pixel == 2u || surface->bytes_per_pixel == 4u);

      /* The depth oracle and the emitter both state the cell's geometry,
       * so a catalogue entry of another shape would record a cell whose
       * regions name pixels the oracle does not read. */
      assert(surface->width == R300_ZB_DEPTH_CONTROL_TARGET_WIDTH);
      assert(surface->height == R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT);
      assert(surface->pitch_pixels == R300_ZB_DEPTH_CONTROL_PITCH_PIXELS);
      assert(surface->allocation_rows ==
             R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS);

      /* The pre-draw fill is the comparison's stored operand and the
       * oracle reads the surface back as an image, so every selectable
       * surface addresses linearly from the host. */
      assert(surface->raw_allocation_mapping);
      assert(surface->uniform_packed_initialization);
      assert(surface->logical_pixel_addressing);
      assert(surface->logical_image_readback);

      /* The sentinel packs under the format, which the fill writes and
       * the oracle compares against. */
      uint32_t word = 0;
      assert(r300_zb_depth_surface_packed_sentinel(surface, &word) == 0);

      /* The queue sizes the depth allocation from this, so it is
       * positive and inside the 32-bit field that carries it. */
      const uint32_t bytes =
         r3v_native_zb_depth_surface_bytes(entry->selection);
      assert(bytes > 0u);
      assert(bytes == surface->pitch_pixels * surface->allocation_rows *
                         surface->bytes_per_pixel);

      /* Distinct entries name distinct descriptors, so a selector always
       * resolves to one surface. */
      for (size_t j = 0; j < i; j++) {
         const struct r300_zb_depth_surface *other =
            r3v_native_zb_depth_surface_descriptor(catalogue[j].selection);
         assert(surface != other);
         assert(catalogue[j].selection != entry->selection);
      }

      printf("  %-16s cpp=%u bytes=%u\n", surface->name,
             surface->bytes_per_pixel, bytes);
   }

   /* A value outside the enumeration names no descriptor and reports no
    * size, which is what the queue's refusal rests on. */
   const enum r3v_native_zb_depth_surface outside =
      (enum r3v_native_zb_depth_surface)(CATALOGUE_COUNT + 5u);
   assert(r3v_native_zb_depth_surface_descriptor(outside) == NULL);
   assert(r3v_native_zb_depth_surface_bytes(outside) == 0u);

   printf("r3v_native_zb_depth_surface_catalogue: %zu selectable surfaces "
          "hold validity, fill width, geometry, and extent\n",
          CATALOGUE_COUNT);
   return 0;
}
