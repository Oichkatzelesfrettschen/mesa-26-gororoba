/* SPDX-License-Identifier: MIT */

#include "r300_tcl_bypass_triangle.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_tcl_bypass_triangle_fs_block.h"

#include "r300_reg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The radeon CS grammar for a BO reference: a type-3 NOP whose one payload
 * dword indexes the relocation chunk in dword units, four dwords per
 * drm_radeon_cs_reloc entry.
 */
#define R300_TRIANGLE_PACKET3_NOP 0x00001000
#define R300_TRIANGLE_RELOC_HEADER CP_PACKET3(R300_TRIANGLE_PACKET3_NOP, 0)
#define R300_TRIANGLE_RELOC_PAYLOAD(slot) ((slot) * 4)

/* Identity PSC swizzle select: X, Y, Z, W in place with a full write mask,
 * the exact per-word value the kernel's TCL-bypass vertex-output check
 * requires on every VAP_PROG_STREAM_CNTL_EXT word.
 */
#define R300_TRIANGLE_PSC_EXT_IDENTITY 0xF688F688u

/* Holds the bare-cell state plus the two-dword-per-clause first-draw
 * contract prefix; the prefix emission checks its room against this bound
 * and the allocation adds the fragment binary's size on top.
 */
#define R300_TRIANGLE_MAX_DWORDS 512

struct r300_triangle_writer {
   uint32_t *ib;
   uint32_t count;
   struct r300_tcl_bypass_triangle_ib *out;
};

static void
write_dword(struct r300_triangle_writer *w, uint32_t value)
{
   w->ib[w->count++] = value;
}

static void
write_reg(struct r300_triangle_writer *w, uint32_t reg, uint32_t value)
{
   write_dword(w, CP_PACKET0(reg, 0));
   write_dword(w, value);
}

static void
write_reloc(struct r300_triangle_writer *w, uint32_t slot)
{
   write_dword(w, R300_TRIANGLE_RELOC_HEADER);
   w->out->reloc_sites[w->out->reloc_site_count++] =
      (struct r300_tcl_bypass_triangle_reloc_site){
         .ib_index = w->count,
         .slot = slot,
      };
   write_dword(w, R300_TRIANGLE_RELOC_PAYLOAD(slot));
}

int
r300_tcl_bypass_triangle_emit(
   const struct r300_tcl_bypass_triangle_params *params,
   struct r300_tcl_bypass_triangle_ib *out)
{
   const struct r300_fragment_binary *fs = params->fragment_binary;

   if (fs == NULL || !fs->validated) {
      return -EINVAL;
   }

   memset(out, 0, sizeof(*out));
   uint32_t *ib = calloc(R300_TRIANGLE_MAX_DWORDS + fs->cb_code_size,
                         sizeof(uint32_t));
   if (ib == NULL) {
      return -ENOMEM;
   }

   struct r300_triangle_writer w = {.ib = ib, .count = 0, .out = out};
   out->ib = ib;

   /* First-draw state prefix: the contract's writes land before any cell
    * state, so every register the draw depends on -- the three proven
    * color-write gates included -- comes from this stream rather than
    * from the previous client.
    */
   if (params->first_draw_contract != NULL) {
      int state_dwords = r300_first_draw_state_emit(
         params->first_draw_contract, &ib[w.count],
         R300_TRIANGLE_MAX_DWORDS - w.count);
      if (state_dwords < 0) {
         free(ib);
         memset(out, 0, sizeof(*out));
         return state_dwords;
      }
      w.count += (uint32_t)state_dwords;
   }

