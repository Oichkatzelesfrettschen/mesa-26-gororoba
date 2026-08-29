/*
 * SPDX-License-Identifier: MIT
 *
 * Capacity, overflow, and first-failure controls for the PM4 writer.
 */

/* The controls below assert the writer's decisions, so a release build with
 * assertions compiled out would run them and report nothing.
 */
#undef NDEBUG

#include "r300_pm4_builder.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Every destination sits between two guard words, so a write that passes its
 * bound is visible as a changed guard rather than as memory the test happens
 * not to read.
 */
#define GUARD 0xdeadbeefu

struct guarded {
   uint32_t head_guard;
   uint32_t words[16];
   uint32_t tail_guard;
};

static void
guarded_init(struct guarded *g)
{
   memset(g, 0, sizeof(*g));
   g->head_guard = GUARD;
   g->tail_guard = GUARD;
}

static void
guards_hold(const struct guarded *g)
{
   assert(g->head_guard == GUARD);
   assert(g->tail_guard == GUARD);
}

/* A destination of zero dwords accepts nothing, and the refusal leaves the
 * count where it started.
 */
static void
test_zero_capacity_refuses(void)
{
   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, g.words, 0);

   r300_pm4_dword(&b, 0x1234);
   assert(b.error == -ENOSPC);
   assert(b.count == 0);
   assert(g.words[0] == 0);
   guards_hold(&g);

   uint32_t written = 7;
   assert(r300_pm4_builder_finish(&b, &written) == -ENOSPC);
   assert(written == 0);
}

/* The reservation admits exactly the remaining dwords and no more. */
static void
test_reserve_is_exact(void)
{
   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, g.words, 4);

   assert(r300_pm4_builder_reserve(&b, 4));
   assert(b.error == 0);
   assert(!r300_pm4_builder_reserve(&b, 5));
   assert(b.error == -ENOSPC);
}

/* count + needed evaluated as a sum wraps and admits a write past the end.
 * The reservation compares against the remaining dwords instead, so a request
 * whose sum would wrap still refuses.  Reservation stores nothing, so the
 * spoofed count below reaches the arithmetic without reaching memory.
 */
static void
test_reserve_refuses_overflow(void)
{
   struct r300_pm4_builder b;
   uint32_t sink = 0;
   r300_pm4_builder_init(&b, &sink, 1);
   b.capacity = UINT32_MAX;
   b.count = UINT32_MAX - 2;

   assert(!r300_pm4_builder_reserve(&b, 8));
   assert(b.error == -ENOSPC);
   assert(b.count == UINT32_MAX - 2);
}

/* The 14-bit header field carries count - 1, so 1..0x4000 payload dwords
 * encode and no other length does.  The bound also removes the count + 1
 * wrap: without it, UINT32_MAX + 1 reserves zero dwords, always fits, and
 * the copy takes the payload count as written.  Each refused length reports
 * -EINVAL before any reservation, so encoding and capacity stay distinct
 * causes.
 */
static uint32_t run_bound_payload[R300_PM4_MAX_RUN];
static uint32_t run_bound_dest[R300_PM4_MAX_RUN + 1];

static void
test_run_length_boundaries(void)
{
   static const struct {
      uint32_t count;
      int expected_error;
   } cases[] = {
      { 0, -EINVAL },
      { 1, 0 },
      { R300_PM4_MAX_RUN, 0 },
      { R300_PM4_MAX_RUN + 1, -EINVAL },
      { UINT32_MAX, -EINVAL },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      struct r300_pm4_builder b;

      memset(run_bound_dest, 0, sizeof(run_bound_dest));
      r300_pm4_builder_init(&b, run_bound_dest, ARRAY_SIZE(run_bound_dest));
      r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, run_bound_payload,
                       cases[i].count);
      assert(b.error == cases[i].expected_error);
      assert(b.count == (cases[i].expected_error == 0 ? cases[i].count + 1
                                                      : 0));

      memset(run_bound_dest, 0, sizeof(run_bound_dest));
      r300_pm4_builder_init(&b, run_bound_dest, ARRAY_SIZE(run_bound_dest));
      r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, run_bound_payload,
                       cases[i].count);
      assert(b.error == cases[i].expected_error);
      assert(b.count == (cases[i].expected_error == 0 ? cases[i].count + 1
                                                      : 0));
   }

   /* An encodable run that exceeds the destination refuses for capacity,
    * which keeps the two causes distinct.
    */
   struct r300_pm4_builder b;
   uint32_t small[4];
   r300_pm4_builder_init(&b, small, ARRAY_SIZE(small));
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, run_bound_payload,
                    R300_PM4_MAX_RUN);
   assert(b.error == -ENOSPC);
   assert(b.count == 0);
}

