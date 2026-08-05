/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V queue: relocation aggregation, CS construction, manifest
 * emission, and the gated submission boundary.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_physical_device.h"

#include "amd/radeon/drm_vk/radeon_drm_vk_cs.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"

#include "vk_log.h"

#include "util/mesa-blake3.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Writes one evidence file atomically: the payload lands in a dot-prefixed
 * temporary and renames into place, so a reader never sees a torn file and
 * a failed write leaves no half-named artifact.  Returns 0 or a negative
 * errno.
 */
static int
write_file_atomic(const char *dir, const char *name, const void *data,
                  size_t size)
{
   char tmp_path[1024];
   char path[1024];
   snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.tmp", dir, name);
   snprintf(path, sizeof(path), "%s/%s", dir, name);

   FILE *f = fopen(tmp_path, "wb");
   if (f == NULL)
      return -errno;
   size_t written = fwrite(data, 1, size, f);
   int flush_result = fflush(f);
   if (fclose(f) != 0 || written != size || flush_result != 0) {
      remove(tmp_path);
      return -EIO;
   }
   if (rename(tmp_path, path) != 0) {
      int saved = -errno;
      remove(tmp_path);
      return saved;
   }
   return 0;
}

static void
blake3_hex(const void *data, size_t size, char out[BLAKE3_OUT_LEN * 2 + 1])
{
   struct mesa_blake3 ctx;
   uint8_t digest[BLAKE3_OUT_LEN];
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, data, size);
   _mesa_blake3_final(&ctx, digest);
   _mesa_blake3_format(out, digest);
}

/* Retains the semantic cell -- the IB and the command buffer's own
 * relocation list, before the completion reference folds in -- as
 * content-bound evidence: ib.bin and relocs.bin land atomically, and
 * manifest.json binds them by BLAKE3 digest.  Returns 0 or a negative
 * errno; the caller decides whether a failure refuses the submission.
 */
