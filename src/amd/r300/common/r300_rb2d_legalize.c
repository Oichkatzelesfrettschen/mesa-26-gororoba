/* SPDX-License-Identifier: MIT */

#include "r300_rb2d_legalize.h"

#include <errno.h>
#include <string.h>

const struct r300_rb2d_cost_weights r300_rb2d_default_cost_weights = {
   .per_window = 64u,
   .per_relocation_site = 16u,
   .per_rect = 8u,
   .per_ib_dword = 1u,
};

const char *
r300_rb2d_window_refusal_name(enum r300_rb2d_window_refusal r)
{
   static const char *const names[R300_RB2D_WINDOW_REFUSAL_COUNT] = {
      "ok",
      "window pointer is null",
      "window format is outside the carrier table",
      "window cpp disagrees with its format",
      "window base is off the 1 KiB grid",
      "window base is outside the offset field",
      "window pitch is zero",
      "window pitch is off the 64-byte grid",
      "window pitch is outside the pitch field",
      "window carries no rectangle",
      "window rectangle is empty",
      "window rectangle starts past the row",
      "window rectangle width overruns the row",
      "window rectangle reaches past the safe scissor end",
      "window height does not cover its rectangles",
      "window footprint leaves the 32-bit surface address",
      "window footprint reaches past the object",
      "window rejected by the fill plan checker",
   };
   return (unsigned)r < R300_RB2D_WINDOW_REFUSAL_COUNT ? names[r] : NULL;
}

const char *
r300_rb2d_legalize_refusal_name(enum r300_rb2d_legalize_refusal r)
{
   static const char *const names[R300_RB2D_LEGALIZE_REFUSAL_COUNT] = {
      "ok",
      "request pointer is null",
      "request usage is outside the table",
      "request contract is outside the table",
      "request size is zero",
      "no carrier reaches the requested evidence class",
      "pinned pitch is not admitted at the requested evidence class",
      "span decomposition refused",
      "window storage is too small",
      "contract admits fewer windows",
      "contract admits fewer rectangles per window",
      "a window failed its invariant check",
      "window coverage differs from the request interval",
      "the contract's evidence does not reach this stream shape",
   };
   return (unsigned)r < R300_RB2D_LEGALIZE_REFUSAL_COUNT ? names[r] : NULL;
}

enum r300_rb2d_window_refusal
r300_rb2d_window_check(const struct r300_rb2d_window *w, uint64_t bo_size)
{
   if (w == NULL)
      return R300_RB2D_WINDOW_REFUSE_NULL;
   const uint32_t cpp = r300_rb2d_format_bytes_per_pixel(w->format);
   if (cpp == 0u)
      return R300_RB2D_WINDOW_REFUSE_FORMAT;
   if (w->cpp != cpp)
      return R300_RB2D_WINDOW_REFUSE_CPP_MISMATCH;
   if (w->bo_base % R300_RB2D_OFFSET_GRANULARITY != 0u)
      return R300_RB2D_WINDOW_REFUSE_BASE_GRID;
   if (w->bo_base / R300_RB2D_OFFSET_GRANULARITY > R300_RB2D_MAX_OFFSET_UNITS)
      return R300_RB2D_WINDOW_REFUSE_BASE_FIELD;
   if (w->pitch_bytes == 0u)
      return R300_RB2D_WINDOW_REFUSE_PITCH_ZERO;
   if (w->pitch_bytes % R300_RB2D_PITCH_GRANULARITY != 0u)
      return R300_RB2D_WINDOW_REFUSE_PITCH_GRID;
   if (w->pitch_bytes / R300_RB2D_PITCH_GRANULARITY > R300_RB2D_MAX_PITCH_UNITS)
      return R300_RB2D_WINDOW_REFUSE_PITCH_FIELD;
   if (w->rect_count == 0u ||
       w->rect_count > R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT)
      return R300_RB2D_WINDOW_REFUSE_NO_RECTS;

   const uint32_t row_pixels = w->pitch_bytes / cpp;
   uint32_t rows_needed = 0u;
   uint64_t last_byte_end = 0u;

   for (uint32_t i = 0; i < w->rect_count; i++) {
      const struct r300_rb2d_fill_rect *r = &w->rects[i];
      if (r->width == 0u || r->height == 0u)
         return R300_RB2D_WINDOW_REFUSE_RECT_EMPTY;
      if (r->x >= row_pixels)
         return R300_RB2D_WINDOW_REFUSE_X_PAST_ROW;
      /* Both sums stay under 2^33 in 64-bit arithmetic. */
      if ((uint64_t)r->x + r->width > row_pixels)
         return R300_RB2D_WINDOW_REFUSE_WIDTH_PAST_ROW;
      if ((uint64_t)r->x + r->width > R300_RB2D_SAFE_EXCLUSIVE_END ||
          (uint64_t)r->y + r->height > R300_RB2D_SAFE_EXCLUSIVE_END)
         return R300_RB2D_WINDOW_REFUSE_BEYOND_SCISSOR;
      if (r->y + r->height > rows_needed)
         rows_needed = r->y + r->height;
      /* The kernel's own end_byte: offset + (y + height - 1) * pitch +
       * (x + width) * cpp, formed here in 64 bits and then held to the
       * 32-bit surface the engine addresses. */
      const uint64_t end = w->bo_base +
                           (uint64_t)(r->y + r->height - 1u) * w->pitch_bytes +
                           ((uint64_t)r->x + r->width) * cpp;
      if (end > last_byte_end)
         last_byte_end = end;
   }
   if (w->height_rows != rows_needed)
      return R300_RB2D_WINDOW_REFUSE_HEIGHT_MISMATCH;
   if (last_byte_end > R300_RB2D_ADDRESS_SPACE_BYTES)
      return R300_RB2D_WINDOW_REFUSE_ADDRESS_WIDTH;
   if (last_byte_end > bo_size)
      return R300_RB2D_WINDOW_REFUSE_PAST_OBJECT;

   struct r300_rb2d_fill_plan plan;
   r300_rb2d_window_to_fill_plan(w, &plan);
   if (r300_rb2d_fill_plan_check(&plan) != R300_RB2D_FILL_OK)
      return R300_RB2D_WINDOW_REFUSE_PLAN_REJECTED;
   return R300_RB2D_WINDOW_OK;
}

