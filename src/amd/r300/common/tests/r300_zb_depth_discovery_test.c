/* SPDX-License-Identifier: MIT */

/* Asserts carry this suite's verdicts, and a release profile compiles
 * them out through NDEBUG, so the definition is removed here before any
 * header reaches the preprocessor. */
#undef NDEBUG

#include "r300_zb_depth_discovery.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The three envelopes the campaign admits, stated as literals rather
 * than recomputed through the helper under test.  Each is
 * pitch_bytes * storage_rows for its tiling: linear stores the 65
 * allocation rows unrounded, the 4x2 microtile rounds 65 up to 66, and
 * the 32x16 macrotile rounds it up to 80.
 *
 *   mode                    rows  bytes    layout span   unclaimed
 *   linear                    65  16640    20736          3840
 *   microtiled                66  16896    20992          3584
 *   microtiled + macrotiled   80  20480    24576             0
 *
 * The constant 24576-byte allocation is the macrotiled span exactly, so
 * the other two rungs carry slack past their suffix guard.  That slack
 * is the reason the observation classifies three regions and not two. */
struct expected_layout {
   const struct r300_zb_depth_discovery_scenario *scenario;
   uint64_t storage_bytes;
   uint64_t storage_start;
   uint64_t storage_end;
   uint64_t suffix_end;
   uint64_t unclaimed_bytes;
};

static const struct expected_layout expected_layouts[] = {
   { &r300_zb_depth_discovery_z24_linear, 16640u, 2048u, 18688u, 20736u,
     3840u },
   { &r300_zb_depth_discovery_z24_microtiled, 16896u, 2048u, 18944u, 20992u,
     3584u },
   { &r300_zb_depth_discovery_z24_macrotiled, 20480u, 2048u, 22528u, 24576u,
     0u },
};

static void
test_expected_layouts(void)
{
   for (size_t i = 0; i < sizeof(expected_layouts) / sizeof(*expected_layouts);
        i++) {
      const struct expected_layout *e = &expected_layouts[i];
      assert(r300_zb_depth_discovery_scenario_check(e->scenario) == 0);

      struct r300_zb_depth_layout layout;
      assert(r300_zb_depth_discovery_layout(e->scenario, &layout) == 0);

      assert(layout.storage_bytes == e->storage_bytes);
      assert(layout.base_offset_bytes == e->storage_start);
      assert(layout.base_offset_bytes + layout.storage_bytes ==
             e->storage_end);
      assert(layout.suffix_guard_offset_bytes == e->storage_end);
      assert(layout.total_bytes == e->suffix_end);
      assert(e->scenario->allocation_bytes ==
             R300_ZB_DISCOVERY_ALLOCATION_BYTES);
      assert(e->scenario->allocation_bytes - layout.total_bytes ==
             e->unclaimed_bytes);

      assert(r300_zb_depth_discovery_layout_validate(
                &layout, e->scenario->allocation_bytes) == 0);

      /* Region classification at each boundary, from the literals. */
      assert(r300_zb_depth_discovery_region_of(&layout, 24576u, 0u) ==
             R300_ZB_DISCOVERY_REGION_GUARD);
      assert(r300_zb_depth_discovery_region_of(&layout, 24576u, 2047u) ==
             R300_ZB_DISCOVERY_REGION_GUARD);
      assert(r300_zb_depth_discovery_region_of(&layout, 24576u,
                                               e->storage_start) ==
             R300_ZB_DISCOVERY_REGION_STORAGE);
      assert(r300_zb_depth_discovery_region_of(&layout, 24576u,
                                               e->storage_end - 1u) ==
             R300_ZB_DISCOVERY_REGION_STORAGE);
      assert(r300_zb_depth_discovery_region_of(&layout, 24576u,
                                               e->storage_end) ==
             R300_ZB_DISCOVERY_REGION_GUARD);
      assert(r300_zb_depth_discovery_region_of(&layout, 24576u,
                                               e->suffix_end - 1u) ==
             R300_ZB_DISCOVERY_REGION_GUARD);
      if (e->unclaimed_bytes > 0u)
         assert(r300_zb_depth_discovery_region_of(&layout, 24576u,
                                                  e->suffix_end) ==
                R300_ZB_DISCOVERY_REGION_UNCLAIMED);
   }
}

