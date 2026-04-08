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
 * terakan_nir_lower_bindings_internal.h — Shared types for the NIR binding
 * lowering split.  Private to the nir/ directory; not part of the public API.
 *
 * Phase 0 mechanical file split.  See TERAKAN_SHADER_ABI_CONTRACT.md.
 */

#pragma once

#include "terakan_descriptor.h"
#include "terakan_descriptor_set_layout.h"
#include "terakan_nir.h"
#include "terakan_physical_device.h"
#include "terakan_pipeline_layout.h"
#include "terakan_push_constants.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "util/bitscan.h"
#include "util/list.h"
#include "util/macros.h"
#include "nir_builder.h"
#include "vk_device.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- Shared data structures --- */

struct terakan_nir_binding {
   struct terakan_pipeline_layout_set const * set;
   struct terakan_descriptor_set_layout_binding const * set_binding;

   /* NULL if not provided or zero. */
   nir_def * array_index;

   unsigned array_index_range_first;
   unsigned array_index_range_last;
};

struct terakan_nir_lower_bindings_state {
   struct terakan_pipeline_layout const * layout;

   /* 0-based. */
   BITSET_WORD * resources_needed;
   uint32_t * samplers_needed;

   unsigned uav_base;
   /* TERAKAN_RESOURCE_RANGE_MUTABLE_BASE-based, NULL if the stage doesn't support UAVs. */
   BITSET_WORD * uavs_for_mutable_resources_needed;

   uint32_t * driver_push_constants_used;

   /* Bitmask of KCACHE banks (0-15) referenced by this shader. */
   uint16_t * kcache_needed;

   /* Effective robust-buffer-access flag for this stage.  Computed by the
    * caller from (device feature robustBufferAccess) OR any per-pipeline
    * VK_EXT_pipeline_robustness state.  When true, terakan_nir_buffer_uav_coord
    * injects a nir_umin_imm clamp before the UAV coordinate.  This is a
    * mandatory software fallback — Terascale silicon does not provide
    * reliable native OOB handling, so this flag must never be replaced by
    * silicon-trust heuristics. */
   bool robust_buffer_access;
};


/* --- Functions exported from terakan_nir_apply_pipeline_layout.c --- */

/* Descriptor chain normalization (called by orchestrator). */
bool terakan_nir_lower_load_vulkan_descriptor_filter(nir_instr const *instr,
                                                     void const *cb_data);
nir_def *terakan_nir_lower_load_vulkan_descriptor_impl(nir_builder *b,
                                                       nir_instr *instr,
                                                       void *cb_data);
bool terakan_nir_lower_vulkan_resource_reindex_instr(nir_builder *b,
                                                     nir_instr *instr,
                                                     void *cb_data);
bool terakan_nir_zero_vulkan_resource_offset_filter(nir_instr const *instr,
                                                    void const *cb_data);
nir_def *terakan_nir_zero_vulkan_resource_offset_impl(nir_builder *b,
                                                      nir_instr *instr,
                                                      void *cb_data);

/* UAV scanning (called by orchestrator). */
void terakan_nir_gather_uavs_needed(nir_shader *shader,
                                    struct terakan_nir_lower_bindings_state *state);

/* Binding resolution utilities (called by lower_abi handlers). */
bool terakan_nir_get_binding(nir_src src, VkDescriptorType expected_type,
                             struct terakan_pipeline_layout const *layout,
                             nir_shader *shader,
                             struct terakan_nir_binding *binding_out);

unsigned terakan_nir_get_binding_uav(
   struct terakan_nir_binding const *binding,
   bool immed_needed,
   struct terakan_nir_lower_bindings_state const *state,
   mesa_shader_stage stage,
   bool *apply_array_index_out);

unsigned terakan_nir_atomic_uav_op(nir_atomic_op atomic_op, bool result_used);

VkDescriptorType terakan_nir_image_descriptor_type(enum glsl_sampler_dim dim);

/* --- Functions exported from terakan_nir_lower_abi.c --- */

/* Per-instruction lowering dispatch (called by orchestrator). */
bool terakan_nir_lower_bindings_instr(nir_builder *b, nir_instr *instr,
                                      void *cb_data);
