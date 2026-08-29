/* SPDX-License-Identifier: MIT */

#include "r300_r2vb_float2_tuple_pass.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_pm4_builder.h"
#include "r300_r2vb_carrier_delivery.h"
#include "r300_r2vb_producer_pass.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Four dwords per drm_radeon_cs_reloc entry, so a slot's payload indexes
 * the relocation chunk at four times the slot.
 */
#define TUPLE_RELOC_PAYLOAD(slot) ((slot) * 4)

/* Prologue, varying routing, fetch and draw framing, and publication
 * tail; the emission bound adds the contract prefix and the fragment
 * binary's US block on top.  The fetched draw embeds no vertex body.
 */
#define TUPLE_FIXED_MAX_DWORDS 120u

/* The fetched input tuple places the FLOAT_4 slot position in VAP vector
 * 0 and the FLOAT_2 model element in vector 6, terminating the fetch.
 * The XY01 selector expands the model fetch to (x, y, 0, 1); the slot
 * element keeps the identity swizzle.
 */
#define TUPLE_PSC_SLOT_VEC 0u
#define TUPLE_PSC_MODEL_VEC 6u

static const uint32_t tuple_psc[8] = {
   (R300_DATA_TYPE_FLOAT_4 | (TUPLE_PSC_SLOT_VEC << R300_DST_VEC_LOC_SHIFT)) |
      ((R300_DATA_TYPE_FLOAT_2 |
        (TUPLE_PSC_MODEL_VEC << R300_DST_VEC_LOC_SHIFT) | R300_LAST_VEC)
       << 16),
   0,
   0,
   0,
   0,
   0,
   0,
   0,
};

static const uint32_t tuple_psc_ext[8] = {
   R300_VAP_SWIZZLE_XYZW | (R300_VAP_SWIZZLE_XY01 << 16),
   0,
   0,
   0,
   0,
   0,
   0,
   0,
};

/* The FLOAT_4 model contrast: both elements fetch at full width under
 * the identity swizzle, so the logical input equals the stored record
 * with no PSC expansion.  Symbol discovery uses (rg --fixed-strings
 * float4_model_psc src/amd/r300/common/; rg --fixed-strings
 * R300_DATA_TYPE_FLOAT_4 src/amd/r300/common/).
 */
static const uint32_t float4_model_psc[8] = {
   (R300_DATA_TYPE_FLOAT_4 | (TUPLE_PSC_SLOT_VEC << R300_DST_VEC_LOC_SHIFT)) |
      ((R300_DATA_TYPE_FLOAT_4 |
        (TUPLE_PSC_MODEL_VEC << R300_DST_VEC_LOC_SHIFT) | R300_LAST_VEC)
       << 16),
   0,
   0,
   0,
   0,
   0,
   0,
   0,
};

static const uint32_t float4_model_psc_ext[8] = {
   R300_VAP_SWIZZLE_XYZW | (R300_VAP_SWIZZLE_XYZW << 16),
   0,
   0,
   0,
   0,
   0,
   0,
   0,
};

static uint32_t
float_bits(float f)
{
   uint32_t bits;
   memcpy(&bits, &f, sizeof(bits));
   return bits;
}

static void
write_reloc(struct r300_pm4_builder *b,
            struct r300_r2vb_float2_tuple_ib *out, uint32_t slot)
{
   if (b->error != 0)
      return;
   if (slot >= R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT ||
       out->reloc_site_count >= R300_R2VB_FLOAT2_TUPLE_RELOC_SITES) {
      b->error = -EINVAL;
      return;
   }

   const uint32_t index =
      r300_pm4_reloc_nop(b, TUPLE_RELOC_PAYLOAD(slot));
   if (index == R300_PM4_NO_INDEX)
      return;

   out->reloc_sites[out->reloc_site_count++] =
      (struct r300_r2vb_float2_tuple_reloc_site){
         .ib_index = index,
         .slot = slot,
      };
}

static void
write_le32(uint8_t **p, uint32_t bits)
{
   (*p)[0] = (uint8_t)bits;
   (*p)[1] = (uint8_t)(bits >> 8);
   (*p)[2] = (uint8_t)(bits >> 16);
   (*p)[3] = (uint8_t)(bits >> 24);
   *p += 4;
}

