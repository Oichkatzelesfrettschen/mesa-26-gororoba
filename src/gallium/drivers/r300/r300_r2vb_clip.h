/*
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

/* Which producer, if any, filled the clip BO the current delivery re-ingests.
 * NONE marks a pure-passthrough delivery: the application vertex buffers reach
 * TCL_BYPASS directly with no producer pass and no clip BO, so the producer-fed
 * delivery capture and its position-only invariant do not apply.  SINGLE is one
 * over-budget-free producer FS; SPLIT is the two-pass carry-BO composition that
 * escapes the 64-slot budget.  The delivery capture reads this to label the
 * record and to skip the pure-passthrough caller. */
enum r300_r2vb_producer_kind {
   R300_R2VB_PRODUCER_NONE = 0,
   R300_R2VB_PRODUCER_SINGLE = 1,
   R300_R2VB_PRODUCER_SPLIT = 2,
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

/* True when any clip-space component is non-finite.  Intersection weight
 * t = da/(da-db) and the FP24 divide both poison on NaN/Inf, so classification
 * and edge rebuild treat such a vertex as undividable (FALLBACK). */
static inline bool
r300_r2vb_clip_nonfinite(const float v[4])
{
   return !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) ||
          !isfinite(v[3]);
}

/* Per-vertex clip code over the six hardwired planes.  Every comparison is
 * written as !(distance >= 0) so a NaN distance classifies OUTSIDE, matching
 * draw's "comparisons must be true for them" rule.  plane_enable is a bit
 * mask of R300_R2VB_CLIP_*; clear NEAR/FAR when the rasterizer disables
 * depth clipping so the oracle matches draw_update_clip_flags. */
static inline uint8_t
r300_r2vb_clipcode_planes(const float v[4], float k, bool half_z,
                          uint8_t plane_enable)
{
   const float x = v[0], y = v[1], z = v[2], w = v[3];
   uint8_t mask = 0;

   if ((plane_enable & R300_R2VB_CLIP_RIGHT) && !(w - k * x >= 0.0f))
      mask |= R300_R2VB_CLIP_RIGHT;
   if ((plane_enable & R300_R2VB_CLIP_LEFT) && !(w + k * x >= 0.0f))
      mask |= R300_R2VB_CLIP_LEFT;
   if ((plane_enable & R300_R2VB_CLIP_TOP) && !(w - k * y >= 0.0f))
      mask |= R300_R2VB_CLIP_TOP;
   if ((plane_enable & R300_R2VB_CLIP_BOTTOM) && !(w + k * y >= 0.0f))
      mask |= R300_R2VB_CLIP_BOTTOM;
   if (plane_enable & R300_R2VB_CLIP_NEAR) {
      if (half_z) {
         if (!(z >= 0.0f))
            mask |= R300_R2VB_CLIP_NEAR;
      } else {
         if (!(z + w >= 0.0f))
            mask |= R300_R2VB_CLIP_NEAR;
      }
   }
   if ((plane_enable & R300_R2VB_CLIP_FAR) && !(w - z >= 0.0f))
      mask |= R300_R2VB_CLIP_FAR;
   return mask;
}

