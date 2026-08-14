/*
 * SPDX-License-Identifier: MIT
 *
 * Structural proof of the R2VB producer pass: the emitted stream decodes
 * as well-formed PM4, carries the target retarget, the embedded POINTS
 * draw, and the publication tail in order, and the refusal legs hold the
 * emission closed outside its domain.
 */

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_r2vb_producer_fs_block.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_tcl_bypass_triangle.h"

#include "r300_reg.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
   do {                                                                \
      if (!(cond)) {                                                   \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                 #cond);                                               \
         failures++;                                                   \
      }                                                                \
   } while (0)

#define PACKET_GET_TYPE(h) (((h) >> 30) & 3u)
#define PACKET_GET_COUNT(h) (((h) >> 16) & 0x3FFFu)
#define PACKET0_GET_REG(h) (((h) & 0x1FFFu) << 2)
#define PACKET0_GET_ONE_REG_WR(h) (((h) >> 15) & 1u)
#define PACKET3_GET_OPCODE(h) (((h) >> 8) & 0xFFu)

/* The stream walk collects the final value of each register this test
 * pins, the draw packet position, and the packet3 opcodes in order, so
 * the assertions read observed state rather than fixed offsets.
 */
struct walk_state {
   uint32_t reg_value[24];
   bool reg_seen[24];
   uint32_t draw_header_index;
   uint32_t draw_count;
   uint32_t nop_count;
   uint32_t pvs_flush_index;
   uint32_t last_dstcache_index;
};

/* The registers the walk records, indexed into walk_state. */
static const uint32_t tracked_regs[] = {
   R300_RB3D_COLOROFFSET0, R300_RB3D_COLORPITCH0, R300_US_OUT_FMT_0,
   R300_VAP_VTX_SIZE,      R300_VAP_VF_MAX_VTX_INDX,
   R300_VAP_CNTL_STATUS,   R300_GA_POINT_SIZE,
   R300_VAP_PVS_STATE_FLUSH_REG, R300_SC_SCISSORS_TL,
   R300_SC_SCISSORS_BR,      R300_RS_COUNT,
   R300_RS_INST_COUNT,       R300_RS_IP_0,
   R300_RS_INST_0,
};

/* Locates the compiled US block inside the stream: the emission copies
 * the fragment binary's cb_code verbatim, so the block is a contiguous
 * run equal to the golden header.  Returns the first index or the stream
 * length when the run is absent.
 */
static uint32_t
find_us_block(const uint32_t *ib, uint32_t count)
{
   const uint32_t block_dwords =
      sizeof(r300_r2vb_producer_fs_block) /
      sizeof(r300_r2vb_producer_fs_block[0]);
   if (count < block_dwords)
      return count;
   for (uint32_t i = 0; i + block_dwords <= count; i++) {
      if (memcmp(&ib[i], r300_r2vb_producer_fs_block,
                 block_dwords * sizeof(uint32_t)) == 0)
         return i;
   }
   return count;
}

static int
tracked_index(uint32_t reg)
{
   for (unsigned i = 0; i < sizeof(tracked_regs) / sizeof(tracked_regs[0]);
        i++) {
      if (tracked_regs[i] == reg)
         return (int)i;
   }
   return -1;
}

/* Walks the stream as radeon_cs_packet_parse frames it: a type-0 header
 * names a register run, a type-3 header names an opcode and payload.
 * Returns 0 on a well-formed stream.
 */
