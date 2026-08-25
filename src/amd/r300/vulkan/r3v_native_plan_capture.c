/*
 * SPDX-License-Identifier: MIT
 *
 * Plan capture: records every executable submission under the drm-shim
 * host model as an ordered-plan transcript.
 */

#include "r3v_native.h"

#include "r3v_native_identity.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/* The host-model proof: the ioctl symbol the process resolves belongs
 * to the preloaded radeon drm-shim, which interposes it.  Interposition
 * is a property of the process whatever profile built the shim, so the
 * proof holds for the release shim as for the debug one; a capture
 * session opens the CS ioctl with the hazard gate closed, and that is
 * admissible only when this interposer answers it.
 */
bool
r3v_native_plan_capture_host_model_present(void)
{
   void *sym = dlsym(RTLD_DEFAULT, "ioctl");
   Dl_info info;
   if (sym == NULL || dladdr(sym, &info) == 0 || info.dli_fname == NULL)
      return false;
   const char *base = strrchr(info.dli_fname, '/');
   base = base != NULL ? base + 1 : info.dli_fname;
   return strcmp(base, "libradeon_noop_drm_shim.so") == 0;
}

/* One transcript per device, addressed by a process-wide ordinal: a
 * dEQP case creates devices freely (vktRobustnessBufferAccessTests.cpp
 * calls createRobustBufferAccessDevice for its own device ahead of the
 * shared context device), so a fixed once-per-process claim would
 * refuse the second vkCreateDevice outright.  The transcript's process
 * identity the runner needs is the case, already carried by the
 * `{case}` template in the declared capture path, so each device
 * within that one process instead claims its own ordinal: the first
 * writes the declared path verbatim, and the Nth (N >= 2) writes
 * `<path>.<N-1>`.  atomic_fetch_add assigns the ordinal since
 * vkCreateDevice carries no external synchronization.
 */
static atomic_uint capture_next_ordinal = ATOMIC_VAR_INIT(0);

/* Relocation roles by cell kind and reference slot, the slot enums the
 * cell emitters bind their references in; a slot past a kind's table is
 * named by index so the entry still distinguishes it.
 */
static const char *const triangle_roles[] = {"vertex", "color"};
/* The public producer route rides the triangle slots with the carrier
 * at the vertex slot, written by the producer and read by the consumer.
 */
static const char *const public_route_roles[] = {"carrier", "color"};
static const char *const direct_write_roles[] = {"color"};
static const char *const producer_roles[] = {"carrier"};
static const char *const reingest_roles[] = {"carrier", "color"};
static const char *const float2_tuple_roles[] = {"carrier", "vertex"};
static const char *const fetched_roles[] = {"carrier", "slot", "source"};
static const char *const zb_depth_roles[] = {"vertex", "color", "depth"};

void
r3v_native_plan_capture_slot_role(enum r3v_native_cell_kind kind,
                                  uint32_t slot, char *out)
{
   const char *const *table = NULL;
   size_t count = 0;
#define TABLE(t) do { table = t; count = sizeof(t) / sizeof(t[0]); } while (0)
   switch (kind) {
   case R3V_NATIVE_CELL_KIND_TRIANGLE:
   case R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE:
      TABLE(triangle_roles);
      break;
   case R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC:
      TABLE(public_route_roles);
      break;
   case R3V_NATIVE_CELL_KIND_DIRECT_WRITE:
      TABLE(direct_write_roles);
      break;
   case R3V_NATIVE_CELL_KIND_R2VB_PRODUCER:
      TABLE(producer_roles);
      break;
   case R3V_NATIVE_CELL_KIND_R2VB_REINGEST:
      TABLE(reingest_roles);
      break;
   case R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE:
   case R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL:
   case R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST:
      TABLE(float2_tuple_roles);
      break;
   case R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED:
      TABLE(fetched_roles);
      break;
   case R3V_NATIVE_CELL_KIND_ZB_DEPTH_CONTROL:
      TABLE(zb_depth_roles);
      break;
   default:
      break;
   }
#undef TABLE
   if (slot < count)
      snprintf(out, R3V_NATIVE_PLAN_NAME_MAX + 1, "%s", table[slot]);
   else
      snprintf(out, R3V_NATIVE_PLAN_NAME_MAX + 1, "command%u", slot);
}

