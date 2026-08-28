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
#include "r300_pm4_builder.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_us_source_read.h"
#include "tests/r300_retained_route_digests.h"
#include "tests/r300_varying_cell_digests.h"

#include "util/macros.h"
#include "util/mesa-blake3.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKT0(reg, count) ((((count) - 1) << 16) | ((reg) >> 2))

/* Mirror of the tracking the kernel replay tool keeps over the stream. */
struct tracker {
   uint32_t fmt0, fmt1, vtx_size, cntl_status;
   bool fmt0_seen, fmt1_seen, vtx_size_seen, cntl_status_seen;
   uint32_t vte_cntl, color_channel_mask, max_vtx_index, min_vtx_index;
   bool vte_cntl_seen, color_channel_mask_seen, max_vtx_index_seen,
      min_vtx_index_seen;
   uint8_t ext_seen_mask;
   bool ext_nonidentity;
   uint32_t draw_vf_cntl;
   unsigned draw_count;
   unsigned reloc_nop_count;   /* The varying cell's additional registers and the vertex-array word. */
   uint32_t psc0, vsm_vtx_assm, rs_count, rs_inst_count, rs_ip0, rs_inst0;
   bool psc0_seen, vsm_vtx_assm_seen, rs_count_seen, rs_inst_count_seen,
      rs_ip0_seen, rs_inst0_seen;
   uint32_t vbpntr_size_stride;
   bool vbpntr_seen;
   /* The sampled cell's TX unit-0 registers. */
   uint32_t tx_enable, tx_filter0, tx_format0, tx_format1, tx_format2,
      tx_offset;
   bool tx_enable_seen, tx_filter0_seen, tx_format0_seen, tx_format1_seen,
      tx_format2_seen, tx_offset_seen;
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
            } else if (r == R300_VAP_VTE_CNTL) {
               t->vte_cntl = value;
               t->vte_cntl_seen = true;
            } else if (r == RB3D_COLOR_CHANNEL_MASK) {
               t->color_channel_mask = value;
               t->color_channel_mask_seen = true;
            } else if (r == R300_VAP_VF_MAX_VTX_INDX) {
               t->max_vtx_index = value;
               t->max_vtx_index_seen = true;
            } else if (r == R300_VAP_VF_MIN_VTX_INDX) {
               t->min_vtx_index = value;
               t->min_vtx_index_seen = true;
            } else if (r >= 0x21E0 && r <= 0x21FC) {
               if (value != 0xF688F688u)
                  t->ext_nonidentity = true;
               t->ext_seen_mask |= 1u << ((r - 0x21E0) >> 2);
            } else if (r == R300_VAP_PROG_STREAM_CNTL_0) {
               t->psc0 = value;
               t->psc0_seen = true;
            } else if (r == R300_VAP_VSM_VTX_ASSM) {
               t->vsm_vtx_assm = value;
               t->vsm_vtx_assm_seen = true;
            } else if (r == R300_RS_COUNT) {
               t->rs_count = value;
               t->rs_count_seen = true;
            } else if (r == R300_RS_INST_COUNT) {
               t->rs_inst_count = value;
               t->rs_inst_count_seen = true;
            } else if (r == R300_RS_IP_0) {
               t->rs_ip0 = value;
               t->rs_ip0_seen = true;
            } else if (r == R300_RS_INST_0) {
               t->rs_inst0 = value;
               t->rs_inst0_seen = true;
            } else if (r == R300_TX_ENABLE) {
               t->tx_enable = value;
               t->tx_enable_seen = true;
            } else if (r == R300_TX_FILTER0_0) {
               t->tx_filter0 = value;
               t->tx_filter0_seen = true;
            } else if (r == R300_TX_FORMAT0_0) {
               t->tx_format0 = value;
               t->tx_format0_seen = true;
            } else if (r == R300_TX_FORMAT1_0) {
               t->tx_format1 = value;
               t->tx_format1_seen = true;
            } else if (r == R300_TX_FORMAT2_0) {
               t->tx_format2 = value;
               t->tx_format2_seen = true;
            } else if (r == R300_TX_OFFSET_0) {
               t->tx_offset = value;
               t->tx_offset_seen = true;
            }
         }
      } else if (type == 3) {
         uint32_t op = header & 0xff00;
         if (op == 0x3400) {
            t->draw_vf_cntl = ib[i + 1];
            t->draw_count++;
         } else if (op == 0x1000) {
            t->reloc_nop_count++;
         } else if (op == R300_PACKET3_3D_LOAD_VBPNTR) {
            t->vbpntr_size_stride = ib[i + 2];
            t->vbpntr_seen = true;
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
   assert(t.vte_cntl_seen && t.vte_cntl == (R300_VTX_XY_FMT | R300_VTX_Z_FMT));
   assert(t.color_channel_mask_seen &&
          t.color_channel_mask ==
             (RB3D_COLOR_CHANNEL_MASK_BLUE_MASK0 |
              RB3D_COLOR_CHANNEL_MASK_GREEN_MASK0 |
              RB3D_COLOR_CHANNEL_MASK_RED_MASK0 |
              RB3D_COLOR_CHANNEL_MASK_ALPHA_MASK0));
   assert(t.max_vtx_index_seen && t.max_vtx_index == 2);
   assert(t.min_vtx_index_seen && t.min_vtx_index == 0);
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

/* The checks the site validator proves against the stream itself: a site is
 * the payload of a relocation NOP, so index zero and a corrupted header each
 * refuse, and the site indices follow the stream order.  An in-range
 * ordinary dword coincidentally equal to the payload satisfies none of them.
 */
static void
test_reloc_site_validator_refuses_each_defect(void)
{
   struct r300_fragment_binary fs;
   struct r300_tcl_bypass_triangle_ib cell;
   make_cell(&fs, &cell);
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == 0);

   /* Corrupting the NOP header alone refuses the otherwise-intact site. */
   const uint32_t header_index = cell.reloc_sites[0].ib_index - 1;
   const uint32_t saved_header = cell.ib[header_index];
   cell.ib[header_index] ^= 1u;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == -EINVAL);
   cell.ib[header_index] = saved_header;

   /* Index zero has no preceding dword to hold the header. */
   const uint32_t saved_index = cell.reloc_sites[0].ib_index;
   cell.reloc_sites[0].ib_index = 0;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == -ERANGE);
   cell.reloc_sites[0].ib_index = saved_index;

   /* Site indices that do not increase contradict the stream order even
    * when each site is individually intact.
    */
   struct r300_tcl_bypass_triangle_ib m = cell;
   m.reloc_sites[0].ib_index = cell.reloc_sites[1].ib_index;
   m.reloc_sites[1].ib_index = cell.reloc_sites[0].ib_index;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) != 0);

   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == 0);

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

   assert(R300_TRIANGLE_DRAW_COLOR_B8G8R8A8 == 0xdf20609fu);

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
         target[y * 64 + x] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(verdict.executed && verdict.interior_pass);

   /* A red/blue byte swap fails the interior check; the corrupted pixel is
    * the analytic centroid, a sample at every extent whose triangle carries
    * the fill-rule margin.  The expected dword has distinct bytes, so this
    * mutation proves the oracle observes channel order rather than merely a
    * non-sentinel write.
    */
   const uint32_t red_blue_swapped =
      (R300_TRIANGLE_DRAW_COLOR_B8G8R8A8 & 0xff000000u) |
      (R300_TRIANGLE_DRAW_COLOR_B8G8R8A8 & 0x0000ff00u) |
      ((R300_TRIANGLE_DRAW_COLOR_B8G8R8A8 & 0x000000ffu) << 16) |
      ((R300_TRIANGLE_DRAW_COLOR_B8G8R8A8 & 0x00ff0000u) >> 16);
   assert(red_blue_swapped != R300_TRIANGLE_DRAW_COLOR_B8G8R8A8);
   target[24 * 64 + 32] = red_blue_swapped;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(!verdict.interior_pass);
   target[24 * 64 + 32] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;

   /* A stray exterior write and a canary-row write each fail closed. */
   target[0] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(!verdict.exterior_pass);
   target[0] = R300_TRIANGLE_COLOR_SENTINEL;

   target[64 * 64 + 5] = 0x00000001u;
   r300_tcl_bypass_triangle_oracle(target, sizeof(target), &verdict);
   assert(!verdict.canary_pass);
}

/* Each rectangle covers the triangle's full vertex bounding box, paints every
 * existing interior sample, and leaves the extent boundary and canary clean.
 * The parameterized extents calibrate the bounding-box exterior samples at
 * both the reference size and a small valid extent.
 */
static void
test_output_oracle_rejects_bounding_box_overdraw(void)
{
   enum { PIXELS = 64 * 65 };
   static uint32_t target[PIXELS];
   struct r300_triangle_oracle_verdict verdict;
   static const struct {
      uint32_t width, height;
      uint32_t min_x, max_x, min_y, max_y;
   } cases[] = {
      {64, 64, 8, 56, 8, 56},
      {9, 10, 1, 7, 1, 8},
   };

   for (unsigned case_index = 0; case_index < ARRAY_SIZE(cases);
        case_index++) {
      for (unsigned i = 0; i < PIXELS; i++)
         target[i] = R300_TRIANGLE_COLOR_SENTINEL;
      for (uint32_t y = cases[case_index].min_y;
           y <= cases[case_index].max_y; y++) {
         for (uint32_t x = cases[case_index].min_x;
              x <= cases[case_index].max_x; x++)
            target[y * 64 + x] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
      }

      r300_tcl_bypass_triangle_extent_oracle(
         cases[case_index].width, cases[case_index].height, target,
         sizeof(target), &verdict);
      assert(verdict.executed && verdict.interior_pass);
      assert(!verdict.exterior_pass && verdict.exterior_samples >= 2);
      assert(verdict.canary_pass);
   }
}

/* The extent oracle's calibration: a synthesized correct 48x20 witness
 * passes with positive sample counts, each corruption class fails its
 * verdict, and an extent whose triangle cannot carry the fill-rule
 * margin fails closed with zero interior samples.
 */
static void
test_extent_oracle_calibration(void)
{
   enum { PITCH = 64 };
   const uint32_t width = 48, height = 20;
   static uint32_t target[PITCH * 21];
   struct r300_triangle_oracle_verdict verdict;

   /* The analytic triangle at 48x20: the NDC reference through the
    * viewport transform, rasterized at pixel centers.
    */
   const float v[6] = { 6.0f, 2.5f, 42.0f, 2.5f, 24.0f, 17.5f };
   for (unsigned i = 0; i < PITCH * 21; i++)
      target[i] = R300_TRIANGLE_COLOR_SENTINEL;
   for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         const float e0 = (v[2] - v[0]) * (py - v[1]) -
                          (v[3] - v[1]) * (px - v[0]);
         const float e1 = (v[4] - v[2]) * (py - v[3]) -
                          (v[5] - v[3]) * (px - v[2]);
         const float e2 = (v[0] - v[4]) * (py - v[5]) -
                          (v[1] - v[5]) * (px - v[4]);
         if (e0 > 0.0f && e1 > 0.0f && e2 > 0.0f)
            target[y * PITCH + x] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
      }
   }
   r300_tcl_bypass_triangle_extent_oracle(width, height, target,
                                          sizeof(target), &verdict);
   assert(verdict.executed && verdict.interior_pass &&
          verdict.exterior_pass && verdict.canary_pass);
   assert(verdict.interior_samples > 0 && verdict.exterior_samples > 0);

   /* The oracle admits the exact retained footprint,
    * pitch * (height + 1) pixels, and refuses anything shorter: a
    * buffer holding only the render rows carries no observable canary
    * band, so the truncated calls fail every pass with zero samples
    * instead of leaving canary_pass vacuously true.
    */
   const uint32_t required_bytes = PITCH * (height + 1) * 4;
   r300_tcl_bypass_triangle_extent_oracle(width, height, target,
                                          required_bytes, &verdict);
   assert(verdict.executed && verdict.interior_pass &&
          verdict.exterior_pass && verdict.canary_pass);
   r300_tcl_bypass_triangle_extent_oracle(width, height, target,
                                          PITCH * height * 4, &verdict);
   assert(!verdict.executed && !verdict.interior_pass &&
          !verdict.exterior_pass && !verdict.canary_pass &&
          verdict.interior_samples == 0 && verdict.exterior_samples == 0);
   r300_tcl_bypass_triangle_extent_oracle(width, height, target,
                                          required_bytes - 4, &verdict);
   assert(!verdict.executed && !verdict.canary_pass &&
          verdict.interior_samples == 0);

   /* A write in the sub-pitch padding band fails the canary. */
   target[5 * PITCH + 50] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
   r300_tcl_bypass_triangle_extent_oracle(width, height, target,
                                          sizeof(target), &verdict);
   assert(!verdict.canary_pass);
   target[5 * PITCH + 50] = R300_TRIANGLE_COLOR_SENTINEL;

   /* A write past the render extent fails the canary. */
   target[height * PITCH + 3] = 0x00000001u;
   r300_tcl_bypass_triangle_extent_oracle(width, height, target,
                                          sizeof(target), &verdict);
   assert(!verdict.canary_pass);
   target[height * PITCH + 3] = R300_TRIANGLE_COLOR_SENTINEL;

   /* An untouched target reports no execution and no interior. */
   for (unsigned i = 0; i < PITCH * 21; i++)
      target[i] = R300_TRIANGLE_COLOR_SENTINEL;
   r300_tcl_bypass_triangle_extent_oracle(width, height, target,
                                          sizeof(target), &verdict);
   assert(!verdict.executed && !verdict.interior_pass);

   /* A 1x1 extent's triangle carries no fill-rule margin, so the
    * oracle fails closed with zero interior samples rather than
    * passing vacuously.
    */
   r300_tcl_bypass_triangle_extent_oracle(1, 1, target, 2 * PITCH * 4,
                                          &verdict);
   assert(verdict.interior_samples == 0 && !verdict.interior_pass);

   /* An extent outside the emitter's domain fails every pass with zero
    * samples.
    */
   r300_tcl_bypass_triangle_extent_oracle(0, 20, target, sizeof(target),
                                          &verdict);
   assert(!verdict.interior_pass && !verdict.exterior_pass &&
          !verdict.canary_pass && verdict.interior_samples == 0);
   r300_tcl_bypass_triangle_extent_oracle(65, 20, target, sizeof(target),
                                          &verdict);
   assert(!verdict.interior_pass && !verdict.exterior_pass &&
          !verdict.canary_pass && verdict.interior_samples == 0);
}

