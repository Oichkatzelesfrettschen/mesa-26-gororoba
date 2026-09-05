/*
 * SPDX-License-Identifier: MIT
 *
 * Exact-word and validation controls for the neutral R300-class depth
 * binding and depth-test state.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG

#include "r300_zb_depth_state.h"

#include "r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CAPACITY 64u

/* The reference binding: a 64-pixel-pitch 16-bit integer depth surface
 * at offset 0x2000 in its buffer object, LESS comparison with depth
 * writes on.
 */
static const struct r300_zb_depth_state_params reference = {
   .pitch_pixels = 64,
   .depth_format = R300_DEPTHFORMAT_16BIT_INT_Z,
   .depth_offset_bytes = 0x2000,
   .depth_relocation_payload = 0x0000000c,
   .depth_function = R300_ZS_LESS,
   .depth_write = true,
};

/* A one-dword PACKET0 run: header naming one payload dword at reg,
 * then the value.  Written from the packet grammar rather than from the
 * emitter, so a copied mistake cannot make both sides agree.
 */
static uint32_t
packet0_header(uint32_t reg)
{
   return (0u << 30) | ((0u) << 16) | ((reg >> 2) & 0x1fffu);
}

/* A type-3 packet header: the type bits, the count of following payload
 * dwords less one, and the opcode.  The BO reference is a NOP carrying
 * one payload dword.
 */
static uint32_t
packet3_header(uint32_t opcode, uint32_t count)
{
   return (3u << 30) | ((count - 1u) << 16) | opcode;
}

static void
exact_reference_stream(void)
{
   uint32_t words[CAPACITY];
   struct r300_pm4_builder b;
   memset(words, 0xa5, sizeof(words));
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &reference, NULL) == 0);
   assert(b.error == 0);
   assert(b.count == r300_zb_depth_state_dwords());

   const uint32_t expected[] = {
      packet0_header(R300_ZB_FORMAT), R300_DEPTHFORMAT_16BIT_INT_Z,
      packet0_header(R300_ZB_DEPTHOFFSET), 0x2000,
      packet3_header(R300_PM4_PACKET3_NOP, 1), 0x0000000c,
      packet0_header(R300_ZB_DEPTHPITCH), 64,
      packet0_header(R300_ZB_CNTL), R300_Z_ENABLE | R300_Z_WRITE_ENABLE,
      packet0_header(R300_ZB_ZSTENCILCNTL), R300_ZS_LESS,
      packet0_header(R300_ZB_BW_CNTL), 0,
   };
   assert(sizeof(expected) / sizeof(expected[0]) ==
          r300_zb_depth_state_dwords());
   for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
      if (words[i] != expected[i]) {
         fprintf(stderr, "dword %u: emitted %08x expected %08x\n", i,
                 words[i], expected[i]);
         assert(false);
      }
   }
   /* The emitter writes exactly its reserved dwords and nothing past
    * them, so the poison beyond the stream survives. */
   assert(words[r300_zb_depth_state_dwords()] == 0xa5a5a5a5u);
}

/* The reported relocation index names the payload the emission wrote:
 * ZB_FORMAT and ZB_DEPTHOFFSET each take two dwords and the relocation
 * NOP's header takes one, so the payload is the sixth.  A caller records
 * this as its depth-slot site, so an index naming any other dword binds
 * the depth object to a word no packet consumes.
 */
static void
reported_relocation_index(void)
{
   uint32_t words[CAPACITY];
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, words, CAPACITY);
   uint32_t reloc_index = R300_PM4_NO_INDEX;
   assert(r300_zb_depth_state_emit(&b, &reference, &reloc_index) == 0);
   assert(reloc_index == 5);
   assert(words[reloc_index] == reference.depth_relocation_payload);
   assert(words[reloc_index - 1] == packet3_header(R300_PM4_PACKET3_NOP, 1));

   /* A refused emission leaves the caller's index untouched, so a site
    * table cannot record a position from a call that wrote nothing.
    */
   struct r300_zb_depth_state_params bad = reference;
   bad.depth_function = R300_ZS_MASK + 1;
   uint32_t untouched = 0xabcdu;
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &bad, &untouched) == -EINVAL);
   assert(untouched == 0xabcdu);
}