/* The seeds reach the device through the host fill alone, so their
 * packed words are pinned against literals here: an experiment whose
 * initial image is wrong emits the same stream as one whose image is
 * right. */
static void
test_seed_words(void)
{
   uint32_t word = 0;
   assert(r300_zb_depth_discovery_initial_word(
             &r300_zb_depth_discovery_z24_linear, &word) == 0);
   assert(word == 0x80000000u);
   assert(r300_zb_depth_discovery_initial_word(
             &r300_zb_depth_discovery_z24_linear_seed_5a, &word) == 0);
   assert(word == 0x8000005au);
   assert(r300_zb_depth_discovery_initial_word(
             &r300_zb_depth_discovery_z24_linear_seed_a5, &word) == 0);
   assert(word == 0x800000a5u);

   /* The three linear scenarios differ in their initial image and in
    * nothing a command stream carries. */
   assert(r300_zb_depth_discovery_z24_linear_seed_5a.marker_depth_code ==
          r300_zb_depth_discovery_z24_linear.marker_depth_code);
   assert(r300_zb_depth_discovery_z24_linear_seed_5a.pixel_x ==
          r300_zb_depth_discovery_z24_linear.pixel_x);
   assert(r300_zb_depth_discovery_z24_linear_seed_5a.pixel_y ==
          r300_zb_depth_discovery_z24_linear.pixel_y);
}

/* A scenario a mutated field must refuse.  Each arm changes one field of
 * an admitted scenario, so a check that stopped reading a field is
 * caught by the arm that names it. */
static void
test_scenario_refusals(void)
{
   const struct r300_zb_depth_discovery_scenario base =
      r300_zb_depth_discovery_z24_linear;
   assert(r300_zb_depth_discovery_scenario_check(&base) == 0);
   assert(r300_zb_depth_discovery_scenario_check(NULL) == -EINVAL);

   struct r300_zb_depth_discovery_scenario bad;

   /* The coordinate leaves the render extent. */
   bad = base;
   bad.pixel_x = base.surface->width;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);
   bad = base;
   bad.pixel_y = base.surface->height;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);

   /* The marker equals the initial code, so a written slot would be
    * indistinguishable from an untouched one. */
   bad = base;
   bad.marker_depth_code = base.initial_depth_code;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);

   /* A depth code wider than the format's 24-bit field. */
   bad = base;
   bad.initial_depth_code = 0x01000000u;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);
   bad = base;
   bad.marker_depth_code = 0x01000000u;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);

   /* A stencil wider than the byte the format stores. */
   bad = base;
   bad.initial_stencil = 0x100u;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);

   /* An allocation short of the layout it must hold. */
   bad = base;
   bad.allocation_bytes = 20735u;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);

   /* A guard off the base alignment. */
   bad = base;
   bad.guard_bytes = 2047u;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);

   /* Z16 stores no stencil, so a seeded scenario over it refuses rather
    * than silently dropping the seed. */
   bad = base;
   bad.surface = &r300_zb_depth_surface_z16_linear;
   bad.initial_depth_code = 0x8000u;
   bad.marker_depth_code = 0x4000u;
   bad.initial_stencil = 0x5au;
   assert(r300_zb_depth_discovery_scenario_check(&bad) != 0);
}

/* The layout validator against layouts that do not describe their
 * mapping.  Each arm corrupts one structural fact of a layout the helper
 * produced, so a validator that dropped a comparison is caught by the
 * arm that names it. */
