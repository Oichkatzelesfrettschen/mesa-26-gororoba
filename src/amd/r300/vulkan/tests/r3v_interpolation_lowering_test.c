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

int
main(void)
{
   test_conjunction_opens_direct();
   test_each_predicate_flipped_replicates();
   test_clip_class();
   if (failures != 0) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return EXIT_FAILURE;
   }
   printf("r3v-interpolation-lowering: ok\n");
   return EXIT_SUCCESS;
}