/* One reference construction backs every fixed-cell authority: the
 * reference emission equals the explicit fs-plus-contract composition
 * byte for byte, repeats deterministically, and differs from the bare
 * cell -- so an authority that hashed the bare cell would fail the
 * digest comparison instead of falsely reporting armed.
 */
static void
test_reference_emit_is_the_single_authority(void)
{
   struct r300_tcl_bypass_triangle_ib ref;
   assert(r300_tcl_bypass_triangle_reference_emit(&ref) == 0);

   struct r300_tcl_bypass_triangle_ib again;
   assert(r300_tcl_bypass_triangle_reference_emit(&again) == 0);
   assert(again.ib_size_dwords == ref.ib_size_dwords);
   assert(memcmp(again.ib, ref.ib,
                 ref.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_tcl_bypass_triangle_release(&again);

   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_first_draw_contract contract;
   assert(r300_tcl_bypass_triangle_reference_contract(&contract) == 0);
   bool format_seen = false;
   for (uint32_t i = 0; i < contract.count; i++) {
      if (contract.entries[i].reg == R300_US_OUT_FMT_0) {
         assert(!format_seen);
         assert(contract.entries[i].value ==
                (R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G |
                 R300_C2_SEL_R | R300_C3_SEL_A));
         format_seen = true;
      }
   }
   assert(format_seen);
   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(64),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   struct r300_tcl_bypass_triangle_ib composed;
   assert(r300_tcl_bypass_triangle_emit(&params, &composed) == 0);
   assert(composed.ib_size_dwords == ref.ib_size_dwords);
   assert(memcmp(composed.ib, ref.ib,
                 ref.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_tcl_bypass_triangle_release(&composed);

   /* The bare cell is the retained negative control: shorter, and not a
    * prefix-extension equal of the reference bytes.
    */
   params.first_draw_contract = NULL;
   struct r300_tcl_bypass_triangle_ib bare;
   assert(r300_tcl_bypass_triangle_emit(&params, &bare) == 0);
   assert(bare.ib_size_dwords < ref.ib_size_dwords);
   assert(memcmp(bare.ib, ref.ib,
                 bare.ib_size_dwords * sizeof(uint32_t)) != 0);
   r300_tcl_bypass_triangle_release(&bare);

   r300_fragment_binary_finish(&fs);
   r300_tcl_bypass_triangle_release(&ref);
}

/* The contract cell's exact size and content, pinned to the retained
 * silicon identity so a change to the emitter, the contract, or the
 * fragment binary reports as a size or digest movement rather than as a
 * silently different cell.  The digest is the one the staging manifest and
 * the arming gate carry; it hashes the little-endian dword stream the
 * manifest writes and the kernel parser reads, independent of host byte
 * order.
 */
#define R300_TRIANGLE_CONTRACT_CELL_DWORDS R300_RETAINED_CPU_ROUTE_IB_DWORDS

static void
test_contract_cell_size_and_digest_are_pinned(void)
{
   struct r300_tcl_bypass_triangle_ib ref;
   assert(r300_tcl_bypass_triangle_reference_emit(&ref) == 0);
   assert(ref.ib_size_dwords == R300_TRIANGLE_CONTRACT_CELL_DWORDS);

   char digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(ref.ib, ref.ib_size_dwords, digest);
   assert(strcmp(digest, R300_RETAINED_CPU_ROUTE_IB_BLAKE3) == 0);

   r300_tcl_bypass_triangle_release(&ref);
}

/* Emission into caller storage takes the whole cell or none of it.  Every
 * capacity below the cell's size refuses, which covers a destination one
 * dword short of the contract prefix, of the fragment block, and of every
 * packet between them.  Guard words on both sides of the destination make a
 * write past the bound visible.
 */
static void
test_emit_into_holds_its_capacity(void)
{
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_first_draw_contract contract;
   assert(r300_tcl_bypass_triangle_reference_contract(&contract) == 0);
   const struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(64),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };

   enum { SLACK = 4 };
   static uint32_t storage[1 + R300_TRIANGLE_CONTRACT_CELL_DWORDS + SLACK + 1];
   uint32_t *const words = &storage[1];
   const uint32_t word_count = ARRAY_SIZE(storage) - 2;

   for (uint32_t capacity = 0;
        capacity <= R300_TRIANGLE_CONTRACT_CELL_DWORDS + SLACK; capacity++) {
      for (uint32_t i = 0; i < ARRAY_SIZE(storage); i++)
         storage[i] = 0xdeadbeefu;
      storage[0] = 0xdeadbeefu;
      storage[ARRAY_SIZE(storage) - 1] = 0xdeadbeefu;

      struct r300_tcl_bypass_triangle_ib cell;
      const int rc =
         r300_tcl_bypass_triangle_emit_into(&params, words, capacity, &cell);

      if (capacity < R300_TRIANGLE_CONTRACT_CELL_DWORDS) {
         assert(rc == -ENOSPC);
         assert(cell.ib_size_dwords == 0);
         assert(cell.ib == NULL);
      } else {
         assert(rc == 0);
         assert(cell.ib_size_dwords == R300_TRIANGLE_CONTRACT_CELL_DWORDS);
         assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == 0);
      }

      assert(storage[0] == 0xdeadbeefu);
      assert(storage[ARRAY_SIZE(storage) - 1] == 0xdeadbeefu);
      /* The dwords beginning at the requested capacity stay untouched.  A
       * refusal that writes past its requested destination changes this guard
       * range even when the write stops before the complete cell size.
       */
      for (uint32_t i = capacity; i < word_count; i++)
         assert(words[i] == 0xdeadbeefu);
   }

   r300_fragment_binary_finish(&fs);
}

/* The emitted cell writes into caller storage without claiming it, so the
 * release leaves that storage alone and frees only what the allocating form
 * owns.
 */
static void
test_emit_into_does_not_own_caller_storage(void)
{
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_first_draw_contract contract;
   assert(r300_tcl_bypass_triangle_reference_contract(&contract) == 0);
   const struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(64),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };

   static uint32_t words[R300_TRIANGLE_CONTRACT_CELL_DWORDS];
   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_emit_into(&params, words,
                                             ARRAY_SIZE(words), &cell) == 0);
   assert(!cell.owns_ib);
   r300_tcl_bypass_triangle_release(&cell);

   struct r300_tcl_bypass_triangle_ib owned;
   assert(r300_tcl_bypass_triangle_reference_emit(&owned) == 0);
   assert(owned.owns_ib);
   /* Both forms produce the same stream. */
   assert(owned.ib_size_dwords == R300_TRIANGLE_CONTRACT_CELL_DWORDS);
   assert(memcmp(owned.ib, words, sizeof(words)) == 0);
   r300_tcl_bypass_triangle_release(&owned);

   r300_fragment_binary_finish(&fs);
}

/* Each way a relocation site can misname the stream it indexes.  The emitted
 * sites validate, and every mutation of them is refused, so the validator
 * decides on the site rather than on the cell having been emitted.
 */
static void
test_reloc_site_mutations_refuse(void)
{
   struct r300_fragment_binary fs;
   struct r300_tcl_bypass_triangle_ib cell;
   make_cell(&fs, &cell);
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == 0);

   struct r300_tcl_bypass_triangle_ib m;

   /* A site index at the end of the stream indexes no dword in it. */
   m = cell;
   m.reloc_sites[0].ib_index = cell.ib_size_dwords;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) == -ERANGE);

   /* A slot equal to the slot count names no BO the transport binds. */
   m = cell;
   m.reloc_sites[1].slot = R300_TRIANGLE_SLOT_COUNT;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) == -EINVAL);

   /* Two sites naming one slot leave the other slot unrelocated. */
   m = cell;
   m.reloc_sites[1] = m.reloc_sites[0];
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) == -EEXIST);

   /* A missing vertex site leaves the vertex BO unresolved. */
   m = cell;
   m.reloc_site_count = 1;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) == -EINVAL);

   /* Swapping the slots makes each site name the payload of the other. */
   m = cell;
   m.reloc_sites[0].slot = R300_TRIANGLE_SLOT_VERTEX;
   m.reloc_sites[1].slot = R300_TRIANGLE_SLOT_COLOR;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) == -EINVAL);

   /* A site count that is not one per slot: the equality guard refuses it
    * before the loop reads an index, which is what keeps a count past the
    * storage from reaching the array.
    */
   m = cell;
   m.reloc_site_count = R300_TRIANGLE_MAX_RELOC_SITES + 1;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) == -EINVAL);

   /* A site whose payload dword stops naming its slot. */
   m = cell;
   uint32_t saved = cell.ib[cell.reloc_sites[0].ib_index];
   cell.ib[cell.reloc_sites[0].ib_index] = saved + 1;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&m) == -EINVAL);
   cell.ib[cell.reloc_sites[0].ib_index] = saved;
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == 0);

   r300_tcl_bypass_triangle_release(&cell);
   r300_fragment_binary_finish(&fs);
}

/* The extent family's two invariants: at the maximum extent the
 * parameterized emission is byte-identical to the reference cell, so
 * the qualified digest anchors the family; at any other extent exactly
 * two dwords deviate -- the SC_SCISSORS_BR and SC_CLIPRECT_BR_0
 * payloads the first-draw contract resolves from the extent -- so the
 * pitch word and every other register class carry the qualified bytes.
 */
static void
test_extent_emit_deviates_in_scissor_words_alone(void)
{
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_reference_emit(&reference) == 0);

   struct r300_tcl_bypass_triangle_ib anchor;
   assert(r300_tcl_bypass_triangle_extent_emit(
             R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
             &anchor) == 0);
   assert(anchor.ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(anchor.ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_tcl_bypass_triangle_release(&anchor);

   /* Scissor and clip-rectangle payloads on non-R500 silicon carry a
    * 1440 bias in both axes, packed x | (y << 13); the reference cell's
    * two BR payloads hold the maximum extent's word, and an extent
    * variant replaces exactly those two payloads with its own.
    */
   const uint32_t reference_br =
      ((R300_TRIANGLE_TARGET_WIDTH - 1 + R300_SCISSORS_OFFSET)
       << R300_SCISSORS_X_SHIFT) |
      ((R300_TRIANGLE_TARGET_HEIGHT - 1 + R300_SCISSORS_OFFSET)
       << R300_SCISSORS_Y_SHIFT);

   static const uint32_t extents[][2] = {
      { 1, 1 }, { 17, 33 }, { 48, 20 }, { 33, 64 }, { 64, 1 },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(extents); i++) {
      const uint32_t extent_br =
         ((extents[i][0] - 1 + R300_SCISSORS_OFFSET)
          << R300_SCISSORS_X_SHIFT) |
         ((extents[i][1] - 1 + R300_SCISSORS_OFFSET)
          << R300_SCISSORS_Y_SHIFT);
      struct r300_tcl_bypass_triangle_ib cell;
      assert(r300_tcl_bypass_triangle_extent_emit(extents[i][0],
                                                  extents[i][1],
                                                  &cell) == 0);
      assert(cell.ib_size_dwords == reference.ib_size_dwords);
      uint32_t deviating = 0;
      for (uint32_t d = 0; d < cell.ib_size_dwords; d++) {
         if (cell.ib[d] != reference.ib[d]) {
            /* Each deviating dword is a BR payload: the reference held
             * the maximum extent's biased word there, and the variant
             * holds its own extent's word.
             */
            assert(reference.ib[d] == reference_br);
            assert(cell.ib[d] == extent_br);
            deviating++;
         }
      }
      assert(deviating == 2);
      r300_tcl_bypass_triangle_release(&cell);
   }

   struct r300_tcl_bypass_triangle_ib refused;
   assert(r300_tcl_bypass_triangle_extent_emit(0, 32, &refused) == -EINVAL);
   assert(r300_tcl_bypass_triangle_extent_emit(32, 0, &refused) == -EINVAL);
   assert(r300_tcl_bypass_triangle_extent_emit(
             R300_TRIANGLE_TARGET_WIDTH + 1, 32, &refused) == -EINVAL);
   assert(r300_tcl_bypass_triangle_extent_emit(
             32, R300_TRIANGLE_TARGET_HEIGHT + 1, &refused) == -EINVAL);

   r300_tcl_bypass_triangle_release(&reference);
}

/* The varying cell: the position-only cell's contract, target, and draw
 * over position-plus-varying records.  Its stream declares the tuple the
 * kernel's TCL-bypass vertex-output check proves -- position plus one
 * four-component texture coordinate, VAP_VTX_SIZE 8, an all-identity
 * two-element PSC list whose second element lands in the
 * texture-coordinate-0 vector -- routes the varying through RS_IP_0 /
 * RS_INST_0 to US input 0, fetches 32-byte records, and carries the
 * pass-through fragment binary; its size and digest pin the cell the
 * attended surface submits.
 */
static void
test_varying_cell_tuple_and_digest_are_pinned(void)
{
   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_varying_reference_emit(&cell) == 0);
   assert(cell.ib_size_dwords == R300_VARYING_CELL_IB_DWORDS);
   char digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, digest);
   assert(strcmp(digest, R300_VARYING_CELL_IB_BLAKE3) == 0);
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&cell) == 0);

   struct tracker t;
   memset(&t, 0, sizeof(t));
   track(&t, cell.ib, cell.ib_size_dwords);
   assert(t.cntl_status_seen && (t.cntl_status & (1u << 8)));
   assert(t.fmt0_seen && t.fmt0 == 0x1);
   assert(t.fmt1_seen && t.fmt1 == R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS);
   assert(t.vtx_size_seen && t.vtx_size == 8);
   assert(t.ext_seen_mask == 0xff && !t.ext_nonidentity);
   /* Two FLOAT_4 elements: vector 0, then vector 6 terminating the
    * list; the summed identity fetch of eight dwords equals VTX_SIZE. */
   assert(t.psc0_seen &&
          t.psc0 == ((R300_DATA_TYPE_FLOAT_4 |
                      (0 << R300_DST_VEC_LOC_SHIFT)) |
                     ((R300_DATA_TYPE_FLOAT_4 |
                       (6 << R300_DST_VEC_LOC_SHIFT) | R300_LAST_VEC)
                      << 16)));
   assert(t.vsm_vtx_assm_seen &&
          t.vsm_vtx_assm == (R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0));
   assert(t.rs_count_seen &&
          t.rs_count == (R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN));
   assert(t.rs_inst_count_seen && t.rs_inst_count == 0);
   assert(t.rs_ip0_seen &&
          t.rs_ip0 == (R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
                       R300_RS_SEL_T(R300_RS_SEL_C1) |
                       R300_RS_SEL_R(R300_RS_SEL_C2) |
                       R300_RS_SEL_Q(R300_RS_SEL_C3)));
   assert(t.rs_inst0_seen &&
          t.rs_inst0 == (R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                         R300_RS_INST_TEX_ADDR(0)));
   assert(t.vbpntr_seen &&
          t.vbpntr_size_stride ==
             (R300_VBPNTR_SIZE0(32) | R300_VBPNTR_STRIDE0(32)));
   assert(t.draw_count == 1 && (t.draw_vf_cntl >> 16) == 3);

   /* The position-only reference keeps its retained identity beside the
    * varying cell: the two are distinct cells from one emitter. */
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_reference_emit(&reference) == 0);
   assert(reference.ib_size_dwords == R300_RETAINED_CPU_ROUTE_IB_DWORDS);
   char reference_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);
   assert(strcmp(reference_digest, R300_RETAINED_CPU_ROUTE_IB_BLAKE3) == 0);
   assert(strcmp(reference_digest, digest) != 0);
   r300_tcl_bypass_triangle_release(&reference);
   r300_tcl_bypass_triangle_release(&cell);
}