static void
test_layout_validate_refusals(void)
{
   struct r300_zb_depth_layout good;
   assert(r300_zb_depth_discovery_layout(&r300_zb_depth_discovery_z24_linear,
                                         &good) == 0);
   assert(r300_zb_depth_discovery_layout_validate(&good, 24576u) == 0);

   assert(r300_zb_depth_discovery_layout_validate(NULL, 24576u) == -EINVAL);
   assert(r300_zb_depth_discovery_layout_validate(&good, 0u) == -EINVAL);

   struct r300_zb_depth_layout bad;

   /* The mapping is shorter than the layout spans. */
   assert(r300_zb_depth_discovery_layout_validate(&good, 20735u) == -EINVAL);

   /* The prefix guard and the envelope overlap. */
   bad = good;
   bad.base_offset_bytes = good.base_offset_bytes - 32u;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);

   /* A gap between the prefix guard and the envelope: bytes belonging to
    * neither, which the adjacency requirement refuses. */
   bad = good;
   bad.prefix_guard_bytes = good.prefix_guard_bytes - 32u;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);

   /* The suffix guard does not begin where the envelope ends. */
   bad = good;
   bad.suffix_guard_offset_bytes = good.suffix_guard_offset_bytes + 32u;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);

   /* The prefix guard does not begin at zero. */
   bad = good;
   bad.prefix_guard_offset_bytes = 32u;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);

   /* The envelope is not a whole number of packed slots. */
   bad = good;
   bad.storage_bytes = good.storage_bytes + 2u;
   bad.suffix_guard_offset_bytes = bad.base_offset_bytes + bad.storage_bytes;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);

   /* The envelope base leaves its stated alignment. */
   bad = good;
   bad.base_alignment_bytes = 4096u;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);

   /* A length that would wrap the 64-bit range. */
   bad = good;
   bad.suffix_guard_bytes = UINT64_MAX;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);

   /* Zero pixel width, so no slot has a size. */
   bad = good;
   bad.bytes_per_pixel = 0u;
   assert(r300_zb_depth_discovery_layout_validate(&bad, 24576u) == -EINVAL);
}

/* One initialized allocation: guards at the guard fill, the envelope at
 * the scenario's packed initial word, the slack past the layout at a
 * third value so a scan that misclassifies it is visible. */
#define UNCLAIMED_FILL 0x5eu

static void
build_initial(const struct r300_zb_depth_discovery_scenario *scenario,
              const struct r300_zb_depth_layout *layout, uint8_t *bytes)
{
   uint32_t word = 0;
   assert(r300_zb_depth_discovery_initial_word(scenario, &word) == 0);

   memset(bytes, UNCLAIMED_FILL, scenario->allocation_bytes);
   for (uint64_t off = 0; off < scenario->allocation_bytes; off++)
      if (r300_zb_depth_layout_is_guard_byte(layout, off))
         bytes[off] = R300_ZB_DISCOVERY_GUARD_FILL;

   const uint32_t bpp = layout->bytes_per_pixel;
   for (uint64_t off = layout->base_offset_bytes;
        off < layout->base_offset_bytes + layout->storage_bytes; off += bpp)
      for (uint32_t i = 0; i < bpp; i++)
         bytes[off + i] = (uint8_t)(word >> (8u * i));
}

static void
poke_word(uint8_t *bytes, uint64_t offset, uint32_t word, uint32_t bpp)
{
   for (uint32_t i = 0; i < bpp; i++)
      bytes[offset + i] = (uint8_t)(word >> (8u * i));
}

/* A depth write anywhere legal is a valid observation reporting its own
 * offset.  An oracle that admitted only the row-major byte would be
 * asking the address model it is supposed to be discovering. */
static void
test_write_anywhere_in_envelope(void)
{
   const struct r300_zb_depth_discovery_scenario *scenario =
      &r300_zb_depth_discovery_z24_macrotiled;
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_discovery_layout(scenario, &layout) == 0);

   uint8_t *before = malloc(scenario->allocation_bytes);
   uint8_t *after = malloc(scenario->allocation_bytes);
   assert(before != NULL && after != NULL);
   build_initial(scenario, &layout, before);

   uint32_t marker = 0;
   assert(r300_zb_depth_pack(scenario->surface, scenario->marker_depth_code,
                             scenario->initial_stencil, &marker) == 0);

   /* First slot, last slot, a microtile interior, and a byte past a
    * macrotile boundary: four physical positions no single address rule
    * makes equivalent. */
   const uint64_t offsets[] = {
      layout.base_offset_bytes,
      layout.base_offset_bytes + layout.storage_bytes - 4u,
      layout.base_offset_bytes + 44u,
      layout.base_offset_bytes + 2048u + 132u,
   };

   for (size_t i = 0; i < sizeof(offsets) / sizeof(*offsets); i++) {
      memcpy(after, before, scenario->allocation_bytes);
      poke_word(after, offsets[i], marker, layout.bytes_per_pixel);

      struct r300_zb_discovery_observation obs;
      r300_zb_depth_discovery_observe(scenario, &layout, before, after,
                                      scenario->allocation_bytes, &obs);
      assert(obs.judged);
      assert(obs.depth_locations == 1u);
      assert(obs.slots_depth_only == 1u);
      assert(obs.slots_stencil_only == 0u);
      assert(obs.slots_both == 0u);
      assert(obs.change_count == 1u);
      assert(!obs.change_overflow);
      assert(obs.changes[0].byte_offset == offsets[i]);
      assert(obs.changes[0].depth_changed);
      assert(!obs.changes[0].stencil_changed);
      assert(obs.changes[0].after_depth == scenario->marker_depth_code);
      assert(obs.changes[0].before_depth == scenario->initial_depth_code);
      assert(obs.guard_bytes_changed == 0u);
      assert(obs.unclaimed_bytes_changed == 0u);
      /* The four classes partition the inspected slots exactly. */
      assert(obs.slots_unchanged + obs.slots_depth_only +
                obs.slots_stencil_only + obs.slots_both ==
             obs.slots_inspected);
      assert(obs.slots_inspected == layout.storage_bytes / 4u);
   }

   free(before);
   free(after);
}