int
r3v_native_plan_capture_init(struct r3v_native_plan_capture *capture,
                             const char *path)
{
   memset(capture, 0, sizeof(*capture));
   if (path == NULL || path[0] != '/' ||
       strlen(path) > R3V_NATIVE_PLAN_PATH_MAX)
      return -EINVAL;
   /* The ordinal names this device's transcript path; a derived path
    * past the schema ceiling refuses here rather than truncating.
    */
   unsigned ordinal = atomic_fetch_add(&capture_next_ordinal, 1);
   char derived[R3V_NATIVE_PLAN_PATH_MAX + 16];
   if (ordinal == 0)
      snprintf(derived, sizeof(derived), "%s", path);
   else
      snprintf(derived, sizeof(derived), "%s.%u", path, ordinal);
   if (strlen(derived) > R3V_NATIVE_PLAN_PATH_MAX)
      return -ENAMETOOLONG;
   /* The transcript must land in a directory that exists before the
    * first submission, so a misdirected capture refuses at device
    * creation rather than after a shard ran.
    */
   char dir[R3V_NATIVE_PLAN_PATH_MAX + 1];
   strcpy(dir, derived);
   char *slash = strrchr(dir, '/');
   if (slash == dir)
      return -EINVAL;
   *slash = '\0';
   if (access(dir, W_OK | X_OK) != 0)
      return -errno;
   if (access(derived, F_OK) == 0)
      return -EEXIST;
   capture->path = strdup(derived);
   if (capture->path == NULL)
      return -ENOMEM;
   return 0;
}

/* A capture that recorded zero executable submissions writes no
 * transcript, and a reader cannot tell that absence from a device that
 * never reached destroy, so the device leaves an explicit marker beside
 * the transcript path instead: `<path>.no_nonempty_ib`, one line, created
 * exclusively.  The runner reads the marker as the case's outcome, and a
 * suffix outside the digit ordinals keeps it out of a transcript family.
 */
int
r3v_native_plan_capture_mark_empty(const struct r3v_native_plan_capture *c)
{
   char marker[R3V_NATIVE_PLAN_PATH_MAX + 32];
   snprintf(marker, sizeof(marker), "%s.no_nonempty_ib", c->path);
   int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0644);
   if (fd < 0)
      return -errno;
   static const char line[] = "no_nonempty_ib\n";
   ssize_t n = write(fd, line, sizeof(line) - 1);
   int saved = n == (ssize_t)(sizeof(line) - 1) ? 0 : (n < 0 ? -errno : -EIO);
   close(fd);
   return saved;
}

void
r3v_native_plan_capture_finish(struct r3v_native_plan_capture *capture)
{
   free(capture->entries);
   free(capture->path);
   memset(capture, 0, sizeof(*capture));
}

/* Builds the plan entry a submission presents: the IB digest and dword
 * count, the cell kind, the emitter, and every relocation as role from
 * the cell's slot order, domains, size, and direction, the completion
 * object last.  Returns 0 or a negative errno.
 */
