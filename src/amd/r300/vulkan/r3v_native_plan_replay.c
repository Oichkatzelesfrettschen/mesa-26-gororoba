/*
 * SPDX-License-Identifier: MIT
 *
 * Plan replay: the exact ordered-plan authorization that opens the CS
 * ioctl for one conformance shard, one submission at a time.
 */

#include "r3v_native.h"
#include "r3v_native_identity.h"

#include "amd/r300/common/r300_compute_verb.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "git_sha1.h"
#include "util/mesa-blake3.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The source SHA the DSO was built from, as git_sha1.h records it: the
 * short hash inside " (git-XXXXXXXXXX)", or empty when the build carried
 * no git identity, which refuses every plan.
 */
static const char *
built_source_sha_prefix(char out[41])
{
   const char *tag = strstr(MESA_GIT_SHA1, "git-");
   out[0] = '\0';
   if (tag == NULL)
      return out;
   size_t n = strspn(tag + 4, "0123456789abcdef");
   if (n < 7 || n > 40)
      return out;
   memcpy(out, tag + 4, n);
   out[n] = '\0';
   return out;
}

static uint64_t
monotonic_seconds(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec;
}

/* Syncs the evidence directory so a rename or a creation inside it is
 * durable across a power loss on a target that locks up hard.
 */
static int
sync_directory(const char *dir)
{
   int dfd = open(dir, O_RDONLY | O_DIRECTORY);
   if (dfd < 0)
      return -errno;
   int result = fsync(dfd) == 0 ? 0 : -errno;
   close(dfd);
   return result;
}

/* The bound record is created with O_EXCL, so two sessions racing one
 * evidence directory cannot both bind; later state updates rewrite the
 * record the session owns.
 */
static int
write_state(const struct r3v_native_plan_replay *replay, const char *state,
            const char *detail)
{
   char path[R3V_NATIVE_PLAN_PATH_MAX + 32];
   snprintf(path, sizeof(path), "%s/session.state", replay->evidence_dir);
   const bool creating = strcmp(state, "bound") == 0;
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC |
                          (creating ? O_EXCL : 0), 0644);
   if (fd < 0)
      return -errno;
   FILE *f = fdopen(fd, "w");
   if (f == NULL) {
      close(fd);
      return -errno;
   }
   int result = 0;
   if (fprintf(f, "%s\t%s\t%u\t%s\n", state, replay->plan.nonce,
               replay->session.next_index, detail != NULL ? detail : "-") < 0 ||
       fflush(f) != 0 || fsync(fileno(f)) != 0)
      result = -EIO;
   if (fclose(f) != 0 && result == 0)
      result = -EIO;
   if (result == 0 && creating)
      result = sync_directory(replay->evidence_dir);
   return result;
}

/* Empty means every entry enumerated and none besides the two dots; an
 * enumeration error reads as occupied.
 */
static bool
directory_empty(const char *path, bool *present)
{
   DIR *d = opendir(path);
   *present = d != NULL;
   if (d == NULL)
      return false;
   bool empty = true;
   struct dirent *ent;
   errno = 0;
   while ((ent = readdir(d)) != NULL) {
      if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
         empty = false;
         break;
      }
   }
   if (errno != 0)
      empty = false;
   closedir(d);
   return empty;
}

/* The gate enumeration with the compute-queue gate masked: that gate is
 * the plan's declared claim mode, judged separately, and every other
 * open gate is contamination whatever the enumeration order.
 */
struct masked_env {
   const struct r3v_native_arming_provider *provider;
};

static const char *
masked_read_env(void *ctx, const char *name)
{
   const struct masked_env *m = ctx;
   if (strcmp(name, R300_COMPUTE_QUEUE_CLAIM_GATE) == 0)
      return NULL;
   return m->provider->read_env(m->provider->ctx, name);
}