static inline uint8_t
r300_r2vb_clipcode(const float v[4], float k, bool half_z)
{
   return r300_r2vb_clipcode_planes(v, k, half_z, 0x3fu);
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
 * clip (intersection weight) paths.  plane_enable selects which hardwired
 * planes participate (clear NEAR/FAR when depth clip is off). */
static inline enum r300_r2vb_tri_class
r300_r2vb_classify_triangle_planes(const float v0[4], const float v1[4],
                                   const float v2[4], float k, bool half_z,
                                   uint8_t plane_enable,
                                   uint8_t *or_mask_out, uint8_t *and_mask_out)
{
   if (r300_r2vb_clip_nonfinite(v0) || r300_r2vb_clip_nonfinite(v1) ||
       r300_r2vb_clip_nonfinite(v2) || r300_r2vb_w_unsafe(v0[3]) ||
       r300_r2vb_w_unsafe(v1[3]) || r300_r2vb_w_unsafe(v2[3])) {
      if (or_mask_out)
         *or_mask_out = 0xffu;
      if (and_mask_out)
         *and_mask_out = 0;
      return R300_R2VB_TRI_FALLBACK;
   }

   const uint8_t m0 = r300_r2vb_clipcode_planes(v0, k, half_z, plane_enable);
   const uint8_t m1 = r300_r2vb_clipcode_planes(v1, k, half_z, plane_enable);
   const uint8_t m2 = r300_r2vb_clipcode_planes(v2, k, half_z, plane_enable);
   const uint8_t or_mask = m0 | m1 | m2;
   const uint8_t and_mask = m0 & m1 & m2;

   if (or_mask_out)
      *or_mask_out = or_mask;
   if (and_mask_out)
      *and_mask_out = and_mask;

   if (or_mask == 0)
      return R300_R2VB_TRI_ACCEPT;
   if (and_mask != 0)
      return R300_R2VB_TRI_REJECT;
   return R300_R2VB_TRI_PARTIAL;
}

static inline enum r300_r2vb_tri_class
r300_r2vb_classify_triangle(const float v0[4], const float v1[4],
                            const float v2[4], float k, bool half_z,
                            uint8_t *or_mask_out, uint8_t *and_mask_out)
{
   return r300_r2vb_classify_triangle_planes(v0, v1, v2, k, half_z, 0x3fu,
                                             or_mask_out, and_mask_out);
}

/* Edge generation runs Sutherland-Hodgman in the clip-space (collineation)
 * domain: every plane functional is linear there, so the intersection of an
 * edge with a plane is the single blend t = d_out / (d_out - d_in) applied
 * uniformly to the clip position AND every carried attribute.  A triangle
 * clipped against six planes gains at most one vertex per plane. */
#define R300_R2VB_CLIP_MAX_POLY  9
#define R300_R2VB_CLIP_MAX_ATTRS 8

struct r300_r2vb_clip_vertex {
   float clip[4];
   float attr[R300_R2VB_CLIP_MAX_ATTRS][4];
};

/* Signed distance to one hardwired plane, positive inside, in the exact
 * functional form the clip-code bits negate.  The bit index selects the
 * plane, so a classification or-mask drives which planes get a clip pass. */
static inline float
r300_r2vb_plane_dist(const float v[4], unsigned plane_bit, float k,
                     bool half_z)
{
   switch (plane_bit) {
   case 0: return v[3] - k * v[0]; /* RIGHT */
   case 1: return v[3] + k * v[0]; /* LEFT */
   case 2: return v[3] - k * v[1]; /* TOP */
   case 3: return v[3] + k * v[1]; /* BOTTOM */
   case 4: return half_z ? v[2] : v[2] + v[3]; /* NEAR */
   default: return v[3] - v[2]; /* FAR */
   }
}

static inline void
r300_r2vb_clip_lerp(const struct r300_r2vb_clip_vertex *a,
                    const struct r300_r2vb_clip_vertex *b, float t,
                    unsigned num_attrs, struct r300_r2vb_clip_vertex *out)
{
   for (int c = 0; c < 4; c++)
      out->clip[c] = a->clip[c] + t * (b->clip[c] - a->clip[c]);
   for (unsigned n = 0; n < num_attrs; n++)
      for (int c = 0; c < 4; c++)
         out->attr[n][c] = a->attr[n][c] + t * (b->attr[n][c] - a->attr[n][c]);
}

/* Clip one triangle against the planes named in plane_mask, carrying
 * num_attrs vec4 attributes through every intersection blend.  Returns the
 * clipped polygon's vertex count (0 when nothing survives); the polygon
 * preserves the input winding, so a fan retriangulation keeps orientation.
 * Vertices exactly on a plane (dist == 0) count as inside, matching the
 * !(dist >= 0) outside rule of the clip codes. */
static inline unsigned
r300_r2vb_clip_triangle(const struct r300_r2vb_clip_vertex in[3],
                        unsigned num_attrs, uint8_t plane_mask, float k,
                        bool half_z,
                        struct r300_r2vb_clip_vertex out[R300_R2VB_CLIP_MAX_POLY])
{
   if (num_attrs > R300_R2VB_CLIP_MAX_ATTRS)
      return 0;
   struct r300_r2vb_clip_vertex buf[R300_R2VB_CLIP_MAX_POLY];
   struct r300_r2vb_clip_vertex *cur = out, *next = buf;
   unsigned n = 3;

   out[0] = in[0];
   out[1] = in[1];
   out[2] = in[2];

   for (unsigned plane = 0; plane < 6 && n >= 3; plane++) {
      if (!(plane_mask & (1u << plane)))
         continue;
      unsigned m = 0;
      for (unsigned i = 0; i < n; i++) {
         const struct r300_r2vb_clip_vertex *a = &cur[i];
         const struct r300_r2vb_clip_vertex *b = &cur[(i + 1) % n];
         const float da = r300_r2vb_plane_dist(a->clip, plane, k, half_z);
         const float db = r300_r2vb_plane_dist(b->clip, plane, k, half_z);
         /* Non-finite distances yield NaN blend weights; refuse the polygon. */
         if (!isfinite(da) || !isfinite(db))
            return 0;
         const bool ina = da >= 0.0f;
         const bool inb = db >= 0.0f;
         if (ina)
            next[m++] = *a;
         if (ina != inb) {
            const float denom = da - db;
            if (!isfinite(denom) || denom == 0.0f)
               return 0;
            r300_r2vb_clip_lerp(a, b, da / denom, num_attrs, &next[m++]);
         }
      }
      struct r300_r2vb_clip_vertex *tmp = cur;
      cur = next;
      next = tmp;
      n = m;
   }
   if (n < 3)
      return 0;
   if (cur != out)
      for (unsigned i = 0; i < n; i++)
         out[i] = cur[i];
   return n;
}

#endif /* R300_R2VB_CLIP_H */
