/*
 * SPDX-License-Identifier: MIT
 *
 * Multi-factor arming gate for native DRM_RADEON_CS submission.
 */

#include "r3v_native_arming.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

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

/* --- The host provider: the only place collection touches the machine --- */

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
   uint32_t pci_device_id, const char *actual_ib_blake3,
   const char *evidence_dir, char *kernel_storage, size_t kernel_size,
   char *module_storage, size_t module_size)
{
   memset(facts, 0, sizeof(*facts));

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

   kernel_storage[0] = '\0';
   provider->read_kernel_release(provider->ctx, kernel_storage, kernel_size);
   facts->running_kernel_release = kernel_storage;

   /* An unloaded radeon module reads as the literal "none", so the
    * operator declares that state explicitly instead of an unreadable
    * fact silently matching an unset declaration.
    */
   module_storage[0] = '\0';
   provider->read_module_srcversion(provider->ctx, module_storage,
                                    module_size);
   if (module_storage[0] == '\0')
      snprintf(module_storage, module_size, "none");
   facts->running_module_srcversion = module_storage;

   if (declared(evidence_dir)) {
      facts->evidence_dir_present =
         provider->directory_present(provider->ctx, evidence_dir);

      char token_path[1024];
      snprintf(token_path, sizeof(token_path), "%s/%s", evidence_dir,
               R3V_NATIVE_ATTEMPT_TOKEN);
      facts->attempt_token_present =
         provider->file_present(provider->ctx, token_path);
   }
}

void
r3v_native_arming_collect(struct r3v_native_arming_facts *facts,
                          uint32_t pci_vendor_id, uint32_t pci_device_id,
                          const char *actual_ib_blake3,
                          const char *evidence_dir, char *kernel_storage,
                          size_t kernel_size, char *module_storage,
                          size_t module_size)
{
   r3v_native_arming_collect_from(&host_provider, facts, pci_vendor_id,
                                  pci_device_id, actual_ib_blake3,
                                  evidence_dir, kernel_storage, kernel_size,
                                  module_storage, module_size);
}

int
r3v_native_arming_disarm(const char *evidence_dir)
{
   if (!declared(evidence_dir))
      return -EINVAL;

   char token_path[1024];
   snprintf(token_path, sizeof(token_path), "%s/%s", evidence_dir,
            R3V_NATIVE_ATTEMPT_TOKEN);

   /* Exclusive creation is the disarm: a token that already exists means
    * an earlier arming reached this point, and creation fails.
    */
   FILE *token = fopen(token_path, "wx");
   if (token == NULL)
      return -errno;
   fputs("r3v-native: one submission attempt was armed in this directory\n",
         token);
   int result = fclose(token) == 0 ? 0 : -EIO;
   return result;
}
