/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V queue: relocation aggregation, CS construction, manifest
 * emission, and the gated submission boundary.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_identity.h"
#include "r3v_physical_device.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_cs.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"

#include "vk_log.h"
#include "vk_sync.h"

#include "util/mesa-blake3.h"

#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Publishes the directory entry itself: the final hard link makes the file
 * visible, and the entry survives power loss only after the directory's own
 * metadata reaches storage.  Returns 0 or a negative errno.
 */
static int
fsync_dir(const char *dir)
{
   int fd = open(dir, O_RDONLY | O_DIRECTORY);
   if (fd < 0)
      return -errno;
   int result = fsync(fd) == 0 ? 0 : -errno;
   close(fd);
   return result;
}

/* Writes one evidence file durably: the payload lands in a unique temporary,
 * every byte is written and fsynced, a hard link publishes the final name
 * without replacing an existing artifact, and the directory fsync makes the
 * entry itself survive power loss.  A final name is one-shot evidence: a
 * second writer returns an error instead of mixing bytes from two submits.
 * Returns 0 or a negative errno.  The attended runners retain their
 * readback artifacts through this same writer, so every one-shot result
 * shares one durability contract.
 */
int
r3v_native_evidence_write_file(const char *dir, const char *name,
                               const void *data, size_t size)
{
   char tmp_path[1024];
   char path[1024];
   int tmp_length =
      snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.XXXXXX", dir, name);
   int length = snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (tmp_length < 0 || (size_t)tmp_length >= sizeof(tmp_path) ||
       length < 0 || (size_t)length >= sizeof(path))
      return -ENAMETOOLONG;

   int fd = mkstemp(tmp_path);
   if (fd < 0)
      return -errno;

   int result = 0;
   if (fchmod(fd, 0644) != 0)
      result = -errno;
   const uint8_t *bytes = data;
   size_t total = 0;
   while (result == 0 && total < size) {
      ssize_t written = write(fd, bytes + total, size - total);
      if (written < 0) {
         if (errno == EINTR)
            continue;
         result = -errno;
         break;
      }
      if (written == 0) {
         result = -EIO;
         break;
      }
      total += (size_t)written;
   }

   if (result == 0 && fsync(fd) != 0)
      result = -errno;
   if (close(fd) != 0 && result == 0)
      result = -errno;
   if (result != 0) {
      unlink(tmp_path);
      return result;
   }

   /* link() is the publication step: unlike rename(), it never replaces a
    * final artifact that another submit already retained.  The temporary
    * name is removed only after the final name exists. */
   if (link(tmp_path, path) != 0) {
      int saved = -errno;
      unlink(tmp_path);
      return saved;
   }
   if (unlink(tmp_path) != 0)
      return -errno;
   return fsync_dir(dir);
}