static int
r3v_native_queue_write_manifest(struct r3v_native_device *device,
                                const uint32_t *ib, uint32_t ib_size_dwords,
                                const struct radeon_drm_vk_reloc_list *relocs)
{
   char ib_hex[BLAKE3_OUT_LEN * 2 + 1];
   char reloc_hex[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(ib, ib_size_dwords * sizeof(uint32_t), ib_hex);
   blake3_hex(relocs->relocs, relocs->count * sizeof(relocs->relocs[0]),
              reloc_hex);

   int result = write_file_atomic(device->manifest_dir, "ib.bin", ib,
                                  ib_size_dwords * sizeof(uint32_t));
   if (result == 0) {
      result = write_file_atomic(device->manifest_dir, "relocs.bin",
                                 relocs->relocs,
                                 relocs->count * sizeof(relocs->relocs[0]));
   }
   if (result == 0) {
      char manifest[1024];
      int length = snprintf(manifest, sizeof(manifest),
                            "{\n"
                            "  \"object\": \"semantic-cell\",\n"
                            "  \"ib_dwords\": %u,\n"
                            "  \"reloc_count\": %u,\n"
                            "  \"ib_blake3\": \"%s\",\n"
                            "  \"relocs_blake3\": \"%s\",\n"
                            "  \"emitter\": \"r3v-native\"\n"
                            "}\n",
                            ib_size_dwords, relocs->count, ib_hex, reloc_hex);
      result = length > 0 && (size_t)length < sizeof(manifest)
                  ? write_file_atomic(device->manifest_dir, "manifest.json",
                                      manifest, (size_t)length)
                  : -EIO;
   }
   if (result != 0) {
      vk_logw(VK_LOG_OBJS(&device->vk.base),
              "r3v-native: semantic-cell manifest retention failed: %s",
              strerror(-result));
   }
   return result;
}

/* Retains the exact submit object -- the final IB and the relocation list
 * with the completion reference folded in, the byte content the
 * DRM_RADEON_CS ioctl would carry -- distinct from the semantic cell.
 * Returns 0 or a negative errno.
 */
static int
r3v_native_queue_write_submit_object(
   struct r3v_native_device *device, const uint32_t *ib,
   uint32_t ib_size_dwords, const struct radeon_drm_vk_reloc_list *relocs,
   const struct radeon_drm_vk_cs *cs)
{
   char ib_hex[BLAKE3_OUT_LEN * 2 + 1];
   char reloc_hex[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(ib, ib_size_dwords * sizeof(uint32_t), ib_hex);
   blake3_hex(relocs->relocs, relocs->count * sizeof(relocs->relocs[0]),
              reloc_hex);

   int result = write_file_atomic(device->manifest_dir, "submit_relocs.bin",
                                  relocs->relocs,
                                  relocs->count * sizeof(relocs->relocs[0]));
   if (result == 0) {
      char manifest[1400];
      int length = snprintf(
         manifest, sizeof(manifest),
         "{\n"
         "  \"object\": \"submit-object\",\n"
         "  \"ib_dwords\": %u,\n"
         "  \"reloc_count\": %u,\n"
         "  \"ib_blake3\": \"%s\",\n"
         "  \"submit_relocs_blake3\": \"%s\",\n"
         "  \"chunks\": [\n"
         "    { \"id\": %u, \"length_dw\": %u },\n"
         "    { \"id\": %u, \"length_dw\": %u },\n"
         "    { \"id\": %u, \"length_dw\": %u }\n"
         "  ],\n"
         "  \"emitter\": \"r3v-native\"\n"
         "}\n",
         ib_size_dwords, relocs->count, ib_hex, reloc_hex,
         cs->chunks[0].chunk_id, cs->chunks[0].length_dw,
         cs->chunks[1].chunk_id, cs->chunks[1].length_dw,
         cs->chunks[2].chunk_id, cs->chunks[2].length_dw);
      result = length > 0 && (size_t)length < sizeof(manifest)
                  ? write_file_atomic(device->manifest_dir,
                                      "submit_manifest.json", manifest,
                                      (size_t)length)
                  : -EIO;
   }
   if (result != 0) {
      vk_logw(VK_LOG_OBJS(&device->vk.base),
              "r3v-native: submit-object retention failed: %s",
              strerror(-result));
   }
   return result;
}

VkResult
r3v_native_queue_submit(struct vk_queue *queue_base,
                        struct vk_queue_submit *submit)
{
   struct r3v_native_device *device =
      container_of(queue_base->base.device, struct r3v_native_device, vk);

   /* An authorization declares one IB digest and its evidence directory
    * disarms after one attempt, so an open gate admits exactly one
    * executable command buffer.  Refusing the whole submit up front
    * keeps a multi-buffer submit from executing its first buffer and
    * then reporting device loss on the disarmed second.
    */
   if (device->submit_hazard_accepted) {
      uint32_t executable = 0;
      for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
         const struct r3v_native_cmd_buffer *cmd_buffer = container_of(
            submit->command_buffers[i], struct r3v_native_cmd_buffer, vk);
         if (cmd_buffer->ib_size_dwords != 0)
            executable++;
      }
      if (executable > 1) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: an armed submission carries one "
                          "command buffer; this submit carries %u",
                          executable);
      }
   }

   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct r3v_native_cmd_buffer *cmd_buffer = container_of(
         submit->command_buffers[i], struct r3v_native_cmd_buffer, vk);

      /* An empty command buffer has nothing to execute and nothing to
       * pretend about.
       */
      if (cmd_buffer->ib_size_dwords == 0)
         continue;

      struct radeon_drm_vk_reloc_list relocs;
      radeon_drm_vk_reloc_list_init(&relocs);
      for (uint32_t r = 0; r < cmd_buffer->reference_count; r++) {
         const struct r3v_native_bo_reference *reference =
            &cmd_buffer->references[r];
         uint32_t index;
         if (radeon_drm_vk_reloc_list_add(&relocs, reference->handle,
                                          reference->read_domains,
                                          reference->write_domain, 0,
                                          &index) != 0) {
            radeon_drm_vk_reloc_list_finish(&relocs);
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         }
      }

      struct radeon_drm_vk_cs cs;
      radeon_drm_vk_cs_build(&cs, cmd_buffer->ib,
                             cmd_buffer->ib_size_dwords, &relocs, 0, true);

      if (device->manifest_dir != NULL &&
          r3v_native_queue_write_manifest(device, cmd_buffer->ib,
                                          cmd_buffer->ib_size_dwords,
                                          &relocs) != 0) {
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: semantic-cell evidence retention "
                          "failed; refusing before any ioctl");
      }

      /* The submission ioctl opens only on the exact-value hazard gate; the
       * closed gate completes the full build, retains the manifest, and
       * fails closed instead of reporting an execution that did not run.
       */
      if (!device->submit_hazard_accepted) {
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission gate closed; set "
                          "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1 for an "
                          "attended run");
      }

      /* An open gate submits only with the evidence chain intact: the
       * manifest directory is part of the submission contract, and the
       * exact submit object retains below before the ioctl runs.
       */
      if (device->manifest_dir == NULL) {
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: open gate requires "
                          "R3V_NATIVE_MANIFEST_DIR for submit-object "
                          "retention");
      }

      /* Finite completion: a 4-byte write-domain BO rides the relocation
       * chunk, the kernel fences it at submit, and the bounded
       * GEM_WAIT_IDLE returns when the submission retires or escalates to
       * device loss.  The CS rebuild folds the completion reference in;
       * the manifest above keeps the pre-completion relocation list, the
       * exact list the offline replay consumes.
       */
      /* The RS480 GART reads and writes with request snooping disabled,
       * and every GTT mapping is ttm_cached, so the driver keeps the
       * HOST_COHERENT promise itself: every referenced memory with a live
       * CPU mapping publishes its cache lines before the submission ioctl
       * and invalidates them after completion, the only device-access
       * window the synchronous submit model has.
       */
      for (uint32_t r = 0; r < cmd_buffer->reference_count; r++) {
         const struct r3v_native_bo_reference *reference =
            &cmd_buffer->references[r];
         if (reference->memory != NULL && reference->memory->map != NULL) {
            radeon_drm_vk_bo_cache_sync(&device->drm,
                                        reference->memory->map,
                                        reference->memory->bo.size);
         }
      }

      struct radeon_drm_vk_completion completion;
      if (radeon_drm_vk_completion_init(&device->drm, &completion) != 0) {
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      uint32_t completion_index;
      if (radeon_drm_vk_completion_reference(&completion, &relocs,
                                             &completion_index) != 0) {
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      radeon_drm_vk_cs_build(&cs, cmd_buffer->ib,
                             cmd_buffer->ib_size_dwords, &relocs, 0, true);

      /* The exact ioctl payload retains before the ioctl; a retention
       * failure refuses the submission with nothing sent.
       */
      if (r3v_native_queue_write_submit_object(device, cmd_buffer->ib,
                                               cmd_buffer->ib_size_dwords,
                                               &relocs, &cs) != 0) {
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submit-object evidence retention "
                          "failed; refusing before the ioctl");
      }

      /* Last gate before the ioctl: every arming factor -- declared
       * bundle digest against the IB about to travel, authorized chip,
       * kernel release, radeon module srcversion, and a fresh evidence
       * directory -- holds at once, or nothing is sent.  Writing the
       * attempt token disarms the directory, so a second run through the
       * same evidence refuses even with the same environment.
       */
      char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
      char kernel_release[128];
      char module_srcversion[128];
      blake3_hex(cmd_buffer->ib,
                 cmd_buffer->ib_size_dwords * sizeof(uint32_t), ib_digest);
      struct r3v_native_arming_facts facts;
      r3v_native_arming_collect(&facts, device->pdevice->pci_vendor_id,
                                device->pdevice->pci_device_id, ib_digest,
                                device->manifest_dir, kernel_release,
                                sizeof(kernel_release), module_srcversion,
                                sizeof(module_srcversion));
      enum r3v_native_arming_verdict arming =
         r3v_native_arming_evaluate(&facts);
      if (arming != R3V_NATIVE_ARMING_ARMED) {
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission refused: %s",
                          r3v_native_arming_verdict_name(arming));
      }
      if (r3v_native_arming_disarm(device->manifest_dir) != 0) {
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: one-shot disarm failed; refusing "
                          "before the ioctl");
      }

      int result = radeon_drm_vk_cs_submit(&device->drm, &cs);
      if (result == 0)
         result = radeon_drm_vk_completion_await(&device->drm, &completion);
      if (result == 0) {
         /* Device writes landed in memory past the cache; drop every
          * stale line over the live mappings before the host reads them.
          */
         for (uint32_t r = 0; r < cmd_buffer->reference_count; r++) {
            const struct r3v_native_bo_reference *reference =
               &cmd_buffer->references[r];
            if (reference->memory != NULL &&
                reference->memory->map != NULL) {
               radeon_drm_vk_bo_cache_sync(&device->drm,
                                           reference->memory->map,
                                           reference->memory->bo.size);
            }
         }
      }
      radeon_drm_vk_completion_finish(&device->drm, &completion);
      radeon_drm_vk_reloc_list_finish(&relocs);
      if (result != 0) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission or completion wait "
                          "failed: %d", result);
      }
   }

   return VK_SUCCESS;
}
