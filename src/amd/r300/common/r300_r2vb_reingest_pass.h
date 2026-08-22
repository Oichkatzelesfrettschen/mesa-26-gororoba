/*
 * SPDX-License-Identifier: MIT
 *
 * R2VB producer-plus-re-ingest pass: one IB carrying the reference
 * producer pass followed by the reference triangle draw fetching the
 * carrier as its vertex stream, so the stream proves the GPU-write to
 * vertex-fetch ordering on one submission.
 */

#ifndef R300_R2VB_REINGEST_PASS_H
#define R300_R2VB_REINGEST_PASS_H

#include <stdbool.h>
#include <stdint.h>

/* The reference producer embeds the fixed triangle's three FLOAT_4
 * window-space vertices, so the delivered carrier is byte-identical to
 * the vertex BO the proven triangle cell fetches.  The re-ingest stream
 * concatenates the two reference emissions: the producer writes the
 * carrier through the color backend and publishes it (destination-cache
 * flush, 3D idle-clean wait, and the engine sync that keeps a later
 * vertex fetch of the same BO off stale vertex-cache content), and the
 * consumer opens with its own first-draw contract -- re-establishing the
 * scissor, color target, and US output format the producer retargeted --
 * then draws the triangle with the carrier bound through
 * 3D_LOAD_VBPNTR.  The color-target verdict is the unchanged triangle
 * oracle, and the carrier check stays available as the producer-stage
 * discriminant when the target fails.
 */

/* Two buffer objects: the carrier crosses both engines (color-backend
 * write, vertex fetch), and the color target receives the consuming
 * draw.  The slot order fixes the relocation-chunk indexing that both
 * component emissions already encode: the producer's carrier NOP payload
 * and the triangle's vertex NOP payload both name chunk entry zero, and
 * the triangle's color NOP payload names entry one, so the concatenation
 * resolves against this table with no payload rewriting.
 */
enum r300_r2vb_reingest_slot {
   R300_R2VB_REINGEST_SLOT_CARRIER = 0,
   R300_R2VB_REINGEST_SLOT_COLOR = 1,
   R300_R2VB_REINGEST_SLOT_COUNT = 2,
};

/* Three relocation sites over the two slots: the carrier is referenced
 * by the producer's color-target site and the consumer's vertex-fetch
 * site.
 */
#define R300_R2VB_REINGEST_MAX_RELOC_SITES 3u

struct r300_r2vb_reingest_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

struct r300_r2vb_reingest_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_r2vb_reingest_reloc_site
      reloc_sites[R300_R2VB_REINGEST_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
   /* The dword index where the consumer stream begins; the producer
    * stream occupies [0, consumer_start).  Retained evidence splits the
    * stream here when a verdict needs the per-stage bytes.
    */
   uint32_t consumer_start_dwords;
   bool owns_ib;
};

/* Emits the complete re-ingest stream from the two reference emissions.
 * Returns 0 or a negative errno; the caller owns the returned IB
 * allocation.
 */
int r300_r2vb_reingest_pass_emit(struct r300_r2vb_reingest_ib *out);

void r300_r2vb_reingest_pass_release(struct r300_r2vb_reingest_ib *ib);

/* Checks the emitted relocation sites against the stream: three sites,
 * each inside the stream and preceded by the relocation NOP header,
 * the carrier slot referenced exactly twice and the color slot exactly
 * once, payloads carrying the slot's chunk index.  Returns 0 or a
 * negative errno.
 */
int r300_r2vb_reingest_validate_reloc_sites(
   const struct r300_r2vb_reingest_ib *ib);

#endif /* R300_R2VB_REINGEST_PASS_H */