int
r3v_native_plan_replay_init(struct r3v_native_plan_replay *replay,
                            const char *plan_path, const char *nonce)
{
   memset(replay, 0, sizeof(*replay));
   if (plan_path == NULL || plan_path[0] != '/' || nonce == NULL ||
       strlen(nonce) != 32)
      return -EINVAL;
   FILE *f = fopen(plan_path, "rb");
   if (f == NULL)
      return -errno;
   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size <= 0 || size > (16 << 20)) {
      fclose(f);
      return -EFBIG;
   }
   char *text = malloc((size_t)size);
   if (text == NULL) {
      fclose(f);
      return -ENOMEM;
   }
   size_t got = fread(text, 1, (size_t)size, f);
   fclose(f);
   if (got != (size_t)size) {
      free(text);
      return -EIO;
   }
   enum r3v_native_plan_parse_result parsed =
      r3v_native_plan_parse(text, (size_t)size, &replay->plan);
   free(text);
   if (parsed != R3V_NATIVE_PLAN_PARSE_OK) {
      replay->parse_result = parsed;
      return -EPROTO;
   }
   replay->evidence_dir = strdup(replay->plan.evidence_dir);
   replay->nonce = strdup(nonce);
   if (replay->evidence_dir == NULL || replay->nonce == NULL) {
      r3v_native_plan_replay_finish(replay);
      return -ENOMEM;
   }
   r3v_native_plan_session_init(&replay->session);
   return 0;
}

const char *
r3v_native_plan_replay_bind(struct r3v_native_plan_replay *replay,
                            const struct r3v_native_arming_provider *provider,
                            uint32_t pci_vendor_id, uint32_t pci_device_id)
{
   char source[41];
   built_source_sha_prefix(source);
   char dso_path[4096];
   char dso_digest[R3V_NATIVE_PLAN_HEX64 + 1];
   const char *dso = r3v_native_identity_collect(dso_path, sizeof(dso_path),
                                                 dso_digest,
                                                 sizeof(dso_digest)) == 0
                        ? dso_digest
                        : NULL;
   char kernel[128] = "";
   char module[128] = "";
   provider->read_kernel_release(provider->ctx, kernel, sizeof(kernel));
   provider->read_module_srcversion(provider->ctx, module, sizeof(module));
   const char *gate = provider->read_env(provider->ctx,
                                         R300_COMPUTE_QUEUE_CLAIM_GATE);
   const bool gate_open =
      gate != NULL && strcmp(gate, R300_COMPUTE_QUEUE_CLAIM_GATE_VALUE) == 0;
   enum r3v_native_plan_queue_claim claim =
      r300_compute_dual_route_coverage_complete()
         ? R3V_NATIVE_PLAN_QUEUE_CONFORMANT
      : r300_compute_verb_queue_claim(gate_open)
         ? R3V_NATIVE_PLAN_QUEUE_EXPERIMENTAL_COMPUTE_SUBSET
         : R3V_NATIVE_PLAN_QUEUE_DEFAULT_GRAPHICS_ONLY;
   bool present;
   bool empty = directory_empty(replay->evidence_dir, &present);
   const char *open_gate = NULL;
   struct masked_env masked = { .provider = provider };
   bool gates_open = r3v_native_plan_gates_open(masked_read_env, &masked,
                                                &open_gate);
   /* The plan's source SHA binds when the DSO's built identity is a
    * prefix of it; the dEQP, partition, and caselist digests are the
    * runner's declarations, verified against the receipt outside the
    * driver, so the bind takes the plan's own values for them.
    */
   struct r3v_native_plan_identity id = {
      .source_sha = source[0] != '\0' &&
                          strncmp(replay->plan.source_sha, source,
                                  strlen(source)) == 0
                       ? replay->plan.source_sha
                       : NULL,
      .source_clean = replay->plan.source_clean,
      .dso_blake3 = dso,
      .deqp_sha256 = replay->plan.deqp_sha256,
      .deqp_release = replay->plan.deqp_release,
      .partition_sha256 = replay->plan.partition_sha256,
      .caselist_sha256 = replay->plan.caselist_sha256,
      .queue_claim = claim,
      .kernel_release = kernel[0] != '\0' ? kernel : NULL,
      .module_srcversion = module[0] != '\0' ? module : NULL,
      .pci_vendor_id = pci_vendor_id,
      .pci_device_id = pci_device_id,
      .nonce = replay->nonce,
      .evidence_dir_present = present,
      .evidence_dir_empty = empty,
      .gates_open = gates_open,
   };
   enum r3v_native_plan_bind_result bound =
      r3v_native_plan_bind(&replay->plan, &id);
   if (bound != R3V_NATIVE_PLAN_BIND_OK) {
      replay->bind_result = bound;
      replay->refused = true;
      return r3v_native_plan_bind_result_name(bound);
   }
   if (r3v_native_plan_session_bind(&replay->session, &replay->plan) !=
       R3V_NATIVE_PLAN_SESSION_ADMITTED) {
      replay->refused = true;
      return "session";
   }
   /* The bound state lands before the first submission, so the
    * evidence directory is occupied from here on and a second session
    * over it refuses at its own bind.
    */
   if (write_state(replay, "bound", NULL) != 0) {
      replay->refused = true;
      replay->bind_result = R3V_NATIVE_PLAN_BIND_EVIDENCE_DIR;
      return "evidence_dir";
   }
   replay->bound = true;
   replay->bound_seconds = monotonic_seconds();
   memset(replay->chain_hex, '0', R3V_NATIVE_PLAN_HEX64);
   replay->chain_hex[R3V_NATIVE_PLAN_HEX64] = '\0';
   return NULL;
}

