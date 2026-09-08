/* SPDX-License-Identifier: MIT */

#include "r300_zb_depth_discovery.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The campaign's coordinate.  It sits away from both axes and off every
 * power-of-two boundary, so a transform that drops a low bit of x or y,
 * swaps the two, or folds a microtile index onto zero moves the observed
 * byte rather than landing on the same one by symmetry. */
#define DISCOVERY_PIXEL_X 37u
#define DISCOVERY_PIXEL_Y 21u

/* The initial code and the marker.  The initial code is the Z24 half
 * depth the depth ladder already uses as its sentinel, and the marker
 * sits below it so a draw at a nearer window-space depth stores a value
 * the comparison would also admit under LESS -- the discovery draw
 * compares ALWAYS, and a marker the ordinary comparison would reject
 * would make the two cells disagree for a reason that is not the
 * address. */
#define DISCOVERY_INITIAL_CODE 0x00800000u
#define DISCOVERY_MARKER_CODE 0x00400000u

#define DISCOVERY_SCENARIO(field_name, field_surface, field_stencil) \
   {                                                                 \
      .name = field_name,                                            \
      .surface = field_surface,                                      \
      .pixel_x = DISCOVERY_PIXEL_X,                                  \
      .pixel_y = DISCOVERY_PIXEL_Y,                                  \
      .initial_depth_code = DISCOVERY_INITIAL_CODE,                  \
      .initial_stencil = field_stencil,                              \
      .marker_depth_code = DISCOVERY_MARKER_CODE,                    \
      .allocation_bytes = R300_ZB_DISCOVERY_ALLOCATION_BYTES,        \
      .guard_bytes = R300_ZB_DISCOVERY_GUARD_BYTES,                  \
   }

const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_linear = DISCOVERY_SCENARIO(
      "z24-linear", &r300_zb_depth_surface_z24_linear, 0x00u);

const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_linear_seed_5a = DISCOVERY_SCENARIO(
      "z24-linear-seed-5a", &r300_zb_depth_surface_z24_linear, 0x5au);

const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_linear_seed_a5 = DISCOVERY_SCENARIO(
      "z24-linear-seed-a5", &r300_zb_depth_surface_z24_linear, 0xa5u);

const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_microtiled = DISCOVERY_SCENARIO(
      "z24-microtiled", &r300_zb_depth_surface_z24_microtiled, 0x00u);

const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_macrotiled = DISCOVERY_SCENARIO(
      "z24-macrotiled", &r300_zb_depth_surface_z24_macrotiled, 0x00u);

int
r300_zb_coordinate_discovery_init(
   enum r300_zb_coordinate_discovery_layout layout, uint32_t pixel_x,
   uint32_t pixel_y, uint32_t pitch_pixels, uint32_t base_bytes,
   struct r300_zb_coordinate_discovery *out)
{
   if (out == NULL || pixel_x >= 64u || pixel_y >= 64u ||
       (pitch_pixels != 64u && pitch_pixels != 96u) ||
       (base_bytes != 2048u && base_bytes != 4096u))
      return -EINVAL;

   const struct r300_zb_depth_surface *template_surface = NULL;
   const char *name = NULL;
   switch (layout) {
   case R300_ZB_COORDINATE_DISCOVERY_MICROTILED:
      template_surface = &r300_zb_depth_surface_z24_microtiled;
      name = "z24-coordinate-microtiled";
      break;
   case R300_ZB_COORDINATE_DISCOVERY_MACROTILED:
      template_surface = &r300_zb_depth_surface_z24_macrotiled;
      name = "z24-coordinate-macrotiled";
      break;
   default:
      return -EINVAL;
   }

   memset(out, 0, sizeof(*out));
   out->surface = *template_surface;
   out->surface.name = name;
   out->surface.pitch_pixels = pitch_pixels;

   struct r300_zb_depth_layout computed;
   if (r300_zb_depth_layout_compute(&out->surface, base_bytes, &computed) != 0)
      return -EINVAL;
   if (computed.total_bytes >
       UINT64_MAX - R300_ZB_COORDINATE_DISCOVERY_TAIL_BYTES)
      return -EINVAL;

   out->scenario = (struct r300_zb_depth_discovery_scenario){
      .name = name,
      .surface = &out->surface,
      .pixel_x = pixel_x,
      .pixel_y = pixel_y,
      .initial_depth_code = DISCOVERY_INITIAL_CODE,
      .initial_stencil = 0u,
      .marker_depth_code = DISCOVERY_MARKER_CODE,
      .allocation_bytes =
         computed.total_bytes + R300_ZB_COORDINATE_DISCOVERY_TAIL_BYTES,
      .guard_bytes = base_bytes,
   };
   return r300_zb_depth_discovery_scenario_check(&out->scenario);
}

