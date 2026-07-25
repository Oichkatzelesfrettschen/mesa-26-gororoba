/*
 * SPDX-License-Identifier: MIT
 */

/* Host corpus for the R2VB Draw-parity clip classifier.  Pins the bit order,
 * the NaN-outside rule, the half-Z near plane, the guard-band coefficient,
 * and the FALLBACK-dominates law against r300_r2vb_clip.h, so the FP24
 * clip-BO oracle and the route action build on a fixed contract. */

#include <stdio.h>
#include <string.h>
#include "r300_r2vb_clip.h"

static int failures;

#define CHECK(cond, name)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL %s: %s\n", name, #cond);                      \
         failures++;                                                         \
      }                                                                      \
   } while (0)

static uint8_t
code(float x, float y, float z, float w, float k, bool half_z)
{
   const float v[4] = { x, y, z, w };
   return r300_r2vb_clipcode(v, k, half_z);
}

static void
test_inside_and_boundaries(void)
{
   const float K = R300_R2VB_CLIP_CANONICAL_K;

   CHECK(code(0, 0, 0, 1, K, false) == 0, "origin inside");
   /* Exactly on a plane: distance 0 satisfies >= 0, no bit set. */
   CHECK(code(1, 0, 0, 1, K, false) == 0, "on right plane");
   CHECK(code(-1, 0, 0, 1, K, false) == 0, "on left plane");
   CHECK(code(0, 1, 0, 1, K, false) == 0, "on top plane");
   CHECK(code(0, -1, 0, 1, K, false) == 0, "on bottom plane");
   CHECK(code(0, 0, -1, 1, K, false) == 0, "on near plane full-Z");
   CHECK(code(0, 0, 1, 1, K, false) == 0, "on far plane");
   CHECK(code(0, 0, 0, 1, K, true) == 0, "on near plane half-Z");
}

static void
test_single_plane_outside(void)
{
   const float K = R300_R2VB_CLIP_CANONICAL_K;

   CHECK(code(2, 0, 0, 1, K, false) == R300_R2VB_CLIP_RIGHT, "outside right");
   CHECK(code(-2, 0, 0, 1, K, false) == R300_R2VB_CLIP_LEFT, "outside left");
   CHECK(code(0, 2, 0, 1, K, false) == R300_R2VB_CLIP_TOP, "outside top");
   CHECK(code(0, -2, 0, 1, K, false) == R300_R2VB_CLIP_BOTTOM,
         "outside bottom");
   CHECK(code(0, 0, -2, 1, K, false) == R300_R2VB_CLIP_NEAR,
         "outside near full-Z");
   CHECK(code(0, 0, 2, 1, K, false) == R300_R2VB_CLIP_FAR, "outside far");
}

static void
test_half_z(void)
{
   const float K = R300_R2VB_CLIP_CANONICAL_K;

   /* z in (-w, 0): inside the full-Z cube, outside the half-Z near plane. */
   CHECK(code(0, 0, -0.5f, 1, K, false) == 0, "z=-0.5 inside full-Z");
   CHECK(code(0, 0, -0.5f, 1, K, true) == R300_R2VB_CLIP_NEAR,
         "z=-0.5 outside half-Z near");
   /* Far plane is identical in both modes. */
   CHECK(code(0, 0, 2, 1, K, true) == R300_R2VB_CLIP_FAR, "half-Z far");
}

static void
test_guard_band(void)
{
   const float G = R300_R2VB_CLIP_GUARD_K;
   const float K = R300_R2VB_CLIP_CANONICAL_K;

   /* x = 1.5w: outside the canonical frustum, inside the [-2w,+2w] guard. */
   CHECK(code(1.5f, 0, 0, 1, K, false) == R300_R2VB_CLIP_RIGHT,
         "x=1.5w outside canonical");
   CHECK(code(1.5f, 0, 0, 1, G, false) == 0, "x=1.5w inside guard band");
   /* The guard boundary itself: x = 2w has distance 0, still inside. */
   CHECK(code(2, 0, 0, 1, G, false) == 0, "x=2w on guard boundary");
   CHECK(code(2.5f, 0, 0, 1, G, false) == R300_R2VB_CLIP_RIGHT,
         "x=2.5w outside guard");
   CHECK(code(0, -2.5f, 0, 1, G, false) == R300_R2VB_CLIP_BOTTOM,
         "y=-2.5w outside guard bottom");
}