void
r300_rb2d_window_to_fill_plan(const struct r300_rb2d_window *w,
                              struct r300_rb2d_fill_plan *plan)
{
   memset(plan, 0, sizeof(*plan));
   plan->surface = (struct r300_rb2d_surface){
      .base_offset_bytes = (uint32_t)w->bo_base,
      .pitch_bytes = w->pitch_bytes,
      .width_pixels = w->cpp != 0u ? w->pitch_bytes / w->cpp : 0u,
      .height_pixels = w->height_rows,
      .format = w->format,
   };
   plan->write_mask = 0xffffffffu;
   plan->rects = w->rects;
   plan->rect_count = w->rect_count;
}

bool
r300_rb2d_windows_touched_bytes(const struct r300_rb2d_window *windows,
                                uint32_t window_count, uint8_t *counts,
                                uint64_t bo_size)
{
   if (windows == NULL || counts == NULL)
      return false;
   for (uint32_t s = 0; s < window_count; s++) {
      const struct r300_rb2d_window *w = &windows[s];
      for (uint32_t i = 0; i < w->rect_count; i++) {
         const struct r300_rb2d_fill_rect *r = &w->rects[i];
         for (uint32_t row = 0; row < r->height; row++) {
            const uint64_t start = w->bo_base +
                                   (uint64_t)(r->y + row) * w->pitch_bytes +
                                   (uint64_t)r->x * w->cpp;
            const uint64_t len = (uint64_t)r->width * w->cpp;
            if (start > bo_size || len > bo_size - start)
               return false;
            for (uint64_t b = 0; b < len; b++) {
               if (counts[start + b] != 0u)
                  return false;
               counts[start + b] = 1u;
            }
         }
      }
   }
   return true;
}

/* Runs the span decomposition on one carrier and converts its segments
 * into windows, checking each and the coverage of all. */
static uint32_t
legalize_on_carrier(const struct r300_rb2d_legalize_request *req,
                    uint32_t pitch_bytes, enum r300_rb2d_format format,
                    struct r300_rb2d_window *windows, uint32_t max_windows,
                    struct r300_rb2d_legalize_result *result)
{
   const struct r300_rb2d_span span = {
      .byte_offset = req->byte_offset,
      .byte_size = req->byte_size,
      .value = req->pattern,
   };
   const struct r300_rb2d_span_layout layout = {
      .pitch_bytes = pitch_bytes,
      .format = format,
   };
   result->pitch_bytes = pitch_bytes;
   result->format = format;

