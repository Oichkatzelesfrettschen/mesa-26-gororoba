/*
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "amd/r300/common/r300_grid_fold.h"

static unsigned failures;

#define CHECK(condition, message)                                              \
   do {                                                                        \
      if (!(condition)) {                                                      \
         fprintf(stderr, "FAIL: %s\n", message);                              \
         failures++;                                                           \
      }                                                                        \
   } while (0)

static const struct r300_grid_fold untouched = {
   .width = 0x11111111u,
   .height = 0x22222222u,
   .dim_x = 0x33333333u,
   .dim_y = 0x44444444u,
   .dim_z = 0x55555555u,
   .z_row_folded = true,
};

static bool
fold_equal(const struct r300_grid_fold *a, const struct r300_grid_fold *b)
{
   /* Compare the contract fields, not object padding: an optimized compiler
    * may implement a struct assignment as member stores and leave padding
    * bytes unspecified even though every published field is unchanged. */
   return a->width == b->width && a->height == b->height &&
          a->dim_x == b->dim_x && a->dim_y == b->dim_y &&
          a->dim_z == b->dim_z && a->z_row_folded == b->z_row_folded;
}

static void
check_refusal_preserves_output(bool accepted,
                               const struct r300_grid_fold *fold,
                               const char *message)
{
   CHECK(!accepted, message);
   CHECK(fold_equal(fold, &untouched),
         "a refused fold must not publish a partial geometry");
}

static void
test_fp24_index_bounds(void)
{
   CHECK(R300_FP24_EXACT_INT_CEILING == 131072u,
         "FP24 exact-integer ceiling is 2^17");
   CHECK(R300_RASTER_AXIS_CAP == 2048u,
         "sampleable raster axis cap is 2048");

   CHECK(!r300_grid_linear_index_exact(0),
         "an empty linear grid refuses");
   CHECK(r300_grid_linear_index_exact(
            (uint64_t)R300_FP24_EXACT_INT_CEILING + 1),
         "2^17 + 1 invocations admit because the final gid is 2^17");
   CHECK(!r300_grid_linear_index_exact(
            (uint64_t)R300_FP24_EXACT_INT_CEILING + 2),
         "the first grid whose final gid exceeds 2^17 refuses");

   CHECK(r300_grid_strided_index_exact(1, UINT32_MAX, 0),
         "a one-element affine grid never materializes its stride");
   CHECK(r300_grid_strided_index_exact(2,
                                       R300_FP24_EXACT_INT_CEILING, 0),
         "an affine maximum exactly at 2^17 admits");
   CHECK(!r300_grid_strided_index_exact(
            2, R300_FP24_EXACT_INT_CEILING + 1, 0),
         "an affine maximum one past 2^17 refuses");
   CHECK(r300_grid_strided_index_exact(UINT64_MAX, 0,
                                       R300_FP24_EXACT_INT_CEILING),
         "zero stride depends only on its constant offset");
   CHECK(!r300_grid_strided_index_exact(
            1, 0, R300_FP24_EXACT_INT_CEILING + 1),
         "a zero-stride constant one past 2^17 refuses");
   CHECK(!r300_grid_strided_index_exact((UINT64_C(1) << 63) + 1, 2, 0),
         "an affine product that wraps to zero in uint64 refuses");

   const uint64_t raster_capacity =
      (uint64_t)R300_RASTER_AXIS_CAP * R300_RASTER_AXIS_CAP;
   CHECK(r300_grid_index_exact(R300_GRID_INDEX_NONE,
                               raster_capacity, 0, 0),
         "position-only work fills the complete raster");
   CHECK(!r300_grid_index_exact(R300_GRID_INDEX_COORD,
                                raster_capacity + 1, 0, 0),
         "position-only work one past the raster refuses");
   CHECK(!r300_grid_index_exact((enum r300_grid_index_class)99,
                                1, 0, 0),
         "an unknown index class refuses");
   CHECK(r300_grid_index_exact(
            R300_GRID_INDEX_LINEAR,
            (uint64_t)R300_FP24_EXACT_INT_CEILING + 1, 0, 0),
         "the class-dispatched linear guard preserves the inclusive index");
}

