/*
 * SPDX-License-Identifier: MIT
 *
 * Host test for the fixed TCL-bypass triangle cell: the emitted stream
 * satisfies the kernel's vertex-output decision contract, the relocation
 * sites bind the right slots, and emission is deterministic.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PKT0(reg, count) ((((count) - 1) << 16) | ((reg) >> 2))

/* Mirror of the tracking the kernel replay tool keeps over the stream. */
struct tracker {
   uint32_t fmt0, fmt1, vtx_size, cntl_status;
   bool fmt0_seen, fmt1_seen, vtx_size_seen, cntl_status_seen;
   uint8_t ext_seen_mask;
   bool ext_nonidentity;
   uint32_t draw_vf_cntl;
   unsigned draw_count;
   unsigned reloc_nop_count;
};

static void
track(struct tracker *t, const uint32_t *ib, uint32_t count)
{
   uint32_t i = 0;
   while (i < count) {
      uint32_t header = ib[i];
      uint32_t type = header >> 30;
      uint32_t payload = ((header >> 16) & 0x3fff) + 1;
      if (type == 0) {
         uint32_t reg = (header & 0xffff) << 2;
         for (uint32_t d = 0; d < payload; d++) {
            uint32_t value = ib[i + 1 + d];
            uint32_t r = reg + 4 * d;
            if (r == 0x2090) {
               t->fmt0 = value;
               t->fmt0_seen = true;
            } else if (r == 0x2094) {
               t->fmt1 = value;
               t->fmt1_seen = true;
            } else if (r == 0x20B4) {
               t->vtx_size = value & 0x7F;
               t->vtx_size_seen = true;
            } else if (r == 0x2140) {
               t->cntl_status = value;
               t->cntl_status_seen = true;
            } else if (r >= 0x21E0 && r <= 0x21FC) {
               if (value != 0xF688F688u)
                  t->ext_nonidentity = true;
               t->ext_seen_mask |= 1u << ((r - 0x21E0) >> 2);
            }
         }
      } else if (type == 3) {
         uint32_t op = header & 0xff00;
         if (op == 0x3400) {
            t->draw_vf_cntl = ib[i + 1];
            t->draw_count++;
         } else if (op == 0x1000) {
            t->reloc_nop_count++;
         }
      } else {
         assert(!"unexpected packet type");
      }
      i += 1 + payload;
   }
   assert(i == count);
}

static void
make_cell(struct r300_fragment_binary *fs,
          struct r300_tcl_bypass_triangle_ib *cell)
{
   /* A minimal structurally valid fragment block: two US register writes. */
   static const uint32_t fs_stream[] = {
      PKT0(0x4600, 1), 0x0,
      PKT0(0x4604, 1), 0x0,
   };
   assert(r300_fragment_binary_init(fs, fs_stream,
                                    sizeof(fs_stream) / sizeof(uint32_t), 0,
                                    0, "test") == 0);

   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(64),
   };
   params.fragment_binary = fs;
   assert(r300_tcl_bypass_triangle_emit(&params, cell) == 0);
}

static void
test_stream_satisfies_kernel_contract(void)
{
   struct r300_fragment_binary fs;
   struct r300_tcl_bypass_triangle_ib cell;
   make_cell(&fs, &cell);

   struct tracker t;
   memset(&t, 0, sizeof(t));
   track(&t, cell.ib, cell.ib_size_dwords);

   /* The exact premise set of the kernel's TCL-bypass vertex-output check:
    * bypass on, position-only output declarations, VAP_VTX_SIZE = 4, all
    * eight extended PSC selectors identity, one non-immediate draw.
    */
   assert(t.cntl_status_seen && (t.cntl_status & (1u << 8)));
   assert(t.fmt0_seen && t.fmt0 == 0x1);
   assert(t.fmt1_seen && t.fmt1 == 0);
   assert(t.vtx_size_seen && t.vtx_size == 4);
   assert(t.ext_seen_mask == 0xff && !t.ext_nonidentity);
   assert(t.draw_count == 1);
   assert(((t.draw_vf_cntl >> 4) & 0x3) == 2);
   assert((t.draw_vf_cntl >> 16) == 3);

   r300_tcl_bypass_triangle_release(&cell);
   r300_fragment_binary_finish(&fs);
}

static void
test_reloc_sites_bind_slots(void)
{
   struct r300_fragment_binary fs;
   struct r300_tcl_bypass_triangle_ib cell;
   make_cell(&fs, &cell);

   struct tracker t;
   memset(&t, 0, sizeof(t));
   track(&t, cell.ib, cell.ib_size_dwords);

   assert(cell.reloc_site_count == 2);
   assert(t.reloc_nop_count == 2);
   /* Color reference precedes the vertex reference in the stream, and each
    * payload names its slot in relocation-chunk dword units.
    */
   assert(cell.reloc_sites[0].slot == R300_TRIANGLE_SLOT_COLOR);
   assert(cell.reloc_sites[1].slot == R300_TRIANGLE_SLOT_VERTEX);
   for (unsigned i = 0; i < cell.reloc_site_count; i++) {
      uint32_t index = cell.reloc_sites[i].ib_index;
      assert(index < cell.ib_size_dwords);
      assert(cell.ib[index] == cell.reloc_sites[i].slot * 4);
      assert(cell.ib[index - 1] == 0xC0001000u);
   }

   r300_tcl_bypass_triangle_release(&cell);
   r300_fragment_binary_finish(&fs);
}

