/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V queue: relocation aggregation, CS construction, manifest
 * emission, and the gated submission boundary.
 */

#include "r3v_native.h"

#include "amd/radeon/drm_vk/radeon_drm_vk_cs.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"

#include "vk_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Writes the built submission as retained no-submit evidence: ib.bin is the
 * raw little-endian IB dword stream the offline kernel-parser replay takes,
 * relocs.bin is the packed drm_radeon_cs_reloc array, and manifest.json
 * records the sizes.  Emission failure is reported and does not change the
 * submission verdict.
 */
static void
r3v_native_queue_write_manifest(struct r3v_native_device *device,
                                const uint32_t *ib, uint32_t ib_size_dwords,
                                const struct radeon_drm_vk_reloc_list *relocs)
{
   char path[1024];

   snprintf(path, sizeof(path), "%s/ib.bin", device->manifest_dir);
   FILE *ib_file = fopen(path, "wb");
   if (ib_file != NULL) {
      fwrite(ib, sizeof(uint32_t), ib_size_dwords, ib_file);
      fclose(ib_file);
   } else {
      vk_logw(VK_LOG_OBJS(&device->vk.base),
              "r3v-native: manifest ib.bin open failed: %s", strerror(errno));
   }

   snprintf(path, sizeof(path), "%s/relocs.bin", device->manifest_dir);
   FILE *reloc_file = fopen(path, "wb");
   if (reloc_file != NULL) {
      fwrite(relocs->relocs, sizeof(relocs->relocs[0]), relocs->count,
             reloc_file);
      fclose(reloc_file);
   }

   snprintf(path, sizeof(path), "%s/manifest.json", device->manifest_dir);
   FILE *manifest_file = fopen(path, "w");
   if (manifest_file != NULL) {
      fprintf(manifest_file,
              "{\n"
              "  \"ib_dwords\": %u,\n"
              "  \"reloc_count\": %u,\n"
              "  \"emitter\": \"r3v-native\"\n"
              "}\n",
              ib_size_dwords, relocs->count);
      fclose(manifest_file);
   }
}

VkResult
r3v_native_queue_submit(struct vk_queue *queue_base,
                        struct vk_queue_submit *submit)
{
   struct r3v_native_device *device =
      container_of(queue_base->base.device, struct r3v_native_device, vk);

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

      if (device->manifest_dir != NULL) {
         r3v_native_queue_write_manifest(device, cmd_buffer->ib,
                                         cmd_buffer->ib_size_dwords,
                                         &relocs);
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

      int result = radeon_drm_vk_cs_submit(&device->drm, &cs);
      radeon_drm_vk_reloc_list_finish(&relocs);
      if (result != 0) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: DRM_RADEON_CS failed: %d", result);
      }
   }

   return VK_SUCCESS;
}