   /* Vertex path: pretransformed positions bypass the TCL block, one
    * FLOAT_4 stream lands whole in output vector zero, and every PSC
    * extended selector stays identity, so the kernel's vertex-output check
    * can prove VAP_VTX_SIZE = 4 covers the fetch.
    */
   write_reg(&w, R300_VAP_CNTL_STATUS, R300_VAP_TCL_BYPASS);
   write_reg(&w, R300_VAP_PROG_STREAM_CNTL_0,
             R300_DATA_TYPE_FLOAT_4 | (0 << R300_DST_VEC_LOC_SHIFT) |
                R300_LAST_VEC);
   write_dword(&w, CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_EXT_0, 7));
   for (unsigned i = 0; i < 8; i++) {
      write_dword(&w, R300_TRIANGLE_PSC_EXT_IDENTITY);
   }
   write_reg(&w, R300_VAP_OUTPUT_VTX_FMT_0,
             R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
   write_reg(&w, R300_VAP_OUTPUT_VTX_FMT_1, 0);
   write_reg(&w, R300_VAP_VTX_SIZE, 4);

   /* Fragment program: the owned binary's US/FG block verbatim, then the
    * two register values the descriptor keeps outside the sequence.
    */
   memcpy(&ib[w.count], fs->cb_code, fs->cb_code_size * sizeof(uint32_t));
   w.count += fs->cb_code_size;
   write_reg(&w, R300_FG_DEPTH_SRC, fs->fg_depth_src);
   write_reg(&w, R300_US_W_FMT, fs->us_out_w);

   /* One color target, depth disabled.  RB3D_COLOROFFSET carries the color
    * BO reference; the pitch/format word travels plain because the
    * submission sets RADEON_CS_KEEP_TILING_FLAGS.
    */
   write_reg(&w, R300_RB3D_CCTL, 0);
   write_reg(&w, R300_ZB_CNTL, 0);
   write_reg(&w, R300_RB3D_COLOROFFSET0, 0);
   write_reloc(&w, R300_TRIANGLE_SLOT_COLOR);
   write_reg(&w, R300_RB3D_COLORPITCH0, params->color_pitch_format);

   /* Vertex fetch: one array, sixteen bytes per vertex, stride sixteen. */
   write_dword(&w, CP_PACKET3(R300_PACKET3_3D_LOAD_VBPNTR, 2));
   write_dword(&w, 1 | R300_VC_FORCE_PREFETCH);
   write_dword(&w, R300_VBPNTR_SIZE0(16) | R300_VBPNTR_STRIDE0(16));
   write_dword(&w, params->vertex_offset);
   write_reloc(&w, R300_TRIANGLE_SLOT_VERTEX);

   /* One vertex-list triangle; the draw packet carries VAP_VF_CNTL. */
   write_dword(&w, CP_PACKET3(R300_PACKET3_3D_DRAW_VBUF_2, 0));
   write_dword(&w, R300_VAP_VF_CNTL__PRIM_TRIANGLES | R300_PRIM_WALK_LIST |
                      (3 << R300_PRIM_NUM_VERTICES_SHIFT));

   /* Destination-cache publication retires the color writes before the IB
    * completes.
    */
   write_reg(&w, R300_RB3D_DSTCACHE_CTLSTAT,
             R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);

   out->ib_size_dwords = w.count;
   return 0;
}

