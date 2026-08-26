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

#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_r2vb_reingest_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_cs.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"

#include "vk_log.h"
#include "vk_sync.h"

#include "util/mesa-blake3.h"

#include <radeon_drm.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
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

/* The geometry fact the arming gate reads, resolved per cell kind.  The
 * triangle and direct-write cells render the public target family, so
 * their attended identity is the maximum 64x64 extent.  The producer
 * cell renders into the carrier, so its geometry is the reference
 * layout's slot row: the bound allocation measures exactly pitch times
 * height times the 16-byte texel -- radeon_drm_vk_bo_create stores the
 * requested size, so the comparison is against the caller's declared
 * footprint -- and the one relocation carries the GTT domain in both
 * directions, since the color backend writes the row and the consuming
 * vertex fetch reads it.  A kind outside the set reports unfrozen, and
 * the gate's own kind check names it first.
 */
static bool
cell_geometry_unfrozen(const struct r3v_native_cmd_buffer *cmd_buffer)
{
   switch (cmd_buffer->cell_kind) {
   case R3V_NATIVE_CELL_KIND_TRIANGLE: {
      /* The render-shape family is the geometry the delivered arms
       * cover: an extent inside R3V_NATIVE_RENDER_MAX_EXTENT on both
       * axes over a row pitch that is a multiple of eight pixels and at
       * least the width.  The recorded cell places each of those
       * through one register class, so a shape inside the family is
       * geometry the emission already froze.
       */
      if (!cmd_buffer->deferred_draw.pending)
         return false;
      const uint32_t width = cmd_buffer->deferred_draw.target_width;
      const uint32_t height = cmd_buffer->deferred_draw.target_height;
      const uint32_t pitch_pixels =
         r3v_native_render_row_pitch_bytes(width) / 4;
      return width < 1 || width > R3V_NATIVE_RENDER_MAX_EXTENT ||
             height < 1 || height > R3V_NATIVE_RENDER_MAX_EXTENT ||
             pitch_pixels < width || pitch_pixels % 8 != 0 ||
             pitch_pixels > R3V_NATIVE_RENDER_MAX_EXTENT;
   }
   case R3V_NATIVE_CELL_KIND_DIRECT_WRITE:
      return cmd_buffer->deferred_draw.pending &&
             (cmd_buffer->deferred_draw.target_width !=
                 R3V_NATIVE_TARGET_WIDTH ||
              cmd_buffer->deferred_draw.target_height !=
                 R3V_NATIVE_TARGET_HEIGHT);
   case R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE: {
      /* The declared shape is the geometry and the digest carries it;
       * the frozen facts are the fixed-cell binding: two relocations,
       * the vertex page device-read and the color target device-written,
       * with no deferred public draw riding the kind.
       */
      if (cmd_buffer->deferred_draw.pending ||
          cmd_buffer->reference_count != R300_TRIANGLE_RENDER_SLOT_COUNT)
         return true;
      const struct r3v_native_bo_reference *vertex =
         &cmd_buffer->references[R300_TRIANGLE_SLOT_VERTEX];
      const struct r3v_native_bo_reference *color =
         &cmd_buffer->references[R300_TRIANGLE_SLOT_COLOR];
      return vertex->read_domains != RADEON_GEM_DOMAIN_GTT ||
             vertex->write_domain != 0 || color->read_domains != 0 ||
             color->write_domain != RADEON_GEM_DOMAIN_GTT;
   }
   case R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC: {
      /* The composed cell renders the consumer's maximum public extent
       * and crosses the carrier through both engines, so the vertex
       * slot's relocation carries the GTT domain in both directions.
       */
      if (!cmd_buffer->deferred_draw.pending ||
          cmd_buffer->deferred_draw.target_width !=
             R3V_NATIVE_TARGET_WIDTH ||
          cmd_buffer->deferred_draw.target_height !=
             R3V_NATIVE_TARGET_HEIGHT)
         return true;
      if (cmd_buffer->reference_count != R300_TRIANGLE_RENDER_SLOT_COUNT)
         return true;
      const struct r3v_native_bo_reference *slot =
         &cmd_buffer->references[R300_TRIANGLE_SLOT_VERTEX];
      return slot->read_domains != RADEON_GEM_DOMAIN_GTT ||
             slot->write_domain != RADEON_GEM_DOMAIN_GTT;
   }
   case R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED: {
      /* The fetched composition renders the consumer's maximum public
       * extent over four relocations: the carrier crosses both engines,
       * the color target is written, and the slot and source arrays are
       * device-read; the slot array is the admission's own page.
       */
      if (!cmd_buffer->deferred_draw.pending ||
          cmd_buffer->deferred_draw.target_width !=
             R3V_NATIVE_TARGET_WIDTH ||
          cmd_buffer->deferred_draw.target_height !=
             R3V_NATIVE_TARGET_HEIGHT)
         return true;
      if (cmd_buffer->reference_count != 4)
         return true;
      const struct r3v_native_bo_reference *carrier =
         &cmd_buffer->references[R300_TRIANGLE_SLOT_VERTEX];
      const struct r3v_native_bo_reference *color =
         &cmd_buffer->references[R300_TRIANGLE_SLOT_COLOR];
      const struct r3v_native_bo_reference *slot =
         &cmd_buffer->references[2];
      const struct r3v_native_bo_reference *source =
         &cmd_buffer->references[3];
      if (carrier->read_domains != RADEON_GEM_DOMAIN_GTT ||
          carrier->write_domain != RADEON_GEM_DOMAIN_GTT)
         return true;
      if (color->write_domain != RADEON_GEM_DOMAIN_GTT)
         return true;
      if (slot->read_domains != RADEON_GEM_DOMAIN_GTT ||
          slot->write_domain != 0 || slot->memory == NULL ||
          slot->memory != cmd_buffer->owned_slot)
         return true;
      return source->read_domains != RADEON_GEM_DOMAIN_GTT ||
             source->write_domain != 0 || source->memory == NULL;
   }
   case R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER: {
      /* The compute identity carrier binds three relocations: the
       * output written by the color backend, the slot array (the
       * admission's own page) and the input array device-read.
       */
      if (!cmd_buffer->deferred_dispatch.pending ||
          !cmd_buffer->deferred_dispatch.gpu_carrier_delivery)
         return true;
      if (cmd_buffer->reference_count != 3)
         return true;
      const struct r3v_native_bo_reference *output =
         &cmd_buffer->references[0];
      const struct r3v_native_bo_reference *slot =
         &cmd_buffer->references[1];
      const struct r3v_native_bo_reference *input =
         &cmd_buffer->references[2];
      if (output->read_domains != 0 ||
          output->write_domain != RADEON_GEM_DOMAIN_GTT ||
          output->memory == NULL)
         return true;
      if (slot->read_domains != RADEON_GEM_DOMAIN_GTT ||
          slot->write_domain != 0 || slot->memory == NULL ||
          slot->memory != cmd_buffer->owned_slot)
         return true;
      return input->read_domains != RADEON_GEM_DOMAIN_GTT ||
             input->write_domain != 0 || input->memory == NULL;
   }
   case R3V_NATIVE_CELL_KIND_ZB_DEPTH_CONTROL: {
      /* The depth control binds three exact footprints: the vertex page
       * device-read, the color target device-written, and the depth
       * surface crossing both directions -- the host fill is the
       * comparison's stored operand and the device writes passing
       * fragments -- so its relocation carries the GTT domain on both
       * sides.
       */
      if (cmd_buffer->reference_count != R300_ZB_DEPTH_CONTROL_SLOT_COUNT)
         return true;
      const struct r3v_native_bo_reference *vertex =
         &cmd_buffer->references[R300_ZB_DEPTH_CONTROL_SLOT_VERTEX];
      const struct r3v_native_bo_reference *color =
         &cmd_buffer->references[R300_ZB_DEPTH_CONTROL_SLOT_COLOR];
      const struct r3v_native_bo_reference *depth =
         &cmd_buffer->references[R300_ZB_DEPTH_CONTROL_SLOT_DEPTH];
      if (vertex->read_domains != RADEON_GEM_DOMAIN_GTT ||
          vertex->write_domain != 0 || vertex->memory == NULL ||
          vertex->memory->bo.size != R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION)
         return true;
      if (color->read_domains != 0 ||
          color->write_domain != RADEON_GEM_DOMAIN_GTT ||
          color->memory == NULL ||
          color->memory->bo.size != R300_ZB_DEPTH_CONTROL_COLOR_BYTES)
         return true;
      return depth->read_domains != RADEON_GEM_DOMAIN_GTT ||
             depth->write_domain != RADEON_GEM_DOMAIN_GTT ||
             depth->memory == NULL ||
             depth->memory->bo.size != R300_ZB_DEPTH_CONTROL_DEPTH_BYTES;
   }
   case R3V_NATIVE_CELL_KIND_R2VB_PRODUCER: {
      uint32_t carrier_bytes;
      if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0)
         return true;
      if (cmd_buffer->reference_count != R300_R2VB_PRODUCER_SLOT_COUNT)
         return true;
      const struct r3v_native_bo_reference *slot =
         &cmd_buffer->references[R300_R2VB_PRODUCER_SLOT_CARRIER];
      if (slot->read_domains != RADEON_GEM_DOMAIN_GTT ||
          slot->write_domain != RADEON_GEM_DOMAIN_GTT)
         return true;
      return slot->memory == NULL || slot->memory->bo.size != carrier_bytes;
   }
   case R3V_NATIVE_CELL_KIND_R2VB_REINGEST: {
      /* The re-ingest cell binds the producer's carrier row and the
       * triangle's target: the carrier crosses both engines, so its
       * relocation carries the GTT domain in both directions, and the
       * color slot is the triangle target family's retained footprint,
       * written through the color backend alone.
       */
      uint32_t carrier_bytes;
      if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0)
         return true;
      if (cmd_buffer->reference_count != R300_R2VB_REINGEST_SLOT_COUNT)
         return true;
      const struct r3v_native_bo_reference *carrier =
         &cmd_buffer->references[R300_R2VB_REINGEST_SLOT_CARRIER];
      const struct r3v_native_bo_reference *color =
         &cmd_buffer->references[R300_R2VB_REINGEST_SLOT_COLOR];
      if (carrier->read_domains != RADEON_GEM_DOMAIN_GTT ||
          carrier->write_domain != RADEON_GEM_DOMAIN_GTT)
         return true;
      if (carrier->memory == NULL ||
          carrier->memory->bo.size != carrier_bytes)
         return true;
      if (color->read_domains != 0 ||
          color->write_domain != RADEON_GEM_DOMAIN_GTT)
         return true;
      return color->memory == NULL ||
             color->memory->bo.size != R3V_NATIVE_TARGET_MEMORY_BYTES;
   }
   case R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE:
   case R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL: {
      /* The tuple cell binds the producer's carrier row and the fetched
       * vertex BO: the carrier crosses the color backend and the frozen
       * fetch declaration, so its relocation carries the GTT domain in
       * both directions, and the vertex BO is device-read alone, sized
       * to the reference records' two fetch arrays.  The serial
       * status-load cell submits the same frozen stream, so it shares
       * the geometry fact.
       */
      uint32_t carrier_bytes;
      if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0)
         return true;
      if (cmd_buffer->reference_count != R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT)
         return true;
      const struct r3v_native_bo_reference *carrier =
         &cmd_buffer->references[R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER];
      const struct r3v_native_bo_reference *vertex =
         &cmd_buffer->references[R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX];
      if (carrier->read_domains != RADEON_GEM_DOMAIN_GTT ||
          carrier->write_domain != RADEON_GEM_DOMAIN_GTT)
         return true;
      if (carrier->memory == NULL ||
          carrier->memory->bo.size != carrier_bytes)
         return true;
      if (vertex->read_domains != RADEON_GEM_DOMAIN_GTT ||
          vertex->write_domain != 0)
         return true;
      return vertex->memory == NULL ||
             vertex->memory->bo.size !=
                R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *
                   (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +
                    R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES);
   }
   case R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST: {
      /* The burst shares the tuple's BO roles; its carrier is the
       * recorded member count of reference rows, so the geometry fact
       * binds the allocation to the composed depth.
       */
      uint32_t carrier_bytes;
      if (r3v_native_burst_carrier_bytes(cmd_buffer->burst_draws,
                                         &carrier_bytes) != 0)
         return true;
      if (cmd_buffer->reference_count != R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT)
         return true;
      const struct r3v_native_bo_reference *carrier =
         &cmd_buffer->references[R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER];
      const struct r3v_native_bo_reference *vertex =
         &cmd_buffer->references[R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX];
      if (carrier->read_domains != RADEON_GEM_DOMAIN_GTT ||
          carrier->write_domain != RADEON_GEM_DOMAIN_GTT)
         return true;
      if (carrier->memory == NULL ||
          carrier->memory->bo.size != carrier_bytes)
         return true;
      if (vertex->read_domains != RADEON_GEM_DOMAIN_GTT ||
          vertex->write_domain != 0)
         return true;
      /* The recorded fetch width sizes the vertex stream: the FLOAT_4
       * model form stores full records behind the slot array.
       */
      return vertex->memory == NULL ||
             vertex->memory->bo.size !=
                R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *
                   (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +
                    (cmd_buffer->burst_model_float4
                        ? R300_R2VB_FLOAT4_MODEL_STRIDE_BYTES
                        : R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES));
   }
   case R3V_NATIVE_CELL_KIND_UNDECLARED:
   default:
      return true;
   }
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
   const char *module_srcversion,
   int serial_admission)
{
   /* A one-shot cell retains one submit object under the fixed names; a
    * serial admission retains its own exact ioctl payload under its
    * 0-based admission index, so every admission's evidence lands fresh
    * and none overwrites a retained predecessor.
    */
   char relocs_name[64];
   char manifest_name[64];
   if (serial_admission < 0) {
      snprintf(relocs_name, sizeof(relocs_name), "submit_relocs.bin");
      snprintf(manifest_name, sizeof(manifest_name),
               "submit_manifest.json");
   } else {
      snprintf(relocs_name, sizeof(relocs_name), "submit_relocs_%02d.bin",
               serial_admission);
      snprintf(manifest_name, sizeof(manifest_name),
               "submit_manifest_%02d.json", serial_admission);
   }
   const char *const names[] = { relocs_name, manifest_name };
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
            device->manifest_dir, relocs_name, relocs->relocs,
            relocs->count * sizeof(relocs->relocs[0]));
         if (result == 0)
            result = r3v_native_evidence_write_file(
               device->manifest_dir, manifest_name, manifest,
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

static bool
r3v_native_submit_resignals_sync(const struct vk_queue_submit *submit,
                                 const struct vk_sync *sync)
{
   for (uint32_t i = 0; i < submit->signal_count; i++) {
      if (submit->signals[i].sync == sync)
         return true;
   }
   return false;
}

bool
r3v_native_queue_wait_is_permanent_binary(const struct vk_queue_submit *submit,
                                          uint32_t wait_index)
{
   const struct vk_sync *wait_sync = submit->waits[wait_index].sync;
   return !(wait_sync->flags & VK_SYNC_IS_TIMELINE) &&
          (submit->_wait_points == NULL ||
           submit->_wait_points[wait_index] == NULL) &&
          submit->_wait_temps[wait_index] == NULL &&
          !r3v_native_submit_resignals_sync(submit, wait_sync);
}

static VkResult
r3v_native_queue_consume_binary_waits(
   struct r3v_native_device *device, const struct vk_queue_submit *submit)
{
   for (uint32_t w = 0; w < submit->wait_count; w++) {
      /* vk_queue_submit_final (rg --fixed-strings
       * "submit->_wait_points[i] = wait_point"
       * src/vulkan/runtime/vk_queue.c) unwraps an emulated timeline wait
       * into its binary point and keeps that point in _wait_points. The
       * timeline owns the point until its value is collected, so resetting
       * the binary payload here would make a completed value wait forever.
       */
      if (!r3v_native_queue_wait_is_permanent_binary(submit, w))
         continue;

      struct vk_sync *wait_sync = submit->waits[w].sync;
      VkResult reset_result = vk_sync_reset(&device->vk, wait_sync);
      if (reset_result != VK_SUCCESS) {
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: binary semaphore wait %u reset "
                          "failed: %d",
                          w, reset_result);
      }
   }

   return VK_SUCCESS;
}

