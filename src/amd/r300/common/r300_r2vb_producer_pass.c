/* SPDX-License-Identifier: MIT */

#include "r300_r2vb_producer_pass.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_r2vb_carrier_delivery.h"
#include "r300_r2vb_producer_fs_block.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_vertex_format.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot.
 */
#define R300_R2VB_PRODUCER_RELOC_PAYLOAD(slot) ((slot) * 4)

/* Prologue, varying routing, draw framing, and publication tail outside
 * the per-vertex body; the emission bound adds the contract prefix, the
 * fragment binary's US block, and the embedded records on top.
 */
#define R300_R2VB_PRODUCER_FIXED_MAX_DWORDS 112u

/* Slot position plus one pre-swizzled record, each FP32x4. */
#define R300_R2VB_PRODUCER_VTX_DWORDS 8u

/* The immediate producer's retained input tuple places the slot position in
 * VAP vector 0 and the model varying in vector 6.  The packet uses
 * R300_DATA_TYPE_FLOAT_4, R300_DST_VEC_LOC_SHIFT, and R300_LAST_VEC from
 * r300_reg.h (rg --fixed-strings R300_DST_VEC_LOC_SHIFT src/); the lower and
 * upper halves share one PROG_STREAM_CNTL word, and the model element
 * terminates the fetch.  The producer-manifest oracle checks this tuple. */
#define R300_R2VB_PRODUCER_PSC_SLOT_VEC 0u
#define R300_R2VB_PRODUCER_PSC_MODEL_VEC 6u

/* The standalone producer has no inherited vertex-array state.  Keep the
 * complete two-element PSC tuple and its zero tail in the stream, with the
 * identity XYZW swizzle for both FP32x4 elements. */
static const uint32_t r300_r2vb_producer_psc[8] = {
   (R300_DATA_TYPE_FLOAT_4 |
    (R300_R2VB_PRODUCER_PSC_SLOT_VEC << R300_DST_VEC_LOC_SHIFT)) |
      ((R300_DATA_TYPE_FLOAT_4 |
        (R300_R2VB_PRODUCER_PSC_MODEL_VEC << R300_DST_VEC_LOC_SHIFT) |
        R300_LAST_VEC)
       << 16),
   0,
   0,
   0,
   0,
   0,
   0,
   0,
};

static const uint32_t r300_r2vb_producer_psc_ext[8] = {
   R300_VAP_SWIZZLE_XYZW | (R300_VAP_SWIZZLE_XYZW << 16),
   0,
   0,
   0,
   0,
   0,
   0,
   0,
};

/* The draw header below is written as a raw dword.  Keep the shared 1024
 * admission inside the packet's 14-bit payload-count encoding; the bound in
 * the public header also leaves room for the surrounding state in one IB.
 */
static_assert(1u + R300_R2VB_PRODUCER_MAX_COUNT *
                      R300_R2VB_PRODUCER_VTX_DWORDS <=
                 R300_PM4_MAX_RUN,
              "the embedded body fits the PACKET3 payload bound");

int
r300_r2vb_producer_layout_single_row(uint32_t count,
                                     struct r300_r2vb_producer_layout *out)
{
   memset(out, 0, sizeof(*out));
   if (count < 1 || count > R300_R2VB_PRODUCER_MAX_COUNT)
      return -EINVAL;
   const uint32_t row = (count + 1u) & ~1u;
   out->count = count;
   out->width = row;
   out->height = 1;
   out->pitch_pixels = row;
   return 0;
}

static void
write_reloc(struct r300_pm4_builder *b, struct r300_r2vb_producer_ib *out,
            uint32_t slot)
{
   if (b->error != 0)
      return;
   if (slot >= R300_R2VB_PRODUCER_SLOT_COUNT ||
       out->reloc_site_count >= R300_R2VB_PRODUCER_SLOT_COUNT) {
      b->error = -EINVAL;
      return;
   }

   const uint32_t index =
      r300_pm4_reloc_nop(b, R300_R2VB_PRODUCER_RELOC_PAYLOAD(slot));
   if (index == R300_PM4_NO_INDEX)
      return;

   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_r2vb_producer_reloc_site){
         .ib_index = index,
         .slot = slot,
      };
}

static uint32_t
float_bits(float f)
{
   uint32_t bits;
   memcpy(&bits, &f, sizeof(bits));
   return bits;
}

