/*
 * SPDX-License-Identifier: MIT
 *
 * Digest parity between the staged cell and the manifest that authorizes it.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_tcl_bypass_triangle.h"

#include "r300_reg.h"
#include "util/mesa-blake3.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An independent hash of the canonical little-endian encoding, written from
 * the BLAKE3 primitive and its own dword-to-byte shift loop rather than
 * through r300_triangle_ib_serialize or r300_triangle_ib_digest, so the
 * shared helper and its callers are checked against something other than
 * themselves.  The reconstruction stays independent of host uint32_t
 * layout: it hashes the encoding, not host memory.
 */
static void
independent_digest_hex(const uint32_t *dwords, uint32_t count, char *out)
{
   uint8_t *bytes = malloc((size_t)count * 4);
   assert(bytes != NULL);
   for (uint32_t i = 0; i < count; i++) {
      bytes[4 * i + 0] = (uint8_t)(dwords[i] & 0xff);
      bytes[4 * i + 1] = (uint8_t)((dwords[i] >> 8) & 0xff);
      bytes[4 * i + 2] = (uint8_t)((dwords[i] >> 16) & 0xff);
      bytes[4 * i + 3] = (uint8_t)((dwords[i] >> 24) & 0xff);
   }

   uint8_t digest[BLAKE3_OUT_LEN];
   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, bytes, (size_t)count * 4);
   _mesa_blake3_final(&ctx, digest);
   for (unsigned i = 0; i < BLAKE3_OUT_LEN; i++)
      snprintf(&out[2 * i], 3, "%02x", digest[i]);
   free(bytes);
}

/* The serializer writes each dword as four little-endian bytes; a known
 * pattern proves the byte order directly rather than through a digest.
 */
static void
test_serialize_writes_little_endian_bytes(void)
{
   const uint32_t dwords[] = {0x11223344u, 0x00000001u, 0xff000000u};
   uint8_t bytes[sizeof(dwords) / sizeof(dwords[0]) * 4];
   r300_triangle_ib_serialize(dwords, sizeof(dwords) / sizeof(dwords[0]),
                              bytes);

   static const uint8_t expected[] = {
      0x44, 0x33, 0x22, 0x11, /* 0x11223344 */
      0x01, 0x00, 0x00, 0x00, /* 0x00000001 */
      0x00, 0x00, 0x00, 0xff, /* 0xff000000 */
   };
   assert(memcmp(bytes, expected, sizeof(expected)) == 0);
}

/* The shared helper hashes the dwords a caller hands it, and hashing the same
 * bytes any other way gives the same string.  A helper that hashed a
 * different range -- the allocation rather than the stream, or the stream
 * plus a trailing word -- would disagree here.
 */
static void
test_helper_matches_an_independent_hash(void)
{
   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_reference_emit(&cell) == 0);

   char shared[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, shared);

   char independent[2 * BLAKE3_OUT_LEN + 1];
   independent_digest_hex(cell.ib, cell.ib_size_dwords, independent);

   assert(strcmp(shared, independent) == 0);

   /* The digest the arming gate is authorized against. */
   assert(strcmp(shared,
                 "55a2103c391a59896fd1294ac93459e11f26ce5e5b2a75a4c573ed910d"
                 "8487d0") == 0);

   r300_tcl_bypass_triangle_release(&cell);
}

/* One changed dword changes the digest, so the gate refuses a cell that is
 * not the one it authorized.  Without this the parity above would hold for a
 * helper that hashed nothing.
 */
static void
test_a_changed_dword_changes_the_digest(void)
{
   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_reference_emit(&cell) == 0);

   char before[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, before);

   for (uint32_t i = 0; i < cell.ib_size_dwords; i += 57) {
      const uint32_t saved = cell.ib[i];
      cell.ib[i] = saved ^ 1u;
      char after[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, after);
      assert(strcmp(before, after) != 0);
      cell.ib[i] = saved;
   }

   /* A shorter range is a different stream, so it hashes differently. */
   char shorter[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords - 1, shorter);
   assert(strcmp(before, shorter) != 0);

   r300_tcl_bypass_triangle_release(&cell);
}