enum r3v_native_queue_status
r3v_native_queue_submission_status(VkDevice _device)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return device != NULL ? device->queue_status
                         : R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION;
}

/* One raw monotonic reading in nanoseconds.  The raw clock stands
 * outside NTP slew, so two readings subtract to a wall interval.  A
 * clock this thread cannot read yields 0, which reads downstream as an
 * unmeasured interval rather than as a wrapped one.
 */
static uint64_t
r3v_native_raw_now_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t
r3v_native_queue_transport_wall_ns(VkDevice _device)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   if (device == NULL || device->transport_enter_ns == 0 ||
       device->transport_return_ns < device->transport_enter_ns)
      return 0;
   return device->transport_return_ns - device->transport_enter_ns;
}

bool
r3v_native_queue_observed_gpu_producer(VkDevice _device)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   return device != NULL && device->transport_gpu_producer_delivery;
}

enum r3v_native_observed_route
r3v_native_queue_observed_route(VkDevice _device)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   if (device == NULL)
      return R3V_NATIVE_OBSERVED_ROUTE_CPU;
   if (device->transport_cell_kind ==
       R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER)
      return R3V_NATIVE_OBSERVED_ROUTE_COMPUTE_IDENTITY_CARRIER;
   if (!device->transport_gpu_producer_delivery)
      return R3V_NATIVE_OBSERVED_ROUTE_CPU;
   return device->transport_cell_kind ==
                R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED
             ? R3V_NATIVE_OBSERVED_ROUTE_GPU_PRODUCER_FETCHED
             : R3V_NATIVE_OBSERVED_ROUTE_GPU_PRODUCER;
}