/* The three region counts against the literal expectations, on a run
 * where nothing changed.  This is where a rung's slack becomes visible:
 * the linear envelope leaves 3840 unclaimed bytes and the macrotiled one
 * leaves none. */
static void
test_region_denominators(void)
{
   for (size_t i = 0; i < sizeof(expected_layouts) / sizeof(*expected_layouts);
        i++) {
      const struct expected_layout *e = &expected_layouts[i];
      struct r300_zb_depth_layout layout;
      assert(r300_zb_depth_discovery_layout(e->scenario, &layout) == 0);

      uint8_t *before = malloc(e->scenario->allocation_bytes);
      uint8_t *after = malloc(e->scenario->allocation_bytes);
      assert(before != NULL && after != NULL);
      build_initial(e->scenario, &layout, before);
      memcpy(after, before, e->scenario->allocation_bytes);

      struct r300_zb_discovery_observation obs;
      r300_zb_depth_discovery_observe(e->scenario, &layout, before, after,
                                      e->scenario->allocation_bytes, &obs);
      assert(obs.judged);
      assert(obs.guard_bytes_inspected == 2u * R300_ZB_DISCOVERY_GUARD_BYTES);
      assert(obs.unclaimed_bytes_inspected == e->unclaimed_bytes);
      assert(obs.slots_inspected == e->storage_bytes / 4u);
      assert(obs.guard_bytes_inspected + obs.unclaimed_bytes_inspected +
                (uint64_t)obs.slots_inspected * 4u ==
             e->scenario->allocation_bytes);
      assert(obs.depth_locations == 0u);
      assert(obs.change_count == 0u);
      assert(obs.slots_unchanged == obs.slots_inspected);

      free(before);
      free(after);
   }
}

/* The classifications the campaign's controls and its failure modes
 * produce, each demanded exactly rather than through a nonzero exit. */