int
r300_zb_coordinate_discovery_declaration(
   const struct r300_zb_depth_discovery_scenario *scenario, char *bytes,
   size_t capacity)
{
   if (bytes == NULL || capacity == 0u || scenario == NULL ||
       r300_zb_depth_discovery_scenario_check(scenario) != 0 ||
       scenario->surface->microtile != R300_ZB_MICROTILE_TILED)
      return -EINVAL;

   const char *layout = NULL;
   switch (scenario->surface->macrotile) {
   case R300_ZB_MACROTILE_LINEAR:
      layout = "microtiled";
      break;
   case R300_ZB_MACROTILE_TILED:
      layout = "macrotiled";
      break;
   default:
      return -EINVAL;
   }

   struct r300_zb_depth_layout computed;
   if (r300_zb_depth_discovery_layout(scenario, &computed) != 0)
      return -EINVAL;
   const int length = snprintf(
      bytes, capacity,
      "schema=r300-zb-coordinate-discovery/1\n"
      "layout=%s\n"
      "pixel_x=%u\n"
      "pixel_y=%u\n"
      "pitch_pixels=%u\n"
      "base_bytes=%u\n"
      "allocation_bytes=%llu\n"
      "storage_bytes=%llu\n"
      "initial_depth_code=0x%06x\n"
      "initial_stencil=0x%02x\n"
      "marker_depth_code=0x%06x\n",
      layout, scenario->pixel_x, scenario->pixel_y,
      scenario->surface->pitch_pixels, scenario->guard_bytes,
      (unsigned long long)scenario->allocation_bytes,
      (unsigned long long)computed.storage_bytes,
      scenario->initial_depth_code, scenario->initial_stencil,
      scenario->marker_depth_code);
   if (length < 0 || (size_t)length >= capacity)
      return -EINVAL;
   return length;
}

int
r300_zb_depth_discovery_scenario_check(
   const struct r300_zb_depth_discovery_scenario *scenario)
{
   if (scenario == NULL || scenario->name == NULL ||
       scenario->surface == NULL)
      return -EINVAL;

   const struct r300_zb_depth_surface *surface = scenario->surface;
   const int surface_rc = r300_zb_depth_surface_check(surface);
   if (surface_rc != 0)
      return surface_rc;

   /* Raw mapping and uniform initialization are the whole host
    * requirement.  A constant image is invariant under the permutation
    * tiling applies, so the before state is known without an address
    * transform, and the after state is read as an unordered set of
    * words.  Demanding logical addressing here would refuse both tiled
    * surfaces and leave the apparatus able to discover only what is
    * already resolved. */
   if (!surface->raw_allocation_mapping ||
       !surface->uniform_packed_initialization)
      return -EINVAL;

   if (scenario->pixel_x >= surface->width ||
       scenario->pixel_y >= surface->height)
      return -EINVAL;

   /* Both states pack, and they differ.  A marker equal to the initial
    * code would leave a written slot indistinguishable from an
    * untouched one. */
   uint32_t initial_word = 0;
   uint32_t marker_word = 0;
   if (r300_zb_depth_pack(surface, scenario->initial_depth_code,
                          scenario->initial_stencil, &initial_word) != 0)
      return -EINVAL;
   if (r300_zb_depth_pack(surface, scenario->marker_depth_code,
                          scenario->initial_stencil, &marker_word) != 0)
      return -EINVAL;
   if (scenario->marker_depth_code == scenario->initial_depth_code)
      return -EINVAL;

   /* The layout the allocation must hold, and the guard alignment it
    * demands.  r300_zb_depth_layout_compute refuses a guard off the base
    * alignment, so this call is also the guard check. */
   struct r300_zb_depth_layout layout;
   const int layout_rc =
      r300_zb_depth_layout_compute(surface, scenario->guard_bytes, &layout);
   if (layout_rc != 0)
      return layout_rc;
   if (scenario->allocation_bytes < layout.total_bytes)
      return -EINVAL;

   return 0;
}

int
r300_zb_depth_discovery_layout(
   const struct r300_zb_depth_discovery_scenario *scenario,
   struct r300_zb_depth_layout *out)
{
   if (out == NULL)
      return -EINVAL;
   memset(out, 0, sizeof(*out));