   const uint32_t needed = r300_rb2d_linear_span_segments(
      &span, &layout, req->bo_size, &result->span_refusal);
   if (needed == 0u) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_SPAN;
      return 0;
   }
   if (needed > max_windows || needed > R300_RB2D_LEGALIZE_MAX_WINDOWS) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_STORAGE;
      return 0;
   }
   if (req->contract == R300_RB2D_CONTRACT_CONST_FILL_V1 &&
       needed > R300_RB2D_CONTRACT_V1_MAX_WINDOWS) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_CONTRACT_WINDOWS;
      return 0;
   }

   struct r300_rb2d_fill_plan plans[R300_RB2D_LEGALIZE_MAX_WINDOWS];
   struct r300_rb2d_fill_rect rects[R300_RB2D_LEGALIZE_MAX_WINDOWS *
                                    R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
   const uint32_t n = r300_rb2d_linear_span_plan(
      &span, &layout, req->bo_size, plans, rects, needed,
      &result->span_refusal);
   if (n != needed) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_SPAN;
      return 0;
   }

   const uint32_t cpp = r300_rb2d_format_bytes_per_pixel(format);
   uint32_t rect_total = 0u;
   for (uint32_t s = 0; s < n; s++) {
      struct r300_rb2d_window *w = &windows[s];
      memset(w, 0, sizeof(*w));
      w->bo_base = plans[s].surface.base_offset_bytes;
      w->pitch_bytes = pitch_bytes;
      w->cpp = cpp;
      w->format = format;
      w->height_rows = plans[s].surface.height_pixels;
      w->rect_count = plans[s].rect_count;
      if (w->rect_count > R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT ||
          (req->contract == R300_RB2D_CONTRACT_CONST_FILL_V1 &&
           w->rect_count > R300_RB2D_CONTRACT_V1_MAX_RECTS)) {
         result->refusal = R300_RB2D_LEGALIZE_REFUSE_CONTRACT_RECTS;
         return 0;
      }
      memcpy(w->rects, plans[s].rects, w->rect_count * sizeof(w->rects[0]));
      w->relocation_site = s;
      rect_total += w->rect_count;

      result->window_refusal = r300_rb2d_window_check(w, req->bo_size);
      if (result->window_refusal != R300_RB2D_WINDOW_OK) {
         result->refusal = R300_RB2D_LEGALIZE_REFUSE_WINDOW;
         return 0;
      }
   }

   /* Coverage: the union of the rectangles' bytes equals the request
    * interval exactly.  Every byte in it is counted once, every byte
    * outside it is not, so a gap, an overlap, or a duplicated row all
    * refuse.  The interval is under 4 GiB and the count is over the
    * touched span alone, offset to the request base. */
   {
      const uint64_t begin = req->byte_offset;
      const uint64_t size = req->byte_size;
      for (uint32_t s = 0; s < n; s++) {
         const struct r300_rb2d_window *w = &windows[s];
         for (uint32_t i = 0; i < w->rect_count; i++) {
            const struct r300_rb2d_fill_rect *r = &w->rects[i];
            for (uint32_t row = 0; row < r->height; row++) {
               const uint64_t start =
                  w->bo_base + (uint64_t)(r->y + row) * w->pitch_bytes +
                  (uint64_t)r->x * cpp;
               const uint64_t len = (uint64_t)r->width * cpp;
               if (start < begin || start - begin > size ||
                   len > size - (start - begin)) {
                  result->refusal = R300_RB2D_LEGALIZE_REFUSE_COVERAGE;
                  return 0;
               }
            }
         }
      }
      /* Every rectangle row is inside the interval; the areas sum to the
       * interval only when they are disjoint and leave no gap, which the
       * decomposition's in-order cut guarantees and this arithmetic
       * confirms without a per-byte map. */
      uint64_t area = 0u;
      for (uint32_t s = 0; s < n; s++) {
         const struct r300_rb2d_window *w = &windows[s];
         for (uint32_t i = 0; i < w->rect_count; i++)
            area += (uint64_t)w->rects[i].width * w->rects[i].height * cpp;
      }
      uint64_t cursor = begin;
      for (uint32_t s = 0; s < n; s++) {
         const struct r300_rb2d_window *w = &windows[s];
         for (uint32_t i = 0; i < w->rect_count; i++) {
            const struct r300_rb2d_fill_rect *r = &w->rects[i];
            for (uint32_t row = 0; row < r->height; row++) {
               const uint64_t start =
                  w->bo_base + (uint64_t)(r->y + row) * w->pitch_bytes +
                  (uint64_t)r->x * cpp;
               if (start != cursor) {
                  result->refusal = R300_RB2D_LEGALIZE_REFUSE_COVERAGE;
                  return 0;
               }
               cursor += (uint64_t)r->width * cpp;
            }
         }
      }
      if (area != size || cursor != begin + size) {
         result->refusal = R300_RB2D_LEGALIZE_REFUSE_COVERAGE;
         return 0;
      }
   }

   result->window_count = n;
   result->rect_count = rect_total;
   result->relocation_sites = n;
   if (!r300_rb2d_legalize_dwords(windows, n, &result->ib_dwords)) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_STORAGE;
      return 0;
   }
   result->refusal = R300_RB2D_LEGALIZE_OK;
   return n;
}