/* Retains the admitted IB by content and extends the hash chain: the
 * IB lands under ib/<blake3>.bin once whatever its occurrence count,
 * and chain.log gains one line per occurrence whose digest covers the
 * previous chain digest and the entry, so the chain proves order and
 * count and the store proves bytes.
 */
static int
retain(struct r3v_native_plan_replay *replay,
       const struct r3v_native_plan_submission *entry, const uint32_t *ib)
{
   char path[R3V_NATIVE_PLAN_PATH_MAX + 96];
   snprintf(path, sizeof(path), "%s/ib", replay->evidence_dir);
   if (mkdir(path, 0755) != 0 && errno != EEXIST)
      return -errno;
   snprintf(path, sizeof(path), "%s/ib/%s.bin", replay->evidence_dir,
            entry->ib_blake3);
   if (access(path, F_OK) != 0) {
      size_t bytes = (size_t)entry->ib_dwords * sizeof(uint32_t);
      uint8_t *raw = malloc(bytes);
      if (raw == NULL)
         return -ENOMEM;
      r300_triangle_ib_serialize(ib, entry->ib_dwords, raw);
      char tmp[R3V_NATIVE_PLAN_PATH_MAX + 104];
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      FILE *f = fopen(tmp, "wb");
      int result = f == NULL ? -errno : 0;
      if (result == 0) {
         if (fwrite(raw, 1, bytes, f) != bytes || fflush(f) != 0 ||
             fsync(fileno(f)) != 0)
            result = -EIO;
         if (fclose(f) != 0 && result == 0)
            result = -EIO;
         if (result == 0 && rename(tmp, path) != 0)
            result = -errno;
         if (result != 0)
            unlink(tmp);
      }
      free(raw);
      if (result == 0)
         result = sync_directory(replay->evidence_dir);
      if (result != 0)
         return result;
   }
   char line[512];
   int n = snprintf(line, sizeof(line), "%u\t%s\t%u\t%s\t%u\t%s\n",
                    replay->session.next_index - 1, entry->ib_blake3,
                    entry->ib_dwords,
                    r3v_native_plan_cell_kind_name(entry->cell_kind),
                    entry->reloc_count, replay->chain_hex);
   if (n <= 0 || (size_t)n >= sizeof(line))
      return -EIO;
   struct mesa_blake3 ctx;
   uint8_t digest[BLAKE3_OUT_LEN];
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, replay->chain_hex, R3V_NATIVE_PLAN_HEX64);
   _mesa_blake3_update(&ctx, line, (size_t)n);
   _mesa_blake3_final(&ctx, digest);
   _mesa_blake3_format(replay->chain_hex, digest);
   snprintf(path, sizeof(path), "%s/chain.log", replay->evidence_dir);
   int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
   if (fd < 0)
      return -errno;
   char record[600];
   int m = snprintf(record, sizeof(record), "%.*s\t%s\n", n - 1, line,
                    replay->chain_hex);
   int result = 0;
   if (m <= 0 || write(fd, record, (size_t)m) != m || fsync(fd) != 0)
      result = -EIO;
   close(fd);
   if (result == 0)
      result = sync_directory(replay->evidence_dir);
   return result;
}