   const int rc = r300_zb_depth_discovery_scenario_check(scenario);
   if (rc != 0)
      return rc;
   return r300_zb_depth_layout_compute(scenario->surface,
                                       scenario->guard_bytes, out);
}

int
r300_zb_depth_discovery_layout_validate(
   const struct r300_zb_depth_layout *layout, uint64_t size_bytes)
{
   if (layout == NULL || size_bytes == 0u)
      return -EINVAL;

   const uint32_t bpp = layout->bytes_per_pixel;
   if (bpp == 0u || layout->storage_bytes == 0u)
      return -EINVAL;
   /* A partial slot at the end of the envelope would leave bytes the
    * scan can neither pair into a word nor classify as anything else. */
   if (layout->storage_bytes % (uint64_t)bpp != 0u)
      return -EINVAL;

   /* Each interval's end is computed once and checked for wrap before it
    * is compared, so an interval whose length runs past the 64-bit range
    * refuses instead of ordering correctly by wrapping. */
   const uint64_t prefix_start = layout->prefix_guard_offset_bytes;
   const uint64_t prefix_len = layout->prefix_guard_bytes;
   const uint64_t storage_start = layout->base_offset_bytes;
   const uint64_t storage_len = layout->storage_bytes;
   const uint64_t suffix_start = layout->suffix_guard_offset_bytes;
   const uint64_t suffix_len = layout->suffix_guard_bytes;

   if (prefix_len > UINT64_MAX - prefix_start ||
       storage_len > UINT64_MAX - storage_start ||
       suffix_len > UINT64_MAX - suffix_start)
      return -EINVAL;

   const uint64_t prefix_end = prefix_start + prefix_len;
   const uint64_t storage_end = storage_start + storage_len;
   const uint64_t suffix_end = suffix_start + suffix_len;

   /* The three intervals are adjacent in this order, which is stronger
    * than non-overlap and is what the layout states it built. */
   if (prefix_start != 0u)
      return -EINVAL;
   if (prefix_end != storage_start)
      return -EINVAL;
   if (storage_end != suffix_start)
      return -EINVAL;

   /* Every interval inside the mapping.  The suffix end is the largest,
    * so it bounds the other two, but each is checked on its own so a
    * layout with a reordered field refuses rather than passing on the
    * strength of one comparison. */
   if (prefix_end > size_bytes || storage_end > size_bytes ||
       suffix_end > size_bytes)
      return -EINVAL;

   /* The envelope base sits on the alignment the layout states, so a
    * slot boundary is a tile boundary. */
   if (layout->base_alignment_bytes == 0u ||
       storage_start % (uint64_t)layout->base_alignment_bytes != 0u)
      return -EINVAL;
   if (storage_start % (uint64_t)bpp != 0u)
      return -EINVAL;

   return 0;
}

int
r300_zb_depth_discovery_initial_word(
   const struct r300_zb_depth_discovery_scenario *scenario, uint32_t *word_out)
{
   if (word_out == NULL)
      return -EINVAL;
   const int rc = r300_zb_depth_discovery_scenario_check(scenario);
   if (rc != 0)
      return rc;
   return r300_zb_depth_pack(scenario->surface, scenario->initial_depth_code,
                             scenario->initial_stencil, word_out);
}

int
r300_zb_depth_discovery_fill_initial(
   const struct r300_zb_depth_discovery_scenario *scenario,
   const struct r300_zb_depth_layout *layout, void *bytes)
{
   if (layout == NULL || bytes == NULL)
      return -EINVAL;
   uint32_t initial_word = 0;
   const int rc = r300_zb_depth_discovery_initial_word(scenario,
                                                       &initial_word);
   if (rc != 0)
      return rc;
   const int layout_rc = r300_zb_depth_discovery_layout_validate(
      layout, scenario->allocation_bytes);
   if (layout_rc != 0)
      return layout_rc;

   /* The guard fill covers the whole allocation first, so the slack past
    * a smaller envelope holds a known value too and a change in it is
    * visible.  The observation still counts guard and unclaimed bytes
    * apart: a guard verdict is a claim the layout states, and the slack
    * carries no such claim. */
   uint8_t *out = bytes;
   memset(out, R300_ZB_DISCOVERY_GUARD_FILL,
          (size_t)scenario->allocation_bytes);
   const uint32_t bpp = layout->bytes_per_pixel;
   for (uint64_t off = layout->base_offset_bytes;
        off < layout->base_offset_bytes + layout->storage_bytes; off += bpp)
      for (uint32_t i = 0; i < bpp; i++)
         out[off + i] = (uint8_t)(initial_word >> (8u * i));
   return 0;
}