/* The varying extent family holds the position-only family's invariant:
 * the maximum extent is the varying reference byte for byte, and every
 * other extent deviates in the two scissor-family payloads alone.
 */
static void
test_varying_extent_emit_deviates_in_scissor_words_alone(void)
{
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_varying_reference_emit(&reference) == 0);
   struct r300_tcl_bypass_triangle_ib anchor;
   assert(r300_tcl_bypass_triangle_varying_extent_emit(
             R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
             &anchor) == 0);
   assert(anchor.ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(anchor.ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_tcl_bypass_triangle_release(&anchor);

   const uint32_t reference_br =
      ((R300_TRIANGLE_TARGET_WIDTH - 1 + R300_SCISSORS_OFFSET)
       << R300_SCISSORS_X_SHIFT) |
      ((R300_TRIANGLE_TARGET_HEIGHT - 1 + R300_SCISSORS_OFFSET)
       << R300_SCISSORS_Y_SHIFT);
   static const uint32_t extents[][2] = { { 1, 1 }, { 17, 33 }, { 64, 1 } };
   for (unsigned i = 0; i < ARRAY_SIZE(extents); i++) {
      const uint32_t extent_br =
         ((extents[i][0] - 1 + R300_SCISSORS_OFFSET)
          << R300_SCISSORS_X_SHIFT) |
         ((extents[i][1] - 1 + R300_SCISSORS_OFFSET)
          << R300_SCISSORS_Y_SHIFT);
      struct r300_tcl_bypass_triangle_ib cell;
      assert(r300_tcl_bypass_triangle_varying_extent_emit(
                extents[i][0], extents[i][1], &cell) == 0);
      assert(cell.ib_size_dwords == reference.ib_size_dwords);
      uint32_t deviating = 0;
      for (uint32_t d = 0; d < cell.ib_size_dwords; d++) {
         if (cell.ib[d] != reference.ib[d]) {
            assert(reference.ib[d] == reference_br);
            assert(cell.ib[d] == extent_br);
            deviating++;
         }
      }
      assert(deviating == 2);
      r300_tcl_bypass_triangle_release(&cell);
   }
   struct r300_tcl_bypass_triangle_ib refused;
   assert(r300_tcl_bypass_triangle_varying_extent_emit(0, 32, &refused) ==
          -EINVAL);
   assert(r300_tcl_bypass_triangle_varying_extent_emit(
             R300_TRIANGLE_TARGET_WIDTH + 1, 32, &refused) == -EINVAL);
   r300_tcl_bypass_triangle_release(&reference);
}

/* Edge function of (a, b) at p, the oracle's own arithmetic. */
static float
edge(const float *a, const float *b, float px, float py)
{
   return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
}

static uint32_t
unorm8(float v)
{
   if (!(v > 0.0f))
      return 0;
   if (v >= 1.0f)
      return 255;
   return (uint32_t)(v * 255.0f + 0.5f);
}

/* Fills a 64x64 sentinel target with the analytic gradient -- the
 * barycentric interpolation of the reference varying colors at every
 * pixel center inside the window-space triangle -- with each channel
 * offset by bias bytes (clamped), and the canary row at the sentinel.
 */
static void
render_gradient(uint32_t *pixels, int bias)
{
   const float *v0 = &r300_tcl_bypass_triangle_vertices[0];
   const float *v1 = &r300_tcl_bypass_triangle_vertices[4];
   const float *v2 = &r300_tcl_bypass_triangle_vertices[8];
   const float area = edge(v0, v1, v2[0], v2[1]);
   for (uint32_t i = 0; i < R300_TRIANGLE_COLOR_BYTES / 4; i++)
      pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   for (uint32_t y = 0; y < R300_TRIANGLE_TARGET_HEIGHT; y++) {
      for (uint32_t x = 0; x < R300_TRIANGLE_TARGET_WIDTH; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         const float w0 = edge(v1, v2, px, py) / area;
         const float w1 = edge(v2, v0, px, py) / area;
         const float w2 = 1.0f - w0 - w1;
         if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
            continue;
         uint32_t dword = 0;
         static const unsigned channel_of_byte[4] = { 2, 1, 0, 3 };
         for (unsigned byte = 0; byte < 4; byte++) {
            const unsigned c = channel_of_byte[byte];
            const float value =
               w0 * r300_tcl_bypass_triangle_varying_colors[c] +
               w1 * r300_tcl_bypass_triangle_varying_colors[4 + c] +
               w2 * r300_tcl_bypass_triangle_varying_colors[8 + c];
            int b = (int)unorm8(value) + bias;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            dword |= (uint32_t)b << (8 * byte);
         }
         pixels[y * R300_TRIANGLE_TARGET_PITCH_PIXELS + x] = dword;
      }
   }
}

/* The varying oracle's calibration: the analytic gradient passes with
 * zero deviation; a gradient biased by the tolerance passes and reports
 * it; one byte past the tolerance fails the interior; the constant draw
 * color -- the position-only cell's output -- fails the interior, so a
 * pipeline that dropped the varying cannot pass this oracle; an
 * unexecuted target reports no execution; and the sentinel-filled
 * exterior and canary rules carry over.
 */
static void
test_varying_oracle_calibration(void)
{
   static uint32_t pixels[R300_TRIANGLE_COLOR_BYTES / 4];
   struct r300_triangle_oracle_verdict v;

   render_gradient(pixels, 0);
   r300_tcl_bypass_triangle_varying_extent_oracle(
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
      r300_tcl_bypass_triangle_varying_colors, pixels, sizeof(pixels), &v);
   assert(v.executed && v.interior_pass && v.exterior_pass && v.canary_pass);
   assert(v.interior_samples == 4 && v.exterior_samples > 0);
   assert(v.interior_max_deviation == 0);

   render_gradient(pixels, (int)R300_TRIANGLE_VARYING_ORACLE_TOLERANCE);
   r300_tcl_bypass_triangle_varying_extent_oracle(
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
      r300_tcl_bypass_triangle_varying_colors, pixels, sizeof(pixels), &v);
   assert(v.interior_pass &&
          v.interior_max_deviation == R300_TRIANGLE_VARYING_ORACLE_TOLERANCE);

   render_gradient(pixels, -(int)R300_TRIANGLE_VARYING_ORACLE_TOLERANCE - 1);
   r300_tcl_bypass_triangle_varying_extent_oracle(
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
      r300_tcl_bypass_triangle_varying_colors, pixels, sizeof(pixels), &v);
   assert(!v.interior_pass && v.exterior_pass && v.canary_pass);
   assert(v.interior_max_deviation ==
          R300_TRIANGLE_VARYING_ORACLE_TOLERANCE + 1);

   /* The constant draw color over the whole triangle. */
   render_gradient(pixels, 0);
   for (uint32_t i = 0; i < R300_TRIANGLE_COLOR_BYTES / 4; i++)
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL)
         pixels[i] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
   r300_tcl_bypass_triangle_varying_extent_oracle(
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
      r300_tcl_bypass_triangle_varying_colors, pixels, sizeof(pixels), &v);
   assert(v.executed && !v.interior_pass && v.exterior_pass &&
          v.canary_pass);

   /* Unexecuted: the sentinel everywhere. */
   for (uint32_t i = 0; i < R300_TRIANGLE_COLOR_BYTES / 4; i++)
      pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   r300_tcl_bypass_triangle_varying_extent_oracle(
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
      r300_tcl_bypass_triangle_varying_colors, pixels, sizeof(pixels), &v);
   assert(!v.executed && !v.interior_pass && v.exterior_pass &&
          v.canary_pass);

   /* A disturbed canary row fails whatever the interior holds. */
   render_gradient(pixels, 0);
   pixels[R300_TRIANGLE_TARGET_PITCH_PIXELS * R300_TRIANGLE_TARGET_HEIGHT] =
      0;
   r300_tcl_bypass_triangle_varying_extent_oracle(
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
      r300_tcl_bypass_triangle_varying_colors, pixels, sizeof(pixels), &v);
   assert(v.interior_pass && !v.canary_pass);

   /* An inadmissible extent fails closed with zero samples. */
   r300_tcl_bypass_triangle_varying_extent_oracle(
      0, R300_TRIANGLE_TARGET_HEIGHT, r300_tcl_bypass_triangle_varying_colors,
      pixels, sizeof(pixels), &v);
   assert(!v.interior_pass && v.interior_samples == 0);

   /* The varying reference payload is the positions plus these colors. */
   for (unsigned i = 0; i < 3; i++) {
      assert(memcmp(&r300_tcl_bypass_triangle_varying_vertices[i * 8],
                    &r300_tcl_bypass_triangle_vertices[i * 4],
                    4 * sizeof(float)) == 0);
      assert(memcmp(&r300_tcl_bypass_triangle_varying_vertices[i * 8 + 4],
                    &r300_tcl_bypass_triangle_varying_colors[i * 4],
                    4 * sizeof(float)) == 0);
   }
}

/* The triangle-count family: count 1 is the reference cell byte for
 * byte; a count T keeps the dword count and differs in exactly the two
 * payloads the host expansion moves -- the contract's VAP_VF_MAX_VTX_INDX
 * (3T - 1) and the draw packet's VAP_VF_CNTL (NUM_VERTICES 3T) -- for
 * the position-only and the varying shape; a count outside
 * 1..R300_TRIANGLE_MAX_TRIANGLES refuses, and the ceiling itself emits
 * with the 16-bit fields at their maximum.
 */
static void
test_family_emit_deviates_in_count_words_alone(void)
{
   for (unsigned shape = 0; shape < 2; shape++) {
      const bool varying = shape == 1;
      struct r300_tcl_bypass_triangle_ib reference;
      assert((varying ? r300_tcl_bypass_triangle_varying_reference_emit(
                           &reference)
                      : r300_tcl_bypass_triangle_reference_emit(
                           &reference)) == 0);
      struct r300_tcl_bypass_triangle_ib anchor;
      assert(r300_tcl_bypass_triangle_family_emit(
                R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
                varying, 1, &anchor) == 0);
      assert(anchor.ib_size_dwords == reference.ib_size_dwords);
      assert(memcmp(anchor.ib, reference.ib,
                    reference.ib_size_dwords * sizeof(uint32_t)) == 0);
      r300_tcl_bypass_triangle_release(&anchor);

      static const uint32_t counts[] = { 2, 7, 1000,
                                         R300_TRIANGLE_MAX_TRIANGLES };
      const uint32_t reference_draw = R300_VAP_VF_CNTL__PRIM_TRIANGLES |
                                      R300_PRIM_WALK_LIST |
                                      (3 << R300_PRIM_NUM_VERTICES_SHIFT);
      for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
         const uint32_t vertices = 3 * counts[i];
         struct r300_tcl_bypass_triangle_ib cell;
         assert(r300_tcl_bypass_triangle_family_emit(
                   R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
                   varying, counts[i], &cell) == 0);
         assert(cell.ib_size_dwords == reference.ib_size_dwords);
         uint32_t deviating = 0;
         for (uint32_t d = 0; d < cell.ib_size_dwords; d++) {
            if (cell.ib[d] == reference.ib[d])
               continue;
            if (reference.ib[d] == reference_draw) {
               assert(cell.ib[d] ==
                      (R300_VAP_VF_CNTL__PRIM_TRIANGLES | R300_PRIM_WALK_LIST |
                       (vertices << R300_PRIM_NUM_VERTICES_SHIFT)));
            } else {
               /* The contract's maximum vertex index payload. */
               assert(reference.ib[d] == 2);
               assert(cell.ib[d] == vertices - 1);
            }
            deviating++;
         }
         assert(deviating == 2);
         r300_tcl_bypass_triangle_release(&cell);
      }
      struct r300_tcl_bypass_triangle_ib refused;
      assert(r300_tcl_bypass_triangle_family_emit(
                R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
                varying, 0, &refused) == -EINVAL);
      assert(r300_tcl_bypass_triangle_family_emit(
                R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
                varying, R300_TRIANGLE_MAX_TRIANGLES + 1, &refused) ==
             -EINVAL);
      r300_tcl_bypass_triangle_release(&reference);
   }
}

/* Counts the dwords where two same-sized IBs differ and records the
 * first R300_TRIANGLE_MAX_DWORDS of their indices.
 */
static uint32_t
ib_deviating_dwords(const struct r300_tcl_bypass_triangle_ib *a,
                    const struct r300_tcl_bypass_triangle_ib *b,
                    uint32_t *indices, uint32_t max_indices)
{
   assert(a->ib_size_dwords == b->ib_size_dwords);
   uint32_t n = 0;
   for (uint32_t d = 0; d < a->ib_size_dwords; d++) {
      if (a->ib[d] != b->ib[d]) {
         if (n < max_indices)
            indices[n] = d;
         n++;
      }
   }
   return n;
}

/* The register a single-register PACKET0 payload at dword index writes,
 * read from the header one dword before it.
 */
static uint32_t
payload_register(const struct r300_tcl_bypass_triangle_ib *ib,
                 uint32_t payload_index)
{
   assert(payload_index >= 1);
   const uint32_t header = ib->ib[payload_index - 1];
   assert((header >> 30) == 0);
   return (header & 0x7fffu) << 2;
}

/* The render-shape family: the reference shape is the reference cell
 * byte for byte, and each parameter alone moves its named register
 * class -- pitch the RB3D_COLORPITCH0 payload, lane order the
 * US_OUT_FMT_0 payload, the fragment constant the four PFS_PARAM_0
 * payloads, the extent the two scissor-family payloads -- so the
 * qualified digest anchors every shape and a silicon arm per parameter
 * isolates one register class.
 */
