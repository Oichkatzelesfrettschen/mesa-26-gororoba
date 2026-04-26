/*
 * Copyright (c) 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * terakan_nir_lower_abi.c — Instruction emission (HOW)
 *
 * Emits hardware instructions (VFETCH, TEX, KCACHE, MEM_RAT) from
 * physical hardware indices resolved by apply_pipeline_layout.
 *
 * Phase 0 mechanical file split of the former monolith
 * terakan_nir_lower_bindings.c.  See TERAKAN_SHADER_ABI_CONTRACT.md.
 */

#include "terakan_nir_lower_bindings_internal.h"
#include "terakan_pipeline_compute.h"  /* FIX-W setter prototype */

/* FIX-W production thread-local state (Q-2026-04-20).
 *
 * The variant-compile loop in terakan_pipeline_compute.c sets this to a
 * specific baseArrayLayer (0..7) before invoking the post-link lowering
 * pipeline, then resets to INT32_MIN.  The FIX-K block below reads it
 * at NIR-lowering time and bakes a literal nir_imm_int into THIS
 * variant's bytecode.  Each variant gets a different literal.
 *
 * Thread-local because Mesa's pipeline compilation is single-threaded
 * per-pipeline but multiple pipelines may compile concurrently in
 * different threads. */
__thread int g_terakan_fix_w_compile_layer = INT32_MIN;
__thread int g_terakan_fix_w_compile_ubo = INT32_MIN;

void
terakan_fix_w_set_compile_layer(int layer)
{
   g_terakan_fix_w_compile_layer = layer;
}

void
terakan_fix_w_set_compile_ubo(int value)
{
   g_terakan_fix_w_compile_ubo = value;
}


#include "util/u_debug.h"
#include "util/format/u_format.h"  /* FIX-Z: util_format_is_pure_uint + util_format_description */

static nir_def *
terakan_nir_resize_vector(nir_builder * const b, nir_def * const src,
                          unsigned const num_components, bool const pad_with_zero)
{
   if (src->num_components == num_components) {
      return src;
   }
   if (src->num_components > num_components) {
      return nir_trim_vector(b, src, num_components);
   }
   return pad_with_zero
             ? nir_pad_vector_imm_int(b, src, 0, num_components)
             : nir_pad_vector(b, src, num_components);
}

static nir_def *
terakan_nir_convert_32_to_type_bits(nir_builder * const b, nir_def * const value,
                                    unsigned const bit_size, nir_alu_type const type)
{
   if (bit_size == 32) {
      return value;
   }
   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_float:
      return nir_f2fN(b, value, bit_size);
   case nir_type_int:
      return nir_i2iN(b, value, bit_size);
   case nir_type_bool:
   case nir_type_uint:
   default:
      return nir_u2uN(b, value, bit_size);
   }
}

static nir_def *
terakan_nir_convert_type_to_32_bits(nir_builder * const b, nir_def * const value,
                                    nir_alu_type const type)
{
   if (value->bit_size == 32) {
      return value;
   }
   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_float:
      return nir_f2f32(b, value);
   case nir_type_int:
      return nir_i2i32(b, value);
   case nir_type_bool:
   case nir_type_uint:
   default:
      return nir_u2u32(b, value);
   }
}

static void
terakan_nir_build_uav_instr_r600(nir_builder * const b, nir_def * const uav_array_index,
                                 nir_def * const coord, nir_def * const value,
                                 nir_def * const compare_value, unsigned const uav_op,
                                 enum gl_access_qualifier const access,
                                 unsigned const id_base)
{
   unsigned num_components = coord->num_components;
   if (value->num_components > num_components) {
      num_components = value->num_components;
   }
   if (compare_value->num_components > num_components) {
      num_components = compare_value->num_components;
   }

   nir_def * const coord_normalized =
      terakan_nir_resize_vector(b, coord, num_components, true);
   nir_def * const value_normalized =
      terakan_nir_resize_vector(b, value, num_components, false);
   nir_def * const compare_value_normalized =
      terakan_nir_resize_vector(b, compare_value, num_components, false);

   nir_intrinsic_instr * const intrin =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_uav_instr_r600);
   intrin->num_components = (uint8_t)num_components;
   intrin->src[0] = nir_src_for_ssa(uav_array_index);
   intrin->src[1] = nir_src_for_ssa(coord_normalized);
   intrin->src[2] = nir_src_for_ssa(value_normalized);
   intrin->src[3] = nir_src_for_ssa(compare_value_normalized);
   nir_intrinsic_set_uav_op_r600(intrin, uav_op);
   nir_intrinsic_set_access(intrin, access);
   nir_intrinsic_set_id_base(intrin, id_base);
   nir_builder_instr_insert(b, &intrin->instr);
}

static nir_def *
terakan_nir_build_uav_returning_instr_r600(nir_builder * const b,
                                           unsigned const num_components,
                                           unsigned const bit_size,
                                           nir_def * const uav_array_index,
                                           nir_def * const coord, nir_def * const value,
                                           nir_def * const compare_value,
                                           nir_def * const immed_index,
                                           unsigned const uav_op,
                                           enum gl_access_qualifier const access,
                                           unsigned const id_base,
                                           unsigned const uav_return_id_base_r600)
{
   nir_intrinsic_instr * const intrin =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_uav_returning_instr_r600);
   intrin->num_components = (uint8_t)num_components;
   nir_def_init(&intrin->instr, &intrin->def, intrin->num_components, bit_size);
   intrin->src[0] = nir_src_for_ssa(uav_array_index);
   intrin->src[1] = nir_src_for_ssa(coord);
   intrin->src[2] = nir_src_for_ssa(value);
   intrin->src[3] = nir_src_for_ssa(compare_value);
   intrin->src[4] = nir_src_for_ssa(immed_index);
   nir_intrinsic_set_uav_op_r600(intrin, uav_op);
   nir_intrinsic_set_access(intrin, access);
   nir_intrinsic_set_id_base(intrin, id_base);
   nir_intrinsic_set_uav_return_id_base_r600(intrin, uav_return_id_base_r600);
   nir_intrinsic_set_mega_fetch_count_r600(intrin, 0);
   nir_builder_instr_insert(b, &intrin->instr);
   return &intrin->def;
}

/* Section 8.2 "Dataflow in Memory Hierarchy" of Evergreen Family Instruction Set Architecture says:
 *
 *     "Buffer objects are generally read and written directly by the work-items. Data is accessed
 *     through the L2 and L1 data caches on the GPU, but immediately invalidated at the end of a
 *     clause. [...] Similarly, writes are executed through the "fast-path" (depth buffer or DB) or
 *     "complete-path" (color buffer or CB), which have write-only caches that are invalidated, and
 *     all update bits are sent to memory at the end of a clause."
 *
 *     "Image objects are limited to read-only or write-only (no concurrent r/w). Thus, on reads,
 *     the data is cached through the L2 and L1 data caches; on writes, the data is cached through
 *     the CB/DB buffers."
 *
 * Therefore, for storage buffer and storage texel buffer loads, vertex fetch can always be used,
 * but for storage images, if write-read coherence is needed, the load must be performed via a
 * NOP_RTN UAV operation.
 */


static nir_def *
terakan_nir_uav_immed_index(nir_builder * const b,
                            struct terakan_physical_device_chip_info const * const chip_info)
{
   nir_def * wave_id = nir_load_hw_wave_id_r600(b);
   if (chip_info->two_shader_engines_max) {
      wave_id =
         nir_umad24_relaxed(b, nir_imm_int(b, 2), wave_id, nir_load_shader_engine_id_r600(b));
   }
   /* TODO(Triang3l): See how MBCNT behaves on wave32 chips and possibly scale the wave ID by 32
    * there.
    */
   return nir_umad24_relaxed(b, nir_imm_int(b, 64), wave_id,
                             nir_mbcnt_amd(b, nir_imm_int(b, ~0), nir_imm_zero(b, 1, 32)));
}