uint32_t
r300_rb2d_legalize_linear_span(const struct r300_rb2d_legalize_request *req,
                               struct r300_rb2d_window *windows,
                               uint32_t max_windows,
                               struct r300_rb2d_legalize_result *result)
{
   struct r300_rb2d_legalize_result scratch;
   if (result == NULL)
      result = &scratch;
   memset(result, 0, sizeof(*result));

   if (req == NULL || windows == NULL || max_windows == 0u) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_NULL;
      return 0;
   }
   if ((unsigned)req->usage >= R300_RB2D_USAGE_COUNT) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_USAGE;
      return 0;
   }
   if ((unsigned)req->contract >= R300_RB2D_CONTRACT_COUNT) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_CONTRACT;
      return 0;
   }
   /* An empty operation is eliminated, never lowered to a zero-pitch or
    * zero-width surface. */
   if (req->byte_size == 0u) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_SIZE_ZERO;
      return 0;
   }

   uint32_t pitch;
   enum r300_rb2d_format format = R300_RB2D_FORMAT_ARGB8888;
   if (req->contract == R300_RB2D_CONTRACT_CONST_FILL_V1) {
      /* V1 is the qualified carrier and nothing else; a pinned pitch that
       * is not it is a contract violation, not a choice. */
      pitch = R300_RB2D_CONTRACT_V1_PITCH_BYTES;
      if (req->pinned_pitch_bytes != 0u && req->pinned_pitch_bytes != pitch) {
         result->refusal = R300_RB2D_LEGALIZE_REFUSE_CONTRACT;
         return 0;
      }
      if (!r300_rb2d_pitch_admitted(pitch, format, req->usage,
                                    req->minimum_evidence)) {
         result->refusal = R300_RB2D_LEGALIZE_REFUSE_NO_ADMITTED_CARRIER;
         return 0;
      }
   } else if (req->pinned_pitch_bytes != 0u) {
      pitch = req->pinned_pitch_bytes;
      if (!r300_rb2d_pitch_admitted(pitch, format, req->usage,
                                    req->minimum_evidence)) {
         result->refusal = R300_RB2D_LEGALIZE_REFUSE_PINNED_PITCH_UNADMITTED;
         return 0;
      }
   } else {
      pitch = r300_rb2d_choose_pitch(req, &r300_rb2d_default_cost_weights,
                                     &format);
      if (pitch == 0u) {
         result->refusal = R300_RB2D_LEGALIZE_REFUSE_NO_ADMITTED_CARRIER;
         return 0;
      }
   }
   const uint32_t n =
      legalize_on_carrier(req, pitch, format, windows, max_windows, result);
   if (n == 0u)
      return 0;

   /* The contract's own evidence, beside the carrier's, and read on the
    * chosen carrier rather than inside the chooser: the widest stream that
    * ran under this contract bounds the widest stream it admits, so a
    * legalization that rebases more windows than the receipt covers refuses
    * even where the carrier holds a receipt.  The chooser ranks shapes at
    * the carrier's evidence alone, which is what keeps this refusal the
    * contract table's and not a missing candidate. */
   if (req->minimum_contract_evidence >
          R300_RB2D_CONTRACT_EVIDENCE_PLANNED &&
       !r300_rb2d_contract_admitted(req->contract, result->window_count,
                                    result->relocation_sites,
                                    req->minimum_contract_evidence)) {
      result->refusal = R300_RB2D_LEGALIZE_REFUSE_CONTRACT_EVIDENCE;
      return 0;
   }
   return n;
}

uint64_t
r300_rb2d_legalize_cost(const struct r300_rb2d_legalize_result *r,
                        const struct r300_rb2d_cost_weights *w)
{
   if (r == NULL || w == NULL || r->refusal != R300_RB2D_LEGALIZE_OK)
      return UINT64_MAX;
   return (uint64_t)w->per_window * r->window_count +
          (uint64_t)w->per_relocation_site * r->relocation_sites +
          (uint64_t)w->per_rect * r->rect_count +
          (uint64_t)w->per_ib_dword * r->ib_dwords;
}

