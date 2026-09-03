/*
 * SPDX-License-Identifier: MIT
 *
 * Determinism, relocation-site, capacity, and oracle-calibration controls
 * for the 2D solid-fill direct-write control cell.
 */

/* The controls below assert the cell's decisions, so a release build with
 * assertions compiled out would run them and report nothing.
 */
#undef NDEBUG

#include "r300_direct_write.h"
#include "r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_PIXELS \
   (R300_TRIANGLE_TARGET_PITCH_PIXELS * R300_TRIANGLE_TARGET_HEIGHT)
#define ALLOC_PIXELS \
   (R300_TRIANGLE_TARGET_PITCH_PIXELS * R300_TRIANGLE_ALLOCATION_ROWS)
#define PIXEL_A \
   (R300_DIRECT_WRITE_A_Y * R300_TRIANGLE_TARGET_PITCH_PIXELS + \
    R300_DIRECT_WRITE_A_X)
#define PIXEL_B \
   (R300_DIRECT_WRITE_B_Y * R300_TRIANGLE_TARGET_PITCH_PIXELS + \
    R300_DIRECT_WRITE_B_X)

/* Two emissions produce one byte sequence, the property every digest
 * authority rests on.
 */
static void
test_deterministic_emission(void)
{
   struct r300_direct_write_ib one, two;

   assert(r300_direct_write_emit(&one) == 0);
   assert(r300_direct_write_emit(&two) == 0);
   assert(one.ib_size_dwords == two.ib_size_dwords);
   assert(one.ib_size_dwords > 0);
   assert(memcmp(one.ib, two.ib, one.ib_size_dwords * 4) == 0);
   assert(r300_direct_write_validate_reloc_sites(&one) == 0);
   r300_direct_write_release(&one);
   r300_direct_write_release(&two);
}

/* The cell references its one BO exactly once, and a corrupted site or
 * payload is refused by the validator.
 */
static void
test_reloc_sites(void)
{
   struct r300_direct_write_ib ib;

   assert(r300_direct_write_emit(&ib) == 0);
   assert(ib.reloc_site_count == 1);
   assert(ib.reloc_sites[0].slot == R300_DIRECT_WRITE_SLOT_COLOR);

   const uint32_t site = ib.reloc_sites[0].ib_index;
   const uint32_t saved = ib.ib[site];

   ib.ib[site] = saved + 4;
   assert(r300_direct_write_validate_reloc_sites(&ib) == -EINVAL);
   ib.ib[site] = saved;
   assert(r300_direct_write_validate_reloc_sites(&ib) == 0);

   ib.reloc_sites[0].ib_index = ib.ib_size_dwords;
   assert(r300_direct_write_validate_reloc_sites(&ib) == -EINVAL);
   ib.reloc_sites[0].ib_index = site;

   r300_direct_write_release(&ib);
}

/* A destination one dword short of the cell refuses with -ENOSPC and
 * reports no dword count; the exact-size destination reproduces the owned
 * emission byte for byte.
 */
static void
test_capacity(void)
{
   struct r300_direct_write_ib owned, into;
   uint32_t words[64];

   assert(r300_direct_write_emit(&owned) == 0);
   assert(owned.ib_size_dwords <= 64);

   assert(r300_direct_write_emit_into(words, owned.ib_size_dwords - 1,
                                      &into) == -ENOSPC);
   assert(into.ib_size_dwords == 0);

   assert(r300_direct_write_emit_into(words, owned.ib_size_dwords,
                                      &into) == 0);
   assert(into.ib_size_dwords == owned.ib_size_dwords);
   assert(memcmp(words, owned.ib, owned.ib_size_dwords * 4) == 0);

   r300_direct_write_release(&owned);
}

static uint32_t *
sentinel_target(void)
{
   uint32_t *pixels = malloc(ALLOC_PIXELS * 4);

   assert(pixels);
   for (uint32_t i = 0; i < ALLOC_PIXELS; i++)
      pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   return pixels;
}