static void
test_1d_fold(void)
{
   struct r300_grid_fold fold = untouched;
   check_refusal_preserves_output(r300_grid_fold_1d(0, &fold), &fold,
                                  "an empty 1D grid refuses");

   CHECK(r300_grid_fold_1d(1, &fold), "one invocation folds");
   CHECK(fold.width == 1 && fold.height == 1 &&
         fold.dim_x == 1 && fold.dim_y == 1 && fold.dim_z == 1 &&
         !fold.z_row_folded,
         "one invocation produces a 1x1 non-z-folded raster");

   CHECK(r300_grid_fold_1d(R300_RASTER_AXIS_CAP + 1, &fold),
         "one invocation past the first row folds");
   CHECK(fold.width == R300_RASTER_AXIS_CAP && fold.height == 2,
         "the first overflow row is 2048x2");

   const uint64_t raster_capacity =
      (uint64_t)R300_RASTER_AXIS_CAP * R300_RASTER_AXIS_CAP;
   CHECK(r300_grid_fold_1d(raster_capacity, &fold),
         "the complete 2048x2048 raster admits");
   CHECK(fold.width == R300_RASTER_AXIS_CAP &&
         fold.height == R300_RASTER_AXIS_CAP,
         "the maximum 1D fold fills both raster axes");

   fold = untouched;
   check_refusal_preserves_output(
      r300_grid_fold_1d(raster_capacity + 1, &fold), &fold,
      "one invocation past the 1D raster capacity refuses");
}

static void
test_2d_and_3d_fold(void)
{
   struct r300_grid_fold fold = untouched;

   CHECK(r300_grid_fold_2d(R300_RASTER_AXIS_CAP,
                           R300_RASTER_AXIS_CAP, &fold),
         "the maximum 2D grid admits");
   CHECK(fold.width == R300_RASTER_AXIS_CAP &&
         fold.height == R300_RASTER_AXIS_CAP && fold.dim_z == 1 &&
         !fold.z_row_folded,
         "the 2D fold preserves both axes");

   fold = untouched;
   check_refusal_preserves_output(
      r300_grid_fold_2d(R300_RASTER_AXIS_CAP + 1, 1, &fold), &fold,
      "a 2D x axis one past the cap refuses");
   fold = untouched;
   check_refusal_preserves_output(r300_grid_fold_2d(1, 0, &fold), &fold,
                                  "an empty 2D axis refuses");

   CHECK(r300_grid_fold_3d(R300_RASTER_AXIS_CAP, 1024, 2, &fold),
         "a 3D grid whose folded rows equal 2048 admits");
   CHECK(fold.width == R300_RASTER_AXIS_CAP &&
         fold.height == R300_RASTER_AXIS_CAP &&
         fold.dim_x == R300_RASTER_AXIS_CAP &&
         fold.dim_y == 1024 && fold.dim_z == 2 && fold.z_row_folded,
         "the 3D fold records its logical dimensions and z carrier");

   fold = untouched;
   check_refusal_preserves_output(
      r300_grid_fold_3d(1, 1025, 2, &fold), &fold,
      "a 3D folded row count one past 2048 refuses");
   fold = untouched;
   check_refusal_preserves_output(
      r300_grid_fold_3d(1, UINT32_MAX, UINT32_MAX, &fold), &fold,
      "a large 3D row product refuses without uint32 wraparound");
   fold = untouched;
   check_refusal_preserves_output(r300_grid_fold_3d(1, 1, 0, &fold), &fold,
                                  "an empty 3D axis refuses");
}

int
main(void)
{
   test_fp24_index_bounds();
   test_1d_fold();
   test_2d_and_3d_fold();

   if (failures) {
      fprintf(stderr, "%u grid-fold checks failed\n", failures);
      return 1;
   }

   puts("r300 grid-fold contract: PASS");
   return 0;
}
