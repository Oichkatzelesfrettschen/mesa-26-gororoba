/*
 * Copyright © 2026 steinmarder project
 * SPDX-License-Identifier: MIT
 *
 * Pipeline cache for Terakan (TeraScale-2 / Evergreen).
 *
 * Implements BLAKE3-based cache key construction and vk_pipeline_cache_object
 * integration for caching compiled R600 shader binaries.  Eliminates
 * redundant SPIR-V → NIR → R600 VLIW5 compilation for identical pipelines.
 *
 * Cache key composition:
 *   BLAKE3( build_id || pci_device_id || stage_key || r600_shader_key || spirv_hash )
 *
 * The PCI device ID and build ID are included to prevent catastrophic cache
 * collisions across driver versions or hardware variants.
 *
 * What is cached:
 *   - Program bytecode (raw instruction words uploaded to GPU BO)
 *   - SQ_PGM_RESOURCES register values
 *   - Stage-specific register state (SPI_VS_OUT_*, SPI_PS_INPUT_*, etc.)
 *   - scratch_item_size_dwords
 *   - db_shader_control (FS only, set by SFN compiler)
 *
 * What is NOT cached (re-derived from NIR on cache hit):
 *   - resources_needed, samplers_needed, kcache_needed (from post-link lowering)
 *   - push_constants_usage (from post-link lowering + pipeline layout)
 *   - fragment_data_uncompacted_locations (from post-link lowering)
 *   - vertex_attributes_needed (from NIR analysis)
 *   - uavs_for_mutable_resources_needed (from post-link lowering)
 *
 * These fields are filled by terakan_shader_lower_and_optimize_post_link()
 * which runs BEFORE the cache lookup, so they are available on cache hit.
 */

#ifndef TERAKAN_PIPELINE_CACHE_H
#define TERAKAN_PIPELINE_CACHE_H

#include "terakan_pipeline_key.h"
#include "terakan_shader.h"

#include "util/mesa-blake3.h"
#include "vk_pipeline_cache.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_device;

/*
 * Cached shader binary — wraps vk_pipeline_cache_object.
 *
 * Contains ONLY flat, pointer-free compilation output.
 * The r600_shader struct (which has pointers/linked lists) is NOT stored.
 */
struct terakan_cached_shader {
   struct vk_pipeline_cache_object base;

   /* Embedded key storage — base.key_data points here */
   blake3_hash key_storage;

   /* Serialized shader program (raw instruction words) */
   uint32_t *program_data;
   size_t program_size_bytes;

   /* Register state from compilation */
   uint32_t sq_pgm_resources[2];
   uint32_t scratch_item_size_dwords;
   mesa_shader_stage stage;

   /* Stage-specific register state (flat, no pointers) */
   union {
      struct {
         uint32_t spi_vs_out_id[10];
         uint32_t spi_vs_out_config;
         uint32_t pa_cl_vs_out_cntl;
      } vs;

      struct {
         uint32_t sq_pgm_exports_ps;
         uint32_t spi_ps_input_cntl[32];
         uint32_t spi_ps_in_control[2];
         uint32_t spi_input_z;
         uint32_t spi_baryc_cntl;
         uint32_t cb_shader_mask;
         uint32_t db_shader_control;
      } ps;
   } regs;
};

/* Pipeline cache object ops for serialization/deserialization */
extern const struct vk_pipeline_cache_object_ops terakan_cached_shader_ops;

/*
 * Compute BLAKE3 hash for a shader stage cache lookup.
 *
 * Combines: build ID, PCI device ID, per-stage key, r600_shader_key,
 * SPIR-V blake3 hash, and optional cross-stage postprocess context
 * into a single 32-byte cache key.
 *
 * postprocess_ctx / postprocess_ctx_size: optional cross-stage context
 * that affects codegen but is not captured in r600_shader_key.  For VS,
 * this includes fs_inputs_read (varying pruning) and remove_point_size.
 * Pass NULL/0 when no cross-stage context applies.
 */
void
terakan_pipeline_cache_hash_shader(blake3_hash hash_out,
                                   struct terakan_device const *device,
                                   struct terakan_shader_stage_key const *stage_key,
                                   union r600_shader_key const *shader_key,
                                   blake3_hash const spirv_hash,
                                   void const *postprocess_ctx,
                                   size_t postprocess_ctx_size);

/*
 * Look up a cached shader binary in the pipeline cache.
 *
 * Returns a ref'd terakan_cached_shader on cache hit, NULL on miss.
 * The caller must unref via vk_pipeline_cache_object_unref when done.
 *
 * cache may be NULL (no-op, returns NULL).
 */
struct terakan_cached_shader *
terakan_pipeline_cache_lookup(struct vk_pipeline_cache *cache,
                              blake3_hash const key);

/*
 * Insert a compiled shader into the pipeline cache.
 *
 * Serializes the terakan_shader_impl compile outputs into a cache object
 * and adds it.  The shader's BO must be CPU-mapped.
 *
 * cache may be NULL (no-op, returns VK_SUCCESS).
 */
VkResult
terakan_pipeline_cache_insert(struct vk_pipeline_cache *cache,
                              blake3_hash const key,
                              struct terakan_shader_impl const *shader,
                              mesa_shader_stage stage,
                              size_t program_size_bytes,
                              struct terakan_device *device);

/*
 * Restore a terakan_shader_impl from a cached shader object.
 *
 * Allocates a new BO, copies the cached bytecode into it, and fills
 * the shader static_state and register values from the cache.
 *
 * Pre-compile fields (resources_needed, samplers, push_constants, etc.)
 * are NOT restored — they must already be filled by the caller via
 * terakan_shader_lower_and_optimize_post_link() before this call.
 */
VkResult
terakan_cached_shader_restore(struct terakan_cached_shader const *cached,
                              struct terakan_shader_impl *shader,
                              struct terakan_device *device,
                              VkAllocationCallbacks const *allocator);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PIPELINE_CACHE_H */