static void
test_render_shape_deviates_per_parameter(void)
{
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_reference_emit(&reference) == 0);

   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == 0);
   assert(r300_tcl_bypass_triangle_render_shape_draw_dword(&shape) ==
          R300_TRIANGLE_DRAW_COLOR_B8G8R8A8);
   assert(r300_tcl_bypass_triangle_render_shape_color_bytes(&shape) ==
          R300_TRIANGLE_COLOR_BYTES);
   float vertices[R300_TRIANGLE_VERTEX_DWORDS];
   r300_tcl_bypass_triangle_render_shape_vertices(&shape, vertices);
   assert(memcmp(vertices, r300_tcl_bypass_triangle_vertices,
                 sizeof(vertices)) == 0);

   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, &cell) == 0);
   assert(cell.ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(cell.ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_tcl_bypass_triangle_release(&cell);

   uint32_t indices[8];

   /* Pitch: one payload, RB3D_COLORPITCH0, carrying the packed pitch. */
   shape.pitch_pixels = 256;
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, &cell) == 0);
   assert(ib_deviating_dwords(&reference, &cell, indices, 8) == 1);
   assert(payload_register(&cell, indices[0]) == R300_RB3D_COLORPITCH0);
   assert(cell.ib[indices[0]] == r300_rb3d_colorpitch0_pack_argb8888(256));
   assert(r300_tcl_bypass_triangle_render_shape_color_bytes(&shape) ==
          256u * 65u * 4u);
   r300_tcl_bypass_triangle_release(&cell);
   r300_tcl_bypass_triangle_render_shape_reference(&shape);

   /* Lane order: one payload, US_OUT_FMT_0, red and blue selectors
    * exchanged; the predicted dword exchanges bytes 0 and 2.
    */
   shape.lanes = R300_TRIANGLE_LANES_R8G8B8A8;
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, &cell) == 0);
   assert(ib_deviating_dwords(&reference, &cell, indices, 8) == 1);
   assert(payload_register(&cell, indices[0]) == R300_US_OUT_FMT_0);
   assert(cell.ib[indices[0]] ==
          (R300_US_OUT_FMT_C4_8 | R300_C0_SEL_R | R300_C1_SEL_G |
           R300_C2_SEL_B | R300_C3_SEL_A));
   assert(r300_tcl_bypass_triangle_render_shape_draw_dword(&shape) ==
          (R300_TRIANGLE_DRAW_COLOR_RED |
           (R300_TRIANGLE_DRAW_COLOR_GREEN << 8) |
           (R300_TRIANGLE_DRAW_COLOR_BLUE << 16) |
           (R300_TRIANGLE_DRAW_COLOR_ALPHA << 24)));
   r300_tcl_bypass_triangle_release(&cell);
   r300_tcl_bypass_triangle_render_shape_reference(&shape);

   /* Fragment constant: the four PFS_PARAM_0 payloads, in the FP24
    * register encoding, and nothing else; magenta predicts 0xffff00ff
    * in B8G8R8A8.
    */
   const uint32_t magenta[4] = { 0x3f800000u, 0, 0x3f800000u, 0x3f800000u };
   memcpy(shape.color_bits, magenta, sizeof(magenta));
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, &cell) == 0);
   assert(ib_deviating_dwords(&reference, &cell, indices, 8) == 4);
   for (unsigned c = 0; c < 4; c++) {
      assert(indices[c] == indices[0] + c);
      assert(cell.ib[indices[c]] == r300_fp24_register_bits(magenta[c]));
   }
   assert(payload_register(&cell, indices[0]) == R300_PFS_PARAM_0_X);
   assert(r300_fp24_register_bits(0x3f800000u) == 0x003f0000u);
   assert(r300_fp24_register_bits(0x3e000000u) == 0x003c0000u);
   assert(r300_fp24_register_bits(0x3ec00000u) == 0x003d8000u);
   assert(r300_tcl_bypass_triangle_render_shape_draw_dword(&shape) ==
          0xffff00ffu);
   r300_tcl_bypass_triangle_release(&cell);
   r300_tcl_bypass_triangle_render_shape_reference(&shape);

   /* Extent past the fixed family's 64: the two scissor-family payloads,
    * as the extent family, with the pitch word untouched.
    */
   shape.width = 256;
   shape.height = 256;
   shape.pitch_pixels = 256;
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, &cell) == 0);
   assert(ib_deviating_dwords(&reference, &cell, indices, 8) == 3);
   uint32_t scissor_words = 0;
   for (unsigned i = 0; i < 3; i++) {
      const uint32_t reg = payload_register(&cell, indices[i]);
      if (reg == R300_RB3D_COLORPITCH0)
         continue;
      assert(reg == R300_SC_SCISSORS_BR || reg == R300_SC_CLIPRECT_BR_0);
      scissor_words++;
   }
   assert(scissor_words == 2);
   r300_tcl_bypass_triangle_render_shape_vertices(&shape, vertices);
   assert(vertices[0] == 32.0f && vertices[1] == 32.0f &&
          vertices[4] == 224.0f && vertices[9] == 224.0f);
   r300_tcl_bypass_triangle_release(&cell);

   /* Target offset: one payload, RB3D_COLOROFFSET0, carrying the byte
    * offset the kernel biases by the relocation base
    * (r300_packet0_check writes ib[idx] = idx_value + reloc->gpu_offset,
    * radeon r300.c), and the footprint grows by exactly that offset.
    */
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.target_offset = 4096;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == 0);
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, &cell) == 0);
   assert(ib_deviating_dwords(&reference, &cell, indices, 8) == 1);
   assert(payload_register(&cell, indices[0]) == R300_RB3D_COLOROFFSET0);
   assert(cell.ib[indices[0]] == 4096u);
   assert(r300_tcl_bypass_triangle_render_shape_color_bytes(&shape) ==
          4096u + R300_TRIANGLE_COLOR_BYTES);
   r300_tcl_bypass_triangle_release(&cell);

   /* An offset carrying a reserved low bit of RB3D_COLOROFFSET refuses:
    * the kernel adds the relocation base without masking, so the
    * driver's admission is the gate.
    */
   shape.target_offset = 16;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   shape.target_offset = R300_TRIANGLE_MAX_TARGET_OFFSET +
                         R300_TRIANGLE_TARGET_OFFSET_ALIGNMENT;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);

   /* Admission refusals: odd pitch, pitch under width, a pitch not a
    * multiple of 8, extent past the ceiling, a triangle count other
    * than 1, a constant off the FP24 lattice, and a NaN constant.
    */
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.pitch_pixels = 65;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   shape.pitch_pixels = 62;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   shape.pitch_pixels = 66;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.width = R300_TRIANGLE_RENDER_MAX_EXTENT + 1;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.triangle_count = 2;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.color_bits[1] = 0x3e000001u;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   shape.color_bits[1] = 0x7fc00000u;
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == -EINVAL);
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, &cell) ==
          -EINVAL);
   assert(cell.ib == NULL);

   r300_tcl_bypass_triangle_release(&reference);
}

/* Paints the analytic triangle for a shape at pixel centers over a
 * sentinel target at the shape's pitch.
 */
static void
paint_shape(const struct r300_triangle_render_shape *shape,
            uint32_t *target, uint32_t dwords)
{
   float v[R300_TRIANGLE_VERTEX_DWORDS];
   r300_tcl_bypass_triangle_render_shape_vertices(shape, v);
   const uint32_t color =
      r300_tcl_bypass_triangle_render_shape_draw_dword(shape);
   for (uint32_t i = 0; i < dwords; i++)
      target[i] = R300_TRIANGLE_COLOR_SENTINEL;
   uint32_t *rows = target + shape->target_offset / 4u;
   for (uint32_t y = 0; y < shape->height; y++) {
      for (uint32_t x = 0; x < shape->width; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         const float e0 = (v[4] - v[0]) * (py - v[1]) -
                          (v[5] - v[1]) * (px - v[0]);
         const float e1 = (v[8] - v[4]) * (py - v[5]) -
                          (v[9] - v[5]) * (px - v[4]);
         const float e2 = (v[0] - v[8]) * (py - v[9]) -
                          (v[1] - v[9]) * (px - v[8]);
         if (e0 > 0.0f && e1 > 0.0f && e2 > 0.0f)
            rows[y * shape->pitch_pixels + x] = color;
      }
   }
}

/* The render-shape oracle's calibration on a dEQP-shaped target
 * (256x200 at pitch 256, R8G8B8A8) with an asymmetric constant, so a
 * lane exchange is observable: the synthesized witness passes; a
 * lane-swapped witness, a reference-color witness, a 64-pitch witness,
 * a padding-band write, and a canary-row write each fail their verdict.
 */
static void
test_render_shape_oracle_calibration(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.width = 256;
   shape.height = 200;
   shape.pitch_pixels = 256;
   shape.lanes = R300_TRIANGLE_LANES_R8G8B8A8;
   /* (1, 0, 0.5, 1): R8G8B8A8 bytes [ff, 00, 80, ff]. */
   const uint32_t color[4] = { 0x3f800000u, 0, 0x3f000000u, 0x3f800000u };
   memcpy(shape.color_bits, color, sizeof(color));
   assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == 0);
   assert(r300_tcl_bypass_triangle_render_shape_draw_dword(&shape) ==
          0xff8000ffu);

   const uint32_t bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&shape);
   assert(bytes == 256u * 201u * 4u);
   static uint32_t target[256 * 201];
   struct r300_triangle_oracle_verdict verdict;

   paint_shape(&shape, target, ARRAY_SIZE(target));
   assert(target[100 * 256 + 128] == 0xff8000ffu);
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes,
                                                &verdict);
   assert(verdict.executed && verdict.interior_pass &&
          verdict.exterior_pass && verdict.canary_pass);
   assert(verdict.interior_samples == 4 && verdict.exterior_samples >= 8);

   /* The truncated footprint fails closed. */
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes - 4,
                                                &verdict);
   assert(!verdict.executed && !verdict.canary_pass &&
          verdict.interior_samples == 0);

   /* The witness a B8G8R8A8 lane placement writes for the same
    * constant fails the interior alone.
    */
   struct r300_triangle_render_shape swapped = shape;
   swapped.lanes = R300_TRIANGLE_LANES_B8G8R8A8;
   paint_shape(&swapped, target, ARRAY_SIZE(target));
   assert(target[100 * 256 + 128] == 0xffff0080u);
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes,
                                                &verdict);
   assert(verdict.executed && !verdict.interior_pass &&
          verdict.exterior_pass && verdict.canary_pass);

   /* A witness at the reference color fails the interior. */
   struct r300_triangle_render_shape wrong_color;
   r300_tcl_bypass_triangle_render_shape_reference(&wrong_color);
   wrong_color.width = shape.width;
   wrong_color.height = shape.height;
   wrong_color.pitch_pixels = shape.pitch_pixels;
   wrong_color.lanes = shape.lanes;
   paint_shape(&wrong_color, target, ARRAY_SIZE(target));
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes,
                                                &verdict);
   assert(verdict.executed && !verdict.interior_pass);

   /* A witness rendered at pitch 64 into a pitch-256 footprint lands
    * its rows in the wrong place: interior samples miss and the canary
    * band carries writes.
    */
   struct r300_triangle_render_shape narrow = shape;
   narrow.width = 64;
   narrow.height = 64;
   narrow.pitch_pixels = 64;
   paint_shape(&narrow, target, ARRAY_SIZE(target));
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes,
                                                &verdict);
   assert(verdict.executed && !verdict.interior_pass);

   /* A padding-band write and a canary-row write each fail the canary. */
   paint_shape(&shape, target, ARRAY_SIZE(target));
   shape.width = 240;
   paint_shape(&shape, target, ARRAY_SIZE(target));
   target[7 * 256 + 250] = 0xffff00ffu;
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes,
                                                &verdict);
   assert(verdict.executed && verdict.interior_pass && !verdict.canary_pass);
   target[7 * 256 + 250] = R300_TRIANGLE_COLOR_SENTINEL;
   target[200 * 256 + 1] = 1u;
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes,
                                                &verdict);
   assert(verdict.executed && verdict.interior_pass && !verdict.canary_pass);
   target[200 * 256 + 1] = R300_TRIANGLE_COLOR_SENTINEL;
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, target, bytes,
                                                &verdict);
   assert(verdict.executed && verdict.interior_pass &&
          verdict.exterior_pass && verdict.canary_pass);

   /* The offset witness: a target whose render row 0 sits 4096 bytes
    * into the allocation.  The painted witness passes; a write in the
    * band below the offset fails the canary, so the bytes the render
    * never addresses are held to the sentinel as the canary row is.
    */
   struct r300_triangle_render_shape offset_shape;
   r300_tcl_bypass_triangle_render_shape_reference(&offset_shape);
   offset_shape.target_offset = 4096;
   const uint32_t offset_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&offset_shape);
   assert(offset_bytes == 4096u + R300_TRIANGLE_COLOR_BYTES);
   static uint32_t offset_target[(4096 + R300_TRIANGLE_COLOR_BYTES) / 4];
   paint_shape(&offset_shape, offset_target, ARRAY_SIZE(offset_target));
   assert(offset_target[1024 + 32 * 64 + 32] ==
          R300_TRIANGLE_DRAW_COLOR_B8G8R8A8);
   r300_tcl_bypass_triangle_render_shape_oracle(
      &offset_shape, offset_target, offset_bytes, &verdict);
   assert(verdict.executed && verdict.interior_pass &&
          verdict.exterior_pass && verdict.canary_pass);

   offset_target[1023] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
   r300_tcl_bypass_triangle_render_shape_oracle(
      &offset_shape, offset_target, offset_bytes, &verdict);
   assert(verdict.executed && verdict.interior_pass &&
          verdict.exterior_pass && !verdict.canary_pass);
   offset_target[1023] = R300_TRIANGLE_COLOR_SENTINEL;

   /* The same painted bytes read at offset zero miss every interior
    * sample, so the oracle reads the offset rather than scanning for
    * the rendered rows.
    */
   struct r300_triangle_render_shape unshifted = offset_shape;
   unshifted.target_offset = 0;
   r300_tcl_bypass_triangle_render_shape_oracle(
      &unshifted, offset_target,
      r300_tcl_bypass_triangle_render_shape_color_bytes(&unshifted),
      &verdict);
   assert(verdict.executed && !verdict.interior_pass);

   /* The truncated footprint fails closed at the offset too. */
   r300_tcl_bypass_triangle_render_shape_oracle(
      &offset_shape, offset_target, offset_bytes - 4, &verdict);
   assert(!verdict.executed && !verdict.canary_pass &&
          verdict.interior_samples == 0);
}

