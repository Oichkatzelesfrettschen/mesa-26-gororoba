/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the post-vertex lowering over the triangle-list
 * record: the smooth default leaves records untouched, one Flat
 * location replicates the provoking vertex's value over three distinct
 * vertex values, the per-location mask selects one varying inside a
 * two-varying record, an indexed permutation follows the dereferenced
 * list order, a dropped qualifier and the wrong provoking vertex each
 * move the oracle, and every malformed input refuses ahead of a write.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_post_vs_lowering.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* One triangle of eight-dword records: position, then the varying.
 * The varyings are three distinct patterns so a replicated record
 * names its source vertex. */
static const uint32_t triangle[3][8] = {
   { 0x00000001u, 0x00000002u, 0x00000003u, 0x3f800000u,
     0x11111111u, 0x11111112u, 0x11111113u, 0x11111114u },
   { 0x00000011u, 0x00000012u, 0x00000013u, 0x3f800000u,
     0x22222221u, 0x22222222u, 0x22222223u, 0x22222224u },
   { 0x00000021u, 0x00000022u, 0x00000023u, 0x3f800000u,
     0x33333331u, 0x33333332u, 0x33333333u, 0x33333334u },
};

static void
expect_triangle(const uint32_t *got, const uint32_t position_source[3],
                const uint32_t varying_source[3])
{
   for (uint32_t v = 0; v < 3; v++) {
      assert(memcmp(&got[v * 8], triangle[position_source[v]], 16) == 0);
      assert(memcmp(&got[v * 8 + 4], &triangle[varying_source[v]][4], 16) ==
             0);
   }
}

static void test_smooth_default(void)
{
   const struct r3v_post_vs_lowering smooth = { 0 };
   uint32_t records[3][8];
   memcpy(records, triangle, sizeof(records));
   assert(r3v_post_vs_lower_triangles(&smooth, &records[0][0], 1, 8) == 0);
   assert(memcmp(records, triangle, sizeof(records)) == 0);
   /* A zero triangle count is a no-op under any lowering. */
   const struct r3v_post_vs_lowering flat = { .flat_mask = 1 };
   assert(r3v_post_vs_lower_triangles(&flat, NULL, 0, 8) == 0);
}

static void test_one_flat_location(void)
{
   const struct r3v_post_vs_lowering flat = {
      .flat_mask = 1, .provoking_vertex = R3V_POST_VS_PROVOKING_VERTEX_FIRST,
   };
   uint32_t records[3][8];
   memcpy(records, triangle, sizeof(records));
   assert(r3v_post_vs_lower_triangles(&flat, &records[0][0], 1, 8) == 0);
   expect_triangle(&records[0][0], (const uint32_t[3]){ 0, 1, 2 },
                   (const uint32_t[3]){ 0, 0, 0 });

   /* Two triangles replicate independently: the second's provoking
    * vertex is its own first record. */
   uint32_t two[6][8];
   memcpy(two[0], triangle, sizeof(triangle));
   memcpy(two[3], triangle[2], 32);
   memcpy(two[4], triangle[0], 32);
   memcpy(two[5], triangle[1], 32);
   assert(r3v_post_vs_lower_triangles(&flat, &two[0][0], 2, 8) == 0);
   expect_triangle(&two[0][0], (const uint32_t[3]){ 0, 1, 2 },
                   (const uint32_t[3]){ 0, 0, 0 });
   expect_triangle(&two[3][0], (const uint32_t[3]){ 2, 0, 1 },
                   (const uint32_t[3]){ 2, 2, 2 });
}

static void test_mixed_flat_and_smooth(void)
{
   /* A twelve-dword record: position, varying 0, varying 1. */
   uint32_t records[3][12];
   for (uint32_t v = 0; v < 3; v++) {
      memcpy(records[v], triangle[v], 32);
      for (uint32_t c = 0; c < 4; c++)
         records[v][8 + c] = triangle[v][4 + c] ^ 0xff000000u;
   }
   uint32_t oracle[3][12];
   memcpy(oracle, records, sizeof(oracle));

   const struct r3v_post_vs_lowering flat0 = { .flat_mask = 1 };
   assert(r3v_post_vs_lower_triangles(&flat0, &records[0][0], 1, 12) == 0);
   for (uint32_t v = 0; v < 3; v++) {
      assert(memcmp(records[v], oracle[v], 16) == 0);
      assert(memcmp(&records[v][4], &oracle[0][4], 16) == 0);
      assert(memcmp(&records[v][8], &oracle[v][8], 16) == 0);
   }

   memcpy(records, oracle, sizeof(records));
   const struct r3v_post_vs_lowering flat1 = { .flat_mask = 2 };
   assert(r3v_post_vs_lower_triangles(&flat1, &records[0][0], 1, 12) == 0);
   for (uint32_t v = 0; v < 3; v++) {
      assert(memcmp(&records[v][4], &oracle[v][4], 16) == 0);
      assert(memcmp(&records[v][8], &oracle[0][8], 16) == 0);
   }

   memcpy(records, oracle, sizeof(records));
   const struct r3v_post_vs_lowering both = { .flat_mask = 3 };
   assert(r3v_post_vs_lower_triangles(&both, &records[0][0], 1, 12) == 0);
   for (uint32_t v = 0; v < 3; v++)
      assert(memcmp(&records[v][4], &oracle[0][4], 32) == 0);
}

