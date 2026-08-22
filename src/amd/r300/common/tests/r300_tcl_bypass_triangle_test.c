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
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"
#include "tests/r300_retained_route_digests.h"

#include "util/macros.h"
#include "util/mesa-blake3.h"

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
   uint32_t vte_cntl, color_channel_mask, max_vtx_index, min_vtx_index;
   bool vte_cntl_seen, color_channel_mask_seen, max_vtx_index_seen,
      min_vtx_index_seen;
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

int
main(void)
{
   test_contract_cell_size_and_digest_are_pinned();
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
   printf("r300_tcl_bypass_triangle_test: all checks passed\n");
   return 0;
}