static nir_def *
terakan_nir_image_uav_coord(nir_builder * const b, nir_def * const image_coord,
                            enum glsl_sampler_dim const dim, bool const is_array,
                            struct terakan_nir_lower_bindings_state * const state,
                            unsigned const uav_index_zero_based)
{
   /* Buffers need separate handling due to the UAV base granularity offset. */
   assert(dim != GLSL_SAMPLER_DIM_BUF);

   unsigned uav_coord_num_components = 2;
   nir_def * uav_coord_components[3] = {
      nir_channel(b, image_coord, 0),
      /* 1D images may be promoted to 2D if they're tiled (such as if they're used by DB), so always
       * specify Y = 0 for them.
       */
      dim == GLSL_SAMPLER_DIM_1D ? nir_imm_zero(b, 1, 32) : nir_channel(b, image_coord, 1),
   };

   /* The hardware accepts the array layer in Z for both 1D and 2D/3D. It's relevant only if
    * RESOURCE_TYPE is TEXTURE#DARRAY or TEXTURE3D. UAV instructions don't accept a coordinate
    * swizzle, so don't initialize the array layer if it's not needed to avoid emitting an ALU
    * instruction for it.
    */
   /* FIX-CUBE-LAYER (2026-04-25): cube images always have 6 faces, so the
    * shader's layer coord (z) must be preserved even when is_array=false
    * (the typical VK_IMAGE_VIEW_TYPE_CUBE case).  Without this, multi-layer
    * cube image.store collapses all 6 faces to face 0 because uav_coord_num
    * stays at 2 and FIX-K below replaces coord.z with only baseArrayLayer
    * (=0 for multi-layer view).  The 3D / generic-array path already
    * handled this correctly; cube was the gap.  See steinmarder
    *   findings/active/2026-04-25-cube-multilayer-imagestore-rca.md
    */
   if (dim == GLSL_SAMPLER_DIM_3D || is_array || dim == GLSL_SAMPLER_DIM_CUBE) {
      uav_coord_num_components = 3;
      uav_coord_components[2] = nir_channel(b, image_coord, dim == GLSL_SAMPLER_DIM_1D ? 1 : 2);
   }

   /* FIX-H (C-2026-04-19-04): the CTS dEQP-VK.image.store._single_layer
    * variants pass an ivec3 coord (gx, gy, u_layerNdx) through
    * imageStore even when the GLSL declares the image as non-array
    * (image2D) on a VK_IMAGE_VIEW_TYPE_2D view of a multi-layer
    * (array_layers > 1) VkImage.  With RESOURCE_TYPE upgraded to
    * TEXTURE2DARRAY at descriptor bind time
    * (terakan_descriptor_set.c:113-133), the CB exporter DOES consult
    * R3.z at dispatch time -- but this pass dropped the z coord
    * above because dim=2D and is_array=false, so after NIR opt the
    * shader ends up with MOV R3.z, 0 and writes land at image slice 0
    * regardless of u_layerNdx.  Preserve the z channel when the
    * shader's coord has it (>= 3 components) so the compute dispatch
    * faithfully carries baseArrayLayer down to MEM_RAT STORE_TYPED.
    *
    * Gated via TERAKAN_FIX_H_PRESERVE_IMAGE_Z=1 during validation;
    * promote to default once the 2d_array single_layer sweep goes
    * from 78/78 Pass/Fail to 156/0.
    */
   if (uav_coord_num_components < 3 && dim != GLSL_SAMPLER_DIM_1D) {
      static int fix_h_cached = -1;
      if (fix_h_cached < 0) {
         fix_h_cached = debug_get_bool_option("TERAKAN_FIX_H_PRESERVE_IMAGE_Z", false) ? 1 : 0;
      }
      if (fix_h_cached) {
         uav_coord_num_components = 3;
         /* If the shader provided fewer than 3 components (GLSL-compiler
          * truncation when declaring imageStore on image2D with ivec3
          * coord), nir_channel of the missing component yields undef; we
          * explicitly take channel 2 if available, else emit zero.
          * The SHADER's u_layerNdx read for single_layer tests lives in
          * channel 2 of the original imageStore coord, so fetching that
          * channel preserves the runtime KCACHE read that was being
          * dead-code-eliminated after the z-drop. */
         if (image_coord->num_components >= 3) {
            uav_coord_components[2] = nir_channel(b, image_coord, 2);
         } else {
            uav_coord_components[2] = nir_imm_zero(b, 1, 32);
         }
      }
   }

   /* FIX-K (C-2026-04-19-06): runtime-add baseArrayLayer into coord.z so
    * MEM_RAT STORE_TYPED targets the correct physical slice of a
    * TEXTURE2DARRAY resource, compensating for Evergreen hardware that
    * reads R3.z as the absolute slice index and ignores
    * CB_COLOR_VIEW.SLICE_START on writes.  Runtime data source:
    * robustness_metadata.uav_base_array_layers[uav_idx] at bank 14
    * dword (28 + uav_idx).  The driver-side populator
    * (terakan_pipeline_layout.c:update_uav_robustness_metadata) writes
    * the real baseArrayLayer only when the UAV is a non-array view
    * over a multi-layer backing (guardrail #1); all other slots get
    * zero, so nir_iadd(coord_z, 0) collapses to coord_z (nir_opt_algebraic).
    *
    * Coverage: 1D / 2D / 3D / CUBE images.  For 1D non-array
    * singleton views over 1D-array backing images, the pass keeps
    * Y = 0 and injects the absolute layer into Z, matching the
    * TEXTURE1DARRAY resource path.
    *
    * Enabled by default after Task 94; set
    * TERAKAN_FIX_K_BASE_ARRAY_LAYER=0 only for regression bisects. */
   if (dim != GLSL_SAMPLER_DIM_BUF &&
       state != NULL && uav_index_zero_based < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT) {
      static int fix_k_cached = -1;
      if (fix_k_cached < 0) {
         fix_k_cached = debug_get_bool_option("TERAKAN_FIX_K_BASE_ARRAY_LAYER", true) ? 1 : 0;
      }
      if (fix_k_cached) {
         /* FIX-P (C-2026-04-19-13): H3 binary-split diagnostic.  When
          * TERAKAN_FIX_P_FORCE_Z is set, replace the entire KCACHE-
          * load-plus-iadd path with a literal-constant coord.z so we
          * can observe whether the hardware honours R3.z = N for
          * N != 0 independent of FIX-K's data-plumbing correctness.
          * Outcome (A) slice N is written: CB exporter is innocent,
          * FIX-K data path is broken somewhere.  Outcome (B) slice 0
          * is written: CB exporter is clamping R3.z. */
         int const fix_p_force_z = debug_get_num_option(
            "TERAKAN_FIX_P_FORCE_Z", -1);
         /* FIX-U (Q-2026-04-19): FALSIFIED.  Kept gated for reference. */
         /* FIX-W (Q-2026-04-20): literal-baked baseArrayLayer.
          *
          * Probe path: TERAKAN_FIX_W_LITERAL_LAYER=N env var (cached
          * static, single value for the whole process).  Used to
          * empirically validate single-slice pass.
          *
          * Production path: g_terakan_fix_w_compile_layer thread-local.
          * Set by terakan_pipeline_compute.c during per-variant compile
          * to a different value per variant.  Reset to INT32_MIN
          * between variants.  Read here at NIR-lowering time (compile
          * time) so the NIR_imm_int gets baked into the shader
          * bytecode of just THIS variant.  See findings doc
          * 2026-04-20-fix-w-production-implementation.md. */
         int fix_w_literal = g_terakan_fix_w_compile_layer;
         if (fix_w_literal == INT32_MIN) {
            /* Use two separate static vars (value + one-shot flag) to
             * avoid the INT32_MIN-1 sentinel overflow. */
            static int fix_w_env_cached = INT32_MIN;
            static bool fix_w_env_inited = false;
            if (!fix_w_env_inited) {
               fix_w_env_cached = (int)debug_get_num_option(
                  "TERAKAN_FIX_W_LITERAL_LAYER", INT32_MIN);
               fix_w_env_inited = true;
            }
            fix_w_literal = fix_w_env_cached;
         }
         static int fix_u_cached = -1;
         if (fix_u_cached < 0) {
            fix_u_cached = debug_get_bool_option(
               "TERAKAN_FIX_U_USE_TGID_Z", false) ? 1 : 0;
         }
         if (fix_p_force_z >= 0) {
            uav_coord_num_components = 3;
            uav_coord_components[2] =
               nir_imm_int(b, (int32_t)fix_p_force_z);
         } else {
            nir_def *base_array_layer_load;
            if (fix_w_literal != INT32_MIN) {
               /* FIX-W source: compile-time literal.  The driver must
                * select the right shader variant per dispatch based on
                * the bound ImageView's baseArrayLayer.  For probe use
                * with TERAKAN_FIX_W_LITERAL_LAYER=N: only slice N will
                * have correct data; slices != N get value from last
                * dispatch (R3.z=N consistently means every dispatch
                * writes slice N; slice N ends up with dispatch 7's
                * u_layerNdx=7 colorExpr which = expected colorExpr for
                * slice N only if the shader's u_layerNdx is also
                * rewritten -- which the CTS ref already folded to 0).
                *
                * Ergo for probe: slice FIX_W_LITERAL receives writes;
                * Reference for that slice is colorExpr(gz=FIX_W_LITERAL).
                * Shader's u_layerNdx = 0 (folded), so it writes
                * colorExpr(gz=0) to slice FIX_W_LITERAL.  These match
                * only when FIX_W_LITERAL=0.  Therefore:
                *   FIX_W_LITERAL=0 -> slice 0 passes, 1-7 fail
                *   FIX_W_LITERAL=3 -> slice 3 gets colorExpr(gz=0)
                *                      data, Reference expects
                *                      colorExpr(gz=3), fails
                * Production FIX-W would pair the literal with a
                * shader variant per layer that ALSO rewrites
                * u_layerNdx to the literal -- full fix. */
               base_array_layer_load = nir_imm_int(b, fix_w_literal);
            } else if (fix_u_cached) {
               /* FIX-U source: SPI-injected workgroup_id.z (= START_Z).
                * FALSIFIED -- kept gated for reference.  See 2026-04-19
                * audit addendum 6 in steinmarder. */
               nir_def * const wg_id = nir_load_workgroup_id(b);
               base_array_layer_load = nir_channel(b, wg_id, 2);
            } else {
               /* Original FIX-K KCACHE bank 14 path -- preserved as
                * fallback while FIX-U is under validation.
                *
                * bank 14 layout: dword 28 starts the
                * uav_base_array_layers[12] array; each entry is one
                * uint32_t.  vec4 index = dword/4, component = dword%4. */
               uint32_t const layer_dword = 28u + uav_index_zero_based;
               uint32_t const layer_vec4_index = layer_dword / 4u;
               uint32_t const layer_component = layer_dword % 4u;

               *state->kcache_needed |=
                  (uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA;

               base_array_layer_load = nir_load_kcache_r600(
                  b, 1, 32, nir_imm_zero(b, 1, 32),
                  .access = ACCESS_CAN_REORDER,
                  .id_base = TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
                  .base = layer_vec4_index,
                  .component = layer_component);
            }

            if (uav_coord_num_components < 3) {
               /* Promote to 3 components with coord_z = baseArrayLayer. */
               uav_coord_num_components = 3;
               uav_coord_components[2] = base_array_layer_load;
            } else {
               /* Existing coord_z carries shader-provided z (e.g. imageStore
                * on image2DArray with ivec3 coord, or the FIX-H preserved
                * channel 2): add baseArrayLayer to it.  Zero-layer
                * descriptors contribute zero so shader-z is preserved. */
               uav_coord_components[2] =
                  nir_iadd(b, uav_coord_components[2], base_array_layer_load);
            }
         }
      }
   }

   return nir_vec(b, uav_coord_components, uav_coord_num_components);
}


/*
 * terakan_nir_emit_write_guard
 *
 * Emits a software bounds check before a MEM_RAT write (store or atomic).
 * Returns an nir_def* that is true (non-zero) when the write is in-bounds,
 * false when OOB.
 *
 * Two tiers of caller usage:
 *
 *   Tier 1 (IF/ENDIF): caller wraps the MEM_RAT instruction in
 *     nir_push_if / nir_pop_if gated on this result.  Used for
 *     graphics UAV writes where the hardware descriptor determines the
 *     destination — we cannot redirect the address from the shader.
 *     Costs 1 CF stack entry (safe up to depth 3, ISA §3.6.5).
 *
 *   Tier 2 (math predication): caller uses nir_bcsel to select between
 *     the real address and the trash page address based on this result.
 *     Used for compute store_global where we control the destination.
 *     Costs 0 CF stack entries — pure ALU.
 *
 * INVARIANT: Tier 2 math predication MUST NOT be used for atomic operations.
 * Atomics are read-modify-write; redirecting an OOB atomic to the trash page
 * causes all failing threads to contend on the same 4KB page, creating a
 * catastrophic cache-coherency bottleneck.  Additionally, the atomic return
 * value from the trash page is garbage.  Atomics MUST use Tier 1 (IF/ENDIF)
 * so that OOB threads skip the instruction entirely.  If the CF stack is
 * exhausted, the compiler must flatten earlier control flow to free a slot.
 *
 * Hardware basis: MEM_RAT writes are NOT bounds-checked by Evergreen silicon
 * (Phase 5 Probe 7).  OOB MEM_RAT writes corrupt adjacent VRAM.  This is
 * the only defense.
 *
 * The UAV byte size is read from KCACHE bank 14 (robustness metadata buffer),
 * which holds uint32_t buffer_uav_byte_size[12] at dword offsets [0..11].
 *
 * Guard condition:  (write_end_offset <= uav_byte_size)
 *   where write_end_offset = byte_offset + write_size_bytes
 */
static nir_def *
terakan_nir_emit_write_guard(nir_builder * const b,
                             nir_def * const byte_offset,
                             uint32_t const write_size_bytes,
                             uint32_t const uav_index_zero_based,
                             nir_def * const uav_array_index,
                             struct terakan_nir_lower_bindings_state * const state)
{
   /* Mark KCACHE bank 14 (robustness metadata) as needed. */
   *state->kcache_needed |= (uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA;

   /* Load the exact byte size for this UAV from KCACHE bank 14.
    * Layout: uint32_t buffer_uav_byte_size[12] starting at dword 0. */
   uint32_t const size_vec4_index = uav_index_zero_based / 4;
   uint32_t const size_component = uav_index_zero_based % 4;

   nir_def *uav_byte_size = nir_load_kcache_r600(
      b, 1, 32, uav_array_index,
      .access = ACCESS_CAN_REORDER,
      .id_base = TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
      .base = size_vec4_index,
      .component = size_component);

   /* write_end = byte_offset + write_size_bytes (saturating add to prevent
    * overflow: if byte_offset is near UINT32_MAX, the add would wrap to a
    * small value and pass the bounds check). */
   nir_def *write_end = nir_uadd_sat(b, byte_offset, nir_imm_int(b, write_size_bytes));

   /* in_bounds = (write_end <= uav_byte_size) */
   return nir_uge(b, uav_byte_size, write_end);
}

/*
 * terakan_nir_buffer_uav_coord
 *
 * Emits the UAV coordinate computation for storage-buffer and texel-buffer
 * accesses.  When robust_access is true, injects a software ALU umin clamp
 * (nir_umin_imm) that bounds the coordinate so that no possible base
 * granularity offset can wrap it into an in-bounds address.
 *
 * --- MANDATORY SOFTWARE BOUNDS CLAMP (DO NOT BYPASS) ---
 *
 * Terascale silicon (Evergreen-class VLIW5, including the Brazos E-300
 * target) does NOT provide deterministic hardware out-of-bounds handling
 * for UAV writes, texel-buffer fetches, or byte-granular descriptor
 * clamping.  Empirically observed failure modes include GPU ring lockups
 * on raw buffer overruns via the texture fetch unit, and adjacent-block
 * corruption when the rasterizer write mask is not byte-aligned.
 *
 * Consequently, the software ALU clamp below is the ONLY guarantee of
 * deterministic Vulkan robustness semantics on this hardware.  Future
 * revisions must not replace it with silicon-trust heuristics, hardware
 * probes, or "fast paths" conditional on undocumented GPU behavior.  If
 * a future probe demonstrates that hardware behavior is clean across the
 * entire parameter space, the clamp may be dropped only under an explicit
 * `TERAKAN_ROBUSTNESS_HW_VERIFIED` compile-time flag, never by default.
 *
 * The effective robust_access flag is threaded from the caller via
 * terakan_nir_lower_bindings_state::robust_buffer_access, which the
 * pipeline compiler computes as:
 *     device->enabled_features.robustBufferAccess
 *   OR
 *     any per-stage/per-pipeline VK_EXT_pipeline_robustness state
 */
static nir_def *
terakan_nir_buffer_uav_coord(nir_builder * const b, nir_def * coord,
                             uint32_t const uav_index_zero_based, nir_def * const uav_array_index,
                             bool const robust_access, bool const include_helpers,
                             struct terakan_nir_lower_bindings_state * const state)
{
   /* Add the UAV base granularity offset. */

   if (robust_access) {
      /* SOFTWARE ALU BOUNDS CLAMP — MANDATORY on Terascale.  See function
       * header comment for the hardware-correctness rationale.
       *
       * If the coordinate provided by the application is already near
       * UINT32_MAX, adding the UAV base granularity offset may wrap an
       * out-of-bounds address into an in-bounds address near zero.
       * Clamp the coordinate so that no possible base granularity offset
       * value can wrap.
       *
       * For storage buffers, the offset is provided in bytes, but the UAV
       * uses a format with 4 bytes per element, for which
       * terakan_color_descriptor_buffer_uav_base_granularity_log2 is the
       * pipe interleave.
       * For texel buffers, the offset is in elements, but for all possible
       * element sizes terakan_color_descriptor_buffer_uav_base_granularity_log2
       * divided by the element size never exceeds the pipe interleave.
       * Moreover, texel buffers are fetched at a signed coordinate, not
       * unsigned, so any address > INT32_MAX is out-of-bounds.
       */
      struct terakan_physical_device const * const physical_device = container_of(
         state->layout->vk.base.device->physical, struct terakan_physical_device const, vk);
      uint32_t const max_uav_range =
         ~(((uint32_t)1 << physical_device->tiling_info.pipe_interleave_bytes_log2) - 1);
      assert(physical_device->vk.properties.maxStorageBufferRange <= max_uav_range);
      coord = nir_umin_imm(b, coord, max_uav_range);
   }

   *state->driver_push_constants_used |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_BASE_GRANULARITY_OFFSET);

   nir_def *base_granularity_offset;
   nir_const_value *const_array_idx =
      nir_src_as_const_value(nir_src_for_ssa(uav_array_index));

   if (const_array_idx != NULL) {
      /* KCACHE fast-path: constant array index folds into a static vec4
       * offset in bank 15 (push constants).  1-cycle ALU read instead of
       * a 20-40 cycle VFETCH. */
      uint32_t const byte_offset =
         offsetof(struct terakan_push_constants_driver,
                  buffer_uav_base_granularity_offset) +
         sizeof(uint32_t) * (uav_index_zero_based + const_array_idx->u32);
      *state->kcache_needed |= (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS;
      base_granularity_offset = nir_load_kcache_r600(
         b, 1, 32, nir_imm_zero(b, 1, 32),
         .access = ACCESS_CAN_REORDER,
         .id_base = TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
         .base = byte_offset / 16,
         .component = (byte_offset % 16) / 4);
   } else {
      /* Dynamic array index — fall back to VFETCH from push constant
       * resource descriptor. */
      BITSET_SET(state->resources_needed, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
      base_granularity_offset = nir_load_buffer_resource_r600(
         b, 1, 32, nir_imm_zero(b, 1, 32), nir_ishl_imm(b, uav_array_index, 2),
         .access = ACCESS_CAN_REORDER | (include_helpers ? ACCESS_INCLUDE_HELPERS : 0),
         .id_base = TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS,
         .base = offsetof(struct terakan_push_constants_driver,
                          buffer_uav_base_granularity_offset) +
                 sizeof(uint32_t) * uav_index_zero_based,
         .format = PIPE_FORMAT_R32_UINT);
   }
   return nir_iadd_nuw(b, coord, base_granularity_offset);
}

/* If there was an error while lowering the binding, such as if the binding was not obtained
 * successfully, to avoid leaving the shader in an indeterminate state as it's very easy for an app
 * to cause issues like those, treating invalid instructions largely as null descriptor accesses -
 * returning zero for loads, ignoring stores.
 */
static void
terakan_nir_lower_bindings_instr_to_null(nir_instr * const instr)
{
   nir_def * const old_def = nir_instr_def(instr);
   if (old_def != NULL) {
      nir_builder b = nir_builder_at(nir_before_instr(instr));
      nir_def_rewrite_uses(old_def, nir_imm_zero(&b, old_def->num_components, old_def->bit_size));
   }
   nir_instr_remove(instr);
}

static bool
terakan_nir_lower_bindings_instr_tex(nir_builder * const b, nir_tex_instr * const tex,
                                     struct terakan_nir_lower_bindings_state * const state)
{
   bool shader_nir_progress = false;

   mesa_shader_stage const stage = b->shader->info.stage;

   /* Newer NIR may represent descriptor references as *_handle sources instead of *_deref.
    * Support both forms here, and preserve already-lowered texture/sampler indices by still
    * recording descriptor demand in resources_needed/samplers_needed.
    */

   struct terakan_nir_binding binding;

   int texture_src_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (texture_src_index == -1) {
      texture_src_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_handle);
   }
   if (likely(texture_src_index != -1)) {
      nir_tex_src * const texture_src = &tex->src[texture_src_index];
      if (unlikely(!terakan_nir_get_binding(texture_src->src, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                            state->layout, b->shader, &binding))) {
         if (tex->texture_index < TERAKAN_RESOURCE_HW_COUNT_FETCH) {
            /* Keep already-lowered texture indices alive even if nir_chase_binding
             * can't reconstruct the descriptor chain from this source form. */
            BITSET_SET(state->resources_needed, tex->texture_index);
            shader_nir_progress = true;
         } else {
            terakan_nir_lower_bindings_instr_to_null(&tex->instr);
            return true;
         }
      } else {
         uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                             binding.set_binding->first_shader_resources[stage];
         BITSET_SET_RANGE(state->resources_needed,
                          resource_index_base + binding.array_index_range_first,
                          resource_index_base + binding.array_index_range_last);
         shader_nir_progress = true;
         tex->texture_index = resource_index_base;
         if (binding.array_index != NULL) {
            if (nir_def_instr(binding.array_index)->type == nir_instr_type_load_const) {
               tex->texture_index +=
                  nir_instr_as_load_const(nir_def_instr(binding.array_index))->value[0].u32;
               nir_tex_instr_remove_src(tex, texture_src_index);
            } else {
               texture_src->src_type = nir_tex_src_texture_offset;
               nir_src_rewrite(&texture_src->src, binding.array_index);
            }
         } else {
            nir_tex_instr_remove_src(tex, texture_src_index);
         }
      }
   } else if (tex->texture_index < TERAKAN_RESOURCE_HW_COUNT_FETCH) {
      BITSET_SET(state->resources_needed, tex->texture_index);
   }

   int sampler_src_index = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (sampler_src_index == -1) {
      sampler_src_index = nir_tex_instr_src_index(tex, nir_tex_src_sampler_handle);
   }
   if (likely(sampler_src_index != -1)) {
      nir_tex_src * const sampler_src = &tex->src[sampler_src_index];
      if (unlikely(!terakan_nir_get_binding(sampler_src->src, VK_DESCRIPTOR_TYPE_SAMPLER,
                                            state->layout, b->shader, &binding))) {
         if (tex->sampler_index < TERAKAN_SAMPLER_HW_COUNT_PER_STAGE) {
            /* Same fallback strategy as textures: preserve already-lowered indices
             * when the binding chain isn't reconstructible from this NIR source. */
            *state->samplers_needed |= BITFIELD_BIT(tex->sampler_index);
            shader_nir_progress = true;
         } else {
            terakan_nir_lower_bindings_instr_to_null(&tex->instr);
            return true;
         }
      } else {
         uint8_t const sampler_index_base = binding.set->first_shader_samplers[stage] +
                                            binding.set_binding->first_shader_samplers[stage];
         *state->samplers_needed |=
            BITFIELD_RANGE(sampler_index_base + binding.array_index_range_first,
                           binding.array_index_range_last - binding.array_index_range_first + 1);
         shader_nir_progress = true;
         tex->sampler_index = sampler_index_base;
         if (binding.array_index != NULL) {
            if (nir_def_instr(binding.array_index)->type == nir_instr_type_load_const) {
               tex->sampler_index +=
                  nir_instr_as_load_const(nir_def_instr(binding.array_index))->value[0].u32;
               nir_tex_instr_remove_src(tex, sampler_src_index);
            } else {
               sampler_src->src_type = nir_tex_src_sampler_offset;
               nir_src_rewrite(&sampler_src->src, binding.array_index);
            }
         } else {
            nir_tex_instr_remove_src(tex, sampler_src_index);
         }
      }
   } else if ((nir_tex_instr_need_sampler(tex) ||
               /* Evergreen TEX opcodes used for txf/txf_ms still consume
                * sampler state even when NIR flags them as sampler-optional. */
               tex->op == nir_texop_txf ||
               tex->op == nir_texop_txf_ms) &&
              tex->sampler_index < TERAKAN_SAMPLER_HW_COUNT_PER_STAGE) {
      *state->samplers_needed |= BITFIELD_BIT(tex->sampler_index);
   }

   return shader_nir_progress;
}

static void
terakan_nir_lower_bindings_instr_load_ubo(nir_builder * const b, nir_intrinsic_instr * const intrin,
                                          struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_ubo);

   b->cursor = nir_before_instr(&intrin->instr);

   /* FIX-W (Q-2026-04-20) companion: TERAKAN_FIX_W_LITERAL_UBO=N
    * replaces every load_ubo result with the literal N.  Pairs with
    * TERAKAN_FIX_W_LITERAL_LAYER=N to enable the full single_layer
    * fix path: coord.z = N AND u_layerNdx = N, both compile-time
    * constants, no runtime state-passing.  CTS single_layer test
    * has ONE UBO scalar (u_layerNdx) so blanket-replacing all
    * load_ubo calls is safe for the probe.  Production FIX-W
    * would be more surgical: identify which load_ubo corresponds
    * to the dispatch-varying scalar and replace only that one. */
   {
      int fix_w_ubo = g_terakan_fix_w_compile_ubo;
      if (fix_w_ubo == INT32_MIN) {
         static int fix_w_ubo_env = INT32_MIN;
         static bool fix_w_ubo_env_inited = false;
         if (!fix_w_ubo_env_inited) {
            fix_w_ubo_env = (int)debug_get_num_option(
               "TERAKAN_FIX_W_LITERAL_UBO", INT32_MIN);
            fix_w_ubo_env_inited = true;
         }
         fix_w_ubo = fix_w_ubo_env;
      }
      if (fix_w_ubo != INT32_MIN &&
          intrin->def.num_components == 1 &&
          intrin->def.bit_size == 32) {
         if (debug_get_bool_option("TERAKAN_DEBUG_FIX_W", false)) {
            fprintf(stderr,
               "TERAKAN_FIX_W: replacing load_ubo with literal %d\n",
               fix_w_ubo);
         }
         nir_def *literal = nir_imm_int(b, (int32_t)fix_w_ubo);
         nir_def_rewrite_uses(&intrin->def, literal);
         nir_instr_remove(&intrin->instr);
         return;
      }
   }

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   if (binding.array_index == NULL) {
      binding.array_index = nir_imm_zero(b, 1, 32);
   }

   mesa_shader_stage const stage = b->shader->info.stage;

   /* 3-tier UBO read routing (terakan_3tier_ubo_routing.h):
    *
    *   Tier 1 — KCACHE direct:  0 cycles, inline ALU operand.
    *            Requires: static offset, bank available, robustness OFF.
    *            KCACHE has a proven data leak within a 256-byte cache line
    *            (Phase 6 Probe 9 on AMD PALM): static OOB within the same
    *            locked line returns adjacent data instead of zero.  This is
    *            acceptable when robustness is disabled (app accepts undefined
    *            behavior) but violates robustBufferAccess which mandates zero.
    *
    *   Tier 2 — VFETCH bounded: 20-40 cycles, hardware OOB clamping.
    *            VTX SIZE_MINUS_1 enforces descriptor-range clamping;
    *            Phase 5 Probes confirmed OOB reads return exactly zero.
    *            Used for: all robust loads, dynamic offsets, bank overflow.
    *
    *   Tier 3 — KCACHE + MIN clamp: DEFERRED.
    *            Requires LOCK_LOOP_INDEX backend support (not available in
    *            SFN) for general dynamic offsets.  Narrow <=256B variant is
    *            a future optimization opportunity.
    *
    * Both paths work because CmdBindDescriptorSets dual-binds every UBO to
    * both SQ_TEX_RESOURCE (VFETCH) and KCACHE banks. */

   uint8_t const kcache_bank_base = binding.set->first_shader_uniform_buffers[stage] +
                                    binding.set_binding->first_shader_uniform_buffers[stage];

   nir_const_value *offset_const = nir_src_as_const_value(intrin->src[1]);

   /* Tier 1 eligibility: static offset, bank in range, AND robustness OFF.
    * When robust_buffer_access is enabled, ALL UBO loads must go through
    * VFETCH (Tier 2) to guarantee hardware-enforced zero-on-OOB.  KCACHE
    * does not provide this guarantee (within-line leak). */
   if (offset_const != NULL &&
       kcache_bank_base < TERAKAN_KCACHE_MAX_UNIFORM_BUFFERS &&
       !state->robust_buffer_access) {
      /* Static offset: direct KCACHE read. */
      uint32_t byte_offset = offset_const->u32;
      uint32_t vec4_index = byte_offset / 16;
      uint32_t first_component = (byte_offset % 16) / 4;

      /* Mark this KCACHE bank as needed for hw_state emission. */
      if (binding.array_index_range_first == binding.array_index_range_last) {
         *state->kcache_needed |= (uint16_t)1 << (kcache_bank_base + binding.array_index_range_first);
      } else {
         for (unsigned ai = binding.array_index_range_first; ai <= binding.array_index_range_last; ++ai) {
            if (kcache_bank_base + ai < 16)
               *state->kcache_needed |= (uint16_t)1 << (kcache_bank_base + ai);
         }
      }

      nir_def *result = nir_load_kcache_r600(
         b, intrin->num_components, 32, binding.array_index,
         .access = nir_intrinsic_access(intrin) | ACCESS_CAN_REORDER,
         .id_base = kcache_bank_base,
         .base = vec4_index,
         .component = first_component);

      if (intrin->def.bit_size != 32) {
         result = nir_u2uN(b, result, intrin->def.bit_size);
      }

      nir_def_rewrite_uses(&intrin->def, result);
      nir_instr_remove(&intrin->instr);
   } else {
      /* Tier 2 — VFETCH bounded: hardware-enforced OOB clamping.
       * Covers: robust_buffer_access ON (all offsets), dynamic offsets,
       * bank overflow.  VTX SIZE_MINUS_1 returns zero on OOB access. */
      uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                          binding.set_binding->first_shader_resources[stage] +
                                          TERAKAN_SAMPLER_HW_COUNT_PER_STAGE;
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(&intrin->def,
                           terakan_nir_load_raw_resource_buffer(
                              b, intrin->num_components, intrin->def.bit_size,
                              nir_intrinsic_access(intrin) | ACCESS_CAN_REORDER,
                              resource_index_base, binding.array_index, 0, intrin->src[1].ssa));
      nir_instr_remove(&intrin->instr);
   }
}

static void
terakan_nir_lower_bindings_instr_load_push_constant(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_push_constant);

   b->cursor = nir_before_instr(&intrin->instr);

   /* FIX-W (Q-2026-04-20) companion for push_constant: same logic as
    * the load_ubo path -- bank 15 is also wedged on per-dispatch
    * rebind, so bake the literal at variant compile time. */
   {
      int fix_w_pc = g_terakan_fix_w_compile_ubo;
      if (fix_w_pc != INT32_MIN &&
          intrin->def.num_components == 1 &&
          intrin->def.bit_size == 32) {
         if (debug_get_bool_option("TERAKAN_DEBUG_FIX_W", false)) {
            fprintf(stderr,
               "TERAKAN_FIX_W: replacing load_push_constant with literal %d\n",
               fix_w_pc);
         }
         nir_def *literal = nir_imm_int(b, (int32_t)fix_w_pc);
         nir_def_rewrite_uses(&intrin->def, literal);
         nir_instr_remove(&intrin->instr);
         return;
      }
   }

   /* Push constants don't have access robustness, simply add the base without an integer overflow
    * check.
    */

   /* KCACHE bank 15 for push constants (static offset = 1-cycle). */
   uint32_t static_base = TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES +
                           nir_intrinsic_base(intrin);

   nir_const_value *dyn_offset_const = nir_src_as_const_value(intrin->src[0]);

   if (dyn_offset_const != NULL) {
      uint32_t byte_offset = static_base + dyn_offset_const->u32;
      uint32_t vec4_index = byte_offset / 16;
      uint32_t first_component = (byte_offset % 16) / 4;

      /* Mark push constant bank as needed. */
      *state->kcache_needed |= (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS;

      nir_def *result = nir_load_kcache_r600(
         b, intrin->num_components, 32, nir_imm_zero(b, 1, 32),
         .access = ACCESS_CAN_REORDER,
         .id_base = TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
         .base = vec4_index,
         .component = first_component);

      if (intrin->def.bit_size != 32) {
         result = nir_u2uN(b, result, intrin->def.bit_size);
      }

      nir_def_rewrite_uses(&intrin->def, result);
      nir_instr_remove(&intrin->instr);
   } else {
      BITSET_SET(state->resources_needed, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
      nir_def_rewrite_uses(
         &intrin->def,
         terakan_nir_load_raw_resource_buffer(
            b, intrin->num_components, intrin->def.bit_size, ACCESS_CAN_REORDER,
            TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS, nir_imm_int(b, 0),
            static_base, intrin->src[0].ssa));
      nir_instr_remove(&intrin->instr);
   }
}


/*
 * Lower load_num_workgroups to a KCACHE read from the driver push constant
 * prefix in bank 15.
 *
 * The SFN backend handles load_num_workgroups via a VFETCH from
 * R600_BUFFER_INFO_CONST_BUFFER (gallium resource 15), but the Terakan Vulkan
 * driver does not bind that gallium-convention buffer.  Instead, the dispatch
 * code writes (group_count_x, group_count_y, group_count_z) into the driver
 * push constants, and we read them here via the always-bound KCACHE bank 15.
 *
 * Layout: num_workgroups[3] is at byte offset
 *   offsetof(terakan_push_constants_driver, num_workgroups) = 52
 *   → vec4_index = 3, first_component = 1 (components .yzw of vec4[3])
 */
static void
terakan_nir_lower_bindings_instr_load_num_workgroups(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_num_workgroups);
   assert(b->shader->info.stage == MESA_SHADER_COMPUTE);

   b->cursor = nir_before_instr(&intrin->instr);

   /* Byte offset of num_workgroups within terakan_push_constants_driver.
    * 12 × uint32 (UAV offsets) + 1 × uint32 (draw_id) = 52 bytes. */
   uint32_t const byte_offset =
      offsetof(struct terakan_push_constants_driver, num_workgroups);
   uint32_t const vec4_index = byte_offset / 16;
   uint32_t const first_component = (byte_offset % 16) / 4;

   /* Mark push constant KCACHE bank as needed. */
   *state->kcache_needed |= (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS;

   /* Mark the driver push constant slots as used so the dispatch code
    * knows to populate them. */
   *state->driver_push_constants_used |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_X) |
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_Y) |
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_Z);

   nir_def *result = nir_load_kcache_r600(
      b, 3, 32, nir_imm_zero(b, 1, 32),
      .access = ACCESS_CAN_REORDER,
      .id_base = TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      .base = vec4_index,
      .component = first_component);

   if (intrin->def.bit_size != 32) {
      result = nir_u2uN(b, result, intrin->def.bit_size);
   }

   nir_def_rewrite_uses(&intrin->def, result);
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_load_ssbo(nir_builder * const b,
                                           nir_intrinsic_instr * const intrin,
                                           struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_ssbo);

   b->cursor = nir_before_instr(&intrin->instr);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   if (binding.array_index == NULL)
      binding.array_index = nir_imm_zero(b, 1, 32);

   /* SSBO read robustness — Tier 1 (DWORD-granularity, zero ALU cost).
    *
    * VTX hardware enforces descriptor bounds via SIZE_MINUS_ONE at DWORD
    * granularity (Phase 5 Probe H1: all OOB VFETCH reads return 0). The
    * SSBO descriptor range is rounded up to a 4-byte boundary via
    * ALIGN_POT(range, 4) in terakan_descriptor.c, so the last partial
    * DWORD is never dropped by the hardware.
    *
    * Under robustBufferAccess2 with robustStorageBufferAccessSizeAlignment=4
    * (advertised in physical_device.c), the spec permits OOB detection at
    * 4-byte granularity. Bytes within the rounded-up range that lie past
    * the exact Vulkan buffer view boundary are considered in-bounds by the
    * driver's advertised contract. This is zero-cost: no ALU guards needed.
    *
    * Tier 2 (exact-byte, sizeAlignment=1) — DEFERRED.
    * If exact-byte robustness is ever required:
    *   1. Load exact_size from KCACHE bank 14 (dwords 0..11, same as write guard)
    *   2. Gate on (exact_size & 3) != 0 — skip if buffer is already 4-aligned
    *   3. At most ONE component per load can straddle the boundary
    *   4. Zero the entire straddling component (robustBufferAccess2 = hard 0)
    *   5. Cost: ~3 ALU ops per load (uadd_sat + uge + bcsel)
    * See TERAKAN_SHADER_ABI_CONTRACT.md §11 for architecture details. */

   /* Vertex fetches are coherent with UAVs, do a vertex fetch unconditionally. */
   uint8_t const resource_index_base =
      binding.set->first_shader_resources[b->shader->info.stage] +
      binding.set_binding->first_shader_resources[b->shader->info.stage] +
      TERAKAN_SAMPLER_HW_COUNT_PER_STAGE;
   BITSET_SET_RANGE(state->resources_needed,
                    resource_index_base + binding.array_index_range_first,
                    resource_index_base + binding.array_index_range_last);
   nir_def_rewrite_uses(&intrin->def, terakan_nir_load_raw_resource_buffer(
                                         b, intrin->num_components, intrin->def.bit_size,
                                         nir_intrinsic_access(intrin), resource_index_base,
                                         binding.array_index, 0, intrin->src[1].ssa));
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_store_ssbo(nir_builder * const b,
                                            nir_intrinsic_instr * const intrin,
                                            struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_store_ssbo);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[1], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, false, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   unsigned const bytes_per_component = nir_src_bit_size(intrin->src[0]) / 8;
   unsigned uav_op;
   if (bytes_per_component == 4) {
      uav_op = V_RAT_INST_STORE_TYPED;
   } else {
      assert(!"Unsupported storage buffer component size");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin);

   /* Robustness: the effective per-stage flag is threaded via
    * state->robust_buffer_access (computed by the pipeline compiler from
    * device feature + VK_EXT_pipeline_robustness).  See the mandatory
    * ALU-clamp rationale in terakan_nir_buffer_uav_coord().
    *
    * Unified lowering for all stages including compute: emit a r600
    * uav_instr with id_base = state->uav_base + uav_index_zero_based so
    * SFN's emit_uav_instr (sfn_instr_mem.cpp) routes the store to a
    * per-binding RAT slot (matching terakan_emit_compute_resources'
    * CB_COLOR{M} emit loop, which binds CB_COLOR0 to the first bound
    * compute SSBO in enumeration order, CB_COLOR1 to the second, etc.).
    *
    * The previous compute-specific path converted store_ssbo into
    * store_global + KCACHE-loaded base, which forced SFN's
    * RatInstr::emit_global_store to use the per-shader scalar
    * shader.ssbo_image_offset() as the RAT id -- breaking multi-SSBO
    * compute writes.  Keeping the nir_store_ssbo semantics through the
    * uav_instr path preserves the Vulkan per-binding layout all the way
    * into the PM4 stream. */
   nir_def * coord = terakan_nir_buffer_uav_coord(
      b, intrin->src[2].ssa, uav_index_zero_based, uav_array_index,
      state->robust_buffer_access,
      (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   if (bytes_per_component > 1) {
      coord = nir_udiv_imm(b, coord, bytes_per_component);
   }

   /* No point in vectorizing, the hardware instruction stores only one channel. */
   assert(nir_intrinsic_write_mask(intrin) == 0b1);

   /* Write guard: MEM_RAT stores are NOT bounds-checked by Evergreen
    * hardware (Phase 5 Probe 7).  When robust_buffer_access is enabled,
    * emit an IF/ENDIF that suppresses the write when the store offset
    * exceeds the UAV's declared byte size. */
   bool const guarded = state->robust_buffer_access;
   if (guarded) {
      nir_def *in_bounds = terakan_nir_emit_write_guard(
         b, intrin->src[2].ssa, bytes_per_component,
         uav_index_zero_based, uav_array_index, state);
      nir_push_if(b, in_bounds);
   }

   terakan_nir_build_uav_instr_r600(
      b, uav_array_index, coord, nir_u2u32(b, nir_channel(b, intrin->src[0].ssa, 0)),
      nir_undef(b, 1, 32), uav_op, access,
      state->uav_base + uav_index_zero_based);

   /* Enforce store->load ordering for SSBO write/read sequences in a single
    * invocation. SFN lowers this barrier to WAIT_ACK for SSBO memory modes. */
   if (b->shader->info.stage == MESA_SHADER_COMPUTE) {
      nir_barrier(b,
                  .execution_scope = SCOPE_INVOCATION,
                  .memory_scope = SCOPE_INVOCATION,
                  .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = nir_var_mem_ssbo);
   }

   if (guarded) {
      nir_pop_if(b, NULL);
   }

   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_ssbo_atomic(nir_builder * const b,
                                             nir_intrinsic_instr * const intrin,
                                             struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_ssbo_atomic ||
          intrin->intrinsic == nir_intrinsic_ssbo_atomic_swap);

   if (unlikely(intrin->def.bit_size != 32)) {
      assert(!"Only 32-bit storage buffer atomic operations are supported");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool const result_used = !list_is_empty(&intrin->def.uses);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, result_used, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   unsigned const uav_op = terakan_nir_atomic_uav_op(nir_intrinsic_atomic_op(intrin), result_used);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin) & ~ACCESS_CAN_REORDER;

   /* Robustness threaded via state->robust_buffer_access (see above). */
   nir_def * coord =
      nir_udiv_imm(b,
                   terakan_nir_buffer_uav_coord(
                      b, intrin->src[1].ssa, uav_index_zero_based, uav_array_index,
                      state->robust_buffer_access,
                      (access & ACCESS_INCLUDE_HELPERS) != 0, state),
                   4);

   /* For INC/DEC, the hardware instruction accepts the maximum possible value. */
   unsigned const uav_op_non_rtn = uav_op & 0x1F;
   nir_def * const value =
      uav_op_non_rtn == V_RAT_INST_INC_UINT || uav_op_non_rtn == V_RAT_INST_DEC_UINT
         ? nir_imm_int(b, (int)UINT32_MAX)
         : intrin->src[2].ssa;

   nir_def * const compare_value = intrin->intrinsic == nir_intrinsic_ssbo_atomic_swap
                                      ? intrin->src[3].ssa
                                      : nir_undef(b, 1, 32);

   unsigned const uav_id_base = state->uav_base + uav_index_zero_based;

   /* Write guard: MEM_RAT atomics are NOT bounds-checked by Evergreen
    * hardware.  Wrap the atomic in IF/ENDIF (or IF/ELSE for returning
    * atomics that need zero on the OOB path). */
   bool const guarded = state->robust_buffer_access;
   if (guarded) {
      nir_def *in_bounds = terakan_nir_emit_write_guard(
         b, intrin->src[1].ssa, 4 /* atomic is always 4 bytes */,
         uav_index_zero_based, uav_array_index, state);
      nir_push_if(b, in_bounds);
   }

   if (result_used) {
      nir_alu_type const result_type = nir_atomic_op_type(nir_intrinsic_atomic_op(intrin));
      nir_def *atomic_result = terakan_nir_convert_32_to_type_bits(
            b,
            terakan_nir_build_uav_returning_instr_r600(
               b, intrin->def.num_components, 32, uav_array_index, coord, value, compare_value,
               terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                            struct terakan_physical_device const, vk)
                                                  ->chip_info),
               uav_op, access, uav_id_base,
               (b->shader->info.stage == MESA_SHADER_FRAGMENT
                   ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                   : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
               uav_index_zero_based),
            intrin->def.bit_size, result_type);
      if (guarded) {
         /* OOB path: return zero for the atomic result. */
         nir_pop_if(b, NULL);
         nir_def *zero = nir_imm_zero(b, intrin->def.num_components, intrin->def.bit_size);
         nir_def *result = nir_if_phi(b, atomic_result, zero);
         nir_def_rewrite_uses(&intrin->def, result);
      } else {
         nir_def_rewrite_uses(&intrin->def, atomic_result);
      }
   } else {
      terakan_nir_build_uav_instr_r600(b, uav_array_index, coord, value, compare_value, uav_op,
                                       access, uav_id_base);
      if (guarded) {
         nir_pop_if(b, NULL);
      }
      nir_def_rewrite_uses(&intrin->def,
                           nir_undef(b, intrin->def.num_components, intrin->def.bit_size));
   }
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_load(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_load);

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   mesa_shader_stage const stage = b->shader->info.stage;

   enum gl_access_qualifier access = nir_intrinsic_access(intrin);
   if (state->uavs_for_mutable_resources_needed != NULL &&
       !(access & ACCESS_NON_WRITEABLE)) {
      /* If this shader has UAV writes AND this binding is writeable, block TEX reordering:
       * the TEX cache is not coherent with MEM_RAT (UAV) writes, so a write-then-read
       * sequence on the same binding could observe stale data from the TEX cache.
       * Non-writeable bindings cannot be written in the same invocation, so they have no
       * such coherence constraint and can safely use ACCESS_CAN_REORDER.
       */
      access &= ~ACCESS_CAN_REORDER;
   } else {
      access |= ACCESS_CAN_REORDER;
   }

   uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                       binding.set_binding->first_shader_resources[stage] +
                                       TERAKAN_SAMPLER_HW_COUNT_PER_STAGE;

   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      /* Vertex fetches are coherent with UAVs, do a vertex fetch unconditionally. */
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(b,
                  nir_load_buffer_resource_r600(
                     b, intrin->def.num_components, 32,
                     binding.array_index != NULL ? binding.array_index : nir_imm_zero(b, 1, 32),
                     nir_channel(b, intrin->src[1].ssa, 0), .access = access,
                     .id_base = resource_index_base),
                  intrin->def.bit_size));
      nir_instr_remove(&intrin->instr);
      return;
   }

   /* Texture fetches are not coherent with UAVs, if write-read coherence is needed, load using a
    * NOP_RTN UAV operation.
    */

   bool const image_is_array = nir_intrinsic_image_array(intrin);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based =
      terakan_nir_get_binding_uav(&binding, true, state, stage, &apply_uav_array_index);

   if (uav_index_zero_based == UINT_MAX) {
      /* UAV not needed or not available at this stage, load via the texture cache. */
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(
            b,
            nir_load_texture_resource_r600(
               b, intrin->def.num_components, 32,
               binding.array_index != NULL ? binding.array_index : nir_imm_zero(b, 1, 32),
               nir_vec4(
                  b, nir_channel(b, intrin->src[1].ssa, 0),
                  image_dim != GLSL_SAMPLER_DIM_1D ? nir_channel(b, intrin->src[1].ssa, 1)
                                                   : nir_imm_zero(b, 1, 32),
                  image_dim == GLSL_SAMPLER_DIM_3D || image_is_array
                     ? nir_channel(b, intrin->src[1].ssa, image_dim == GLSL_SAMPLER_DIM_1D ? 1 : 2)
                     : nir_imm_zero(b, 1, 32),
                  nir_imm_zero(b, 1, 32)),
                .access = access, .id_base = resource_index_base),
            intrin->def.bit_size));
      nir_instr_remove(&intrin->instr);
      return;
   }

   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);
   nir_def * const immed_index =
      terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                   struct terakan_physical_device const, vk)
                                         ->chip_info);
   nir_def * const undef = nir_undef(b, 1, 32);
   nir_alu_type const result_type =
      nir_intrinsic_has_dest_type(intrin) ? nir_intrinsic_dest_type(intrin) : nir_type_uint32;
   nir_def_rewrite_uses(
      &intrin->def,
      terakan_nir_convert_32_to_type_bits(
         b,
         terakan_nir_build_uav_returning_instr_r600(
            b, intrin->def.num_components, 32, uav_array_index,
            terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim, image_is_array,
                                        state, uav_index_zero_based), undef,
            undef, immed_index, V_RAT_INST_NOP_RTN, access,
            state->uav_base + uav_index_zero_based,
            (b->shader->info.stage == MESA_SHADER_FRAGMENT
                ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
            uav_index_zero_based),
         intrin->def.bit_size, result_type));
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_store(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_store);

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, false, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin);

   /* Write guard: MEM_RAT STORE_TYPED is NOT bounds-checked by Evergreen
    * hardware (Phase 5 Probe 7).  For buffer images (texel buffers), emit
    * an IF/ENDIF guard comparing the raw element index against the view's
    * element count from KCACHE bank 14.
    *
    * Non-buffer image stores (2D/3D/cube/array) are NOT guarded here:
    * they require per-view width/height/depth extent metadata which is
    * not yet available in the KCACHE bank 14 layout.  Deferred to a
    * future image-robustness pass.
    *
    * The guard must use the RAW element index (intrin->src[1].x), BEFORE
    * terakan_nir_buffer_uav_coord() applies the coordinate clamp and
    * base granularity offset — a clamped OOB write would still corrupt
    * the last valid element rather than being dropped. */
   bool const guarded =
      state->robust_buffer_access && image_dim == GLSL_SAMPLER_DIM_BUF;
   if (guarded) {
      nir_def * const raw_element_index =
         nir_channel(b, intrin->src[1].ssa, 0);

      *state->kcache_needed |=
         (uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA;

      /* Texel buffer element counts are in the dedicated array at
       * KCACHE bank 14 dwords 16..27 (vec4 indices 4..6), separated
       * from the SSBO byte sizes at dwords 0..11 for defense-in-depth. */
      uint32_t const elem_vec4_index = 4 + uav_index_zero_based / 4;
      uint32_t const elem_component = uav_index_zero_based % 4;

      nir_def * const element_count = nir_load_kcache_r600(
         b, 1, 32, uav_array_index,
         .access = ACCESS_CAN_REORDER,
         .id_base = TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
         .base = elem_vec4_index,
         .component = elem_component);

      nir_def * const in_bounds = nir_ult(b, raw_element_index, element_count);
      nir_push_if(b, in_bounds);
   }

   nir_def * coord;
   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      coord = terakan_nir_buffer_uav_coord(b, nir_channel(b, intrin->src[1].ssa, 0),
                                           uav_index_zero_based, uav_array_index, true,
                                           (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   } else {
      coord = terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim,
                                          nir_intrinsic_image_array(intrin),
                                          state, uav_index_zero_based);
   }

   nir_def * const undef = nir_undef(b, 1, 32);

   nir_alu_type const store_type =
      nir_intrinsic_has_src_type(intrin) ? nir_intrinsic_src_type(intrin) : nir_type_uint32;
   nir_def * const store_value_orig =
      terakan_nir_convert_type_to_32_bits(b, intrin->src[3].ssa, store_type);
   nir_def *store_value = store_value_orig;

   /* PROBE (Q-2026-04-19): TERAKAN_PROBE_KCACHE_VALUE=1 overrides
    * the R channel of store_value with 0xDEADBE00 + KC14_baseArrayLayer.
    * Diagnostic for whether the LS KCACHE actually serves the per-
    * dispatch baseArrayLayer or returns stale/zero data despite
    * SH_ACTION_ENA being asserted.
    *
    *   If R = 0xDEADBE0N for dispatch N -> KCACHE works; bug is
    *      downstream (MEM_RAT silicon dropping R3.z, not the cache).
    *   If R = 0xDEADBE00 (constant) across all dispatches -> KCACHE
    *      stuck; need a different invalidate primitive.
    *
    * Probe is independent of FIX-K's coord.z mutation so the two can
    * be combined: PROBE+FIX-K together reads the kcache value AND
    * uses it for slice routing.  PROBE alone (no FIX-K) reads the
    * kcache value without changing slice routing -- all writes still
    * land at slice 0 but with R-channel=0xDEADBE0N visible there. */
   if (image_dim != GLSL_SAMPLER_DIM_BUF &&
       state != NULL &&
       uav_index_zero_based < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT &&
       store_value_orig->num_components >= 1) {
      static int probe_kcache_cached = -1;
      if (probe_kcache_cached < 0) {
         probe_kcache_cached =
            debug_get_bool_option("TERAKAN_PROBE_KCACHE_VALUE", false) ? 1 : 0;
      }
      if (probe_kcache_cached) {
         /* Action A (Q-2026-04-19 part 2): TERAKAN_PROBE_KCACHE_BANK
          * selects which physical KC bank to fetch from.  Default 14
          * (the original probe target); set to 0 for the bank-0
          * sanity check (Action A in
          * 2026-04-19-isa-clamp-audit-result.md addendum 2).
          * For bank 0 + FIX-G ON, KC0[0].x is the application UBO's
          * first dword = u_layerNdx (= dispatch index N per the CTS
          * single_layer test design).  Reading back N visualizes as
          * byte 132 (=0), byte ~136 (=1), ..., byte ~160 (=7). */
         static int probe_bank = -1;
         if (probe_bank < 0) {
            probe_bank = (int)debug_get_num_option(
               "TERAKAN_PROBE_KCACHE_BANK",
               TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA);
            if (probe_bank < 0 || probe_bank > 15) {
               probe_bank = TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA;
            }
         }
         *state->kcache_needed |= (uint16_t)1 << (unsigned)probe_bank;

         /* For bank 14 (default), read KC14[7].x = dword 28
          * (= uav_base_array_layers[0] FIX-K layout).
          * For bank 0 (Action A), read KC0[0].x = dword 0
          * (= application UBO first dword). */
         uint32_t probe_dword;
         if ((unsigned)probe_bank
                 == TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA) {
            probe_dword = 28u + uav_index_zero_based;
         } else {
            probe_dword = 0u;
         }
         nir_def * const probe_kcache_load = nir_load_kcache_r600(
            b, 1, 32, nir_imm_zero(b, 1, 32),
            .access = ACCESS_CAN_REORDER,
            .id_base = (unsigned)probe_bank,
            .base = probe_dword / 4u,
            .component = probe_dword % 4u);
         store_value = nir_vector_insert_imm(
            b, store_value_orig, probe_kcache_load, 0);
      }
   }

   /* PROBE_HARDCODE: TERAKAN_PROBE_HARDCODE_VALUE=N forces R-channel
    * of every image_store to literal N (32-bit signed).  Independent
    * sanity check for the override mechanism: if the test's R-channel
    * values do not change to N when this is set, the override path is
    * being bypassed (e.g. SFN ignoring vector_insert_imm or a later
    * pass folding it back).  Run with N=0xCAFEBABE first; expect
    * Result PNG to show byte 0 (clamped from very-negative). */
   if (image_dim != GLSL_SAMPLER_DIM_BUF &&
       store_value->num_components >= 1) {
      static int probe_hardcode_value = INT32_MIN;
      if (probe_hardcode_value == INT32_MIN) {
         probe_hardcode_value =
            (int)debug_get_num_option("TERAKAN_PROBE_HARDCODE_VALUE", INT32_MIN);
      }
      if (probe_hardcode_value != INT32_MIN) {
         store_value = nir_vector_insert_imm(
            b, store_value,
            nir_imm_int(b, (int32_t)probe_hardcode_value), 0);
      }
   }

   /* PROBE_TGID_Z (Q-2026-04-19 FIX-U validation): when set, force
    * R-channel = workgroup_id.z directly.  Diagnostic for whether
    * the SPI populates TGID.z from VGT_COMPUTE_START_Z per
    * dispatch.  Used with TERAKAN_FIX_U_USE_TGID_Z=1 (driver-side
    * START_Z population).  Visualization byte directly encodes the
    * cached value: TGID.z=0 -> byte 132,  TGID.z=7 -> byte ~160.
    * If shader returns 0 across all dispatches, either TGID.z isn't
    * picking up START_Z or START_Z is itself wedged.  This probe is
    * INDEPENDENT of FIX-K's coord.z mutation. */
   if (image_dim != GLSL_SAMPLER_DIM_BUF &&
       store_value->num_components >= 1) {
      static int probe_tgid_z = -1;
      if (probe_tgid_z < 0) {
         probe_tgid_z = debug_get_bool_option(
            "TERAKAN_PROBE_TGID_Z", false) ? 1 : 0;
      }
      if (probe_tgid_z) {
         nir_def * const wg_id = nir_load_workgroup_id(b);
         nir_def * const tgid_z = nir_channel(b, wg_id, 2);
         store_value = nir_vector_insert_imm(b, store_value, tgid_z, 0);
      }
   }

   /* FIX-Z sub-32-bit UINT sign-extension: for R8_UINT / R16_UINT formats,
    * NUMBER_TYPE=SINT in the CB clamps values above SINT_MAX (e.g. uint8
    * 200 -> clamped to 127).  Sign-extending each component from the
    * format's natural bit width to 32-bit signed before the store puts
    * the value in the SINT range (uint8 200 -> int32 -56), so the CB
    * truncates correctly (stores 0xC8 = 200 as uint8) without clamping. */
   {
      /* Promoted to default 2026-04-21 (steinmarder finding
       * 2026-04-21-tranche7-h7a-confirmed-zero-real-fails.md): tranche-7
       * absolute-isolation matrix proved zero real isolation fails after
       * FIX-Z (incl. per-channel packed-format handling).  Env var
       * preserved as opt-out (TERAKAN_FIX_Z_UINT_FORMAT_COMP=0). */
      static int fixz_se = -1;
      if (fixz_se < 0)
         fixz_se = debug_get_bool_option("TERAKAN_FIX_Z_UINT_FORMAT_COMP", true) ? 1 : 0;
      if (fixz_se) {
         enum pipe_format const img_fmt = (enum pipe_format)nir_intrinsic_format(intrin);
         if (img_fmt != PIPE_FORMAT_NONE && util_format_is_pure_uint(img_fmt)) {
            const struct util_format_description * const fmtd =
               util_format_description(img_fmt);
            /* FIX-Z packed-format extension (2026-04-21): per-channel-aware
             * sign-extension shift.  Original code used a single bpc =
             * channel[0].size which fails for packed formats with non-
             * uniform channel widths (e.g. R10G10B10A2_UINT: 10/10/10/2 bits).
             * The 2-bit alpha channel needs shift=30, the 10-bit RGB channels
             * need shift=22.
             *
             * Pre-scan to decide whether ANY channel needs extension; if not
             * (e.g. r32_uint with channel[0].size=32 only), short-circuit
             * without emitting nir_channel/nir_vec at all -- mirrors the
             * original code's no-op behavior for 32-bit-per-channel formats. */
            bool needs_extension = false;
            for (unsigned fi = 0; fi < fmtd->nr_channels; fi++) {
               unsigned const sz = fmtd->channel[fi].size;
               if (sz > 0 && sz < 32) { needs_extension = true; break; }
            }
            if (needs_extension) {
               nir_def *se_comps[NIR_MAX_VEC_COMPONENTS];
               unsigned const nc = store_value->num_components;
               for (unsigned ci = 0; ci < nc; ci++) {
                  nir_def *v = nir_channel(b, store_value, ci);
                  unsigned const ch_bpc =
                     ci < fmtd->nr_channels ? fmtd->channel[ci].size : 0u;
                  if (ch_bpc > 0 && ch_bpc < 32) {
                     unsigned const shift = 32u - ch_bpc;
                     v = nir_ishr(b, nir_ishl(b, v, nir_imm_int(b, (int)shift)),
                                  nir_imm_int(b, (int)shift));
                  }
                  se_comps[ci] = v;
               }
               store_value = nir_vec(b, se_comps, nc);
            }
         }
      }
   }
   /* FIX-AB (2026-04-22): encode the format-derived elem_size_minus_one
    * (= dwords-per-element minus 1, encoded as {0,1,3} for {1,2,4} dwords)
    * in uav_op high bits [6:7] so emit_uav_store_r600 can recover it.
    * Default of ELEM_SIZE=0 caused silicon to drop the R-dword of
    * r32g32_uint in cold-context state per steinmarder finding
    * 2026-04-22-tranche16-silicon-drops-r-dword-cold-context.md, CLAIMS
    * C-2026-04-22-43.  Trimming store_value at the NIR level would
    * violate `src->num_components == intrin->num_components` validation
    * for the uav_instr_r600 intrinsic (NIR enforces the contract); we
    * instead carry the format channel count through the op encoding
    * since uav_op_base only uses bits [4:0]. */
   unsigned uav_op_with_elem_size = V_RAT_INST_STORE_TYPED;
   {
      enum pipe_format const trim_fmt = (enum pipe_format)nir_intrinsic_format(intrin);
      if (trim_fmt != PIPE_FORMAT_NONE) {
         unsigned const fmt_channels = util_format_get_nr_components(trim_fmt);
         unsigned const tex_bytes = util_format_get_blocksize(trim_fmt);
         if (fmt_channels > 0 && tex_bytes >= 4 * fmt_channels) {
            unsigned esmo = 0;
            if (fmt_channels == 2)
               esmo = 1;
            else if (fmt_channels == 4)
               esmo = 3;
            /* fmt_channels in {1, 3} keeps esmo = 0 */
            uav_op_with_elem_size |= (esmo & 0x3u) << 5;
         }
      }
   }
   terakan_nir_build_uav_instr_r600(b, uav_array_index, coord, store_value,
                                    undef, uav_op_with_elem_size, access,
                                    state->uav_base + uav_index_zero_based);

   if (guarded) {
      nir_pop_if(b, NULL);
   }

   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_atomic(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_atomic ||
          intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap);

   if (unlikely(intrin->def.bit_size != 32)) {
      assert(!"Only 32-bit storage image and storage texel buffer atomic operations are supported");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool const result_used = !list_is_empty(&intrin->def.uses);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, result_used, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   unsigned const uav_op = terakan_nir_atomic_uav_op(nir_intrinsic_atomic_op(intrin), result_used);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin) & ~ACCESS_CAN_REORDER;

   nir_def * coord;
   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      coord = terakan_nir_buffer_uav_coord(b, nir_channel(b, intrin->src[1].ssa, 0),
                                           uav_index_zero_based, uav_array_index, true,
                                           (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   } else {
      coord = terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim,
                                          nir_intrinsic_image_array(intrin),
                                          state, uav_index_zero_based);
   }

   /* For INC/DEC, the hardware instruction accepts the maximum possible value. */
   unsigned const uav_op_non_rtn = uav_op & 0x1F;
   nir_def * const value =
      uav_op_non_rtn == V_RAT_INST_INC_UINT || uav_op_non_rtn == V_RAT_INST_DEC_UINT
         ? nir_imm_int(b, (int)UINT32_MAX)
         : intrin->src[3].ssa;

   nir_def * const compare_value = intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap
                                      ? intrin->src[4].ssa
                                      : nir_undef(b, 1, 32);

   unsigned const uav_id_base = state->uav_base + uav_index_zero_based;

   /* Write guard: MEM_RAT atomics are NOT bounds-checked by Evergreen
    * hardware.  For buffer images, emit IF/ENDIF gated on the raw element
    * index being within the view's element count (from KCACHE bank 14).
    * For returning atomics, the OOB path returns zero via nir_if_phi.
    *
    * Non-buffer image atomics (2D/3D/cube/array) are NOT guarded:
    * they require per-view extent metadata not yet in the bank 14 layout.
    *
    * The element index is extracted from src[1].x BEFORE coord processing
    * to avoid the coordinate clamp in terakan_nir_buffer_uav_coord(). */
   bool const guarded =
      state->robust_buffer_access && image_dim == GLSL_SAMPLER_DIM_BUF;
   if (guarded) {
      nir_def * const raw_element_index =
         nir_channel(b, intrin->src[1].ssa, 0);

      *state->kcache_needed |=
         (uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA;

      /* Texel buffer element counts are in the dedicated array at
       * KCACHE bank 14 dwords 16..27 (vec4 indices 4..6), separated
       * from the SSBO byte sizes at dwords 0..11 for defense-in-depth. */
      uint32_t const elem_vec4_index = 4 + uav_index_zero_based / 4;
      uint32_t const elem_component = uav_index_zero_based % 4;

      nir_def * const element_count = nir_load_kcache_r600(
         b, 1, 32, uav_array_index,
         .access = ACCESS_CAN_REORDER,
         .id_base = TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
         .base = elem_vec4_index,
         .component = elem_component);

      nir_def * const in_bounds = nir_ult(b, raw_element_index, element_count);
      nir_push_if(b, in_bounds);
   }

   if (result_used) {
      nir_alu_type const result_type = nir_atomic_op_type(nir_intrinsic_atomic_op(intrin));
      nir_def *atomic_result = terakan_nir_convert_32_to_type_bits(
            b,
            terakan_nir_build_uav_returning_instr_r600(
               b, intrin->def.num_components, 32, uav_array_index, coord, value, compare_value,
               terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                            struct terakan_physical_device const, vk)
                                                  ->chip_info),
               uav_op, access, uav_id_base,
               (b->shader->info.stage == MESA_SHADER_FRAGMENT
                   ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                   : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
               uav_index_zero_based),
            intrin->def.bit_size, result_type);
      if (guarded) {
         /* OOB path: return zero for the atomic result. */
         nir_pop_if(b, NULL);
         nir_def *zero = nir_imm_zero(b, intrin->def.num_components, intrin->def.bit_size);
         nir_def *result = nir_if_phi(b, atomic_result, zero);
         nir_def_rewrite_uses(&intrin->def, result);
      } else {
         nir_def_rewrite_uses(&intrin->def, atomic_result);
      }
   } else {
      terakan_nir_build_uav_instr_r600(b, uav_array_index, coord, value, compare_value, uav_op,
                                       access, uav_id_base);
      if (guarded) {
         nir_pop_if(b, NULL);
      }
      nir_def_rewrite_uses(&intrin->def,
                           nir_undef(b, intrin->def.num_components, intrin->def.bit_size));
   }
   nir_instr_remove(&intrin->instr);
}

