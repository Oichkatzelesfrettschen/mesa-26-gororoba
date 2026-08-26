/*
 * SPDX-License-Identifier: MIT
 *
 * Multi-factor arming gate for native DRM_RADEON_CS submission.
 */

#include "r3v_native_arming.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define R3V_NATIVE_ATTEMPT_TOKEN "attempt.token"

static bool
declared(const char *value)
{
   return value != NULL && value[0] != '\0';
}

enum r3v_native_arming_verdict
r3v_native_arming_evaluate(const struct r3v_native_arming_facts *facts)
{
   if (facts == NULL)
      return R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED;

   if (!declared(facts->hazard_gate) || strcmp(facts->hazard_gate, "1") != 0)
      return R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED;

   if (!declared(facts->authorized_ib_blake3))
      return R3V_NATIVE_ARMING_BUNDLE_UNDECLARED;
   if (!declared(facts->actual_ib_blake3) ||
       strcmp(facts->authorized_ib_blake3, facts->actual_ib_blake3) != 0)
      return R3V_NATIVE_ARMING_BUNDLE_MISMATCH;
   /* The kind selects the geometry predicate, so a kind outside the set
    * refuses before the extent fact is read: an unrecognized cell has no
    * frozen geometry the fact could describe.
    */
   switch (facts->cell_kind) {
   case R3V_NATIVE_CELL_KIND_TRIANGLE:
   case R3V_NATIVE_CELL_KIND_DIRECT_WRITE:
   case R3V_NATIVE_CELL_KIND_R2VB_PRODUCER:
   case R3V_NATIVE_CELL_KIND_R2VB_REINGEST:
   case R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE:
   case R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL:
   case R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST:
   case R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC:
   case R3V_NATIVE_CELL_KIND_ZB_DEPTH_CONTROL:
   case R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED:
   case R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER:
   case R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE:
   case R3V_NATIVE_CELL_KIND_TRIANGLE_SAMPLED:
   case R3V_NATIVE_CELL_KIND_TRIANGLE_COMPOSED_RENDER_SAMPLE:
      break;
   case R3V_NATIVE_CELL_KIND_UNDECLARED:
   default:
      return R3V_NATIVE_ARMING_UNKNOWN_CELL_KIND;
   }
   if (facts->nonmaximum_extent)
      return R3V_NATIVE_ARMING_NONMAXIMUM_EXTENT;

   if (facts->pci_vendor_id != R3V_NATIVE_ARMING_PCI_VENDOR ||
       facts->pci_device_id != R3V_NATIVE_ARMING_PCI_DEVICE)
      return R3V_NATIVE_ARMING_CHIP_MISMATCH;

   if (!declared(facts->authorized_kernel_release))
      return R3V_NATIVE_ARMING_KERNEL_UNDECLARED;
   if (!declared(facts->running_kernel_release) ||
       strcmp(facts->authorized_kernel_release,
              facts->running_kernel_release) != 0)
      return R3V_NATIVE_ARMING_KERNEL_MISMATCH;

   if (!declared(facts->authorized_module_srcversion))
      return R3V_NATIVE_ARMING_MODULE_UNDECLARED;
   if (!declared(facts->running_module_srcversion) ||
       strcmp(facts->authorized_module_srcversion,
              facts->running_module_srcversion) != 0)
      return R3V_NATIVE_ARMING_MODULE_MISMATCH;

   if (!facts->evidence_dir_present)
      return R3V_NATIVE_ARMING_EVIDENCE_ABSENT;

   /* The serial kind trades the one-shot token for a declared bound: the
    * token still disarms the directory against every other process, and
    * within the instance that wrote it, each admission counts against the
    * exact declared bound.  A token this instance did not write, a missing
    * token mid-run, an undeclared bound, and an exhausted bound each
    * refuse by name.
    */
   if (facts->cell_kind == R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL) {
      if (facts->serial_authorized_submissions < 1 ||
          facts->serial_authorized_submissions >
             R3V_NATIVE_ARMING_SERIAL_MAX_SUBMISSIONS)
         return R3V_NATIVE_ARMING_SERIAL_BOUND_UNDECLARED;
      if (facts->serial_submissions_consumed >=
          facts->serial_authorized_submissions)
         return R3V_NATIVE_ARMING_SERIAL_BOUND_EXHAUSTED;
      if (facts->serial_submissions_consumed == 0) {
         if (facts->attempt_token_present)
            return R3V_NATIVE_ARMING_ALREADY_ATTEMPTED;
      } else if (!facts->attempt_token_present) {
         return R3V_NATIVE_ARMING_SERIAL_CONTINUITY_BROKEN;
      }
      return R3V_NATIVE_ARMING_ARMED;
   }

   /* The burst kind keeps the one-shot token and adds the declared
    * member depth: the operator authorizes a specific duty cycle, so a
    * recorded composition at any other depth refuses even under a
    * matching stream digest.
    */
   if (facts->cell_kind == R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST) {
      if (facts->burst_authorized_draws < 1 ||
          facts->burst_authorized_draws > R3V_NATIVE_ARMING_BURST_MAX_DRAWS)
         return R3V_NATIVE_ARMING_BURST_DRAWS_UNDECLARED;
      if (facts->burst_recorded_draws != facts->burst_authorized_draws)
         return R3V_NATIVE_ARMING_BURST_DRAWS_MISMATCH;
   }

   if (facts->attempt_token_present)
      return R3V_NATIVE_ARMING_ALREADY_ATTEMPTED;

   return R3V_NATIVE_ARMING_ARMED;
}

