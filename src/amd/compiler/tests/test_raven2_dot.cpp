/*
 * SPDX-License-Identifier: MIT
 */
#include "helpers.h"
#include "test_raven2_dot-spirv.h"

BEGIN_TEST(isel.raven2.sdot_4x8_iadd)
   /* Raven2 is GFX9 without v_dot4_i32_i8, so ACO expands the signed 8-bit dot
    * product itself: one 24-bit multiply per byte pair, whose byte extracts
    * fold into the SDWA operand selects, and two three-input adds. Vega20 is
    * also GFX9 and keeps the native instruction, so the expansion gates on
    * has_accelerated_dot_product rather than on the generation.
    */
   QoShaderModuleCreateInfo cs = qoShaderModuleCreateInfoGLSL(COMPUTE,
      QO_EXTENSION GL_EXT_integer_dot_product : require
      layout(local_size_x=64) in;
      layout(binding=0) buffer PackedDot {
         ivec4 packed_dot_values[];
      };
      void main() {
         uint index = gl_GlobalInvocationID.x;
         ivec4 packed_dot = packed_dot_values[index];
         //>> v1: %prod0 = v_mul_i32_i24 %a, %b dst_sel:dword src0_sel:sbyte0 src1_sel:sbyte0
         //! v1: %prod1 = v_mul_i32_i24 %a, %b dst_sel:dword src0_sel:sbyte1 src1_sel:sbyte1
         //! v1: %prod2 = v_mul_i32_i24 %a, %b dst_sel:dword src0_sel:sbyte2 src1_sel:sbyte2
         //! v1: %prod3 = v_mul_i32_i24 (kill)%a, (kill)%b dst_sel:dword src0_sel:sbyte3 src1_sel:sbyte3
         //! v1: %partial = v_add3_u32 (kill)%prod0, (kill)%prod1, (kill)%prod2
         //! v1: %dot = v_add3_u32 (kill)%partial, (kill)%prod3, (kill)%acc
         int dot = dotPacked4x8EXT(packed_dot.x, packed_dot.y) + packed_dot.z;
         packed_dot_values[index].w = dot;
      }
   );

   PipelineBuilder pbld(get_vk_device(CHIP_RAVEN2));
   pbld.add_cs(cs);
   pbld.print_ir(VK_SHADER_STAGE_COMPUTE_BIT, "ACO IR", true);
END_TEST