/* The shared UNORM8 packer: the conversion the color buffer applies to
 * a shaded value, and the one a Vulkan clear color reaches its texel
 * through.  Clamping and NaN handling are part of the contract, so the
 * out-of-range and unordered inputs are calibrated beside the ordinary
 * quarter-step one.
 */
static void
test_pack_unorm8_dword(void)
{
   const float quarters[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
   assert(r300_tcl_bypass_triangle_pack_unorm8_dword(
             R300_TRIANGLE_LANES_B8G8R8A8, quarters) == 0xff4080bfu);
   assert(r300_tcl_bypass_triangle_pack_unorm8_dword(
             R300_TRIANGLE_LANES_R8G8B8A8, quarters) == 0xffbf8040u);

   /* Out of range clamps to the endpoints and a NaN channel stores
    * zero, so every float32 quadruple has a defined texel.
    */
   const float extremes[4] = { -1.0f, 2.0f, 0.0f / 0.0f, 1.0f };
   assert(r300_tcl_bypass_triangle_pack_unorm8_dword(
             R300_TRIANGLE_LANES_R8G8B8A8, extremes) == 0xff00ff00u);

   /* The shape's draw dword is the shape constant through this packer. */
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   float reference_rgba[4];
   memcpy(reference_rgba, shape.color_bits, sizeof(reference_rgba));
   assert(r300_tcl_bypass_triangle_pack_unorm8_dword(shape.lanes,
                                                     reference_rgba) ==
          r300_tcl_bypass_triangle_render_shape_draw_dword(&shape));
}

/* The sampled cell's stream: the TX unit-0 block lands with the
 * declared geometry, the texture reloc site precedes the color and
 * vertex sites in stream order, the site validator accepts the
 * three-slot list, and each admission bound refuses by itself.
 */
static void
test_sampled_cell_stream_and_refusals(void)
{
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_sampled_fs(&fs) == 0);

   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(64),
      .fragment_binary = &fs,
      .varying = true,
      .sampled = true,
      .texture_offset = 4096,
      .texture_width = 64,
      .texture_height = 64,
      .texture_pitch_texels = 64,
      .texture_lanes = R300_TRIANGLE_LANES_R8G8B8A8,
   };
   struct r300_tcl_bypass_triangle_ib ib;
   assert(r300_tcl_bypass_triangle_emit(&params, &ib) == 0);

   struct tracker t = { 0 };
   track(&t, ib.ib, ib.ib_size_dwords);
   assert(t.tx_enable_seen && t.tx_enable == R300_TX_ENABLE_0);
   assert(t.tx_filter0_seen &&
          t.tx_filter0 ==
             ((R300_TX_CLAMP_TO_EDGE << R300_TX_WRAP_S_SHIFT) |
              (R300_TX_CLAMP_TO_EDGE << R300_TX_WRAP_T_SHIFT) |
              R300_TX_MAG_FILTER_NEAREST | R300_TX_MIN_FILTER_NEAREST));
   assert(t.tx_format0_seen &&
          t.tx_format0 == ((63u << R300_TX_WIDTHMASK_SHIFT) |
                           (63u << R300_TX_HEIGHTMASK_SHIFT) |
                           R300_TX_PITCH_EN));
   assert(t.tx_format1_seen &&
          t.tx_format1 == R300_EASY_TX_FORMAT(Z, Y, X, W, W8Z8Y8X8));
   assert(t.tx_format2_seen && t.tx_format2 == 63);
   assert(t.tx_offset_seen && t.tx_offset == 4096);

   assert(ib.reloc_site_count == R300_TRIANGLE_SAMPLED_SLOT_COUNT);
   assert(ib.reloc_sites[0].slot == R300_TRIANGLE_SLOT_TEXTURE);
   assert(ib.reloc_sites[1].slot == R300_TRIANGLE_SLOT_COLOR);
   assert(ib.reloc_sites[2].slot == R300_TRIANGLE_SLOT_VERTEX);
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&ib) == 0);
   r300_tcl_bypass_triangle_release(&ib);

   struct r300_tcl_bypass_triangle_params bad;

   bad = params;
   bad.varying = false;
   assert(r300_tcl_bypass_triangle_emit(&bad, &ib) == -EINVAL);

   bad = params;
   bad.texture_offset = 4100;
   assert(r300_tcl_bypass_triangle_emit(&bad, &ib) == -EINVAL);

   bad = params;
   bad.texture_pitch_texels = 32;
   assert(r300_tcl_bypass_triangle_emit(&bad, &ib) == -EINVAL);

   bad = params;
   bad.texture_width = 2049;
   assert(r300_tcl_bypass_triangle_emit(&bad, &ib) == -EINVAL);

   bad = params;
   bad.texture_lanes = (enum r300_triangle_lane_order)2;
   assert(r300_tcl_bypass_triangle_emit(&bad, &ib) == -EINVAL);

   /* A height-one texture is the 1D shape: FORMAT0's height mask reads
    * zero and the rest of the block keeps its words.
    */
   struct r300_tcl_bypass_triangle_params one_row = params;
   one_row.texture_height = 1;
   assert(r300_tcl_bypass_triangle_emit(&one_row, &ib) == 0);
   struct tracker t1 = { 0 };
   track(&t1, ib.ib, ib.ib_size_dwords);
   assert(t1.tx_format0_seen &&
          t1.tx_format0 == ((63u << R300_TX_WIDTHMASK_SHIFT) |
                            (0u << R300_TX_HEIGHTMASK_SHIFT) |
                            R300_TX_PITCH_EN));
   assert(t1.tx_format1_seen && t1.tx_format1 == t.tx_format1);
   assert(t1.tx_offset_seen && t1.tx_offset == t.tx_offset);
   r300_tcl_bypass_triangle_release(&ib);

   /* The B8G8R8A8 lane order swaps the R and B selects and leaves the
    * rest of the stream at the R8G8B8A8 cell's words.
    */
   params.texture_lanes = R300_TRIANGLE_LANES_B8G8R8A8;
   assert(r300_tcl_bypass_triangle_emit(&params, &ib) == 0);
   struct tracker t2 = { 0 };
   track(&t2, ib.ib, ib.ib_size_dwords);
   assert(t2.tx_format1_seen &&
          t2.tx_format1 == R300_EASY_TX_FORMAT(X, Y, Z, W, W8Z8Y8X8));
   assert(t2.tx_format0_seen && t2.tx_format0 == t.tx_format0);
   assert(t2.tx_format2_seen && t2.tx_format2 == t.tx_format2);
   r300_tcl_bypass_triangle_release(&ib);

   r300_fragment_binary_finish(&fs);
}

/* The dword index of the first or last type-0 write to reg, or -1. */
static int
find_reg_write(const uint32_t *ib, uint32_t count, uint32_t reg, bool last)
{
   int found = -1;
   uint32_t i = 0;
   while (i < count) {
      const uint32_t header = ib[i];
      const uint32_t payload = ((header >> 16) & 0x3fffu) + 1u;
      if ((header >> 30) == 0 && ((header & 0x7fffu) << 2) == reg) {
         found = (int)i;
         if (!last)
            return found;
      }
      i += 1 + payload;
   }
   return found;
}

/* The composed cell: the render half's stream followed by the sample
 * half's, five relocation sites over five slots, and the coherency edge
 * between them -- the render half's destination-cache flush precedes the
 * sample half's texture-tag invalidate, so the color writes publish
 * before the fetch reads them.
 */
static void
test_composed_render_sample_cell(void)
{
   struct r300_triangle_composed_render_sample composed;
   r300_tcl_bypass_triangle_render_shape_reference(&composed.render);
   r300_tcl_bypass_triangle_render_shape_reference(&composed.sample);
   composed.sample.target_offset = 65536;

   struct r300_tcl_bypass_triangle_ib ib;
   assert(r300_tcl_bypass_triangle_composed_render_sample_emit(
             &composed, &ib) == 0);

   struct r300_tcl_bypass_triangle_ib render_half;
   assert(r300_tcl_bypass_triangle_render_shape_emit(&composed.render,
                                                     &render_half) == 0);
   assert(ib.ib_size_dwords > render_half.ib_size_dwords);
   assert(memcmp(ib.ib, render_half.ib,
                 render_half.ib_size_dwords * sizeof(uint32_t)) == 0);

   assert(ib.reloc_site_count == R300_TRIANGLE_COMPOSED_SLOT_COUNT);
   uint32_t slot_mask = 0;
   for (uint32_t i = 0; i < ib.reloc_site_count; i++) {
      slot_mask |= 1u << ib.reloc_sites[i].slot;
      assert(ib.reloc_sites[i].ib_index < ib.ib_size_dwords);
   }
   assert(slot_mask == ((1u << R300_TRIANGLE_SLOT_VERTEX) |
                        (1u << R300_TRIANGLE_SLOT_COLOR) |
                        (1u << R300_TRIANGLE_SLOT_TEXTURE) |
                        (1u << R300_TRIANGLE_SLOT_COMPOSED_VERTEX) |
                        (1u << R300_TRIANGLE_SLOT_COMPOSED_COLOR)));
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&ib) == 0);

   const int flush = find_reg_write(ib.ib, render_half.ib_size_dwords,
                                    R300_RB3D_DSTCACHE_CTLSTAT, true);
   /* The render half's own state prefix invalidates the texture tags
    * too, so the edge reads the sample half's first invalidate: the
    * search starts at the half boundary, which is a packet boundary.
    */
   const int invalidate =
      find_reg_write(ib.ib + render_half.ib_size_dwords,
                     ib.ib_size_dwords - render_half.ib_size_dwords,
                     R300_TX_INVALTAGS, false);
   assert(flush >= 0 && invalidate >= 0);
   assert((uint32_t)flush < render_half.ib_size_dwords);

   /* The sample half reads the render half's target: its texture offset
    * is that target's, so the two halves name one byte range.
    */
   const int tx_offset =
      find_reg_write(ib.ib + render_half.ib_size_dwords,
                     ib.ib_size_dwords - render_half.ib_size_dwords,
                     R300_TX_OFFSET_0, false) +
      (int)render_half.ib_size_dwords;
   assert(tx_offset >= 0 &&
          ib.ib[tx_offset + 1] == composed.render.target_offset);

   r300_tcl_bypass_triangle_release(&render_half);
   r300_tcl_bypass_triangle_release(&ib);

   struct r300_triangle_composed_render_sample bad = composed;
   bad.render.width = 0;
   assert(r300_tcl_bypass_triangle_composed_render_sample_emit(&bad, &ib) ==
          -EINVAL);
   bad = composed;
   bad.sample.pitch_pixels = 1;
   assert(r300_tcl_bypass_triangle_composed_render_sample_emit(&bad, &ib) ==
          -EINVAL);
}


/* The reference geometry's coverage predicate: window vertices (8, 8),
 * (56, 8), (32, 56), which wind so an interior point yields three
 * positive edge functions.
 */
static bool
sample_inside_reference(float px, float py)
{
   static const float vx[3] = { 8.0f, 56.0f, 32.0f };
   static const float vy[3] = { 8.0f, 8.0f, 56.0f };
   bool in = true;
   for (unsigned e = 0; e < 3; e++) {
      const unsigned n = (e + 1) % 3;
      in &= (vx[n] - vx[e]) * (py - vy[e]) -
               (vy[n] - vy[e]) * (px - vx[e]) >
            0.0f;
   }
   return in;
}

/* Calibrates the exact-coverage verdict on a synthesized known-good
 * footprint and on one mutation per failure mode.  The synthesized
 * interior is the analytic triangle itself, so the known-good case
 * proves the classifier agrees with its own geometry and each mutation
 * proves a distinct counter moves.
 */
static void
test_coverage_oracle_calibration(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const uint32_t interior = 0xe02060a0u, exterior = 0x40404040u;
   const uint32_t pitch = shape.pitch_pixels;
   const uint32_t dwords =
      pitch * (shape.height + R300_TRIANGLE_CANARY_ROWS);
   uint32_t *pixels = calloc(dwords, sizeof(*pixels));
   assert(pixels != NULL);

   struct r300_triangle_coverage_verdict good;
   for (uint32_t i = 0; i < dwords; i++)
      pixels[i] = exterior;
   /* The reference geometry: NDC (-0.75, -0.75), (0.75, -0.75),
    * (0, 0.75) through the viewport transform at the 64x64 extent,
    * giving window vertices (8, 8), (56, 8), (32, 56).
    */
   uint32_t painted = 0;
   for (uint32_t y = 0; y < shape.height; y++) {
      for (uint32_t x = 0; x < shape.width; x++) {
         if (sample_inside_reference((float)x + 0.5f, (float)y + 0.5f)) {
            pixels[y * pitch + x] = interior;
            painted++;
         }
      }
   }
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 1, exterior,
                                            pixels, dwords * 4u, &good);
   assert(good.coverage_exact && good.canary_pass);
   assert(good.mismatch_pixels == 0 && good.ambiguous_pixels == 0);
   assert(good.interior_pixels == good.analytic_pixels);
   /* Base 48 pixels and height 48 give the half-open triangle's 1152
    * covered centers, so the count is fixed rather than whatever the
    * classifier happened to produce.
    */
   assert(good.analytic_pixels == 1152 && painted == 1152);
   assert(good.exterior_pixels == shape.width * shape.height - 1152);

   /* Underdraw: one interior pixel left at the exterior value. */
   struct r300_triangle_coverage_verdict bad;
   pixels[32 * pitch + 32] = exterior;
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 1, exterior,
                                            pixels, dwords * 4u, &bad);
   assert(!bad.coverage_exact && bad.canary_pass);
   assert(bad.interior_pixels == good.interior_pixels - 1);
   assert(bad.mismatch_pixels == 0);
   pixels[32 * pitch + 32] = interior;

   /* Overdraw: one exterior pixel taking the interior value. */
   pixels[0] = interior;
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 1, exterior,
                                            pixels, dwords * 4u, &bad);
   assert(!bad.coverage_exact);
   assert(bad.interior_pixels == good.interior_pixels + 1);
   pixels[0] = exterior;

   /* A third value inside the extent is neither expectation. */
   pixels[0] = 0x12345678u;
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 1, exterior,
                                            pixels, dwords * 4u, &bad);
   assert(!bad.coverage_exact && bad.mismatch_pixels == 1);
   pixels[0] = exterior;

   /* A write in the canary row past the extent leaves the classified
    * band exact and fails the canary alone.
    */
   pixels[shape.height * pitch] = interior;
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 1, exterior,
                                            pixels, dwords * 4u, &bad);
   assert(bad.coverage_exact && !bad.canary_pass);
   pixels[shape.height * pitch] = exterior;

   /* A footprint short of the canary rows refuses rather than judging a
    * band it cannot read.
    */
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 1, exterior,
                                            pixels, dwords * 4u - 4u, &bad);
   assert(!bad.coverage_exact && bad.analytic_pixels == 0);

   /* An empty admitted-interior set refuses; every drawn pixel would
    * otherwise read as a mismatch and the verdict would describe the
    * call rather than the device.
    */
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 0, exterior,
                                            pixels, dwords * 4u, &bad);
   assert(!bad.coverage_exact && bad.interior_pixels == 0);

   /* Two admitted interior values: a split fragment source paints both
    * halves and the verdict stays exact, since placement is the
    * caller's own check.
    */
   const uint32_t split[2] = { interior, 0xd0905010u };
   for (uint32_t y = shape.height / 2; y < shape.height; y++) {
      for (uint32_t x = 0; x < shape.width; x++) {
         if (pixels[y * pitch + x] == interior)
            pixels[y * pitch + x] = split[1];
      }
   }
   r300_tcl_bypass_triangle_coverage_oracle(&shape, split, 2, exterior,
                                            pixels, dwords * 4u, &bad);
   assert(bad.coverage_exact && bad.canary_pass);
   assert(bad.interior_pixels == good.interior_pixels);
   /* One admitted value alone reads the other half as underdraw, so the
    * admitted set is what the verdict rests on.
    */
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &interior, 1, exterior,
                                            pixels, dwords * 4u, &bad);
   assert(!bad.coverage_exact && bad.mismatch_pixels > 0);

   free(pixels);
}


