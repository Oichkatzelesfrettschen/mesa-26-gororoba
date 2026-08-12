/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#include "r300_fragment_binary.h"

#include "r300_reg.h"

#include "util/mesa-blake3.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* r300g bakes the fragment program as CP_PACKET0 register writes: header
 * bits 31:30 are zero, bits 29:16 carry payload count minus one, bit 15
 * selects one-register writes, and bits 14:0 carry the register byte offset
 * divided by four.
 */
#define R300_FRAGMENT_BINARY_PKT_TYPE(header) ((header) >> 30)
#define R300_FRAGMENT_BINARY_PKT_COUNT(header) ((((header) >> 16) & 0x3fff) + 1)
#define R300_FRAGMENT_BINARY_PKT_REG(header) (((header) & 0x7fff) << 2)
#define R300_FRAGMENT_BINARY_PKT_ONE_REG(header) \
   (((header) & RADEON_ONE_REG_WR) != 0)

/* The R300/R400 fragment program lives in the US/FG register block, its
 * immediate constants in the US constant file (R300_PFS_PARAM_0_X through
 * R300_PFS_PARAM_31_W), and the R500 upload path streams instruction words
 * through R500_GA_US_VECTOR_INDEX and R500_GA_US_VECTOR_DATA.  The data-port
 * packet carries RADEON_ONE_REG_WR so every payload dword reaches one port.
 * Symbol discovery uses `(rg --fixed-strings R300_PFS_PARAM_0_X
 * src/gallium/drivers/r300/)` and `(rg --fixed-strings
 * R500_GA_US_VECTOR_DATA src/gallium/drivers/r300/)`.
 */
#define R300_FRAGMENT_BINARY_US_FG_FIRST R300_US_CONFIG
#define R300_FRAGMENT_BINARY_US_FG_END R300_RB3D_CCTL
#define R300_FRAGMENT_BINARY_GA_US_VECTOR_INDEX R500_GA_US_VECTOR_INDEX
#define R300_FRAGMENT_BINARY_GA_US_VECTOR_DATA R500_GA_US_VECTOR_DATA

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
   bool vector_index_selected = false;
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
      const bool one_reg = R300_FRAGMENT_BINARY_PKT_ONE_REG(header);
      /* A multi-dword packet advances the register per dword unless the
       * write targets the GA US vector data port, which is a stream port at
       * one address.
       */
      if (reg == R300_FRAGMENT_BINARY_GA_US_VECTOR_DATA) {
         if (!one_reg || !vector_index_selected) {
            return false;
         }
      } else if (one_reg) {
         return false;
      } else if (reg == R300_FRAGMENT_BINARY_GA_US_VECTOR_INDEX &&
                 count != 1) {
         return false;
      } else {
         if (!r300_fragment_binary_register_valid(reg) ||
             !r300_fragment_binary_register_valid(reg + 4 * (count - 1))) {
            return false;
         }
      }
      if (reg == R300_FRAGMENT_BINARY_GA_US_VECTOR_INDEX)
         vector_index_selected = true;
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