enum r300_zb_discovery_region
r300_zb_depth_discovery_region_of(const struct r300_zb_depth_layout *layout,
                                  uint64_t size_bytes, uint64_t byte_offset)
{
   /* Outside the mapping is not a class this enumeration carries, and a
    * caller reaching here with such an offset has already left its scan
    * bound.  Unclaimed is the answer that claims the least. */
   if (layout == NULL || byte_offset >= size_bytes)
      return R300_ZB_DISCOVERY_REGION_UNCLAIMED;
   if (r300_zb_depth_layout_is_guard_byte(layout, byte_offset))
      return R300_ZB_DISCOVERY_REGION_GUARD;
   if (byte_offset >= layout->base_offset_bytes &&
       byte_offset - layout->base_offset_bytes < layout->storage_bytes)
      return R300_ZB_DISCOVERY_REGION_STORAGE;
   return R300_ZB_DISCOVERY_REGION_UNCLAIMED;
}

/* Reads one packed word out of a mapping at a byte offset, in the
 * little-endian order the host wrote it and the device reads it. */
static uint32_t
read_word(const uint8_t *bytes, uint64_t offset, uint32_t bpp)
{
   uint32_t word = 0;
   for (uint32_t i = 0; i < bpp; i++)
      word |= (uint32_t)bytes[offset + i] << (8u * i);
   return word;
}

void
r300_zb_depth_discovery_observe(
   const struct r300_zb_depth_discovery_scenario *scenario,
   const struct r300_zb_depth_layout *layout, const void *before,
   const void *after, uint64_t size_bytes,
   struct r300_zb_discovery_observation *out)
{
   if (out == NULL)
      return;
   memset(out, 0, sizeof(*out));

   if (scenario == NULL || layout == NULL || before == NULL || after == NULL)
      return;
   if (r300_zb_depth_discovery_scenario_check(scenario) != 0)
      return;
   if (size_bytes != scenario->allocation_bytes)
      return;
   /* The layout is held to the mapping before a byte is classified: the
    * guard predicate answers about the layout it is handed, so a layout
    * describing a different allocation would classify confidently and
    * wrongly. */
   if (r300_zb_depth_discovery_layout_validate(layout, size_bytes) != 0)
      return;
   if (layout->bytes_per_pixel != scenario->surface->bytes_per_pixel)
      return;

   const uint8_t *b = before;
   const uint8_t *a = after;
   const uint32_t bpp = layout->bytes_per_pixel;

   out->pixel_x = scenario->pixel_x;
   out->pixel_y = scenario->pixel_y;

   /* Guard and unclaimed bytes first, judged one byte at a time.
    * Neither carries a pixel, so neither has a packing to read, and a
    * change in either is a byte-level fact. */
   for (uint64_t off = 0; off < size_bytes; off++) {
      const enum r300_zb_discovery_region region =
         r300_zb_depth_discovery_region_of(layout, size_bytes, off);
      if (region == R300_ZB_DISCOVERY_REGION_STORAGE)
         continue;
      const bool changed = a[off] != b[off];
      if (region == R300_ZB_DISCOVERY_REGION_GUARD) {
         out->guard_bytes_inspected++;
         if (changed)
            out->guard_bytes_changed++;
      } else {
         out->unclaimed_bytes_inspected++;
         if (changed)
            out->unclaimed_bytes_changed++;
      }
   }