/* Serializes the slot-position array followed by the model array.  The
 * FLOAT_4 model form stores each record's XY01 expansion explicitly, so
 * the stored model bytes equal the logical input either fetch delivers.
 */
static int
tuple_width_vertex_stream(const float (*records)[2], uint32_t count,
                          uint8_t *vertex_bytes,
                          uint32_t vertex_capacity_bytes, bool model_float4)
{
   if (records == NULL || vertex_bytes == NULL || count < 1 ||
       count > R300_R2VB_PRODUCER_MAX_COUNT)
      return -EINVAL;
   const uint32_t model_stride =
      model_float4 ? R300_R2VB_FLOAT4_MODEL_STRIDE_BYTES
                   : R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES;
   const uint32_t needed =
      count * (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES + model_stride);
   if (vertex_capacity_bytes < needed)
      return -ENOSPC;

   uint8_t *p = vertex_bytes;
   for (uint32_t v = 0; v < count; v++) {
      write_le32(&p, float_bits((float)v + 0.5f));
      write_le32(&p, float_bits(0.5f));
      write_le32(&p, float_bits(0.0f));
      write_le32(&p, float_bits(1.0f));
   }
   for (uint32_t v = 0; v < count; v++) {
      write_le32(&p, float_bits(records[v][0]));
      write_le32(&p, float_bits(records[v][1]));
      if (model_float4) {
         write_le32(&p, float_bits(0.0f));
         write_le32(&p, float_bits(1.0f));
      }
   }
   return 0;
}

int
r300_r2vb_float2_tuple_vertex_stream(const float (*records)[2],
                                     uint32_t count, uint8_t *vertex_bytes,
                                     uint32_t vertex_capacity_bytes)
{
   return tuple_width_vertex_stream(records, count, vertex_bytes,
                                    vertex_capacity_bytes, false);
}

int
r300_r2vb_float4_model_vertex_stream(const float (*records)[2],
                                     uint32_t count, uint8_t *vertex_bytes,
                                     uint32_t vertex_capacity_bytes)
{
   return tuple_width_vertex_stream(records, count, vertex_bytes,
                                    vertex_capacity_bytes, true);
}

int
r300_r2vb_float2_tuple_expected(const float (*records)[2], uint32_t count,
                                uint32_t *expected,
                                uint32_t expected_dwords)
{
   if (records == NULL || expected == NULL || count < 1 ||
       count > R300_R2VB_PRODUCER_MAX_COUNT)
      return -EINVAL;
   if (expected_dwords < count * 4)
      return -ENOSPC;
   for (uint32_t v = 0; v < count; v++) {
      for (uint32_t c = 0; c < 2; c++) {
         const uint32_t bits = float_bits(records[v][c]);
         if (!r300_r2vb_fp24_identity_admits(bits))
            return -EDOM;
         expected[v * 4 + c] = bits;
      }
      expected[v * 4 + 2] = 0;
      expected[v * 4 + 3] = float_bits(1.0f);
   }
   return 0;
}