/* The composed cell names its first target twice -- the render half
 * writes it, the sample half reads it as the texture -- and a winsys
 * that merges duplicate handles gives that buffer one relocation entry,
 * so the slot-numbered payloads the emitter writes stop naming the
 * buffers they were emitted for.  The merge rule here mirrors
 * radeon_drm_vk_reloc_list_add: first-add order, one entry per handle.
 */
static void
test_composed_reloc_payloads_bind_to_merged_indices(void)
{
   struct r300_triangle_composed_render_sample composed;
   r300_tcl_bypass_triangle_render_shape_reference(&composed.render);
   r300_tcl_bypass_triangle_render_shape_reference(&composed.sample);
   composed.sample.target_offset = 0;
   struct r300_tcl_bypass_triangle_ib ib;
   assert(r300_tcl_bypass_triangle_composed_render_sample_emit(&composed,
                                                              &ib) == 0);

   /* One handle per slot, with the render half's color and the sample
    * half's texture sharing handle 7.
    */
   uint32_t handle_of_slot[R300_TRIANGLE_SLOT_COUNT];
   handle_of_slot[R300_TRIANGLE_SLOT_VERTEX] = 5;
   handle_of_slot[R300_TRIANGLE_SLOT_COLOR] = 7;
   handle_of_slot[R300_TRIANGLE_SLOT_TEXTURE] = 7;
   handle_of_slot[R300_TRIANGLE_SLOT_COMPOSED_VERTEX] = 9;
   handle_of_slot[R300_TRIANGLE_SLOT_COMPOSED_COLOR] = 11;

   uint32_t merged[R300_TRIANGLE_SLOT_COUNT];
   uint32_t merged_handles[R300_TRIANGLE_SLOT_COUNT];
   uint32_t merged_count = 0;
   for (uint32_t slot = 0; slot < R300_TRIANGLE_SLOT_COUNT; slot++) {
      uint32_t found = merged_count;
      for (uint32_t i = 0; i < merged_count; i++) {
         if (merged_handles[i] == handle_of_slot[slot]) {
            found = i;
            break;
         }
      }
      if (found == merged_count)
         merged_handles[merged_count++] = handle_of_slot[slot];
      merged[slot] = found;
   }
   /* Five slots over four buffer objects, so the texture shares the
    * color's entry and every later slot shifts down one.
    */
   assert(merged_count == 4);
   assert(merged[R300_TRIANGLE_SLOT_TEXTURE] ==
          merged[R300_TRIANGLE_SLOT_COLOR]);
   assert(merged[R300_TRIANGLE_SLOT_COMPOSED_COLOR] == 3);

   /* The emitted payload names the slot, which the merged chunk no
    * longer agrees with at three of the five sites.
    */
   uint32_t disagreements = 0;
   for (uint32_t i = 0; i < ib.reloc_site_count; i++) {
      const uint32_t slot = ib.reloc_sites[i].slot;
      if (ib.ib[ib.reloc_sites[i].ib_index] != merged[slot] * 4u)
         disagreements++;
   }
   assert(disagreements == 3);

   assert(r300_tcl_bypass_triangle_bind_reloc_indices(
             &ib, merged, R300_TRIANGLE_SLOT_COUNT) == 0);
   for (uint32_t i = 0; i < ib.reloc_site_count; i++) {
      const uint32_t slot = ib.reloc_sites[i].slot;
      assert(ib.ib[ib.reloc_sites[i].ib_index] == merged[slot] * 4u);
   }
   /* Binding leaves the emitted form behind, so a second bind refuses
    * rather than remapping indices that already name the merged chunk.
    */
   assert(r300_tcl_bypass_triangle_bind_reloc_indices(
             &ib, merged, R300_TRIANGLE_SLOT_COUNT) != 0);
   r300_tcl_bypass_triangle_release(&ib);

   /* A cell whose slots name distinct buffers takes the identity map,
    * where binding is the emitted form itself and the sites still
    * validate.
    */
   struct r300_tcl_bypass_triangle_ib plain;
   assert(r300_tcl_bypass_triangle_render_shape_emit(&composed.render,
                                                     &plain) == 0);
   const uint32_t identity[R300_TRIANGLE_SLOT_COUNT] = { 0, 1, 2, 3, 4 };
   assert(r300_tcl_bypass_triangle_bind_reloc_indices(
             &plain, identity, R300_TRIANGLE_SLOT_COUNT) == 0);
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&plain) == 0);

   /* A map short of the slots the cell references refuses, so a caller
    * that forgot the composed slots cannot leave a payload unbound.
    */
   struct r300_tcl_bypass_triangle_ib composed_again;
   assert(r300_tcl_bypass_triangle_composed_render_sample_emit(
             &composed, &composed_again) == 0);
   assert(r300_tcl_bypass_triangle_bind_reloc_indices(&composed_again, merged,
                                                      3u) == -EINVAL);
   /* The refusal left every payload in its emitted form. */
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&composed_again) == 0);
   r300_tcl_bypass_triangle_release(&composed_again);
   r300_tcl_bypass_triangle_release(&plain);
}


/* The split model the varying cell realizes: TEX0's t is 0.125 at the
 * two base vertices and 0.875 at the apex, and both base vertices sit
 * at window y 8 while the apex sits at y 56, so t is linear in y alone
 * -- t = 0.125 + (y - 8) / 64 -- and a texture split at its midpoint
 * changes texel at the center row where t reaches 0.5.
 */
static uint32_t
split_expectation(void *data, uint32_t x, uint32_t y)
{
   const uint32_t *texel = data;
   (void)x;
   const float t = 0.125f + ((float)y + 0.5f - 8.0f) / 64.0f;
   return t < 0.5f ? texel[0] : texel[1];
}

/* Calibrates the predicted form: a model that names the right dword at
 * every interior center passes, and a model shifted by one row fails,
 * so the verdict rests on placement rather than on the admitted set.
 */
static void
test_coverage_oracle_predicted_calibration(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   uint32_t texel[2] = { 0xe02060a0u, 0xd0905010u };
   const uint32_t exterior = 0x40404040u;
   const uint32_t pitch = shape.pitch_pixels;
   const uint32_t dwords = pitch * (shape.height + R300_TRIANGLE_CANARY_ROWS);
   uint32_t *pixels = calloc(dwords, sizeof(*pixels));
   assert(pixels != NULL);
   for (uint32_t i = 0; i < dwords; i++)
      pixels[i] = exterior;

   const float vx[3] = { 8.0f, 56.0f, 32.0f }, vy[3] = { 8.0f, 8.0f, 56.0f };
   uint32_t upper = 0, lower = 0;
   for (uint32_t y = 0; y < shape.height; y++) {
      for (uint32_t x = 0; x < shape.width; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         bool in = true;
         for (unsigned e = 0; e < 3; e++) {
            const unsigned n = (e + 1) % 3;
            in &= (vx[n] - vx[e]) * (py - vy[e]) -
                     (vy[n] - vy[e]) * (px - vx[e]) >
                  0.0f;
         }
         if (!in)
            continue;
         const uint32_t want = split_expectation(texel, x, y);
         pixels[y * pitch + x] = want;
         if (want == texel[0])
            upper++;
         else
            lower++;
      }
   }
   /* The crossing row and the triangle's widening toward its base fix
    * the two counts, so the split is a stated quantity rather than
    * whatever the loop produced.
    */
   assert(upper == 864 && lower == 288);

   struct r300_triangle_coverage_verdict v;
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &shape, NULL, 0, split_expectation, texel, exterior, pixels,
      dwords * 4u, &v);
   assert(v.coverage_exact && v.canary_pass);
   assert(v.interior_pixels == 1152 && v.mismatch_pixels == 0);

   /* The admitted-set form passes on the same bytes without judging
    * placement, so the two forms differ in what they prove.
    */
   r300_tcl_bypass_triangle_coverage_oracle(&shape, texel, 2, exterior,
                                            pixels, dwords * 4u, &v);
   assert(v.coverage_exact);

   /* A model whose texels are exchanged names the wrong dword at every
    * interior center, which the admitted-set form cannot see.
    */
   uint32_t swapped[2] = { texel[1], texel[0] };
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &shape, NULL, 0, split_expectation, swapped, exterior, pixels,
      dwords * 4u, &v);
   assert(!v.coverage_exact && v.mismatch_pixels == 1152);

   /* One interior pixel taking the other half's texel fails the
    * predicted form alone.
    */
   pixels[20 * pitch + 32] = texel[1];
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &shape, NULL, 0, split_expectation, texel, exterior, pixels,
      dwords * 4u, &v);
   assert(!v.coverage_exact && v.mismatch_pixels == 1);
   r300_tcl_bypass_triangle_coverage_oracle(&shape, texel, 2, exterior,
                                            pixels, dwords * 4u, &v);
   assert(v.coverage_exact);
   pixels[20 * pitch + 32] = texel[0];

   /* A cell whose fragment color arrives through the TX unit carries no
    * R300_PFS_PARAM_0 constant, so its color_bits sit wherever the
    * caller left them.  0x3e008081 is one ulp off the FP24 s1e7m16
    * lattice -- r300_fp24_quantize_bits carries it to 0x3e008080 -- and
    * the emitter's admission refuses it while the verdict, which reads
    * geometry and takes its interior values as arguments, still judges.
    */
   struct r300_triangle_render_shape tx_sourced = shape;
   for (unsigned i = 0; i < 4; i++)
      tx_sourced.color_bits[i] = 0x3e008081u;
   assert(r300_fp24_quantize_bits(0x3e008081u) == 0x3e008080u);
   assert(r300_tcl_bypass_triangle_render_shape_validate(&tx_sourced) ==
          -EINVAL);
   assert(r300_tcl_bypass_triangle_render_shape_validate_geometry(
             &tx_sourced) == 0);
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &tx_sourced, NULL, 0, split_expectation, texel, exterior, pixels,
      dwords * 4u, &v);
   assert(v.judged && v.coverage_exact && v.canary_pass);
   assert(v.interior_pixels == 1152 && v.mismatch_pixels == 0);

   /* Inadmissible geometry still refuses, and the refusal is legible:
    * judged stays false where a total mismatch would report counters.
    */
   struct r300_triangle_render_shape bad_pitch = shape;
   bad_pitch.pitch_pixels = shape.pitch_pixels + 1u;
   assert(r300_tcl_bypass_triangle_render_shape_validate_geometry(
             &bad_pitch) == -EINVAL);
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &bad_pitch, NULL, 0, split_expectation, texel, exterior, pixels,
      dwords * 4u, &v);
   assert(!v.judged && !v.coverage_exact && v.interior_pixels == 0);

   struct r300_triangle_render_shape bad_offset = shape;
   bad_offset.target_offset = 4u;
   assert(r300_tcl_bypass_triangle_render_shape_validate_geometry(
             &bad_offset) == -EINVAL);
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &bad_offset, NULL, 0, split_expectation, texel, exterior, pixels,
      dwords * 4u, &v);
   assert(!v.judged);

   /* The judged flag separates a refusal from a judged total mismatch,
    * which both report every counter zero on coverage_exact alone.
    */
   for (uint32_t i = 0; i < dwords; i++)
      pixels[i] = 0xdeadbeefu;
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &shape, NULL, 0, split_expectation, texel, exterior, pixels,
      dwords * 4u, &v);
   assert(v.judged && !v.coverage_exact && v.mismatch_pixels == 4096);

   free(pixels);
}


/* Calibrates the extent-parameterized varying writer against the
 * reference array it reproduces, and shows the positions scaling while
 * the normalized TEX0 payload holds.
 */
static void
test_varying_shape_vertices_scale_positions_alone(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   float records[R300_TRIANGLE_VARYING_VERTEX_DWORDS];
   r300_tcl_bypass_triangle_varying_shape_vertices(&shape, records);
   assert(memcmp(records, r300_tcl_bypass_triangle_varying_vertices,
                 sizeof(records)) == 0);

   /* Half the extent halves every window coordinate and leaves the
    * texture coordinate where the fetch reads it.
    */
   shape.width = R300_TRIANGLE_TARGET_WIDTH / 2;
   shape.height = R300_TRIANGLE_TARGET_HEIGHT / 2;
   shape.pitch_pixels = R300_TRIANGLE_TARGET_PITCH_PIXELS;
   float halved[R300_TRIANGLE_VARYING_VERTEX_DWORDS];
   r300_tcl_bypass_triangle_varying_shape_vertices(&shape, halved);
   for (unsigned i = 0; i < 3; i++) {
      assert(halved[i * 8 + 0] == records[i * 8 + 0] / 2.0f);
      assert(halved[i * 8 + 1] == records[i * 8 + 1] / 2.0f);
      assert(halved[i * 8 + 2] == 0.0f && halved[i * 8 + 3] == 1.0f);
      for (unsigned c = 0; c < 4; c++)
         assert(halved[i * 8 + 4 + c] == records[i * 8 + 4 + c]);
   }
}