/* The first refusal is the one reported, and every later operation is a
 * no-op: a short run followed by a run that would fit must not resume.
 */
static void
test_first_failure_is_preserved(void)
{
   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, g.words, 3);

   r300_pm4_dword(&b, 0xaa);
   assert(b.count == 1);

   /* Four dwords do not fit in the two remaining. */
   const uint32_t payload[3] = { 1, 2, 3 };
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, payload, 3);
   assert(b.error == -ENOSPC);
   assert(b.count == 1);
   /* Nothing of the refused run reached the destination. */
   assert(g.words[1] == 0 && g.words[2] == 0);

   /* One dword would fit, and the builder still refuses it. */
   r300_pm4_dword(&b, 0xbb);
   assert(b.error == -ENOSPC);
   assert(b.count == 1);
   assert(g.words[1] == 0);

   /* A later malformed call does not overwrite the recorded cause. */
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, NULL, 0);
   assert(b.error == -ENOSPC);
   guards_hold(&g);
}

/* A run reserves its header and payload together, so a destination one dword
 * short takes none of it.
 */
static void
test_packet_runs_are_all_or_nothing(void)
{
   const uint32_t payload[4] = { 0x11, 0x22, 0x33, 0x44 };

   for (uint32_t capacity = 0; capacity < 5; capacity++) {
      struct guarded g;
      guarded_init(&g);
      struct r300_pm4_builder b;
      r300_pm4_builder_init(&b, g.words, capacity);

      r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, payload, 4);
      assert(b.error == -ENOSPC);
      assert(b.count == 0);
      for (unsigned i = 0; i < 5; i++)
         assert(g.words[i] == 0);
      guards_hold(&g);
   }

   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, g.words, 5);
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, payload, 4);
   assert(b.error == 0);
   assert(b.count == 5);
   assert(g.words[0] == CP_PACKET0(R300_VAP_VTX_SIZE, 3));
   assert(memcmp(&g.words[1], payload, sizeof(payload)) == 0);
   guards_hold(&g);
}

/* Both packet headers name count - 1 payload dwords, so an empty run has no
 * encoding in either form, and a null payload is malformed at any count.
 */
static void
test_packet_shapes(void)
{
   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, g.words, 16);

   const uint32_t one = 0x55;
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, &one, 0);
   assert(b.error == -EINVAL);
   assert(b.count == 0);

   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, NULL, 1);
   assert(b.error == -EINVAL);

   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, &one, 0);
   assert(b.error == -EINVAL);
   assert(b.count == 0);

   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, NULL, 2);
   assert(b.error == -EINVAL);

   /* An empty block asks for nothing and leaves the builder healthy. */
   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_block(&b, NULL, 0);
   assert(b.error == 0 && b.count == 0);
   r300_pm4_block(&b, NULL, 3);
   assert(b.error == -EINVAL);
   guards_hold(&g);
}

/* The reloc write reports the index its payload landed at, and refuses
 * whole when the two dwords do not both fit.
 */
static void
test_reloc_reports_its_site(void)
{
   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, g.words, 3);

   r300_pm4_dword(&b, 0x99);
   const uint32_t index = r300_pm4_reloc_nop(&b, 4);
   assert(index == 2);
   assert(g.words[index] == 4);
   assert(b.count == 3);

   /* One dword remains where the write needs two. */
   r300_pm4_builder_init(&b, g.words, 3);
   r300_pm4_dword(&b, 0x99);
   r300_pm4_dword(&b, 0x99);
   const uint32_t refused = r300_pm4_reloc_nop(&b, 4);
   assert(refused == R300_PM4_NO_INDEX);
   assert(b.error == -ENOSPC);
   assert(b.count == 2);
   guards_hold(&g);
}

