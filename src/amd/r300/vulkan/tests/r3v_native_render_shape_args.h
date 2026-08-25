/*
 * SPDX-License-Identifier: MIT
 *
 * Command-line form of a render shape, shared by the arming runner and
 * the attended render-shape runner so the digest one reports names the
 * cell the other submits.
 */
#ifndef R3V_NATIVE_RENDER_SHAPE_ARGS_H
#define R3V_NATIVE_RENDER_SHAPE_ARGS_H

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R3V_RENDER_SHAPE_ARGC 8

/* --shape <width> <height> <pitch> <bgra|rgba> <r> <g> <b> <a>: extent
 * and pitch in decimal pixels, the lane order by Vulkan format prefix,
 * and the constant as four binary32 bit patterns in hex (0x3f800000 is
 * 1.0), so the declaration is exact and the report grep-matches it.
 * Every token is vetted before the numeric parse, and a value outside
 * the family's admission refuses with the reason on stderr.
 */
static inline bool
r3v_render_shape_parse_decimal(const char *text, unsigned long *out)
{
   if (text[0] == '\0')
      return false;
   for (const char *c = text; *c != '\0'; c++) {
      if (*c < '0' || *c > '9')
         return false;
   }
   char *end = NULL;
   errno = 0;
   *out = strtoul(text, &end, 10);
   return errno == 0 && end != text && *end == '\0';
}

static inline bool
r3v_render_shape_parse_hex32(const char *text, uint32_t *out)
{
   if (strncmp(text, "0x", 2) != 0 || text[2] == '\0' || strlen(text) > 10)
      return false;
   for (const char *c = text + 2; *c != '\0'; c++) {
      const bool hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') ||
                       (*c >= 'A' && *c <= 'F');
      if (!hex)
         return false;
   }
   char *end = NULL;
   errno = 0;
   const unsigned long value = strtoul(text + 2, &end, 16);
   if (errno != 0 || end == text + 2 || *end != '\0' || value > 0xfffffffful)
      return false;
   *out = (uint32_t)value;
   return true;
}

/* Parses the eight tokens after --shape into shape.  Returns true on an
 * admitted shape; a refusal prints its reason.
 */
static inline bool
r3v_render_shape_parse(char *const argv[], struct r300_triangle_render_shape *shape)
{
   unsigned long extent[3];
   for (int i = 0; i < 3; i++) {
      if (!r3v_render_shape_parse_decimal(argv[i], &extent[i]) ||
          extent[i] < 1 || extent[i] > R300_TRIANGLE_RENDER_MAX_EXTENT) {
         fprintf(stderr, "shape extent or pitch outside 1..%u\n",
                 R300_TRIANGLE_RENDER_MAX_EXTENT);
         return false;
      }
   }
   memset(shape, 0, sizeof(*shape));
   shape->width = (uint32_t)extent[0];
   shape->height = (uint32_t)extent[1];
   shape->pitch_pixels = (uint32_t)extent[2];
   if (strcmp(argv[3], "bgra") == 0) {
      shape->lanes = R300_TRIANGLE_LANES_B8G8R8A8;
   } else if (strcmp(argv[3], "rgba") == 0) {
      shape->lanes = R300_TRIANGLE_LANES_R8G8B8A8;
   } else {
      fprintf(stderr, "shape lane order is bgra or rgba\n");
      return false;
   }
   for (int i = 0; i < 4; i++) {
      if (!r3v_render_shape_parse_hex32(argv[4 + i], &shape->color_bits[i])) {
         fprintf(stderr, "shape constant %d is a 0x-prefixed binary32 bit "
                 "pattern\n", i);
         return false;
      }
   }
   shape->triangle_count = 1;
   if (r300_tcl_bypass_triangle_render_shape_validate(shape) != 0) {
      fprintf(stderr, "shape refused by the render-shape family: pitch is "
              "even and at least the width, and each constant lies on the "
              "FP24 lattice\n");
      return false;
   }
   return true;
}

/* --offset <bytes>: the optional trailing pair naming where render row
 * 0 sits inside the color allocation, the cell's RB3D_COLOROFFSET0
 * payload.  Absent, the shape renders at the allocation base.  Returns
 * true when the two tokens are that pair and the value is admitted.
 */
static inline bool
r3v_render_shape_parse_offset(const char *flag, const char *value,
                              struct r300_triangle_render_shape *shape)
{
   unsigned long offset;
   if (strcmp(flag, "--offset") != 0 ||
       !r3v_render_shape_parse_decimal(value, &offset) ||
       offset > R300_TRIANGLE_MAX_TARGET_OFFSET ||
       (offset % R300_TRIANGLE_TARGET_OFFSET_ALIGNMENT) != 0) {
      fprintf(stderr, "--offset takes a multiple of %u inside %u\n",
              R300_TRIANGLE_TARGET_OFFSET_ALIGNMENT,
              R300_TRIANGLE_MAX_TARGET_OFFSET);
      return false;
   }
   shape->target_offset = (uint32_t)offset;
   return r300_tcl_bypass_triangle_render_shape_validate(shape) == 0;
}

static inline void
r3v_render_shape_print(FILE *out, const struct r300_triangle_render_shape *s)
{
   fprintf(out, "%ux%u pitch %u %s 0x%08x 0x%08x 0x%08x 0x%08x offset %u",
           s->width, s->height, s->pitch_pixels,
           s->lanes == R300_TRIANGLE_LANES_R8G8B8A8 ? "rgba" : "bgra",
           s->color_bits[0], s->color_bits[1], s->color_bits[2],
           s->color_bits[3], s->target_offset);
}

#endif /* R3V_NATIVE_RENDER_SHAPE_ARGS_H */