const char *
r3v_native_arming_verdict_name(enum r3v_native_arming_verdict verdict)
{
   switch (verdict) {
   case R3V_NATIVE_ARMING_ARMED:
      return "armed";
   case R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED:
      return "hazard gate closed (R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED)";
   case R3V_NATIVE_ARMING_BUNDLE_UNDECLARED:
      return "authorized bundle digest undeclared "
             "(R3V_NATIVE_AUTHORIZED_IB_BLAKE3)";
   case R3V_NATIVE_ARMING_BUNDLE_MISMATCH:
      return "submitted IB differs from the authorized bundle digest";
   case R3V_NATIVE_ARMING_UNKNOWN_CELL_KIND:
      return "recorded cell kind carries no frozen geometry contract";
   case R3V_NATIVE_ARMING_NONMAXIMUM_EXTENT:
      return "extent differs from the cell kind's frozen geometry";
   case R3V_NATIVE_ARMING_CHIP_MISMATCH:
      return "enumerated chip is not the authorized RS482 identity";
   case R3V_NATIVE_ARMING_KERNEL_UNDECLARED:
      return "authorized kernel release undeclared "
             "(R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE)";
   case R3V_NATIVE_ARMING_KERNEL_MISMATCH:
      return "running kernel release differs from the authorized release";
   case R3V_NATIVE_ARMING_MODULE_UNDECLARED:
      return "authorized radeon module srcversion undeclared "
             "(R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION)";
   case R3V_NATIVE_ARMING_MODULE_MISMATCH:
      return "running radeon module srcversion differs from the "
             "authorized srcversion";
   case R3V_NATIVE_ARMING_EVIDENCE_ABSENT:
      return "evidence directory is absent";
   case R3V_NATIVE_ARMING_ALREADY_ATTEMPTED:
      return "evidence directory holds an attempt token from an earlier "
             "arming";
   case R3V_NATIVE_ARMING_SERIAL_BOUND_UNDECLARED:
      return "serial submission bound undeclared or outside 1..64 "
             "(R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS)";
   case R3V_NATIVE_ARMING_SERIAL_BOUND_EXHAUSTED:
      return "declared serial submission bound is exhausted";
   case R3V_NATIVE_ARMING_SERIAL_CONTINUITY_BROKEN:
      return "serial continuation without the attempt token this "
             "instance wrote";
   case R3V_NATIVE_ARMING_BURST_DRAWS_UNDECLARED:
      return "burst member count undeclared or outside 1..64 "
             "(R3V_NATIVE_AUTHORIZED_BURST_DRAWS)";
   case R3V_NATIVE_ARMING_BURST_DRAWS_MISMATCH:
      return "recorded burst member count differs from the declared "
             "count";
   }
   return "unknown verdict";
}