/* The composed cell's two halves address the shared target through
 * different register paths, and each path carries its own tiling
 * fields: RB3D_COLORPITCH0 holds COLORTILE and COLORMICROTILE beside
 * the pitch, while TX_OFFSET_0 holds TXO_MACRO_TILE and TXO_MICRO_TILE
 * in the bits below its base.  A disagreement there would have the
 * texture fetch read a swizzle of what the render half wrote, so the
 * shared target's linear layout is a property of both words.  The
 * 32-byte target alignment the shape validator enforces for
 * RB3D_COLOROFFSET's base field is what holds the TX tiling bits at
 * linear.
 */
static void
test_composed_halves_agree_on_linear_layout(void)
{
   struct r300_triangle_composed_render_sample composed;
   r300_tcl_bypass_triangle_render_shape_reference(&composed.render);
   r300_tcl_bypass_triangle_render_shape_reference(&composed.sample);
   composed.sample.target_offset = 65536;

   struct r300_tcl_bypass_triangle_ib ib;
   assert(r300_tcl_bypass_triangle_composed_render_sample_emit(
             &composed, &ib) == 0);

   const uint32_t tile_bits = R300_COLOR_TILE(1) | R300_COLOR_MICROTILE(3);
   for (uint32_t at = 0, seen = 0; at < ib.ib_size_dwords; at++) {
      const int found = find_reg_write(ib.ib + at, ib.ib_size_dwords - at,
                                       R300_RB3D_COLORPITCH0, false);
      if (found < 0) {
         assert(seen == 2);
         break;
      }
      at += (uint32_t)found;
      assert((ib.ib[at + 1] & tile_bits) == 0);
      assert((ib.ib[at + 1] & R300_COLORPITCH_MASK) ==
             (seen == 0 ? composed.render.pitch_pixels
                        : composed.sample.pitch_pixels));
      seen++;
   }

   const int tx_offset =
      find_reg_write(ib.ib, ib.ib_size_dwords, R300_TX_OFFSET_0, false);
   assert(tx_offset >= 0);
   assert((ib.ib[tx_offset + 1] &
           (R300_TXO_MACRO_TILE(1) | R300_TXO_MICRO_TILE(3))) == 0);

   /* A base off the 32-byte grid would set TXO_MACRO_TILE or
    * TXO_MICRO_TILE while displacing the texture, and the shape
    * validator refuses it before the emitter runs.
    */
   struct r300_triangle_composed_render_sample skewed = composed;
   skewed.render.target_offset = 4;
   struct r300_tcl_bypass_triangle_ib refused;
   assert(r300_tcl_bypass_triangle_composed_render_sample_emit(
             &skewed, &refused) == -EINVAL);

   r300_tcl_bypass_triangle_release(&ib);
}

/* TEX0 at each vertex is that vertex's window position over the render
 * extent, so a nearest fetch from a texture at the render extent reads
 * pixel (x, y) at texel (x, y): the interpolated coordinate at pixel
 * center (x + 0.5, y + 0.5) is (x + 0.5) / width, which scaled by the
 * texture width lands a half texel from either boundary.  The composed
 * cell's sample half therefore reproduces its texture's coverage pixel
 * for pixel, which is what makes one predicted interior dword cover
 * both of its targets.
 */