const char *
r3v_native_observed_route_name(enum r3v_native_observed_route route)
{
   switch (route) {
   case R3V_NATIVE_OBSERVED_ROUTE_CPU:
      return "cpu";
   case R3V_NATIVE_OBSERVED_ROUTE_GPU_PRODUCER:
      return "gpu";
   case R3V_NATIVE_OBSERVED_ROUTE_GPU_PRODUCER_FETCHED:
      return "fetched";
   case R3V_NATIVE_OBSERVED_ROUTE_COMPUTE_IDENTITY_CARRIER:
      return "compute-identity-carrier";
   }
   return "unknown";
}

static int
r3v_native_submission_trace_emit(
   struct r3v_native_device *device,
   enum r3v_native_submission_trace_event event)
{
   if (device->submission_trace.emit == NULL)
      return 0;
   return device->submission_trace.emit(device->submission_trace.ctx, event);
}

void
r3v_native_prepared_release(struct r3v_native_device *device)
{
   struct r3v_native_prepared_submission *prepared = &device->prepared;
   if (!prepared->valid)
      return;
   radeon_drm_vk_completion_finish(&device->drm, &prepared->completion);
   radeon_drm_vk_reloc_list_finish(&prepared->relocs);
   free(prepared->reference_indices);
   memset(prepared, 0, sizeof(*prepared));
}

