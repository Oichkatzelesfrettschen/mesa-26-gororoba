/*
 * SPDX-License-Identifier: MIT
 *
 * Stream, relocation, and verdict controls for the depth control cell.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG

#include "r300_zb_depth_control_cell.h"

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIXELS \
   (R300_ZB_DEPTH_CONTROL_PITCH_PIXELS * \
    R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS)

/* A one-dword PACKET0 run's header, written from the packet grammar
 * rather than from the emitter, so a copied mistake cannot make both
 * sides agree.
 */
static uint32_t
packet0_header(uint32_t reg)
{
   return (0u << 30) | (0u << 16) | ((reg >> 2) & 0x1fffu);
}

static uint32_t
packet3_header(uint32_t opcode, uint32_t count)
{
   return (3u << 30) | ((count - 1u) << 16) | opcode;
}

/* The value of the last one-dword PACKET0 run writing reg, with the
 * number of such runs reported.  The contract prefix and the cell both
 * write the depth registers -- the contract disables them and the cell
 * establishes them -- so the register the hardware sees is the last
 * write, and the count is what distinguishes the two writers.
 */
static uint32_t
reg_last_value(const struct r300_zb_depth_control_ib *cell, uint32_t reg,
               uint32_t *occurrences)
{
   uint32_t value = 0;
   *occurrences = 0;
   for (uint32_t i = 0; i + 1 < cell->ib_size_dwords; i++) {
      if (cell->ib[i] != packet0_header(reg))
         continue;
      value = cell->ib[i + 1];
      (*occurrences)++;
   }
   return value;
}

/* The last value written to reg, asserted to come from the expected
 * number of writers.
 */
static uint32_t
reg_value(const struct r300_zb_depth_control_ib *cell, uint32_t reg,
          uint32_t expected_writes)
{
   uint32_t occurrences = 0;
   const uint32_t value = reg_last_value(cell, reg, &occurrences);
   assert(occurrences == expected_writes);
   return value;
}

/* The reference cell's depth state: every register the depth test and the
 * depth binding depend on, checked against the values the packet grammar
 * and the register definitions give rather than against the emitter.
 */
static void
test_depth_state_words(void)
{
   struct r300_zb_depth_control_ib cell;
   assert(r300_zb_depth_control_reference_emit(&cell) == 0);

   /* The depth resource words carry the REFERENCE_ARTIFACT disposition,
    * which r300_first_draw_contract_resolve drops from the contract, and
    * ZB_CNTL stands outside the table entirely, so the cell is the only
    * writer of the binding and of the enable that arms both the depth
    * test and the kernel's depth-buffer size check.
    */
   assert(reg_value(&cell, R300_ZB_FORMAT, 1) ==
          R300_DEPTHFORMAT_16BIT_INT_Z);
   assert(reg_value(&cell, R300_ZB_DEPTHOFFSET, 1) == 0);
   assert(reg_value(&cell, R300_ZB_DEPTHPITCH, 1) ==
          (R300_ZB_DEPTH_CONTROL_PITCH_PIXELS | R300_DEPTHMACROTILE_DISABLE |
           R300_DEPTHMICROTILE_LINEAR | R300_DEPTHENDIAN(R300_SURF_NO_SWAP)));
   assert(reg_value(&cell, R300_ZB_CNTL, 1) ==
          (R300_Z_ENABLE | R300_Z_WRITE_ENABLE));

   /* The two depth-function clauses are EXPLICIT_DISABLE, which the
    * contract does emit, so each is written twice and the cell's write
    * is the one the hardware acts on.
    */
   assert(reg_value(&cell, R300_ZB_ZSTENCILCNTL, 2) ==
          (uint32_t)(R300_ZS_LESS << R300_Z_FUNC_SHIFT));
   assert(reg_value(&cell, R300_ZB_BW_CNTL, 2) ==
          (R300_HIZ_DISABLE | R300_FAST_FILL_DISABLE));

   /* Both caches publish after the draw: the color path retires the
    * color writes and the Z path the depth writes, so a readback of
    * either surface reads memory rather than a held line.  Each value
    * equals the contract's ordering-barrier value, so the publication
    * restores the state the barrier clause names.
    */
   assert(reg_value(&cell, R300_ZB_ZCACHE_CTLSTAT, 2) ==
          (R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
           R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE));
   assert(reg_value(&cell, R300_RB3D_DSTCACHE_CTLSTAT, 2) ==
          (R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
           R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS));

   r300_zb_depth_control_release(&cell);
   printf("depth state words ok\n");
}