static int
walk_stream(const uint32_t *ib, uint32_t count, struct walk_state *st)
{
   memset(st, 0, sizeof(*st));
   uint32_t idx = 0;
   while (idx < count) {
      const uint32_t header = ib[idx];
      const uint32_t payload = PACKET_GET_COUNT(header) + 1;
      switch (PACKET_GET_TYPE(header)) {
      case 0: {
         if (idx + 1 + payload > count)
            return -ERANGE;
         const uint32_t one_reg = PACKET0_GET_ONE_REG_WR(header);
         for (uint32_t i = 0; i < payload; i++) {
            const uint32_t reg =
               PACKET0_GET_REG(header) + (one_reg ? 0 : i * 4);
            const int t = tracked_index(reg);
            if (t >= 0) {
               st->reg_value[t] = ib[idx + 1 + i];
               st->reg_seen[t] = true;
            }
            if (reg == R300_VAP_PVS_STATE_FLUSH_REG)
               st->pvs_flush_index = idx;
            if (reg == R300_RB3D_DSTCACHE_CTLSTAT)
               st->last_dstcache_index = idx;
         }
         idx += 1 + payload;
         break;
      }
      case 3: {
         if (idx + 1 + payload > count)
            return -ERANGE;
         const uint32_t opcode = PACKET3_GET_OPCODE(header);
         if (opcode == (R300_PACKET3_3D_DRAW_IMMD_2 >> 8)) {
            st->draw_header_index = idx;
            st->draw_count++;
         }
         if (opcode == (R300_PM4_PACKET3_NOP >> 8))
            st->nop_count++;
         idx += 1 + payload;
         break;
      }
      default:
         return -EINVAL;
      }
   }
   return 0;
}