int
r3v_native_plan_entry_from_submission(
   struct r3v_native_plan_submission *e,
   const struct r3v_native_cmd_buffer *cmd_buffer,
   const struct radeon_drm_vk_reloc_list *relocs,
   const uint32_t *reference_indices, uint32_t completion_index,
   uint64_t completion_size)
{
   if (relocs->count == 0 || relocs->count > R3V_NATIVE_PLAN_RELOC_MAX)
      return -EMSGSIZE;
   memset(e, 0, sizeof(*e));
   r300_triangle_ib_digest_hex(cmd_buffer->ib, cmd_buffer->ib_size_dwords,
                               e->ib_blake3);
   e->ib_dwords = cmd_buffer->ib_size_dwords;
   e->cell_kind = cmd_buffer->cell_kind;
   strcpy(e->emitter, "r3v");
   e->reloc_count = relocs->count;
   for (uint32_t i = 0; i < relocs->count; i++) {
      struct r3v_native_plan_reloc *r = &e->relocs[i];
      const struct drm_radeon_cs_reloc *raw = &relocs->relocs[i];
      r->read_domains = raw->read_domains;
      r->write_domain = raw->write_domain;
      r->direction = (raw->read_domains ? R3V_NATIVE_PLAN_DIRECTION_READ : 0) |
                     (raw->write_domain ? R3V_NATIVE_PLAN_DIRECTION_WRITE : 0);
      /* A relocation with no domain carries no access, which no emitter
       * produces; recording it would invent a direction.
       */
      if (r->direction == 0)
         return -EINVAL;
      if (i == completion_index) {
         strcpy(r->role, "completion");
         r->size = completion_size;
         continue;
      }
      /* A command-buffer reference maps to its relocation slot through
       * reference_indices; the first reference naming the slot gives
       * the role (its slot in the cell's reference order) and the
       * memory the slot binds.  Every reference sharing a slot shares
       * its GEM handle, and a handle names one memory object.
       */
      r->size = 0;
      for (uint32_t k = 0; k < cmd_buffer->reference_count; k++) {
         if (reference_indices[k] == i) {
            const struct r3v_native_memory *memory =
               cmd_buffer->references[k].memory;
            r->size = memory != NULL ? memory->bo.size : 0;
            r3v_native_plan_capture_slot_role(cmd_buffer->cell_kind, k,
                                              r->role);
            break;
         }
      }
      if (r->size == 0)
         return -EINVAL;
   }
   return 0;
}

int
r3v_native_plan_capture_record(struct r3v_native_plan_capture *capture,
                               const struct r3v_native_cmd_buffer *cmd_buffer,
                               const struct radeon_drm_vk_reloc_list *relocs,
                               const uint32_t *reference_indices,
                               uint32_t completion_index,
                               uint64_t completion_size)
{
   /* An entry above the per-entry relocation ceiling is outside the
    * schema whatever the shard; a capture at the submission ceiling is
    * full and the shard splits.
    */
   if (relocs->count == 0 || relocs->count > R3V_NATIVE_PLAN_RELOC_MAX)
      return -EMSGSIZE;
   if (capture->count == R3V_NATIVE_PLAN_SUBMISSION_MAX)
      return -E2BIG;
   if (capture->count == capture->capacity) {
      uint32_t next = capture->capacity == 0 ? 16 : capture->capacity * 2;
      struct r3v_native_plan_submission *grown =
         realloc(capture->entries, (size_t)next * sizeof(*grown));
      if (grown == NULL)
         return -ENOMEM;
      capture->entries = grown;
      capture->capacity = next;
   }
   int built = r3v_native_plan_entry_from_submission(
      &capture->entries[capture->count], cmd_buffer, relocs,
      reference_indices, completion_index, completion_size);
   if (built != 0)
      return built;
   capture->count++;
   return 0;
}

/* Whether the transcript lands after this submission: every one of the
 * first 256, then every 256th, since the seal covers the whole body and
 * each landing rewrites it; the final landing at device destruction
 * covers the tail, and a planning pass runs on the host, where process
 * death is the failure the cadence bounds.
 */
bool
r3v_native_plan_capture_lands_now(const struct r3v_native_plan_capture *c)
{
   return c->count <= 256 || c->count % 256 == 0;
}

/* Writes the transcript: a plan whose identity fields the driver can
 * name (kernel release, module srcversion, PCI identity, DSO digest)
 * are filled and whose run identities (source, dEQP, partition,
 * caselist, nonce, evidence directory) are placeholders the compose step
 * replaces; the ceilings are the observed maxima.  The file lands by
 * rename with the directory synced after it, so a reader sees the
 * previous complete transcript or this one.  Zero entries write no
 * file: an empty transcript is outside the plan schema, and the caller
 * reports the absence.
 */
