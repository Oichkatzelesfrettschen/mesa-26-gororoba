/* SPDX-License-Identifier: MIT */

#include "r300_fragment_binary.h"

#include "util/mesa-blake3.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* r300g bakes the fragment program as CP_PACKET0 register writes: header
 * bits 31:30 are zero, bits 29:16 carry payload count minus one, and bits
 * 15:0 carry the register byte offset divided by four.
 */
#define R300_FRAGMENT_BINARY_PKT_TYPE(header) ((header) >> 30)
#define R300_FRAGMENT_BINARY_PKT_COUNT(header) ((((header) >> 16) & 0x3fff) + 1)
#define R300_FRAGMENT_BINARY_PKT_REG(header) (((header) & 0xffff) << 2)

/* The R300/R400 fragment program lives in the US/FG register block, its
 * immediate constants in the US constant file (R300_PFS_PARAM_0_X 0x4C00
 * through the last parameter word below RB3D at 0x4E00), and the R500
 * upload path streams instruction words through the GA US vector
 * index/data pair.
 */
#define R300_FRAGMENT_BINARY_US_FG_FIRST 0x4600
#define R300_FRAGMENT_BINARY_US_FG_END 0x4e00
#define R300_FRAGMENT_BINARY_GA_US_VECTOR_INDEX 0x4054
#define R300_FRAGMENT_BINARY_GA_US_VECTOR_DATA 0x4058

static bool
r300_fragment_binary_register_valid(uint32_t reg)
{
   if (reg >= R300_FRAGMENT_BINARY_US_FG_FIRST &&
       reg < R300_FRAGMENT_BINARY_US_FG_END) {
      return true;
   }
   return reg == R300_FRAGMENT_BINARY_GA_US_VECTOR_INDEX ||
          reg == R300_FRAGMENT_BINARY_GA_US_VECTOR_DATA;
}

bool
r300_fragment_binary_stream_valid(const uint32_t *cb_code,
                                  uint32_t cb_code_size)
{
   if (cb_code == NULL || cb_code_size == 0) {
      return false;
   }

   uint32_t i = 0;
   while (i < cb_code_size) {
      uint32_t header = cb_code[i];
      if (R300_FRAGMENT_BINARY_PKT_TYPE(header) != 0) {
         return false;
      }
      uint32_t count = R300_FRAGMENT_BINARY_PKT_COUNT(header);
      if (i + 1 + count > cb_code_size) {
         return false;
      }
      uint32_t reg = R300_FRAGMENT_BINARY_PKT_REG(header);
      /* A multi-dword packet advances the register per dword unless the
       * write targets the GA US vector data port, which is a stream port at
       * one address.
       */
      if (reg == R300_FRAGMENT_BINARY_GA_US_VECTOR_DATA) {
         /* Whole payload lands on the port. */
      } else {
         if (!r300_fragment_binary_register_valid(reg) ||
             !r300_fragment_binary_register_valid(reg + 4 * (count - 1))) {
            return false;
         }
      }
      i += 1 + count;
   }
   return i == cb_code_size;
}

int
r300_fragment_binary_init(struct r300_fragment_binary *binary,
                          const uint32_t *cb_code, uint32_t cb_code_size,
                          uint32_t fg_depth_src, uint32_t us_out_w,
                          const char *compiler_identity)
{
   memset(binary, 0, sizeof(*binary));

   if (!r300_fragment_binary_stream_valid(cb_code, cb_code_size)) {
      return -EINVAL;
   }

   binary->cb_code = malloc(cb_code_size * sizeof(uint32_t));
   if (binary->cb_code == NULL) {
      return -ENOMEM;
   }
   memcpy(binary->cb_code, cb_code, cb_code_size * sizeof(uint32_t));
   binary->cb_code_size = cb_code_size;
   binary->fg_depth_src = fg_depth_src;
   binary->us_out_w = us_out_w;

   if (compiler_identity != NULL) {
      strncpy(binary->compiler_identity, compiler_identity,
              sizeof(binary->compiler_identity) - 1);
   }

   struct mesa_blake3 blake3;
   _mesa_blake3_init(&blake3);
   _mesa_blake3_update(&blake3, binary->cb_code,
                       cb_code_size * sizeof(uint32_t));
   _mesa_blake3_update(&blake3, &fg_depth_src, sizeof(fg_depth_src));
   _mesa_blake3_update(&blake3, &us_out_w, sizeof(us_out_w));
   _mesa_blake3_final(&blake3, binary->hash);

   binary->validated = true;
   return 0;
}

void
r300_fragment_binary_finish(struct r300_fragment_binary *binary)
{
   free(binary->cb_code);
   memset(binary, 0, sizeof(*binary));
}
