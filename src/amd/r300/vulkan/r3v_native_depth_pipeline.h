/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_NATIVE_DEPTH_PIPELINE_H
#define R3V_NATIVE_DEPTH_PIPELINE_H

#include "amd/r300/common/r300_zb_depth_state.h"

#include <vulkan/vulkan_core.h>

#include <stdbool.h>

struct r3v_native_depth_shader_flags {
   bool discards_fragments;
   bool writes_depth;
   /* Set only when the selected admitted fragment binary programs the W
    * export registers for FragDepth.  SPIR-V decoration alone does not set
    * this capability. */
   bool fragment_depth_export;
   bool has_observable_side_effects;
   bool requested_early_fragment_tests;
};

struct r3v_native_depth_pipeline_state {
   struct r300_zb_depth_state_params hardware;
   bool depth_test_enable;
   bool early_fragment_tests;
   bool late_fragment_tests;
};

/* Maps the static depth/stencil state to the R300 depth state vocabulary.
 * The surface binding fields in hardware are supplied by the selected depth
 * image contract; this helper lowers comparison, write, and timing state. */
int r3v_native_depth_pipeline_lower(
   const VkPipelineDepthStencilStateCreateInfo *state,
   const struct r3v_native_depth_shader_flags *shader,
   bool depth_bias_enabled,
   struct r3v_native_depth_pipeline_state *out);

/* Lowers the same state with the static rasterization depth-bias values.
 * The legacy helper above remains useful for common tests that only carry a
 * depth-bias enable bit; callers with a Vulkan rasterization state use this
 * entry point so D24 scale, constant, and clamp handling is explicit. */
int r3v_native_depth_pipeline_lower_rasterization(
   const VkPipelineDepthStencilStateCreateInfo *state,
   const VkPipelineRasterizationStateCreateInfo *rasterization,
   const struct r3v_native_depth_shader_flags *shader,
   struct r3v_native_depth_pipeline_state *out);

#endif