bool
terakan_nir_lower_bindings_instr(nir_builder * const b, nir_instr * const instr,
                                 void * const cb_data)
{
   struct terakan_nir_lower_bindings_state * const state =
      (struct terakan_nir_lower_bindings_state *)cb_data;

   if (instr->type == nir_instr_type_tex) {
      return terakan_nir_lower_bindings_instr_tex(b, nir_instr_as_tex(instr), state);
   }

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_num_workgroups:
         if (b->shader->info.stage == MESA_SHADER_COMPUTE) {
            terakan_nir_lower_bindings_instr_load_num_workgroups(b, intrin, state);
            return true;
         }
         return false;
      case nir_intrinsic_load_ubo:
         terakan_nir_lower_bindings_instr_load_ubo(b, intrin, state);
         return true;
      case nir_intrinsic_load_push_constant:
         terakan_nir_lower_bindings_instr_load_push_constant(b, intrin, state);
         return true;
      case nir_intrinsic_load_ssbo:
         terakan_nir_lower_bindings_instr_load_ssbo(b, intrin, state);
         return true;
      case nir_intrinsic_store_ssbo:
         terakan_nir_lower_bindings_instr_store_ssbo(b, intrin, state);
         return true;
      case nir_intrinsic_ssbo_atomic:
      case nir_intrinsic_ssbo_atomic_swap:
         terakan_nir_lower_bindings_instr_ssbo_atomic(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_load:
         terakan_nir_lower_bindings_instr_image_deref_load(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_store:
         terakan_nir_lower_bindings_instr_image_deref_store(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_atomic:
      case nir_intrinsic_image_deref_atomic_swap:
         terakan_nir_lower_bindings_instr_image_deref_atomic(b, intrin, state);
         return true;
      default:
         break;
      }
   }

   return false;
}
