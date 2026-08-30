/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the interpolation route selector: the direct GA route
 * opens on the full conjunction alone, and each predicate flipped on
 * its own -- the partially clipped primitive included -- lands on
 * replication with a reason naming it.
 */

#include "r3v_interpolation_lowering.h"
#include "r3v_shader_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                        \
   do {                                                                    \
      if (!(cond)) {                                                       \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                 #cond);                                                   \
         failures++;                                                       \
      }                                                                    \
   } while (0)

static void
flat_vec4_link(struct r3v_shader_interface_link *link)
{
   memset(link, 0, sizeof(*link));
   link->varying_mask = 1u;
   link->flat_mask = 1u;
   link->varyings[0] = (struct r3v_shader_interface_varying){
      .present = true,
      .scalar = R3V_SHADER_INTERFACE_SCALAR_FLOAT32,
      .width = 4,
      .component_mask = 0xf,
      .interpolation = R3V_SHADER_INTERFACE_FLAT,
   };
}

static struct r3v_interpolation_query
direct_query(const struct r3v_shader_interface_link *link)
{
   return (struct r3v_interpolation_query){
      .cpu_delivery = true,
      .triangle_list = true,
      .clip_class = R3V_INTERPOLATION_CLIP_ACCEPT,
      .link = link,
      .rs_destination_available = true,
      .fragment_consumes_destination = true,
      .provoking_first_representable = true,
   };
}

static void
test_conjunction_opens_direct(void)
{
   struct r3v_shader_interface_link link;
   flat_vec4_link(&link);
   const struct r3v_interpolation_query q = direct_query(&link);
   const char *reason = NULL;
   CHECK(r3v_interpolation_route_select(&q, &reason) ==
         R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0);
   CHECK(reason != NULL && strstr(reason, "direct") != NULL);
}

static void
expect_replicate(const struct r3v_interpolation_query *q,
                 const char *fragment)
{
   const char *reason = NULL;
   CHECK(r3v_interpolation_route_select(q, &reason) ==
         R3V_INTERPOLATION_ROUTE_REPLICATE);
   CHECK(reason != NULL && strstr(reason, fragment) != NULL);
}

static void
expect_unsupported(const struct r3v_interpolation_query *q,
                   const char *fragment)
{
   const char *reason = NULL;
   CHECK(r3v_interpolation_route_select(q, &reason) ==
         R3V_INTERPOLATION_ROUTE_UNSUPPORTED);
   CHECK(reason != NULL && strstr(reason, fragment) != NULL);
}

static void
test_each_predicate_flipped_replicates(void)
{
   struct r3v_shader_interface_link link;
   flat_vec4_link(&link);
   struct r3v_interpolation_query q;

   q = direct_query(&link);
   q.cpu_delivery = false;
   expect_replicate(&q, "not CPU");

   q = direct_query(&link);
   q.triangle_list = false;
   expect_replicate(&q, "triangle list");

   /* The partially clipped primitive: the direct selector refuses it. */
   q = direct_query(&link);
   q.clip_class = R3V_INTERPOLATION_CLIP_PARTIAL;
   expect_replicate(&q, "ACCEPT");

   q = direct_query(&link);
   q.rs_destination_available = false;
   expect_replicate(&q, "RS destination");

   q = direct_query(&link);
   q.fragment_consumes_destination = false;
   expect_replicate(&q, "consume");

   q = direct_query(&link);
   q.provoking_first_representable = false;
   expect_replicate(&q, "FIRST");

   struct r3v_shader_interface_link two;
   flat_vec4_link(&two);
   two.varying_mask = 3u;
   two.varyings[1] = two.varyings[0];
   two.varyings[1].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   q = direct_query(&two);
   expect_replicate(&q, "color 0");

   struct r3v_shader_interface_link vec3;
   flat_vec4_link(&vec3);
   vec3.varyings[0].width = 3;
   vec3.varyings[0].component_mask = 0x7;
   q = direct_query(&vec3);
   expect_replicate(&q, "vec4");

   struct r3v_shader_interface_link smooth;
   flat_vec4_link(&smooth);
   smooth.flat_mask = 0;
   q = direct_query(&smooth);
   expect_replicate(&q, "no Flat");
}