static void
test_observation_classes(void)
{
   const struct r300_zb_depth_discovery_scenario *scenario =
      &r300_zb_depth_discovery_z24_linear_seed_5a;
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_discovery_layout(scenario, &layout) == 0);

   const uint64_t bytes = scenario->allocation_bytes;
   uint8_t *before = malloc(bytes);
   uint8_t *after = malloc(bytes);
   assert(before != NULL && after != NULL);
   build_initial(scenario, &layout, before);

   uint32_t initial = 0;
   assert(r300_zb_depth_discovery_initial_word(scenario, &initial) == 0);
   const uint64_t first = layout.base_offset_bytes;
   const uint64_t second = layout.base_offset_bytes + 4u;
   struct r300_zb_discovery_observation obs;

   /* No depth write: the comparison-NEVER control and the
    * depth-writes-disabled control both land here. */
   memcpy(after, before, bytes);
   r300_zb_depth_discovery_observe(scenario, &layout, before, after, bytes,
                                   &obs);
   assert(obs.judged && obs.depth_locations == 0u && obs.change_count == 0u);

   /* Two depth writes: a single-pixel run that reached two slots. */
   memcpy(after, before, bytes);
   poke_word(after, first, 0x4000005au, 4u);
   poke_word(after, second, 0x4000005au, 4u);
   r300_zb_depth_discovery_observe(scenario, &layout, before, after, bytes,
                                   &obs);
   assert(obs.judged && obs.depth_locations == 2u);
   assert(obs.slots_depth_only == 2u && obs.change_count == 2u);

   /* Stencil-only: the low byte moved and the depth field did not.  It
    * is counted and retained, and it contributes no depth location. */
   memcpy(after, before, bytes);
   poke_word(after, first, (initial & 0xffffff00u) | 0xa5u, 4u);
   r300_zb_depth_discovery_observe(scenario, &layout, before, after, bytes,
                                   &obs);
   assert(obs.judged);
   assert(obs.depth_locations == 0u);
   assert(obs.slots_stencil_only == 1u);
   assert(obs.slots_depth_only == 0u && obs.slots_both == 0u);
   assert(obs.change_count == 1u);
   assert(obs.changes[0].stencil_changed && !obs.changes[0].depth_changed);
   assert(obs.changes[0].before_stencil == 0x5au);
   assert(obs.changes[0].after_stencil == 0xa5u);

   /* Both components: a depth write that replaced the stencil byte, the
    * observation the seed pair exists to separate from preservation. */
   memcpy(after, before, bytes);
   poke_word(after, first, 0x40000000u, 4u);
   r300_zb_depth_discovery_observe(scenario, &layout, before, after, bytes,
                                   &obs);
   assert(obs.judged);
   assert(obs.depth_locations == 1u);
   assert(obs.slots_both == 1u && obs.slots_depth_only == 0u);
   assert(obs.changes[0].depth_changed && obs.changes[0].stencil_changed);

   /* Guard corruption at the first and last byte of each guard: the
    * verdict must see it, and it never enters the slot denominator. */
   const uint64_t guard_bytes[] = {
      0u,
      R300_ZB_DISCOVERY_GUARD_BYTES - 1u,
      layout.suffix_guard_offset_bytes,
      layout.suffix_guard_offset_bytes + layout.suffix_guard_bytes - 1u,
   };
   for (size_t i = 0; i < sizeof(guard_bytes) / sizeof(*guard_bytes); i++) {
      memcpy(after, before, bytes);
      after[guard_bytes[i]] ^= 0xffu;
      r300_zb_depth_discovery_observe(scenario, &layout, before, after, bytes,
                                      &obs);
      assert(obs.judged);
      assert(obs.guard_bytes_changed == 1u);
      assert(obs.unclaimed_bytes_changed == 0u);
      assert(obs.depth_locations == 0u);
      assert(obs.slots_inspected == layout.storage_bytes / 4u);
   }

   /* A change in the slack past the linear suffix guard.  It is neither
    * a guard verdict nor a discovered depth address, and an observation
    * that folded it into either would be reporting the wrong fact. */
   memcpy(after, before, bytes);
   after[layout.total_bytes] ^= 0xffu;
   r300_zb_depth_discovery_observe(scenario, &layout, before, after, bytes,
                                   &obs);
   assert(obs.judged);
   assert(obs.unclaimed_bytes_changed == 1u);
   assert(obs.guard_bytes_changed == 0u);
   assert(obs.depth_locations == 0u);

   /* A truncated mapping: the size disagrees with the declared
    * allocation, so nothing is judged. */
   memcpy(after, before, bytes);
   r300_zb_depth_discovery_observe(scenario, &layout, before, after,
                                   bytes - 4u, &obs);
   assert(!obs.judged);
   assert(obs.slots_inspected == 0u && obs.depth_locations == 0u);

   /* A null image, and a layout describing another allocation. */
   r300_zb_depth_discovery_observe(scenario, &layout, NULL, after, bytes,
                                   &obs);
   assert(!obs.judged);
   struct r300_zb_depth_layout wrong = layout;
   wrong.base_offset_bytes = 0u;
   r300_zb_depth_discovery_observe(scenario, &wrong, before, after, bytes,
                                   &obs);
   assert(!obs.judged);

   /* More changed slots than the retention capacity: the counts stay
    * exact and the overflow is declared. */
   memcpy(after, before, bytes);
   for (uint32_t i = 0; i < R300_ZB_DISCOVERY_MAX_CHANGES + 3u; i++)
      poke_word(after, layout.base_offset_bytes + 4u * i, 0x4000005au, 4u);
   r300_zb_depth_discovery_observe(scenario, &layout, before, after, bytes,
                                   &obs);
   assert(obs.judged);
   assert(obs.depth_locations == R300_ZB_DISCOVERY_MAX_CHANGES + 3u);
   assert(obs.change_count == R300_ZB_DISCOVERY_MAX_CHANGES);
   assert(obs.change_overflow);

   free(before);
   free(after);
}

