/* SPDX-License-Identifier: MIT */

#include "r300_tcl_bypass_triangle.h"
#include "r300_fragment_binary.h"

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

#define R300_TRIANGLE_MAX_DWORDS 256

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