/* A null destination is a malformed builder rather than a zero-capacity one,
 * so it reports the argument error and stores nothing.
 */
static void
test_null_destination(void)
{
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, NULL, 8);
   assert(b.error == -EINVAL);
   assert(b.capacity == 0);
   r300_pm4_dword(&b, 1);
   assert(b.error == -EINVAL);
   assert(b.count == 0);

   /* Null with zero capacity is the same malformed builder; a zero-capacity
    * probe supplies a valid pointer instead.
    */
   r300_pm4_builder_init(&b, NULL, 0);
   assert(b.error == -EINVAL);
}

/* The vertex-index-range write is one PACKET0 run over the adjacent
 * VAP_VF_MAX_VTX_INDX/VAP_VF_MIN_VTX_INDX pair, maximum first, so the
 * stream is header, max, min. An inverted pair or an index past the
 * registers' 24-bit width is -EINVAL before any reservation; a
 * destination one dword short takes none of the run.
 */
static void
test_vertex_index_range(void)
{
   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;

   /* min=0, max=2: the exact three-dword stream. */
   r300_pm4_builder_init(&b, g.words, 3);
   r300_pm4_emit_vertex_index_range(&b, 0, 2);
   assert(b.error == 0);
   assert(b.count == 3);
   assert(g.words[0] == CP_PACKET0(R300_VAP_VF_MAX_VTX_INDX, 1));
   assert(g.words[1] == 2);
   assert(g.words[2] == 0);
   guards_hold(&g);

   /* min=max=0, a value above the old count ceiling, and the largest
    * accepted maximum all encode. */
   r300_pm4_builder_init(&b, g.words, 3);
   r300_pm4_emit_vertex_index_range(&b, 0, 0);
   assert(b.error == 0 && b.count == 3);
   assert(g.words[1] == 0 && g.words[2] == 0);

   r300_pm4_builder_init(&b, g.words, 3);
   r300_pm4_emit_vertex_index_range(&b, 0x10000, 0x10000);
   assert(b.error == 0 && b.count == 3);
   assert(g.words[1] == 0x10000 && g.words[2] == 0x10000);

   r300_pm4_builder_init(&b, g.words, 3);
   r300_pm4_emit_vertex_index_range(&b, R300_PM4_VTX_INDX_LIMIT,
                                    R300_PM4_VTX_INDX_LIMIT);
   assert(b.error == 0 && b.count == 3);
   assert(g.words[1] == R300_PM4_VTX_INDX_LIMIT);
   assert(g.words[2] == R300_PM4_VTX_INDX_LIMIT);

   /* An inverted pair refuses without writing. */
   guarded_init(&g);
   r300_pm4_builder_init(&b, g.words, 3);
   r300_pm4_emit_vertex_index_range(&b, 3, 2);
   assert(b.error == -EINVAL);
   assert(b.count == 0);
   assert(g.words[0] == 0);

   /* An index past the 24-bit register width refuses. */
   r300_pm4_builder_init(&b, g.words, 3);
   r300_pm4_emit_vertex_index_range(&b, 0, R300_PM4_VTX_INDX_LIMIT + 1);
   assert(b.error == -EINVAL);
   assert(b.count == 0);

   /* One dword short takes none of the run. */
   guarded_init(&g);
   r300_pm4_builder_init(&b, g.words, 2);
   r300_pm4_emit_vertex_index_range(&b, 0, 2);
   assert(b.error == -ENOSPC);
   assert(b.count == 0);
   assert(g.words[0] == 0 && g.words[1] == 0);
   guards_hold(&g);

   /* An error-latched builder stays a no-op, and a later malformed range
    * does not overwrite the recorded cause.
    */
   r300_pm4_builder_init(&b, g.words, 0);
   r300_pm4_dword(&b, 0xaa);
   assert(b.error == -ENOSPC);
   r300_pm4_emit_vertex_index_range(&b, 3, 2);
   assert(b.error == -ENOSPC);
   assert(b.count == 0);
}

