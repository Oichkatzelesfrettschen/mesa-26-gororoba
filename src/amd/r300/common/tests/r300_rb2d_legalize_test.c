/*
 * SPDX-License-Identifier: MIT
 *
 * The legalizer's obligations: every raw rejection the kernel tracker
 * names has a semantic equivalent the legalizer lowers into windows the
 * window checker admits and whose bytes equal the request interval
 * exactly; the V1 contract emits the plan emitter's bytes unchanged; the
 * window checker refuses each invariant when it alone is broken; the
 * pitch registry admits nothing beyond its evidence; and the chooser
 * ranks by the cost model.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_rb2d_legalize.h"
#include "radeon_legacy_2d_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static struct r300_rb2d_window windows[R300_RB2D_LEGALIZE_MAX_WINDOWS];

static struct r300_rb2d_legalize_request
request(uint64_t offset, uint64_t size, uint64_t bo_size,
        enum r300_rb2d_contract contract)
{
   return (struct r300_rb2d_legalize_request){
      .byte_offset = offset,
      .byte_size = size,
      .pattern = 0x11223344u,
      .bo_size = bo_size,
      .usage = R300_RB2D_USAGE_FILL_BUFFER,
      .contract = contract,
      .minimum_evidence = R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT,
   };
}

/* Legalizes and holds the byte set to the interval through the
 * independent per-byte oracle. */
static uint32_t
legalize_exactly(const struct r300_rb2d_legalize_request *req,
                 struct r300_rb2d_legalize_result *result)
{
   const uint32_t n = r300_rb2d_legalize_linear_span(
      req, windows, ARRAY_LEN(windows), result);
   if (result->refusal != R300_RB2D_LEGALIZE_OK) {
      fprintf(stderr, "legalize refused offset %llu size %llu bo %llu: %s / %s / %s\n",
              (unsigned long long)req->byte_offset,
              (unsigned long long)req->byte_size,
              (unsigned long long)req->bo_size,
              r300_rb2d_legalize_refusal_name(result->refusal),
              r300_rb2d_span_refusal_name(result->span_refusal),
              r300_rb2d_window_refusal_name(result->window_refusal));
      abort();
   }
   assert(n == result->window_count && n > 0u);

   uint8_t *counts = calloc((size_t)req->bo_size, 1);
   assert(counts != NULL);
   assert(r300_rb2d_windows_touched_bytes(windows, n, counts, req->bo_size));
   for (uint64_t b = 0; b < req->bo_size; b++) {
      const bool inside =
         b >= req->byte_offset && b < req->byte_offset + req->byte_size;
      assert(counts[b] == (inside ? 1u : 0u));
   }
   free(counts);

   for (uint32_t s = 0; s < n; s++) {
      assert(r300_rb2d_window_check(&windows[s], req->bo_size) ==
             R300_RB2D_WINDOW_OK);
      assert(windows[s].relocation_site == s);
   }
   return n;
}

/* The transformation table: each row is a raw rejection class the kernel
 * tracker names, stated as the semantic request whose naive rectangle
 * would have triggered it.  Every row legalizes and covers exactly.  The
 * table pins the witnessed 256-byte carrier, so each row names the
 * decomposition of that carrier's 64-pixel row and the x bound below
 * holds. */
static void
test_transformation_table(void)
{
   struct row {
      const char *raw_reject;
      uint64_t offset, size, bo_size;
      enum r300_rb2d_contract contract;
      uint32_t expect_windows;
   };
   static const struct row rows[] = {
      /* x past pitch: offset 260 on a 256-byte row is x = 1, y = 1. */
      { "x past pitch", 260u, 8u, 4096u, R300_RB2D_CONTRACT_CONST_FILL_V1,
        1u },
      /* width overruns pitch: 300 bytes from x = 3 splits across rows. */
      { "width overruns pitch", 12u, 300u, 4096u,
        R300_RB2D_CONTRACT_CONST_FILL_V1, 1u },
      /* y too large / height too large: beyond one window's reach on the
       * 256-byte carrier, which is 0x1fff rows. */
      { "y beyond one surface", 256u * 0x1fffu + 512u, 1024u,
        256u * 0x1fffu + 4096u, R300_RB2D_CONTRACT_CONST_FILL_V2, 1u },
      { "height overrun", 0u, 256u * 0x1fffu + 512u, 256u * 0x1fffu + 4096u,
        R300_RB2D_CONTRACT_CONST_FILL_V2, 2u },
      /* x + width field overflow: a run wider than 0xffff pixels becomes
       * whole rows. */
      { "x+width field overflow", 0u, 4u * 70000u, 4u * 70000u + 1024u,
        R300_RB2D_CONTRACT_CONST_FILL_V1, 1u },
      /* pitch zero / width zero never reach the emitter: the request
       * names no pitch and a partial row cuts no empty rectangle. */
      { "row-aligned start, tail only", 256u, 8u, 4096u,
        R300_RB2D_CONTRACT_CONST_FILL_V1, 1u },
      { "exact whole rows", 1024u, 2048u, 8192u,
        R300_RB2D_CONTRACT_CONST_FILL_V1, 1u },
      /* the attended cell */
      { "attended cell", 12u, 4992u, 65536u,
        R300_RB2D_CONTRACT_CONST_FILL_V1, 1u },
   };
   for (size_t i = 0; i < ARRAY_LEN(rows); i++) {
      const struct row *r = &rows[i];
      struct r300_rb2d_legalize_request req =
         request(r->offset, r->size, r->bo_size, r->contract);
      req.pinned_pitch_bytes = 256u;
      struct r300_rb2d_legalize_result result;
      const uint32_t n = legalize_exactly(&req, &result);
      if (n != r->expect_windows) {
         fprintf(stderr, "%s: %u windows, expected %u\n", r->raw_reject, n,
                 r->expect_windows);
         abort();
      }
      for (uint32_t s = 0; s < n; s++) {
         for (uint32_t k = 0; k < windows[s].rect_count; k++) {
            const struct r300_rb2d_fill_rect *rect = &windows[s].rects[k];
            assert(rect->width != 0u && rect->height != 0u);
            assert(rect->x < 64u);
            assert(rect->x + rect->width <= 64u);
         }
      }
   }
}

