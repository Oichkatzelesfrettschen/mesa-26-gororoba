/* SPDX-License-Identifier: MIT */

#include "r300_zmask_clear_plan.h"

#include "r300_pm4_builder.h"
#include "r300_reg.h"

#include <errno.h>
#include <string.h>

/* r300_emit_zmask_clear writes OUT_CS_PKT3(3D_CLEAR_ZMASK, 2) followed
 * by zero, the level's ZMASK dword count, and zero: a start index, the
 * dwords to clear, and the value written into each.
 */
#define ZMASK_CLEAR_PAYLOAD_DWORDS 3u

/* The ladder as data: each stage's ZB_BW_CNTL word, the compression
 * block it pins, whether it admits the larger block on request, and the
 * evidence class it can demonstrate.  The two compressed words are
 * r300_update_hyperz's own groups, the decompression path's
 * FAST_FILL_ENABLE | RD_COMP_ENABLE and the in-use path's
 * FAST_FILL_ENABLE | RD_COMP_ENABLE | WR_COMP_ENABLE, so rule 8 holds by
 * construction: no row sets WR_COMP_ENABLE without RD_COMP_ENABLE, and
 * r300_zmask_clear_bw_cntl_check re-reads every word before it is
 * emitted.
 */
struct zmask_clear_stage_row {
   enum r300_zmask_clear_stage stage;
   uint32_t zb_bw_cntl;
   enum r300_zmask_compression block;
   bool admits_8x8;
   bool names_evidence_class;
   enum r300_zmask_evidence_class evidence_class;
   const char *name;
};

static const struct zmask_clear_stage_row stage_rows[] = {
   {R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY, 0u, R300_ZCOMP_4X4, false, false,
    R300_ZMASK_EVIDENCE_FAST_CLEAR_SUBSTITUTION, "depth only"},
   {R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY, 0u, R300_ZCOMP_4X4, false, false,
    R300_ZMASK_EVIDENCE_FAST_CLEAR_SUBSTITUTION, "ownership only"},
   {R300_ZMASK_CLEAR_STAGE_BIND_CLEAR, 0u, R300_ZCOMP_4X4, false, false,
    R300_ZMASK_EVIDENCE_FAST_CLEAR_SUBSTITUTION, "ZMASK bind and clear"},
   {R300_ZMASK_CLEAR_STAGE_FAST_FILL, R300_FAST_FILL_ENABLE, R300_ZCOMP_4X4,
    false, true, R300_ZMASK_EVIDENCE_FAST_CLEAR_SUBSTITUTION,
    "ZMASK bind and clear with fast fill"},
   {R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED,
    R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE, R300_ZCOMP_4X4, false, true,
    R300_ZMASK_EVIDENCE_COMPRESSED_READ, "ZMASK compressed read"},
   {R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED,
    R300_FAST_FILL_ENABLE | R300_RD_COMP_ENABLE | R300_WR_COMP_ENABLE,
    R300_ZCOMP_4X4, true, true, R300_ZMASK_EVIDENCE_COMPRESSED_WRITE,
    "ZMASK compressed write"},
};

#define STAGE_ROW_COUNT (sizeof(stage_rows) / sizeof(stage_rows[0]))

static const struct zmask_clear_stage_row *
find_stage_row(enum r300_zmask_clear_stage stage)
{
   for (size_t i = 0; i < STAGE_ROW_COUNT; i++) {
      if (stage_rows[i].stage == stage)
         return &stage_rows[i];
   }
   return NULL;
}

static void
emit_bind_and_clear(struct r300_pm4_builder *b,
                    const struct r300_zmask_layout *layout,
                    uint32_t zb_bw_cntl)
{
   /* ZB_ZMASK_OFFSET and ZB_ZMASK_PITCH are adjacent, so the bind is one
    * register run.  r300_emit_fb_state writes offset zero and the
    * surface's zmask_stride_in_pixels as the pitch, which places the
    * level at the base of the ZMASK RAM.
    */
   const uint32_t bind[] = {0u, layout->stride_in_pixels};
   r300_pm4_packet0(b, R300_ZB_ZMASK_OFFSET, bind, 2u);

   /* The autoincrementing ZMASK RAM access indices.  The kernel's HyperZ
    * table carries no row for either, so both admit on their own; the
    * plan writes them so the RAM window the clear fills starts where the
    * bind placed it rather than where a predecessor left the index.
    */
   r300_pm4_reg(b, R300_ZB_ZMASK_WRINDEX, 0u);
   r300_pm4_reg(b, R300_ZB_ZMASK_RDINDEX, 0u);

   /* The plane-equation format the block size names.  It and the
    * 3D_CLEAR_ZMASK payload below are read off one layout, so the
    * register and the coverage describe one surface: the 64x64 reference
    * level clears four dwords at 8x8 and sixteen at 4x4, and the builder
    * refuses a layout whose block disagrees with the stage before
    * reaching here.  The 4x4 case writes its value explicitly rather
    * than leaving the register at whatever a predecessor left.
    */
   r300_pm4_reg(b, R300_GB_Z_PEQ_CONFIG,
                layout->zcomp8x8 ? R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8
                                 : R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4);

   r300_pm4_reg(b, R300_ZB_BW_CNTL, zb_bw_cntl);

   const uint32_t clear[ZMASK_CLEAR_PAYLOAD_DWORDS] = {0u, layout->dwords,
                                                       0u};
   r300_pm4_packet3(b, R300_PACKET3_3D_CLEAR_ZMASK, clear,
                    ZMASK_CLEAR_PAYLOAD_DWORDS);
}