static void test_indexed_permutation(void)
{
   /* The list an indexed draw (2, 0, 1) dereferences: the provoking
    * vertex is the list's first record, vertex 2. */
   const struct r3v_post_vs_lowering flat = { .flat_mask = 1 };
   uint32_t records[3][8];
   memcpy(records[0], triangle[2], 32);
   memcpy(records[1], triangle[0], 32);
   memcpy(records[2], triangle[1], 32);
   assert(r3v_post_vs_lower_triangles(&flat, &records[0][0], 1, 8) == 0);
   expect_triangle(&records[0][0], (const uint32_t[3]){ 2, 0, 1 },
                   (const uint32_t[3]){ 2, 2, 2 });
}

static void test_metadata_drop_and_wrong_provoking(void)
{
   /* The interface with Flat linked derives the lowering; the same
    * interface with the qualifier dropped derives the identity, and the
    * two disagree on every non-provoking record. */
   struct r3v_shader_interface_link link = { 0 };
   link.varying_mask = 1;
   link.flat_mask = 1;
   struct r3v_post_vs_lowering flat, dropped;
   r3v_post_vs_lowering_from_interface(&link, &flat);
   assert(flat.flat_mask == 1 &&
          flat.provoking_vertex == R3V_POST_VS_PROVOKING_VERTEX_FIRST);
   link.flat_mask = 0;
   r3v_post_vs_lowering_from_interface(&link, &dropped);
   assert(dropped.flat_mask == 0);

   uint32_t oracle[3][8], records[3][8];
   memcpy(oracle, triangle, sizeof(oracle));
   assert(r3v_post_vs_lower_triangles(&flat, &oracle[0][0], 1, 8) == 0);
   memcpy(records, triangle, sizeof(records));
   assert(r3v_post_vs_lower_triangles(&dropped, &records[0][0], 1, 8) == 0);
   assert(memcmp(records[0], oracle[0], 32) == 0);
   assert(memcmp(&records[1][4], &oracle[1][4], 16) != 0);
   assert(memcmp(&records[2][4], &oracle[2][4], 16) != 0);

   /* The last-vertex provoking convention (R300's GA_COLOR_CONTROL
    * default) replicates vertex 2, so every record disagrees with the
    * Vulkan first-vertex oracle, which carries vertex 0 throughout. */
   const struct r3v_post_vs_lowering last = {
      .flat_mask = 1, .provoking_vertex = 2,
   };
   memcpy(records, triangle, sizeof(records));
   assert(r3v_post_vs_lower_triangles(&last, &records[0][0], 1, 8) == 0);
   expect_triangle(&records[0][0], (const uint32_t[3]){ 0, 1, 2 },
                   (const uint32_t[3]){ 2, 2, 2 });
   for (uint32_t v = 0; v < 3; v++)
      assert(memcmp(&records[v][4], &oracle[v][4], 16) != 0);
}

static void test_refusals(void)
{
   uint32_t records[3][8];
   memcpy(records, triangle, sizeof(records));
   const struct r3v_post_vs_lowering flat = { .flat_mask = 1 };
   assert(r3v_post_vs_lower_triangles(&flat, &records[0][0], 1, 5) ==
          -EINVAL);
   assert(r3v_post_vs_lower_triangles(&flat, &records[0][0], 1, 0) ==
          -EINVAL);
   /* A Flat location past the record's one varying. */
   const struct r3v_post_vs_lowering beyond = { .flat_mask = 2 };
   assert(r3v_post_vs_lower_triangles(&beyond, &records[0][0], 1, 8) ==
          -EINVAL);
   /* A position-only record carries no varying to replicate. */
   assert(r3v_post_vs_lower_triangles(&flat, &records[0][0], 1, 4) ==
          -EINVAL);
   const struct r3v_post_vs_lowering outside = {
      .flat_mask = 1, .provoking_vertex = 3,
   };
   assert(r3v_post_vs_lower_triangles(&outside, &records[0][0], 1, 8) ==
          -EINVAL);
   assert(r3v_post_vs_lower_triangles(&flat, NULL, 1, 8) == -EINVAL);
   assert(r3v_post_vs_lower_triangles(NULL, &records[0][0], 1, 8) ==
          -EINVAL);
   /* Every refusal left the records untouched. */
   assert(memcmp(records, triangle, sizeof(records)) == 0);
}

int main(void)
{
   test_smooth_default();
   test_one_flat_location();
   test_mixed_flat_and_smooth();
   test_indexed_permutation();
   test_metadata_drop_and_wrong_provoking();
   test_refusals();
   printf("r3v_post_vs_lowering: smooth default, one flat location, mixed "
          "mask, indexed permutation, dropped qualifier, wrong provoking "
          "vertex, and refusals calibrated\n");
   return 0;
}