VkResult
r3v_native_queue_prepare_submission(VkDevice _device,
                                    VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, cmd_buffer, commandBuffer);
   struct r3v_native_prepared_submission *prepared = &device->prepared;

   if (prepared->valid) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: one submission prepares at a time; a "
                       "prepared submission is already pending");
   }
   if (cmd_buffer->vk.record_result != VK_SUCCESS) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: the command buffer carries recording "
                       "error %d", cmd_buffer->vk.record_result);
   }
   if (cmd_buffer->ib_size_dwords == 0 ||
       cmd_buffer->deferred_draw.pending ||
       cmd_buffer->deferred_dispatch.pending ||
       cmd_buffer->deferred_copy_count != 0) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: prepare covers transport-only command "
                       "buffers; deferred host work executes submission-time "
                       "semantics the prepare would hoist");
   }
   if (!device->submit_hazard_accepted || device->manifest_dir == NULL) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: prepare requires the open hazard gate "
                       "and R3V_NATIVE_MANIFEST_DIR");
   }

   radeon_drm_vk_reloc_list_init(&prepared->relocs);
   prepared->reference_indices = NULL;
   if (cmd_buffer->reference_count != 0) {
      prepared->reference_indices = calloc(cmd_buffer->reference_count,
                                           sizeof(uint32_t));
      if (prepared->reference_indices == NULL)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   for (uint32_t r = 0; r < cmd_buffer->reference_count; r++) {
      const struct r3v_native_bo_reference *reference =
         &cmd_buffer->references[r];
      if (radeon_drm_vk_reloc_list_add(&prepared->relocs, reference->handle,
                                       reference->read_domains,
                                       reference->write_domain, 0,
                                       &prepared->reference_indices[r]) !=
          0) {
         free(prepared->reference_indices);
         radeon_drm_vk_reloc_list_finish(&prepared->relocs);
         memset(prepared, 0, sizeof(*prepared));
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
   char kernel_release[128];
   char module_srcversion[128];
   struct r3v_native_arming_facts facts = {0};
   r300_triangle_ib_digest_hex(cmd_buffer->ib, cmd_buffer->ib_size_dwords,
                               ib_digest);
   r3v_native_arming_collect_from(
      device->arming_provider != NULL ? device->arming_provider
                                      : r3v_native_arming_host_provider(),
      &facts, device->pdevice->pci_vendor_id,
      device->pdevice->pci_device_id, cmd_buffer->cell_kind, ib_digest,
      device->manifest_dir, kernel_release, sizeof(kernel_release),
      module_srcversion, sizeof(module_srcversion));
   facts.nonmaximum_extent = cell_geometry_unfrozen(cmd_buffer);
   facts.serial_submissions_consumed = device->serial_submissions_consumed;
   facts.burst_recorded_draws = cmd_buffer->burst_draws;

   const bool serial_kind =
      cmd_buffer->cell_kind == R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL;
   const bool serial_continuation =
      serial_kind && device->serial_submissions_consumed > 0;
   VkResult result = VK_SUCCESS;
   if (facts.attempt_token_present && !serial_continuation) {
      result = vk_errorf(device, VK_ERROR_DEVICE_LOST,
                         "r3v-native: prepare refused before evidence "
                         "retention: %s",
                         r3v_native_arming_verdict_name(
                            R3V_NATIVE_ARMING_ALREADY_ATTEMPTED));
      goto prepare_fail;
   }

   if (!(serial_kind && device->serial_submissions_consumed > 0) &&
       r3v_native_queue_write_manifest(device, cmd_buffer->ib,
                                       cmd_buffer->ib_size_dwords,
                                       &prepared->relocs) != 0) {
      result = vk_errorf(device, VK_ERROR_DEVICE_LOST,
                         "r3v-native: semantic-cell evidence retention "
                         "failed; refusing before any ioctl");
      goto prepare_fail;
   }

   if (radeon_drm_vk_completion_init(&device->drm, &prepared->completion) !=
       0) {
      result = vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto prepare_fail;
   }
   if (radeon_drm_vk_completion_reference(&prepared->completion,
                                          &prepared->relocs,
                                          &prepared->completion_index) != 0) {
      radeon_drm_vk_completion_finish(&device->drm, &prepared->completion);
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto prepare_fail;
   }
   /* The one CS build: the argument block lives in the device-resident
    * prepared struct, so its self-referential chunk pointers stay valid
    * until the commit's ioctl consumes them.
    */
   radeon_drm_vk_cs_build(&prepared->cs, cmd_buffer->ib,
                          cmd_buffer->ib_size_dwords, &prepared->relocs, 0,
                          true);

   if (r3v_native_queue_write_submit_object(
          device, cmd_buffer->ib, cmd_buffer->ib_size_dwords,
          &prepared->relocs, &prepared->cs, cmd_buffer->references,
          prepared->reference_indices, cmd_buffer->reference_count,
          prepared->completion_index, prepared->completion.bo.handle,
          prepared->completion.bo.size, kernel_release, module_srcversion,
          serial_kind ? (int)device->serial_submissions_consumed : -1) !=
       0) {
      radeon_drm_vk_completion_finish(&device->drm, &prepared->completion);
      result = vk_errorf(device, VK_ERROR_DEVICE_LOST,
                         "r3v-native: submit-object evidence retention "
                         "failed; refusing before the ioctl");
      goto prepare_fail;
   }

   enum r3v_native_arming_verdict arming =
      r3v_native_arming_evaluate(&facts);
   if (arming != R3V_NATIVE_ARMING_ARMED) {
      radeon_drm_vk_completion_finish(&device->drm, &prepared->completion);
      result = vk_errorf(device, VK_ERROR_DEVICE_LOST,
                         "r3v-native: prepare refused: %s",
                         r3v_native_arming_verdict_name(arming));
      goto prepare_fail;
   }
   /* Authorization is consumed at prepare: the token lands and the
    * serial bound counts here, so a prepared submission that never
    * commits still spends its admission, the same fail-closed accounting
    * the inline path applies to a failed ioctl.
    */
   if (!facts.attempt_token_present &&
       r3v_native_arming_disarm(device->manifest_dir, ib_digest) != 0) {
      radeon_drm_vk_completion_finish(&device->drm, &prepared->completion);
      result = vk_errorf(device, VK_ERROR_DEVICE_LOST,
                         "r3v-native: one-shot disarm failed; refusing "
                         "before the ioctl");
      goto prepare_fail;
   }
   if (serial_kind)
      device->serial_submissions_consumed++;

   prepared->cmd_buffer = cmd_buffer;
   prepared->valid = true;
   return VK_SUCCESS;

prepare_fail:
   free(prepared->reference_indices);
   radeon_drm_vk_reloc_list_finish(&prepared->relocs);
   memset(prepared, 0, sizeof(*prepared));
   return result;
}

/* Commits the prepared submission: cache publication over the live
 * mappings, the ioctl, and the bounded completion wait -- the transport
 * tail alone, so a sampler window opened after prepare measures the
 * kernel submission instead of evidence journaling.  Releases the
 * prepared state on every path.
 */
static VkResult
r3v_native_queue_commit_prepared(struct r3v_native_device *device,
                                 struct r3v_native_cmd_buffer *cmd_buffer)
{
   struct r3v_native_prepared_submission *prepared = &device->prepared;

   for (uint32_t r = 0; r < cmd_buffer->reference_count; r++) {
      const struct r3v_native_bo_reference *reference =
         &cmd_buffer->references[r];
      if (reference->memory != NULL && reference->memory->map != NULL) {
         radeon_drm_vk_bo_cache_sync(&device->drm, reference->memory->map,
                                     reference->memory->bo.size);
      }
   }

   int trace_result = r3v_native_submission_trace_emit(
      device, R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_ENTER);
   if (trace_result != 0) {
      r3v_native_prepared_release(device);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: transport trace refused before the "
                       "submission ioctl");
   }
   /* The prepared path reports the same transport bracket the inline
    * path does, so one accessor describes both.
    */
   device->transport_gpu_producer_delivery =
      cmd_buffer->deferred_draw.gpu_producer_delivery;
   device->transport_cell_kind = cmd_buffer->cell_kind;
   device->transport_return_ns = 0;
   device->transport_enter_ns = r3v_native_raw_now_ns();
   int result = radeon_drm_vk_cs_submit(&device->drm, &prepared->cs);
   if (r3v_native_submission_trace_emit(
          device, R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_RETURN) != 0)
      trace_result = -EIO;
   const bool ioctl_accepted = result == 0;
   if (ioctl_accepted) {
      device->queue_status = R3V_NATIVE_QUEUE_STATUS_SUBMITTED;
      if (r3v_native_submission_trace_emit(
             device, R3V_NATIVE_SUBMISSION_TRACE_COMPLETION_WAIT_BEGIN) != 0)
         trace_result = -EIO;
      result = radeon_drm_vk_completion_await(&device->drm,
                                              &prepared->completion);
      if (r3v_native_submission_trace_emit(
             device,
             R3V_NATIVE_SUBMISSION_TRACE_COMPLETION_WAIT_RETURN) != 0)
         trace_result = -EIO;
   }
   device->transport_return_ns = r3v_native_raw_now_ns();
   if (result == 0 && trace_result != 0)
      result = trace_result;
   if (result == 0) {
      for (uint32_t r = 0; r < cmd_buffer->reference_count; r++) {
         const struct r3v_native_bo_reference *reference =
            &cmd_buffer->references[r];
         if (reference->memory != NULL && reference->memory->map != NULL) {
            radeon_drm_vk_bo_cache_sync(&device->drm, reference->memory->map,
                                        reference->memory->bo.size);
         }
      }
   }
   r3v_native_prepared_release(device);
   if (result != 0) {
      device->queue_status =
         r3v_native_queue_status_from_transport(ioctl_accepted, false);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: submission or completion wait failed: "
                       "%d", result);
   }
   device->queue_status =
      r3v_native_queue_status_from_transport(ioctl_accepted, true);
   return VK_SUCCESS;
}