static bool
layout_valid(const struct r300_r2vb_producer_layout *layout)
{
   return layout->count >= 1 &&
          layout->count <= R300_R2VB_PRODUCER_MAX_COUNT &&
          layout->height == 1 && layout->width == layout->pitch_pixels &&
          layout->pitch_pixels == ((layout->count + 1u) & ~1u);
}

int
r300_r2vb_producer_pass_emit_into(
   const struct r300_r2vb_producer_params *params, uint32_t *words,
   uint32_t capacity, struct r300_r2vb_producer_ib *out)
{
   const struct r300_r2vb_producer_layout *layout = &params->layout;
   const struct r300_fragment_binary *fs = params->fragment_binary;

   memset(out, 0, sizeof(*out));
   if (params->records == NULL || !layout_valid(layout))
      return -EINVAL;
   if (fs == NULL || !fs->validated)
      return -EINVAL;

   /* All-or-nothing FP24 domain scan: the US datapath narrows every
    * routed component to s1e7m16, so a record outside the fixed-point
    * domain cannot survive the pass byte-exact and refuses the whole
    * emission before any dword is written.
    */
   for (uint32_t v = 0; v < layout->count; v++) {
      for (uint32_t c = 0; c < 4; c++) {
         if (!r300_r2vb_fp24_identity_admits(
                float_bits(params->records[v][c])))
            return -EDOM;
      }
   }

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, capacity);

   /* First-draw state prefix: the contract's writes land before any
    * producer state, so the kernel tracker and the hardware read texture,
    * blend, and z-state from this stream rather than from the previous
    * client.
    */
   if (params->first_draw_contract != NULL && b.error == 0) {
      const uint32_t state_dwords =
         r300_first_draw_state_dwords(params->first_draw_contract);
      if (r300_pm4_builder_reserve(&b, state_dwords)) {
         const int emitted = r300_first_draw_state_emit(
            params->first_draw_contract, &b.words[b.count], state_dwords);
         if (emitted < 0)
            b.error = emitted;
         else if ((uint32_t)emitted != state_dwords)
            b.error = -EINVAL;
         else
            b.count += state_dwords;
      }
   }

   /* Target prologue.  Depth and antialias off, screendoor open, scissor
    * confined to the slot row under the non-R500 1440 raster bias.
    */
   r300_pm4_reg(&b, R300_ZB_CNTL, 0);
   static const uint32_t mspos[2] = { 0x66666666, 0x06666666 };
   r300_pm4_packet0(&b, R300_GB_MSPOS0, mspos, ARRAY_SIZE(mspos));
   r300_pm4_reg(&b, R300_GB_AA_CONFIG, R300_GB_AA_CONFIG_AA_DISABLE);
   r300_pm4_reg(&b, R300_RB3D_AARESOLVE_CTL, 0);
   r300_pm4_reg(&b, R300_SC_SCREENDOOR, 0x00ffffff);
   const uint32_t scissors[2] = {
      (1440u << R300_SCISSORS_X_SHIFT) | (1440u << R300_SCISSORS_Y_SHIFT),
      ((layout->width + 1440u - 1u) << R300_SCISSORS_X_SHIFT) |
         ((layout->height + 1440u - 1u) << R300_SCISSORS_Y_SHIFT),
   };
   r300_pm4_packet0(&b, R300_SC_SCISSORS_TL, scissors,
                    ARRAY_SIZE(scissors));

   /* Destination-cache barrier before the retarget: dirty tiles of the
    * previously bound target flush while its COLOROFFSET is still
    * programmed, so the retarget cannot drop another surface's cached
    * pixels.
    */
   r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   r300_pm4_reg(&b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(&b, RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);

   /* Carrier retarget: one color target, FP32x4 pixels.  The pitch word
    * travels plain because the submission sets
    * RADEON_CS_KEEP_TILING_FLAGS.
    */
   r300_pm4_reg(&b, R300_RB3D_CCTL, 0);
   r300_pm4_reg(&b, R300_RB3D_COLOROFFSET0, params->carrier_offset);
   write_reloc(&b, out, R300_R2VB_PRODUCER_SLOT_CARRIER);
   r300_pm4_reg(&b, R300_RB3D_COLORPITCH0,
                layout->pitch_pixels | R300_COLOR_FORMAT_ARGB32323232);

   /* BGRA output select: the embedded records travel pre-swizzled as
    * (z, y, x, w), and the select reverses that order, so the carrier
    * stores the source (x, y, z, w) bytes.
    */
   const uint32_t us_out_fmt[4] = {
      R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_B | R300_C1_SEL_G |
         R300_C2_SEL_R | R300_C3_SEL_A,
      R300_US_OUT_FMT_UNUSED,
      R300_US_OUT_FMT_UNUSED,
      R300_US_OUT_FMT_UNUSED,
   };
   r300_pm4_packet0(&b, R300_US_OUT_FMT_0, us_out_fmt,
                    ARRAY_SIZE(us_out_fmt));

   /* Full-write color backend: ROP, blend, dither, and alpha test each
    * pass every shaded dword through unchanged.
    */
   r300_pm4_reg(&b, R300_RB3D_ROPCNTL, 0);
   const uint32_t cblend[3] = {
      0,
      0,
      RB3D_COLOR_CHANNEL_MASK_BLUE_MASK0 |
         RB3D_COLOR_CHANNEL_MASK_GREEN_MASK0 |
         RB3D_COLOR_CHANNEL_MASK_RED_MASK0 |
         RB3D_COLOR_CHANNEL_MASK_ALPHA_MASK0,
   };
   r300_pm4_packet0(&b, R300_RB3D_CBLEND, cblend, ARRAY_SIZE(cblend));
   r300_pm4_reg(&b, R300_RB3D_DITHER_CTL, 0);
   r300_pm4_reg(&b, R300_FG_ALPHA_FUNC, R300_FG_ALPHA_FUNC_DISABLE);

   r300_pm4_reg(&b, R300_SU_CULL_MODE, 0);
   r300_pm4_reg(&b, R300_SC_CLIP_RULE, 0xFFFF);

   /* One-pixel points: GA_POINT_SIZE holds sixths of a pixel per axis,
    * so 6/6 rasterizes each slot vertex as exactly its own pixel.
    */
   r300_pm4_reg(&b, R300_GA_POINT_SIZE,
                (6u << R300_POINTSIZE_Y_SHIFT) |
                   (6u << R300_POINTSIZE_X_SHIFT));
   r300_pm4_reg(&b, R300_GA_POINT_MINMAX,
                (6u << R300_GA_POINT_MINMAX_MIN_SHIFT) |
                   (6u << R300_GA_POINT_MINMAX_MAX_SHIFT));

   /* Pretransformed slot positions bypass the TCL block; clip off and
    * VTE passthrough deliver each (slot + 0.5) center to the raster
    * unchanged.
    */
   r300_pm4_reg(&b, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   r300_pm4_reg(&b, R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
   r300_pm4_reg(&b, R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);

   /* Complete VAP input and output tuple: two FP32x4 inputs (slot position
    * and model varying), a position plus one four-component TEX0 output,
    * and the eight-dword fetch size.  The zero tail prevents an inherited
    * stream from adding a phantom fetch to the embedded body.
    */
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_0,
                    r300_r2vb_producer_psc,
                    ARRAY_SIZE(r300_r2vb_producer_psc));
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_EXT_0,
                    r300_r2vb_producer_psc_ext,
                    ARRAY_SIZE(r300_r2vb_producer_psc_ext));
   r300_pm4_reg(&b, R300_VAP_VTX_SIZE, R300_R2VB_PRODUCER_VTX_DWORDS);
   r300_pm4_reg(&b, R300_VAP_VTX_STATE_CNTL, 0x5555);
   r300_pm4_reg(&b, R300_VAP_VSM_VTX_ASSM,
                R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0);
   const uint32_t vap_output_fmt[2] = {
      R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT,
      R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS,
   };
   r300_pm4_packet0(&b, R300_VAP_OUTPUT_VTX_FMT_0, vap_output_fmt,
                    ARRAY_SIZE(vap_output_fmt));

   /* Varying routing: the VAP writes position plus one four-component
    * TEX0 varying, so RS_COUNT declares four interpolated components with
    * no rasterized colors, RS_IP_0 reads texture pointer zero and selects
    * its four channels in order, and RS_INST_0 writes the result to US
    * input register zero.  RS_INST_COUNT of zero runs instruction 0
    * alone, so an inherited RS_IP_1..7 residue stays out of the shaded
    * value.
    */
   r300_pm4_reg(&b, R300_RS_COUNT,
                R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN);
   r300_pm4_reg(&b, R300_RS_INST_COUNT, 0);
   r300_pm4_reg(&b, R300_RS_IP_0,
                R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
                   R300_RS_SEL_T(R300_RS_SEL_C1) |
                   R300_RS_SEL_R(R300_RS_SEL_C2) |
                   R300_RS_SEL_Q(R300_RS_SEL_C3));
   r300_pm4_reg(&b, R300_RS_INST_0,
                R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
                   R300_RS_INST_TEX_ADDR(0));

   /* Fragment program: the owned binary's US/FG block verbatim, then the
    * two register values the descriptor keeps outside the sequence.  The
    * block establishes the US code the slot pixels shade through, so the
    * carrier receives the interpolated record rather than whatever
    * program the previous client left resident.
    */
   r300_pm4_block(&b, fs->cb_code, fs->cb_code_size);
   r300_pm4_reg(&b, R300_FG_DEPTH_SRC, fs->fg_depth_src);
   r300_pm4_reg(&b, R300_US_W_FMT, fs->us_out_w);

   /* Embedded POINTS draw: the packet carries VAP_VF_CNTL and then
    * count * 8 dwords, one slot position plus one pre-swizzled record
    * per vertex.
    */
   r300_pm4_reg(&b, R300_VAP_VF_MAX_VTX_INDX, layout->count - 1);

   const uint32_t body_dwords =
      1 + layout->count * R300_R2VB_PRODUCER_VTX_DWORDS;
   if (r300_pm4_builder_reserve(&b, 1 + body_dwords)) {
      r300_pm4_dword(&b, CP_PACKET3(R300_PACKET3_3D_DRAW_IMMD_2,
                                    body_dwords - 1));
      r300_pm4_dword(&b, R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED |
                            (layout->count << 16) |
                            R300_VAP_VF_CNTL__PRIM_POINTS);
      for (uint32_t v = 0; v < layout->count; v++) {
         r300_pm4_dword(&b, float_bits((float)v + 0.5f));
         r300_pm4_dword(&b, float_bits(0.5f));
         r300_pm4_dword(&b, float_bits(0.0f));
         r300_pm4_dword(&b, float_bits(1.0f));
         r300_pm4_dword(&b, float_bits(params->records[v][2]));
         r300_pm4_dword(&b, float_bits(params->records[v][1]));
         r300_pm4_dword(&b, float_bits(params->records[v][0]));
         r300_pm4_dword(&b, float_bits(params->records[v][3]));
      }
   }

   /* Publication tail: push the carrier out of the color caches, wait
    * 3D idle-clean, then sync the VAP.  The cache flushes leave the
    * vertex cache untouched, and a later vertex fetch of this same BO
    * can return stale content the vertex cache kept from an earlier
    * fetch of the recycled GART page; writing zero to
    * VAP_PVS_STATE_FLUSH_REG synchronizes the engine and clears that
    * state before the re-ingest.
    */
   r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   r300_pm4_reg(&b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(&b, RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
   r300_pm4_reg(&b, R300_VAP_PVS_STATE_FLUSH_REG, 0);

   const int rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (rc != 0) {
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   return 0;
}

int
r300_r2vb_producer_pass_emit(const struct r300_r2vb_producer_params *params,
                             struct r300_r2vb_producer_ib *out)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;

   memset(out, 0, sizeof(*out));
   if (params->records == NULL || !layout_valid(&params->layout))
      return -EINVAL;
   if (fs == NULL || !fs->validated)
      return -EINVAL;

   uint32_t capacity = R300_R2VB_PRODUCER_FIXED_MAX_DWORDS +
                       fs->cb_code_size +
                       params->layout.count * R300_R2VB_PRODUCER_VTX_DWORDS;
   if (params->first_draw_contract != NULL)
      capacity +=
         r300_first_draw_state_dwords(params->first_draw_contract);

   uint32_t *ib = calloc(capacity, sizeof(uint32_t));
   if (ib == NULL)
      return -ENOMEM;

   const int rc =
      r300_r2vb_producer_pass_emit_into(params, ib, capacity, out);
   if (rc != 0) {
      free(ib);
      return rc;
   }
   out->owns_ib = true;
   return 0;
}

void
r300_r2vb_producer_pass_release(struct r300_r2vb_producer_ib *ib)
{
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_r2vb_producer_pass_validate_reloc_sites(
   const struct r300_r2vb_producer_ib *ib)
{
   if (ib->reloc_site_count != R300_R2VB_PRODUCER_SLOT_COUNT)
      return -EINVAL;

   const struct r300_r2vb_producer_reloc_site *site = &ib->reloc_sites[0];
   if (site->slot != R300_R2VB_PRODUCER_SLOT_CARRIER)
      return -EINVAL;
   /* The site is the payload dword of a relocation NOP, so the header
    * precedes it; an in-range ordinary dword that happens to equal the
    * payload does not qualify.
    */
   if (site->ib_index == 0 || site->ib_index >= ib->ib_size_dwords)
      return -ERANGE;
   if (ib->ib[site->ib_index - 1] != CP_PACKET3(R300_PM4_PACKET3_NOP, 0))
      return -EINVAL;
   if (ib->ib[site->ib_index] !=
       R300_R2VB_PRODUCER_RELOC_PAYLOAD(site->slot))
      return -EINVAL;

   return 0;
}

int
r300_r2vb_producer_reference_fs(struct r300_fragment_binary *fs)
{
   return r300_fragment_binary_init(
      fs, r300_r2vb_producer_fs_block,
      ARRAY_SIZE(r300_r2vb_producer_fs_block),
      R300_R2VB_PRODUCER_FS_FG_DEPTH_SRC, R300_R2VB_PRODUCER_FS_US_OUT_W,
      "r300-r2vb-producer-compiled");
}

int
r300_r2vb_producer_reference_emit(struct r300_r2vb_producer_ib *out)
{
   memset(out, 0, sizeof(*out));

   struct r300_r2vb_producer_layout layout;
   int rc = r300_r2vb_producer_layout_single_row(
      R300_R2VB_PRODUCER_REFERENCE_COUNT, &layout);
   if (rc != 0)
      return rc;

   /* The contract resolves for the slot row's extent, so scissor, clip,
    * and the maximum vertex index derive from the carrier geometry.
    */
   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = layout.width,
      .height = layout.height,
      .max_vtx_index = layout.count - 1,
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc != 0)
      return rc;

   /* The reference records are the fixed triangle's three FLOAT_4
    * vertices, the same bytes the CPU gather and the delivery identity
    * control carry.
    */
   struct r300_fragment_binary fs;
   rc = r300_r2vb_producer_reference_fs(&fs);
   if (rc != 0)
      return rc;
   struct r300_r2vb_producer_params params = {
      .carrier_offset = 0,
      .layout = layout,
      .records = (const float(*)[4])r300_tcl_bypass_triangle_vertices,
      .first_draw_contract = &contract,
      .fragment_binary = &fs,
   };
   rc = r300_r2vb_producer_pass_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_r2vb_producer_reference_expected(uint32_t *expected,
                                      uint32_t expected_dwords)
{
   if (expected == NULL)
      return -EINVAL;
   if (expected_dwords < R300_R2VB_PRODUCER_REFERENCE_COUNT * 4)
      return -ENOSPC;

   const struct r300_cpu_vertex_stream stream = {
      .data = (const uint8_t *)r300_tcl_bypass_triangle_vertices,
      .stride = 4 * sizeof(float),
      .size_bytes = (uint64_t)R300_R2VB_PRODUCER_REFERENCE_COUNT * 4 *
                    sizeof(float),
   };
   return r300_r2vb_identity_deliver(R300_VERTEX_FORMAT_F32_4, &stream, 0,
                                     R300_R2VB_PRODUCER_REFERENCE_COUNT,
                                     expected, expected_dwords);
}

int
r300_r2vb_producer_carrier_check(
   const uint32_t *expected, uint32_t expected_dwords, uint32_t poison,
   const void *carrier, uint32_t carrier_bytes,
   struct r300_r2vb_producer_carrier_verdict *out)
{
   if (expected == NULL || carrier == NULL || out == NULL ||
       expected_dwords == 0)
      return -EINVAL;
   if (carrier_bytes % sizeof(uint32_t) != 0)
      return -EINVAL;
   const uint32_t carrier_dwords = carrier_bytes / sizeof(uint32_t);
   if (carrier_dwords < expected_dwords)
      return -EINVAL;

   /* The poison names a dword the pass left unwritten, so a delivered
    * value equal to it leaves the negative side undecidable and the
    * check refuses the pairing before reading the carrier.
    */
   for (uint32_t i = 0; i < expected_dwords; i++) {
      if (expected[i] == poison)
         return -EINVAL;
   }

   memset(out, 0, sizeof(*out));
   const uint32_t *dwords = carrier;
   for (uint32_t i = 0; i < expected_dwords; i++) {
      if (dwords[i] != expected[i])
         out->mismatched_dwords++;
   }
   for (uint32_t i = expected_dwords; i < carrier_dwords; i++) {
      if (dwords[i] != poison)
         out->disturbed_tail_dwords++;
   }
   out->expected_pass = out->mismatched_dwords == 0;
   out->tail_poison_pass = out->disturbed_tail_dwords == 0;
   return 0;
}