/* V1: the attended cell's legalization emits the exact stream the plan
 * emitter emits for the span decomposition's plan. */
static void
test_v1_byte_identity(void)
{
   const struct r300_rb2d_legalize_request req =
      request(12u, 4992u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V1);
   struct r300_rb2d_legalize_result result;
   const uint32_t n = legalize_exactly(&req, &result);
   assert(n == 1u);
   assert(result.pitch_bytes == 256u);
   assert(windows[0].rect_count == 3u);
   assert(result.ib_dwords == 38u);

   uint32_t legal[64], plain[64];
   struct r300_rb2d_legalized_ib lib;
   assert(r300_rb2d_legalize_emit(windows, n, legal, 64u, &lib) == 0);
   assert(lib.ib_size_dwords == 38u);
   assert(lib.site_count == 1u);

   const struct r300_rb2d_span span = {
      .byte_offset = 12u, .byte_size = 4992u, .value = 0x11223344u };
   const struct r300_rb2d_span_layout layout = {
      .pitch_bytes = 256u, .format = R300_RB2D_FORMAT_ARGB8888 };
   struct r300_rb2d_fill_plan plans[1];
   struct r300_rb2d_fill_rect rects[3];
   enum r300_rb2d_span_refusal sr;
   assert(r300_rb2d_linear_span_plan(&span, &layout, 65536u, plans, rects,
                                     1u, &sr) == 1u);
   struct r300_rb2d_fill_ib ib;
   assert(r300_rb2d_fill_emit_into(&plans[0], plain, 64u, &ib) == 0);
   assert(ib.ib_size_dwords == 38u);
   assert(memcmp(legal, plain, 38u * sizeof(uint32_t)) == 0);
   assert(lib.site_count == ib.reloc_site_count);
   assert(lib.sites[0].ib_index == ib.reloc_sites[0].ib_index);
   assert(lib.sites[0].slot == ib.reloc_sites[0].slot);

   /* The witnessed stream's fixed words. */
   assert(legal[1] == ((4u << 22) | 0u));
   assert(legal[2] == (0xC0000000u | R300_PM4_PACKET3_NOP));
   assert(legal[3] == 0u);
}

/* V2: a multi-window request emits one stream with one site per window,
 * each window's block byte-identical to its own plan emission.  The pin
 * holds the witnessed 256-byte carrier, whose window reach of 0x1fff rows
 * is what cuts the second window. */