static void
set_vertex(uint32_t *records, uint32_t record_dwords, uint32_t vertex,
           float x, float y, float z, float w)
{
   const float p[4] = { x, y, z, w };
   memcpy(&records[vertex * record_dwords], p, sizeof(p));
}

static void
test_clip_class(void)
{
   uint32_t records[3 * 8] = { 0 };
   set_vertex(records, 8, 0, -0.75f, -0.75f, 0.0f, 1.0f);
   set_vertex(records, 8, 1, 0.75f, -0.75f, 0.0f, 1.0f);
   set_vertex(records, 8, 2, 0.0f, 0.75f, 0.0f, 1.0f);
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 8) ==
         R3V_INTERPOLATION_CLIP_ACCEPT);
   /* On the boundary is inside the closed volume. */
   set_vertex(records, 8, 2, 1.0f, 1.0f, 1.0f, 1.0f);
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 8) ==
         R3V_INTERPOLATION_CLIP_ACCEPT);
   set_vertex(records, 8, 2, 1.5f, 0.75f, 0.0f, 1.0f);
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 8) ==
         R3V_INTERPOLATION_CLIP_PARTIAL);
   set_vertex(records, 8, 2, 0.0f, 0.75f, -0.1f, 1.0f);
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 8) ==
         R3V_INTERPOLATION_CLIP_PARTIAL);
   set_vertex(records, 8, 2, 0.0f, 0.0f, 0.0f, 0.0f);
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 8) ==
         R3V_INTERPOLATION_CLIP_PARTIAL);
   set_vertex(records, 8, 2, 0.0f, 0.0f, 0.0f, -1.0f);
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 8) ==
         R3V_INTERPOLATION_CLIP_PARTIAL);
   const uint32_t nan = 0x7fc00000u;
   memcpy(&records[2 * 8], &nan, sizeof(nan));
   set_vertex(records, 8, 2, 0.0f, 0.0f, 0.0f, 1.0f);
   memcpy(&records[2 * 8], &nan, sizeof(nan));
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 8) ==
         R3V_INTERPOLATION_CLIP_PARTIAL);
   CHECK(r3v_interpolation_clip_class_of_triangle(NULL, 8) ==
         R3V_INTERPOLATION_CLIP_PARTIAL);
   CHECK(r3v_interpolation_clip_class_of_triangle(records, 3) ==
         R3V_INTERPOLATION_CLIP_PARTIAL);
}

static void
noperspective_vec4_link(struct r3v_shader_interface_link *link)
{
   memset(link, 0, sizeof(*link));
   link->varying_mask = 1u;
   link->noperspective_mask = 1u;
   link->varyings[0] = (struct r3v_shader_interface_varying){
      .present = true,
      .scalar = R3V_SHADER_INTERFACE_SCALAR_FLOAT32,
      .width = 4,
      .component_mask = 0xf,
      .interpolation = R3V_SHADER_INTERFACE_NOPERSPECTIVE,
   };
}

static struct r3v_rs_probe_query
probe_query(const struct r3v_shader_interface_link *link, bool tex_adj,
            bool w_select)
{
   return (struct r3v_rs_probe_query){
      .tex_adj_gate = tex_adj,
      .w_select_gate = w_select,
      .cpu_delivery = true,
      .triangle_list = true,
      .link = link,
      .rs_destination_available = true,
      .fragment_consumes_destination = true,
   };
}

static enum r3v_rs_probe_candidate
select_probe(struct r3v_rs_probe_query query, const char **reason)
{
   return r3v_rs_probe_candidate_select(&query, reason);
}

/* The probe candidate opens on exactly one gate over a NoPerspective
 * full-vec4 interface on the CPU triangle-list route, and every
 * flipped predicate -- including the Smooth interface under an open
 * gate, the mutation that keeps the candidate state without the
 * interface -- yields the control. */
/* The direct GB W_SELECT route opens on the NoPerspective conjunction
 * and refuses, UNSUPPORTED, on each flipped predicate: no NoPerspective
 * varying reaches replication's perspective interpolation. */