/* The tile bits ride the pitch word.  A linear caller passes zero and the
 * word is the pitch alone, which is what the reference stream above pins;
 * a tiled caller's two ZB_DEPTHPITCH fields land beside the pitch, and a
 * value outside those fields or naming the reserved microtile encoding is
 * refused before the first dword.
 */
static void
tile_bits_ride_the_pitch(void)
{
   uint32_t words[CAPACITY];
   struct r300_pm4_builder b;
   struct r300_zb_depth_state_params params = reference;

   params.pitch_tile_bits =
      R300_DEPTHMACROTILE_ENABLE | R300_DEPTHMICROTILE_TILED;
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &params, NULL) == 0);
   assert(words[7] == (64u | R300_DEPTHMACROTILE_ENABLE |
                       R300_DEPTHMICROTILE_TILED));

   params.pitch_tile_bits = R300_DEPTHMICROTILE_TILED_SQUARE;
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &params, NULL) == 0);
   assert(words[7] == (64u | R300_DEPTHMICROTILE_TILED_SQUARE));

   /* Microtile 3 is the reserved encoding. */
   params.pitch_tile_bits = R300_DEPTHMICROTILE(3u);
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &params, NULL) == -EINVAL);
   assert(b.count == 0);

   /* Bit 18 is the microtile field's high bit, so it alone is the
    * TILED_SQUARE encoding rather than a stray bit. */
   assert(R300_DEPTHMICROTILE_TILED_SQUARE == (1u << 18));

   /* A bit outside the two fields would reach the endian field above them
    * or the pitch below. */
   params.pitch_tile_bits = 1u << 19;
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &params, NULL) == -EINVAL);
   assert(b.count == 0);

   params.pitch_tile_bits = 1u;
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &params, NULL) == -EINVAL);
   assert(b.count == 0);
}

static void
depth_write_disabled(void)
{
   uint32_t words[CAPACITY];
   struct r300_pm4_builder b;
   struct r300_zb_depth_state_params params = reference;
   params.depth_write = false;
   r300_pm4_builder_init(&b, words, CAPACITY);
   assert(r300_zb_depth_state_emit(&b, &params, NULL) == 0);
   /* ZB_CNTL is the ninth dword: Z enabled, the write bit clear, so a
    * depth-tested draw that must not disturb the surface has a state. */
   assert(words[9] == R300_Z_ENABLE);
}

static void
every_comparison_encodes(void)
{
   for (uint32_t function = R300_ZS_NEVER; function <= R300_ZS_ALWAYS;
        function++) {
      uint32_t words[CAPACITY];
      struct r300_pm4_builder b;
      struct r300_zb_depth_state_params params = reference;
      params.depth_function = function;
      r300_pm4_builder_init(&b, words, CAPACITY);
      assert(r300_zb_depth_state_emit(&b, &params, NULL) == 0);
      assert(words[11] == function);
   }
}

static void
every_depth_format_encodes(void)
{
   static const uint32_t formats[] = {
      R300_DEPTHFORMAT_16BIT_INT_Z,
      R300_DEPTHFORMAT_16BIT_13E3 | R300_INVERT_13E3_LEADING_ONES,
      R300_DEPTHFORMAT_16BIT_13E3 | R300_INVERT_13E3_LEADING_ZEROS,
      R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL,
   };
   for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
      uint32_t words[CAPACITY];
      struct r300_pm4_builder b;
      struct r300_zb_depth_state_params params = reference;
      params.depth_format = formats[i];
      r300_pm4_builder_init(&b, words, CAPACITY);
      assert(r300_zb_depth_state_emit(&b, &params, NULL) == 0);
      assert(words[1] == formats[i]);
   }
}

/* A refused call leaves the builder untouched, so a caller that ignores
 * the status cannot submit a half-written binding.
 */
static void
refusal_writes_nothing(const char *label,
                       const struct r300_zb_depth_state_params *params,
                       int expected)
{
   uint32_t words[CAPACITY];
   struct r300_pm4_builder b;
   memset(words, 0xa5, sizeof(words));
   r300_pm4_builder_init(&b, words, CAPACITY);
   const int result = r300_zb_depth_state_emit(&b, params, NULL);
   if (result != expected) {
      fprintf(stderr, "%s: returned %d, expected %d\n", label, result,
              expected);
      assert(false);
   }
   assert(b.count == 0);
   for (unsigned i = 0; i < CAPACITY; i++)
      assert(words[i] == 0xa5a5a5a5u);
}