static void
test_v2_multi_window_emission(void)
{
   const uint64_t bo = 256u * 0x1fffu + 8192u;
   struct r300_rb2d_legalize_request req =
      request(1024u, 256u * 0x1fffu + 2048u, bo,
              R300_RB2D_CONTRACT_CONST_FILL_V2);
   req.pinned_pitch_bytes = 256u;
   struct r300_rb2d_legalize_result result;
   const uint32_t n = legalize_exactly(&req, &result);
   assert(n == 2u);
   assert(result.relocation_sites == 2u);

   uint32_t words[256];
   struct r300_rb2d_legalized_ib lib;
   assert(r300_rb2d_legalize_emit(windows, n, words, 256u, &lib) == 0);
   assert(lib.site_count == 2u);
   assert(lib.ib_size_dwords == result.ib_dwords);
   uint32_t at = 0u;
   for (uint32_t s = 0; s < n; s++) {
      struct r300_rb2d_fill_plan plan;
      struct r300_rb2d_fill_ib ib;
      uint32_t block[64];
      r300_rb2d_window_to_fill_plan(&windows[s], &plan);
      assert(r300_rb2d_fill_emit_into(&plan, block, 64u, &ib) == 0);
      assert(memcmp(words + at, block, ib.ib_size_dwords * 4u) == 0);
      assert(lib.sites[s].ib_index == at + ib.reloc_sites[0].ib_index);
      at += ib.ib_size_dwords;
   }
   assert(at == lib.ib_size_dwords);

   /* The same request under V1 refuses on the window bound. */
   struct r300_rb2d_legalize_request v1 = req;
   v1.contract = R300_RB2D_CONTRACT_CONST_FILL_V1;
   assert(r300_rb2d_legalize_linear_span(&v1, windows, ARRAY_LEN(windows),
                                         &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_CONTRACT_WINDOWS);
   /* Storage smaller than the need refuses whole. */
   assert(r300_rb2d_legalize_linear_span(&req, windows, 1u, &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_STORAGE);
}

/* The two cells the design pins.
 *
 * The multi-window cell is the V2 shape nothing has run: on the 256-byte
 * carrier a window reaches the safe scissor end at row 0x1fff, and
 * 0x1fff * 256 lands 768 bytes past a 1 KiB boundary, so the rebase carries
 * a local y of 3 and the tail is one rectangle in a second window.
 *
 * The dense cell is the widest 64-byte pitch under the scissor end times
 * four bytes, cut on a buffer half its own row, which the kernel's end_byte
 * footprint admits; it legalizes at SILICON_RECEIPT because the attended
 * run over this exact geometry receipted the carrier.  The multi-window
 * cell pins its shape at the PLANNED floor, where the carrier ranks rather
 * than carries a receipt. */
static void
test_designed_cells(void)
{
   /* window 0 covers 61 + 8190 * 64 pixels, window 1 the 32-pixel tail. */
   const uint64_t window0_bytes = (61u + 8190u * 64u) * 4u;
   const uint64_t tail_bytes = 128u;
   struct r300_rb2d_legalize_request multi =
      request(12u, window0_bytes + tail_bytes, 2u * 1024u * 1024u,
              R300_RB2D_CONTRACT_CONST_FILL_V2);
   multi.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_PLANNED;
   multi.pinned_pitch_bytes = 256u;

   struct r300_rb2d_legalize_result result;
   assert(multi.byte_size == 2097012u);
   assert(legalize_exactly(&multi, &result) == 2u);
   assert(result.window_count == 2u && result.relocation_sites == 2u);
   assert(result.pitch_bytes == 256u);

   assert(windows[0].bo_base == 0u);
   assert(windows[0].height_rows == 0x1fffu);
   assert(windows[0].rect_count == 2u);
   assert(windows[0].rects[0].x == 3u && windows[0].rects[0].y == 0u &&
          windows[0].rects[0].width == 61u &&
          windows[0].rects[0].height == 1u);
   assert(windows[0].rects[1].x == 0u && windows[0].rects[1].y == 1u &&
          windows[0].rects[1].width == 64u &&
          windows[0].rects[1].height == 8190u);
   assert(windows[1].bo_base == 2096128u);
   assert(windows[1].height_rows == 4u);
   assert(windows[1].rect_count == 1u);
   assert(windows[1].rects[0].x == 0u && windows[1].rects[0].y == 3u &&
          windows[1].rects[0].width == 32u &&
          windows[1].rects[0].height == 1u);

   /* The dense carrier: DST_PITCH_OFFSET's 8-bit pitch field reaches 255
    * units, so the widest surface is 16320 bytes and one row is 4080
    * pixels.  The interval is the first row's remainder, three whole
    * rows, and a forty-pixel tail. */
   struct r300_rb2d_legalize_request dense =
      request(12u, 65428u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V2);
   dense.pinned_pitch_bytes = 16320u;

   assert(legalize_exactly(&dense, &result) == 1u);
   assert(result.pitch_bytes == 16320u);
   assert(windows[0].height_rows == 5u);
   assert(windows[0].rect_count == 3u);
   static const struct r300_rb2d_fill_rect expect[3] = {
      { .x = 3u, .y = 0u, .width = 4077u, .height = 1u },
      { .x = 0u, .y = 1u, .width = 4080u, .height = 3u },
      { .x = 0u, .y = 4u, .width = 40u, .height = 1u },
   };
   for (uint32_t i = 0; i < 3u; i++) {
      assert(windows[0].rects[i].x == expect[i].x);
      assert(windows[0].rects[i].y == expect[i].y);
      assert(windows[0].rects[i].width == expect[i].width);
      assert(windows[0].rects[i].height == expect[i].height);
   }
}

/* Contract evidence is a second authority beside the carrier's: each
 * receipted shape is admitted at its own width, and a stream that rebases
 * once more than the run that receipted it refuses at SILICON_RECEIPT
 * however strong its carrier's evidence is.  V1's own second window is out
 * of reach of this authority, because the structural V1 window cap refuses
 * it first on REFUSE_CONTRACT_WINDOWS. */
static void
test_contract_evidence_admission(void)
{
   struct r300_rb2d_legalize_request v1 =
      request(12u, 4992u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V1);
   v1.minimum_contract_evidence =
      R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT;
   struct r300_rb2d_legalize_result result;
   assert(legalize_exactly(&v1, &result) == 1u);

   /* The attended multiwindow run receipted two windows through two
    * relocation sites on the 256-byte carrier, so its own 2 MiB interval
    * legalizes at SILICON_RECEIPT. */
   struct r300_rb2d_legalize_request v2 =
      request(12u, 2097012u, 2u * 1024u * 1024u,
              R300_RB2D_CONTRACT_CONST_FILL_V2);
   v2.minimum_contract_evidence =
      R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT;
   v2.pinned_pitch_bytes = 256u;
   assert(legalize_exactly(&v2, &result) == 2u);
   assert(result.relocation_sites == 2u);

   /* One 256-byte window carries 0x1fff rows of 64 pixels, 2096896 bytes,
    * so a span past twice that needs a third window and leaves the
    * receipt. */
   struct r300_rb2d_legalize_request v2_third = v2;
   v2_third.byte_offset = 0u;
   v2_third.byte_size = 2u * 2096896u + 4u;
   v2_third.bo_size = 4u * 1024u * 1024u;
   assert(r300_rb2d_legalize_linear_span(&v2_third, windows,
                                         ARRAY_LEN(windows), &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_CONTRACT_EVIDENCE);

   /* The same request with the contract authority unasked legalizes, which
    * is what keeps the refusal the contract table's and not the
    * carrier's. */
   v2_third.minimum_contract_evidence = R300_RB2D_CONTRACT_EVIDENCE_PLANNED;
   assert(legalize_exactly(&v2_third, &result) == 3u);

   assert(r300_rb2d_legalize_refusal_name(
             R300_RB2D_LEGALIZE_REFUSE_CONTRACT_EVIDENCE) != NULL);
}

/* Each window invariant refuses when it alone is broken. */
static void
test_window_checker(void)
{
   const struct r300_rb2d_legalize_request req =
      request(12u, 4992u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V1);
   struct r300_rb2d_legalize_result result;
   assert(legalize_exactly(&req, &result) == 1u);
   const struct r300_rb2d_window good = windows[0];
   assert(r300_rb2d_window_check(&good, 65536u) == R300_RB2D_WINDOW_OK);
   assert(r300_rb2d_window_check(NULL, 65536u) == R300_RB2D_WINDOW_REFUSE_NULL);

   struct r300_rb2d_window w;
#define MUTATE(field, value, expect)                                          \
   do {                                                                       \
      w = good;                                                               \
      w.field = (value);                                                      \
      assert(r300_rb2d_window_check(&w, 65536u) == (expect));                 \
   } while (0)
   MUTATE(format, R300_RB2D_FORMAT_COUNT, R300_RB2D_WINDOW_REFUSE_FORMAT);
   MUTATE(cpp, 2u, R300_RB2D_WINDOW_REFUSE_CPP_MISMATCH);
   MUTATE(bo_base, 512u, R300_RB2D_WINDOW_REFUSE_BASE_GRID);
   MUTATE(bo_base, (uint64_t)(R300_RB2D_MAX_OFFSET_UNITS + 1u) * 1024u,
          R300_RB2D_WINDOW_REFUSE_BASE_FIELD);
   MUTATE(pitch_bytes, 0u, R300_RB2D_WINDOW_REFUSE_PITCH_ZERO);
   MUTATE(pitch_bytes, 96u, R300_RB2D_WINDOW_REFUSE_PITCH_GRID);
   MUTATE(pitch_bytes, (R300_RB2D_MAX_PITCH_UNITS + 1u) * 64u,
          R300_RB2D_WINDOW_REFUSE_PITCH_FIELD);
   MUTATE(rect_count, 0u, R300_RB2D_WINDOW_REFUSE_NO_RECTS);
   MUTATE(rects[1].width, 0u, R300_RB2D_WINDOW_REFUSE_RECT_EMPTY);
   MUTATE(rects[0].x, 64u, R300_RB2D_WINDOW_REFUSE_X_PAST_ROW);
   MUTATE(rects[0].width, 62u, R300_RB2D_WINDOW_REFUSE_WIDTH_PAST_ROW);
   MUTATE(rects[1].height, 0x1fffu, R300_RB2D_WINDOW_REFUSE_BEYOND_SCISSOR);
   MUTATE(height_rows, good.height_rows + 1u,
          R300_RB2D_WINDOW_REFUSE_HEIGHT_MISMATCH);
#undef MUTATE
   /* Past the object: the same window against a 4 KiB object. */
   assert(r300_rb2d_window_check(&good, 4096u) ==
          R300_RB2D_WINDOW_REFUSE_PAST_OBJECT);
   /* Address width: a base near the top of the 32-bit surface whose rows
    * run past it.  The base field allows it; the footprint does not. */
   w = good;
   w.bo_base = (uint64_t)R300_RB2D_MAX_OFFSET_UNITS * 1024u;
   assert(r300_rb2d_window_check(&w, UINT64_MAX) ==
          R300_RB2D_WINDOW_REFUSE_ADDRESS_WIDTH);
   for (unsigned r = 0; r < R300_RB2D_WINDOW_REFUSAL_COUNT; r++)
      assert(r300_rb2d_window_refusal_name(r) != NULL);
   assert(r300_rb2d_window_refusal_name(R300_RB2D_WINDOW_REFUSAL_COUNT) ==
          NULL);
}

/* The coverage oracle refuses an overlap and a rectangle past the
 * buffer. */
static void
test_coverage_oracle(void)
{
   const struct r300_rb2d_legalize_request req =
      request(12u, 4992u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V1);
   struct r300_rb2d_legalize_result result;
   assert(legalize_exactly(&req, &result) == 1u);
   uint8_t *counts = calloc(65536u, 1);
   assert(counts != NULL);
   struct r300_rb2d_window two[2] = { windows[0], windows[0] };
   assert(!r300_rb2d_windows_touched_bytes(two, 2u, counts, 65536u));
   memset(counts, 0, 65536u);
   assert(!r300_rb2d_windows_touched_bytes(windows, 1u, counts, 5000u));
   free(counts);
}

/* Registry: self-consistent, execution admits the two receipted ARGB8888
 * carriers, a pinned pitch below execution evidence refuses, and the
 * lowest class admits every row. */
static void
test_pitch_registry(void)
{
   assert(r300_rb2d_pitch_evidence_self_check() == 0);
   uint32_t n = 0u;
   const struct r300_rb2d_pitch_evidence *rows =
      r300_rb2d_pitch_evidence_rows(&n);
   assert(n >= 7u);
   bool receipted_256 = false, receipted_16320 = false;
   uint32_t silicon = 0u;
   for (uint32_t i = 0; i < n; i++) {
      assert(r300_rb2d_pitch_evidence_class_name(rows[i].evidence) != NULL);
      if (rows[i].evidence == R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT) {
         silicon++;
         assert(rows[i].format == R300_RB2D_FORMAT_ARGB8888);
         receipted_256 |= rows[i].pitch_bytes == 256u;
         receipted_16320 |= rows[i].pitch_bytes == 16320u;
      }
   }
   assert(silicon == 2u && receipted_256 && receipted_16320);
   assert(r300_rb2d_pitch_evidence_find(512u, R300_RB2D_FORMAT_ARGB8888,
                                        R300_RB2D_USAGE_FILL_BUFFER) == NULL);

   struct r300_rb2d_legalize_request req =
      request(0u, 65536u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V2);
   struct r300_rb2d_legalize_result result;
   /* 8192 carries no receipt, so execution evidence refuses it and PLANNED
    * admits it. */
   req.pinned_pitch_bytes = 8192u;
   assert(r300_rb2d_legalize_linear_span(&req, windows, ARRAY_LEN(windows),
                                         &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_PINNED_PITCH_UNADMITTED);
   req.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_PLANNED;
   assert(legalize_exactly(&req, &result) == 1u);
   assert(result.pitch_bytes == 8192u);

   /* The 16320-byte carrier holds its own receipt, so execution evidence
    * admits it: 65536 bytes are four whole 4080-pixel rows and a
    * 64-pixel tail. */
   req.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT;
   req.pinned_pitch_bytes = 16320u;
   assert(legalize_exactly(&req, &result) == 1u);
   assert(result.pitch_bytes == 16320u);
   assert(windows[0].rect_count == 2u);

   /* V1 refuses a pinned pitch other than its own. */
   req.contract = R300_RB2D_CONTRACT_CONST_FILL_V1;
   assert(r300_rb2d_legalize_linear_span(&req, windows, ARRAY_LEN(windows),
                                         &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_CONTRACT);
}

/* The chooser: under execution evidence it ranks the two receipted
 * ARGB8888 carriers; under PLANNED a wide dense carrier wins a large
 * interval on the cost model, and the RGB565 row is never chosen. */
static void
test_chooser_and_cost(void)
{
   /* Eight MiB: on the witnessed 256-byte carrier that is four windows
    * of 0x1fff rows, so a dense carrier that holds it in one window wins
    * the cost model, and among the carriers that do, exact division into
    * whole rows gives one rectangle and the smallest such pitch wins the
    * tie.  One MiB fits one 256-byte window in one rectangle, so there the
    * witnessed pitch is already the cheapest. */
   struct r300_rb2d_legalize_request req =
      request(0u, 8u << 20, 8u << 20, R300_RB2D_CONTRACT_CONST_FILL_V2);
   enum r300_rb2d_format format = R300_RB2D_FORMAT_COUNT;
   assert(r300_rb2d_choose_pitch(&req, &r300_rb2d_default_cost_weights,
                                 &format) == 16320u);
   assert(format == R300_RB2D_FORMAT_ARGB8888);
   struct r300_rb2d_legalize_result result;
   /* One window reaches 0x1fff rows of 256 bytes, just under 2 MiB, so
    * eight MiB pinned to the witnessed carrier takes five. */
   req.pinned_pitch_bytes = 256u;
   assert(legalize_exactly(&req, &result) == 5u);
   req.pinned_pitch_bytes = 0u;

   req.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_PLANNED;
   const uint32_t dense = r300_rb2d_choose_pitch(
      &req, &r300_rb2d_default_cost_weights, &format);
   assert(dense == 4096u);
   assert(format == R300_RB2D_FORMAT_ARGB8888);
   assert(legalize_exactly(&req, &result) == 1u);
   assert(result.pitch_bytes == 4096u);
   assert(result.rect_count == 1u);

   struct r300_rb2d_legalize_request one_mib =
      request(0u, 1u << 20, 1u << 20, R300_RB2D_CONTRACT_CONST_FILL_V2);
   one_mib.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_PLANNED;
   assert(r300_rb2d_choose_pitch(&one_mib, &r300_rb2d_default_cost_weights,
                                 &format) == 256u);
   req.pinned_pitch_bytes = 16320u;
   assert(legalize_exactly(&req, &result) == 1u);
   assert(result.rect_count == 2u);
   const uint64_t widest_cost =
      r300_rb2d_legalize_cost(&result, &r300_rb2d_default_cost_weights);
   req.pinned_pitch_bytes = 0u;
   assert(legalize_exactly(&req, &result) == 1u);
   const uint64_t dense_cost =
      r300_rb2d_legalize_cost(&result, &r300_rb2d_default_cost_weights);
   assert(dense_cost < widest_cost);
   req.pinned_pitch_bytes = 256u;
   assert(legalize_exactly(&req, &result) == 5u);
   assert(r300_rb2d_legalize_cost(&result, &r300_rb2d_default_cost_weights) >
          widest_cost);
   req.pinned_pitch_bytes = 0u;

   /* Where the dense carrier starts paying, computed rather than
    * asserted from a guess, so a receipt cell for the chooser can be
    * sized from a number this test owns.  Two crossings, and they are far
    * apart because the chooser ranks every admitted row rather than a
    * pair.
    *
    * Head to head against the witnessed 256-byte carrier, one 256-byte
    * window reaches 0x1fff rows of 64 pixels, 2096896 bytes, so 2096900
    * is the smallest dword-aligned offset-zero size needing a second
    * window, and 16320 wins at the tested 2 MiB because that rebind costs
    * more than the wider carrier's extra rectangle.
    *
    * As the chooser's own verdict, 16320 wins only from 34467840 bytes,
    * where 8616960 pixels divide exactly into 2112 rows of 4080 and the
    * carrier reaches one rectangle while 4096 and 8192 still need a tail.
    */
   {
      const struct {
         uint64_t size;
         uint32_t pinned;
      } crossing[] = { { 2u << 20, 256u }, { 34467840u, 0u } };
      /* The exact second-window boundary on the 256-byte carrier: the
       * last size one window carries and the first that needs two. */
      {
         struct r300_rb2d_legalize_request edge =
            request(0u, 2096896u, 2096900u, R300_RB2D_CONTRACT_CONST_FILL_V2);
         edge.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_PLANNED;
         edge.pinned_pitch_bytes = 256u;
         struct r300_rb2d_legalize_result r;
         assert(legalize_exactly(&edge, &r) == 1u);
         edge.byte_size = 2096900u;
         assert(r300_rb2d_legalize_linear_span(&edge, windows,
                                               ARRAY_LEN(windows), &r) == 2u);
      }
      for (unsigned k = 0; k < ARRAY_LEN(crossing); k++) {
         struct r300_rb2d_legalize_request at =
            request(0u, crossing[k].size, crossing[k].size,
                    R300_RB2D_CONTRACT_CONST_FILL_V2);
         at.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_PLANNED;
         struct r300_rb2d_legalize_request under = at;
         under.byte_size = crossing[k].size - 4096u;
         under.bo_size = under.byte_size;

         if (crossing[k].pinned != 0u) {
            /* The pair: the dense carrier costs less at the crossing and
             * no less one page below it. */
            struct r300_rb2d_legalize_result wide, witnessed;
            at.pinned_pitch_bytes = 16320u;
            assert(legalize_exactly(&at, &wide) == 1u);
            at.pinned_pitch_bytes = crossing[k].pinned;
            assert(r300_rb2d_legalize_linear_span(&at, windows,
                                                  ARRAY_LEN(windows),
                                                  &witnessed) == 2u);
            assert(r300_rb2d_legalize_cost(&wide,
                                           &r300_rb2d_default_cost_weights) <
                   r300_rb2d_legalize_cost(
                      &witnessed, &r300_rb2d_default_cost_weights));
            under.pinned_pitch_bytes = 16320u;
            assert(legalize_exactly(&under, &wide) == 1u);
            under.pinned_pitch_bytes = crossing[k].pinned;
            assert(legalize_exactly(&under, &witnessed) == 1u);
            assert(r300_rb2d_legalize_cost(&wide,
                                           &r300_rb2d_default_cost_weights) >=
                   r300_rb2d_legalize_cost(
                      &witnessed, &r300_rb2d_default_cost_weights));
         } else {
            /* The chooser's verdict over the whole registry. */
            enum r300_rb2d_format chosen = R300_RB2D_FORMAT_COUNT;
            assert(r300_rb2d_choose_pitch(&at,
                                          &r300_rb2d_default_cost_weights,
                                          &chosen) == 16320u);
            assert(chosen == R300_RB2D_FORMAT_ARGB8888);
            assert(r300_rb2d_choose_pitch(&under,
                                          &r300_rb2d_default_cost_weights,
                                          &chosen) != 16320u);
         }
      }
   }

   /* Cost of a refusal is unbounded. */
   struct r300_rb2d_legalize_result bad = { .refusal =
                                               R300_RB2D_LEGALIZE_REFUSE_SPAN };
   assert(r300_rb2d_legalize_cost(&bad, &r300_rb2d_default_cost_weights) ==
          UINT64_MAX);

   /* RGB565: admitted at KERNEL_REPLAY for an equal-halves pattern on the
    * 256-byte carrier, with the brush carrying the low half; refused for
    * a pattern whose halves differ; withheld from execution.  The chooser
    * ranks it behind the receipted ARGB8888 carriers, whose widest row
    * holds the whole 4992-byte interval in one rectangle. */
   struct r300_rb2d_legalize_request half =
      request(12u, 4992u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V2);
   half.pattern = 0xabcdabcdu;
   half.minimum_evidence = R300_RB2D_PITCH_EVIDENCE_KERNEL_REPLAY;
   half.pinned_pitch_bytes = 256u;
   /* The pinned path keeps ARGB8888; the registry row for RGB565 is
    * reached by the chooser, so pin nothing and check the chooser ranks
    * ARGB8888 first on a tie-free cost. */
   half.pinned_pitch_bytes = 0u;
   assert(r300_rb2d_choose_pitch(&half, &r300_rb2d_default_cost_weights,
                                 &format) == 16320u);
   assert(format == R300_RB2D_FORMAT_ARGB8888);

   const struct r300_rb2d_span span565 = {
      .byte_offset = 12u, .byte_size = 4992u, .value = 0xabcdabcdu };
   const struct r300_rb2d_span_layout l565 = {
      .pitch_bytes = 256u, .format = R300_RB2D_FORMAT_RGB565 };
   enum r300_rb2d_span_refusal sr;
   assert(r300_rb2d_linear_span_segments(&span565, &l565, 65536u, &sr) == 1u);
   struct r300_rb2d_fill_plan plans[1];
   struct r300_rb2d_fill_rect rects[3];
   assert(r300_rb2d_linear_span_plan(&span565, &l565, 65536u, plans, rects,
                                     1u, &sr) == 1u);
   assert(plans[0].surface.width_pixels == 128u);
   assert(plans[0].rects[0].x == 6u);
   assert(plans[0].rects[0].value == 0xabcdu);
   uint64_t pixels = 0u;
   for (uint32_t i = 0; i < plans[0].rect_count; i++)
      pixels += (uint64_t)plans[0].rects[i].width * plans[0].rects[i].height;
   assert(pixels == 4992u / 2u);
   const struct r300_rb2d_span mixed = {
      .byte_offset = 12u, .byte_size = 4992u, .value = 0x11223344u };
   assert(r300_rb2d_linear_span_segments(&mixed, &l565, 65536u, &sr) == 0u);
   assert(sr == R300_RB2D_SPAN_REFUSE_PATTERN_WIDTH);
   assert(!r300_rb2d_pitch_admitted(256u, R300_RB2D_FORMAT_RGB565,
                                    R300_RB2D_USAGE_FILL_BUFFER,
                                    R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT));
}

/* Two receipted ARGB8888 carriers stand in the registry, so execution
 * evidence hands the chooser a pair rather than a single row.  The V1
 * contract admits its own 256-byte carrier alone, and under V2 the pair
 * crosses where the witnessed carrier needs a second window: one 256-byte
 * window reaches 0x1fff rows of 64 pixels, 2096896 bytes, and the rebind
 * past it costs more than the wider carrier's extra rectangle. */
static void
test_two_receipted_carriers(void)
{
   enum r300_rb2d_format format = R300_RB2D_FORMAT_COUNT;
   struct r300_rb2d_legalize_request v1 =
      request(12u, 4992u, 65536u, R300_RB2D_CONTRACT_CONST_FILL_V1);
   assert(r300_rb2d_choose_pitch(&v1, &r300_rb2d_default_cost_weights,
                                 &format) == 256u);
   assert(format == R300_RB2D_FORMAT_ARGB8888);

   struct r300_rb2d_legalize_request below =
      request(0u, 2093056u, 2093056u, R300_RB2D_CONTRACT_CONST_FILL_V2);
   assert(r300_rb2d_choose_pitch(&below, &r300_rb2d_default_cost_weights,
                                 &format) == 256u);
   struct r300_rb2d_legalize_request above =
      request(0u, 2097152u, 2097152u, R300_RB2D_CONTRACT_CONST_FILL_V2);
   assert(r300_rb2d_choose_pitch(&above, &r300_rb2d_default_cost_weights,
                                 &format) == 16320u);
   assert(format == R300_RB2D_FORMAT_ARGB8888);
}

/* The chooser cell's geometry, where the verdict is the fact under test.
 * At the executing floor the registry admits 256 and 16320, and 2 MiB is
 * the smallest allocation on the 4 KiB grid where the wider carrier's one
 * window costs less than the narrower carrier's two, so the chooser
 * returns 16320 there and 256 one 4 KiB step below.  Pinning 256 over the
 * same interval still legalizes, into the two windows the pinned cell
 * declares, so the cell discriminates the chooser from the pin. */
static void
test_chooser_selected_cell(void)
{
   enum r300_rb2d_format format = R300_RB2D_FORMAT_COUNT;
   struct r300_rb2d_legalize_request chooser =
      request(12u, 2097012u, 2097152u, R300_RB2D_CONTRACT_CONST_FILL_V2);
   assert(r300_rb2d_choose_pitch(&chooser, &r300_rb2d_default_cost_weights,
                                 &format) == 16320u);
   assert(format == R300_RB2D_FORMAT_ARGB8888);

   struct r300_rb2d_legalize_result result;
   assert(legalize_exactly(&chooser, &result) == 1u);
   assert(result.pitch_bytes == 16320u);
   assert(result.window_count == 1u && result.relocation_sites == 1u);
   assert(result.rect_count == 3u && result.ib_dwords == 38u);
   assert(windows[0].bo_base == 0u && windows[0].height_rows == 129u);
   assert(windows[0].rects[0].x == 3u && windows[0].rects[0].y == 0u &&
          windows[0].rects[0].width == 4077u &&
          windows[0].rects[0].height == 1u);
   assert(windows[0].rects[1].x == 0u && windows[0].rects[1].y == 1u &&
          windows[0].rects[1].width == 4080u &&
          windows[0].rects[1].height == 127u);
   assert(windows[0].rects[2].x == 0u && windows[0].rects[2].y == 128u &&
          windows[0].rects[2].width == 2016u &&
          windows[0].rects[2].height == 1u);

   /* The same geometry with 16320 withheld by a pin at 256: the interval
    * still legalizes, and it costs the two windows and two relocation
    * sites the chooser declined. */
   struct r300_rb2d_legalize_request pinned = chooser;
   pinned.pinned_pitch_bytes = 256u;
   assert(legalize_exactly(&pinned, &result) == 2u);
   assert(result.pitch_bytes == 256u);
   assert(result.window_count == 2u && result.relocation_sites == 2u);

   /* One 4 KiB step below the cell, at the cell's own offset, the
    * narrower carrier wins, so the cell sits at the crossover. */
   struct r300_rb2d_legalize_request below =
      request(12u, 2092916u, 2093056u, R300_RB2D_CONTRACT_CONST_FILL_V2);
   assert(r300_rb2d_choose_pitch(&below, &r300_rb2d_default_cost_weights,
                                 &format) == 256u);
}

/* Refusals at the request boundary. */
static void
test_request_refusals(void)
{
   struct r300_rb2d_legalize_result result;
   assert(r300_rb2d_legalize_linear_span(NULL, windows, 1u, &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_NULL);
   struct r300_rb2d_legalize_request req =
      request(0u, 0u, 4096u, R300_RB2D_CONTRACT_CONST_FILL_V1);
   assert(r300_rb2d_legalize_linear_span(&req, windows, 1u, &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_SIZE_ZERO);
   req.byte_size = 4u;
   req.usage = R300_RB2D_USAGE_COUNT;
   assert(r300_rb2d_legalize_linear_span(&req, windows, 1u, &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_USAGE);
   req.usage = R300_RB2D_USAGE_FILL_BUFFER;
   req.contract = R300_RB2D_CONTRACT_COUNT;
   assert(r300_rb2d_legalize_linear_span(&req, windows, 1u, &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_CONTRACT);
   req.contract = R300_RB2D_CONTRACT_CONST_FILL_V1;
   req.byte_offset = 2u;
   assert(r300_rb2d_legalize_linear_span(&req, windows, 1u, &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_SPAN);
   assert(result.span_refusal == R300_RB2D_SPAN_REFUSE_OFFSET_ALIGNMENT);
   req.byte_offset = 0u;
   req.byte_size = 8192u;
   assert(r300_rb2d_legalize_linear_span(&req, windows, 1u, &result) == 0u);
   assert(result.refusal == R300_RB2D_LEGALIZE_REFUSE_SPAN);
   assert(result.span_refusal == R300_RB2D_SPAN_REFUSE_OUTSIDE_BUFFER);
   for (unsigned r = 0; r < R300_RB2D_LEGALIZE_REFUSAL_COUNT; r++)
      assert(r300_rb2d_legalize_refusal_name(r) != NULL);
}

/* The emitter's state machine: a launch before its destination or format
 * records -EINVAL, and a surface rebind clears the format epoch. */
static void
test_emitter_epochs(void)
{
   uint32_t words[64];
   struct r300_rb2d_fill_ib out;
   struct r300_rb2d_emitter e;
   const struct r300_rb2d_surface s = {
      .base_offset_bytes = 0u, .pitch_bytes = 256u, .width_pixels = 64u,
      .height_pixels = 4u, .format = R300_RB2D_FORMAT_ARGB8888 };
   const struct r300_rb2d_fill_rect r = { 0u, 0u, 1u, 1u, 0u };
   const struct r300_rb2d_relocation dst = { .slot = R300_RB2D_FILL_SLOT_DST };

   r300_rb2d_emitter_init(&e, words, 64u, &out);
   r300_rb2d_emit_rect(&e, &r);
   assert(r300_rb2d_emitter_finish(&e) == -EINVAL);

   r300_rb2d_emitter_init(&e, words, 64u, &out);
   r300_rb2d_emit_common_state(&e, 0xffffffffu);
   assert(r300_rb2d_emitter_finish(&e) == -EINVAL);

   r300_rb2d_emitter_init(&e, words, 64u, &out);
   r300_rb2d_emit_surface_state(&e, &s, dst);
   r300_rb2d_emit_rect(&e, &r);
   assert(r300_rb2d_emitter_finish(&e) == -EINVAL);

   /* A second surface in one stream needs a second site and the plan
    * vocabulary carries one, so the rebind refuses at the site bound. */
   r300_rb2d_emitter_init(&e, words, 64u, &out);
   r300_rb2d_emit_surface_state(&e, &s, dst);
   r300_rb2d_emit_common_state(&e, 0xffffffffu);
   assert(e.dst_epoch == 1u && e.format_epoch == 1u);
   r300_rb2d_emit_surface_state(&e, &s, dst);
   assert(r300_rb2d_emitter_finish(&e) == -EINVAL);

   r300_rb2d_emitter_init(&e, words, 64u, &out);
   r300_rb2d_emit_surface_state(&e, &s, dst);
   r300_rb2d_emit_common_state(&e, 0xffffffffu);
   r300_rb2d_emit_rect(&e, &r);
   r300_rb2d_emit_epilogue(&e);
   assert(e.origin_epoch == 1u);
   assert(r300_rb2d_emitter_finish(&e) == 0);
   assert(out.ib_size_dwords == R300_RB2D_FILL_DWORDS(1u));
   assert(r300_rb2d_fill_validate_reloc_sites(&out) == 0);

   /* A slot outside the vocabulary has no emission. */
   r300_rb2d_emitter_init(&e, words, 64u, &out);
   r300_rb2d_emit_surface_state(&e, &s,
                                (struct r300_rb2d_relocation){ .slot = 7u });
   assert(r300_rb2d_emitter_finish(&e) == -EINVAL);
}

int
main(void)
{
   test_transformation_table();
   test_v1_byte_identity();
   test_v2_multi_window_emission();
   test_designed_cells();
   test_contract_evidence_admission();
   test_window_checker();
   test_coverage_oracle();
   test_pitch_registry();
   test_chooser_and_cost();
   test_two_receipted_carriers();
   test_chooser_selected_cell();
   test_request_refusals();
   test_emitter_epochs();
   printf("r300_rb2d_legalize_test: all checks passed\n");
   return 0;
}