static void
test_negative_and_zero_w(void)
{
   const float K = R300_R2VB_CLIP_CANONICAL_K;

   /* w = 0 at the origin: every plane distance is exactly 0, mask clear --
    * which is exactly why divide safety is carried separately. */
   CHECK(code(0, 0, 0, 0, K, false) == 0, "w=0 origin mask clear");
   CHECK(r300_r2vb_w_unsafe(0.0f), "w=0 unsafe");
   /* Negative w: every +/- plane pair fails simultaneously. */
   CHECK(code(0, 0, 0, -1, K, false) ==
            (R300_R2VB_CLIP_RIGHT | R300_R2VB_CLIP_LEFT | R300_R2VB_CLIP_TOP |
             R300_R2VB_CLIP_BOTTOM | R300_R2VB_CLIP_NEAR | R300_R2VB_CLIP_FAR),
         "w=-1 outside all planes");
   CHECK(r300_r2vb_w_unsafe(-1.0f), "w=-1 unsafe");
   /* Sub-floor positive w is undividable but geometrically classifiable. */
   CHECK(r300_r2vb_w_unsafe(1.0f / 65536.0f), "sub-floor w unsafe");
   CHECK(!r300_r2vb_w_unsafe(1.0f / 32768.0f), "floor w safe");
   CHECK(!r300_r2vb_w_unsafe(1.0f), "w=1 safe");
}

static void
test_nan_inf(void)
{
   const float K = R300_R2VB_CLIP_CANONICAL_K;
   const float nan = nanf("");
   const float inf = INFINITY;

   /* NaN position: every plane distance is NaN, !(NaN >= 0) is true, so the
    * vertex classifies outside every plane -- draw's rule. */
   CHECK(code(nan, 0, 0, 1, K, false) ==
            (R300_R2VB_CLIP_RIGHT | R300_R2VB_CLIP_LEFT),
         "NaN x outside left+right");
   CHECK(code(nan, nan, nan, nan, K, false) == 0x3f, "all-NaN outside all");
   CHECK(r300_r2vb_w_unsafe(nan), "NaN w unsafe");
   CHECK(r300_r2vb_w_unsafe(inf), "Inf w unsafe");
   CHECK(r300_r2vb_w_unsafe(-inf), "-Inf w unsafe");
   /* +Inf x fails the right plane, satisfies the left. */
   CHECK(code(inf, 0, 0, 1, K, false) == R300_R2VB_CLIP_RIGHT,
         "+Inf x outside right");
}

static void
tri(enum r300_r2vb_tri_class want, const char *name, const float v0[4],
    const float v1[4], const float v2[4], float k, bool half_z)
{
   uint8_t om = 0xff, am = 0xff;
   enum r300_r2vb_tri_class got =
      r300_r2vb_classify_triangle(v0, v1, v2, k, half_z, &om, &am);
   if (got != want) {
      fprintf(stderr, "FAIL %s: class %d want %d (or=0x%02x and=0x%02x)\n",
              name, got, want, om, am);
      failures++;
   }
}