int
r300_zmask_clear_plan_build_at_block(enum r300_zmask_clear_stage stage,
                                     enum r300_zmask_compression block,
                                     const struct r300_zmask_layout *layout,
                                     struct r300_zmask_clear_plan *out)
{
   if (layout == NULL || out == NULL)
      return -EINVAL;

   const struct zmask_clear_stage_row *row = find_stage_row(stage);
   if (row == NULL || !r300_zmask_clear_stage_admits_block(stage, block))
      return -EINVAL;
   /* The word a stage emits is a table row, so this reads a value no
    * caller chose; it holds rule 8 over an edited row as well. */
   if (r300_zmask_clear_bw_cntl_check(row->zb_bw_cntl) != 0)
      return -EINVAL;

   struct r300_zmask_clear_plan plan;
   memset(&plan, 0, sizeof(plan));

   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, plan.words, R300_ZMASK_CLEAR_PLAN_MAX_DWORDS);

   switch (stage) {
   case R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY:
      break;
   case R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY:
      plan.requires_hyperz_ownership = true;
      break;
   case R300_ZMASK_CLEAR_STAGE_BIND_CLEAR:
   case R300_ZMASK_CLEAR_STAGE_FAST_FILL:
   case R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED:
   case R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED:
      if (!layout->fits_zmask_ram || layout->dwords == 0u ||
          layout->stride_in_pixels == 0u ||
          layout->zmask_ram_dwords == 0u ||
          layout->dwords > layout->zmask_ram_dwords)
         return -EINVAL;
      /* The block the layout was computed at is the block this stage
       * programs.  A layout resolved at the other one is refused rather
       * than emitted at a coverage its register contradicts. */
      if (layout->zcomp8x8 != (block == R300_ZCOMP_8X8))
         return -EINVAL;
      plan.requires_hyperz_ownership = true;
      plan.writes_hyperz_registers = true;
      /* SC_HYPERZ stays unwritten: the scan converter's HiZ bit belongs
       * to the HiZ stage past this ladder.
       */
      emit_bind_and_clear(&b, layout, row->zb_bw_cntl);
      break;
   default:
      return -EINVAL;
   }

   const int err = r300_pm4_builder_finish(&b, &plan.dword_count);
   if (err != 0)
      return err;

   *out = plan;
   return 0;
}

int
r300_zmask_clear_plan_build(enum r300_zmask_clear_stage stage,
                            const struct r300_zmask_layout *layout,
                            struct r300_zmask_clear_plan *out)
{
   return r300_zmask_clear_plan_build_at_block(
      stage, r300_zmask_clear_stage_block(stage), layout, out);
}

int
r300_zmask_fast_clear_plan_build(
   const struct r300_zb_depth_surface *surface,
   const struct r300_zmask_layout *layout, uint32_t depth_code,
   uint32_t stencil, struct r300_zmask_clear_plan *out)
{
   if (surface == NULL || layout == NULL || out == NULL ||
       r300_zb_depth_surface_check(surface) != 0 ||
       surface->bytes_per_pixel != 4u || !surface->microtile ||
       !surface->macrotile)
      return -EINVAL;

   struct r300_zmask_clear_plan bind_plan;
   if (r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_FAST_FILL, layout,
                                   &bind_plan) != 0)
      return -EINVAL;

   uint32_t clear_word;
   if (r300_zb_depth_pack(surface, depth_code, stencil, &clear_word) != 0)
      return -EINVAL;

   struct r300_zmask_clear_plan plan;
   memset(&plan, 0, sizeof(plan));
   struct r300_pm4_builder builder;
   r300_pm4_builder_init(&builder, plan.words,
                         R300_ZMASK_CLEAR_PLAN_MAX_DWORDS);
   r300_pm4_reg(&builder, R300_ZB_DEPTHCLEARVALUE, clear_word);
   r300_pm4_block(&builder, bind_plan.words, bind_plan.dword_count);
   const int result = r300_pm4_builder_finish(&builder, &plan.dword_count);
   if (result != 0)
      return result;
   plan.requires_hyperz_ownership = bind_plan.requires_hyperz_ownership;
   plan.writes_hyperz_registers = bind_plan.writes_hyperz_registers;
   *out = plan;
   return 0;
}