static void
test_noperspective_conjunction_opens_w_select(void)
{
   struct r3v_shader_interface_link link;
   noperspective_vec4_link(&link);
   const struct r3v_interpolation_query q = direct_query(&link);
   const char *reason = NULL;
   CHECK(r3v_interpolation_route_select(&q, &reason) ==
         R3V_INTERPOLATION_ROUTE_DIRECT_GB_W_SELECT);
   CHECK(reason != NULL && strstr(reason, "W_SELECT") != NULL);

   struct r3v_interpolation_query f = direct_query(&link);
   f.cpu_delivery = false;
   expect_unsupported(&f, "not CPU");
   f = direct_query(&link);
   f.triangle_list = false;
   expect_unsupported(&f, "triangle list");
   f = direct_query(&link);
   f.clip_class = R3V_INTERPOLATION_CLIP_PARTIAL;
   expect_unsupported(&f, "ACCEPT");
   f = direct_query(&link);
   f.rs_destination_available = false;
   expect_unsupported(&f, "RS destination");
   f = direct_query(&link);
   f.fragment_consumes_destination = false;
   expect_unsupported(&f, "consume");

   /* A Smooth location beside the NoPerspective one: W_SELECT is one
    * word for the draw, so the interface refuses. */
   struct r3v_shader_interface_link beside;
   noperspective_vec4_link(&beside);
   beside.varying_mask = 3u;
   beside.varyings[1] = beside.varyings[0];
   beside.varyings[1].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   f = direct_query(&beside);
   expect_unsupported(&f, "map completely");
   struct r3v_shader_interface_link narrow;
   noperspective_vec4_link(&narrow);
   narrow.varyings[0].width = 3;
   f = direct_query(&narrow);
   expect_unsupported(&f, "full float vec4");
   /* A Flat location beside the NoPerspective one refuses: the Flat
    * route would replicate the NoPerspective location with
    * perspective. */
   struct r3v_shader_interface_link mixed;
   noperspective_vec4_link(&mixed);
   mixed.varying_mask = 3u;
   mixed.flat_mask = 2u;
   mixed.varyings[1] = mixed.varyings[0];
   mixed.varyings[1].interpolation = R3V_SHADER_INTERFACE_FLAT;
   f = direct_query(&mixed);
   expect_unsupported(&f, "mixed");
   /* A Smooth-only interface stays on replication. */
   struct r3v_shader_interface_link smooth;
   noperspective_vec4_link(&smooth);
   smooth.noperspective_mask = 0;
   smooth.varyings[0].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   f = direct_query(&smooth);
   expect_replicate(&f, "no Flat");

   /* The forced-carrier gate moves the same conjunction onto the
    * reciprocal carrier and leaves every refusal UNSUPPORTED. */
   f = direct_query(&link);
   f.carrier_forced = true;
   reason = NULL;
   CHECK(r3v_interpolation_route_select(&f, &reason) ==
         R3V_INTERPOLATION_ROUTE_RECIPROCAL_CARRIER);
   CHECK(reason != NULL && strstr(reason, "carrier") != NULL);
   f = direct_query(&narrow);
   f.carrier_forced = true;
   expect_unsupported(&f, "full float vec4");
   f = direct_query(&beside);
   f.carrier_forced = true;
   expect_unsupported(&f, "map completely");
}

