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

/* A run is one dword longer than its payload, so a payload at UINT32_MAX has
 * no representable run length: adding one wraps to zero, a zero-dword
 * reservation always fits, and the copy that follows would take the payload
 * count as written.  Both packet forms refuse that length before reserving.
 */
static void
test_run_length_refuses_overflow(void)
{
   struct guarded g;
   const uint32_t payload[1] = { 0x77 };

   guarded_init(&g);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, g.words, ARRAY_SIZE(g.words));
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, payload, UINT32_MAX);
   assert(b.error == -EINVAL);
   assert(b.count == 0);
   assert(g.words[0] == 0);
   guards_hold(&g);

   guarded_init(&g);
   r300_pm4_builder_init(&b, g.words, ARRAY_SIZE(g.words));
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, payload, UINT32_MAX);
   assert(b.error == -EINVAL);
   assert(b.count == 0);
   assert(g.words[0] == 0);
   guards_hold(&g);

   /* One below that length is representable, so it refuses for capacity
    * rather than for the encoding, which keeps the two causes distinct.
    */
   guarded_init(&g);
   r300_pm4_builder_init(&b, g.words, ARRAY_SIZE(g.words));
   r300_pm4_packet0(&b, R300_VAP_VTX_SIZE, payload, UINT32_MAX - 1);
   assert(b.error == -ENOSPC);
   assert(b.count == 0);
   guards_hold(&g);
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

/* PACKET0 encodes count - 1, so an empty run has no encoding; PACKET3 encodes
 * an empty payload as zero, so a header alone is a packet.
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
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, NULL, 0);
   assert(b.error == 0);
   assert(b.count == 1);
   assert(g.words[0] == CP_PACKET3(R300_PACKET3_3D_DRAW_VBUF_2, 0));

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
}

int
main(void)
{
   test_zero_capacity_refuses();
   test_reserve_is_exact();
   test_reserve_refuses_overflow();
   test_run_length_refuses_overflow();
   test_first_failure_is_preserved();
   test_packet_runs_are_all_or_nothing();
   test_packet_shapes();
   test_reloc_reports_its_site();
   test_null_destination();

   printf("r300_pm4_builder_test: all controls held\n");
   return 0;
}