VkResult
r3v_native_queue_submit(struct vk_queue *queue_base,
                        struct vk_queue_submit *submit)
{
   struct r3v_native_device *device =
      container_of(queue_base->base.device, struct r3v_native_device, vk);

   /* Every non-success return below is a refusal until the CS ioctl accepts
    * the command.  The completed and completion-failure states are installed
    * only at the transport boundary, so an API-level device-loss result keeps
    * its queue-phase meaning for the attended control.
    */
   device->queue_status = R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED;
   bool submit_has_executable_ib = false;

   /* An authorization declares one IB digest and its evidence directory
    * disarms after one attempt, so an open gate admits exactly one
    * executable command buffer.  Refusing the whole submit up front
    * keeps a multi-buffer submit from executing its first buffer and
    * then reporting device loss on the disarmed second.
    */
   if (device->submit_hazard_accepted || device->manifest_dir != NULL ||
       device->plan_capture_active || device->plan_replay_active) {
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

   /* r3v_cpu_sync_wait (rg --fixed-strings "r3v_cpu_sync_wait"
    * src/amd/r300/vulkan/r3v_cpu_sync.c) completes each declared dependency
    * before deferred CPU execution below.  The queue path
    * r3v_native_queue_submit (rg --fixed-strings "r3v_native_queue_submit"
    * src/amd/r300/vulkan/r3v_native_queue.c) consumes a permanent binary
    * wait after execution and signals the submit's completion set afterward.
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

      /* A prepared submission commits through its transport tail alone.
       * The prepared state binds one command buffer submitted by itself;
       * any other submit shape releases the state and refuses, because
       * the prepared evidence and consumed authorization describe
       * exactly that one transport.
       */
      if (device->prepared.valid) {
         if (device->prepared.cmd_buffer != cmd_buffer ||
             submit->command_buffer_count != 1) {
            r3v_native_prepared_release(device);
            return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                             "r3v-native: the prepared submission is bound "
                             "to a different submit shape; prepared state "
                             "released");
         }
         VkResult committed =
            r3v_native_queue_commit_prepared(device, cmd_buffer);
         if (committed != VK_SUCCESS)
            return committed;
         submit_has_executable_ib = true;
         continue;
      }