static void
test_probe_candidate_conjunction(void)
{
   struct r3v_shader_interface_link link;
   noperspective_vec4_link(&link);
   const char *reason = "unset";
   CHECK(select_probe(probe_query(&link, true, false),
                                       &reason) == R3V_RS_PROBE_TEX_ADJ);
   CHECK(reason == NULL);
   CHECK(select_probe(probe_query(&link, false, true),
                                       &reason) == R3V_RS_PROBE_W_SELECT_ONE);
   CHECK(reason == NULL);
   CHECK(select_probe(probe_query(&link, false, false),
                                       &reason) == R3V_RS_PROBE_NONE);
   CHECK(reason != NULL && strstr(reason, "no probe gate") != NULL);
   CHECK(select_probe(probe_query(&link, true, true),
                                       &reason) == R3V_RS_PROBE_NONE);
   CHECK(reason != NULL && strstr(reason, "both") != NULL);

   struct r3v_rs_probe_query q = probe_query(&link, true, false);
   q.cpu_delivery = false;
   CHECK(r3v_rs_probe_candidate_select(&q, NULL) == R3V_RS_PROBE_NONE);
   q = probe_query(&link, true, false);
   q.triangle_list = false;
   CHECK(r3v_rs_probe_candidate_select(&q, NULL) == R3V_RS_PROBE_NONE);
   q = probe_query(&link, true, false);
   q.rs_destination_available = false;
   CHECK(r3v_rs_probe_candidate_select(&q, NULL) == R3V_RS_PROBE_NONE);
   q = probe_query(&link, true, false);
   q.fragment_consumes_destination = false;
   CHECK(r3v_rs_probe_candidate_select(&q, NULL) == R3V_RS_PROBE_NONE);
   CHECK(r3v_rs_probe_candidate_select(NULL, NULL) == R3V_RS_PROBE_NONE);

   /* The Smooth interface under the open gate: the control. */
   struct r3v_shader_interface_link smooth;
   noperspective_vec4_link(&smooth);
   smooth.noperspective_mask = 0;
   smooth.varyings[0].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   CHECK(select_probe(probe_query(&smooth, true, false),
                                       &reason) == R3V_RS_PROBE_NONE);
   CHECK(reason != NULL && strstr(reason, "no NoPerspective") != NULL);
   /* A Flat interface is not a probe interface either. */
   struct r3v_shader_interface_link flat;
   flat_vec4_link(&flat);
   CHECK(select_probe(probe_query(&flat, true, false),
                                       NULL) == R3V_RS_PROBE_NONE);
   /* Two locations, or a vec3, leave the one-TEX0 cell. */
   struct r3v_shader_interface_link two;
   noperspective_vec4_link(&two);
   two.varying_mask = 3u;
   two.noperspective_mask = 3u;
   two.varyings[1] = two.varyings[0];
   CHECK(select_probe(probe_query(&two, true, false),
                                       NULL) == R3V_RS_PROBE_NONE);
   struct r3v_shader_interface_link vec3;
   noperspective_vec4_link(&vec3);
   vec3.varyings[0].width = 3;
   vec3.varyings[0].component_mask = 0x7;
   CHECK(select_probe(probe_query(&vec3, true, false),
                                       NULL) == R3V_RS_PROBE_NONE);
}

/* The q-lane route opens on a NoPerspective float, vec2, or vec3 at
 * location 0 with a contiguous mask from x under the narrow
 * pass-through fragment program of the same width, admits every
 * clipping class, and refuses UNSUPPORTED on each flipped predicate:
 * width 4 under the narrow program, a narrow program on a Smooth or
 * Flat interface, a component offset, a mismatched width, a Smooth
 * or Flat location beside it, and every route predicate. */
