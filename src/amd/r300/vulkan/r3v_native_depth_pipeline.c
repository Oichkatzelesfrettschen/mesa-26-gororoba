/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_native_depth_pipeline.h"
#include "amd/r300/common/r300_reg.h"

#include <errno.h>
#include <math.h>
#include <string.h>

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

static bool
lower_stencil_op(VkStencilOp op, uint32_t *operation)
{
   static const uint32_t operations[] = {
      [VK_STENCIL_OP_KEEP] = R300_ZS_KEEP,
      [VK_STENCIL_OP_ZERO] = R300_ZS_ZERO,
      [VK_STENCIL_OP_REPLACE] = R300_ZS_REPLACE,
      [VK_STENCIL_OP_INCREMENT_AND_CLAMP] = R300_ZS_INCR,
      [VK_STENCIL_OP_DECREMENT_AND_CLAMP] = R300_ZS_DECR,
      [VK_STENCIL_OP_INVERT] = R300_ZS_INVERT,
      [VK_STENCIL_OP_INCREMENT_AND_WRAP] = R300_ZS_INCR_WRAP,
      [VK_STENCIL_OP_DECREMENT_AND_WRAP] = R300_ZS_DECR_WRAP,
   };
   if (operation == NULL || (unsigned)op > VK_STENCIL_OP_DECREMENT_AND_WRAP)
      return false;
   *operation = operations[op];
   return true;
}

static bool
lower_stencil_face(const VkStencilOpState *source,
                   struct r300_zb_stencil_face_state *destination)
{
   if (source == NULL || destination == NULL ||
       !lower_compare(source->compareOp, &destination->function) ||
       !lower_stencil_op(source->failOp, &destination->fail_op) ||
       !lower_stencil_op(source->passOp, &destination->zpass_op) ||
       !lower_stencil_op(source->depthFailOp, &destination->zfail_op))
      return false;

   destination->reference = source->reference & UINT8_MAX;
   destination->compare_mask = source->compareMask & UINT8_MAX;
   destination->write_mask = source->writeMask & UINT8_MAX;
   return true;
}

static uint32_t
stencil_reference_mask(const struct r300_zb_stencil_face_state *face)
{
   return ((face->reference & UINT8_MAX) << R300_STENCILREF_SHIFT) |
          ((face->compare_mask & UINT8_MAX) << R300_STENCILMASK_SHIFT) |
          ((face->write_mask & UINT8_MAX) << R300_STENCILWRITEMASK_SHIFT);
}

static uint32_t
stencil_control(const struct r300_zb_stencil_face_state *front,
                const struct r300_zb_stencil_face_state *back,
                uint32_t depth_function)
{
   return ((depth_function & R300_ZS_MASK) << R300_Z_FUNC_SHIFT) |
          ((front->function & R300_ZS_MASK) << R300_S_FRONT_FUNC_SHIFT) |
          ((front->fail_op & R300_ZS_MASK) << R300_S_FRONT_SFAIL_OP_SHIFT) |
          ((front->zpass_op & R300_ZS_MASK) << R300_S_FRONT_ZPASS_OP_SHIFT) |
          ((front->zfail_op & R300_ZS_MASK) << R300_S_FRONT_ZFAIL_OP_SHIFT) |
          ((back->function & R300_ZS_MASK) << R300_S_BACK_FUNC_SHIFT) |
          ((back->fail_op & R300_ZS_MASK) << R300_S_BACK_SFAIL_OP_SHIFT) |
          ((back->zpass_op & R300_ZS_MASK) << R300_S_BACK_ZPASS_OP_SHIFT) |
          ((back->zfail_op & R300_ZS_MASK) << R300_S_BACK_ZFAIL_OP_SHIFT);
}

static bool
stencil_faces_differ(const struct r300_zb_stencil_face_state *front,
                     const struct r300_zb_stencil_face_state *back)
{
   return front->function != back->function ||
          front->fail_op != back->fail_op ||
          front->zpass_op != back->zpass_op ||
          front->zfail_op != back->zfail_op;
}

static uint32_t
float_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

static bool
lower_polygon_offset(const VkPipelineRasterizationStateCreateInfo *rasterization,
                     struct r300_zb_polygon_offset_state *offset)
{
   if (rasterization == NULL || offset == NULL)
      return false;

   memset(offset, 0, sizeof(*offset));
   if (!rasterization->depthBiasEnable)
      return true;

   /* R300 has scale and constant fields but no Vulkan clamp operation. */
   if (!isfinite(rasterization->depthBiasConstantFactor) ||
       !isfinite(rasterization->depthBiasSlopeFactor) ||
       !isfinite(rasterization->depthBiasClamp) ||
       rasterization->depthBiasClamp != 0.0f)
      return false;

   const float scale = rasterization->depthBiasSlopeFactor * 12.0f;
   const float constant = rasterization->depthBiasConstantFactor * 2.0f;
   if (!isfinite(scale) || !isfinite(constant))
      return false;