/* A type-2 CP packet is one filler dword with no payload; its bits 29:16
 * decode as zero under the type-0/type-3 count rule, so a walk applying
 * that rule to a filler directly before the draw would step over the draw
 * header and miss it.  The synthetic stream places the filler at the draw's
 * doorstep and requires the walk to report the draw.
 */
static void
test_draw_walk_skips_type2_filler(void)
{
   uint32_t words[3] = {
      0x80000000u,
      (3u << 30) | R300_PACKET3_3D_DRAW_VBUF_2,
      0,
   };
   const struct r300_tcl_bypass_triangle_ib synthetic = {
      .ib = words,
      .ib_size_dwords = 3,
   };
   assert(r300_triangle_draw_dword(&synthetic) == 1);
}

/* The manifest's published geometry is the geometry the contract resolves
 * against, and the draw index it publishes is the draw the stream carries.
 */
static void
test_published_geometry_matches_the_cell(void)
{
   struct r300_tcl_bypass_triangle_ib cell;
   assert(r300_tcl_bypass_triangle_reference_emit(&cell) == 0);

   const uint32_t draw = r300_triangle_draw_dword(&cell);
   assert(draw > 0 && draw < cell.ib_size_dwords);
   assert((cell.ib[draw] >> 30) == 3);
   assert((cell.ib[draw] & 0xff00) == R300_PACKET3_3D_DRAW_VBUF_2);
   /* The draw is the cell's last draw, so no later dword carries one. */
   for (uint32_t i = draw + 1; i < cell.ib_size_dwords; i++)
      assert(!((cell.ib[i] >> 30) == 3 &&
               (cell.ib[i] & 0xff00) == R300_PACKET3_3D_DRAW_VBUF_2));

   /* The allocation carries one row past the render extent for the oracle's
    * canary, so the two differ by exactly that row.
    */
   assert(R300_TRIANGLE_ALLOCATION_ROWS == R300_TRIANGLE_TARGET_HEIGHT + 1);
   assert(R300_TRIANGLE_TARGET_PITCH_PIXELS >= R300_TRIANGLE_TARGET_WIDTH);

   r300_tcl_bypass_triangle_release(&cell);
}

/* The contract the manifest counts clauses for is the contract the emission
 * prefixes, so the count moves with the stream rather than beside it.
 */
static void
test_published_clause_count_matches_the_contract(void)
{
   struct r300_first_draw_contract contract;
   assert(r300_tcl_bypass_triangle_reference_contract(&contract) == 0);
   assert(contract.count == 79);

   struct r300_tcl_bypass_triangle_ib bare, full;
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format =
         r300_rb3d_colorpitch0_pack_argb8888(R300_TRIANGLE_TARGET_PITCH_PIXELS),
      .fragment_binary = &fs,
      .first_draw_contract = NULL,
   };
   assert(r300_tcl_bypass_triangle_emit(&params, &bare) == 0);
   params.first_draw_contract = &contract;
   assert(r300_tcl_bypass_triangle_emit(&params, &full) == 0);

   /* Each clause is a one-register PACKET0: a header and a payload. */
   assert(full.ib_size_dwords - bare.ib_size_dwords == contract.count * 2);

   r300_tcl_bypass_triangle_release(&bare);
   r300_tcl_bypass_triangle_release(&full);
   r300_fragment_binary_finish(&fs);
}

int
main(void)
{
   test_serialize_writes_little_endian_bytes();
   test_helper_matches_an_independent_hash();
   test_a_changed_dword_changes_the_digest();
   test_draw_walk_skips_type2_filler();
   test_published_geometry_matches_the_cell();
   test_published_clause_count_matches_the_contract();
   printf("r300_staging_manifest_test: digest and geometry parity held\n");
   return 0;
}
