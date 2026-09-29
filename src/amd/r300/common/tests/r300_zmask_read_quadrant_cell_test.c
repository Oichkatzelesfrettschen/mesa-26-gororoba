/*
 * SPDX-License-Identifier: MIT
 *
 * The ZMASK read-group cell held to its experiment: the ZB_BW_CNTL
 * sequence 0x00, 0x1c, 0x04, 0x0c at the four draws, a Z-cache flush and
 * free ahead of every draw, one whole-level clear, a stream the HyperZ
 * walker admits with ownership and refuses without, and every PACKET0
 * register inside the kernel's authority.  Each state-check rule carries
 * a known-bad mutation that must refuse, and the oracles are calibrated
 * on synthetic images before any silicon image reaches them.
 */
#undef NDEBUG

#include "r300_kernel_packet0_authority.h"

#include "r300_pm4_builder.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_zb_hyperz_admission.h"
#include "r300_zmask_read_quadrant_cell.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t failures;

static void
expect(bool condition, const char *what)
{
   if (!condition) {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
   }
}

/* Every PACKET0 register the stream writes lies in the kernel's union of
 * reg_srcs/r300 and r300_packet0_check; the draw-path opcodes stay with
 * r300_packet3_check. */
static bool
kernel_admits_every_register(const uint32_t *ib, uint32_t dwords)
{
   for (uint32_t i = 0; i < dwords;) {
      const uint32_t header = ib[i];
      const uint32_t type = header >> 30;
      if (type == 2u) {
         i++;
         continue;
      }
      if (type == 1u)
         return false;
      const uint32_t count = ((header >> 16) & 0x3fffu) + 1u;
      if (count > dwords - 1u - i)
         return false;
      if (type == 0u) {
         const uint32_t base = (header & 0x1fffu) << 2;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0u;
         for (uint32_t k = 0; k < count; k++) {
            const uint32_t reg = one_reg ? base : base + 4u * k;
            if (reg >= R300_KERNEL_PACKET0_REGISTER_LIMIT ||
                !r300_kernel_admits_packet0_register(reg)) {
               fprintf(stderr, "register 0x%04x outside the authority\n",
                       reg);
               return false;
            }
         }
      }
      i += count + 1u;
   }
   return true;
}

/* The index of the n-th PACKET0 write of reg carrying value, or -1. */
static int
find_write(const uint32_t *ib, uint32_t dwords, uint32_t reg, uint32_t value,
           unsigned nth)
{
   for (uint32_t i = 0; i < dwords;) {
      const uint32_t header = ib[i];
      const uint32_t type = header >> 30;
      if (type == 2u) {
         i++;
         continue;
      }
      const uint32_t count = ((header >> 16) & 0x3fffu) + 1u;
      if (type == 0u && count == 1u && ((header & 0x1fffu) << 2) == reg &&
          ib[i + 1u] == value) {
         if (nth == 0u)
            return (int)(i + 1u);
         nth--;
      }
      i += count + 1u;
   }
   return -1;
}

static bool
check_mutation(const struct r300_zmask_read_quadrant_ib *cell,
               const struct r300_zmask_read_quadrant_params *params,
               uint32_t reg, uint32_t from, unsigned nth, uint32_t to,
               const char *what)
{
   uint32_t *copy = malloc(cell->ib_size_dwords * sizeof(*copy));
   assert(copy != NULL);
   memcpy(copy, cell->ib, cell->ib_size_dwords * sizeof(*copy));
   const int index = find_write(copy, cell->ib_size_dwords, reg, from, nth);
   bool refused = false;
   if (index >= 0) {
      copy[index] = to;
      refused = r300_zmask_read_quadrant_check_state(
                   params, copy, cell->ib_size_dwords) != 0;
   }
   free(copy);
   if (index < 0)
      fprintf(stderr, "mutation site for %s not found\n", what);
   return index >= 0 && refused;
}