static int
r3v_native_evidence_require_fresh(const char *dir,
                                  const char *const *names, size_t count)
{
   char path[1024];
   for (size_t i = 0; i < count; i++) {
      int length = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
      if (length < 0 || (size_t)length >= sizeof(path))
         return -ENAMETOOLONG;
      if (access(path, F_OK) == 0)
         return -EEXIST;
      if (errno != ENOENT)
         return -errno;
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
   static const char *const names[] = {
      "ib.bin", "relocs.bin", "manifest.json",
   };
   int result = r3v_native_evidence_require_fresh(
      device->manifest_dir, names, ARRAY_SIZE(names));
   if (result != 0)
      return result;

   char ib_hex[BLAKE3_OUT_LEN * 2 + 1];
   char reloc_hex[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(ib, ib_size_dwords, ib_hex);
   blake3_hex(relocs->relocs, relocs->count * sizeof(relocs->relocs[0]),
              reloc_hex);

   /* ib.bin carries the canonical little-endian encoding
    * (r300_triangle_ib_serialize), matching ib_blake3 above.
    */
   /* The byte size can exceed size_t only where size_t is narrower than
    * the dword count times four, so the guard compiles only there; a
    * 64-bit size_t admits every uint32_t dword count.
    */
#if SIZE_MAX < UINT64_C(4) * UINT32_MAX
   if (ib_size_dwords > SIZE_MAX / sizeof(uint32_t))
      return -EOVERFLOW;
#endif
   const size_t ib_byte_size = (size_t)ib_size_dwords * sizeof(uint32_t);
   uint8_t *ib_bytes = malloc(ib_byte_size);
   result = ib_bytes == NULL ? -ENOMEM : 0;
   if (result == 0) {
      r300_triangle_ib_serialize(ib, ib_size_dwords, ib_bytes);
      result = r3v_native_evidence_write_file(device->manifest_dir,
                                              "ib.bin", ib_bytes,
                                              ib_byte_size);
   }
   free(ib_bytes);
   if (result == 0) {
      result = r3v_native_evidence_write_file(
         device->manifest_dir, "relocs.bin", relocs->relocs,
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
                  ? r3v_native_evidence_write_file(device->manifest_dir,
                                                   "manifest.json", manifest,
                                                   (size_t)length)
                  : -EIO;
   }
   if (result != 0) {
      vk_logw(VK_LOG_OBJS(&device->vk.base),
              "r3v-native: semantic-cell manifest retention failed: %s",
              strerror(-result));
   }
   return result;
}

static int
r3v_native_queue_append_bo_row(char **table, size_t *length,
                               size_t *capacity, bool first,
                               uint32_t reloc_index, uint32_t handle,
                               uint32_t read_domains, uint32_t write_domain,
                               uint64_t size, const char *role)
{
   char row[256];
   int row_length = snprintf(
      row, sizeof(row),
      "%s    { \"reloc_index\": %u, \"handle\": %u, "
      "\"read_domains\": %u, \"write_domain\": %u, "
      "\"size\": %llu, \"role\": \"%s\" }",
      first ? "" : ",\n", reloc_index, handle, read_domains,
      write_domain, (unsigned long long)size, role);
   if (row_length < 0 || (size_t)row_length >= sizeof(row))
      return -EIO;

   const size_t row_bytes = (size_t)row_length;
   if (*length > SIZE_MAX - row_bytes - 1)
      return -EOVERFLOW;
   const size_t required = *length + row_bytes + 1;
   if (required > *capacity) {
      size_t new_capacity = *capacity == 0 ? 256 : *capacity;
      while (new_capacity < required) {
         if (new_capacity > SIZE_MAX / 2)
            return -EOVERFLOW;
         new_capacity *= 2;
      }
      char *new_table = realloc(*table, new_capacity);
      if (new_table == NULL)
         return -ENOMEM;
      *table = new_table;
      *capacity = new_capacity;
   }

   memcpy(*table + *length, row, row_bytes);
   *length += row_bytes;
   (*table)[*length] = '\0';
   return 0;
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
   const struct radeon_drm_vk_cs *cs,
   const struct r3v_native_bo_reference *references,
   const uint32_t *reference_indices, uint32_t reference_count,
   uint32_t completion_index, uint32_t completion_handle,
   uint64_t completion_size,
   const char *kernel_release,
   const char *module_srcversion)
{
   static const char *const names[] = {
      "submit_relocs.bin", "submit_manifest.json",
   };
   int result = r3v_native_evidence_require_fresh(
      device->manifest_dir, names, ARRAY_SIZE(names));
   if (result != 0)
      return result;

   char ib_hex[BLAKE3_OUT_LEN * 2 + 1];
   char reloc_hex[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(ib, ib_size_dwords, ib_hex);
   blake3_hex(relocs->relocs, relocs->count * sizeof(relocs->relocs[0]),
              reloc_hex);

   char elf_path[1024];
   char elf_hex[BLAKE3_OUT_LEN * 2 + 1];
   char escaped_kernel_release[128 * 6 + 1];
   char escaped_module_srcversion[128 * 6 + 1];
   char escaped_elf_path[sizeof(elf_path) * 6 + 1];
   result = r3v_native_identity_collect(
      elf_path, sizeof(elf_path), elf_hex, sizeof(elf_hex));
   if (result != 0)
      return result;
   if (r3v_native_json_escape(escaped_kernel_release,
                              sizeof(escaped_kernel_release),
                              kernel_release) < 0 ||
       r3v_native_json_escape(escaped_module_srcversion,
                              sizeof(escaped_module_srcversion),
                              module_srcversion) < 0 ||
       r3v_native_json_escape(escaped_elf_path, sizeof(escaped_elf_path),
                              elf_path) < 0)
      return -EIO;

   if (result == 0) {
      /* The final relocation array is deduplicated by handle.  Each row uses
      * that array's index and domains, while the reference index map supplies
      * the command BO size and the completion index supplies its role.
      */
      if (reference_count != 0 && reference_indices == NULL)
         return -EINVAL;
      if (completion_index >= relocs->count)
         return -EINVAL;

      const struct r3v_native_bo_reference **references_by_index = NULL;
      char *bo_table = NULL;
      size_t bo_length = 0;
      size_t bo_capacity = 0;
      if (relocs->count != 0) {
         references_by_index = calloc(relocs->count,
                                      sizeof(*references_by_index));
         if (references_by_index == NULL)
            return -ENOMEM;
      }
      for (uint32_t r = 0; r < reference_count; r++) {
         if (reference_indices[r] >= relocs->count) {
            result = -EINVAL;
            goto submit_manifest_cleanup;
         }
         if (references_by_index[reference_indices[r]] == NULL)
            references_by_index[reference_indices[r]] = &references[r];
      }

      for (uint32_t reloc_index = 0; reloc_index < relocs->count;
           reloc_index++) {
         const struct r3v_native_bo_reference *reference =
            references_by_index[reloc_index];
         const bool is_completion = reloc_index == completion_index;
         if (reference == NULL && !is_completion) {
            result = -EINVAL;
            goto submit_manifest_cleanup;
         }
         if (is_completion &&
             relocs->relocs[reloc_index].handle != completion_handle) {
            result = -EINVAL;
            goto submit_manifest_cleanup;
         }

         const uint64_t size = is_completion
                                  ? completion_size
                                  : reference->memory != NULL
                                       ? reference->memory->bo.size
                                       : 0;
         const char *role = is_completion ? "completion" : "command";
         result = r3v_native_queue_append_bo_row(
            &bo_table, &bo_length, &bo_capacity, reloc_index == 0,
            reloc_index, relocs->relocs[reloc_index].handle,
            relocs->relocs[reloc_index].read_domains,
            relocs->relocs[reloc_index].write_domain, size, role);
         if (result != 0)
            goto submit_manifest_cleanup;
      }

      char manifest[16384];
      int length = snprintf(
         manifest, sizeof(manifest),
         "{\n"
         "  \"object\": \"submit-object\",\n"
         "  \"ib_dwords\": %u,\n"
         "  \"reloc_count\": %u,\n"
         "  \"ib_blake3\": \"%s\",\n"
         "  \"submit_relocs_blake3\": \"%s\",\n"
         "  \"cs_flags\": [%u, %u, %u],\n"
         "  \"chunks\": [\n"
         "    { \"id\": %u, \"length_dw\": %u },\n"
         "    { \"id\": %u, \"length_dw\": %u },\n"
         "    { \"id\": %u, \"length_dw\": %u }\n"
         "  ],\n"
         "  \"bo_table\": [\n%s\n  ],\n"
         "  \"kernel_release\": \"%s\",\n"
         "  \"module_srcversion\": \"%s\",\n"
         "  \"driver_elf_path\": \"%s\",\n"
         "  \"driver_elf_blake3\": \"%s\",\n"
         "  \"emitter\": \"r3v-native\"\n"
         "}\n",
         ib_size_dwords, relocs->count, ib_hex, reloc_hex,
         cs->flags[0], cs->flags[1], cs->flags[2],
         cs->chunks[0].chunk_id, cs->chunks[0].length_dw,
         cs->chunks[1].chunk_id, cs->chunks[1].length_dw,
         cs->chunks[2].chunk_id, cs->chunks[2].length_dw,
         bo_table != NULL ? bo_table : "", escaped_kernel_release,
         escaped_module_srcversion, escaped_elf_path, elf_hex);
      if (length <= 0 || (size_t)length >= sizeof(manifest)) {
         result = -EIO;
      } else {
         /* Validate and size every derived field before publishing either
          * final artifact, so allocation failure cannot leave a partial
          * submit-object group in a fresh evidence directory.
          */
         result = r3v_native_evidence_write_file(
            device->manifest_dir, "submit_relocs.bin", relocs->relocs,
            relocs->count * sizeof(relocs->relocs[0]));
         if (result == 0)
            result = r3v_native_evidence_write_file(
               device->manifest_dir, "submit_manifest.json", manifest,
               (size_t)length);
      }

submit_manifest_cleanup:
      free(bo_table);
      free(references_by_index);
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
   if (device->submit_hazard_accepted || device->manifest_dir != NULL) {
      uint32_t executable = 0;
      for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
         const struct r3v_native_cmd_buffer *cmd_buffer = container_of(
            submit->command_buffers[i], struct r3v_native_cmd_buffer, vk);
         if (cmd_buffer->ib_size_dwords != 0)
            executable++;
      }
      if (executable > 1) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: a retained submission carries one "
                          "command buffer; this submit carries %u",
                          executable);
      }
   }

   /* Declared dependencies order the deferred CPU execution below: each
    * wait completes before any gather or clear runs, so a producer that
    * supplies the vertex data through a semaphore is honored.  The
    * synchronous submit model signals every sync at completion, so a
    * same-queue wait is already satisfied and completes immediately.
    */
   for (uint32_t w = 0; w < submit->wait_count; w++) {
      VkResult wait_result =
         vk_sync_wait(&device->vk, submit->waits[w].sync,
                      submit->waits[w].wait_value, VK_SYNC_WAIT_COMPLETE,
                      UINT64_MAX);
      if (wait_result != VK_SUCCESS) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: semaphore wait %u failed: %d", w,
                          wait_result);
      }
   }

   /* Recording fails closed: an unsupported command poisons the buffer's
    * record_result and vkEndCommandBuffer returns the error.  The runtime
    * moves every submitted buffer to PENDING before driver_submit runs, so
    * record_result is the signal that survives to this boundary; a poisoned
    * buffer refuses the whole submit before any buffer runs.
    */
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      const struct vk_command_buffer *vk_cmd = submit->command_buffers[i];
      if (vk_cmd->record_result != VK_SUCCESS) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: command buffer %u carries recording "
                          "error %d", i, vk_cmd->record_result);
      }
   }

   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct r3v_native_cmd_buffer *cmd_buffer = container_of(
         submit->command_buffers[i], struct r3v_native_cmd_buffer, vk);

      /* Recorded transfer copies execute here, per submission, through
       * host mappings -- Vulkan's execution-time ordering, the same
       * contract the deferred draw holds below.  The recording refuses
       * a buffer mixing copies with the pass, so a copy-carrying
       * buffer has no IB and finishes at the continue.
       */
      VkResult copies =
         r3v_native_cmd_buffer_execute_deferred_copies(device, cmd_buffer);
      if (copies != VK_SUCCESS) {
         /* The runtime folds every driver_submit failure through
          * vk_queue_set_lost, so the application observes device loss
          * whatever code returns here; the loss is the honest verdict
          * for a submission whose earlier copies may already have
          * landed.
          */
         return vk_error(device, VK_ERROR_DEVICE_LOST);
      }

      /* An empty command buffer has nothing to execute and nothing to
       * pretend about.
       */
      if (cmd_buffer->ib_size_dwords == 0)
         continue;

      struct radeon_drm_vk_reloc_list relocs;
      radeon_drm_vk_reloc_list_init(&relocs);
      uint32_t *reference_indices = NULL;
      if (cmd_buffer->reference_count != 0) {
         reference_indices = calloc(cmd_buffer->reference_count,
                                    sizeof(*reference_indices));
         if (reference_indices == NULL)
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      for (uint32_t r = 0; r < cmd_buffer->reference_count; r++) {
         const struct r3v_native_bo_reference *reference =
            &cmd_buffer->references[r];
         if (radeon_drm_vk_reloc_list_add(&relocs, reference->handle,
                                          reference->read_domains,
                                          reference->write_domain, 0,
                                          &reference_indices[r]) != 0) {
            free(reference_indices);
            radeon_drm_vk_reloc_list_finish(&relocs);
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         }
      }

      struct radeon_drm_vk_cs cs;
      radeon_drm_vk_cs_build(&cs, cmd_buffer->ib,
                             cmd_buffer->ib_size_dwords, &relocs, 0, true);

      char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
      char kernel_release[128];
      char module_srcversion[128];
      struct r3v_native_arming_facts facts;
      if (device->submit_hazard_accepted) {
         /* An open gate without a retention destination fails before the
          * deferred CPU execution and before any evidence bytes are written.
          */
         if (device->manifest_dir == NULL) {
            free(reference_indices);
            radeon_drm_vk_reloc_list_finish(&relocs);
            return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                             "r3v-native: open gate requires "
                             "R3V_NATIVE_MANIFEST_DIR for submit-object "
                             "retention");
         }

         r300_triangle_ib_digest_hex(cmd_buffer->ib,
                                     cmd_buffer->ib_size_dwords, ib_digest);
         r3v_native_arming_collect_from(
            device->arming_provider != NULL
               ? device->arming_provider
               : r3v_native_arming_host_provider(),
            &facts, device->pdevice->pci_vendor_id,
            device->pdevice->pci_device_id, ib_digest,
            device->manifest_dir, kernel_release, sizeof(kernel_release),
            module_srcversion, sizeof(module_srcversion));

         if (facts.attempt_token_present) {
            free(reference_indices);
            radeon_drm_vk_reloc_list_finish(&relocs);
            return vk_errorf(
               device, VK_ERROR_DEVICE_LOST,
               "r3v-native: submission refused before evidence retention: "
               "%s",
               r3v_native_arming_verdict_name(
                  R3V_NATIVE_ARMING_ALREADY_ATTEMPTED));
         }
      }

      if (device->manifest_dir != NULL &&
          r3v_native_queue_write_manifest(device, cmd_buffer->ib,
                                          cmd_buffer->ib_size_dwords,
                                          &relocs) != 0) {
         free(reference_indices);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: semantic-cell evidence retention "
                          "failed; refusing before any ioctl");
      }

      /* Finite completion: a 4-byte write-domain BO rides the relocation
       * chunk, the kernel fences it at submit, and the bounded
       * GEM_WAIT_IDLE returns when the submission retires or escalates to
       * device loss.  The CS rebuild folds the completion reference in;
       * the semantic manifest above keeps the pre-completion relocation list,
       * while submit_manifest.json describes the final list that
       * submit_relocs.bin carries.  The completion allocates
       * here, before the deferred execution below, so every fallible
       * preparation step precedes the first application-memory write and
       * an allocation failure returns with the target untouched.
       */
      struct radeon_drm_vk_completion completion;
      if (radeon_drm_vk_completion_init(&device->drm, &completion) != 0) {
         free(reference_indices);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      uint32_t completion_index;
      if (radeon_drm_vk_completion_reference(&completion, &relocs,
                                             &completion_index) != 0) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      radeon_drm_vk_cs_build(&cs, cmd_buffer->ib,
                             cmd_buffer->ib_size_dwords, &relocs, 0, true);

      /* The public draw's vertex reads and load-op clear execute here,
       * per submission, so the stream bytes the carrier travels with are
       * the ones live at submit -- Vulkan's execution-time ordering --
       * and an unsubmitted command buffer leaves application memory
       * untouched.  Execution precedes the hazard gate because the
       * deferred work is CPU-side; the gate guards the ioctl alone.
       */
      VkResult deferred =
         r3v_native_cmd_buffer_execute_deferred_draw(device, cmd_buffer);
      if (deferred != VK_SUCCESS) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         /* vkQueueSubmit's registry contract carries host and device
          * exhaustion plus device loss, so a map failure reports as
          * host exhaustion -- the exhausted resource is host address
          * space -- and any other execution failure reports as device
          * loss.
          */
         if (deferred == VK_ERROR_MEMORY_MAP_FAILED)
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         if (deferred != VK_ERROR_OUT_OF_HOST_MEMORY &&
             deferred != VK_ERROR_OUT_OF_DEVICE_MEMORY)
            return vk_error(device, VK_ERROR_DEVICE_LOST);
         return deferred;
      }

      /* The submission ioctl opens only on the exact-value hazard gate; the
       * closed gate completes the full build, retains the manifest, and
       * fails closed instead of reporting an execution that did not run.
       */
      if (!device->submit_hazard_accepted) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission gate closed; set "
                          "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1 for an "
                          "attended run");
      }

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

      /* The collected arming facts carry the same kernel and module identity
       * the gate below judges; the evaluation itself stays the last gate
       * before the ioctl.  The exact ioctl payload retains before that gate,
       * and a retention failure refuses with nothing sent.
       */
      if (r3v_native_queue_write_submit_object(
             device, cmd_buffer->ib, cmd_buffer->ib_size_dwords, &relocs,
             &cs, cmd_buffer->references, reference_indices,
             cmd_buffer->reference_count, completion_index,
             completion.bo.handle, completion.bo.size, kernel_release,
             module_srcversion) != 0) {
         free(reference_indices);
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
      enum r3v_native_arming_verdict arming =
         r3v_native_arming_evaluate(&facts);
      if (arming != R3V_NATIVE_ARMING_ARMED) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission refused: %s",
                          r3v_native_arming_verdict_name(arming));
      }
      if (r3v_native_arming_disarm(device->manifest_dir, ib_digest) != 0) {
         free(reference_indices);
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
      free(reference_indices);
      radeon_drm_vk_completion_finish(&device->drm, &completion);
      radeon_drm_vk_reloc_list_finish(&relocs);
      if (result != 0) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission or completion wait "
                          "failed: %d", result);
      }
   }

   /* The bounded completion wait above retired every buffer, so the
    * submit's signal set fires here and a dependent submit's wait
    * completes immediately.
    */
   return vk_sync_signal_many(&device->vk, submit->signal_count,
                              submit->signals);
}
