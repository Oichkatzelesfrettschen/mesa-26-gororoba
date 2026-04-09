/*
 * Copyright © 2026 steinmarder project
 * SPDX-License-Identifier: MIT
 *
 * Pipeline cache implementation for Terakan (TeraScale-2 / Evergreen).
 */

#include "terakan_pipeline_cache.h"

#include "terakan_bo.h"
#include "terakan_device.h"
#include "terakan_physical_device.h"

#include "util/blob.h"
#include "util/disk_cache.h"
#include "util/mesa-blake3.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <stdlib.h>
#include <string.h>

/* --- Serialization format ---
 *
 * All values host-endian (caches are device-local, not portable).
 *
 * Layout:
 *   terakan_cached_shader_blob_header
 *   uint8_t program_data[program_size_bytes]
 *   uint8_t stage_regs[regs_size]
 */

#define TERAKAN_CACHE_BLOB_VERSION 1

struct terakan_cached_shader_blob_header {
   uint32_t version;
   uint32_t stage;
   uint32_t program_size_bytes;
   uint32_t sq_pgm_resources[2];
   uint32_t scratch_item_size_dwords;
   uint32_t regs_size;
};

/* --- vk_pipeline_cache_object ops --- */

static bool
terakan_cached_shader_serialize(struct vk_pipeline_cache_object *object,
                                struct blob *blob)
{
   struct terakan_cached_shader *cached =
      container_of(object, struct terakan_cached_shader, base);

   uint32_t regs_size;
   const void *regs_data;
   switch (cached->stage) {
   case MESA_SHADER_VERTEX:
      regs_size = sizeof(cached->regs.vs);
      regs_data = &cached->regs.vs;
      break;
   case MESA_SHADER_FRAGMENT:
      regs_size = sizeof(cached->regs.ps);
      regs_data = &cached->regs.ps;
      break;
   default:
      regs_size = 0;
      regs_data = NULL;
      break;
   }

   struct terakan_cached_shader_blob_header hdr = {
      .version = TERAKAN_CACHE_BLOB_VERSION,
      .stage = (uint32_t)cached->stage,
      .program_size_bytes = (uint32_t)cached->program_size_bytes,
      .sq_pgm_resources = { cached->sq_pgm_resources[0], cached->sq_pgm_resources[1] },
      .scratch_item_size_dwords = cached->scratch_item_size_dwords,
      .regs_size = regs_size,
   };

   blob_write_bytes(blob, &hdr, sizeof(hdr));

   if (cached->program_size_bytes > 0 && cached->program_data != NULL)
      blob_write_bytes(blob, cached->program_data, cached->program_size_bytes);

   if (regs_size > 0 && regs_data != NULL)
      blob_write_bytes(blob, regs_data, regs_size);

   return !blob->out_of_memory;
}

static struct vk_pipeline_cache_object *
terakan_cached_shader_deserialize(struct vk_pipeline_cache *cache,
                                  const void *key_data, size_t key_size,
                                  struct blob_reader *blob)
{
   struct terakan_cached_shader_blob_header hdr;
   blob_copy_bytes(blob, &hdr, sizeof(hdr));

   if (blob->overrun || hdr.version != TERAKAN_CACHE_BLOB_VERSION)
      return NULL;

   /* Validate stage */
   if (hdr.stage != MESA_SHADER_VERTEX && hdr.stage != MESA_SHADER_FRAGMENT &&
       hdr.stage != MESA_SHADER_COMPUTE)
      return NULL;

   /* Validate sizes — reject absurdly large or non-dword-aligned blobs */
   if (hdr.program_size_bytes > 16 * 1024 * 1024 ||
       (hdr.program_size_bytes > 0 && hdr.program_size_bytes % 4 != 0))
      return NULL;

   uint32_t expected_regs_size;
   switch (hdr.stage) {
   case MESA_SHADER_VERTEX:   expected_regs_size = sizeof(((struct terakan_cached_shader *)0)->regs.vs); break;
   case MESA_SHADER_FRAGMENT: expected_regs_size = sizeof(((struct terakan_cached_shader *)0)->regs.ps); break;
   default:                   expected_regs_size = 0; break;
   }
   if (hdr.regs_size != expected_regs_size)
      return NULL;