static void
test_reference_structure(void)
{
   struct r300_r2vb_producer_ib pass;
   CHECK(r300_r2vb_producer_reference_emit(&pass) == 0);
   CHECK(r300_r2vb_producer_pass_validate_reloc_sites(&pass) == 0);

   struct walk_state st;
   CHECK(walk_stream(pass.ib, pass.ib_size_dwords, &st) == 0);

   /* One embedded draw, one relocation NOP. */
   CHECK(st.draw_count == 1);
   CHECK(st.nop_count == 1);

   /* Carrier retarget: FP32x4 pixels at the even row pitch. */
   CHECK(st.reg_seen[tracked_index(R300_RB3D_COLORPITCH0)]);
   CHECK(st.reg_value[tracked_index(R300_RB3D_COLORPITCH0)] ==
         (4u | R300_COLOR_FORMAT_ARGB32323232));
   CHECK(st.reg_seen[tracked_index(R300_RB3D_COLOROFFSET0)]);
   CHECK(st.reg_value[tracked_index(R300_RB3D_COLOROFFSET0)] == 0);

   /* BGRA output select on the C4_32_FP container. */
   CHECK(st.reg_seen[tracked_index(R300_US_OUT_FMT_0)]);
   CHECK(st.reg_value[tracked_index(R300_US_OUT_FMT_0)] ==
         (R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_B | R300_C1_SEL_G |
          R300_C2_SEL_R | R300_C3_SEL_A));

   /* The US program travels inside the stream, ahead of the draw it
    * shades, so the slot pixels run this program rather than whatever the
    * previous client left resident.
    */
   const uint32_t us_block_index =
      find_us_block(pass.ib, pass.ib_size_dwords);
   CHECK(us_block_index < pass.ib_size_dwords);
   CHECK(us_block_index < st.draw_header_index);

   /* Varying routing into that program: four interpolated components, no
    * rasterized colors, TEX0's four channels in order, delivered to US
    * input register zero by instruction 0 alone.
    */
   CHECK(st.reg_seen[tracked_index(R300_RS_COUNT)]);
   CHECK(st.reg_value[tracked_index(R300_RS_COUNT)] ==
         (R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN));
   CHECK(st.reg_seen[tracked_index(R300_RS_INST_COUNT)]);
   CHECK(st.reg_value[tracked_index(R300_RS_INST_COUNT)] == 0);
   CHECK(st.reg_seen[tracked_index(R300_RS_IP_0)]);
   CHECK(st.reg_value[tracked_index(R300_RS_IP_0)] ==
         (R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
          R300_RS_SEL_T(R300_RS_SEL_C1) |
          R300_RS_SEL_R(R300_RS_SEL_C2) |
          R300_RS_SEL_Q(R300_RS_SEL_C3)));
   CHECK(st.reg_seen[tracked_index(R300_RS_INST_0)]);
   CHECK(st.reg_value[tracked_index(R300_RS_INST_0)] ==
         (R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
          R300_RS_INST_TEX_ADDR(0)));

   /* Embedded draw framing: eight dwords per vertex, three vertices,
    * POINTS through the embedded walk.
    */
   CHECK(st.reg_value[tracked_index(R300_VAP_VTX_SIZE)] == 8);
   CHECK(st.reg_value[tracked_index(R300_VAP_VF_MAX_VTX_INDX)] == 2);
   CHECK(st.reg_value[tracked_index(R300_VAP_CNTL_STATUS)] ==
         R300_VAP_TCL_BYPASS);
   const uint32_t draw_header = pass.ib[st.draw_header_index];
   CHECK(PACKET_GET_COUNT(draw_header) == 3 * 8);
   const uint32_t vf_cntl = pass.ib[st.draw_header_index + 1];
   CHECK(vf_cntl == (R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED |
                     (3u << 16) | R300_VAP_VF_CNTL__PRIM_POINTS));

   /* Body bytes: each vertex carries its slot center then the source
    * record pre-swizzled (z, y, x, w), so the BGRA select restores the
    * source order.
    */
   for (uint32_t v = 0; v < 3; v++) {
      const uint32_t *vtx = &pass.ib[st.draw_header_index + 2 + v * 8];
      float slot_x, slot_y, z, w;
      memcpy(&slot_x, &vtx[0], 4);
      memcpy(&slot_y, &vtx[1], 4);
      memcpy(&z, &vtx[2], 4);
      memcpy(&w, &vtx[3], 4);
      CHECK(slot_x == (float)v + 0.5f);
      CHECK(slot_y == 0.5f);
      CHECK(z == 0.0f);
      CHECK(w == 1.0f);
      const float *rec = &r300_tcl_bypass_triangle_vertices[v * 4];
      uint32_t rec_bits[4];
      memcpy(rec_bits, rec, sizeof(rec_bits));
      CHECK(vtx[4] == rec_bits[2]);
      CHECK(vtx[5] == rec_bits[1]);
      CHECK(vtx[6] == rec_bits[0]);
      CHECK(vtx[7] == rec_bits[3]);
   }

   /* Raster confinement: the scissor pair carries the non-R500 1440
    * bias over the 4x1 slot row -- the BR y payload is what the kernel
    * derives maxy = 1 from, the height the color-buffer bound
    * multiplies -- and the point size holds one pixel per axis in
    * sixths.
    */
   CHECK(st.reg_seen[tracked_index(R300_SC_SCISSORS_TL)]);
   CHECK(st.reg_value[tracked_index(R300_SC_SCISSORS_TL)] ==
         ((1440u << R300_SCISSORS_X_SHIFT) |
          (1440u << R300_SCISSORS_Y_SHIFT)));
   CHECK(st.reg_seen[tracked_index(R300_SC_SCISSORS_BR)]);
   CHECK(st.reg_value[tracked_index(R300_SC_SCISSORS_BR)] ==
         (((4u + 1440u - 1u) << R300_SCISSORS_X_SHIFT) |
          ((1u + 1440u - 1u) << R300_SCISSORS_Y_SHIFT)));
   CHECK(st.reg_seen[tracked_index(R300_GA_POINT_SIZE)]);
   CHECK(st.reg_value[tracked_index(R300_GA_POINT_SIZE)] ==
         ((6u << R300_POINTSIZE_Y_SHIFT) |
          (6u << R300_POINTSIZE_X_SHIFT)));

   /* Publication tail: the final VAP_PVS_STATE_FLUSH_REG write lands
    * after the draw and after the last destination-cache flush, and
    * carries zero.
    */
   CHECK(st.reg_seen[tracked_index(R300_VAP_PVS_STATE_FLUSH_REG)]);
   CHECK(st.reg_value[tracked_index(R300_VAP_PVS_STATE_FLUSH_REG)] == 0);
   CHECK(st.pvs_flush_index > st.draw_header_index);
   CHECK(st.last_dstcache_index > st.draw_header_index);
   CHECK(st.pvs_flush_index > st.last_dstcache_index);
   CHECK(st.pvs_flush_index + 2 == pass.ib_size_dwords);

   /* The emission is deterministic: a second reference emission carries
    * identical bytes, so the digest names one stream.
    */
   struct r300_r2vb_producer_ib again;
   CHECK(r300_r2vb_producer_reference_emit(&again) == 0);
   CHECK(again.ib_size_dwords == pass.ib_size_dwords);
   CHECK(memcmp(again.ib, pass.ib,
                pass.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_r2vb_producer_pass_release(&again);

   r300_r2vb_producer_pass_release(&pass);
}

/* The FP24 boundary sweep: every component is pinned to its binary32
 * encoding on the admission lattice, the emission accepts the whole
 * table, its expected dwords are the identity over it, and its stream
 * differs from the reference stream only in the twelve embedded record
 * dwords -- the frozen geometry, contract, and program bytes stay equal.
 */
static void
test_fp24_sweep_stream(void)
{
   /* The literals in the sweep table carry these exact encodings; a
    * literal drifting off the lattice fails here before any emission.
    */
   static const uint32_t sweep_bits[R300_R2VB_PRODUCER_FP24_SWEEP_COUNT][4] = {
      { 0x00000000u, 0x21000000u, 0x21000080u, 0x21800000u },
      { 0x3f800000u, 0x3f800080u, 0x3fffff80u, 0x40000000u },
      { 0x4479c000u, 0x60000000u, 0x607fff00u, 0x607fff80u },
   };
   uint32_t table_bits[R300_R2VB_PRODUCER_FP24_SWEEP_COUNT][4];
   memcpy(table_bits, r300_r2vb_producer_fp24_sweep_records,
          sizeof(table_bits));
   CHECK(memcmp(table_bits, sweep_bits, sizeof(sweep_bits)) == 0);

   struct r300_r2vb_producer_ib sweep;
   CHECK(r300_r2vb_producer_fp24_sweep_emit(&sweep) == 0);
   CHECK(r300_r2vb_producer_pass_validate_reloc_sites(&sweep) == 0);

   /* The expected carrier is the identity over the table: F32_4 keeps
    * all four source lanes.
    */
   uint32_t expected[R300_R2VB_PRODUCER_FP24_SWEEP_COUNT * 4];
   CHECK(r300_r2vb_producer_fp24_sweep_expected(
            expected, R300_R2VB_PRODUCER_FP24_SWEEP_COUNT * 4) == 0);
   CHECK(memcmp(expected, sweep_bits, sizeof(sweep_bits)) == 0);
   CHECK(r300_r2vb_producer_fp24_sweep_expected(expected, 11) == -ENOSPC);

   /* Same construction, different records: the streams are equal-length
    * and diverge only inside the embedded draw's record dwords, so one
    * arming digest never authorizes the other stream.
    */
   struct r300_r2vb_producer_ib reference;
   CHECK(r300_r2vb_producer_reference_emit(&reference) == 0);
   CHECK(sweep.ib_size_dwords == reference.ib_size_dwords);
   CHECK(memcmp(sweep.ib, reference.ib,
                sweep.ib_size_dwords * sizeof(uint32_t)) != 0);
   struct walk_state st;
   CHECK(walk_stream(sweep.ib, sweep.ib_size_dwords, &st) == 0);
   uint32_t divergent = 0;
   for (uint32_t i = 0; i < sweep.ib_size_dwords; i++) {
      if (sweep.ib[i] == reference.ib[i])
         continue;
      divergent++;
      /* Vertex v occupies eight dwords after the two draw header
       * dwords; its record rides dwords 4..7 of that span.
       */
      CHECK(i > st.draw_header_index + 1);
      const uint32_t body_offset = i - (st.draw_header_index + 2);
      CHECK(body_offset < 3 * 8);
      CHECK(body_offset % 8 >= 4);
   }
   CHECK(divergent > 0 && divergent <= 12);
   r300_r2vb_producer_pass_release(&reference);
   r300_r2vb_producer_pass_release(&sweep);
}

static void
test_fp24_bisect_stream(void)
{
   /* The literals in the bisection table carry these exact encodings:
    * 2^32 through 2^58, every exponent 2^59 through 2^62, and the top
    * candidates 2^63, its maximum mantissa, 2^64, and its maximum
    * mantissa.
    */
   static const uint32_t bisect_bits
      [R300_R2VB_PRODUCER_FP24_BISECT_COUNT][4] = {
      { 0x4f800000u, 0x57800000u, 0x5b800000u, 0x5c800000u },
      { 0x5d000000u, 0x5d800000u, 0x5e000000u, 0x5e800000u },
      { 0x5f000000u, 0x5f7fff80u, 0x5f800000u, 0x5fffff80u },
   };
   uint32_t table_bits[R300_R2VB_PRODUCER_FP24_BISECT_COUNT][4];
   memcpy(table_bits, r300_r2vb_producer_fp24_bisect_records,
          sizeof(table_bits));
   CHECK(memcmp(table_bits, bisect_bits, sizeof(bisect_bits)) == 0);

   struct r300_r2vb_producer_ib bisect;
   CHECK(r300_r2vb_producer_fp24_bisect_emit(&bisect) == 0);
   CHECK(r300_r2vb_producer_pass_validate_reloc_sites(&bisect) == 0);

   /* Every lane sits inside the admission window, so the expected
    * carrier is the identity over the table.
    */
   uint32_t expected[R300_R2VB_PRODUCER_FP24_BISECT_COUNT * 4];
   CHECK(r300_r2vb_producer_fp24_bisect_expected(
            expected, R300_R2VB_PRODUCER_FP24_BISECT_COUNT * 4) == 0);
   CHECK(memcmp(expected, bisect_bits, sizeof(bisect_bits)) == 0);
   CHECK(r300_r2vb_producer_fp24_bisect_expected(expected, 11) == -ENOSPC);

   /* Equal-length streams diverging only in the embedded records: one
    * arming digest never authorizes another stream.
    */
   struct r300_r2vb_producer_ib sweep;
   CHECK(r300_r2vb_producer_fp24_sweep_emit(&sweep) == 0);
   CHECK(bisect.ib_size_dwords == sweep.ib_size_dwords);
   CHECK(memcmp(bisect.ib, sweep.ib,
                bisect.ib_size_dwords * sizeof(uint32_t)) != 0);
   r300_r2vb_producer_pass_release(&sweep);
   r300_r2vb_producer_pass_release(&bisect);

   /* The stream finder pairs each emission with its own oracle and
    * refuses unknown names, so a tool cannot mix streams.
    */
   const struct r300_r2vb_producer_stream_ops *ops =
      r300_r2vb_producer_stream_find("fp24-bisect");
   CHECK(ops != NULL &&
         ops->emit == r300_r2vb_producer_fp24_bisect_emit &&
         ops->expected == r300_r2vb_producer_fp24_bisect_expected);
   ops = r300_r2vb_producer_stream_find("reference");
   CHECK(ops != NULL && ops->emit == r300_r2vb_producer_reference_emit);
   ops = r300_r2vb_producer_stream_find("fp24-sweep");
   CHECK(ops != NULL && ops->emit == r300_r2vb_producer_fp24_sweep_emit);
   CHECK(r300_r2vb_producer_stream_find("fp24") == NULL);
   CHECK(r300_r2vb_producer_stream_find(NULL) == NULL);
}

static void
test_layout_domain(void)
{
   struct r300_r2vb_producer_layout layout;
   CHECK(r300_r2vb_producer_layout_single_row(0, &layout) == -EINVAL);
   CHECK(r300_r2vb_producer_layout_single_row(1024, &layout) == 0);
   CHECK(layout.width == 1024 && layout.pitch_pixels == 1024 &&
         layout.height == 1);
   CHECK(r300_r2vb_producer_layout_single_row(1025, &layout) == -EINVAL);
   CHECK(r300_r2vb_producer_layout_single_row(2047, &layout) == -EINVAL);
   CHECK(r300_r2vb_producer_layout_single_row(1, &layout) == 0);
   CHECK(layout.width == 2 && layout.pitch_pixels == 2 &&
         layout.height == 1);
}

static void
test_immediate_count_ceiling(void)
{
   static float records[1024][4];
   for (uint32_t v = 0; v < 1024; v++) {
      records[v][0] = (float)(v & 3u);
      records[v][1] = 1.0f;
      records[v][2] = 0.5f;
      records[v][3] = 1.0f;
   }

   struct r300_r2vb_producer_layout layout = {
      .count = 1024,
      .width = 1024,
      .height = 1,
      .pitch_pixels = 1024,
   };
   struct r300_fragment_binary fs;
   CHECK(r300_r2vb_producer_reference_fs(&fs) == 0);
   struct r300_r2vb_producer_params params = {
      .carrier_offset = 0,
      .layout = layout,
      .records = records,
      .first_draw_contract = NULL,
      .fragment_binary = &fs,
   };
   struct r300_r2vb_producer_ib pass = { 0 };
   const int emit_rc = r300_r2vb_producer_pass_emit(&params, &pass);
   CHECK(emit_rc == 0);
   if (emit_rc != 0) {
      r300_fragment_binary_finish(&fs);
      return;
   }
   const int validate_rc = r300_r2vb_producer_pass_validate_reloc_sites(&pass);
   CHECK(validate_rc == 0);
   if (validate_rc != 0) {
      r300_r2vb_producer_pass_release(&pass);
      r300_fragment_binary_finish(&fs);
      return;
   }
   struct walk_state st;
   const int walk_rc = walk_stream(pass.ib, pass.ib_size_dwords, &st);
   CHECK(walk_rc == 0);
   if (walk_rc != 0) {
      r300_r2vb_producer_pass_release(&pass);
      r300_fragment_binary_finish(&fs);
      return;
   }
   CHECK(st.draw_count == 1);
   if (st.draw_count == 1)
      CHECK(PACKET_GET_COUNT(pass.ib[st.draw_header_index]) == 1024 * 8);
   r300_r2vb_producer_pass_release(&pass);

   const uint32_t rejected_counts[] = { 1025, 2047 };
   for (unsigned i = 0; i < sizeof(rejected_counts) /
                                  sizeof(rejected_counts[0]); i++) {
      params.layout.count = rejected_counts[i];
      params.layout.width = rejected_counts[i] + 1;
      params.layout.pitch_pixels = rejected_counts[i] + 1;
      CHECK(r300_r2vb_producer_pass_emit(&params, &pass) == -EINVAL);
      CHECK(pass.ib == NULL && pass.ib_size_dwords == 0);
   }
   r300_fragment_binary_finish(&fs);
}

static void
test_refusals(void)
{
   struct r300_r2vb_producer_layout layout;
   CHECK(r300_r2vb_producer_layout_single_row(3, &layout) == 0);

   static const float records[3][4] = {
      { 1.0f, 2.0f, 0.5f, 1.0f },
      { 0.75f, 8.0f, 0.0f, 1.0f },
      { 56.0f, 0.5f, 0.25f, 1.0f },
   };

   struct r300_fragment_binary fs;
   CHECK(r300_r2vb_producer_reference_fs(&fs) == 0);

   /* Null records refuse. */
   struct r300_r2vb_producer_params params = {
      .carrier_offset = 0,
      .layout = layout,
      .records = NULL,
      .first_draw_contract = NULL,
      .fragment_binary = &fs,
   };
   struct r300_r2vb_producer_ib pass;
   CHECK(r300_r2vb_producer_pass_emit(&params, &pass) == -EINVAL);

   /* A pass without its own US program refuses: the slot pixels would
    * shade through the resident program of the previous client.
    */
   params.records = records;
   params.fragment_binary = NULL;
   CHECK(r300_r2vb_producer_pass_emit(&params, &pass) == -EINVAL);
   struct r300_fragment_binary unvalidated;
   memset(&unvalidated, 0, sizeof(unvalidated));
   params.fragment_binary = &unvalidated;
   CHECK(r300_r2vb_producer_pass_emit(&params, &pass) == -EINVAL);
   params.fragment_binary = &fs;

   /* A malformed layout refuses: odd pitch cannot encode. */
   params.layout.pitch_pixels = 3;
   params.layout.width = 3;
   CHECK(r300_r2vb_producer_pass_emit(&params, &pass) == -EINVAL);
   params.layout = layout;

   /* A record outside the FP24 fixed-point domain refuses -EDOM before
    * any write: 0.1 carries mantissa bits below the 16-bit window.
    */
   float bad[3][4];
   memcpy(bad, records, sizeof(bad));
   bad[1][2] = 0.1f;
   params.records = (const float(*)[4])bad;
   uint32_t canary[512];
   memset(canary, 0xc5, sizeof(canary));
   struct r300_r2vb_producer_ib into = { 0 };
   CHECK(r300_r2vb_producer_pass_emit_into(&params, canary, 512, &into) ==
         -EDOM);
   for (unsigned i = 0; i < 512; i++)
      CHECK(canary[i] == 0xc5c5c5c5u);
   params.records = records;

   /* Negative source values are outside the identity route: the measured
    * RS48x source-read model shifts them toward zero before the producer's
    * copy, so the shared admission must refuse them before emission. */
   static const float negative_records[3][4] = {
      { 1.0f, 2.0f, 0.5f, 1.0f },
      { -0.75f, 8.0f, 0.0f, 1.0f },
      { 56.0f, 0.5f, 0.25f, 1.0f },
   };
   params.records = negative_records;
   memset(canary, 0xc5, sizeof(canary));
   CHECK(r300_r2vb_producer_pass_emit_into(&params, canary, 512, &into) ==
         -EDOM);
   for (unsigned i = 0; i < 512; i++)
      CHECK(canary[i] == 0xc5c5c5c5u);
   params.records = records;

   /* A destination too small refuses -ENOSPC with no dword count. */
   CHECK(r300_r2vb_producer_pass_emit_into(&params, canary, 8, &into) ==
         -ENOSPC);
   CHECK(into.ib_size_dwords == 0);

   /* The admitted form emits clean without the contract prefix. */
   CHECK(r300_r2vb_producer_pass_emit(&params, &pass) == 0);
   CHECK(r300_r2vb_producer_pass_validate_reloc_sites(&pass) == 0);
   struct walk_state st;
   CHECK(walk_stream(pass.ib, pass.ib_size_dwords, &st) == 0);
   CHECK(st.draw_count == 1);
   CHECK(find_us_block(pass.ib, pass.ib_size_dwords) < st.draw_header_index);
   r300_r2vb_producer_pass_release(&pass);
   r300_fragment_binary_finish(&fs);
}

int
main(void)
{
   test_reference_structure();
   test_fp24_sweep_stream();
   test_fp24_bisect_stream();
   test_layout_domain();
   test_immediate_count_ceiling();
   test_refusals();

   if (failures != 0) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   printf("r300_r2vb_producer_pass_test: all checks passed\n");
   return 0;
}
