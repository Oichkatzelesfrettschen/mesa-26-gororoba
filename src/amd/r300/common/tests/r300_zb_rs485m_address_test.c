/* SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "r300_zb_depth_layout.h"
#include "r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct observation { uint32_t x, y, pitch, base, offset; };
/* Public factual vectors, separate from the algebraic enumeration. */
static const struct observation observations[] = {
   {36,20,64,2048,10016}, {37,20,64,2048,10020},
   {38,20,64,2048,10024}, {39,20,64,2048,10028},
   {36,21,64,2048,10032}, {37,21,64,2048,10036},
   {38,21,64,2048,10040}, {39,21,64,2048,10044},
   {1,1,64,2048,2068}, {33,1,64,2048,5140},
   {1,17,64,2048,6676}, {33,17,64,2048,9748},
   {5,1,64,2048,2100}, {9,1,64,2048,2196},
   {17,1,64,2048,2836}, {1,3,64,2048,2132},
   {1,5,64,2048,2324}, {1,9,64,2048,3220},
   {1,33,64,2048,10260}, {13,7,64,2048,2548},
   {29,15,64,2048,3700}, {45,23,64,2048,10228},
   {61,31,64,2048,8308}, {53,47,64,2048,13044},
   {33,17,96,2048,11796}, {37,21,64,4096,12084},
};

static struct r300_zb_depth_surface
surface(uint32_t pitch)
{
   struct r300_zb_depth_surface result = r300_zb_depth_surface_z24_macrotiled;
   result.pitch_pixels = pitch;
   result.address_resolver = &r300_zb_depth_address_rs485m_tiled;
   result.logical_pixel_addressing = true;
   result.logical_image_readback = true;
   return result;
}

static void
check_vectors(void)
{
   for (unsigned index = 0; index < sizeof(observations)/sizeof(observations[0]); ++index) {
      const struct observation *point = &observations[index];
      struct r300_zb_depth_surface image = surface(point->pitch);
      uint64_t offset = UINT64_MAX;
      assert(r300_zb_depth_surface_check(&image) == 0);
      assert(r300_zb_depth_address_checked(&image, point->base, 40000,
                                          point->x, point->y, &offset) == 0);
      assert(offset == point->offset);
   }
   for (uint32_t lane = 0; lane < 8; ++lane) {
      struct r300_zb_depth_surface image = surface(64);
      image.macrotile = R300_ZB_MACROTILE_LINEAR;
      uint64_t offset;
      assert(image.address_resolver->byte_offset(&image, 2048, 36 + lane % 4,
                                                 20 + lane / 4, &offset) == 0);
      assert(offset == 7456 + lane * 4);
   }
}

static void
check_bijection(void)
{
   unsigned storage_words = 0;
   for (uint32_t pitch = 64; pitch <= 96; pitch += 32) {
      for (uint32_t base = 2048; base <= 4096; base += 2048) {
         struct r300_zb_depth_surface image = surface(pitch);
         const uint64_t storage = (uint64_t)pitch * 80 * 4;
         const uint64_t mapped = base * 2 + storage + 4096;
         bool seen[96 * 80] = { false };
         unsigned logical = 0;
         for (uint64_t offset = base; offset < base + storage; offset += 4) {
            struct r300_zb_depth_address_coordinate coordinate;
            assert(image.address_resolver->coordinate(&image, base, mapped,
                                                       offset, &coordinate) == 0);
            assert(coordinate.x < pitch && coordinate.y < 80);
            const unsigned index = coordinate.y * pitch + coordinate.x;
            assert(!seen[index]);
            seen[index] = true;
            if (coordinate.x < 64 && coordinate.y < 64) {
               uint64_t roundtrip;
               assert(coordinate.region == R300_ZB_DEPTH_ADDRESS_LOGICAL);
               assert(r300_zb_depth_address_checked(&image, base, mapped,
                                                    coordinate.x, coordinate.y,
                                                    &roundtrip) == 0);
               assert(roundtrip == offset);
               ++logical;
            } else {
               uint64_t untouched = UINT64_MAX;
               assert(coordinate.region == R300_ZB_DEPTH_ADDRESS_PADDING);
               assert(r300_zb_depth_address_checked(&image, base, mapped,
                                                    coordinate.x, coordinate.y,
                                                    &untouched) != 0);
               assert(untouched == UINT64_MAX);
            }
            ++storage_words;
         }
         assert(logical == 4096);
      }
   }
   assert(storage_words == 25600);
}