static void
test_triangles(void)
{
   const float K = R300_R2VB_CLIP_CANONICAL_K;
   const float in0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
   const float in1[4] = { 0.5f, 0.0f, 0.0f, 1.0f };
   const float in2[4] = { 0.0f, 0.5f, 0.0f, 1.0f };
   const float right_out[4] = { 3.0f, 0.0f, 0.0f, 1.0f };
   const float right_out2[4] = { 4.0f, 0.2f, 0.0f, 1.0f };
   const float right_out3[4] = { 3.5f, -0.2f, 0.0f, 1.0f };
   const float near_out[4] = { 0.0f, 0.0f, -3.0f, 1.0f };
   const float far_out[4] = { 0.0f, 0.0f, 3.0f, 1.0f };
   const float tiny_w[4] = { 0.0f, 0.0f, 0.0f, 1.0f / 65536.0f };
   const float nan_w[4] = { 0.0f, 0.0f, 0.0f, NAN };

   tri(R300_R2VB_TRI_ACCEPT, "all inside", in0, in1, in2, K, false);
   tri(R300_R2VB_TRI_REJECT, "all beyond right", right_out, right_out2,
       right_out3, K, false);
   tri(R300_R2VB_TRI_PARTIAL, "one beyond right", in0, in1, right_out, K,
       false);
   tri(R300_R2VB_TRI_PARTIAL, "one behind near", in0, in1, near_out, K,
       false);
   /* Outside two DIFFERENT planes with an empty intersection: or-mask is
    * nonzero, and-mask is zero -- PARTIAL, not REJECT. */
   tri(R300_R2VB_TRI_PARTIAL, "split across near and far", in0, near_out,
       far_out, K, false);
   /* FALLBACK dominates: geometrically accepted but undividable. */
   tri(R300_R2VB_TRI_FALLBACK, "tiny w dominates accept", in0, in1, tiny_w,
       K, false);
   /* FALLBACK also dominates a would-be trivial reject. */
   tri(R300_R2VB_TRI_FALLBACK, "NaN w dominates reject", right_out,
       right_out2, nan_w, K, false);
   /* Half-Z flips a z = -0.5w vertex from accept to partial. */
   const float halfz_v[4] = { 0.0f, 0.0f, -0.5f, 1.0f };
   tri(R300_R2VB_TRI_ACCEPT, "z=-0.5 full-Z accept", in0, in1, halfz_v, K,
       false);
   tri(R300_R2VB_TRI_PARTIAL, "z=-0.5 half-Z partial", in0, in1, halfz_v, K,
       true);
}

static bool
close4(const float a[4], float x, float y, float z, float w, float tol)
{
   return fabsf(a[0] - x) <= tol && fabsf(a[1] - y) <= tol &&
          fabsf(a[2] - z) <= tol && fabsf(a[3] - w) <= tol;
}

static void
mkvert(struct r300_r2vb_clip_vertex *v, float x, float y, float z, float w)
{
   v->clip[0] = x;
   v->clip[1] = y;
   v->clip[2] = z;
   v->clip[3] = w;
   /* Carried attribute = the clip position itself, so the lerp of the
    * payload must land exactly where the lerp of the position does. */
   memcpy(v->attr[0], v->clip, sizeof(v->clip));
}