static void
validation(void)
{
   struct r300_zb_depth_state_params params;

   params = reference;
   params.depth_format = 3;
   refusal_writes_nothing("reserved base depth format", &params, -EINVAL);

   params = reference;
   params.depth_format = R300_INVERT_13E3_LEADING_ZEROS;
   refusal_writes_nothing("13E3 inversion on integer depth", &params,
                          -EINVAL);

   params = reference;
   params.depth_format = R300_DEPTHFORMAT_16BIT_13E3 | (1u << 5);
   refusal_writes_nothing("depth format bit outside the encoding", &params,
                          -EINVAL);

   params = reference;
   params.depth_function = R300_ZS_ALWAYS + 1;
   refusal_writes_nothing("function past the field", &params, -EINVAL);

   params = reference;
   params.pitch_pixels = 0;
   refusal_writes_nothing("zero pitch", &params, -EINVAL);

   params = reference;
   params.pitch_pixels = 66;
   refusal_writes_nothing("pitch off the four-pixel grid", &params, -EINVAL);

   params = reference;
   params.pitch_pixels = R300_DEPTHPITCH_MASK + 4;
   refusal_writes_nothing("pitch past the field", &params, -EINVAL);

   params = reference;
   params.depth_offset_bytes = 0x2010;
   refusal_writes_nothing("offset the low five bits reach", &params,
                          -EINVAL);

   /* A null builder or params is malformed whatever the capacity. */
   assert(r300_zb_depth_state_emit(NULL, &reference, NULL) == -EINVAL);
   {
      uint32_t words[CAPACITY];
      struct r300_pm4_builder b;
      r300_pm4_builder_init(&b, words, CAPACITY);
      assert(r300_zb_depth_state_emit(&b, NULL, NULL) == -EINVAL);
      assert(b.count == 0);
   }
}

static void
existing_builder_error_stands(void)
{
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, NULL, CAPACITY);
   assert(b.error == -EINVAL);
   assert(r300_zb_depth_state_emit(&b, &reference, NULL) == -EINVAL);
   assert(b.error == -EINVAL);
   assert(b.count == 0);
}

static void
capacity_one_short(void)
{
   const uint32_t needed = r300_zb_depth_state_dwords();
   uint32_t words[CAPACITY];
   struct r300_pm4_builder b;
   memset(words, 0xa5, sizeof(words));
   r300_pm4_builder_init(&b, words, needed - 1);
   assert(r300_zb_depth_state_emit(&b, &reference, NULL) == -ENOSPC);
   assert(b.count == 0);
   for (unsigned i = 0; i < CAPACITY; i++)
      assert(words[i] == 0xa5a5a5a5u);

   /* Exactly the reserved capacity fits. */
   r300_pm4_builder_init(&b, words, needed);
   assert(r300_zb_depth_state_emit(&b, &reference, NULL) == 0);
   assert(b.count == needed);
}

static void
determinism(void)
{
   uint32_t first[CAPACITY], second[CAPACITY];
   struct r300_pm4_builder a, b;
   memset(first, 0, sizeof(first));
   memset(second, 0xff, sizeof(second));
   r300_pm4_builder_init(&a, first, CAPACITY);
   r300_pm4_builder_init(&b, second, CAPACITY);
   assert(r300_zb_depth_state_emit(&a, &reference, NULL) == 0);
   assert(r300_zb_depth_state_emit(&b, &reference, NULL) == 0);
   assert(memcmp(first, second,
                 r300_zb_depth_state_dwords() * sizeof(uint32_t)) == 0);
}

int
main(void)
{
   exact_reference_stream();
   reported_relocation_index();
   tile_bits_ride_the_pitch();
   depth_write_disabled();
   every_comparison_encodes();
   every_depth_format_encodes();
   validation();
   existing_builder_error_stands();
   capacity_one_short();
   determinism();
   printf("r300_zb_depth_state_test: all checks passed\n");
   return 0;
}