/* A misclassifying guard predicate must be visible to the fixture, so
 * the guard expectation is stated from the literal intervals rather than
 * from the predicate the observation calls. */
static void
test_guard_expectation_is_independent(void)
{
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_discovery_layout(&r300_zb_depth_discovery_z24_linear,
                                         &layout) == 0);
   for (uint64_t off = 0; off < 24576u; off++) {
      const bool literal_guard = off < 2048u || (off >= 18688u && off < 20736u);
      assert(r300_zb_depth_layout_is_guard_byte(&layout, off) ==
             literal_guard);
   }
}

static void
test_color_oracle(void)
{
   const uint32_t pitch = 64u, width = 64u, height = 64u;
   const uint32_t sentinel = 0xff00ff00u, draw = 0xffffffffu;
   const uint64_t bytes = (uint64_t)pitch * (height + 1u) * 4u;
   uint32_t *pixels = malloc(bytes);
   assert(pixels != NULL);

   struct r300_zb_discovery_color_verdict v;

   /* Exactly the declared pixel colored. */
   for (uint64_t i = 0; i < bytes / 4u; i++)
      pixels[i] = sentinel;
   pixels[21u * pitch + 37u] = draw;
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         37u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(v.judged && v.exact);
   assert(v.inside_samples == 1u && v.inside_colored == 1u);
   assert(v.outside_samples == width * height - 1u && v.outside_colored == 0u);
   /* The allocation carries one row past the extent and no sub-pitch
    * padding, since the pitch equals the width. */
   assert(v.beyond_samples == pitch && v.beyond_changed == 0u);

   /* A write into the row past the render extent: every in-extent
    * verdict still holds, and the run is not exact. */
   for (uint64_t i = 0; i < bytes / 4u; i++)
      pixels[i] = sentinel;
   pixels[21u * pitch + 37u] = draw;
   pixels[height * pitch] = draw;
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         37u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(v.judged && !v.exact);
   assert(v.inside_colored == 1u && v.outside_colored == 0u);
   assert(v.beyond_changed == 1u);

   /* The device colored a different pixel.  The expectation comes from
    * the declaration, so this fails rather than relocating itself. */
   for (uint64_t i = 0; i < bytes / 4u; i++)
      pixels[i] = sentinel;
   pixels[21u * pitch + 38u] = draw;
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         37u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(v.judged && !v.exact);
   assert(v.inside_colored == 0u && v.outside_colored == 1u);

   /* Nothing colored at all. */
   for (uint64_t i = 0; i < bytes / 4u; i++)
      pixels[i] = sentinel;
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         37u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(v.judged && !v.exact && v.inside_colored == 0u);

   /* The whole target colored: the scissor did not confine the covering
    * primitive. */
   for (uint64_t i = 0; i < bytes / 4u; i++)
      pixels[i] = draw;
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         37u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(v.judged && !v.exact);
   assert(v.inside_colored == 1u);
   assert(v.outside_colored == width * height - 1u);

   /* Declaration errors and short buffers yield no verdict. */
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         64u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(!v.judged);
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         63u, 21u, 2u, 1u, sentinel, draw, &v);
   assert(!v.judged);
   r300_zb_depth_discovery_color_observe(pixels, bytes, pitch, width, height,
                                         37u, 21u, 1u, 1u, draw, draw, &v);
   assert(!v.judged);
   r300_zb_depth_discovery_color_observe(pixels, 1024u, pitch, width, height,
                                         37u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(!v.judged);
   r300_zb_depth_discovery_color_observe(NULL, bytes, pitch, width, height,
                                         37u, 21u, 1u, 1u, sentinel, draw, &v);
   assert(!v.judged);

   free(pixels);
}

int
main(void)
{
   test_expected_layouts();
   test_seed_words();
   test_scenario_refusals();
   test_layout_validate_refusals();
   test_guard_expectation_is_independent();
   test_region_denominators();
   test_write_anywhere_in_envelope();
   test_observation_classes();
   test_color_oracle();
   printf("r300_zb_depth_discovery_test: ok\n");
   return 0;
}
