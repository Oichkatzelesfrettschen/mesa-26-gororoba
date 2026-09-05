/* SPDX-License-Identifier: MIT */

#include "r300_rb2d_pitch_evidence.h"

#include <errno.h>
#include <string.h>

/* One row per carrier the legalizer may consider.  The 256-byte ARGB8888
 * row is the witnessed direct-write pitch; its receipt is the sealed
 * attended CONTROL_PASS of the public vkCmdFillBuffer route.  The 64-byte
 * row is the tightest DST_PITCH_OFFSET grid and is exercised by the
 * decomposition tests alone.  The dense candidates up to 16320 bytes --
 * R300_RB2D_MAX_PITCH_UNITS of the 64-byte grid, the widest surface
 * DST_PITCH_OFFSET's 8-bit pitch field names -- are the pitch-only
 * qualification targets and stay PLANNED until one runs.  The RGB565 row
 * records the kernel replay's 128-pixel row acceptance and pixel-129
 * refusal.
 */
static const struct r300_rb2d_pitch_evidence rows[] = {
   { 64u, R300_RB2D_FORMAT_ARGB8888, R300_RB2D_USAGE_FILL_BUFFER,
     R300_RB2D_PITCH_EVIDENCE_HOST_MODEL,
     "r300_rb2d_linear_span_test tight carrier arms" },
   { 256u, R300_RB2D_FORMAT_ARGB8888, R300_RB2D_USAGE_FILL_BUFFER,
     R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT,
     "steinmarder-r300 src/re/r300/results/"
     "r3v-native-rb2d-const-fill-public-route-receipt-"
     "vostro1000_rs485m_5974-strict-2d-cs" },
   { 1024u, R300_RB2D_FORMAT_ARGB8888, R300_RB2D_USAGE_FILL_BUFFER,
     R300_RB2D_PITCH_EVIDENCE_PLANNED, "planned" },
   { 4096u, R300_RB2D_FORMAT_ARGB8888, R300_RB2D_USAGE_FILL_BUFFER,
     R300_RB2D_PITCH_EVIDENCE_PLANNED, "planned" },
   { 8192u, R300_RB2D_FORMAT_ARGB8888, R300_RB2D_USAGE_FILL_BUFFER,
     R300_RB2D_PITCH_EVIDENCE_PLANNED, "planned" },
   { 16320u, R300_RB2D_FORMAT_ARGB8888, R300_RB2D_USAGE_FILL_BUFFER,
     R300_RB2D_PITCH_EVIDENCE_PLANNED, "planned" },
   { 256u, R300_RB2D_FORMAT_RGB565, R300_RB2D_USAGE_FILL_BUFFER,
     R300_RB2D_PITCH_EVIDENCE_KERNEL_REPLAY,
     "linux-radeon-gororoba scripts/run_r300_cs_2d_dst_controls.sh "
     "RGB565 row controls" },
};

const char *
r300_rb2d_pitch_evidence_class_name(enum r300_rb2d_pitch_evidence_class c)
{
   static const char *const names[R300_RB2D_PITCH_EVIDENCE_CLASS_COUNT] = {
      "planned", "host-model", "kernel-replay", "silicon-receipt",
   };
   return (unsigned)c < R300_RB2D_PITCH_EVIDENCE_CLASS_COUNT ? names[c] : NULL;
}

const struct r300_rb2d_pitch_evidence *
r300_rb2d_pitch_evidence_rows(uint32_t *count_out)
{
   if (count_out != NULL)
      *count_out = (uint32_t)(sizeof(rows) / sizeof(rows[0]));
   return rows;
}

const struct r300_rb2d_pitch_evidence *
r300_rb2d_pitch_evidence_find(uint32_t pitch_bytes,
                              enum r300_rb2d_format format,
                              enum r300_rb2d_usage usage)
{
   for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
      if (rows[i].pitch_bytes == pitch_bytes && rows[i].format == format &&
          rows[i].usage == usage)
         return &rows[i];
   }
   return NULL;
}

bool
r300_rb2d_pitch_admitted(uint32_t pitch_bytes, enum r300_rb2d_format format,
                         enum r300_rb2d_usage usage,
                         enum r300_rb2d_pitch_evidence_class at_least)
{
   const struct r300_rb2d_pitch_evidence *row =
      r300_rb2d_pitch_evidence_find(pitch_bytes, format, usage);
   return row != NULL && row->evidence >= at_least;
}

int
r300_rb2d_pitch_evidence_self_check(void)
{
   const size_t n = sizeof(rows) / sizeof(rows[0]);

   for (size_t i = 0; i < n; i++) {
      const struct r300_rb2d_pitch_evidence *r = &rows[i];
      if (r->pitch_bytes == 0u ||
          r->pitch_bytes % R300_RB2D_PITCH_GRANULARITY != 0u ||
          r->pitch_bytes / R300_RB2D_PITCH_GRANULARITY >
             R300_RB2D_MAX_PITCH_UNITS)
         return -EINVAL;
      if (r300_rb2d_format_bytes_per_pixel(r->format) == 0u)
         return -EINVAL;
      if ((unsigned)r->usage >= R300_RB2D_USAGE_COUNT ||
          (unsigned)r->evidence >= R300_RB2D_PITCH_EVIDENCE_CLASS_COUNT)
         return -EINVAL;
      if (r->artifact == NULL || r->artifact[0] == '\0')
         return -EINVAL;
      if (r->evidence == R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT &&
          strcmp(r->artifact, "planned") == 0)
         return -EINVAL;
      if (r->evidence == R300_RB2D_PITCH_EVIDENCE_PLANNED &&
          strcmp(r->artifact, "planned") != 0)
         return -EINVAL;
      for (size_t j = 0; j < i; j++) {
         const struct r300_rb2d_pitch_evidence *q = &rows[j];
         if (q->format == r->format && q->usage == r->usage) {
            if (q->pitch_bytes >= r->pitch_bytes)
               return -EINVAL;
         }
      }
   }
   if (!r300_rb2d_pitch_admitted(256u, R300_RB2D_FORMAT_ARGB8888,
                                 R300_RB2D_USAGE_FILL_BUFFER,
                                 R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT))
      return -EINVAL;
   return 0;
}