/* One triangle-list draw of six vertices: the array pointer is the only
 * start a DRAW_VBUF_2 has, so both triangles ride one walk.
 */
static void
test_draw_packet(void)
{
   struct r300_zb_depth_control_ib cell;
   assert(r300_zb_depth_control_reference_emit(&cell) == 0);

   const uint32_t draw = r300_zb_depth_control_draw_dword(&cell);
   assert(draw + 1 < cell.ib_size_dwords);
   assert(cell.ib[draw] == packet3_header(R300_PACKET3_3D_DRAW_VBUF_2, 1));
   assert(cell.ib[draw + 1] ==
          (R300_VAP_VF_CNTL__PRIM_TRIANGLES | R300_PRIM_WALK_LIST |
           (6u << R300_PRIM_NUM_VERTICES_SHIFT)));

   /* Exactly one LOAD_VBPNTR: a second vertex array would need a second
    * vertex-slot relocation the site table has no room for.
    */
   uint32_t loads = 0;
   for (uint32_t i = 0; i < cell.ib_size_dwords; i++) {
      if (cell.ib[i] == packet3_header(R300_PACKET3_3D_LOAD_VBPNTR, 3))
         loads++;
   }
   assert(loads == 1);

   r300_zb_depth_control_release(&cell);
   printf("draw packet ok\n");
}

/* The contract bounds the vertex index at five, so the kernel's vertex
 * check admits the sixth vertex the draw walks.
 */
static void
test_contract_vertex_bound(void)
{
   struct r300_first_draw_contract contract;
   assert(r300_zb_depth_control_reference_contract(&contract) == 0);

   bool found_max = false, found_min = false;
   for (uint32_t i = 0; i < contract.count; i++) {
      if (contract.entries[i].reg == R300_VAP_VF_MAX_VTX_INDX) {
         assert(contract.entries[i].value == 5);
         found_max = true;
      }
      if (contract.entries[i].reg == R300_VAP_VF_MIN_VTX_INDX) {
         assert(contract.entries[i].value == 0);
         found_min = true;
      }
      if (contract.entries[i].reg == R300_US_OUT_FMT_0) {
         assert(contract.entries[i].value ==
                (uint32_t)(R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B |
                           R300_C1_SEL_G | R300_C2_SEL_R | R300_C3_SEL_A));
      }
   }
   assert(found_max && found_min);
   printf("contract vertex bound ok\n");
}

/* The poison-model checker over the cell's own contract: the depth
 * comparison is the cell's one departure from it.  The depth resource
 * words carry the REFERENCE_ARTIFACT disposition and never reach the
 * contract, so they are no clauses to depart from, and ZB_BW_CNTL's
 * HIZ_DISABLE and FAST_FILL_DISABLE are zero-valued encodings, so the
 * cell's write reproduces the contract's disabled word.  Every remaining
 * clause, both ordering barriers included, holds its contract value at
 * the draw boundary and at the end of the stream, so arming the depth
 * test leaves the rest of the first-draw contract intact.
 */