enum r300_zmask_compression
r300_zmask_clear_stage_block(enum r300_zmask_clear_stage stage)
{
   const struct zmask_clear_stage_row *row = find_stage_row(stage);
   return row != NULL ? row->block : R300_ZCOMP_4X4;
}

bool
r300_zmask_clear_stage_admits_block(enum r300_zmask_clear_stage stage,
                                    enum r300_zmask_compression block)
{
   const struct zmask_clear_stage_row *row = find_stage_row(stage);
   if (row == NULL)
      return false;
   if (block == row->block)
      return true;
   return block == R300_ZCOMP_8X8 && row->admits_8x8;
}

uint32_t
r300_zmask_clear_stage_bw_cntl(enum r300_zmask_clear_stage stage)
{
   const struct zmask_clear_stage_row *row = find_stage_row(stage);
   return row != NULL ? row->zb_bw_cntl : 0u;
}

int
r300_zmask_clear_bw_cntl_check(uint32_t zb_bw_cntl)
{
   const bool fast_fill = (zb_bw_cntl & R300_FAST_FILL_ENABLE) != 0u;
   const bool read_compressed = (zb_bw_cntl & R300_RD_COMP_ENABLE) != 0u;
   const bool write_compressed = (zb_bw_cntl & R300_WR_COMP_ENABLE) != 0u;
   /* Rule 8 of the fast-clear notes in r300_blit.c: FASTFILL cannot be
    * used to compress the zbuffer, so a word that asks the pipe to write
    * compressed tiles while reading uncompressed ones is refused. */
   if (fast_fill && write_compressed && !read_compressed)
      return -EINVAL;
   return 0;
}

bool
r300_zmask_clear_stage_evidence_class(
   enum r300_zmask_clear_stage stage,
   enum r300_zmask_evidence_class *out_class)
{
   const struct zmask_clear_stage_row *row = find_stage_row(stage);
   if (row == NULL || !row->names_evidence_class)
      return false;
   if (out_class != NULL)
      *out_class = row->evidence_class;
   return true;
}

const char *
r300_zmask_evidence_class_name(enum r300_zmask_evidence_class c)
{
   switch (c) {
   case R300_ZMASK_EVIDENCE_FAST_CLEAR_SUBSTITUTION:
      return "fast-clear metadata substitution";
   case R300_ZMASK_EVIDENCE_COMPRESSED_READ:
      return "compressed read of a non-clear tile";
   case R300_ZMASK_EVIDENCE_COMPRESSED_WRITE:
      return "compressed write producing nonuniform contents";
   case R300_ZMASK_EVIDENCE_CLASS_COUNT:
      break;
   }
   return NULL;
}

int
r300_zmask_clear_stages_self_check(void)
{
   for (size_t i = 0; i < STAGE_ROW_COUNT; i++) {
      const struct zmask_clear_stage_row *row = &stage_rows[i];
      if (row->stage != (enum r300_zmask_clear_stage)i || row->name == NULL)
         return -EINVAL;
      if (r300_zmask_clear_bw_cntl_check(row->zb_bw_cntl) != 0)
         return -EINVAL;
      /* HiZ has no RAM on CHIP_RS480 and SC_HYPERZ stays unwritten, so
       * no row carries the HiZ enable. */
      if ((row->zb_bw_cntl & R300_HIZ_ENABLE) != 0u)
         return -EINVAL;
      /* The class a row names and the mechanism it enables are the same
       * fact read two ways: substitution needs FAST_FILL alone,
       * a compressed read needs RD_COMP, a compressed write needs
       * WR_COMP. */
      if (row->names_evidence_class) {
         const uint32_t required =
            row->evidence_class == R300_ZMASK_EVIDENCE_COMPRESSED_WRITE
               ? R300_WR_COMP_ENABLE
            : row->evidence_class == R300_ZMASK_EVIDENCE_COMPRESSED_READ
               ? R300_RD_COMP_ENABLE
               : R300_FAST_FILL_ENABLE;
         if ((row->zb_bw_cntl & required) == 0u)
            return -EINVAL;
         if (r300_zmask_evidence_class_name(row->evidence_class) == NULL)
            return -EINVAL;
      } else if (row->zb_bw_cntl != 0u) {
         return -EINVAL;
      }
      /* 8x8 opens where compressed writes do and nowhere else. */
      if (row->admits_8x8 !=
          ((row->zb_bw_cntl & R300_WR_COMP_ENABLE) != 0u))
         return -EINVAL;
   }
   return 0;
}

const char *
r300_zmask_clear_stage_name(enum r300_zmask_clear_stage stage)
{
   const struct zmask_clear_stage_row *row = find_stage_row(stage);
   return row != NULL ? row->name : NULL;
}