      /* GPU-producer admission precedes the reloc list, digest, and
       * manifest below: an admitted route rewrites the IB to
       * producer ++ consumer and widens the carrier's domain, so every
       * retained and armed fact describes the composed transport.  A
       * refused admission refuses the submit by name rather than
       * downgrading a route the caller opted into.
       */
      VkResult gpu_admit =
         r3v_native_deferred_draw_admit_gpu_producer(device, cmd_buffer);
      if (gpu_admit == VK_SUCCESS)
         gpu_admit = r3v_native_deferred_dispatch_admit_gpu(device, cmd_buffer);
      if (gpu_admit != VK_SUCCESS) {
         if (gpu_admit == VK_ERROR_OUT_OF_HOST_MEMORY ||
             gpu_admit == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            return gpu_admit;
         if (gpu_admit == VK_ERROR_MEMORY_MAP_FAILED)
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         return vk_error(device, VK_ERROR_DEVICE_LOST);
      }

      /* Recorded query transitions publish here, in recorded order:
       * an end makes its query available with the exact zero count,
       * a reset returns its range to unavailable.
       */
      for (uint32_t q = 0; q < cmd_buffer->query_op_count; q++) {
         const struct r3v_native_query_op *op = &cmd_buffer->query_ops[q];
         const uint8_t value =
            op->kind == R3V_NATIVE_QUERY_OP_MAKE_AVAILABLE ? 1 : 0;
         memset(&op->pool->state[op->first_query], value, op->query_count);
      }

      /* Recorded event transitions and waits execute here, in order:
       * a wait finding its event unsignaled is unsatisfiable on the
       * synchronous timeline and the submission is lost.
       */
      for (uint32_t e = 0; e < cmd_buffer->event_op_count; e++) {
         const struct r3v_native_event_op *op = &cmd_buffer->event_ops[e];
         switch (op->kind) {
         case R3V_NATIVE_EVENT_OP_SET:
            op->event->signaled = true;
            break;
         case R3V_NATIVE_EVENT_OP_RESET:
            op->event->signaled = false;
            break;
         case R3V_NATIVE_EVENT_OP_WAIT:
            if (!op->event->signaled)
               return vk_error(device, VK_ERROR_DEVICE_LOST);
            break;
         }
      }

      /* A zero-IB command buffer can still carry a deferred load-op clear
       * from an empty render pass, recorded transfer copies, or a
       * recorded dispatch on the CPU compute route.  Execute that
       * host-side work before treating the buffer as having no transport
       * submission.
       */
      if (cmd_buffer->ib_size_dwords == 0) {
         /* Recorded transfer copies execute per submission through host
          * mappings -- Vulkan's execution-time ordering, the same
          * contract the deferred draw holds.  The pre-draw group runs
          * ahead of the load-op clear it was recorded before.  The
          * runtime folds every driver_submit failure through
          * vk_queue_set_lost, so the application observes device loss
          * whatever code returns here; the loss is the honest verdict
          * for a submission whose earlier copies may already have
          * landed.
          */
         if (r3v_native_cmd_buffer_execute_deferred_copies(
                device, cmd_buffer,
                R3V_NATIVE_COPY_GROUP_BEFORE_DRAW) != VK_SUCCESS)
            return vk_error(device, VK_ERROR_DEVICE_LOST);

         VkResult dispatched =
            r3v_native_cmd_buffer_execute_deferred_dispatch(device,
                                                            cmd_buffer);
         if (dispatched != VK_SUCCESS) {
            if (dispatched == VK_ERROR_MEMORY_MAP_FAILED)
               return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
            return vk_error(device, VK_ERROR_DEVICE_LOST);
         }
         VkResult deferred =
            r3v_native_cmd_buffer_execute_deferred_draw(device, cmd_buffer);
         if (deferred != VK_SUCCESS) {
            if (deferred == VK_ERROR_MEMORY_MAP_FAILED)
               return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
            if (deferred != VK_ERROR_OUT_OF_HOST_MEMORY &&
                deferred != VK_ERROR_OUT_OF_DEVICE_MEMORY)
               return vk_error(device, VK_ERROR_DEVICE_LOST);
            return deferred;
         }

         /* The post-draw group reads what the clear published, so it
          * follows the deferred draw on this path as it follows the
          * completion wait on the transport path.
          */
         if (r3v_native_cmd_buffer_execute_deferred_copies(
                device, cmd_buffer,
                R3V_NATIVE_COPY_GROUP_AFTER_DRAW) != VK_SUCCESS)
            return vk_error(device, VK_ERROR_DEVICE_LOST);
         continue;
      }

      submit_has_executable_ib = true;

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

      char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
      char kernel_release[128];
      char module_srcversion[128];
      struct r3v_native_arming_facts facts = {0};
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
            device->pdevice->pci_device_id, cmd_buffer->cell_kind,
            ib_digest, device->manifest_dir, kernel_release,
            sizeof(kernel_release), module_srcversion,
            sizeof(module_srcversion));
         facts.nonmaximum_extent = cell_geometry_unfrozen(cmd_buffer);
         facts.serial_submissions_consumed =
            device->serial_submissions_consumed;
         facts.burst_recorded_draws = cmd_buffer->burst_draws;