static void
test_noperspective_q_lane_conjunction(void)
{
   for (uint8_t width = 1; width <= 3; width++) {
      struct r3v_shader_interface_link link;
      noperspective_vec4_link(&link);
      link.varyings[0].width = width;
      link.varyings[0].component_mask = (uint8_t)((1u << width) - 1u);
      struct r3v_interpolation_query q = direct_query(&link);
      q.narrow_passthrough_width = width;
      const char *reason = NULL;
      CHECK(r3v_interpolation_route_select(&q, &reason) ==
            R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE);
      CHECK(reason != NULL && strstr(reason, "q-lane") != NULL);
      /* The stage packs ahead of the clipper. */
      q.clip_class = R3V_INTERPOLATION_CLIP_PARTIAL;
      CHECK(r3v_interpolation_route_select(&q, NULL) ==
            R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE);
      /* The force gate leaves the q-lane shape alone. */
      q.carrier_forced = true;
      CHECK(r3v_interpolation_route_select(&q, NULL) ==
            R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE);

      struct r3v_interpolation_query f = direct_query(&link);
      f.narrow_passthrough_width = width;
      f.cpu_delivery = false;
      expect_unsupported(&f, "not CPU");
      f = direct_query(&link);
      f.narrow_passthrough_width = width;
      f.triangle_list = false;
      expect_unsupported(&f, "triangle list");
      f = direct_query(&link);
      f.narrow_passthrough_width = width;
      f.rs_destination_available = false;
      expect_unsupported(&f, "RS destination");
      f = direct_query(&link);
      f.narrow_passthrough_width = width;
      f.fragment_consumes_destination = false;
      expect_unsupported(&f, "consume");
      /* The vec4 pass-through program on a narrow varying. */
      f = direct_query(&link);
      expect_unsupported(&f, "q-lane shape");
      /* A narrow program of another width. */
      f = direct_query(&link);
      f.narrow_passthrough_width = (uint32_t)(width % 3) + 1u;
      expect_unsupported(&f, "q-lane shape");
      /* A component offset: the mask does not start at x. */
      if (width < 4) {
         struct r3v_shader_interface_link offset = link;
         offset.varyings[0].component_mask =
            (uint8_t)(((1u << width) - 1u) << 1);
         f = direct_query(&offset);
         f.narrow_passthrough_width = width;
         expect_unsupported(&f, "q-lane shape");
      }
      /* A Smooth location beside the narrow one. */
      struct r3v_shader_interface_link beside = link;
      beside.varying_mask = 3u;
      beside.varyings[1] = link.varyings[0];
      beside.varyings[1].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
      f = direct_query(&beside);
      f.narrow_passthrough_width = width;
      expect_unsupported(&f, "map completely");
      /* A Flat location beside it. */
      struct r3v_shader_interface_link mixed = link;
      mixed.varying_mask = 3u;
      mixed.flat_mask = 2u;
      mixed.varyings[1] = link.varyings[0];
      mixed.varyings[1].interpolation = R3V_SHADER_INTERFACE_FLAT;
      f = direct_query(&mixed);
      f.narrow_passthrough_width = width;
      expect_unsupported(&f, "mixed");
      /* The narrow program on a Smooth interface of the same width:
       * the q-lane binary would write alpha 1 over a perspective
       * varying, so the draw refuses. */
      struct r3v_shader_interface_link smooth = link;
      smooth.noperspective_mask = 0;
      smooth.varyings[0].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
      f = direct_query(&smooth);
      f.narrow_passthrough_width = width;
      expect_unsupported(&f, "narrow pass-through");
      /* On a Flat interface. */
      struct r3v_shader_interface_link flat = smooth;
      flat.flat_mask = 1u;
      flat.varyings[0].interpolation = R3V_SHADER_INTERFACE_FLAT;
      f = direct_query(&flat);
      f.narrow_passthrough_width = width;
      expect_unsupported(&f, "narrow pass-through");
   }
   /* Width 4 occupies the q lane: the narrow program cannot serve it,
    * and the vec4 conjunction keeps W_SELECT. */
   struct r3v_shader_interface_link vec4;
   noperspective_vec4_link(&vec4);
   struct r3v_interpolation_query f = direct_query(&vec4);
   f.narrow_passthrough_width = 3;
   expect_unsupported(&f, "narrow pass-through fragment program on a vec4");
   f = direct_query(&vec4);
   CHECK(r3v_interpolation_route_select(&f, NULL) ==
         R3V_INTERPOLATION_ROUTE_DIRECT_GB_W_SELECT);
}


/* The mixed carrier conjunction: Smooth vec4 at location 0 beside
 * NoPerspective vec4 at location 1 under the mixed fragment program
 * selects the mixed reciprocal carrier with no gate and in every
 * clipping class; each predicate flipped on its own -- and every other
 * mixed shape -- is UNSUPPORTED, and the mixed interface without the
 * mixed program is UNSUPPORTED. */
static void
mixed_carrier_link(struct r3v_shader_interface_link *link)
{
   memset(link, 0, sizeof(*link));
   link->varying_mask = 3u;
   link->noperspective_mask = 2u;
   link->varyings[0] = (struct r3v_shader_interface_varying){
      .present = true,
      .scalar = R3V_SHADER_INTERFACE_SCALAR_FLOAT32,
      .width = 4,
      .component_mask = 0xf,
      .interpolation = R3V_SHADER_INTERFACE_SMOOTH,
   };
   link->varyings[1] = link->varyings[0];
   link->varyings[1].interpolation = R3V_SHADER_INTERFACE_NOPERSPECTIVE;
}