static void
test_varying_tex0_is_the_window_position_fraction(void)
{
   const uint32_t extents[][2] = { { 64, 64 }, { 32, 32 }, { 128, 64 } };
   for (unsigned e = 0; e < ARRAY_SIZE(extents); e++) {
      struct r300_triangle_render_shape shape;
      r300_tcl_bypass_triangle_render_shape_reference(&shape);
      shape.width = extents[e][0];
      shape.height = extents[e][1];
      shape.pitch_pixels = extents[e][0];
      assert(r300_tcl_bypass_triangle_render_shape_validate(&shape) == 0);

      float records[R300_TRIANGLE_VARYING_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_varying_shape_vertices(&shape, records);
      for (unsigned i = 0; i < 3; i++) {
         assert(records[i * 8 + 4] == records[i * 8 + 0] / (float)shape.width);
         assert(records[i * 8 + 5] == records[i * 8 + 1] / (float)shape.height);
      }
   }
}

/* The multisample resolve cell's emitted form: the subsample registers
 * open the stream, the resolve register run carries the destination's
 * offset, pitch, and mode with its relocation behind it, and both the
 * resolve mode and the subsample set close.  The words are checked
 * against r300g's own values (r300_emit_fb_state_pipelined for the
 * sample positions, r300_emit_aa_state for the resolve run), so a
 * transcription error in either fails here rather than on the one
 * attended submission the cell gets.
 */
static void
test_msaa_resolve_cell(void)
{
   struct r300_triangle_msaa_resolve msaa;
   r300_tcl_bypass_triangle_render_shape_reference(&msaa.render);
   r300_tcl_bypass_triangle_render_shape_reference(&msaa.destination);
   msaa.destination.target_offset = 0;
   msaa.sample_count = 4;
   /* A resolve-half constant no multisample sample holds: the render
    * half's color is the reference shape's, so an opaque green here is
    * distinct from it and from the sentinel.
    */
   const float resolve_rgba[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
   for (unsigned i = 0; i < 4; i++)
      memcpy(&msaa.resolve_color_bits[i], &resolve_rgba[i], sizeof(float));

   /* GB_AA_CONFIG and the MSPOS pair against r300g's own encoding. */
   assert(r300_tcl_bypass_triangle_gb_aa_config(2) == (1u | (0u << 1)));
   assert(r300_tcl_bypass_triangle_gb_aa_config(4) == (1u | (2u << 1)));
   assert(r300_tcl_bypass_triangle_gb_aa_config(1) == 0);
   assert(r300_tcl_bypass_triangle_gb_aa_config(3) == 0);
   assert(r300_tcl_bypass_triangle_gb_aa_config(6) == 0);
   /* 4x samples (4,4) (8,8) (2,10) (10,2) with lanes 4 and 5 repeating
    * (10,2): MSPOS0 packs samples 0-2 then (D0_Y, D0_X) = (2, 2), and
    * MSPOS1 packs samples 3-5 then the minimum over all twelve, 2.
    */
   assert(r300_tcl_bypass_triangle_gb_mspos(0, 4) ==
          (4u | (4u << 4) | (8u << 8) | (8u << 12) | (2u << 16) |
           (10u << 20) | (2u << 24) | (2u << 28)));
   assert(r300_tcl_bypass_triangle_gb_mspos(1, 4) ==
          (10u | (2u << 4) | (10u << 8) | (2u << 12) | (10u << 16) |
           (2u << 20) | (2u << 24)));
   /* 2x samples (3,9) (9,3): every lane past the second repeats (9,3),
    * so both minima are 3.
    */
   assert(r300_tcl_bypass_triangle_gb_mspos(0, 2) ==
          (3u | (9u << 4) | (9u << 8) | (3u << 12) | (9u << 16) |
           (3u << 20) | (3u << 24) | (3u << 28)));
   assert(r300_tcl_bypass_triangle_gb_mspos(0, 3) == 0);
   assert(r300_tcl_bypass_triangle_gb_mspos(2, 4) == 0);

   /* The cover triangle: (0,0), (2w,0), (0,2h) contains every pixel
    * center in the extent, which is what full resolve coverage needs.
    */
   float cover[R300_TRIANGLE_VERTEX_DWORDS];
   r300_tcl_bypass_triangle_cover_vertices(&msaa.render, cover);
   assert(cover[0] == 0.0f && cover[1] == 0.0f);
   assert(cover[4] == 2.0f * (float)msaa.render.width && cover[5] == 0.0f);
   assert(cover[8] == 0.0f && cover[9] == 2.0f * (float)msaa.render.height);
   for (unsigned i = 0; i < 3; i++)
      assert(cover[i * 4 + 2] == 0.0f && cover[i * 4 + 3] == 1.0f);
   for (uint32_t y = 0; y < msaa.render.height; y++)
      for (uint32_t x = 0; x < msaa.render.width; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         assert(px >= 0.0f && py >= 0.0f &&
                px / (2.0f * (float)msaa.render.width) +
                      py / (2.0f * (float)msaa.render.height) <
                   1.0f);
      }

   struct r300_tcl_bypass_triangle_ib ib;
   assert(r300_tcl_bypass_triangle_msaa_resolve_emit(&msaa, &ib) == 0);
   assert(r300_tcl_bypass_triangle_validate_reloc_sites(&ib) == 0);

   /* The subsample set travels through each half's first-draw contract,
    * which writes GB_AA_CONFIG, both GB_MSPOS words, and
    * RB3D_AARESOLVE_CTL at their single-sample values.  A set programmed
    * ahead of the contract is written back before the draw the set was
    * meant for, so the pinned invariant is the sequence of values each
    * register carries across the stream, not the position of a prologue.
    */
   uint32_t aa_config_values[8], mspos0_values[8], resolve_ctl_values[8];
   uint32_t aa_config_count = 0, mspos0_count = 0, resolve_ctl_count = 0;
   for (uint32_t i = 0; i < ib.ib_size_dwords;) {
      const uint32_t header = ib.ib[i];
      if ((header >> 30) != 0) {
         /* PACKET3 carries a count in the same field; the cell emits no
          * other packet type, so the walk steps over both shapes.
          */
         i += 2 + ((header >> 16) & 0x3fff);
         continue;
      }
      const uint32_t count = ((header >> 16) & 0x3fff) + 1;
      const uint32_t reg = (header & 0x1fff) * 4;
      for (uint32_t k = 0; k < count && i + 1 + k < ib.ib_size_dwords; k++) {
         const uint32_t at = reg + 4 * k;
         const uint32_t value = ib.ib[i + 1 + k];
         if (at == R300_GB_AA_CONFIG && aa_config_count < 8)
            aa_config_values[aa_config_count++] = value;
         else if (at == R300_GB_MSPOS0 && mspos0_count < 8)
            mspos0_values[mspos0_count++] = value;
         else if (at == R300_RB3D_AARESOLVE_CTL && resolve_ctl_count < 8)
            resolve_ctl_values[resolve_ctl_count++] = value;
      }
      i += 1 + count;
   }

   /* Each half's contract arms the subsample set for its own draw, and
    * the epilogue closes it: three writes, the last one the disable.
    */
   assert(aa_config_count == 3);
   assert(aa_config_values[0] == r300_tcl_bypass_triangle_gb_aa_config(4));
   assert(aa_config_values[1] == r300_tcl_bypass_triangle_gb_aa_config(4));
   assert(aa_config_values[2] == R300_GB_AA_CONFIG_AA_DISABLE);
   assert(mspos0_count == 2);
   assert(mspos0_values[0] == r300_tcl_bypass_triangle_gb_mspos(0, 4));
   assert(mspos0_values[1] == r300_tcl_bypass_triangle_gb_mspos(0, 4));

   /* Resolve mode is live for the second half alone: the render half
    * draws into the multisample surface under NORMAL, the resolve half's
    * own contract arms RESOLVE, and the epilogue closes it.
    */
   assert(resolve_ctl_count == 3);
   assert(resolve_ctl_values[0] ==
          R300_RB3D_AARESOLVE_CTL_AARESOLVE_MODE_NORMAL);
   assert(resolve_ctl_values[1] ==
          (R300_RB3D_AARESOLVE_CTL_AARESOLVE_MODE_RESOLVE |
           R300_RB3D_AARESOLVE_CTL_AARESOLVE_ALPHA_AVERAGE));
   assert(resolve_ctl_values[2] ==
          R300_RB3D_AARESOLVE_CTL_AARESOLVE_MODE_NORMAL);

   /* The destination's base and pitch are the caller's alone -- no
    * contract entry names them -- so one PACKET0 run carries both with
    * the relocation behind it, ahead of the contract that arms the mode.
    */
   uint32_t run = 0;
   bool found_run = false;
   for (uint32_t i = 0; i + 3 < ib.ib_size_dwords; i++) {
      if (ib.ib[i] == CP_PACKET0(R300_RB3D_AARESOLVE_OFFSET, 1)) {
         assert(!found_run);
         found_run = true;
         run = i;
      }
   }
   assert(found_run);
   assert(ib.ib[run + 1] == msaa.destination.target_offset);
   assert(ib.ib[run + 2] ==
          (msaa.destination.pitch_pixels &
           (uint32_t)R300_RB3D_AARESOLVE_PITCH_MASK));
   assert(ib.ib[run + 3] == CP_PACKET3(R300_PM4_PACKET3_NOP, 0));

   /* Four buffer objects reach the cell: the render half's vertices and
    * the multisample surface, the resolve half's cover vertices, and the
    * resolve destination.  The multisample surface carries two sites.
    */
   uint32_t seen[R300_TRIANGLE_SLOT_COUNT] = { 0 };
   for (uint32_t i = 0; i < ib.reloc_site_count; i++)
      seen[ib.reloc_sites[i].slot]++;
   assert(seen[R300_TRIANGLE_SLOT_VERTEX] == 1);
   assert(seen[R300_TRIANGLE_SLOT_COLOR] == 1);
   /* The resolve half's second binding of the multisample surface takes
    * the texture slot, which the map resolves to the color slot's entry.
    */
   assert(seen[R300_TRIANGLE_SLOT_TEXTURE] == 1);
   assert(seen[R300_TRIANGLE_SLOT_COMPOSED_VERTEX] == 1);
   assert(seen[R300_TRIANGLE_SLOT_COMPOSED_COLOR] == 1);

   /* The validator admits two five-site sequences, so a five-site
    * sequence matching neither still refuses: swapping the resolve
    * destination's slot with the cover geometry's leaves every slot
    * present exactly once and every site rising, and the stream is
    * still rejected on order alone.
    */
   {
      struct r300_tcl_bypass_triangle_ib mutated = ib;
      uint32_t *copy = malloc(ib.ib_size_dwords * sizeof(uint32_t));
      assert(copy != NULL);
      memcpy(copy, ib.ib, ib.ib_size_dwords * sizeof(uint32_t));
      mutated.ib = copy;
      mutated.owns_ib = false;
      const uint32_t a = 2, b = 4;
      const uint32_t slot_a = mutated.reloc_sites[a].slot;
      mutated.reloc_sites[a].slot = mutated.reloc_sites[b].slot;
      mutated.reloc_sites[b].slot = slot_a;
      /* The payload is the slot's dword index into the relocation
       * chunk, so it moves with the slot.
       */
      copy[mutated.reloc_sites[a].ib_index] = mutated.reloc_sites[a].slot * 4;
      copy[mutated.reloc_sites[b].ib_index] = mutated.reloc_sites[b].slot * 4;
      assert(r300_tcl_bypass_triangle_validate_reloc_sites(&mutated) != 0);
      free(copy);
   }

   /* The merged map binds without refusing, and binding twice refuses. */
   assert(r300_tcl_bypass_triangle_bind_reloc_indices(
             &ib, r300_tcl_bypass_triangle_msaa_slot_index,
             R300_TRIANGLE_SLOT_COUNT) == 0);
   assert(r300_tcl_bypass_triangle_bind_reloc_indices(
             &ib, r300_tcl_bypass_triangle_msaa_slot_index,
             R300_TRIANGLE_SLOT_COUNT) != 0);
   r300_tcl_bypass_triangle_release(&ib);

   /* Refusals: a sample count with no subsample set, a destination
    * pitch outside the resolve register's mask, and an unaligned
    * destination offset each refuse before any word is emitted.
    */
   struct r300_triangle_msaa_resolve bad = msaa;
   bad.sample_count = 3;
   assert(r300_tcl_bypass_triangle_msaa_resolve_emit(&bad, &ib) == -EINVAL);
   bad = msaa;
   bad.destination.target_offset = 4;
   assert(r300_tcl_bypass_triangle_msaa_resolve_emit(&bad, &ib) == -EINVAL);
   assert(r300_tcl_bypass_triangle_msaa_resolve_emit(NULL, &ib) == -EINVAL);

   /* The two shapes admit on different predicates.  The render half's
    * color_bits reach R300_PFS_PARAM_0, so an off-lattice constant
    * there names a value other than the one the emitter declares and
    * refuses.  The destination's reach the AA register run, which
    * carries offset and pitch alone, so the same off-lattice word there
    * admits and the cell emits.
    */
   bad = msaa;
   for (unsigned i = 0; i < 4; i++)
      bad.render.color_bits[i] = 0x3e008081u;
   assert(r300_tcl_bypass_triangle_msaa_resolve_emit(&bad, &ib) == -EINVAL);
   bad = msaa;
   for (unsigned i = 0; i < 4; i++)
      bad.destination.color_bits[i] = 0x3e008081u;
   assert(r300_tcl_bypass_triangle_msaa_resolve_emit(&bad, &ib) == 0);
   r300_tcl_bypass_triangle_release(&ib);
}

/* Calibrates the sample-set verdict against the pixel-center one.  The
 * two footprints stand in a containment relation -- a pixel whose whole
 * sample set clears the edges has its center inside -- so painting the
 * center set satisfies the sample-set oracle and cannot refute it.  The
 * discriminating arm runs the other way: a footprint painted to the 4x
 * sample-set predicate leaves the 48 pixels the two disagree on
 * unpainted, which the center oracle counts as analytic and refuses.
 * A calibration that generated its footprint from the predicate under
 * test would pass every arm, so the judged counts are pinned against an
 * independent enumeration in exact rational arithmetic: 1152 at one
 * sample, 1128 at two, 1104 at four.
 */
static void
test_sample_set_oracle_calibration(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const uint32_t admitted = 0x11223344u;
   const uint32_t footprint =
      shape.pitch_pixels * (shape.height + R300_TRIANGLE_CANARY_ROWS);
   uint32_t *pixels = malloc((size_t)footprint * sizeof(uint32_t));
   assert(pixels != NULL);

   /* Judged and unjudged pixels per sample count.  One sample sits at
    * the pixel center, so it reduces to the center oracle's footprint
    * with nothing unjudged; wider sets trade judged pixels for the
    * edge band the resolve blends.
    */
   const uint32_t counts[3] = { 1, 2, 4 };
   const uint32_t judged[3] = { 1152, 1128, 1104 };
   const uint32_t unjudged[3] = { 0, 48, 96 };

   /* The center footprint contains every sample-set footprint, so each
    * sample count passes on it while judging strictly fewer pixels as
    * the samples spread.
    */
   for (uint32_t i = 0; i < footprint; i++)
      pixels[i] = 0xdead0000u + (i & 0xffffu);
   for (uint32_t y = 0; y < shape.height; y++)
      for (uint32_t x = 0; x < shape.width; x++)
         if (sample_inside_reference((float)x + 0.5f, (float)y + 0.5f))
            pixels[y * shape.pitch_pixels + x] = admitted;

   struct r300_triangle_sample_set_verdict verdict;
   for (unsigned i = 0; i < 3; i++) {
      r300_tcl_bypass_triangle_sample_set_oracle(&shape, counts[i], &admitted,
                                                 1, pixels,
                                                 footprint * sizeof(uint32_t),
                                                 &verdict);
      assert(verdict.interior_exact);
      assert(verdict.analytic_pixels == judged[i]);
      assert(verdict.interior_pixels == judged[i]);
      assert(verdict.unjudged_pixels == unjudged[i]);
   }

   /* The discriminating arm: paint only the pixels whose whole 4x sample
    * set clears the edges.  The sample-set verdict passes and the
    * pixel-center verdict refuses on exactly the pixels the two
    * denominators disagree on, so a run separates them.
    */
   uint8_t positions[R300_TRIANGLE_MAX_SUBSAMPLES][2];
   assert(r300_tcl_bypass_triangle_subsample_positions(4, positions) == 4);
   for (uint32_t i = 0; i < footprint; i++)
      pixels[i] = 0xdead0000u + (i & 0xffffu);
   uint32_t painted = 0;
   for (uint32_t y = 0; y < shape.height; y++) {
      for (uint32_t x = 0; x < shape.width; x++) {
         bool all_in = true;
         for (unsigned s = 0; s < 4; s++)
            all_in &= sample_inside_reference(
               (float)x + (float)positions[s][0] / 12.0f,
               (float)y + (float)positions[s][1] / 12.0f);
         if (all_in) {
            pixels[y * shape.pitch_pixels + x] = admitted;
            painted++;
         }
      }
   }
   assert(painted == 1104);
   r300_tcl_bypass_triangle_sample_set_oracle(
      &shape, 4, &admitted, 1, pixels, footprint * sizeof(uint32_t), &verdict);
   assert(verdict.interior_exact && verdict.interior_pixels == 1104);

   struct r300_triangle_interior_verdict center;
   r300_tcl_bypass_triangle_interior_oracle(
      &shape, &admitted, 1, pixels, footprint * sizeof(uint32_t), &center);
   assert(!center.interior_exact);
   assert(center.analytic_pixels == 1152);
   assert(center.interior_pixels == 1104);

   /* One judged dword off refuses, and the count names how many. */
   uint32_t flipped = 0;
   for (uint32_t y = 0; y < shape.height && flipped == 0; y++)
      for (uint32_t x = 0; x < shape.width && flipped == 0; x++)
         if (pixels[y * shape.pitch_pixels + x] == admitted) {
            pixels[y * shape.pitch_pixels + x] = ~admitted;
            flipped = 1;
         }
   assert(flipped == 1);
   r300_tcl_bypass_triangle_sample_set_oracle(
      &shape, 4, &admitted, 1, pixels, footprint * sizeof(uint32_t), &verdict);
   assert(!verdict.interior_exact && verdict.interior_pixels == 1103);

   /* A resolve that wrote nothing leaves the whole footprint at the
    * pattern: the failure the destination takes when the resolve never
    * reached it, separated from a correct write by the counter.
    */
   for (uint32_t i = 0; i < footprint; i++)
      pixels[i] = 0xdead0000u + (i & 0xffffu);
   r300_tcl_bypass_triangle_sample_set_oracle(
      &shape, 4, &admitted, 1, pixels, footprint * sizeof(uint32_t), &verdict);
   assert(!verdict.interior_exact && verdict.interior_pixels == 0);
   assert(verdict.analytic_pixels == 1104);

   /* A buffer short of the rendered rows, an empty admitted set, a
    * sample count no MSPOS set covers, and an inadmissible shape each
    * refuse with a zero denominator rather than a pass.
    */
   r300_tcl_bypass_triangle_sample_set_oracle(
      &shape, 4, &admitted, 1, pixels,
      (size_t)shape.pitch_pixels * shape.height * sizeof(uint32_t) - 4,
      &verdict);
   assert(!verdict.interior_exact && verdict.analytic_pixels == 0);
   r300_tcl_bypass_triangle_sample_set_oracle(
      &shape, 4, &admitted, 0, pixels, footprint * sizeof(uint32_t), &verdict);
   assert(!verdict.interior_exact && verdict.analytic_pixels == 0);
   r300_tcl_bypass_triangle_sample_set_oracle(
      &shape, 3, &admitted, 1, pixels, footprint * sizeof(uint32_t), &verdict);
   assert(!verdict.interior_exact && verdict.analytic_pixels == 0);
   assert(r300_tcl_bypass_triangle_subsample_positions(3, positions) == 0);
   struct r300_triangle_render_shape bad = shape;
   bad.target_offset = 4;
   r300_tcl_bypass_triangle_sample_set_oracle(
      &bad, 4, &admitted, 1, pixels, footprint * sizeof(uint32_t), &verdict);
   assert(!verdict.interior_exact && verdict.analytic_pixels == 0);

   free(pixels);
}

/* Calibrates the interior-only verdict: a footprint whose analytic
 * interior holds the admitted dword passes with arbitrary bytes
 * everywhere else, which is the property an uncleared target needs, and
 * one mutation per failure mode refuses.  The exterior arm is the
 * discriminating one -- a verdict that read the exterior would fail it,
 * and a verdict that read nothing would pass every arm.
 */
static void
test_interior_oracle_calibration(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const uint32_t admitted = 0x11223344u;
   const uint32_t footprint =
      shape.pitch_pixels * (shape.height + R300_TRIANGLE_CANARY_ROWS);
   uint32_t *pixels = malloc((size_t)footprint * sizeof(uint32_t));
   assert(pixels != NULL);

   /* Everything outside the analytic interior takes a per-dword pattern
    * no admitted value equals, so a verdict reading it would refuse.
    */
   for (uint32_t i = 0; i < footprint; i++)
      pixels[i] = 0xdead0000u + (i & 0xffffu);
   /* The reference geometry: window vertices (8, 8), (56, 8), (32, 56),
    * whose half-open coverage is 1152 of the 4096 centers.
    */
   uint32_t painted = 0;
   for (uint32_t y = 0; y < shape.height; y++) {
      for (uint32_t x = 0; x < shape.width; x++) {
         if (sample_inside_reference((float)x + 0.5f, (float)y + 0.5f)) {
            pixels[y * shape.pitch_pixels + x] = admitted;
            painted++;
         }
      }
   }
   assert(painted == 1152);

   struct r300_triangle_interior_verdict verdict;
   r300_tcl_bypass_triangle_interior_oracle(&shape, &admitted, 1, pixels,
                                            footprint * sizeof(uint32_t),
                                            &verdict);
   assert(verdict.interior_exact);
   assert(verdict.analytic_pixels == painted);
   assert(verdict.interior_pixels == painted);
   assert(verdict.ambiguous_pixels == 0);

   /* One interior dword off refuses, and the count names how many. */
   const uint32_t apex_y = shape.height / 2;
   pixels[apex_y * shape.pitch_pixels + shape.width / 2] = ~admitted;
   r300_tcl_bypass_triangle_interior_oracle(&shape, &admitted, 1, pixels,
                                            footprint * sizeof(uint32_t),
                                            &verdict);
   assert(!verdict.interior_exact);
   assert(verdict.interior_pixels == painted - 1);
   pixels[apex_y * shape.pitch_pixels + shape.width / 2] = admitted;

   /* A device that wrote nothing leaves the whole footprint at the
    * pattern: the failure mode an uncleared resolve destination takes,
    * and the one the verdict has to separate from a correct write.
    */
   for (uint32_t i = 0; i < footprint; i++)
      pixels[i] = 0xdead0000u + (i & 0xffffu);
   r300_tcl_bypass_triangle_interior_oracle(&shape, &admitted, 1, pixels,
                                            footprint * sizeof(uint32_t),
                                            &verdict);
   assert(!verdict.interior_exact && verdict.interior_pixels == 0);
   assert(verdict.analytic_pixels == painted);

   /* A buffer short of the rendered rows refuses before it reads, and
    * the refusal reports a zero denominator rather than a pass.
    */
   r300_tcl_bypass_triangle_interior_oracle(
      &shape, &admitted, 1, pixels,
      (size_t)shape.pitch_pixels * shape.height * sizeof(uint32_t) - 4,
      &verdict);
   assert(!verdict.interior_exact && verdict.analytic_pixels == 0);

   /* An empty admitted set and an inadmissible shape refuse the same
    * way, so a caller that forgot its prediction cannot read a pass.
    */
   r300_tcl_bypass_triangle_interior_oracle(&shape, &admitted, 0, pixels,
                                            footprint * sizeof(uint32_t),
                                            &verdict);
   assert(!verdict.interior_exact && verdict.analytic_pixels == 0);
   struct r300_triangle_render_shape bad = shape;
   bad.pitch_pixels = 1;
   r300_tcl_bypass_triangle_interior_oracle(&bad, &admitted, 1, pixels,
                                            footprint * sizeof(uint32_t),
                                            &verdict);
   assert(!verdict.interior_exact && verdict.analytic_pixels == 0);

   free(pixels);
}

int
main(void)
{
   test_sampled_cell_stream_and_refusals();
   test_composed_render_sample_cell();
   test_composed_reloc_payloads_bind_to_merged_indices();
   test_family_emit_deviates_in_count_words_alone();
   test_contract_cell_size_and_digest_are_pinned();
   test_varying_cell_tuple_and_digest_are_pinned();
   test_varying_extent_emit_deviates_in_scissor_words_alone();
   test_varying_oracle_calibration();
   test_emit_into_holds_its_capacity();
   test_emit_into_does_not_own_caller_storage();
   test_reloc_site_mutations_refuse();
   test_stream_satisfies_kernel_contract();
   test_reloc_sites_bind_slots();
   test_reloc_site_validator_refuses_each_defect();
   test_emission_is_deterministic();
   test_contract_emission_is_self_contained();
   test_reference_emit_is_the_single_authority();
   test_emit_rejects_unvalidated_binary();
   test_colorpitch0_pack();
   test_output_oracle();
   test_output_oracle_rejects_bounding_box_overdraw();
   test_extent_oracle_calibration();
   test_extent_emit_deviates_in_scissor_words_alone();
   test_render_shape_deviates_per_parameter();
   test_render_shape_oracle_calibration();
   test_coverage_oracle_calibration();
   test_coverage_oracle_predicted_calibration();
   test_interior_oracle_calibration();
   test_sample_set_oracle_calibration();
   test_msaa_resolve_cell();
   test_pack_unorm8_dword();
   test_varying_shape_vertices_scale_positions_alone();
   test_varying_tex0_is_the_window_position_fraction();
   test_composed_halves_agree_on_linear_layout();
   printf("r300_tcl_bypass_triangle_test: all checks passed\n");
   return 0;
}