static void
test_immediate_points(void)
{
   struct guarded g;
   guarded_init(&g);
   struct r300_pm4_builder b;
   uint32_t payload[8];
   for (uint32_t i = 0; i < ARRAY_SIZE(payload); i++)
      payload[i] = 0x100u + i;

   /* Two vertices of four dwords: the exact fifteen-dword stream, with
    * the seven-dword prefix in packet order and the payload verbatim.
    */
   assert(R300_PM4_IMMEDIATE_POINTS_DWORDS(2, 4) == 15);
   r300_pm4_builder_init(&b, g.words, 15);
   r300_pm4_emit_immediate_points(&b, 2, 4, payload);
   assert(b.error == 0);
   assert(b.count == 15);
   assert(g.words[0] == CP_PACKET0(R300_VAP_VTX_SIZE, 0));
   assert(g.words[1] == 4);
   assert(g.words[2] == CP_PACKET0(R300_VAP_VF_MAX_VTX_INDX, 1));
   assert(g.words[3] == 1);
   assert(g.words[4] == 0);
   assert(g.words[5] == CP_PACKET3(R300_PACKET3_3D_DRAW_IMMD_2, 8));
   assert(g.words[6] == (R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED |
                         (2u << 16) | R300_VAP_VF_CNTL__PRIM_POINTS));
   assert(memcmp(&g.words[7], payload, sizeof(payload)) == 0);
   guards_hold(&g);

   /* The single-vertex, single-dword minimum encodes. */
   r300_pm4_builder_init(&b, g.words, 8);
   r300_pm4_emit_immediate_points(&b, 1, 1, payload);
   assert(b.error == 0 && b.count == 8);
   assert(g.words[5] == CP_PACKET3(R300_PACKET3_3D_DRAW_IMMD_2, 1));
   assert(g.words[7] == payload[0]);

   /* Zero vertices, a vertex count past the index registers, zero vertex
    * dwords, a body past the PACKET3 14-bit field, and a null payload
    * each refuse without writing.
    */
   guarded_init(&g);
   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_emit_immediate_points(&b, 0, 4, payload);
   assert(b.error == -EINVAL && b.count == 0 && g.words[0] == 0);
   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_emit_immediate_points(&b, R300_PM4_VTX_COUNT_LIMIT + 1, 4,
                                  payload);
   assert(b.error == -EINVAL && b.count == 0);
   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_emit_immediate_points(&b, 2, 0, payload);
   assert(b.error == -EINVAL && b.count == 0);
   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_emit_immediate_points(&b, 0x3fff, 2, payload);
   assert(b.error == -EINVAL && b.count == 0);
   r300_pm4_builder_init(&b, g.words, 16);
   r300_pm4_emit_immediate_points(&b, 2, 4, NULL);
   assert(b.error == -EINVAL && b.count == 0 && g.words[0] == 0);
   guards_hold(&g);

   /* One dword short of the body latches -ENOSPC and finish publishes
    * nothing.
    */
   r300_pm4_builder_init(&b, g.words, 14);
   r300_pm4_emit_immediate_points(&b, 2, 4, payload);
   assert(b.error == -ENOSPC);
   assert(b.count == 0);
   for (unsigned i = 0; i < ARRAY_SIZE(g.words); i++)
      assert(g.words[i] == 0);
   uint32_t published = 0xffffffffu;
   assert(r300_pm4_builder_finish(&b, &published) == -ENOSPC);
   assert(published == 0);
   guards_hold(&g);

   /* An error-latched builder stays a no-op. */
   r300_pm4_builder_init(&b, g.words, 0);
   r300_pm4_dword(&b, 0xaa);
   assert(b.error == -ENOSPC);
   r300_pm4_emit_immediate_points(&b, 2, 4, payload);
   assert(b.error == -ENOSPC && b.count == 0);
}

int
main(void)
{
   test_vertex_index_range();
   test_immediate_points();
   test_zero_capacity_refuses();
   test_reserve_is_exact();
   test_reserve_refuses_overflow();
   test_run_length_boundaries();
   test_first_failure_is_preserved();
   test_packet_runs_are_all_or_nothing();
   test_packet_shapes();
   test_reloc_reports_its_site();
   test_null_destination();

   printf("r300_pm4_builder_test: all controls held\n");
   return 0;
}