const char *
r3v_native_plan_replay_admit(struct r3v_native_plan_replay *replay,
                             const struct r3v_native_cmd_buffer *cmd_buffer,
                             const struct radeon_drm_vk_reloc_list *relocs,
                             const uint32_t *reference_indices,
                             uint32_t completion_index,
                             uint64_t completion_size,
                             uint32_t executable_count)
{
   if (!replay->bound)
      return "unbound";
   struct r3v_native_plan_submission actual;
   int built = r3v_native_plan_entry_from_submission(
      &actual, cmd_buffer, relocs, reference_indices, completion_index,
      completion_size);
   if (built != 0) {
      r3v_native_plan_session_fail(&replay->session,
                                   R3V_NATIVE_PLAN_SESSION_MISMATCH);
      write_state(replay, "terminal", "entry_outside_schema");
      return "entry_outside_schema";
   }
   enum r3v_native_plan_session_result admitted =
      r3v_native_plan_session_admit(
         &replay->session, &actual, executable_count,
         monotonic_seconds() - replay->bound_seconds);
   if (admitted != R3V_NATIVE_PLAN_SESSION_ADMITTED) {
      const char *name = r3v_native_plan_session_result_name(admitted);
      char detail[128];
      snprintf(detail, sizeof(detail), "%s%s%s", name,
               admitted == R3V_NATIVE_PLAN_SESSION_MISMATCH ? ":" : "",
               admitted == R3V_NATIVE_PLAN_SESSION_MISMATCH
                  ? r3v_native_plan_match_result_name(
                       replay->session.last_mismatch)
                  : "");
      write_state(replay, "terminal", detail);
      snprintf(replay->last_refusal, sizeof(replay->last_refusal), "%s",
               detail);
      return replay->last_refusal;
   }
   int retained = retain(replay, &actual, cmd_buffer->ib);
   if (retained != 0) {
      r3v_native_plan_session_fail(&replay->session,
                                   R3V_NATIVE_PLAN_SESSION_TERMINAL);
      write_state(replay, "terminal", "retention");
      return "retention";
   }
   memcpy(replay->admitted_ib_blake3, actual.ib_blake3,
          sizeof(replay->admitted_ib_blake3));
   return NULL;
}

/* The bytes the ioctl sends are the bytes the session admitted: the
 * digest is recomputed over the IB at the ioctl boundary and held to the
 * admitted entry, so a rewrite between admission and submission refuses
 * and latches.
 */
const char *
r3v_native_plan_replay_check_ib(struct r3v_native_plan_replay *replay,
                                const uint32_t *ib, uint32_t ib_size_dwords)
{
   char digest[R3V_NATIVE_PLAN_HEX64 + 1];
   r300_triangle_ib_digest_hex(ib, ib_size_dwords, digest);
   if (strcmp(digest, replay->admitted_ib_blake3) != 0) {
      r3v_native_plan_session_fail(&replay->session,
                                   R3V_NATIVE_PLAN_SESSION_MISMATCH);
      write_state(replay, "terminal", "ib_rewritten_after_admission");
      return "ib_rewritten_after_admission";
   }
   return NULL;
}

void
r3v_native_plan_replay_fail(struct r3v_native_plan_replay *replay,
                            const char *why, int err)
{
   r3v_native_plan_session_fail(&replay->session,
                                R3V_NATIVE_PLAN_SESSION_TERMINAL);
   char detail[160];
   snprintf(detail, sizeof(detail), "%s:%d:%s", why, err,
            err < 0 ? strerror(-err) : "-");
   write_state(replay, "terminal", detail);
}

const char *
r3v_native_plan_replay_close(struct r3v_native_plan_replay *replay)
{
   if (!replay->bound)
      return replay->refused ? "refused" : "unbound";
   enum r3v_native_plan_session_result finished =
      r3v_native_plan_session_finish(&replay->session,
                                     monotonic_seconds() - replay->bound_seconds);
   const char *state = finished == R3V_NATIVE_PLAN_SESSION_ADMITTED
                          ? "complete"
                       : finished == R3V_NATIVE_PLAN_SESSION_INCOMPLETE
                          ? "incomplete"
                          : "terminal";
   write_state(replay, state, r3v_native_plan_session_result_name(finished));
   return state;
}

void
r3v_native_plan_replay_finish(struct r3v_native_plan_replay *replay)
{
   r3v_native_plan_finish(&replay->plan);
   free(replay->evidence_dir);
   free(replay->nonce);
   memset(replay, 0, sizeof(*replay));
}