static void
check_micro_inverse(void)
{
   unsigned storage_words = 0;
   for (uint32_t pitch = 64; pitch <= 96; pitch += 32) {
      for (uint32_t base = 2048; base <= 4096; base += 2048) {
         struct r300_zb_depth_surface image = surface(pitch);
         image.macrotile = R300_ZB_MACROTILE_LINEAR;
         bool seen[96 * 66] = {false};
         for (uint64_t offset = base; offset < base + pitch * 66u * 4u; offset += 4) {
            struct r300_zb_depth_address_coordinate coordinate;
            assert(image.address_resolver->coordinate(&image, base, 40000,
                                                       offset, &coordinate) == 0);
            assert(coordinate.x < pitch && coordinate.y < 66);
            const unsigned index = coordinate.y * pitch + coordinate.x;
            assert(!seen[index]);
            seen[index] = true;
            uint64_t result = UINT64_MAX;
            const int status = r300_zb_depth_address_checked(
               &image, base, 40000, coordinate.x, coordinate.y, &result);
            if (coordinate.x < 64 && coordinate.y < 64) {
               assert(coordinate.region == R300_ZB_DEPTH_ADDRESS_LOGICAL);
               assert(status == 0 && result == offset);
            } else {
               assert(coordinate.region == R300_ZB_DEPTH_ADDRESS_PADDING);
               assert(status != 0 && result == UINT64_MAX);
            }
            storage_words++;
         }
      }
   }
   assert(storage_words == 21120);
}

static void
check_refusals(void)
{
   struct r300_zb_depth_surface image = surface(64);
   uint64_t untouched = UINT64_MAX;
   struct r300_zb_depth_address_coordinate out = {77, 88, R300_ZB_DEPTH_ADDRESS_PADDING};
   const struct r300_zb_depth_address_coordinate sentinel = out;
   const uint64_t bad_offsets[] = {0, 2047, 2049, 22528, UINT64_MAX};
   for (unsigned index = 0; index < sizeof(bad_offsets)/sizeof(bad_offsets[0]); ++index) {
      assert(image.address_resolver->coordinate(&image, 2048, 28672,
                                                bad_offsets[index], &out) != 0);
      assert(memcmp(&out, &sentinel, sizeof(out)) == 0);
   }
   assert(r300_zb_depth_address_checked(&image, 2048, 2051, 0, 0, &untouched) != 0);
   assert(untouched == UINT64_MAX);
   assert(r300_zb_depth_address_checked(&image, UINT64_MAX - 3, UINT64_MAX,
                                        1, 1, &untouched) != 0);
   assert(untouched == UINT64_MAX);
   image.address_resolver = NULL;
   assert(r300_zb_depth_surface_check(&image) != 0);
   image = surface(64); image.bytes_per_pixel = 2;
   assert(r300_zb_depth_surface_check(&image) != 0);
   image = surface(64); image.pitch_pixels = 68;
   assert(r300_zb_depth_surface_check(&image) != 0);
   image = surface(64); image.depth_format = R300_DEPTHFORMAT_16BIT_INT_Z;
   assert(r300_zb_depth_surface_check(&image) != 0);
   assert(r300_zb_depth_surface_z24_macrotiled.address_resolver == NULL);
   assert(!r300_zb_depth_surface_z24_macrotiled.logical_pixel_addressing);
}

static void
check_components(void)
{
   struct r300_zb_depth_surface image = surface(64);
   for (uint32_t stencil = 0; stencil < 256; ++stencil) {
      for (uint32_t mask = 0; mask < 256; ++mask) {
         uint32_t updated;
         const uint32_t old = 0xabcdef00u | stencil;
         assert(r300_zb_depth_packed_update(&image, old, false, 0, true,
                                            0x5a, mask, &updated) == 0);
         assert(updated == (0xabcdef00u | ((stencil & ~mask) | (0x5a & mask))));
         assert(r300_zb_depth_packed_update(&image, old, true, 0x123456, false,
                                            0, 0, &updated) == 0);
         assert(updated == (0x12345600u | stencil));
      }
   }
}

int main(void)
{
   check_vectors();
   check_bijection();
   check_micro_inverse();
   check_refusals();
   check_components();
   puts("RS485M address: 34 vectors, 25600 macro and 21120 micro storage words, packed masks PASS");
   return 0;
}