         /* The serial kind admits its own token within the declared
          * bound; the full serial predicate is the evaluation below, and
          * this early refusal only keeps a foreign token from reaching
          * evidence retention.
          */
         const bool serial_continuation =
            cmd_buffer->cell_kind ==
               R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL &&
            device->serial_submissions_consumed > 0;
         if (facts.attempt_token_present && !serial_continuation) {
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

      /* The serial cell resubmits one unchanged semantic stream, so the
       * first admission retains ib.bin, relocs.bin, and manifest.json and
       * every later admission runs under that retained copy; the arming
       * gate re-proves the stream digest against the declared one each
       * admission, so the retained semantic cell stays bound to every
       * admission it covers.
       */
      const bool serial_kind =
         cmd_buffer->cell_kind ==
         R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL;
      const bool semantic_cell_retained =
         serial_kind && device->serial_submissions_consumed > 0;
      if (device->manifest_dir != NULL && !semantic_cell_retained &&
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
       * device loss.  The single CS build below folds the completion
       * reference in;
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
      /* The one CS build on the inline path: the completion reference
       * above finalized the relocation list, so the argument block binds
       * the exact chunks the ioctl sends.
       */
      struct radeon_drm_vk_cs cs;
      radeon_drm_vk_cs_build(&cs, cmd_buffer->ib,
                             cmd_buffer->ib_size_dwords, &relocs, 0, true);

      /* The submission ioctl opens only on the exact-value hazard gate; the
       * closed gate completes the full build, retains the manifest, and
       * fails closed with application memory untouched: the deferred
       * draw's writes come after every gate below.
       */
      if (!device->submit_hazard_accepted && !device->plan_capture_active &&
          !device->plan_replay_active) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission gate closed; set "
                          "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1 for an "
                          "attended run");
      }