static void
test_edge_generation(void)
{
   const float K = R300_R2VB_CLIP_CANONICAL_K;
   const float tol = 1e-6f;
   struct r300_r2vb_clip_vertex in[3], out[R300_R2VB_CLIP_MAX_POLY];

   /* Fully inside: the polygon is the input triangle unchanged. */
   mkvert(&in[0], 0, 0, 0, 1);
   mkvert(&in[1], 0.5f, 0, 0, 1);
   mkvert(&in[2], 0, 0.5f, 0, 1);
   CHECK(r300_r2vb_clip_triangle(in, 1, 0x3f, K, false, out) == 3,
         "inside tri untouched");
   CHECK(close4(out[1].clip, 0.5f, 0, 0, 1, tol), "inside v1 verbatim");

   /* One vertex beyond the right plane (x = 3, w = 1): the quad's two new
    * vertices sit exactly on x = w.  Edge (0,0)->(3,0): t = (1-0)/((1-0)-(1-3))
    * = 1/3 -> (1,0). Edge (3,0)->(0,0.5): d=(-2)->(1), t=2/3 -> (1, 1/3). */
   mkvert(&in[0], 0, 0, 0, 1);
   mkvert(&in[1], 3.0f, 0, 0, 1);
   mkvert(&in[2], 0, 0.5f, 0, 1);
   unsigned n = r300_r2vb_clip_triangle(in, 1, R300_R2VB_CLIP_RIGHT, K, false,
                                        out);
   CHECK(n == 4, "one-out right clip yields quad");
   CHECK(close4(out[1].clip, 1.0f, 0.0f, 0, 1, tol), "right entry point");
   CHECK(close4(out[2].clip, 1.0f, 1.0f / 3.0f, 0, 1, tol),
         "right exit point");
   /* The carried attribute took the identical blend. */
   CHECK(close4(out[2].attr[0], 1.0f, 1.0f / 3.0f, 0, 1, tol),
         "attr lerped with position");

   /* Two vertices out: a smaller triangle survives. */
   mkvert(&in[0], 0, 0, 0, 1);
   mkvert(&in[1], 3.0f, 0.25f, 0, 1);
   mkvert(&in[2], 3.0f, -0.25f, 0, 1);
   CHECK(r300_r2vb_clip_triangle(in, 1, R300_R2VB_CLIP_RIGHT, K, false, out)
            == 3, "two-out right clip yields tri");

   /* Fully outside the clipped plane: nothing survives. */
   mkvert(&in[0], 3, 0, 0, 1);
   mkvert(&in[1], 4, 0, 0, 1);
   mkvert(&in[2], 3, 1, 0, 1);
   CHECK(r300_r2vb_clip_triangle(in, 1, R300_R2VB_CLIP_RIGHT, K, false, out)
            == 0, "all-out right clip empty");

   /* Half-Z near clip at z = 0: one vertex behind (z = -1) generates the
    * intersection at z = 0 with t = d0/(d0-d1) on z alone. */
   mkvert(&in[0], 0, 0, 0.5f, 1);
   mkvert(&in[1], 0.5f, 0, 0.5f, 1);
   mkvert(&in[2], 0, 0.5f, -1.0f, 1);
   n = r300_r2vb_clip_triangle(in, 1, R300_R2VB_CLIP_NEAR, K, true, out);
   CHECK(n == 4, "half-Z near clip yields quad");
   for (unsigned i = 0; i < n; i++)
      CHECK(out[i].clip[2] >= -tol, "half-Z near output z >= 0");

   /* Corner straddling two planes (right + top): both get clipped when both
    * bits are in the mask, and every output is inside both. */
   mkvert(&in[0], 0, 0, 0, 1);
   mkvert(&in[1], 3.0f, 0, 0, 1);
   mkvert(&in[2], 0, 3.0f, 0, 1);
   n = r300_r2vb_clip_triangle(in, 1,
                               R300_R2VB_CLIP_RIGHT | R300_R2VB_CLIP_TOP, K,
                               false, out);
   CHECK(n >= 3 && n <= R300_R2VB_CLIP_MAX_POLY, "two-plane clip poly size");
   for (unsigned i = 0; i < n; i++) {
      CHECK(out[i].clip[0] <= 1.0f + tol, "two-plane output inside right");
      CHECK(out[i].clip[1] <= 1.0f + tol, "two-plane output inside top");
   }

   /* Degenerate: a triangle sliced to nothing by successive planes (entirely
    * in the right/top corner outside both) returns 0, not a sliver. */
   mkvert(&in[0], 2.0f, 2.0f, 0, 1);
   mkvert(&in[1], 3.0f, 2.0f, 0, 1);
   mkvert(&in[2], 2.0f, 3.0f, 0, 1);
   CHECK(r300_r2vb_clip_triangle(in, 1,
                                 R300_R2VB_CLIP_RIGHT | R300_R2VB_CLIP_TOP, K,
                                 false, out) == 0,
         "corner-outside tri fully clipped");
}

int
main(void)
{
   test_inside_and_boundaries();
   test_single_plane_outside();
   test_half_z();
   test_guard_band();
   test_negative_and_zero_w();
   test_nan_inf();
   test_triangles();
   test_edge_generation();

   if (failures) {
      fprintf(stderr, "r300_r2vb_clip_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("r300_r2vb_clip_test: all checks passed\n");
   return 0;
}