int
main(void)
{
   const struct r300_zb_depth_surface *surface;
   struct r300_zb_depth_layout depth_layout;
   struct r300_zmask_layout zmask_layout;
   expect(r300_zmask_read_quadrant_surface(&surface, &depth_layout,
                                           &zmask_layout) == 0,
          "surface resolves");
   expect(zmask_layout.zcomp8x8 && zmask_layout.stride_in_pixels == 64u,
          "8x8 layout at the lifecycle's ZMASK pitch");
   expect(depth_layout.base_offset_bytes ==
             R300_ZMASK_READ_QUADRANT_DEPTH_GUARD_BYTES,
          "envelope at the lifecycle's ZB_DEPTHOFFSET");

   struct r300_zmask_read_quadrant_ib cell;
   expect(r300_zmask_read_quadrant_reference_emit(
             R300_ZMASK_READ_QUADRANT_ARM_NEAR, &cell) == 0,
          "reference cell emits");
   if (failures != 0)
      return 1;
   expect(r300_zmask_read_quadrant_validate_reloc_sites(&cell) == 0,
          "relocation sites");

   const struct r300_zmask_read_quadrant_params params = {
      .arm = R300_ZMASK_READ_QUADRANT_ARM_NEAR,
      .surface = surface,
      .zmask_layout = &zmask_layout,
      .depth_offset_bytes = (uint32_t)depth_layout.base_offset_bytes,
   };
   expect(r300_zmask_read_quadrant_check_state(&params, cell.ib,
                                               cell.ib_size_dwords) == 0,
          "reference cell passes its own state check");

   struct r300_zmask_read_quadrant_stream_state state;
   expect(r300_zmask_read_quadrant_read_state(cell.ib, cell.ib_size_dwords,
                                              &state) == 0 &&
             state.draw_count == 4u,
          "four draws");
   static const uint32_t groups[4] = { 0x00u, 0x1cu, 0x04u, 0x0cu };
   static const uint32_t peq[4] = { 0u, 1u, 1u, 1u };
   for (uint32_t q = 0; q < 4u; q++) {
      expect(state.draws[q].zb_bw_cntl == groups[q], "ZB_BW_CNTL sequence");
      expect(state.draws[q].gb_z_peq_config == peq[q],
             "plane equations 4x4 for A, 8x8 for the wrapped draws");
      expect(state.draws[q].zcache_flushes_before >= 1u,
             "Z-cache flush and free ahead of every draw");
      expect(state.draws[q].zb_depthoffset == 0x800u,
             "ZB_DEPTHOFFSET 0x800");
      expect(state.draws[q].zb_depthpitch == 0x00030040u,
             "ZB_DEPTHPITCH the lifecycle executed under");
      expect(state.draws[q].zb_depthclearvalue == 0x8000003cu,
             "ZB_DEPTHCLEARVALUE the lifecycle executed under");
   }
   expect(state.zmask_clears == 1u && state.zmask_clear_payload[1] == 4u,
          "one clear over four metadata dwords");

   expect(r300_zb_hyperz_admit_stream(cell.ib, cell.ib_size_dwords,
                                      R300_ZB_HYPERZ_OWNED, NULL) ==
             R300_ZB_HYPERZ_ADMIT,
          "HyperZ walker admits the stream with ownership");
   expect(r300_zb_hyperz_admit_stream(cell.ib, cell.ib_size_dwords,
                                      R300_ZB_HYPERZ_UNOWNED, NULL) ==
             R300_ZB_HYPERZ_REFUSE_OWNERSHIP,
          "HyperZ walker refuses the stream without ownership");
   expect(kernel_admits_every_register(cell.ib, cell.ib_size_dwords),
          "every PACKET0 register inside the kernel authority");

   /* The near arm is the stream RS485M executed; its digest is the one the
    * sealed prediction and the retained bundle name. */
   char digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, digest);
   expect(cell.ib_size_dwords == 385u &&
             strcmp(digest, "edb292fad46ca951fc837d279a8cd8c0dfa01aa3e1b0"
                            "2866bd5fab9d2d0fdce3") == 0,
          "near-arm stream keeps the executed digest");

   /* The far arm: same shape, its own vertex sets, its own digest, and
    * each arm's check refuses the other arm's stream. */
   struct r300_zmask_read_quadrant_ib far;
   expect(r300_zmask_read_quadrant_reference_emit(
             R300_ZMASK_READ_QUADRANT_ARM_FAR, &far) == 0,
          "far cell emits");
   const struct r300_zmask_read_quadrant_params far_params = {
      .arm = R300_ZMASK_READ_QUADRANT_ARM_FAR,
      .surface = surface,
      .zmask_layout = &zmask_layout,
      .depth_offset_bytes = (uint32_t)depth_layout.base_offset_bytes,
   };
   expect(r300_zmask_read_quadrant_validate_reloc_sites(&far) == 0 &&
             r300_zmask_read_quadrant_check_state(&far_params, far.ib,
                                                  far.ib_size_dwords) == 0,
          "far cell passes its own state check");
   expect(r300_zmask_read_quadrant_check_state(&params, far.ib,
                                               far.ib_size_dwords) != 0 &&
             r300_zmask_read_quadrant_check_state(&far_params, cell.ib,
                                                  cell.ib_size_dwords) != 0,
          "each arm refuses the other arm's stream");
   char far_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(far.ib, far.ib_size_dwords, far_digest);
   expect(far.ib_size_dwords == cell.ib_size_dwords &&
             strcmp(far_digest, digest) != 0,
          "far arm has the near shape and its own digest");
   printf("far-arm ib_blake3=%s\n", far_digest);
   r300_zmask_read_quadrant_release(&far);

   /* Every far depth fails LESS against both candidate references for
    * the wrapped draws, and against the backing for A. */
   for (uint32_t q = 0; q < 4u; q++) {
      const struct r300_zmask_read_quadrant_draw *d =
         &r300_zmask_read_quadrant_draws[q];
      const uint32_t far_code = d->depth_code[R300_ZMASK_READ_QUADRANT_ARM_FAR];
      expect(far_code >= R300_ZMASK_READ_QUADRANT_BACKING_DEPTH_CODE &&
                (!d->wrapped ||
                 far_code >= R300_ZMASK_READ_QUADRANT_CLEAR_DEPTH_CODE),
             "far depths fail their references");
      expect((uint32_t)(d->z[R300_ZMASK_READ_QUADRANT_ARM_FAR] * 16777216.0f) ==
                far_code,
             "far z names its depth code");
   }

   /* Known-bad mutations, one per rule. */
   expect(check_mutation(&cell, &params, R300_ZB_BW_CNTL, 0x04u, 1u, 0x0cu,
                         "C group"),
          "C at 0x0c refuses");
   expect(check_mutation(&cell, &params, R300_ZB_BW_CNTL, 0x1cu, 0u, 0x0cu,
                         "B group"),
          "B at 0x0c refuses");
   expect(check_mutation(&cell, &params, R300_GB_Z_PEQ_CONFIG, 1u, 1u, 0u,
                         "B plane equations"),
          "B at 4x4 refuses");
   /* Flush-and-free writes in stream order: the contract's, the two
    * around the clear, the restore ahead of A, then the one after A --
    * the only flush between A and B. */
   expect(check_mutation(&cell, &params, R300_ZB_ZCACHE_CTLSTAT, 3u, 4u, 0u,
                         "flush between A and B"),
          "a missing flush ahead of B refuses");
   expect(check_mutation(&cell, &params, R300_ZB_CNTL, R300_Z_ENABLE, 0u,
                         R300_Z_ENABLE | R300_Z_WRITE_ENABLE, "depth write"),
          "a depth write refuses");
   expect(check_mutation(&cell, &params, R300_ZB_DEPTHCLEARVALUE,
                         0x8000003cu, 1u, 0x2000005au, "clear word"),
          "a wrapped draw at another clear word refuses");

   /* Depth seed: guard outside, backing word inside. */
   const uint64_t depth_bytes = r300_zmask_read_quadrant_depth_bytes();
   expect(depth_bytes == depth_layout.total_bytes && depth_bytes != 0u,
          "depth allocation size");
   uint8_t *depth = malloc((size_t)depth_bytes);
   assert(depth != NULL);
   expect(r300_zmask_read_quadrant_fill_depth(&depth_layout, depth,
                                              depth_bytes) == 0,
          "depth fill");
   uint32_t first_word, last_word;
   memcpy(&first_word, depth + depth_layout.base_offset_bytes, 4u);
   memcpy(&last_word,
          depth + depth_layout.base_offset_bytes + depth_layout.storage_bytes -
             4u,
          4u);
   expect(first_word == 0x2000005au && last_word == 0x2000005au,
          "envelope holds the backing word");
   expect(depth[0] == 0xa3u && depth[depth_bytes - 1u] == 0xa3u,
          "guards hold the fill");
   uint8_t *after = malloc((size_t)depth_bytes);
   assert(after != NULL);
   memcpy(after, depth, (size_t)depth_bytes);
   expect(r300_zmask_read_quadrant_depth_changed_bytes(depth, after,
                                                       depth_bytes) == 0u,
          "identical images differ in no byte");
   after[depth_layout.base_offset_bytes] ^= 1u;
   expect(r300_zmask_read_quadrant_depth_changed_bytes(depth, after,
                                                       depth_bytes) == 1u,
          "one flipped byte counts once");
   free(after);
   free(depth);

   /* Color oracle calibration: sentinel image, the prediction's
    * (colored, colored, sentinel, sentinel) image, a half-written
    * quadrant, and a guard-row write. */
   uint8_t *color = malloc(R300_ZMASK_READ_QUADRANT_COLOR_BYTES);
   assert(color != NULL);
   struct r300_zmask_read_quadrant_color_observation seen;
   r300_zmask_read_quadrant_fill_color(color,
                                       R300_ZMASK_READ_QUADRANT_COLOR_BYTES);
   r300_zmask_read_quadrant_observe_color(
      color, R300_ZMASK_READ_QUADRANT_COLOR_BYTES, &seen);
   for (uint32_t q = 0; q < 4u; q++)
      expect(seen.judged &&
                seen.reading[q] == R300_ZMASK_READ_QUADRANT_SENTINEL,
             "sentinel image reads sentinel");
   for (uint32_t y = 0; y < 32u; y++)
      for (uint32_t x = 0; x < 64u; x++) {
         const uint32_t q = x / 32u;
         memcpy(color + 4u * (y * 64u + x),
                &r300_zmask_read_quadrant_draws[q].color, 4u);
      }
   r300_zmask_read_quadrant_observe_color(
      color, R300_ZMASK_READ_QUADRANT_COLOR_BYTES, &seen);
   expect(seen.reading[0] == R300_ZMASK_READ_QUADRANT_COLORED &&
             seen.reading[1] == R300_ZMASK_READ_QUADRANT_COLORED &&
             seen.reading[2] == R300_ZMASK_READ_QUADRANT_SENTINEL &&
             seen.reading[3] == R300_ZMASK_READ_QUADRANT_SENTINEL,
          "top-half image reads (colored, colored, sentinel, sentinel)");
   /* B's color painted into A reads foreign, not colored. */
   memcpy(color, &r300_zmask_read_quadrant_draws[1].color, 4u);
   r300_zmask_read_quadrant_observe_color(
      color, R300_ZMASK_READ_QUADRANT_COLOR_BYTES, &seen);
   expect(seen.reading[0] == R300_ZMASK_READ_QUADRANT_MIXED &&
             seen.foreign[0] == 1u,
          "a foreign color in A reads mixed");
   const uint32_t stray = 0u;
   memcpy(color + 4u * (64u * 64u), &stray, 4u);
   r300_zmask_read_quadrant_observe_color(
      color, R300_ZMASK_READ_QUADRANT_COLOR_BYTES, &seen);
   expect(seen.guard_changed == 1u, "a guard-row write counts");
   free(color);

   r300_zmask_read_quadrant_release(&cell);
   if (failures != 0) {
      fprintf(stderr, "%u failures\n", failures);
      return 1;
   }
   printf("r300-zmask-read-quadrant-cell: ok\n");
   return 0;
}