   struct terakan_cached_shader *cached = calloc(1, sizeof(*cached));
   if (cached == NULL)
      return NULL;

   /* Copy key into embedded storage and init with pointer to it */
   if (key_size <= sizeof(cached->key_storage))
      memcpy(cached->key_storage, key_data, key_size);

   vk_pipeline_cache_object_init(cache->base.device, &cached->base,
                                 &terakan_cached_shader_ops,
                                 cached->key_storage, key_size);

   cached->stage = (mesa_shader_stage)hdr.stage;
   cached->program_size_bytes = hdr.program_size_bytes;
   cached->sq_pgm_resources[0] = hdr.sq_pgm_resources[0];
   cached->sq_pgm_resources[1] = hdr.sq_pgm_resources[1];
   cached->scratch_item_size_dwords = hdr.scratch_item_size_dwords;

   if (hdr.program_size_bytes > 0) {
      cached->program_data = malloc(hdr.program_size_bytes);
      if (cached->program_data == NULL) {
         vk_pipeline_cache_object_finish(&cached->base);
         free(cached);
         return NULL;
      }
      blob_copy_bytes(blob, cached->program_data, hdr.program_size_bytes);
   }

   if (hdr.regs_size > 0) {
      void *regs_dest;
      switch (hdr.stage) {
      case MESA_SHADER_VERTEX:   regs_dest = &cached->regs.vs; break;
      case MESA_SHADER_FRAGMENT: regs_dest = &cached->regs.ps; break;
      default:                   regs_dest = NULL; break;
      }
      if (regs_dest != NULL)
         blob_copy_bytes(blob, regs_dest, hdr.regs_size);
   }

   if (blob->overrun) {
      free(cached->program_data);
      vk_pipeline_cache_object_finish(&cached->base);
      free(cached);
      return NULL;
   }

   return &cached->base;
}

static void
terakan_cached_shader_destroy(struct vk_device *device,
                              struct vk_pipeline_cache_object *object)
{
   struct terakan_cached_shader *cached =
      container_of(object, struct terakan_cached_shader, base);

   free(cached->program_data);
   vk_pipeline_cache_object_finish(object);
   free(cached);
}

const struct vk_pipeline_cache_object_ops terakan_cached_shader_ops = {
   .serialize = terakan_cached_shader_serialize,
   .deserialize = terakan_cached_shader_deserialize,
   .destroy = terakan_cached_shader_destroy,
};

/* --- BLAKE3 cache key construction --- */

void
terakan_pipeline_cache_hash_shader(blake3_hash hash_out,
                                   struct terakan_device const *device,
                                   struct terakan_shader_stage_key const *stage_key,
                                   union r600_shader_key const *shader_key,
                                   blake3_hash const spirv_hash,
                                   void const *postprocess_ctx,
                                   size_t postprocess_ctx_size)
{
   struct terakan_physical_device const *pdev =
      terakan_device_physical_device(device);

   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);

   /* Driver build identity — prevents collisions across driver versions */
   disk_cache_get_function_identifier(terakan_pipeline_cache_hash_shader, &ctx);

   /* Physical device PCI ID — prevents collisions across GPU variants */
   _mesa_blake3_update(&ctx, &pdev->chip_info.pci_device_id,
                       sizeof(pdev->chip_info.pci_device_id));

   /* Per-stage compilation flags */
   _mesa_blake3_update(&ctx, stage_key, sizeof(*stage_key));

   /* R600 backend shader key (nr_cbufs, dual_source_blend, etc.) */
   _mesa_blake3_update(&ctx, shader_key, sizeof(*shader_key));

   /* SPIR-V content hash */
   _mesa_blake3_update(&ctx, spirv_hash, sizeof(blake3_hash));

   /* Optional cross-stage postprocess context.  For VS, this includes
    * FS inputs_read and remove_point_size — codegen-affecting decisions
    * that are not captured in the shared r600_shader_key.  Including them
    * here ensures that pruned and unpruned shader variants produce
    * distinct cache keys without mutating the shared Gallium key union. */
   if (postprocess_ctx != NULL && postprocess_ctx_size > 0)
      _mesa_blake3_update(&ctx, postprocess_ctx, postprocess_ctx_size);

   _mesa_blake3_final(&ctx, hash_out);
}

