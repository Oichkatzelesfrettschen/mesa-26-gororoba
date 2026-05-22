// SPDX-License-Identifier: MIT

/* R300 fragment shader PFS_PARAM float24 packing utility.
 *
 * The R300 fragment shader loads constants into PFS_PARAM_0..31 registers
 * (AMD R3xx Programming Guide; see r300_fs.c:r300_emit_fs_code_to_buffer).  Each component is encoded
 * as a 24-bit float: bit 23 = sign, bits 22:16 = 7-bit exponent biased by
 * 63 (stored as frexpf_exponent + 62), bits 15:0 = top 16 bits of the IEEE
 * 754 mantissa.  The pack function mirrors r300_emit.c:pack_float24.
 *
 * Build and run self-tests:
 *   ninja -C builddir src/amd/r300/tools/r300_float24
 *   ./builddir/src/amd/r300/tools/r300_float24 --test
 *
 * Pack arbitrary floats:
 *   ./builddir/src/amd/r300/tools/r300_float24 1.0 -1.0 3.14159
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Convert an IEEE 754 single-precision float to the R300 fragment shader
 * 24-bit float format.  Mirrors r300_emit.c:pack_float24 exactly.
 *
 * The format stores:
 *   bit 23     sign
 *   bits 22:16 frexpf exponent + 62 (7 bits; bias 63 relative to frexpf)
 *   bits 15:0  top 16 bits of IEEE 754 mantissa (bits 22:7)
 *
 * Valid for normal IEEE floats within the 7-bit exponent range.  Does not
 * handle inf, NaN, or subnormals -- the hardware does not need them for
 * PFS_PARAM constant loads.
 */
static uint32_t
r300_pack_float24(float f)
{
   union {
      float fl;
      uint32_t u;
   } u;
   float mantissa;
   int exponent;
   uint32_t float24 = 0;

   if (f == 0.0f)
      return 0;

   u.fl = f;
   mantissa = frexpf(f, &exponent);

   if (mantissa < 0) {
      float24 |= (1u << 23);
      mantissa = -mantissa;
   }

   (void)mantissa; /* used only for sign; mantissa bits come from u.u */
   exponent += 62;
   float24 |= (uint32_t)((unsigned)exponent << 16);
   float24 |= (u.u & 0x7FFFFFu) >> 7;

   return float24;
}

/* Reconstruct a float from R300 float24 encoding.  Used only for round-trip
 * verification; not part of the hardware path.
 *
 * Inverse of r300_pack_float24: recover IEEE sign, exponent (frexpf_exp =
 * stored_exp - 62; IEEE_stored = frexpf_exp + 126), and top 16 mantissa bits.
 */
static float
r300_unpack_float24(uint32_t f24)
{
   union {
      float fl;
      uint32_t u;
   } u;
   uint32_t sign;
   unsigned stored_exp;
   uint32_t mantissa_bits;

   if (f24 == 0)
      return 0.0f;

   sign = (f24 >> 23) & 1u;
   stored_exp = (f24 >> 16) & 0x7Fu;
   mantissa_bits = f24 & 0xFFFFu;

   /* frexpf exponent = stored_exp - 62.  IEEE stored exponent bias is 127;
    * frexpf normalizes to [0.5, 1.0) so IEEE_stored = frexpf_exp - 1 + 127
    * = (stored_exp - 62) + 126 = stored_exp + 64.
    */
   u.u = (sign << 31) | ((stored_exp + 64u) << 23) | (mantissa_bits << 7);
   return u.fl;
}

/* Known golden values derived from the encoding rules and hand-verified. */
struct known_value {
   float input;
   uint32_t expected;
   const char *label;
};

static const struct known_value known_values[] = {
   { 0.0f,   0x000000u, "0.0"  },
   { 1.0f,   0x3F0000u, "1.0"  },
   {-1.0f,   0xBF0000u, "-1.0" },
   { 2.0f,   0x400000u, "2.0"  },
   { 0.5f,   0x3E0000u, "0.5"  },
   { 0.25f,  0x3D0000u, "0.25" },
   { 1.5f,   0x3F8000u, "1.5"  },
};

static int
run_known_value_tests(void)
{
   int fail = 0;

   printf("known-value tests:\n");
   for (int i = 0; i < (int)(sizeof(known_values) / sizeof(known_values[0])); i++) {
      float f = known_values[i].input;
      uint32_t expected = known_values[i].expected;
      uint32_t got = r300_pack_float24(f);

      if (got == expected) {
         printf("  PASS  %-8s -> 0x%06X\n", known_values[i].label, got);
      } else {
         printf("  FAIL  %-8s -> got 0x%06X, expected 0x%06X\n",
                known_values[i].label, got, expected);
         fail++;
      }
   }
   return fail;
}

/* Round-trip tests: pack then unpack must recover the original float to
 * within the 16-bit mantissa precision.  Dropping bits 6:0 of the IEEE
 * mantissa introduces at most 2^-16 relative error (~1.5e-5), so the
 * threshold 1e-4 gives ~6x headroom.
 */
static int
run_roundtrip_tests(void)
{
   static const float inputs[] = {
      1.0f, -1.0f, 2.0f, 0.5f, 0.25f, 1.5f, 3.14159f, -0.333f, 1024.0f, 0.001f,
   };
   int fail = 0;

   printf("round-trip tests (pack -> unpack within 16-bit mantissa precision):\n");
   for (int i = 0; i < (int)(sizeof(inputs) / sizeof(inputs[0])); i++) {
      float f = inputs[i];
      uint32_t packed = r300_pack_float24(f);
      float recovered = r300_unpack_float24(packed);
      float rel_err = (f != 0.0f) ? fabsf((recovered - f) / f) : fabsf(recovered);
      int ok = (rel_err < 1e-4f);

      printf("  %s  %12.6f -> 0x%06X -> %12.6f (rel_err %.2e)\n",
             ok ? "PASS" : "FAIL",
             (double)f, packed, (double)recovered, (double)rel_err);
      if (!ok)
         fail++;
   }
   return fail;
}

static void
print_usage(const char *argv0)
{
   fprintf(stderr,
      "Usage: %s [--test] [float ...]\n"
      "\n"
      "  --test     run known-value and round-trip self-tests\n"
      "  float ...  pack each float and print the R300 float24 hex\n"
      "\n"
      "R300 float24 (PFS_PARAM): bit 23 = sign, bits 22:16 = exponent\n"
      "(bias 63, frexpf_exp + 62), bits 15:0 = top 16 IEEE mantissa bits.\n",
      argv0);
}

int
main(int argc, char **argv)
{
   if (argc < 2) {
      print_usage(argv[0]);
      return 1;
   }

   if (strcmp(argv[1], "--test") == 0) {
      int fail = 0;
      fail += run_known_value_tests();
      fail += run_roundtrip_tests();
      if (fail) {
         printf("\n%d test(s) FAILED\n", fail);
         return 1;
      }
      printf("\nAll tests passed.\n");
      return 0;
   }

   for (int i = 1; i < argc; i++) {
      char *end;
      float f = strtof(argv[i], &end);
      if (*end != '\0') {
         fprintf(stderr, "error: not a float: %s\n", argv[i]);
         return 1;
      }
      printf("r300_pack_float24(%g) = 0x%06X\n", (double)f, r300_pack_float24(f));
   }
   return 0;
}
