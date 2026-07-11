/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_CLIP_H
#define R300_R2VB_CLIP_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Coordinate space of an R2VB producer's position output.  CLIP is the raw
 * fragment-ALU M*v result -- a linear homogeneous domain where every clip
 * decision is the sign of a plane functional and edge intersection is a
 * linear blend, so classification and clipping happen here, BEFORE the
 * perspective divide.  WINDOW is divide + viewport applied with w = 1, the
 * form the re-ingest fetches verbatim (VTX_XY_FMT | VTX_Z_FMT | VTX_W0_FMT). */
enum r300_r2vb_position_space {
   R300_R2VB_POSITION_CLIP = 0,
   R300_R2VB_POSITION_WINDOW = 1,
};

/* Clip-code bit order matches the gallium draw module's software clipper
 * (draw_cliptest_tmp.h), so R2VB classification can be differentially tested
 * against draw instead of inventing a second convention.  Bits 6 and up are
 * draw's user-clip-plane range and stay unconsumed here. */
#define R300_R2VB_CLIP_RIGHT  (1u << 0) /* !(w - k*x >= 0) */
#define R300_R2VB_CLIP_LEFT   (1u << 1) /* !(w + k*x >= 0) */
#define R300_R2VB_CLIP_TOP    (1u << 2) /* !(w - k*y >= 0) */
#define R300_R2VB_CLIP_BOTTOM (1u << 3) /* !(w + k*y >= 0) */
#define R300_R2VB_CLIP_NEAR   (1u << 4) /* full-Z !(z + w >= 0); half-Z !(z >= 0) */
#define R300_R2VB_CLIP_FAR    (1u << 5) /* !(w - z >= 0) */

/* draw's XY guard-band coefficient: the guard volume is [-2w, +2w], expressed
 * as a 0.5 multiplier on x/y in the plane functionals.  1.0 gives the
 * canonical [-w, +w] frustum for test vectors. */
#define R300_R2VB_CLIP_GUARD_K    0.5f
#define R300_R2VB_CLIP_CANONICAL_K 1.0f

/* Reciprocal safety floor for the FP24 divide (matches the producer's
 * 1/32768 guard in r2vb_divide_position). */
#define R300_R2VB_W_SAFE_MIN (1.0f / 32768.0f)

/* Per-vertex clip code over the six hardwired planes.  Every comparison is
 * written as !(distance >= 0) so a NaN distance classifies OUTSIDE, matching
 * draw's "comparisons must be true for them" rule. */
static inline uint8_t
r300_r2vb_clipcode(const float v[4], float k, bool half_z)
{
   const float x = v[0], y = v[1], z = v[2], w = v[3];
   uint8_t mask = 0;

   if (!(w - k * x >= 0.0f))
      mask |= R300_R2VB_CLIP_RIGHT;
   if (!(w + k * x >= 0.0f))
      mask |= R300_R2VB_CLIP_LEFT;
   if (!(w - k * y >= 0.0f))
      mask |= R300_R2VB_CLIP_TOP;
   if (!(w + k * y >= 0.0f))
      mask |= R300_R2VB_CLIP_BOTTOM;
   if (half_z) {
      if (!(z >= 0.0f))
         mask |= R300_R2VB_CLIP_NEAR;
   } else {
      if (!(z + w >= 0.0f))
         mask |= R300_R2VB_CLIP_NEAR;
   }
   if (!(w - z >= 0.0f))
      mask |= R300_R2VB_CLIP_FAR;
   return mask;
}

/* Divide safety is carried separately from the clip mask: w below the FP24
 * reciprocal floor (or non-finite) cannot be divided by the producer, and a
 * primitive containing such a vertex must FALL BACK rather than be accepted
 * or dropped.  The negated >= form classifies NaN as unsafe. */
static inline bool
r300_r2vb_w_unsafe(float w)
{
   return !isfinite(w) || !(w >= R300_R2VB_W_SAFE_MIN);
}

enum r300_r2vb_tri_class {
   R300_R2VB_TRI_ACCEPT,   /* or-mask 0: rasterize via the window producer */
   R300_R2VB_TRI_REJECT,   /* and-mask nonzero: all vertices outside one plane */
   R300_R2VB_TRI_PARTIAL,  /* straddles a plane: needs geometric clipping */
   R300_R2VB_TRI_FALLBACK, /* unsafe w present: undividable, route to gallivm */
};

/* Trivial accept / trivial reject over three clip-space vertices.  PARTIAL is
 * the exact handoff to edge generation; FALLBACK dominates every other class
 * because an undividable vertex poisons both the accept (divide) and the
 * clip (intersection weight) paths. */
static inline enum r300_r2vb_tri_class
r300_r2vb_classify_triangle(const float v0[4], const float v1[4],
                            const float v2[4], float k, bool half_z,
                            uint8_t *or_mask_out, uint8_t *and_mask_out)
{
   const uint8_t m0 = r300_r2vb_clipcode(v0, k, half_z);
   const uint8_t m1 = r300_r2vb_clipcode(v1, k, half_z);
   const uint8_t m2 = r300_r2vb_clipcode(v2, k, half_z);
   const uint8_t or_mask = m0 | m1 | m2;
   const uint8_t and_mask = m0 & m1 & m2;

   if (or_mask_out)
      *or_mask_out = or_mask;
   if (and_mask_out)
      *and_mask_out = and_mask;

   if (r300_r2vb_w_unsafe(v0[3]) || r300_r2vb_w_unsafe(v1[3]) ||
       r300_r2vb_w_unsafe(v2[3]))
      return R300_R2VB_TRI_FALLBACK;
   if (or_mask == 0)
      return R300_R2VB_TRI_ACCEPT;
   if (and_mask != 0)
      return R300_R2VB_TRI_REJECT;
   return R300_R2VB_TRI_PARTIAL;
}

#endif /* R300_R2VB_CLIP_H */