static int
tuple_width_pass_emit(const struct r300_r2vb_float2_tuple_params *params,
                      struct r300_r2vb_float2_tuple_ib *out,
                      bool model_float4)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;

   memset(out, 0, sizeof(*out));
   if (params->records == NULL || params->count < 1 ||
       params->count > R300_R2VB_PRODUCER_MAX_COUNT)
      return -EINVAL;
   if (fs == NULL || !fs->validated)
      return -EINVAL;

   struct r300_r2vb_producer_layout layout;
   int rc = r300_r2vb_producer_layout_single_row(params->count, &layout);
   if (rc != 0)
      return rc;

   /* All-or-nothing FP24 domain scan over the model components; the
    * synthesized 0.0 and 1.0 lanes are fixed points by construction.
    */
   for (uint32_t v = 0; v < params->count; v++) {
      for (uint32_t c = 0; c < 2; c++) {
         if (!r300_r2vb_fp24_identity_admits(
                float_bits(params->records[v][c])))
            return -EDOM;
      }
   }

   uint32_t capacity = TUPLE_FIXED_MAX_DWORDS + fs->cb_code_size;
   if (params->first_draw_contract != NULL)
      capacity +=
         r300_first_draw_state_dwords(params->first_draw_contract);

   uint32_t *words = calloc(capacity, sizeof(uint32_t));
   if (words == NULL)
      return -ENOMEM;

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, capacity);

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

   /* Target prologue, carrier retarget, and color backend exactly as the
    * immediate producer emits them, except the output select: the model
    * varying arrives in source order (x, y, 0, 1) from the PSC
    * expansion, so the straight RGBA select stores it unchanged.
    */
   r300_pm4_reg(&b, R300_ZB_CNTL, 0);
   static const uint32_t mspos[2] = { 0x66666666, 0x06666666 };
   r300_pm4_packet0(&b, R300_GB_MSPOS0, mspos, ARRAY_SIZE(mspos));
   r300_pm4_reg(&b, R300_GB_AA_CONFIG, R300_GB_AA_CONFIG_AA_DISABLE);
   r300_pm4_reg(&b, R300_RB3D_AARESOLVE_CTL, 0);
   r300_pm4_reg(&b, R300_SC_SCREENDOOR, 0x00ffffff);
   const uint32_t scissors[2] = {
      (1440u << R300_SCISSORS_X_SHIFT) | (1440u << R300_SCISSORS_Y_SHIFT),
      ((layout.width + 1440u - 1u) << R300_SCISSORS_X_SHIFT) |
         ((layout.height + 1440u - 1u) << R300_SCISSORS_Y_SHIFT),
   };
   r300_pm4_packet0(&b, R300_SC_SCISSORS_TL, scissors,
                    ARRAY_SIZE(scissors));

   r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   r300_pm4_reg(&b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(&b, RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);

   r300_pm4_reg(&b, R300_RB3D_CCTL, 0);
   r300_pm4_reg(&b, R300_RB3D_COLOROFFSET0, params->carrier_offset);
   write_reloc(&b, out, R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER);
   r300_pm4_reg(&b, R300_RB3D_COLORPITCH0,
                layout.pitch_pixels | R300_COLOR_FORMAT_ARGB32323232);

   const uint32_t us_out_fmt[4] = {
      R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_R | R300_C1_SEL_G |
         R300_C2_SEL_B | R300_C3_SEL_A,
      R300_US_OUT_FMT_UNUSED,
      R300_US_OUT_FMT_UNUSED,
      R300_US_OUT_FMT_UNUSED,
   };
   r300_pm4_packet0(&b, R300_US_OUT_FMT_0, us_out_fmt,
                    ARRAY_SIZE(us_out_fmt));

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

   r300_pm4_reg(&b, R300_GA_POINT_SIZE,
                (6u << R300_POINTSIZE_Y_SHIFT) |
                   (6u << R300_POINTSIZE_X_SHIFT));
   r300_pm4_reg(&b, R300_GA_POINT_MINMAX,
                (6u << R300_GA_POINT_MINMAX_MIN_SHIFT) |
                   (6u << R300_GA_POINT_MINMAX_MAX_SHIFT));

   r300_pm4_reg(&b, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   r300_pm4_reg(&b, R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
   r300_pm4_reg(&b, R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);

   /* The two-element fetch: FLOAT_4 identity plus the width-selected
    * model element (FLOAT_2 XY01 at six fetched dwords, or FLOAT_4
    * identity at eight), position plus one four-component TEX0 output.
    * The zero tail keeps inherited elements out of the fetch; the
    * kernel width check reads the same declaration.
    */
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_0,
                    model_float4 ? float4_model_psc : tuple_psc, 8);
   r300_pm4_packet0(&b, R300_VAP_PROG_STREAM_CNTL_EXT_0,
                    model_float4 ? float4_model_psc_ext : tuple_psc_ext, 8);
   r300_pm4_reg(&b, R300_VAP_VTX_SIZE,
                model_float4 ? R300_R2VB_FLOAT4_MODEL_VTX_SIZE_DWORDS
                             : R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS);
   r300_pm4_reg(&b, R300_VAP_VTX_STATE_CNTL, 0x5555);
   r300_pm4_reg(&b, R300_VAP_VSM_VTX_ASSM,
                R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0);
   const uint32_t vap_output_fmt[2] = {
      R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT,
      R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS,
   };
   r300_pm4_packet0(&b, R300_VAP_OUTPUT_VTX_FMT_0, vap_output_fmt,
                    ARRAY_SIZE(vap_output_fmt));

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

   r300_pm4_block(&b, fs->cb_code, fs->cb_code_size);
   r300_pm4_reg(&b, R300_FG_DEPTH_SRC, fs->fg_depth_src);
   r300_pm4_reg(&b, R300_US_W_FMT, fs->us_out_w);

   r300_pm4_emit_vertex_index_range(&b, 0, layout.count - 1);

   /* Two-array vertex fetch: the FLOAT_4 slot array at the vertex
    * offset, the model array behind it at its width's stride, one
    * relocation per pointer, both resolving to the vertex BO.
    */
   const uint32_t model_stride =
      model_float4 ? R300_R2VB_FLOAT4_MODEL_STRIDE_BYTES
                   : R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES;
   const uint32_t model_offset =
      params->vertex_offset +
      params->count * R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES;
   const uint32_t vbpntr[4] = {
      2 | R300_VC_FORCE_PREFETCH,
      R300_VBPNTR_SIZE0(R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES) |
         R300_VBPNTR_STRIDE0(R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES) |
         R300_VBPNTR_SIZE1(model_stride) |
         R300_VBPNTR_STRIDE1(model_stride),
      params->vertex_offset,
      model_offset,
   };
   r300_pm4_packet3(&b, R300_PACKET3_3D_LOAD_VBPNTR, vbpntr,
                    ARRAY_SIZE(vbpntr));
   write_reloc(&b, out, R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX);
   write_reloc(&b, out, R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX);

   /* One vertex-list POINTS draw; the draw packet carries VAP_VF_CNTL. */
   const uint32_t draw = R300_VAP_VF_CNTL__PRIM_POINTS |
                         R300_PRIM_WALK_LIST |
                         (layout.count << 16);
   r300_pm4_packet3(&b, R300_PACKET3_3D_DRAW_VBUF_2, &draw, 1);

   /* Producer publication tail: color caches flushed, 3D idle-clean,
    * VAP synchronized before any later fetch of the carrier.
    */
   r300_pm4_reg(&b, R300_ZB_ZCACHE_CTLSTAT,
                R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
   r300_pm4_reg(&b, R300_RB3D_DSTCACHE_CTLSTAT,
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                   R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
   r300_pm4_reg(&b, RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
   r300_pm4_reg(&b, R300_VAP_PVS_STATE_FLUSH_REG, 0);

   rc = r300_pm4_builder_finish(&b, &out->ib_size_dwords);
   if (rc != 0) {
      free(words);
      memset(out, 0, sizeof(*out));
      return rc;
   }
   out->ib = words;
   out->owns_ib = true;
   return 0;
}

int
r300_r2vb_float2_tuple_pass_emit(
   const struct r300_r2vb_float2_tuple_params *params,
   struct r300_r2vb_float2_tuple_ib *out)
{
   return tuple_width_pass_emit(params, out, false);
}

int
r300_r2vb_float4_model_pass_emit(
   const struct r300_r2vb_float2_tuple_params *params,
   struct r300_r2vb_float2_tuple_ib *out)
{
   return tuple_width_pass_emit(params, out, true);
}

void
r300_r2vb_float2_tuple_pass_release(struct r300_r2vb_float2_tuple_ib *ib)
{
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_r2vb_float2_tuple_pass_validate_reloc_sites(
   const struct r300_r2vb_float2_tuple_ib *ib)
{
   static const uint32_t expected_slots[R300_R2VB_FLOAT2_TUPLE_RELOC_SITES] = {
      R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER,
      R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX,
      R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX,
   };

   if (ib->reloc_site_count != R300_R2VB_FLOAT2_TUPLE_RELOC_SITES)
      return -EINVAL;

   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_r2vb_float2_tuple_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot != expected_slots[i])
         return -EINVAL;
      if (site->ib_index == 0 || site->ib_index >= ib->ib_size_dwords)
         return -ERANGE;
      if (ib->ib[site->ib_index - 1] != CP_PACKET3(R300_PM4_PACKET3_NOP, 0))
         return -EINVAL;
      if (ib->ib[site->ib_index] != TUPLE_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
   }
   return 0;
}

/* The unit test pins each component to its binary32 encoding, so a
 * literal drifting off the lattice fails calibration.
 */
const float r300_r2vb_float2_tuple_reference_records
   [R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT][2] = {
   { 8.0f, 0.75f },
   { 56.0f, 1.0f },
   { 999.0f, 2.0f },
};

static int
tuple_width_reference_emit(struct r300_r2vb_float2_tuple_ib *out,
                           bool model_float4)
{
   memset(out, 0, sizeof(*out));

   struct r300_r2vb_producer_layout layout;
   int rc = r300_r2vb_producer_layout_single_row(
      R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, &layout);
   if (rc != 0)
      return rc;

   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = layout.width,
      .height = layout.height,
      .min_vtx_index = 0,
      .max_vtx_index = layout.count - 1,
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc != 0)
      return rc;

   struct r300_fragment_binary fs;
   rc = r300_r2vb_producer_reference_fs(&fs);
   if (rc != 0)
      return rc;
   struct r300_r2vb_float2_tuple_params params = {
      .carrier_offset = 0,
      .vertex_offset = 0,
      .count = R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT,
      .records = r300_r2vb_float2_tuple_reference_records,
      .first_draw_contract = &contract,
      .fragment_binary = &fs,
   };
   rc = tuple_width_pass_emit(&params, out, model_float4);
   r300_fragment_binary_finish(&fs);
   return rc;
}

int
r300_r2vb_float2_tuple_reference_emit(struct r300_r2vb_float2_tuple_ib *out)
{
   return tuple_width_reference_emit(out, false);
}

int
r300_r2vb_float4_model_reference_emit(struct r300_r2vb_float2_tuple_ib *out)
{
   return tuple_width_reference_emit(out, true);
}

int
r300_r2vb_float2_tuple_reference_expected(uint32_t *expected,
                                          uint32_t expected_dwords)
{
   return r300_r2vb_float2_tuple_expected(
      r300_r2vb_float2_tuple_reference_records,
      R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, expected, expected_dwords);
}

int
r300_r2vb_float2_tuple_burst_member_stride_bytes(uint32_t *out)
{
   struct r300_r2vb_producer_layout layout;
   int rc = r300_r2vb_producer_layout_single_row(
      R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, &layout);
   if (rc != 0)
      return rc;
   *out = layout.pitch_pixels * layout.height * R300_R2VB_PRODUCER_CPP_BYTES;
   return 0;
}

static int
tuple_width_burst_reference_emit(uint32_t draws,
                                 struct r300_r2vb_float2_tuple_burst_ib *out,
                                 bool model_float4)
{
   memset(out, 0, sizeof(*out));
   if (draws < 1 || draws > R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS)
      return -EINVAL;

   struct r300_r2vb_producer_layout layout;
   int rc = r300_r2vb_producer_layout_single_row(
      R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, &layout);
   if (rc != 0)
      return rc;
   const uint32_t member_stride =
      layout.pitch_pixels * layout.height * R300_R2VB_PRODUCER_CPP_BYTES;

   struct r300_first_draw_params draw_params = {
      .chip_family = CHIP_RS480,
      .width = layout.width,
      .height = layout.height,
      .min_vtx_index = 0,
      .max_vtx_index = layout.count - 1,
      .texture_enabled = false,
   };
   struct r300_first_draw_contract contract;
   rc = r300_first_draw_contract_resolve(&draw_params, &contract);
   if (rc != 0)
      return rc;

   struct r300_fragment_binary fs;
   rc = r300_r2vb_producer_reference_fs(&fs);
   if (rc != 0)
      return rc;

   /* Each member is the single pass's own emission, byte for byte, so
    * the composition inherits the qualified stream by construction; the
    * concatenation only rebases each member's relocation-site indices.
    */
   struct r300_r2vb_float2_tuple_ib
      members[R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS];
   memset(members, 0, sizeof(members));
   uint32_t emitted = 0;
   uint32_t total_dwords = 0;
   for (uint32_t m = 0; rc == 0 && m < draws; m++) {
      struct r300_r2vb_float2_tuple_params params = {
         .carrier_offset = m * member_stride,
         .vertex_offset = 0,
         .count = R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT,
         .records = r300_r2vb_float2_tuple_reference_records,
         .first_draw_contract = m == 0 ? &contract : NULL,
         .fragment_binary = &fs,
      };
      rc = tuple_width_pass_emit(&params, &members[m], model_float4);
      if (rc == 0) {
         emitted = m + 1;
         rc = r300_r2vb_float2_tuple_pass_validate_reloc_sites(&members[m]);
      }
      if (rc == 0 &&
          total_dwords > UINT32_MAX - members[m].ib_size_dwords)
         rc = -EOVERFLOW;
      if (rc == 0)
         total_dwords += members[m].ib_size_dwords;
   }
   r300_fragment_binary_finish(&fs);

   uint32_t *words = NULL;
   if (rc == 0) {
      words = calloc(total_dwords, sizeof(uint32_t));
      if (words == NULL)
         rc = -ENOMEM;
   }
   if (rc == 0) {
      uint32_t base = 0;
      for (uint32_t m = 0; m < draws; m++) {
         out->member_start[m] = base;
         memcpy(&words[base], members[m].ib,
                members[m].ib_size_dwords * sizeof(uint32_t));
         for (uint32_t s = 0; s < members[m].reloc_site_count; s++) {
            out->reloc_sites[out->reloc_site_count++] =
               (struct r300_r2vb_float2_tuple_reloc_site){
                  .ib_index = base + members[m].reloc_sites[s].ib_index,
                  .slot = members[m].reloc_sites[s].slot,
               };
         }
         base += members[m].ib_size_dwords;
      }
      out->ib = words;
      out->ib_size_dwords = total_dwords;
      out->draws = draws;
      out->owns_ib = true;
   }
   for (uint32_t m = 0; m < emitted; m++)
      r300_r2vb_float2_tuple_pass_release(&members[m]);
   if (rc != 0) {
      free(words);
      memset(out, 0, sizeof(*out));
   }
   return rc;
}

int
r300_r2vb_float2_tuple_burst_reference_emit(
   uint32_t draws, struct r300_r2vb_float2_tuple_burst_ib *out)
{
   return tuple_width_burst_reference_emit(draws, out, false);
}

int
r300_r2vb_float4_model_burst_reference_emit(
   uint32_t draws, struct r300_r2vb_float2_tuple_burst_ib *out)
{
   return tuple_width_burst_reference_emit(draws, out, true);
}

void
r300_r2vb_float2_tuple_burst_release(
   struct r300_r2vb_float2_tuple_burst_ib *ib)
{
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_r2vb_float2_tuple_burst_validate_reloc_sites(
   const struct r300_r2vb_float2_tuple_burst_ib *ib)
{
   static const uint32_t member_slots[R300_R2VB_FLOAT2_TUPLE_RELOC_SITES] = {
      R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER,
      R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX,
      R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX,
   };

   if (ib->draws < 1 || ib->draws > R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS)
      return -EINVAL;
   if (ib->reloc_site_count !=
       ib->draws * R300_R2VB_FLOAT2_TUPLE_RELOC_SITES)
      return -EINVAL;

   uint32_t previous_index = 0;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_r2vb_float2_tuple_reloc_site *site =
         &ib->reloc_sites[i];
      const uint32_t member = i / R300_R2VB_FLOAT2_TUPLE_RELOC_SITES;
      if (site->slot !=
          member_slots[i % R300_R2VB_FLOAT2_TUPLE_RELOC_SITES])
         return -EINVAL;
      if (site->ib_index == 0 || site->ib_index >= ib->ib_size_dwords)
         return -ERANGE;
      if (site->ib_index <= previous_index)
         return -ERANGE;
      if (site->ib_index < ib->member_start[member])
         return -ERANGE;
      if (ib->ib[site->ib_index - 1] != CP_PACKET3(R300_PM4_PACKET3_NOP, 0))
         return -EINVAL;
      if (ib->ib[site->ib_index] != TUPLE_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
      previous_index = site->ib_index;
   }
   return 0;
}
