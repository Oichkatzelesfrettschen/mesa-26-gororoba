/*
 * SPDX-License-Identifier: MIT
 */

#include "../r3v_native_depth_pipeline.h"
#include "../../common/r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

int
main(void)
{
   struct r3v_native_depth_shader_flags shader = {0};
   struct r3v_native_depth_pipeline_state state;
   /* ZB_ZSTENCILCNTL comparison encodings in Vulkan comparison order. */
   const uint32_t encodings[] = {0u, 1u, 3u, 2u, 5u, 6u, 4u, 7u};
   for (VkCompareOp comparison = VK_COMPARE_OP_NEVER;
        comparison <= VK_COMPARE_OP_ALWAYS; comparison++) {
      VkPipelineDepthStencilStateCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
         .depthTestEnable = VK_TRUE,
         .depthWriteEnable = VK_TRUE,
         .depthCompareOp = comparison,
      };
      assert(r3v_native_depth_pipeline_lower(&info, &shader, false,
                                             &state) == 0);
      assert(state.hardware.depth_function == encodings[comparison]);
      assert(state.hardware.depth_write && state.early_fragment_tests);
   }

   VkPipelineDepthStencilStateCreateInfo disabled = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
   };
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.hardware.depth_write && !state.early_fragment_tests);

   disabled.depthTestEnable = VK_TRUE;
   disabled.depthWriteEnable = VK_FALSE;
   disabled.depthCompareOp = VK_COMPARE_OP_EQUAL;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(state.hardware.depth_function == R300_ZS_EQUAL &&
          !state.hardware.depth_write && state.early_fragment_tests);

   shader.requested_early_fragment_tests = true;
   struct r3v_native_depth_pipeline_state before_early = state;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before_early, sizeof(state)) == 0);
   shader.requested_early_fragment_tests = false;
   shader.discards_fragments = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests);

   shader.discards_fragments = false;
   shader.has_observable_side_effects = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests);
   shader.has_observable_side_effects = false;
   shader.writes_depth = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests);

   struct r3v_native_depth_pipeline_state before = state;
   disabled.front.failOp = VK_STENCIL_OP_REPLACE;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests);
   disabled.front.failOp = VK_STENCIL_OP_KEEP;
   disabled.stencilTestEnable = VK_TRUE;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.stencilTestEnable = VK_FALSE;
   disabled.depthCompareOp = (VkCompareOp)-1;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.depthCompareOp = VK_COMPARE_OP_EQUAL;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, true, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.depthBoundsTestEnable = VK_TRUE;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.depthBoundsTestEnable = VK_FALSE;
   disabled.pNext = &disabled;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   return 0;
}
