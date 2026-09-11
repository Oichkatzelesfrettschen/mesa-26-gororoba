/* SPDX-License-Identifier: MIT */

#include "../r3v_native.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SURFACE 64u

struct matrix_row {
   enum r3v_native_zmask_plan_operation operation;
   uint32_t aspect_mask;
   bool full_region;
   enum r3v_native_image_representation representation;
   enum r3v_native_zmask_plan_route route;
   bool requires_previous_contents;
   bool preserves_untouched_aspect;
   bool requires_materialization;
};

static const uint32_t aspect_masks[] = {
   R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH,
   R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL,
   R300_ZB_COMBINED_CLEAR_ASPECTS,
};

static const enum r3v_native_image_representation representations[] = {
   R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_LINEAR,
   R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED,
   R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
   R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED,
};

static struct r3v_native_zmask_plan_request
build_request(enum r3v_native_zmask_plan_operation operation,
              uint32_t aspect_mask, bool full_region,
              enum r3v_native_image_representation representation)
{
   return (struct r3v_native_zmask_plan_request){
      .operation = operation,
      .aspect_mask = aspect_mask,
      .x = full_region ? 0u : 8u,
      .y = full_region ? 0u : 4u,
      .width = full_region ? SURFACE : 16u,
      .height = full_region ? SURFACE : 8u,
      .logical_width = SURFACE,
      .logical_height = SURFACE,
      .representation = representation,
      .layout_admitted = true,
   };
}

/* The expected route restates the admission rule from the operation's own
 * obligations: a constant write over both aspects of the whole surface owes
 * nothing to previous contents, and a linear representation carries no
 * ZMASK level to substitute for. */
static enum r3v_native_zmask_plan_route
expected_route(enum r3v_native_zmask_plan_operation operation,
               uint32_t aspect_mask, bool full_region,
               enum r3v_native_image_representation representation)
{
   if (operation != R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR || !full_region ||
       aspect_mask != R300_ZB_COMBINED_CLEAR_ASPECTS ||
       representation == R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_LINEAR)
      return R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY;
   return R3V_NATIVE_ZMASK_PLAN_ROUTE_FAST_CLEAR;
}

static bool
metadata_backed(enum r3v_native_image_representation representation)
{
   return representation == R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR ||
          representation == R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED;
}

static uint32_t
test_matrix(void)
{
   uint32_t rows = 0u;
   uint32_t fast_clear_rows = 0u;
   uint32_t materialization_rows = 0u;
   static const enum r3v_native_zmask_plan_operation operations[] = {
      R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR,
      R3V_NATIVE_ZMASK_PLAN_OPERATION_STORE,
      R3V_NATIVE_ZMASK_PLAN_OPERATION_TRANSFER_READ,
   };

   for (size_t op = 0u; op < ARRAY_SIZE(operations); op++) {
      for (size_t aspect = 0u; aspect < ARRAY_SIZE(aspect_masks); aspect++) {
         for (uint32_t full = 0u; full < 2u; full++) {
            for (size_t rep = 0u; rep < ARRAY_SIZE(representations); rep++) {
               const struct r3v_native_zmask_plan_request request =
                  build_request(operations[op], aspect_masks[aspect],
                                full != 0u, representations[rep]);
               struct r3v_native_zmask_plan_obligations obligations;
               assert(r3v_native_zmask_plan_obligations_init(&request,
                                                             &obligations));

               const bool depth = (aspect_masks[aspect] &
                                   R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH) != 0u;
               const bool stencil =
                  (aspect_masks[aspect] &
                   R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL) != 0u;
               const bool writes =
                  operations[op] != R3V_NATIVE_ZMASK_PLAN_OPERATION_TRANSFER_READ;
               const bool reads =
                  operations[op] != R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR;
               assert(obligations.writes_depth == (writes && depth));
               assert(obligations.writes_stencil == (writes && stencil));
               assert(obligations.reads_depth == (reads && depth));
               assert(obligations.reads_stencil == (reads && stencil));
               assert(obligations.preserves_untouched_aspect ==
                      !(obligations.writes_depth &&
                        obligations.writes_stencil));
               assert(obligations.requires_previous_contents ==
                      (reads || obligations.preserves_untouched_aspect ||
                       full == 0u));
               assert(obligations.requires_materialization ==
                      metadata_backed(representations[rep]));

               const enum r3v_native_zmask_plan_route route =
                  r3v_native_zmask_plan_admit(&request, &obligations);
               assert(route ==
                      expected_route(operations[op], aspect_masks[aspect],
                                     full != 0u, representations[rep]));
               if (route == R3V_NATIVE_ZMASK_PLAN_ROUTE_FAST_CLEAR)
                  fast_clear_rows++;
               if (obligations.requires_materialization)
                  materialization_rows++;
               rows++;
            }
         }
      }
   }

   /* Three representations carry a ZMASK level, so exactly three of the
    * seventy-two rows admit the substitution, and half the rows owe a
    * metadata resolve. */
   assert(rows == 72u);
   assert(fast_clear_rows == 3u);
   assert(materialization_rows == 36u);
   return rows;
}