uint32_t
r300_rb2d_choose_pitch(const struct r300_rb2d_legalize_request *req,
                       const struct r300_rb2d_cost_weights *weights,
                       enum r300_rb2d_format *format_out)
{
   if (req == NULL || weights == NULL)
      return 0u;
   uint32_t rows = 0u;
   const struct r300_rb2d_pitch_evidence *table =
      r300_rb2d_pitch_evidence_rows(&rows);
   /* The candidates are legalized into this call's own storage; the
    * chooser's verdict is a pitch, and the caller legalizes again on it.
    * Stack storage keeps concurrent callers -- a submission thread and a
    * resource thread both choosing a carrier -- from sharing a buffer. */
   struct r300_rb2d_window scratch[R300_RB2D_LEGALIZE_MAX_WINDOWS];
   uint64_t best_cost = UINT64_MAX;
   uint32_t best_pitch = 0u;
   enum r300_rb2d_format best_format = R300_RB2D_FORMAT_ARGB8888;

   for (uint32_t i = 0; i < rows; i++) {
      const struct r300_rb2d_pitch_evidence *row = &table[i];
      if (row->usage != req->usage || row->evidence < req->minimum_evidence)
         continue;
      if (req->contract == R300_RB2D_CONTRACT_CONST_FILL_V1 &&
          (row->pitch_bytes != R300_RB2D_CONTRACT_V1_PITCH_BYTES ||
           row->format != R300_RB2D_FORMAT_ARGB8888))
         continue;
      struct r300_rb2d_legalize_result r;
      memset(&r, 0, sizeof(r));
      if (legalize_on_carrier(req, row->pitch_bytes, row->format, scratch,
                              R300_RB2D_LEGALIZE_MAX_WINDOWS, &r) == 0u)
         continue;
      const uint64_t cost = r300_rb2d_legalize_cost(&r, weights);
      /* Strict less-than keeps the smaller pitch on a tie, because the
       * table is ascending within a format and ARGB8888 rows precede
       * RGB565 rows. */
      if (cost < best_cost) {
         best_cost = cost;
         best_pitch = row->pitch_bytes;
         best_format = row->format;
      }
   }
   if (best_pitch != 0u && format_out != NULL)
      *format_out = best_format;
   return best_pitch;
}

bool
r300_rb2d_legalize_dwords(const struct r300_rb2d_window *windows,
                          uint32_t window_count, uint32_t *dwords_out)
{
   if (dwords_out == NULL)
      return false;
   *dwords_out = 0u;
   if (windows == NULL || window_count == 0u)
      return false;
   uint64_t n = 0u;
   for (uint32_t s = 0; s < window_count; s++) {
      if (windows[s].rect_count == 0u ||
          windows[s].rect_count > R300_RB2D_FILL_MAX_RECTS)
         return false;
      n += R300_RB2D_FILL_DWORDS(windows[s].rect_count);
      if (n > UINT32_MAX)
         return false;
   }
   *dwords_out = (uint32_t)n;
   return true;
}

int
r300_rb2d_legalize_emit(const struct r300_rb2d_window *windows,
                        uint32_t window_count, uint32_t *words,
                        uint32_t capacity, struct r300_rb2d_legalized_ib *out)
{
   if (windows == NULL || words == NULL || out == NULL ||
       window_count == 0u || window_count > R300_RB2D_LEGALIZE_MAX_WINDOWS)
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   out->ib = words;

   uint32_t at = 0u;
   for (uint32_t s = 0; s < window_count; s++) {
      const struct r300_rb2d_window *w = &windows[s];
      struct r300_rb2d_fill_plan plan;
      struct r300_rb2d_fill_ib segment;
      r300_rb2d_window_to_fill_plan(w, &plan);
      /* Each window is one full DESTINATION..EPILOGUE sequence on its own
       * surface, so the kernel tracker sees the destination, format, and
       * origin re-established before every launch, and a V1 window is
       * byte-identical to the plan emitter's stream. */
      const int r = r300_rb2d_fill_emit_into(&plan, words + at, capacity - at,
                                             &segment);
      if (r != 0) {
         memset(out, 0, sizeof(*out));
         return r;
      }
      if (segment.reloc_site_count != 1u ||
          segment.reloc_sites[0].slot != R300_RB2D_FILL_SLOT_DST) {
         memset(out, 0, sizeof(*out));
         return -EINVAL;
      }
      out->sites[out->site_count++] = (struct r300_rb2d_fill_reloc_site){
         .ib_index = at + segment.reloc_sites[0].ib_index,
         .slot = R300_RB2D_FILL_SLOT_DST,
      };
      at += segment.ib_size_dwords;
   }
   out->ib_size_dwords = at;
   return 0;
}