/* --- Cache lookup / insert --- */

struct terakan_cached_shader *
terakan_pipeline_cache_lookup(struct vk_pipeline_cache *cache,
                              blake3_hash const key)
{
   if (cache == NULL)
      return NULL;

   struct vk_pipeline_cache_object *object =
      vk_pipeline_cache_lookup_object(cache, key, sizeof(blake3_hash),
                                      &terakan_cached_shader_ops, NULL);

   if (object == NULL)
      return NULL;

   return container_of(object, struct terakan_cached_shader, base);
}

VkResult
terakan_pipeline_cache_insert(struct vk_pipeline_cache *cache,
                              blake3_hash const key,
                              struct terakan_shader_impl const *shader,
                              mesa_shader_stage stage,
                              size_t program_size_bytes,
                              struct terakan_device *device)
{
   if (cache == NULL)
      return VK_SUCCESS;

   struct terakan_cached_shader *cached = calloc(1, sizeof(*cached));
   if (cached == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Copy key into embedded storage so the pointer outlives the caller */
   memcpy(cached->key_storage, key, sizeof(blake3_hash));

   vk_pipeline_cache_object_init(&device->vk, &cached->base,
                                 &terakan_cached_shader_ops,
                                 cached->key_storage, sizeof(blake3_hash));

   cached->stage = stage;
   cached->sq_pgm_resources[0] = shader->static_state.sq_pgm_resources[0];
   cached->sq_pgm_resources[1] = shader->static_state.sq_pgm_resources[1];
   cached->scratch_item_size_dwords = shader->scratch_item_size_dwords;

   /* Deep-copy the program bytecode from the BO.
    * The BO may be unmapped after compilation; re-map to read.
    */
   if (shader->static_state.program_bo == NULL || program_size_bytes == 0)
      goto skip_cache;

   void *bo_mapping = terakan_bo_map(shader->static_state.program_bo);
   if (bo_mapping == NULL)
      goto skip_cache;

   cached->program_size_bytes = program_size_bytes;
   cached->program_data = malloc(program_size_bytes);
   if (cached->program_data == NULL) {
      terakan_bo_unmap(shader->static_state.program_bo);
      vk_pipeline_cache_object_finish(&cached->base);
      free(cached);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   memcpy(cached->program_data, bo_mapping, program_size_bytes);
   terakan_bo_unmap(shader->static_state.program_bo);

   /* Copy stage-specific register state */
   switch (stage) {
   case MESA_SHADER_VERTEX:
      memcpy(cached->regs.vs.spi_vs_out_id,
             shader->static_state.stage.vs.spi_vs_out_id,
             sizeof(cached->regs.vs.spi_vs_out_id));
      cached->regs.vs.spi_vs_out_config = shader->static_state.stage.vs.spi_vs_out_config;
      cached->regs.vs.pa_cl_vs_out_cntl = shader->static_state.stage.vs.pa_cl_vs_out_cntl;
      break;
   case MESA_SHADER_FRAGMENT:
      cached->regs.ps.sq_pgm_exports_ps = shader->static_state.stage.ps.sq_pgm_exports_ps;
      memcpy(cached->regs.ps.spi_ps_input_cntl,
             shader->static_state.stage.ps.spi_ps_input_cntl,
             sizeof(cached->regs.ps.spi_ps_input_cntl));
      memcpy(cached->regs.ps.spi_ps_in_control,
             shader->static_state.stage.ps.spi_ps_in_control,
             sizeof(cached->regs.ps.spi_ps_in_control));
      cached->regs.ps.spi_input_z = shader->static_state.stage.ps.spi_input_z;
      cached->regs.ps.spi_baryc_cntl = shader->static_state.stage.ps.spi_baryc_cntl;
      cached->regs.ps.cb_shader_mask = shader->static_state.stage.ps.cb_shader_mask;
      cached->regs.ps.db_shader_control = shader->fs.db_shader_control;
      break;
   default:
      break;
   }

   /* Add to pipeline cache — may return a different object if another thread
    * won the insertion race.  We must use the returned (canonical) object and
    * unref it since we don't store a persistent reference here.
    *
    * Note: immediate unref is safe because normal VkPipelineCache holds a
    * strong ref.  If Mesa ever creates weak-ref caches, this would need
    * to retain the reference.
    */
   struct vk_pipeline_cache_object *canonical =
      vk_pipeline_cache_add_object(cache, &cached->base);
   vk_pipeline_cache_object_unref(&device->vk, canonical);

   return VK_SUCCESS;

skip_cache:
   /* Non-fatal — shader compiled fine but we couldn't cache it. */
   vk_pipeline_cache_object_finish(&cached->base);
   free(cached);
   return VK_SUCCESS;
}

/* --- Restore from cache --- */

VkResult
terakan_cached_shader_restore(struct terakan_cached_shader const *cached,
                              struct terakan_shader_impl *shader,
                              struct terakan_device *device,
                              VkAllocationCallbacks const *allocator)
{
   if (cached->program_size_bytes == 0 || cached->program_data == NULL)
      return VK_ERROR_UNKNOWN;

   /* Allocate a new BO for the shader program via winsys */
   struct terakan_bo *bo = NULL;
   VkResult result = device->winsys_fn->bo->allocate_device_memory(
      device, cached->program_size_bytes, TERAKAN_SHADER_PROGRAM_ALIGNMENT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &bo);
   if (result != VK_SUCCESS)
      return result;

   /* Map and copy cached bytecode into the BO */
   void *bo_mapping = terakan_bo_map(bo);
   if (bo_mapping == NULL) {
      terakan_bo_free(bo, allocator);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   /* Copy cached bytecode into the BO.
    * The cached data is already in BO-native byte order (little-endian on x86),
    * so we use plain memcpy — NOT util_memcpy_cpu_to_le32 which would
    * double-swap on a hypothetical big-endian host.
    */
   memcpy(bo_mapping, cached->program_data, cached->program_size_bytes);
   terakan_bo_unmap(bo);

   /* Fill the static state (program_va_shr8 = 0 for relative addressing) */
   shader->static_state.program_bo = bo;
   shader->static_state.program_va_shr8 = 0;
   shader->static_state.sq_pgm_resources[0] = cached->sq_pgm_resources[0];
   shader->static_state.sq_pgm_resources[1] = cached->sq_pgm_resources[1];
   shader->scratch_item_size_dwords = cached->scratch_item_size_dwords;

   /* Restore stage-specific register state */
   switch (cached->stage) {
   case MESA_SHADER_VERTEX:
      memcpy(shader->static_state.stage.vs.spi_vs_out_id,
             cached->regs.vs.spi_vs_out_id,
             sizeof(shader->static_state.stage.vs.spi_vs_out_id));
      shader->static_state.stage.vs.spi_vs_out_config = cached->regs.vs.spi_vs_out_config;
      shader->static_state.stage.vs.pa_cl_vs_out_cntl = cached->regs.vs.pa_cl_vs_out_cntl;
      break;
   case MESA_SHADER_FRAGMENT:
      shader->static_state.stage.ps.sq_pgm_exports_ps = cached->regs.ps.sq_pgm_exports_ps;
      memcpy(shader->static_state.stage.ps.spi_ps_input_cntl,
             cached->regs.ps.spi_ps_input_cntl,
             sizeof(shader->static_state.stage.ps.spi_ps_input_cntl));
      memcpy(shader->static_state.stage.ps.spi_ps_in_control,
             cached->regs.ps.spi_ps_in_control,
             sizeof(shader->static_state.stage.ps.spi_ps_in_control));
      shader->static_state.stage.ps.spi_input_z = cached->regs.ps.spi_input_z;
      shader->static_state.stage.ps.spi_baryc_cntl = cached->regs.ps.spi_baryc_cntl;
      shader->static_state.stage.ps.cb_shader_mask = cached->regs.ps.cb_shader_mask;
      shader->fs.db_shader_control = cached->regs.ps.db_shader_control;
      break;
   default:
      break;
   }

   return VK_SUCCESS;
}