static void
test_contract_departure(void)
{
   struct r300_first_draw_contract contract;
   assert(r300_zb_depth_control_reference_contract(&contract) == 0);

   struct r300_zb_depth_control_ib cell;
   assert(r300_zb_depth_control_reference_emit(&cell) == 0);

   static const uint32_t established[] = {
      R300_ZB_ZSTENCILCNTL,
   };
   const uint32_t established_count =
      (uint32_t)(sizeof(established) / sizeof(established[0]));

   struct r300_first_draw_check_report report;
   const uint32_t unsatisfied = r300_first_draw_state_check(
      &contract, cell.ib, cell.ib_size_dwords, 0xdeadbeefu, &report);
   assert(unsatisfied == established_count);

   for (uint32_t i = 0; i < report.unsatisfied_count; i++) {
      const uint32_t reg = contract.entries[report.unsatisfied[i]].reg;
      bool named = false;
      for (uint32_t k = 0; k < established_count; k++)
         named = named || reg == established[k];
      assert(named);
   }

   r300_zb_depth_control_release(&cell);
   printf("contract departure ok\n");
}

static void
test_reloc_sites(void)
{
   struct r300_zb_depth_control_ib cell;
   assert(r300_zb_depth_control_reference_emit(&cell) == 0);
   assert(cell.reloc_site_count == R300_ZB_DEPTH_CONTROL_SLOT_COUNT);
   assert(r300_zb_depth_control_validate_reloc_sites(&cell) == 0);

   /* Stream order, which the validator spells out rather than deriving
    * from the enum: color target, depth surface, vertex array.
    */
   assert(cell.reloc_sites[0].slot == R300_ZB_DEPTH_CONTROL_SLOT_COLOR);
   assert(cell.reloc_sites[1].slot == R300_ZB_DEPTH_CONTROL_SLOT_DEPTH);
   assert(cell.reloc_sites[2].slot == R300_ZB_DEPTH_CONTROL_SLOT_VERTEX);

   /* The depth site is the payload the depth-state emitter reported, so
    * it sits one dword past a relocation NOP header and carries the
    * depth slot's chunk index.
    */
   const uint32_t depth_index = cell.reloc_sites[1].ib_index;
   assert(cell.ib[depth_index] ==
          R300_ZB_DEPTH_CONTROL_SLOT_DEPTH * 4u);
   assert(cell.ib[depth_index - 1] ==
          CP_PACKET3(R300_PM4_PACKET3_NOP, 0));
   /* The depth binding writes ZB_DEPTHOFFSET immediately before its
    * relocation, so the reported index names that packet rather than
    * some other NOP in the stream.
    */
   assert(cell.ib[depth_index - 2] == 0);
   assert(cell.ib[depth_index - 3] == packet0_header(R300_ZB_DEPTHOFFSET));

   /* Known-bad arms: a site outside the stream, a site whose payload
    * names another slot, and a duplicated slot each refuse.
    */
   struct r300_zb_depth_control_ib mutated = cell;
   mutated.reloc_sites[1].ib_index = cell.ib_size_dwords;
   assert(r300_zb_depth_control_validate_reloc_sites(&mutated) == -ERANGE);

   mutated = cell;
   mutated.reloc_sites[1].slot = R300_ZB_DEPTH_CONTROL_SLOT_COLOR;
   assert(r300_zb_depth_control_validate_reloc_sites(&mutated) == -EEXIST);

   mutated = cell;
   mutated.reloc_site_count = R300_ZB_DEPTH_CONTROL_SLOT_COUNT - 1u;
   assert(r300_zb_depth_control_validate_reloc_sites(&mutated) == -EINVAL);

   r300_zb_depth_control_release(&cell);
   printf("reloc sites ok\n");
}