static void
test_mixed_carrier_conjunction(void)
{
   struct r3v_shader_interface_link link;
   mixed_carrier_link(&link);
   struct r3v_interpolation_query q = direct_query(&link);
   q.mixed_carrier_fragment = true;
   const char *reason = NULL;
   CHECK(r3v_interpolation_route_select(&q, &reason) ==
         R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER);
   CHECK(reason != NULL && strstr(reason, "mixed") != NULL);
   q.clip_class = R3V_INTERPOLATION_CLIP_PARTIAL;
   CHECK(r3v_interpolation_route_select(&q, NULL) ==
         R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER);
   q.carrier_forced = true;
   CHECK(r3v_interpolation_route_select(&q, NULL) ==
         R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER);
   CHECK(r3v_interpolation_published_record_dwords(
            R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER, 12) == 16);
   CHECK(r3v_interpolation_published_record_dwords(
            R3V_INTERPOLATION_ROUTE_RECIPROCAL_CARRIER, 8) == 12);
   CHECK(r3v_interpolation_published_record_dwords(
            R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE, 8) == 8);
   CHECK(r3v_interpolation_published_record_dwords(
            R3V_INTERPOLATION_ROUTE_REPLICATE, 4) == 4);

   struct r3v_interpolation_query f = direct_query(&link);
   f.mixed_carrier_fragment = true;
   f.cpu_delivery = false;
   expect_unsupported(&f, "not CPU");
   f = direct_query(&link);
   f.mixed_carrier_fragment = true;
   f.triangle_list = false;
   expect_unsupported(&f, "triangle list");
   f = direct_query(&link);
   f.mixed_carrier_fragment = true;
   f.rs_destination_available = false;
   expect_unsupported(&f, "RS destination");
   f = direct_query(&link);
   f.mixed_carrier_fragment = true;
   f.fragment_consumes_destination = false;
   expect_unsupported(&f, "consume");
   /* The mixed interface under the vec4 pass-through program. */
   f = direct_query(&link);
   expect_unsupported(&f, "map completely");
   /* Reordered locations: NoPerspective at 0, Smooth at 1. */
   struct r3v_shader_interface_link reordered = link;
   reordered.noperspective_mask = 1u;
   reordered.varyings[0].interpolation = R3V_SHADER_INTERFACE_NOPERSPECTIVE;
   reordered.varyings[1].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   f = direct_query(&reordered);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "Smooth location 0");
   /* Both NoPerspective, both Smooth. */
   struct r3v_shader_interface_link both = link;
   both.noperspective_mask = 3u;
   both.varyings[0].interpolation = R3V_SHADER_INTERFACE_NOPERSPECTIVE;
   f = direct_query(&both);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "Smooth location 0");
   both.noperspective_mask = 0u;
   both.varyings[0].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   both.varyings[1].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   f = direct_query(&both);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "Smooth location 0");
   /* Flat mixed in. */
   struct r3v_shader_interface_link flat = link;
   flat.flat_mask = 1u;
   flat.varyings[0].interpolation = R3V_SHADER_INTERFACE_FLAT;
   f = direct_query(&flat);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "Smooth location 0");
   /* A third location. */
   struct r3v_shader_interface_link three = link;
   three.varying_mask = 7u;
   three.varyings[2] = link.varyings[0];
   f = direct_query(&three);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "Smooth location 0");
   /* A width mismatch, a component offset, an integer varying. */
   struct r3v_shader_interface_link narrow = link;
   narrow.varyings[1].width = 3;
   narrow.varyings[1].component_mask = 0x7;
   f = direct_query(&narrow);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "full float vec4");
   narrow = link;
   narrow.varyings[0].width = 2;
   narrow.varyings[0].component_mask = 0x6;
   f = direct_query(&narrow);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "full float vec4");
   narrow = link;
   narrow.varyings[1].scalar = R3V_SHADER_INTERFACE_SCALAR_INT32;
   f = direct_query(&narrow);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "full float vec4");
   /* The mixed program over a one-location interface. */
   struct r3v_shader_interface_link one;
   noperspective_vec4_link(&one);
   f = direct_query(&one);
   f.mixed_carrier_fragment = true;
   expect_unsupported(&f, "Smooth location 0");
}

int
main(void)
{
   test_conjunction_opens_direct();
   test_each_predicate_flipped_replicates();
   test_clip_class();
   test_probe_candidate_conjunction();
   test_noperspective_conjunction_opens_w_select();
   test_noperspective_q_lane_conjunction();
   test_mixed_carrier_conjunction();
   if (failures != 0) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return EXIT_FAILURE;
   }
   printf("r3v-interpolation-lowering: ok\n");
   return EXIT_SUCCESS;
}