/* Reads one whitespace-terminated line into caller storage; leaves the
 * storage empty when the file is unreadable, which refuses.
 */
static void
read_first_token(const char *path, char *storage, size_t size)
{
   storage[0] = '\0';
   FILE *f = fopen(path, "r");
   if (f == NULL)
      return;
   if (fgets(storage, (int)size, f) != NULL) {
      size_t length = strlen(storage);
      while (length > 0 && (storage[length - 1] == '\n' ||
                            storage[length - 1] == '\r' ||
                            storage[length - 1] == ' '))
         storage[--length] = '\0';
   }
   fclose(f);
}

/* The host provider: the only place collection touches the machine. */

static const char *
host_read_env(void *ctx, const char *name)
{
   (void)ctx;
   return getenv(name);
}

static void
host_read_kernel_release(void *ctx, char *out, size_t size)
{
   (void)ctx;
   struct utsname host;
   out[0] = '\0';
   if (uname(&host) == 0)
      snprintf(out, size, "%s", host.release);
}

static void
host_read_module_srcversion(void *ctx, char *out, size_t size)
{
   (void)ctx;
   read_first_token("/sys/module/radeon/srcversion", out, size);
}

static bool
host_directory_present(void *ctx, const char *path)
{
   (void)ctx;
   struct stat status;
   return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool
host_file_present(void *ctx, const char *path)
{
   (void)ctx;
   struct stat status;
   return stat(path, &status) == 0;
}

static const struct r3v_native_arming_provider host_provider = {
   .read_env = host_read_env,
   .read_kernel_release = host_read_kernel_release,
   .read_module_srcversion = host_read_module_srcversion,
   .directory_present = host_directory_present,
   .file_present = host_file_present,
   .ctx = NULL,
};

const struct r3v_native_arming_provider *
r3v_native_arming_host_provider(void)
{
   return &host_provider;
}

void
r3v_native_arming_collect_from(
   const struct r3v_native_arming_provider *provider,
   struct r3v_native_arming_facts *facts, uint32_t pci_vendor_id,
   uint32_t pci_device_id, enum r3v_native_cell_kind cell_kind,
   const char *actual_ib_blake3, const char *evidence_dir,
   char *kernel_storage, size_t kernel_size, char *module_storage,
   size_t module_size)
{
   memset(facts, 0, sizeof(*facts));

   facts->cell_kind = cell_kind;

   facts->hazard_gate =
      provider->read_env(provider->ctx, "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   facts->authorized_ib_blake3 =
      provider->read_env(provider->ctx, "R3V_NATIVE_AUTHORIZED_IB_BLAKE3");
   facts->actual_ib_blake3 = actual_ib_blake3;
   facts->pci_vendor_id = pci_vendor_id;
   facts->pci_device_id = pci_device_id;
   facts->authorized_kernel_release =
      provider->read_env(provider->ctx,
                         "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE");
   facts->authorized_module_srcversion =
      provider->read_env(provider->ctx,
                         "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION");

   /* Exact decimal 1..64; every other spelling -- empty, signs, leading
    * zeros, trailing bytes, out of range -- parses to 0 and refuses the
    * serial kind.  serial_submissions_consumed is instance state the
    * queue installs after collection, like the extent fact.
    */
   const char *serial =
      provider->read_env(provider->ctx,
                         "R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS");
   if (declared(serial) && serial[0] >= '1' && serial[0] <= '9') {
      uint32_t value = 0;
      size_t i = 0;
      for (; serial[i] >= '0' && serial[i] <= '9' && i < 3; i++)
         value = value * 10 + (uint32_t)(serial[i] - '0');
      if (serial[i] == '\0' &&
          value <= R3V_NATIVE_ARMING_SERIAL_MAX_SUBMISSIONS)
         facts->serial_authorized_submissions = value;
   }

   /* Same exact-decimal spelling as the serial bound; burst_recorded_draws
    * is instance state the queue installs after collection.
    */
   const char *burst =
      provider->read_env(provider->ctx,
                         "R3V_NATIVE_AUTHORIZED_BURST_DRAWS");
   if (declared(burst) && burst[0] >= '1' && burst[0] <= '9') {
      uint32_t value = 0;
      size_t i = 0;
      for (; burst[i] >= '0' && burst[i] <= '9' && i < 3; i++)
         value = value * 10 + (uint32_t)(burst[i] - '0');
      if (burst[i] == '\0' && value <= R3V_NATIVE_ARMING_BURST_MAX_DRAWS)
         facts->burst_authorized_draws = value;
   }

   kernel_storage[0] = '\0';
   provider->read_kernel_release(provider->ctx, kernel_storage, kernel_size);
   facts->running_kernel_release = kernel_storage;

   /* An absent or unreadable radeon module has no identity.  The empty fact
    * remains undeclared so an operator value such as "none" cannot authorize
    * a live submission without a module identity. */
   module_storage[0] = '\0';
   provider->read_module_srcversion(provider->ctx, module_storage,
                                    module_size);
   facts->running_module_srcversion = module_storage;

   if (declared(evidence_dir)) {
      facts->evidence_dir_present =
         provider->directory_present(provider->ctx, evidence_dir);

      /* A truncated path would probe a different file than the token, so
       * truncation reads as an attempt already present and the gate stays
       * closed.
       */
      char token_path[1024];
      int token_length =
         snprintf(token_path, sizeof(token_path), "%s/%s", evidence_dir,
                  R3V_NATIVE_ATTEMPT_TOKEN);
      if (token_length < 0 || (size_t)token_length >= sizeof(token_path))
         facts->attempt_token_present = true;
      else
         facts->attempt_token_present =
            provider->file_present(provider->ctx, token_path);
   }
}

void
r3v_native_arming_collect(struct r3v_native_arming_facts *facts,
                          uint32_t pci_vendor_id, uint32_t pci_device_id,
                          enum r3v_native_cell_kind cell_kind,
                          const char *actual_ib_blake3,
                          const char *evidence_dir, char *kernel_storage,
                          size_t kernel_size, char *module_storage,
                          size_t module_size)
{
   r3v_native_arming_collect_from(&host_provider, facts, pci_vendor_id,
                                  pci_device_id, cell_kind,
                                  actual_ib_blake3, evidence_dir,
                                  kernel_storage, kernel_size,
                                  module_storage, module_size);
}

int
r3v_native_arming_disarm(const char *evidence_dir,
                         const char *declared_digest)
{
   if (!declared(evidence_dir))
      return -EINVAL;

   /* A truncated path would disarm a different location than the directory
    * the collection probes, so truncation refuses before any file exists.
    */
   char token_path[1024];
   int token_length = snprintf(token_path, sizeof(token_path), "%s/%s",
                               evidence_dir, R3V_NATIVE_ATTEMPT_TOKEN);
   if (token_length < 0 || (size_t)token_length >= sizeof(token_path))
      return -ENAMETOOLONG;

   /* Exclusive creation is the disarm: a token that already exists means
    * an earlier arming reached this point, and creation fails.  The token
    * carries the declared digest and the wall-clock instant, and both the
    * file and the directory entry fsync before this returns -- the caller
    * issues the ioctl only after, so a power failure past the ioctl cannot
    * leave the directory apparently unused.
    */
   int fd = open(token_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
   if (fd < 0)
      return -errno;

   struct timespec now;
   clock_gettime(CLOCK_REALTIME, &now);
   char body[256];
   int body_length = snprintf(
      body, sizeof(body),
      "declared_ib_blake3: %s\nunix_time: %lld.%09ld\n",
      declared(declared_digest) ? declared_digest : "(undeclared)",
      (long long)now.tv_sec, now.tv_nsec);
   if (body_length < 0 || (size_t)body_length >= sizeof(body)) {
      close(fd);
      return -EIO;
   }
   int result = 0;
   size_t total = 0;
   while (result == 0 && total < (size_t)body_length) {
      ssize_t written = write(fd, body + total, (size_t)body_length - total);
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
   if (result != 0)
      return result;

   int dir_fd = open(evidence_dir, O_RDONLY | O_DIRECTORY);
   if (dir_fd < 0)
      return -errno;
   result = fsync(dir_fd) == 0 ? 0 : -errno;
   if (close(dir_fd) != 0 && result == 0)
      result = -errno;
   return result;
}