/* The field pair that separates the route from the resolve: the same
 * depth-only clear stays on the ordinary path over both representations and
 * owes materialization over the metadata-backed one alone. */
static void
test_materialization_pair(void)
{
   const struct r3v_native_zmask_plan_request tiled = build_request(
      R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR,
      R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH, true,
      R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   const struct r3v_native_zmask_plan_request fast_clear = build_request(
      R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR,
      R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH, true,
      R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
   struct r3v_native_zmask_plan_obligations tiled_obligations;
   struct r3v_native_zmask_plan_obligations fast_clear_obligations;
   assert(r3v_native_zmask_plan_obligations_init(&tiled, &tiled_obligations));
   assert(r3v_native_zmask_plan_obligations_init(&fast_clear,
                                                 &fast_clear_obligations));
   assert(r3v_native_zmask_plan_admit(&tiled, &tiled_obligations) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);
   assert(r3v_native_zmask_plan_admit(&fast_clear, &fast_clear_obligations) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);
   assert(!tiled_obligations.requires_materialization);
   assert(fast_clear_obligations.requires_materialization);
   assert(tiled_obligations.preserves_untouched_aspect);
   assert(fast_clear_obligations.preserves_untouched_aspect);
}

static void
test_malformed_requests(void)
{
   struct r3v_native_zmask_plan_obligations obligations;
   assert(!r3v_native_zmask_plan_obligations_init(NULL, &obligations));

   struct r3v_native_zmask_plan_request request = build_request(
      R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR, R300_ZB_COMBINED_CLEAR_ASPECTS,
      true, R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   assert(r3v_native_zmask_plan_obligations_init(&request, NULL) == false);

   request.aspect_mask = 0u;
   assert(!r3v_native_zmask_plan_obligations_init(&request, &obligations));
   request.aspect_mask = R300_ZB_COMBINED_CLEAR_ASPECTS | 4u;
   assert(!r3v_native_zmask_plan_obligations_init(&request, &obligations));
   assert(r3v_native_depth_plan_aspect_mask(VK_IMAGE_ASPECT_COLOR_BIT) == 0u);
   assert(r3v_native_depth_plan_aspect_mask(VK_IMAGE_ASPECT_DEPTH_BIT |
                                            VK_IMAGE_ASPECT_STENCIL_BIT) ==
          R300_ZB_COMBINED_CLEAR_ASPECTS);

   request = build_request(R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR,
                           R300_ZB_COMBINED_CLEAR_ASPECTS, true,
                           R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   request.width = 0u;
   assert(!r3v_native_zmask_plan_obligations_init(&request, &obligations));
   request.width = SURFACE + 1u;
   assert(!r3v_native_zmask_plan_obligations_init(&request, &obligations));
   request.width = SURFACE;
   request.x = SURFACE;
   assert(!r3v_native_zmask_plan_obligations_init(&request, &obligations));
   request.x = 0u;
   request.logical_height = 0u;
   assert(!r3v_native_zmask_plan_obligations_init(&request, &obligations));
}

/* Known-bad calibration: admission reads the obligation fields, so a
 * caller-asserted fast-clear shape over a stencil-only write is refused,
 * and so is a full both-aspect clear whose ZMASK layout is unadmitted. */
static void
test_known_bad_claims(void)
{
   struct r3v_native_zmask_plan_request request = build_request(
      R3V_NATIVE_ZMASK_PLAN_OPERATION_CLEAR,
      R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL, true,
      R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   struct r3v_native_zmask_plan_obligations claimed = {
      .writes_stencil = true,
      .preserves_untouched_aspect = true,
   };
   assert(r3v_native_zmask_plan_admit(&request, &claimed) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);

   claimed.preserves_untouched_aspect = false;
   assert(r3v_native_zmask_plan_admit(&request, &claimed) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);

   /* The positive arm of the same calibration: both writes and no owed
    * contents admit the substitution. */
   claimed.writes_depth = true;
   assert(r3v_native_zmask_plan_admit(&request, &claimed) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_FAST_CLEAR);

   request.layout_admitted = false;
   assert(r3v_native_zmask_plan_admit(&request, &claimed) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);
   request.layout_admitted = true;
   request.operation = R3V_NATIVE_ZMASK_PLAN_OPERATION_STORE;
   assert(r3v_native_zmask_plan_admit(&request, &claimed) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);

   assert(r3v_native_zmask_plan_admit(NULL, &claimed) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);
   assert(r3v_native_zmask_plan_admit(&request, NULL) ==
          R3V_NATIVE_ZMASK_PLAN_ROUTE_ORDINARY);
}

int
main(void)
{
   const uint32_t rows = test_matrix();
   test_materialization_pair();
   test_malformed_requests();
   test_known_bad_claims();
   printf("r3v ZMASK plan aspect obligations: OK (%u matrix rows)\n", rows);
   return 0;
}