   /* Storage slots in offset order.  The scan reports whatever offset
    * moved; whether that offset is the one an address model predicts is
    * a question for the validator that owns the model. */
   const uint64_t slots = layout->storage_bytes / (uint64_t)bpp;
   for (uint64_t i = 0; i < slots; i++) {
      const uint64_t off = layout->base_offset_bytes + i * (uint64_t)bpp;
      out->slots_inspected++;

      const uint32_t before_word = read_word(b, off, bpp);
      const uint32_t after_word = read_word(a, off, bpp);
      if (before_word == after_word) {
         out->slots_unchanged++;
         continue;
      }

      /* A word the format cannot decode is still a change, and it is
       * reported with both components zero rather than dropped: an
       * undecodable word past a depth write is itself the finding. */
      uint32_t before_depth = 0, before_stencil = 0;
      uint32_t after_depth = 0, after_stencil = 0;
      const bool before_ok =
         r300_zb_depth_unpack(scenario->surface, before_word, &before_depth,
                              &before_stencil) == 0;
      const bool after_ok =
         r300_zb_depth_unpack(scenario->surface, after_word, &after_depth,
                              &after_stencil) == 0;

      /* Two words that differ must differ in at least one component, so
       * a decodable pair falls in exactly one of the three changed
       * classes.  An undecodable pair is classified as both changed,
       * which is the class that overstates nothing about which
       * component the device left alone. */
      const bool decodable = before_ok && after_ok;
      const bool depth_changed =
         decodable ? before_depth != after_depth : true;
      const bool stencil_changed =
         decodable ? before_stencil != after_stencil : true;

      if (depth_changed && stencil_changed)
         out->slots_both++;
      else if (depth_changed)
         out->slots_depth_only++;
      else
         out->slots_stencil_only++;
      if (depth_changed)
         out->depth_locations++;

      if (out->change_count >= R300_ZB_DISCOVERY_MAX_CHANGES) {
         out->change_overflow = true;
         continue;
      }
      out->changes[out->change_count++] =
         (struct r300_zb_discovery_slot_change){
            .byte_offset = off,
            .before_word = before_word,
            .after_word = after_word,
            .before_depth = before_depth,
            .after_depth = after_depth,
            .before_stencil = before_stencil,
            .after_stencil = after_stencil,
            .depth_changed = depth_changed,
            .stencil_changed = stencil_changed,
         };
   }

   out->judged = true;
}

void
r300_zb_depth_discovery_color_observe(
   const uint32_t *pixels, uint64_t size_bytes, uint32_t pitch_pixels,
   uint32_t width, uint32_t height, uint32_t rect_x, uint32_t rect_y,
   uint32_t rect_width, uint32_t rect_height, uint32_t sentinel,
   uint32_t draw_color, struct r300_zb_discovery_color_verdict *out)
{
   if (out == NULL)
      return;
   memset(out, 0, sizeof(*out));
   if (pixels == NULL || pitch_pixels == 0u || width == 0u || height == 0u)
      return;
   if (width > pitch_pixels)
      return;
   if (rect_width == 0u || rect_height == 0u)
      return;
   /* The rectangle is stated in render-extent coordinates and must lie
    * inside them; a rectangle that does not is a declaration error, not
    * a device result. */
   if (rect_x >= width || rect_y >= height)
      return;
   if (rect_width > width - rect_x || rect_height > height - rect_y)
      return;
   /* The sentinel and the draw color must differ, or no pixel carries a
    * distinguishable state and every count would be an artifact of the
    * comparison. */
   if (sentinel == draw_color)
      return;
   if (size_bytes / 4u < (uint64_t)pitch_pixels * (uint64_t)height)
      return;

   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
         const uint32_t pixel = pixels[(uint64_t)y * pitch_pixels + x];
         /* Membership comes from the declared rectangle alone.  Reading
          * the emitted scissor back and calling its region correct
          * would check the emitter against itself. */
         const bool inside = x >= rect_x && x - rect_x < rect_width &&
                             y >= rect_y && y - rect_y < rect_height;
         if (inside) {
            out->inside_samples++;
            if (pixel == draw_color)
               out->inside_colored++;
         } else {
            /* An outside pixel is judged against the sentinel it was
             * filled with, not against the draw color.  A pixel the
             * device moved to some third value carries no draw color and
             * would leave a draw-color count at zero, which would read
             * as containment while the scissor had in fact admitted
             * it. */
            out->outside_samples++;
            if (pixel != sentinel)
               out->outside_colored++;
         }
      }
   }

   /* Everything the allocation holds past the render extent: the
    * padding band inside each rendered row's pitch, and every row the
    * allocation carries beyond the extent.  The scissor confines the
    * write to one pixel, so any change out here is a write past the
    * target. */
   const uint64_t total_pixels = size_bytes / 4u;
   for (uint64_t i = 0; i < total_pixels; i++) {
      const uint64_t x = i % pitch_pixels;
      const uint64_t y = i / pitch_pixels;
      if (x < width && y < height)
         continue;
      out->beyond_samples++;
      if (pixels[i] != sentinel)
         out->beyond_changed++;
   }

   out->exact = out->inside_samples > 0u &&
                out->inside_colored == out->inside_samples &&
                out->outside_colored == 0u && out->beyond_changed == 0u;
   out->judged = true;
}