/* Oracle calibration: the known-good landing, the untouched target, each
 * single-value absence, a byte-lane permutation, a sentinel disturbance,
 * and a canary overrun each produce their own verdict shape.
 */
static void
test_oracle(void)
{
   struct r300_direct_write_verdict v;
   uint32_t *pixels = sentinel_target();

   /* Untouched target: nothing executed, both values absent. */
   r300_direct_write_oracle(pixels, ALLOC_PIXELS * 4, &v);
   assert(!v.executed && !v.value_a_pass && !v.value_b_pass);
   assert(v.sentinel_pass && v.canary_pass);

   /* The known-good landing. */
   pixels[PIXEL_A] = R300_DIRECT_WRITE_A_VALUE;
   pixels[PIXEL_B] = R300_DIRECT_WRITE_B_VALUE;
   r300_direct_write_oracle(pixels, ALLOC_PIXELS * 4, &v);
   assert(v.executed && v.value_a_pass && v.value_b_pass);
   assert(v.sentinel_pass && v.canary_pass);

   /* A byte-lane permutation of value A is executed and not a pass. */
   pixels[PIXEL_A] = 0x44332211u;
   r300_direct_write_oracle(pixels, ALLOC_PIXELS * 4, &v);
   assert(v.executed && !v.value_a_pass && v.value_b_pass);
   assert(v.sentinel_pass);

   /* A write outside both probe pixels disturbs the sentinel verdict. */
   pixels[PIXEL_A] = R300_DIRECT_WRITE_A_VALUE;
   pixels[0] = 0;
   r300_direct_write_oracle(pixels, ALLOC_PIXELS * 4, &v);
   assert(v.executed && !v.sentinel_pass && v.canary_pass);
   pixels[0] = R300_TRIANGLE_COLOR_SENTINEL;

   /* A write past the render extent fails the canary alone. */
   pixels[TARGET_PIXELS] = 0;
   r300_direct_write_oracle(pixels, ALLOC_PIXELS * 4, &v);
   assert(v.executed && v.sentinel_pass && !v.canary_pass);
   pixels[TARGET_PIXELS] = R300_TRIANGLE_COLOR_SENTINEL;

   /* A target smaller than the allocation contract reports nothing. */
   r300_direct_write_oracle(pixels, (ALLOC_PIXELS - 1) * 4, &v);
   assert(!v.executed && !v.value_a_pass && !v.sentinel_pass);

   free(pixels);
}

/* The stream this cell emits, pinned.  The cell is the RB2D unit's only
 * retained witness, and the arming gate compares this digest, so the bytes
 * are the artifact rather than a consequence of how they are produced: the
 * emitter may be refactored underneath, and this value may not move without
 * a new witness.  32 dwords, 128 bytes.
 */
#define R300_DIRECT_WRITE_GOLDEN_IB_BLAKE3 \
   "03e186d5b4ca74058ed5a05559c7c9b146aea585a2cca95683386177af02477a"
#define R300_DIRECT_WRITE_GOLDEN_IB_DWORDS 32u

static void
test_digest_stability(void)
{
   struct r300_direct_write_ib ib;
   char hex_one[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   char hex_two[2 * R300_TRIANGLE_DIGEST_SIZE + 1];

   assert(r300_direct_write_emit(&ib) == 0);
   r300_triangle_ib_digest_hex(ib.ib, ib.ib_size_dwords, hex_one);
   r300_triangle_ib_digest_hex(ib.ib, ib.ib_size_dwords, hex_two);
   assert(strcmp(hex_one, hex_two) == 0);
   assert(ib.ib_size_dwords == R300_DIRECT_WRITE_GOLDEN_IB_DWORDS);
   assert(strcmp(hex_one, R300_DIRECT_WRITE_GOLDEN_IB_BLAKE3) == 0);
   r300_direct_write_release(&ib);
}

int
main(void)
{
   test_deterministic_emission();
   test_reloc_sites();
   test_capacity();
   test_oracle();
   test_digest_stability();
   printf("r300_direct_write_test: all controls hold\n");
   return 0;
}