void
r300_tcl_bypass_triangle_release(struct r300_tcl_bypass_triangle_ib *ib)
{
   free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_tcl_bypass_triangle_reference_fs(struct r300_fragment_binary *fs)
{
   return r300_fragment_binary_init(
      fs, r300_tcl_bypass_triangle_fs_block,
      sizeof(r300_tcl_bypass_triangle_fs_block) /
         sizeof(r300_tcl_bypass_triangle_fs_block[0]),
      R300_TCL_BYPASS_TRIANGLE_FS_FG_DEPTH_SRC,
      R300_TCL_BYPASS_TRIANGLE_FS_US_OUT_W,
      "r300-tcl-bypass-triangle-compiled");
}

int
r300_tcl_bypass_triangle_reference_contract(
   struct r300_first_draw_contract *out)
{
   /* The cell's target is 64x64, the draw fetches vertices 0..2, and it
    * binds no texture; the contract resolution derives scissor, clip, and
    * the maximum vertex index from these parameters.
    */
   struct r300_first_draw_params params = {
      .width = 64,
      .height = 64,
      .max_vtx_index = 2,
      .texture_enabled = false,
   };
   return r300_first_draw_contract_resolve(&params, out);
}

int
r300_tcl_bypass_triangle_reference_emit(
   struct r300_tcl_bypass_triangle_ib *out)
{
   struct r300_fragment_binary fs;
   int rc = r300_tcl_bypass_triangle_reference_fs(&fs);
   if (rc != 0)
      return rc;

   struct r300_first_draw_contract contract;
   rc = r300_tcl_bypass_triangle_reference_contract(&contract);
   if (rc != 0) {
      r300_fragment_binary_finish(&fs);
      return rc;
   }

   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = r300_rb3d_colorpitch0_pack_argb8888(64),
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   rc = r300_tcl_bypass_triangle_emit(&params, out);
   r300_fragment_binary_finish(&fs);
   return rc;
}

uint32_t
r300_rb3d_colorpitch0_pack_argb8888(uint32_t pitch_pixels)
{
   /* R300_COLORPITCH_MASK covers bits 1-13, so the pitch is even and at
    * most 0x3ffe pixels; the format field is R300_COLOR_FORMAT_ARGB8888,
    * and the linear little-endian target keeps tile, microtile, and
    * endian at zero.
    */
   if (pitch_pixels == 0 || (pitch_pixels & 1) != 0 ||
       pitch_pixels > R300_COLORPITCH_MASK)
      return 0;
   return pitch_pixels | R300_COLOR_FORMAT_ARGB8888;
}

/* Edge function twice the signed area of (a, b, p); the triangle's
 * vertices wind so every interior point yields three positive values.
 */
static int32_t
triangle_edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px,
              int32_t py)
{
   return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static bool
triangle_interior(uint32_t x, uint32_t y)
{
   /* The cell's vertices: (8,8), (56,8), (32,56). */
   return triangle_edge(8, 8, 56, 8, (int32_t)x, (int32_t)y) > 0 &&
          triangle_edge(56, 8, 32, 56, (int32_t)x, (int32_t)y) > 0 &&
          triangle_edge(32, 56, 8, 8, (int32_t)x, (int32_t)y) > 0;
}

void
r300_tcl_bypass_triangle_oracle(const uint32_t *pixels, uint32_t size_bytes,
                                struct r300_triangle_oracle_verdict *verdict)
{
   /* Sample points sit at least two pixels from every triangle edge, so
    * the verdict does not ride the hardware's exact fill rule.
    */
   static const struct { uint8_t x, y; } interior[] = {
      { 32, 20 }, { 20, 12 }, { 44, 12 }, { 32, 44 },
   };
   static const struct { uint8_t x, y; } exterior[] = {
      { 0, 0 }, { 63, 0 }, { 0, 63 }, { 63, 63 }, { 2, 32 }, { 61, 32 },
   };

   *verdict = (struct r300_triangle_oracle_verdict) {
      .interior_pass = true,
      .exterior_pass = true,
      .canary_pass = true,
   };

   const uint32_t pixel_count = size_bytes / 4;
   for (uint32_t i = 0; i < pixel_count; i++) {
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL) {
         verdict->executed = true;
         break;
      }
   }

   /* Each sample also re-proves its own triangle membership, so a sample
    * table drifting out of the analytic geometry reads as a failed
    * verdict instead of a wrong oracle.
    */
   for (unsigned i = 0; i < sizeof(interior) / sizeof(interior[0]); i++) {
      uint32_t index = (uint32_t)interior[i].y * 64 + interior[i].x;
      if (!triangle_interior(interior[i].x, interior[i].y) ||
          index >= pixel_count ||
          pixels[index] != R300_TRIANGLE_DRAW_COLOR_ARGB8888)
         verdict->interior_pass = false;
   }
   for (unsigned i = 0; i < sizeof(exterior) / sizeof(exterior[0]); i++) {
      uint32_t index = (uint32_t)exterior[i].y * 64 + exterior[i].x;
      if (triangle_interior(exterior[i].x, exterior[i].y) ||
          index >= pixel_count ||
          pixels[index] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->exterior_pass = false;
   }
   for (uint32_t i = 64 * 64; i < pixel_count; i++) {
      if (pixels[i] != R300_TRIANGLE_COLOR_SENTINEL)
         verdict->canary_pass = false;
   }
}

/* TCL bypass consumes screen-space positions: an inset triangle inside the
 * 64x64 target, z = 0, w = 1.
 */
const float r300_tcl_bypass_triangle_vertices[R300_TRIANGLE_VERTEX_DWORDS] = {
    8.0f,  8.0f, 0.0f, 1.0f,
   56.0f,  8.0f, 0.0f, 1.0f,
   32.0f, 56.0f, 0.0f, 1.0f,
};
