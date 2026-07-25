/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_GRID_FOLD_H
#define R300_GRID_FOLD_H

#include <stdbool.h>
#include <stdint.h>

/* Grid-fold model for compute-as-raster dispatch replay.
 *
 * The R300 fragment ALU is FP24 (1 sign, 7 exponent, 16 mantissa bits, a
 * 17-bit significand with the implicit leading one).  Every integer in
 * [0, 2^17] is exactly representable; 2^17 + 1 is the first integer that is
 * not.  Whether a folded compute grid is admissible therefore depends on how
 * the kernel consumes the work-item index, not on the raw invocation count:
 *
 *  - A position-addressed kernel (index class NONE or COORD) never
 *    materializes the linear global index as one FP24 number.  Each raster
 *    axis coordinate stays at or below the R300 2D texture axis cap of 2048
 *    (2^11), far inside the exact window, so the full 2048x2048 = 2^22
 *    invocation raster is honest.
 *
 *  - A linear-indexed kernel (index class LINEAR) materializes
 *    gid = y * width + x as a single FP24 value.  Index identity holds only
 *    while every gid in [0, total - 1] is exactly representable, so total
 *    must stay at or below 2^17.
 *
 *  - A strided kernel (index class STRIDED) materializes a * gid + b.  The
 *    multiply of two exact integers and the following add are exact in FP24
 *    if and only if the result itself sits inside the exact-integer window,
 *    so a * (total - 1) + b must stay at or below 2^17.
 *
 * 3D grids fold the z coordinate into raster rows: y_raster = z * dim_y + y,
 * recovered in the fragment program as z = floor(y_raster / dim_y).  The
 * folded row index never exceeds the 2048 axis cap, so the recovery
 * arithmetic is always exact; the binding constraint is dim_y * dim_z <= 2048.
 */

/* Largest integer N such that every integer in [0, N] is exact in FP24. */
#define R300_FP24_EXACT_INT_CEILING ((uint32_t)1 << 17)

/* Common safe raster axis for a surface that is both rendered and sampled:
 * the R300 2D texture axis cap (2048), which is the smaller of the sampler
 * limit and the 2560 color-render limit.  The fold uses it so every grid
 * surface stays sampleable as well as renderable. */
#define R300_RASTER_AXIS_CAP 2048u

/* How the admitted kernel consumes the synthetic work-item index.  The
 * admission detectors classify this from the NIR consumption shape of
 * load_global_invocation_id / the lowered index varying. */
enum r300_grid_index_class {
   /* Index never read: output addressed implicitly by texel position
    * (CONSTFILL, pure map kernels). */
   R300_GRID_INDEX_NONE = 0,
   /* Per-axis coordinates consumed, linear index never formed.  Each
    * coordinate is bounded by the raster axis cap. */
   R300_GRID_INDEX_COORD,
   /* Linear global index consumed as one number. */
   R300_GRID_INDEX_LINEAR,
   /* Affine function of the linear index consumed: a * gid + b. */
   R300_GRID_INDEX_STRIDED,
};

/* True when every linear index in [0, total_invocations - 1] is exactly
 * representable in FP24.  The boundary total == 2^17 is admissible: the
 * largest materialized index is then 2^17 - 1. */
static inline bool
r300_grid_linear_index_exact(uint64_t total_invocations)
{
   return total_invocations > 0 &&
          total_invocations <= R300_FP24_EXACT_INT_CEILING;
}

/* True when a * gid + b stays inside the FP24 exact-integer window for
 * every gid in [0, total_invocations - 1].  Integer products and sums in
 * FP24 are exact exactly while the result remains representable, so checking
 * the maximum value suffices.  A zero stride is a literal constant-value
 * affine, not an absent stride; only b needs the FP24 value bound. */
static inline bool
r300_grid_strided_index_exact(uint64_t total_invocations,
                                uint32_t stride, uint32_t offset)
{
   if (total_invocations == 0)
      return false;
   if (stride == 0)
      return offset <= R300_FP24_EXACT_INT_CEILING;
   const uint64_t max_value =
      (uint64_t)stride * (total_invocations - 1) + offset;
   return max_value <= R300_FP24_EXACT_INT_CEILING;
}

