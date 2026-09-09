/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_native_depth_pipeline.h"
#include "amd/r300/common/r300_reg.h"

#include <errno.h>

static bool
lower_compare(VkCompareOp op, uint32_t *function)
{
   static const uint32_t functions[] = {
      [VK_COMPARE_OP_NEVER] = R300_ZS_NEVER,
      [VK_COMPARE_OP_LESS] = R300_ZS_LESS,
      [VK_COMPARE_OP_EQUAL] = R300_ZS_EQUAL,
      [VK_COMPARE_OP_LESS_OR_EQUAL] = R300_ZS_LEQUAL,
      [VK_COMPARE_OP_GREATER] = R300_ZS_GREATER,
      [VK_COMPARE_OP_NOT_EQUAL] = R300_ZS_NOTEQUAL,
      [VK_COMPARE_OP_GREATER_OR_EQUAL] = R300_ZS_GEQUAL,
      [VK_COMPARE_OP_ALWAYS] = R300_ZS_ALWAYS,
   };
   if (function == NULL || (unsigned)op > VK_COMPARE_OP_ALWAYS)
      return false;
   *function = functions[op];
   return true;
}

int
r3v_native_depth_pipeline_lower(
   const VkPipelineDepthStencilStateCreateInfo *state,
   const struct r3v_native_depth_shader_flags *shader,
   bool depth_bias_enabled,
   struct r3v_native_depth_pipeline_state *out)
{
   if (state == NULL || shader == NULL || out == NULL ||
       state->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
       state->pNext != NULL || state->flags != 0 ||
       state->depthBoundsTestEnable || state->stencilTestEnable ||
       depth_bias_enabled || shader->requested_early_fragment_tests)
      return -EINVAL;

   uint32_t function = R300_ZS_ALWAYS;
   if (state->depthTestEnable && !lower_compare(state->depthCompareOp,
                                                &function))
      return -EINVAL;

   struct r3v_native_depth_pipeline_state candidate = {
      .hardware = {
         .depth_function = function,
         .depth_write = state->depthTestEnable && state->depthWriteEnable,
         .depth_test_disabled = !state->depthTestEnable,
      },
      .depth_test_enable = state->depthTestEnable,
      .early_fragment_tests = state->depthTestEnable &&
                              !shader->discards_fragments &&
                              !shader->writes_depth &&
                              !shader->has_observable_side_effects,
   };
   *out = candidate;
   return 0;
}