static void
test_refusals(void)
{
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_first_draw_contract contract;
   assert(r300_zb_depth_control_reference_contract(&contract) == 0);

   const struct r300_zb_depth_control_params good = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(
         R300_ZB_DEPTH_CONTROL_PITCH_PIXELS),
      .depth_offset_bytes = 0,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   struct r300_zb_depth_control_ib cell;

   /* A control whose verdict is the difference between two halves of one
    * target cannot inherit a predecessor's depth state, so a null
    * contract refuses.
    */
   struct r300_zb_depth_control_params params = good;
   params.first_draw_contract = NULL;
   assert(r300_zb_depth_control_emit(&params, &cell) == -EINVAL);

   params = good;
   params.fragment_binary = NULL;
   assert(r300_zb_depth_control_emit(&params, &cell) == -EINVAL);

   /* An offset the low five bits reach has no ZB_DEPTHOFFSET encoding. */
   params = good;
   params.depth_offset_bytes = 16;
   assert(r300_zb_depth_control_emit(&params, &cell) == -EINVAL);

   /* Storage one dword short of the cell refuses rather than reporting a
    * truncated stream.
    */
   uint32_t words[R300_ZB_DEPTH_CONTROL_MAX_DWORDS + 4096];
   assert(r300_zb_depth_control_emit(&good, &cell) == 0);
   const uint32_t exact = cell.ib_size_dwords;
   r300_zb_depth_control_release(&cell);
   assert(exact <= (uint32_t)(sizeof(words) / sizeof(words[0])));
   assert(r300_zb_depth_control_emit_into(&good, words, exact - 1u, &cell) ==
          -ENOSPC);
   assert(cell.ib_size_dwords == 0);
   assert(r300_zb_depth_control_emit_into(&good, words, exact, &cell) == 0);
   assert(cell.ib_size_dwords == exact);

   r300_fragment_binary_finish(&fs);
   printf("refusals ok\n");
}