/* Admission guard dispatched on index class.  Position-addressed classes are
 * bounded by the raster extent alone (the caller separately enforces the
 * 2048x2048 fold); index-materializing classes are bounded by the FP24
 * exact-integer ceiling. */
static inline bool
r300_grid_index_exact(enum r300_grid_index_class index_class,
                        uint64_t total_invocations,
                        uint32_t stride, uint32_t offset)
{
   switch (index_class) {
   case R300_GRID_INDEX_NONE:
   case R300_GRID_INDEX_COORD:
      return total_invocations > 0 &&
             total_invocations <=
                (uint64_t)R300_RASTER_AXIS_CAP * R300_RASTER_AXIS_CAP;
   case R300_GRID_INDEX_LINEAR:
      return r300_grid_linear_index_exact(total_invocations);
   case R300_GRID_INDEX_STRIDED:
      return r300_grid_strided_index_exact(total_invocations, stride,
                                             offset);
   }
   return false;
}

/* Resolved fold of a compute grid onto one 2D raster target. */
struct r300_grid_fold {
   unsigned width;        /* raster width in texels */
   unsigned height;       /* raster height in texels */
   unsigned dim_x;        /* logical grid extent on x */
   unsigned dim_y;        /* logical grid extent on y */
   unsigned dim_z;        /* logical grid extent on z (1 for 1D/2D) */
   bool     z_row_folded; /* z carried as y_raster = z * dim_y + y */
};

/* Fold a 1D total onto the raster: one row up to the axis cap, then add
 * rows.  Identical shape to the identity-map fold so existing kernels keep
 * their texel layout. */
static inline bool
r300_grid_fold_1d(uint64_t total_invocations, struct r300_grid_fold *fold)
{
   if (total_invocations == 0 ||
       total_invocations >
          (uint64_t)R300_RASTER_AXIS_CAP * R300_RASTER_AXIS_CAP)
      return false;
   const uint32_t total = (uint32_t)total_invocations;
   fold->width  = total <= R300_RASTER_AXIS_CAP ? total
                                                  : R300_RASTER_AXIS_CAP;
   fold->height = (total + R300_RASTER_AXIS_CAP - 1) /
                  R300_RASTER_AXIS_CAP;
   if (total <= R300_RASTER_AXIS_CAP)
      fold->height = 1;
   fold->dim_x = fold->width;
   fold->dim_y = fold->height;
   fold->dim_z = 1;
   fold->z_row_folded = false;
   return fold->width <= R300_RASTER_AXIS_CAP &&
          fold->height <= R300_RASTER_AXIS_CAP;
}

/* Fold a 2D grid natively: raster extent equals the grid extent. */
static inline bool
r300_grid_fold_2d(uint32_t dim_x, uint32_t dim_y,
                    struct r300_grid_fold *fold)
{
   if (dim_x == 0 || dim_y == 0 ||
       dim_x > R300_RASTER_AXIS_CAP || dim_y > R300_RASTER_AXIS_CAP)
      return false;
   fold->width  = dim_x;
   fold->height = dim_y;
   fold->dim_x = dim_x;
   fold->dim_y = dim_y;
   fold->dim_z = 1;
   fold->z_row_folded = false;
   return true;
}

/* Fold a 3D grid by carrying z in raster rows: y_raster = z * dim_y + y.
 * The fragment program recovers z = floor(y_raster / dim_y) and
 * y = y_raster - z * dim_y; both stay exact because y_raster never exceeds
 * the 2048 axis cap.  The sub-pixel interpolation lane for z is a separate,
 * calibration-gated alternative and is not expressed here. */
static inline bool
r300_grid_fold_3d(uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                    struct r300_grid_fold *fold)
{
   if (dim_x == 0 || dim_y == 0 || dim_z == 0)
      return false;
   const uint64_t folded_rows = (uint64_t)dim_y * dim_z;
   if (dim_x > R300_RASTER_AXIS_CAP || folded_rows > R300_RASTER_AXIS_CAP)
      return false;
   fold->width  = dim_x;
   fold->height = (unsigned)folded_rows;
   fold->dim_x = dim_x;
   fold->dim_y = dim_y;
   fold->dim_z = dim_z;
   fold->z_row_folded = dim_z > 1;
   return true;
}

#endif /* R300_GRID_FOLD_H */