static void
test_emission_is_deterministic(void)
{
   struct r300_fragment_binary fs_a, fs_b;
   struct r300_tcl_bypass_triangle_ib cell_a, cell_b;
   make_cell(&fs_a, &cell_a);
   make_cell(&fs_b, &cell_b);

   assert(cell_a.ib_size_dwords == cell_b.ib_size_dwords);
   assert(memcmp(cell_a.ib, cell_b.ib,
                 cell_a.ib_size_dwords * sizeof(uint32_t)) == 0);

   r300_tcl_bypass_triangle_release(&cell_a);
   r300_tcl_bypass_triangle_release(&cell_b);
   r300_fragment_binary_finish(&fs_a);
   r300_fragment_binary_finish(&fs_b);
}

static void
test_contract_emission_is_self_contained(void)
{
   /* The reference-contract cell still satisfies the kernel's TCL-bypass
    * vertex-output contract, and its stream establishes every first-draw
    * clause itself from an all-zero predecessor -- the state under which
    * the three proven color-write gates all suppress output.
    */
   struct r300_fragment_binary fs;
   static const uint32_t fs_stream[] = {
      PKT0(0x4600, 1), 0x0,
      PKT0(0x4604, 1), 0x0,
   };
   assert(r300_fragment_binary_init(&fs, fs_stream,
                                    sizeof(fs_stream) / sizeof(uint32_t), 0,
                                    0, "test") == 0);

   struct r300_first_draw_contract contract;
   assert(r300_tcl_bypass_triangle_reference_contract(&contract) == 0);

   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(64),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_emit(&params, &cell) == 0);

   struct tracker t;
   memset(&t, 0, sizeof(t));
   track(&t, cell.ib, cell.ib_size_dwords);
   assert(t.cntl_status_seen && (t.cntl_status & (1u << 8)));
   assert(t.vtx_size_seen && t.vtx_size == 4);
   assert(t.ext_seen_mask == 0xff && !t.ext_nonidentity);
   assert(t.draw_count == 1);

   struct r300_first_draw_check_report report;
   assert(r300_first_draw_state_check(&contract, cell.ib,
                                      cell.ib_size_dwords, 0x00000000,
                                      &report) == 0);

   r300_tcl_bypass_triangle_release(&cell);
   r300_fragment_binary_finish(&fs);
}

static void
test_emit_rejects_unvalidated_binary(void)
{
   struct r300_tcl_bypass_triangle_ib cell;
   struct r300_tcl_bypass_triangle_params params = {0};
   assert(r300_tcl_bypass_triangle_emit(&params, &cell) == -EINVAL);

   struct r300_fragment_binary unvalidated;
   memset(&unvalidated, 0, sizeof(unvalidated));
   params.fragment_binary = &unvalidated;
   assert(r300_tcl_bypass_triangle_emit(&params, &cell) == -EINVAL);
}

static void
test_colorpitch0_pack(void)
{
   /* 64 pixels, linear little-endian ARGB8888: pitch field 64 plus the
    * ARGB8888 format select (6 << 21).
    */
   assert(r300_rb3d_colorpitch0_pack_argb8888(64) == 0x00c00040u);
   assert(r300_rb3d_colorpitch0_pack_argb8888(0) == 0);
   assert(r300_rb3d_colorpitch0_pack_argb8888(63) == 0);
   assert(r300_rb3d_colorpitch0_pack_argb8888(0x4000) == 0);
}

static void
test_output_oracle(void)
{
   enum { PIXELS = 64 * 65 };
   static uint32_t target[PIXELS];
   struct r300_triangle_oracle_verdict verdict;

   /* Untouched sentinel target: no execution evidence, boundaries hold. */
   for (unsigned i = 0; i < PIXELS; i++)
      target[i] = R300_TRIANGLE_COLOR_SENTINEL;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(!verdict.executed && !verdict.interior_pass &&
          verdict.exterior_pass && verdict.canary_pass);

   /* A painted block covering every interior sample point carries the
    * draw color while the exterior samples stay sentinel.
    */
   for (unsigned y = 9; y < 55; y++)
      for (unsigned x = 10; x < 54; x++)
         target[y * 64 + x] = R300_TRIANGLE_DRAW_COLOR_ARGB8888;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(verdict.executed && verdict.interior_pass);

   /* Wrong interior color fails the interior check. */
   target[20 * 64 + 32] = 0xffff0000u;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(!verdict.interior_pass);
   target[20 * 64 + 32] = R300_TRIANGLE_DRAW_COLOR_ARGB8888;

   /* A stray exterior write and a canary-row write each fail closed. */
   target[0] = R300_TRIANGLE_DRAW_COLOR_ARGB8888;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(!verdict.exterior_pass);
   target[0] = R300_TRIANGLE_COLOR_SENTINEL;

   target[64 * 64 + 5] = 0x00000001u;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(!verdict.canary_pass);
}

int
main(void)
{
   test_stream_satisfies_kernel_contract();
   test_reloc_sites_bind_slots();
   test_emission_is_deterministic();
   test_contract_emission_is_self_contained();
   test_emit_rejects_unvalidated_binary();
   test_colorpitch0_pack();
   test_output_oracle();
   printf("r300_tcl_bypass_triangle_test: all checks passed\n");
   return 0;
}