static void
test_determinism(void)
{
   struct r300_zb_depth_control_ib a, b;
   assert(r300_zb_depth_control_reference_emit(&a) == 0);
   assert(r300_zb_depth_control_reference_emit(&b) == 0);
   assert(a.ib_size_dwords == b.ib_size_dwords);
   assert(memcmp(a.ib, b.ib, a.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_zb_depth_control_release(&a);
   r300_zb_depth_control_release(&b);
   printf("determinism ok\n");
}

/* Paints the target the way a device honoring the depth test does: the
 * draw color inside near_colored's triangle, the sentinel everywhere
 * else.  `invert` paints the complement a reversed comparison sense
 * produces.
 */
static void
paint_color(uint32_t *pixels, bool invert)
{
   for (uint32_t i = 0; i < PIXELS; i++)
      pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;

   const float *v = r300_zb_depth_control_vertices;
   const uint32_t base = invert ? 12u : 0u;
   /* The painted region is the vertex payload's own triangle, filled by
    * bounding box and edge sign; the oracle's margin keeps its samples
    * clear of the boundary this fill draws differently.
    */
   for (uint32_t y = 0; y < R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT; y++) {
      for (uint32_t x = 0; x < R300_ZB_DEPTH_CONTROL_TARGET_WIDTH; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         const float ax = v[base + 0], ay = v[base + 1];
         const float bx = v[base + 4], by = v[base + 5];
         const float cx = v[base + 8], cy = v[base + 9];
         const float e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
         const float e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
         const float e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
         if (e0 > 0.0f && e1 > 0.0f && e2 > 0.0f)
            pixels[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x] =
               R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
      }
   }
}

static void
paint_depth(uint16_t *depth, uint16_t near_value, bool invert)
{
   for (uint32_t i = 0; i < PIXELS; i++)
      depth[i] = R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL;

   const float *v = r300_zb_depth_control_vertices;
   const uint32_t base = invert ? 12u : 0u;
   for (uint32_t y = 0; y < R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT; y++) {
      for (uint32_t x = 0; x < R300_ZB_DEPTH_CONTROL_TARGET_WIDTH; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         const float ax = v[base + 0], ay = v[base + 1];
         const float bx = v[base + 4], by = v[base + 5];
         const float cx = v[base + 8], cy = v[base + 9];
         const float e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
         const float e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
         const float e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
         if (e0 > 0.0f && e1 > 0.0f && e2 > 0.0f)
            depth[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x] = near_value;
      }
   }
}

static void
test_color_oracle(void)
{
   uint32_t *pixels = malloc(PIXELS * sizeof(uint32_t));
   assert(pixels != NULL);
   struct r300_zb_depth_control_color_verdict v;

   /* Known-good: the near half carries the draw color, the far half the
    * sentinel, which is the depth test rejecting the far triangle.
    */
   paint_color(pixels, false);
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t), &v);
   assert(v.executed && v.near_pass && v.far_pass && v.exterior_pass &&
          v.canary_pass);
   assert(v.near_samples > 0 && v.far_samples > 0 && v.exterior_samples > 0);
   assert(v.near_colored == v.near_samples && v.far_colored == 0);

   /* Known-bad, reversed comparison sense: the complement image.  The two
    * colored counts name which half the device wrote, so the failure is
    * legible rather than a generic mismatch.
    */
   paint_color(pixels, true);
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t), &v);
   assert(v.executed && !v.near_pass && !v.far_pass);
   assert(v.near_colored == 0 && v.far_colored == v.far_samples);

   /* Known-bad, depth test absent: both halves colored. */
   paint_color(pixels, false);
   for (uint32_t y = 0; y < R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT; y++) {
      for (uint32_t x = 32; x < R300_ZB_DEPTH_CONTROL_TARGET_WIDTH; x++) {
         uint32_t *p = &pixels[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x];
         if (*p == R300_TRIANGLE_COLOR_SENTINEL)
            *p = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
      }
   }
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t), &v);
   assert(v.near_pass && !v.far_pass && v.far_colored == v.far_samples);

   /* Known-bad, nothing executed: the whole target stays the sentinel. */
   for (uint32_t i = 0; i < PIXELS; i++)
      pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t), &v);
   assert(!v.executed && !v.near_pass && v.far_pass && v.exterior_pass);

   /* Known-bad, canary overwritten past the render extent. */
   paint_color(pixels, false);
   pixels[R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT *
          R300_ZB_DEPTH_CONTROL_PITCH_PIXELS] = 0;
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t), &v);
   assert(!v.canary_pass);

   /* A buffer short of the retained footprint carries no canary, so
    * every pass fails with zero samples.
    */
   paint_color(pixels, false);
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t) - 4u,
                                      &v);
   assert(!v.executed && !v.near_pass && !v.far_pass && !v.exterior_pass &&
          !v.canary_pass);
   assert(v.near_samples == 0 && v.far_samples == 0);
   r300_zb_depth_control_color_oracle(NULL, PIXELS * sizeof(uint32_t), &v);
   assert(!v.near_pass && v.near_samples == 0);

   free(pixels);
   printf("color oracle ok\n");
}

