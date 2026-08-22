/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "amd/r300/common/r300_carrier_policy.h"

static unsigned failures;

#define CHECK(COND, NAME)                 \
   do {                                   \
      if (COND) {                         \
         printf("  ok   - %s\n", NAME); \
      } else {                            \
         printf("  FAIL - %s\n", NAME); \
         failures++;                      \
      }                                   \
   } while (0)

static void
check_policy_formats(void)
{
   static const struct {
      const struct r300_carrier_policy *policy;
      const char *name;
      enum r300_numeric_domain domain;
      enum r300_carrier_encoding encoding;
      enum r300_carrier_format value_format;
      enum r300_carrier_format bit_format;
      unsigned input_stride;
      unsigned output_stride;
      unsigned max_exact_result;
      bool encodes_full_uint32;
      bool requires_fp32_rt;
   } cases[] = {
      { &r300_carrier_identity,
        "identity", R300_NUM_DOMAIN_FP24_RTZ, R300_CARRIER_ENC_IDENTITY,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        4, 4, 0, false, false },
      { &r300_carrier_dp4_u7,
        "dp4-u7-exact", R300_NUM_DOMAIN_U7_DOT,
        R300_CARRIER_ENC_RGBA8_UINT,
        R300_CARRIER_FORMAT_R32G32B32A32_FLOAT,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        16, 4, 64516, false, false },
      { &r300_carrier_dp4_u8_boundary,
        "dp4-u8-boundary", R300_NUM_DOMAIN_U8_OFFGRID,
        R300_CARRIER_ENC_RGBA8_UINT,
        R300_CARRIER_FORMAT_R32G32B32A32_FLOAT,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        16, 4, 131072, false, false },
      { &r300_carrier_blend_acc,
        "blend-acc-reduction", R300_NUM_DOMAIN_RB3D_BLEND,
        R300_CARRIER_ENC_RGBA8_UNORM,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        4, 4, 0, false, false },
      { &r300_carrier_zpass,
        "zpass-count", R300_NUM_DOMAIN_ZPASS_COUNT,
        R300_CARRIER_ENC_UINT32_COUNTER,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        4, 4, 0, false, false },
      { &r300_carrier_ieee16_classify,
        "ieee16-classify", R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
        R300_CARRIER_ENC_FP16_RAWBITS_RGBA8,
        R300_CARRIER_FORMAT_R32_FLOAT,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        4, 4, 65535, false, false },
      { &r300_carrier_ieee16_mul,
        "ieee16-mul", R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
        R300_CARRIER_ENC_RGBA8_U24,
        R300_CARRIER_FORMAT_R32G32_FLOAT,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        8, 4, 4190209, false, false },
      { &r300_carrier_ieee16_result,
        "ieee16-result", R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
        R300_CARRIER_ENC_RGBA8_U16,
        R300_CARRIER_FORMAT_R32_FLOAT,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        4, 4, 65535, false, false },
      { &r300_carrier_ieee16_debug,
        "ieee16-debug", R300_NUM_DOMAIN_IEEE_FP16_VIRTUAL,
        R300_CARRIER_ENC_RGBA8_UINT,
        R300_CARRIER_FORMAT_R32_FLOAT,
        R300_CARRIER_FORMAT_R8G8B8A8_UNORM,
        4, 4, 0, true, false },
   };

   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      CHECK(strcmp(cases[i].policy->name, cases[i].name) == 0 &&
               cases[i].policy->domain == cases[i].domain &&
               cases[i].policy->encoding == cases[i].encoding,
            cases[i].name);
      CHECK(cases[i].policy->value_format == cases[i].value_format &&
               cases[i].policy->bit_format == cases[i].bit_format,
            cases[i].name);
      CHECK(cases[i].policy->input_stride == cases[i].input_stride &&
               cases[i].policy->output_stride == cases[i].output_stride &&
               cases[i].policy->max_exact_result ==
                  cases[i].max_exact_result &&
               cases[i].policy->encodes_full_uint32 ==
                  cases[i].encodes_full_uint32 &&
               cases[i].policy->requires_fp32_rt == cases[i].requires_fp32_rt,
            cases[i].name);
   }
}

static void
check_dp4_selection(void)
{
   CHECK(r300_carrier_dp4_select(0) == &r300_carrier_dp4_u7,
         "DP4 magnitude zero selects the U7 policy");
   CHECK(r300_carrier_dp4_select(127) == &r300_carrier_dp4_u7,
         "DP4 magnitude 127 stays inside the U7 policy");
   CHECK(r300_carrier_dp4_select(128) == &r300_carrier_dp4_u8_boundary,
         "DP4 magnitude 128 selects the U8 boundary policy");
   CHECK(r300_carrier_dp4_select(255) == &r300_carrier_dp4_u8_boundary,
         "DP4 magnitude 255 stays inside the U8 boundary policy");
   CHECK(r300_carrier_dp4_select(256) == NULL,
         "DP4 magnitude 256 fails closed");
}

int
main(void)
{
   check_policy_formats();
   check_dp4_selection();

   printf("r300 carrier policy: %u failure(s)\n", failures);
   return failures != 0;
}