int
r3v_native_plan_capture_write(struct r3v_native_plan_capture *capture,
                              uint32_t pci_vendor_id, uint32_t pci_device_id,
                              const char *module_srcversion)
{
   if (capture->count == 0)
      return -ENOENT;
   struct r3v_native_plan plan = {
      .schema_version = R3V_NATIVE_PLAN_SCHEMA_VERSION,
      .source_clean = false,
      .queue_claim = R3V_NATIVE_PLAN_QUEUE_DEFAULT_GRAPHICS_ONLY,
      .pci_vendor_id = pci_vendor_id,
      .pci_device_id = pci_device_id,
      .max_submissions = capture->count,
      .submission_count = capture->count,
      .submissions = capture->entries,
      .max_runtime_seconds = 1,
   };
   memset(plan.source_sha, '0', 40);
   memset(plan.dso_blake3, '0', R3V_NATIVE_PLAN_HEX64);
   memset(plan.deqp_sha256, '0', R3V_NATIVE_PLAN_HEX64);
   memset(plan.partition_sha256, '0', R3V_NATIVE_PLAN_HEX64);
   memset(plan.caselist_sha256, '0', R3V_NATIVE_PLAN_HEX64);
   memset(plan.nonce, '0', 32);
   strcpy(plan.deqp_release, "transcript");
   strcpy(plan.evidence_dir, "/");
   struct utsname host;
   if (uname(&host) != 0 || strlen(host.release) > R3V_NATIVE_PLAN_NAME_MAX)
      return -EIO;
   strcpy(plan.kernel_release, host.release);
   strcpy(plan.module_srcversion,
          module_srcversion != NULL && module_srcversion[0] != '\0' &&
                strlen(module_srcversion) <= R3V_NATIVE_PLAN_NAME_MAX
             ? module_srcversion
             : "unloaded");
   char dso_path[4096];
   char dso_digest[R3V_NATIVE_PLAN_HEX64 + 1];
   if (r3v_native_identity_collect(dso_path, sizeof(dso_path), dso_digest,
                                   sizeof(dso_digest)) == 0)
      memcpy(plan.dso_blake3, dso_digest, R3V_NATIVE_PLAN_HEX64 + 1);
   uint64_t bytes = 0;
   for (uint32_t i = 0; i < capture->count; i++) {
      const struct r3v_native_plan_submission *e = &capture->entries[i];
      if (e->ib_dwords > plan.max_ib_dwords)
         plan.max_ib_dwords = e->ib_dwords;
      if (e->reloc_count > plan.max_relocs)
         plan.max_relocs = e->reloc_count;
      for (uint32_t r = 0; r < e->reloc_count; r++)
         bytes += e->relocs[r].size;
   }
   plan.max_cumulative_bytes = bytes;
   long size = r3v_native_plan_write(&plan, NULL, 0);
   if (size <= 0)
      return -EINVAL;
   char *text = malloc((size_t)size);
   if (text == NULL)
      return -ENOMEM;
   long written = r3v_native_plan_write(&plan, text, (size_t)size);
   int result = written == size ? 0 : -EIO;
   if (result == 0) {
      char tmp[R3V_NATIVE_PLAN_PATH_MAX + 8];
      snprintf(tmp, sizeof(tmp), "%s.tmp", capture->path);
      FILE *f = fopen(tmp, "wb");
      if (f == NULL) {
         result = -errno;
      } else {
         if (fwrite(text, 1, (size_t)size, f) != (size_t)size ||
             fflush(f) != 0 || fsync(fileno(f)) != 0)
            result = -EIO;
         if (fclose(f) != 0 && result == 0)
            result = -EIO;
         if (result == 0 && rename(tmp, capture->path) != 0)
            result = -errno;
         if (result != 0)
            unlink(tmp);
      }
      if (result == 0) {
         char dir[R3V_NATIVE_PLAN_PATH_MAX + 1];
         strcpy(dir, capture->path);
         *strrchr(dir, '/') = '\0';
         int dfd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY);
         if (dfd < 0 || fsync(dfd) != 0)
            result = -EIO;
         if (dfd >= 0)
            close(dfd);
      }
   }
   free(text);
   return result;
}