      /* Plan capture records the whole entry the live replay must
       * present -- digest, dwords, kind, emitter, every relocation --
       * before the ioctl reaches the shim, and refuses the submission
       * when the entry cannot be recorded; the attended-run machinery
       * (submit-object retention, arming, disarm) belongs to the open
       * gate and stays out of a capture session.
       */
      /* Plan replay: the plan binds to the running identity at the
       * first submission, and every submission's whole entry is admitted
       * by the session before any device-visible effect; a refusal
       * latches the session and loses the queue.
       */
      if (device->plan_replay_active) {
         const char *refusal = NULL;
         if (!device->plan_replay.bound) {
            refusal = r3v_native_plan_replay_bind(
               &device->plan_replay,
               device->arming_provider != NULL
                  ? device->arming_provider
                  : r3v_native_arming_host_provider(),
               device->pdevice->pci_vendor_id,
               device->pdevice->pci_device_id);
         }
         if (refusal == NULL) {
            refusal = r3v_native_plan_replay_admit(
               &device->plan_replay, cmd_buffer, &relocs, reference_indices,
               completion_index, completion.bo.size, 1);
         }
         if (refusal != NULL) {
            free(reference_indices);
            radeon_drm_vk_completion_finish(&device->drm, &completion);
            radeon_drm_vk_reloc_list_finish(&relocs);
            return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                             "r3v-native: plan replay refused the "
                             "submission: %s", refusal);
         }
      }
      if (device->plan_capture_active) {
         int recorded = r3v_native_plan_capture_record(
            &device->plan_capture, cmd_buffer, &relocs, reference_indices,
            completion_index, completion.bo.size);
         if (recorded != 0) {
            free(reference_indices);
            radeon_drm_vk_completion_finish(&device->drm, &completion);
            radeon_drm_vk_reloc_list_finish(&relocs);
            if (recorded == -E2BIG) {
               return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                                "r3v-native: plan capture is full; split "
                                "the shard");
            }
            if (recorded == -EMSGSIZE) {
               return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                                "r3v-native: submission carries %u "
                                "relocations, outside the plan schema",
                                relocs.count);
            }
            return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                             "r3v-native: plan capture refused the "
                             "submission: %s", strerror(-recorded));
         }
      }

      /* The collected arming facts carry the same kernel and module identity
       * the gate below judges; the evaluation itself stays the last gate
       * before the ioctl.  The exact ioctl payload retains before that gate,
       * and a retention failure refuses with nothing sent.
       */
      if (!device->plan_capture_active && !device->plan_replay_active &&
          r3v_native_queue_write_submit_object(
             device, cmd_buffer->ib, cmd_buffer->ib_size_dwords, &relocs,
             &cs, cmd_buffer->references, reference_indices,
             cmd_buffer->reference_count, completion_index,
             completion.bo.handle, completion.bo.size, kernel_release,
             module_srcversion,
             serial_kind ? (int)device->serial_submissions_consumed
                         : -1) != 0) {
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
         device->plan_capture_active || device->plan_replay_active
            ? R3V_NATIVE_ARMING_ARMED
            : r3v_native_arming_evaluate(&facts);
      if (arming != R3V_NATIVE_ARMING_ARMED) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission refused: %s",
                          r3v_native_arming_verdict_name(arming));
      }

      /* The pre-draw copies land with the draw's own writes, past every
       * gate and ahead of the publish loop below, so a copy into a BO the
       * IB references reaches memory before the ioctl and a refused
       * submission leaves application memory untouched.
       */
      if (r3v_native_cmd_buffer_execute_deferred_copies(
             device, cmd_buffer,
             R3V_NATIVE_COPY_GROUP_BEFORE_DRAW) != VK_SUCCESS) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_error(device, VK_ERROR_DEVICE_LOST);
      }

      /* The public draw's vertex reads and load-op clear execute here,
       * per submission, so the stream bytes the carrier travels with are
       * the ones live at submit -- Vulkan's execution-time ordering.
       * Every gate, allocation, mapping, retention, and the arming
       * verdict precede this point, so a refused or failed submission
       * leaves the application's target and carrier bytes untouched, and
       * the one-shot disarm below follows it, so a refused draw spends no
       * authorization.
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

      /* The first admission writes the token; a serial continuation runs
       * under the token this instance already wrote, and the evaluation
       * above proved that pairing.  Every admission counts against the
       * serial bound before the ioctl, so a failed ioctl still consumes
       * its authorization.
       */
      if (!device->plan_capture_active && !device->plan_replay_active &&
          !facts.attempt_token_present &&
          r3v_native_arming_disarm(device->manifest_dir, ib_digest) != 0) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: one-shot disarm failed; refusing "
                          "before the ioctl");
      }
      if (cmd_buffer->cell_kind ==
          R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL)
         device->serial_submissions_consumed++;

      /* The IB at the ioctl boundary is the IB the session admitted. */
      if (device->plan_replay_active) {
         const char *rewritten = r3v_native_plan_replay_check_ib(
            &device->plan_replay, cmd_buffer->ib, cmd_buffer->ib_size_dwords);
         if (rewritten != NULL) {
            free(reference_indices);
            radeon_drm_vk_completion_finish(&device->drm, &completion);
            radeon_drm_vk_reloc_list_finish(&relocs);
            return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                             "r3v-native: plan replay refused the "
                             "submission: %s", rewritten);
         }
      }

      int trace_result = r3v_native_submission_trace_emit(
         device, R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_ENTER);
      if (trace_result != 0) {
         free(reference_indices);
         radeon_drm_vk_completion_finish(&device->drm, &completion);
         radeon_drm_vk_reloc_list_finish(&relocs);
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: transport trace refused before "
                          "the submission ioctl");
      }

      /* The transport bracket a route measurement reads: the ioctl and
       * the completion wait, with the evidence writes, the digest, the
       * completion allocation, and the arming reads left outside it.
       * The trace emission inside the bracket is one write to an already
       * open transcript, and it runs identically on every route.
       */
      device->transport_gpu_producer_delivery =
         cmd_buffer->deferred_draw.gpu_producer_delivery;
      device->transport_cell_kind = cmd_buffer->cell_kind;
      device->transport_return_ns = 0;
      device->transport_enter_ns = r3v_native_raw_now_ns();
      int result = radeon_drm_vk_cs_submit(&device->drm, &cs);
      if (r3v_native_submission_trace_emit(
             device, R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_RETURN) != 0)
         trace_result = -EIO;
      const bool ioctl_accepted = result == 0;
      if (ioctl_accepted) {
         device->queue_status = R3V_NATIVE_QUEUE_STATUS_SUBMITTED;
         if (r3v_native_submission_trace_emit(
                device,
                R3V_NATIVE_SUBMISSION_TRACE_COMPLETION_WAIT_BEGIN) != 0)
            trace_result = -EIO;
         result = radeon_drm_vk_completion_await(&device->drm, &completion);
         if (r3v_native_submission_trace_emit(
                device,
                R3V_NATIVE_SUBMISSION_TRACE_COMPLETION_WAIT_RETURN) != 0)
            trace_result = -EIO;
      }
      device->transport_return_ns = r3v_native_raw_now_ns();
      if (result == 0 && trace_result != 0)
         result = trace_result;
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
         device->queue_status =
            r3v_native_queue_status_from_transport(ioctl_accepted, false);
         if (device->plan_replay_active) {
            r3v_native_plan_replay_fail(&device->plan_replay,
                                        ioctl_accepted ? "completion"
                                                       : "ioctl",
                                        result);
         }
         return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                          "r3v-native: submission or completion wait "
                          "failed: %d", result);
      }
      /* The completed GPU-producer delivery earns the routed verdict
       * here: the carrier read-back against the retained CPU oracle,
       * with a divergence quarantining the capability.
       */
      VkResult gpu_verdict =
         r3v_native_deferred_draw_verify_gpu_producer(device, cmd_buffer);
      if (gpu_verdict == VK_SUCCESS)
         gpu_verdict =
            r3v_native_deferred_dispatch_verify_gpu(device, cmd_buffer);
      if (gpu_verdict != VK_SUCCESS)
         return gpu_verdict;

      /* The post-draw copies read the completed submission: execution
       * here is past the completion wait, and execute_copy's own source
       * invalidate drops the stale lines over the mapping it takes, so a
       * read of the render target observes the device output.
       */
      if (r3v_native_cmd_buffer_execute_deferred_copies(
             device, cmd_buffer,
             R3V_NATIVE_COPY_GROUP_AFTER_DRAW) != VK_SUCCESS)
         return vk_error(device, VK_ERROR_DEVICE_LOST);
      /* The transcript lands on the capture cadence after a completed
       * submission, so a planning pass cut short keeps the entries that
       * landed; a write failure loses the queue, since a plan missing an
       * entry that ran would replay short.
       */
      if (device->plan_capture_active &&
          r3v_native_plan_capture_lands_now(&device->plan_capture)) {
         int written = r3v_native_plan_capture_write(
            &device->plan_capture, device->pdevice->pci_vendor_id,
            device->pdevice->pci_device_id, NULL);
         if (written != 0) {
            return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                             "r3v-native: plan transcript write failed: %s",
                             strerror(-written));
         }
      }
      device->queue_status =
         r3v_native_queue_status_from_transport(ioctl_accepted, true);
   }

   device->queue_status = r3v_native_queue_status_finalize_submit(
      device->queue_status, submit_has_executable_ib);

   /* The bounded completion wait above retired every buffer.  Consuming
    * permanent binary waits before signaling keeps the semaphore state ready
    * for its next Vulkan signal operation; a same-submit signal remains live.
    */
   VkResult consume_result =
      r3v_native_queue_consume_binary_waits(device, submit);
   if (consume_result != VK_SUCCESS)
      return consume_result;

   return vk_sync_signal_many(&device->vk, submit->signal_count,
                              submit->signals);
}