   offset->enabled = true;
   offset->front_scale = float_bits(scale);
   offset->front_offset = float_bits(constant);
   offset->back_scale = offset->front_scale;
   offset->back_offset = offset->front_offset;
   if ((rasterization->cullMode & VK_CULL_MODE_FRONT_BIT) == 0)
      offset->enable_mask |= R300_FRONT_ENABLE;
   if ((rasterization->cullMode & VK_CULL_MODE_BACK_BIT) == 0)
      offset->enable_mask |= R300_BACK_ENABLE;
   offset->enabled = offset->enable_mask != 0;
   return true;
}

static int
lower_pipeline(const VkPipelineDepthStencilStateCreateInfo *state,
               const VkPipelineRasterizationStateCreateInfo *rasterization,
               const struct r3v_native_depth_shader_flags *shader,
               struct r3v_native_depth_pipeline_state *out)
{
   if (state == NULL || rasterization == NULL || shader == NULL || out == NULL ||
       state->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
       state->pNext != NULL || state->flags != 0 ||
       state->depthBoundsTestEnable)
      return -EINVAL;

   /* A FragDepth decoration does not prove that the selected fragment binary
    * writes W.  The flag is admitted only when the binary selector supplies
    * the corresponding export contract. */
   if (shader->writes_depth && !shader->fragment_depth_export)
      return -EINVAL;

   uint32_t depth_function = R300_ZS_ALWAYS;
   if (state->depthTestEnable &&
       !lower_compare(state->depthCompareOp, &depth_function))
      return -EINVAL;

   struct r300_zb_stencil_state stencil = {0};
   if (state->stencilTestEnable) {
      if (!lower_stencil_face(&state->front, &stencil.front) ||
          !lower_stencil_face(&state->back, &stencil.back))
         return -EINVAL;
      stencil.enabled = true;
      stencil.two_sided =
         stencil_faces_differ(&stencil.front, &stencil.back) ||
         stencil_reference_mask(&stencil.front) !=
            stencil_reference_mask(&stencil.back);
      stencil.back_reference_requires_draw_split =
         stencil_reference_mask(&stencil.front) !=
         stencil_reference_mask(&stencil.back);
      stencil.front_reference_mask = stencil_reference_mask(&stencil.front);
      stencil.back_reference_mask = stencil_reference_mask(&stencil.back);
      stencil.zstencil_control =
         stencil_control(&stencil.front, &stencil.back, depth_function);
   }

   struct r300_zb_polygon_offset_state polygon_offset;
   if (!lower_polygon_offset(rasterization, &polygon_offset))
      return -EINVAL;

   const bool early_fragment_tests =
      shader->requested_early_fragment_tests ||
      (state->depthTestEnable && !shader->discards_fragments &&
       !shader->writes_depth && !shader->has_observable_side_effects);

   struct r3v_native_depth_pipeline_state candidate = {
      .hardware = {
         .depth_function = depth_function,
         .depth_write = state->depthTestEnable && state->depthWriteEnable,
         .depth_test_disabled = !state->depthTestEnable,
         .stencil = stencil,
         .polygon_offset = polygon_offset,
      },
      .depth_test_enable = state->depthTestEnable,
      .early_fragment_tests = early_fragment_tests,
      .late_fragment_tests = state->depthTestEnable && !early_fragment_tests,
   };

   if (shader->writes_depth) {
      candidate.hardware.fragment_depth_source = R300_FG_DEPTH_SRC_SHADER;
      candidate.hardware.fragment_depth_format = R300_W_FMT_W24 | R300_W_SRC_US;
   } else {
      candidate.hardware.fragment_depth_source = R300_FG_DEPTH_SRC_SCAN;
      candidate.hardware.fragment_depth_format = R300_W_FMT_W0 | R300_W_SRC_US;
   }
   *out = candidate;
   return 0;
}

int
r3v_native_depth_pipeline_lower_rasterization(
   const VkPipelineDepthStencilStateCreateInfo *state,
   const VkPipelineRasterizationStateCreateInfo *rasterization,
   const struct r3v_native_depth_shader_flags *shader,
   struct r3v_native_depth_pipeline_state *out)
{
   return lower_pipeline(state, rasterization, shader, out);
}

int
r3v_native_depth_pipeline_lower(
   const VkPipelineDepthStencilStateCreateInfo *state,
   const struct r3v_native_depth_shader_flags *shader,
   bool depth_bias_enabled,
   struct r3v_native_depth_pipeline_state *out)
{
   if (depth_bias_enabled)
      return -EINVAL;

   const VkPipelineRasterizationStateCreateInfo rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .cullMode = VK_CULL_MODE_NONE,
      .depthBiasEnable = VK_FALSE,
   };
   return lower_pipeline(state, &rasterization, shader, out);
}