static void
test_depth_oracle(void)
{
   uint16_t *depth = malloc(PIXELS * sizeof(uint16_t));
   assert(depth != NULL);
   struct r300_zb_depth_control_depth_verdict v;

   /* Known-good: the near half stores a value that compared below the
    * sentinel, the far half keeps the sentinel.  The stored value is
    * reported, not gated, so no window-Z to Z16 rounding rule decides.
    */
   paint_depth(depth, 0x4000, false);
   r300_zb_depth_control_depth_oracle(depth, PIXELS * sizeof(uint16_t), &v);
   assert(v.written && v.near_pass && v.far_pass && v.exterior_pass &&
          v.canary_pass);
   assert(v.near_min == 0x4000 && v.near_max == 0x4000);
   assert(v.near_samples > 0 && v.far_samples > 0);

   /* A different stored value inside the same ordering still passes, and
    * the reported range moves with it.
    */
   paint_depth(depth, 0x3fff, false);
   r300_zb_depth_control_depth_oracle(depth, PIXELS * sizeof(uint16_t), &v);
   assert(v.near_pass && v.near_min == 0x3fff && v.near_max == 0x3fff);

   /* Known-bad, a surface nothing wrote: zeros in the near half clear the
    * upper bound under any rounding rule, so the lower bound is what
    * refuses them.
    */
   paint_depth(depth, 0x0000, false);
   r300_zb_depth_control_depth_oracle(depth, PIXELS * sizeof(uint16_t), &v);
   assert(v.written && !v.near_pass && v.far_pass);
   assert(v.near_min == 0 && v.near_max == 0);

   /* Known-bad, no depth write: the near half keeps the sentinel, so the
    * ordering the comparison promises never appears in memory.
    */
   for (uint32_t i = 0; i < PIXELS; i++)
      depth[i] = R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL;
   r300_zb_depth_control_depth_oracle(depth, PIXELS * sizeof(uint16_t), &v);
   assert(!v.written && !v.near_pass && v.far_pass);

   /* Known-bad, the far triangle wrote too: a rejected fragment reached
    * depth memory.
    */
   paint_depth(depth, 0x4000, false);
   for (uint32_t y = 0; y < R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT; y++) {
      for (uint32_t x = 32; x < R300_ZB_DEPTH_CONTROL_TARGET_WIDTH; x++) {
         uint16_t *p = &depth[y * R300_ZB_DEPTH_CONTROL_PITCH_PIXELS + x];
         if (*p == R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL)
            *p = 0xc000;
      }
   }
   r300_zb_depth_control_depth_oracle(depth, PIXELS * sizeof(uint16_t), &v);
   assert(v.near_pass && !v.far_pass);

   /* Known-bad, canary overwritten past the render extent. */
   paint_depth(depth, 0x4000, false);
   depth[R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT *
         R300_ZB_DEPTH_CONTROL_PITCH_PIXELS] = 0;
   r300_zb_depth_control_depth_oracle(depth, PIXELS * sizeof(uint16_t), &v);
   assert(!v.canary_pass);

   paint_depth(depth, 0x4000, false);
   r300_zb_depth_control_depth_oracle(depth, PIXELS * sizeof(uint16_t) - 2u,
                                      &v);
   assert(!v.written && !v.near_pass && !v.far_pass && v.near_samples == 0);
   r300_zb_depth_control_depth_oracle(NULL, PIXELS * sizeof(uint16_t), &v);
   assert(!v.near_pass && v.near_samples == 0);

   free(depth);
   printf("depth oracle ok\n");
}

/* The two triangles are disjoint at the verdict margin, so no pixel
 * carries both regions' verdicts and the two halves decide separately.
 */
static void
test_regions_disjoint(void)
{
   uint32_t *pixels = malloc(PIXELS * sizeof(uint32_t));
   assert(pixels != NULL);
   struct r300_zb_depth_control_color_verdict near_only, far_only;

   paint_color(pixels, false);
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t),
                                      &near_only);
   paint_color(pixels, true);
   r300_zb_depth_control_color_oracle(pixels, PIXELS * sizeof(uint32_t),
                                      &far_only);

   assert(near_only.near_samples == far_only.near_samples);
   assert(near_only.far_samples == far_only.far_samples);
   assert(near_only.near_colored == near_only.near_samples);
   assert(near_only.far_colored == 0);
   assert(far_only.far_colored == far_only.far_samples);
   assert(far_only.near_colored == 0);

   free(pixels);
   printf("regions disjoint ok\n");
}

int
main(void)
{
   test_depth_state_words();
   test_draw_packet();
   test_contract_vertex_bound();
   test_contract_departure();
   test_reloc_sites();
   test_refusals();
   test_determinism();
   test_color_oracle();
   test_depth_oracle();
   test_regions_disjoint();
   printf("r300_zb_depth_control_cell_test: all checks passed\n");
   return 0;
}
