/*
 * SPDX-License-Identifier: MIT
 *
 * Attended application for the public RB2D constant-fill cell on
 * silicon: one loader-linked process, one device, one destination, one
 * vkCmdFillBuffer, one vkQueueSubmit, one bounded fence wait, one
 * inspection of the whole destination.  The binary links libvulkan and
 * libc alone and refuses before vkCreateInstance whenever the process
 * is not the attended shape: a preloaded library, a drm-shim in the
 * address space, a shim counter symbol, a manifest other than the
 * declared one, or a declaration that disagrees with the host.  There
 * is no switch that turns this binary into the shim transport control;
 * r3v_native_loader_fill_application is that control and stays
 * separate.
 *
 * Usage: r3v_native_attended_rb2d_fill <declaration> <receipt-dir>
 *
 * The declaration is key=value lines; every key is required:
 *   vk_driver_files                    the exact VK_DRIVER_FILES value
 *   icd_manifest_sha256                sha256 of that manifest file
 *   icd_dso                            the ICD DSO the loader must map
 *   icd_sha256                         sha256 of that DSO
 *   attended_application_sha256        sha256 of this executable
 *   kernel_release                     uname -r
 *   module_srcversion                  /sys/module/radeon/srcversion
 *   boot_id                            /proc/sys/kernel/random/boot_id
 *   authorized_ib_blake3               R3V_NATIVE_AUTHORIZED_IB_BLAKE3
 *   authorized_fill_identity_blake3    R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3
 *   manifest_dir                       R3V_NATIVE_MANIFEST_DIR
 *   memory_type_index                  the memory type the cell binds
 *   fill_offset, fill_bytes, fill_value  the cell's request
 *   wait_bound_ns                      the sealed completion bound
 *
 * One key is optional:
 *   cell_name                          the named cell; v1_public when absent
 *
 * The cell decides which route runs the fill, so the binary requires the
 * environment that route needs and refuses each missing or wrong gate by
 * name: v1_public runs the receipted single-window route,
 * v2_multiwindow_256 runs the windowed route with the V1 gate closed and
 * the 256-byte carrier pinned, and v2_chooser_16320 runs it with the pin
 * withdrawn and the expected carrier declared instead.  A route-receipt
 * cell runs with the qualification carrier unset, which is the floor it
 * receipts at.
 *
 * The receipt directory must exist and be empty.  outcome.json and, once
 * a destination exists, destination.bin land there through full writes,
 * fsync, atomic rename, and directory fsync before the verdict prints.
 * Exit 0 is CONTROL_PASS alone; 1 a destination verdict, 2 an
 * infrastructure refusal, 3 a submit failure, 4 a completion failure.
 */

#include "r3v_public_rb2d_fill_oracle.h"
#include "r3v_public_rb2d_fill_scenario.h"

#include "amd/r300/common/r300_chip_identity.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* SHA-256 (FIPS 180-4) over a file, so the binary verifies the manifest,
 * the DSO, and itself without a library beyond libc. */
struct sha256 {
   uint32_t h[8];
   uint8_t block[64];
   uint64_t length;
   size_t fill;
};

static const uint32_t sha256_k[64] = {
   0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
   0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
   0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
   0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
   0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
   0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
   0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
   0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
   0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
   0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
   0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha256_block(struct sha256 *c, const uint8_t *p)
{
   uint32_t w[64];
   for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
             (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
   for (int i = 16; i < 64; i++) {
      const uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
   }
   uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4],
            f = c->h[5], g = c->h[6], h = c->h[7];
   for (int i = 0; i < 64; i++) {
      const uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
      const uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
      const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
      const uint32_t t2 = S0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = cc;
      cc = b;
      b = a;
      a = t1 + t2;
   }
   c->h[0] += a;
   c->h[1] += b;
   c->h[2] += cc;
   c->h[3] += d;
   c->h[4] += e;
   c->h[5] += f;
   c->h[6] += g;
   c->h[7] += h;
}

static void
sha256_init(struct sha256 *c)
{
   static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                  0xa54ff53a, 0x510e527f, 0x9b05688c,
                                  0x1f83d9ab, 0x5be0cd19};
   memcpy(c->h, iv, sizeof(iv));
   c->length = 0;
   c->fill = 0;
}

static void
sha256_update(struct sha256 *c, const uint8_t *data, size_t len)
{
   c->length += len;
   while (len > 0) {
      const size_t take = 64 - c->fill < len ? 64 - c->fill : len;
      memcpy(c->block + c->fill, data, take);
      c->fill += take;
      data += take;
      len -= take;
      if (c->fill == 64) {
         sha256_block(c, c->block);
         c->fill = 0;
      }
   }
}

static void
sha256_final(struct sha256 *c, char hex[65])
{
   const uint64_t bits = c->length * 8;
   const uint8_t one = 0x80;
   sha256_update(c, &one, 1);
   const uint8_t zero = 0;
   while (c->fill != 56)
      sha256_update(c, &zero, 1);
   uint8_t len[8];
   for (int i = 0; i < 8; i++)
      len[i] = (uint8_t)(bits >> (56 - 8 * i));
   sha256_update(c, len, 8);
   for (int i = 0; i < 8; i++)
      snprintf(hex + 8 * i, 9, "%08x", c->h[i]);
}

static bool
sha256_file(const char *path, char hex[65])
{
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return false;
   struct sha256 c;
   sha256_init(&c);
   uint8_t buf[65536];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      sha256_update(&c, buf, n);
   const bool ok = !ferror(f);
   fclose(f);
   if (ok)
      sha256_final(&c, hex);
   return ok;
}

/* The declaration and the receipt. */

#define DECLARATION_KEYS(X)                                                  \
   X(vk_driver_files)                                                        \
   X(icd_manifest_sha256)                                                    \
   X(icd_dso)                                                                \
   X(icd_sha256)                                                             \
   X(attended_application_sha256)                                            \
   X(kernel_release)                                                         \
   X(module_srcversion)                                                      \
   X(boot_id)                                                                \
   X(authorized_ib_blake3)                                                   \
   X(authorized_fill_identity_blake3)                                        \
   X(manifest_dir)                                                           \
   X(memory_type_index)                                                      \
   X(fill_offset)                                                            \
   X(fill_bytes)                                                             \
   X(fill_value)                                                             \
   X(wait_bound_ns)

/* Keys the declaration may omit.  The sealed cell's declaration predates
 * the cell table and names no cell, so an absent cell_name is v1_public
 * and every sealed declaration still parses. */
#define OPTIONAL_DECLARATION_KEYS(X) X(cell_name)

struct declaration {
#define FIELD(name) char name[512];
   DECLARATION_KEYS(FIELD)
   OPTIONAL_DECLARATION_KEYS(FIELD)
#undef FIELD
};

static bool
read_declaration(const char *path, struct declaration *d, char *why, size_t n)
{
   memset(d, 0, sizeof(*d));
   FILE *f = fopen(path, "r");
   if (f == NULL) {
      snprintf(why, n, "the attended declaration %s is absent", path);
      return false;
   }
   char line[1024];
   while (fgets(line, sizeof(line), f) != NULL) {
      line[strcspn(line, "\r\n")] = '\0';
      if (line[0] == '\0' || line[0] == '#')
         continue;
      char *eq = strchr(line, '=');
      if (eq == NULL) {
         snprintf(why, n, "declaration line without '=': %s", line);
         fclose(f);
         return false;
      }
      *eq = '\0';
      const char *key = line, *value = eq + 1;
      bool known = false;
#define MATCH(name)                                                          \
   if (strcmp(key, #name) == 0) {                                            \
      snprintf(d->name, sizeof(d->name), "%s", value);                       \
      known = true;                                                          \
   }
      DECLARATION_KEYS(MATCH)
      OPTIONAL_DECLARATION_KEYS(MATCH)
#undef MATCH
      if (!known) {
         snprintf(why, n, "declaration names an unknown key: %s", key);
         fclose(f);
         return false;
      }
   }
   fclose(f);
#define REQUIRE_KEY(name)                                                    \
   if (d->name[0] == '\0') {                                                 \
      snprintf(why, n, "declaration lacks %s", #name);                       \
      return false;                                                          \
   }
   DECLARATION_KEYS(REQUIRE_KEY)
#undef REQUIRE_KEY
   return true;
}

static bool
read_first_line(const char *path, char *out, size_t n)
{
   FILE *f = fopen(path, "r");
   if (f == NULL)
      return false;
   const bool ok = fgets(out, (int)n, f) != NULL;
   fclose(f);
   if (ok)
      out[strcspn(out, "\r\n")] = '\0';
   return ok;
}

static bool
parse_number(const char *text, uint64_t *out)
{
   char *end = NULL;
   errno = 0;
   const unsigned long long v = strtoull(text, &end, 0);
   if (errno != 0 || end == text || *end != '\0')
      return false;
   *out = v;
   return true;
}

static bool
write_durable(const char *dir, const char *name, const void *data, size_t len)
{
   char tmp[4096], final[4096];
   snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, name);
   snprintf(final, sizeof(final), "%s/%s", dir, name);
   const int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
   if (fd < 0)
      return false;
   const uint8_t *p = data;
   while (len > 0) {
      const ssize_t w = write(fd, p, len);
      if (w < 0) {
         if (errno == EINTR)
            continue;
         close(fd);
         return false;
      }
      p += w;
      len -= (size_t)w;
   }
   if (fsync(fd) != 0 || close(fd) != 0)
      return false;
   if (rename(tmp, final) != 0)
      return false;
   const int dfd = open(dir, O_RDONLY | O_DIRECTORY);
   if (dfd < 0)
      return false;
   const bool ok = fsync(dfd) == 0;
   close(dfd);
   return ok;
}

static bool
directory_empty(const char *path)
{
   DIR *d = opendir(path);
   if (d == NULL)
      return false;
   bool empty = true;
   struct dirent *e;
   while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
         empty = false;
         break;
      }
   }
   closedir(d);
   return empty;
}

static bool
maps_name_a_shim(void)
{
   FILE *maps = fopen("/proc/self/maps", "r");
   if (maps == NULL)
      return true;
   char line[4096];
   bool found = false;
   while (fgets(line, sizeof(line), maps) != NULL) {
      if (strstr(line, "drm_shim") != NULL || strstr(line, "drm-shim") != NULL) {
         found = true;
         break;
      }
   }
   fclose(maps);
   return found;
}

static const char *receipt_dir;
static char outcome_reason[512];
/* The cell this run names, resolved from the declaration.  A refusal
 * before resolution still writes a record, so finish() falls back to the
 * sealed cell and the record names the cell it was judged against. */
static const struct r3v_public_rb2d_fill_cell *attended_cell;

/* The outcome record.  Written before the verdict prints so a process
 * that dies between the two still leaves its classification. */
static _Noreturn void
finish(enum r3v_public_rb2d_fill_outcome outcome,
       const struct r3v_public_rb2d_fill_report *report,
       const struct r3v_public_rb2d_fill_scenario *s, const uint8_t *image)
{
   const struct r3v_public_rb2d_fill_cell *cell =
      attended_cell != NULL ? attended_cell
                            : r3v_public_rb2d_fill_sealed_cell();
   char json[4096];
   int n = snprintf(
      json, sizeof(json),
      "{\n  \"outcome\": \"%s\",\n  \"exit_status\": %d,\n"
      "  \"reason\": \"%s\",\n  \"submit_result\": \"%s\",\n"
      "  \"wait_result\": \"%s\",\n  \"memory_type_index\": %u,\n"
      "  \"host_coherent\": %s,\n  \"destination_protected\": %s,\n"
      "  \"destination_written\": %s,\n",
      r3v_public_rb2d_fill_outcome_name(outcome),
      r3v_public_rb2d_fill_exit_status(outcome), outcome_reason,
      s != NULL ? r3v_public_rb2d_fill_result_name(s->submit_result) : "(none)",
      s != NULL ? r3v_public_rb2d_fill_result_name(s->wait_result) : "(none)",
      s != NULL ? s->memory_type_index : 0,
      s != NULL && s->host_coherent ? "true" : "false",
      s != NULL && s->map_protected ? "true" : "false",
      image != NULL ? "true" : "false");
   if (report != NULL) {
      n += snprintf(
         json + n, sizeof(json) - (size_t)n,
         "  \"changed_bytes\": %u,\n  \"changed_dwords\": %u,\n"
         "  \"expected_changed_bytes\": %u,\n  \"expected_changed_dwords\": %u,\n"
         "  \"interval_pattern_dwords\": %u,\n  \"interval_sentinel_dwords\": %u,\n"
         "  \"interval_other_dwords\": %u,\n  \"outside_changed_bytes\": %u,\n"
         "  \"tail_changed_bytes\": %u,\n  \"first_changed\": %lld,\n"
         "  \"last_changed\": %lld,\n  \"shifted\": %s,\n"
         "  \"shifted_run_start\": %u,\n",
         report->changed_bytes, report->changed_dwords, cell->fill_bytes,
         cell->fill_bytes / 4, report->interval_pattern_dwords,
         report->interval_sentinel_dwords, report->interval_other_dwords,
         report->outside_changed_bytes, report->tail_changed_bytes,
         report->first_changed == UINT32_MAX ? -1LL : (long long)report->first_changed,
         report->changed_bytes == 0 ? -1LL : (long long)report->last_changed,
         report->shifted ? "true" : "false", report->shifted_run_start);
   }
   n += snprintf(json + n, sizeof(json) - (size_t)n,
                 "  \"cell_name\": \"%s\",\n"
                 "  \"expected_pitch\": %u,\n"
                 "  \"expected_windows\": %u,\n"
                 "  \"expected_relocation_sites\": %u,\n"
                 "  \"allocation_bytes\": %u,\n"
                 "  \"fill_offset\": %u,\n  \"fill_bytes\": %u,\n"
                 "  \"fill_value\": \"0x%08x\"\n}\n",
                 cell->name, cell->expected_pitch_bytes,
                 cell->expected_window_count,
                 cell->expected_relocation_sites, cell->allocation_bytes,
                 cell->fill_offset, cell->fill_bytes, cell->fill_value);
   bool durable = true;
   if (image != NULL)
      durable &= write_durable(receipt_dir, "destination.bin", image,
                               cell->allocation_bytes);
   durable &= write_durable(receipt_dir, "outcome.json", json, (size_t)n);
   if (!durable)
      fprintf(stderr, "the receipt directory %s did not take the record: %s\n",
              receipt_dir, strerror(errno));
   printf("outcome=%s\n", r3v_public_rb2d_fill_outcome_name(outcome));
   printf("r3v-native-attended-rb2d-fill: %s\n",
          outcome == R3V_PUBLIC_RB2D_FILL_CONTROL_PASS ? "CONTROL_PASS"
                                                       : "FAIL");
   fflush(stdout);
   exit(r3v_public_rb2d_fill_exit_status(outcome));
}

static _Noreturn void
refuse(const char *why)
{
   snprintf(outcome_reason, sizeof(outcome_reason), "%s", why);
   fprintf(stderr, "INFRASTRUCTURE_REFUSAL: %s\n", why);
   finish(R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL, NULL, NULL, NULL);
}

static void
require_env_equal(const char *name, const char *declared)
{
   const char *value = getenv(name);
   if (value == NULL || strcmp(value, declared) != 0) {
      char why[1024];
      snprintf(why, sizeof(why), "%s is %s; the declaration says %s", name,
               value != NULL ? value : "(unset)", declared);
      refuse(why);
   }
}

/* A gate the cell's route requires to stand closed.  An open gate names a
 * second executor for the same destination, which the device refuses at
 * creation; refusing here names which gate and which cell disagree. */
static void
require_env_absent(const char *name, const char *why_cell)
{
   const char *value = getenv(name);
   if (value != NULL && value[0] != '\0') {
      char why[1024];
      snprintf(why, sizeof(why), "%s is %s; cell %s runs with it unset",
               name, value, why_cell);
      refuse(why);
   }
}

static void
require_host_equal(const char *what, const char *observed, const char *declared)
{
   if (strcmp(observed, declared) != 0) {
      char why[1024];
      snprintf(why, sizeof(why), "%s is %s; the declaration says %s", what,
               observed, declared);
      refuse(why);
   }
}

int
main(int argc, char **argv)
{
   if (argc != 3) {
      fprintf(stderr, "usage: %s <declaration> <receipt-dir>\n", argv[0]);
      return 2;
   }
   receipt_dir = argv[2];
   struct stat st;
   if (stat(receipt_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
      fprintf(stderr, "INFRASTRUCTURE_REFUSAL: the receipt directory %s is "
                      "absent\n",
              receipt_dir);
      return 2;
   }
   if (!directory_empty(receipt_dir)) {
      fprintf(stderr, "INFRASTRUCTURE_REFUSAL: the receipt directory %s is "
                      "not empty\n",
              receipt_dir);
      return 2;
   }
   printf("phase=preflight\n");

   /* The attended shape, ahead of any Vulkan call. */
   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0')
      refuse("LD_PRELOAD is set; the attended application admits no "
             "preloaded library");
   if (maps_name_a_shim())
      refuse("a drm-shim DSO is mapped in this process");
   if (dlsym(RTLD_DEFAULT, "drm_shim_test_radeon_cs_ioctls") != NULL)
      refuse("a drm-shim counter symbol resolves in this process");

   struct declaration d;
   char why[1024];
   if (!read_declaration(argv[1], &d, why, sizeof(why)))
      refuse(why);

   const char *driver_files = getenv("VK_DRIVER_FILES");
   if (driver_files == NULL || strcmp(driver_files, d.vk_driver_files) != 0) {
      snprintf(why, sizeof(why), "VK_DRIVER_FILES is %s; the declaration "
                                  "names %s",
               driver_files != NULL ? driver_files : "(unset)",
               d.vk_driver_files);
      refuse(why);
   }
   char hex[65];
   if (!sha256_file(d.vk_driver_files, hex) || strcmp(hex, d.icd_manifest_sha256) != 0) {
      snprintf(why, sizeof(why), "the manifest %s hashes to %s; the "
                                  "declaration says %s",
               d.vk_driver_files, hex, d.icd_manifest_sha256);
      refuse(why);
   }
   if (!sha256_file(d.icd_dso, hex) || strcmp(hex, d.icd_sha256) != 0) {
      snprintf(why, sizeof(why), "the ICD %s hashes to %s; the declaration "
                                  "says %s",
               d.icd_dso, hex, d.icd_sha256);
      refuse(why);
   }
   if (!sha256_file("/proc/self/exe", hex) ||
       strcmp(hex, d.attended_application_sha256) != 0) {
      snprintf(why, sizeof(why), "this executable hashes to %s; the "
                                  "declaration says %s",
               hex, d.attended_application_sha256);
      refuse(why);
   }

   require_env_equal("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", d.authorized_ib_blake3);
   require_env_equal("R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3",
                     d.authorized_fill_identity_blake3);
   require_env_equal("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", d.kernel_release);
   require_env_equal("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
                     d.module_srcversion);
   require_env_equal("R3V_NATIVE_MANIFEST_DIR", d.manifest_dir);
   require_env_equal("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1");
   require_env_equal("R3V_NATIVE_EXECUTION_POLICY", "gpu_only");
   if (stat(d.manifest_dir, &st) != 0 || !S_ISDIR(st.st_mode))
      refuse("the declared manifest directory is absent");

   /* The cell, then the environment its route needs.  The declaration
    * selects the cell and the cell selects the gates, so an operator who
    * declares one cell and arms another is refused by the name of the
    * gate that disagrees rather than by a fill that lands short. */
   const char *cell_name =
      d.cell_name[0] != '\0' ? d.cell_name : "v1_public";
   const struct r3v_public_rb2d_fill_cell *cell =
      r3v_public_rb2d_fill_cell_by_name(cell_name);
   if (cell == NULL) {
      char why[1024];
      snprintf(why, sizeof(why), "the declaration names no cell: %s",
               cell_name);
      refuse(why);
   }
   attended_cell = cell;
   if (cell->contract == R300_RB2D_CONTRACT_CONST_FILL_V1) {
      require_env_equal("R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL", "1");
   } else {
      require_env_equal("R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL",
                        "1");
      require_env_absent("R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL",
                         cell->name);
      char pitch[32];
      if (cell->pinned_pitch_bytes != 0u) {
         snprintf(pitch, sizeof(pitch), "%u", cell->pinned_pitch_bytes);
         require_env_equal("R3V_NATIVE_RB2D_V2_PINNED_PITCH_BYTES", pitch);
      } else {
         /* A chooser cell receipts the cost model's verdict, so the pin
          * -- the one declaration that selects a carrier instead of
          * asserting one -- stays unset, and the expected carrier states
          * the pitch the chooser has to return. */
         require_env_absent("R3V_NATIVE_RB2D_V2_PINNED_PITCH_BYTES",
                            cell->name);
         snprintf(pitch, sizeof(pitch), "%u", cell->expected_pitch_bytes);
         require_env_equal("R3V_NATIVE_RB2D_V2_EXPECTED_PITCH_BYTES", pitch);
      }
   }
   /* A route receipt runs at the executing evidence floor.  The
    * qualification declaration is the one lever that drops a carrier to
    * PLANNED, so it stays unset for every route-receipt cell and the
    * operator is refused by its name rather than by a receipt that turns
    * out to carry a weaker floor than it claims. */
   if (cell->evidence_scope == R3V_PUBLIC_RB2D_FILL_SCOPE_ROUTE_RECEIPT)
      require_env_absent("R3V_NATIVE_RB2D_CARRIER_QUALIFICATION_PITCH_BYTES",
                         cell->name);

   uint64_t offset, bytes, value, type_index, wait_bound;
   if (!parse_number(d.fill_offset, &offset) || !parse_number(d.fill_bytes, &bytes) ||
       !parse_number(d.fill_value, &value) ||
       !parse_number(d.memory_type_index, &type_index) ||
       !parse_number(d.wait_bound_ns, &wait_bound))
      refuse("a numeric declaration field is malformed");
   if (offset != cell->fill_offset || bytes != cell->fill_bytes ||
       value != cell->fill_value) {
      char why[1024];
      snprintf(why, sizeof(why),
               "the declared fill request %llu/%llu/0x%08llx differs from "
               "cell %s: %u/%u/0x%08x",
               (unsigned long long)offset, (unsigned long long)bytes,
               (unsigned long long)value, cell->name, cell->fill_offset,
               cell->fill_bytes, cell->fill_value);
      refuse(why);
   }
   if (wait_bound == 0 || wait_bound > (uint64_t)120 * 1000 * 1000 * 1000)
      refuse("the declared wait bound is outside (0, 120 s]");

   /* The host facts last, so a declaration that is complete and
    * self-consistent reaches them on any host and is refused there by
    * the fact that differs. */
   struct utsname uts;
   if (uname(&uts) != 0)
      refuse("uname failed");
   require_host_equal("the kernel release", uts.release, d.kernel_release);
   char text[512];
   if (!read_first_line("/sys/module/radeon/srcversion", text, sizeof(text)))
      refuse("/sys/module/radeon/srcversion is unreadable; the radeon module "
             "is not loaded");
   require_host_equal("the radeon module srcversion", text, d.module_srcversion);
   if (!read_first_line("/proc/sys/kernel/random/boot_id", text, sizeof(text)))
      refuse("the boot id is unreadable");
   require_host_equal("the boot id", text, d.boot_id);

   printf("declaration=%s receipt_dir=%s\n", argv[1], receipt_dir);
   printf("cell=%s expected_pitch=%u expected_windows=%u "
          "expected_relocation_sites=%u allocation_bytes=%u\n",
          cell->name, cell->expected_pitch_bytes,
          cell->expected_window_count, cell->expected_relocation_sites,
          cell->allocation_bytes);
   printf("fill_offset=%u fill_bytes=%u fill_value=0x%08x wait_bound_ns=%llu\n",
          cell->fill_offset, cell->fill_bytes, cell->fill_value,
          (unsigned long long)wait_bound);
   fflush(stdout);

   /* The public scenario.  A failure before the submit is infrastructure;
    * the destination exists from the map onward and is retained. */
   struct r3v_public_rb2d_fill_scenario s;
   const struct r3v_public_rb2d_fill_scenario_config config = {
      .cell = cell,
      .protect_destination = true,
      .wait_bound_ns = wait_bound,
      .required_vendor_id = R300_PCI_VENDOR_ATI,
      .required_device_id = R300_PCI_DEVICE_RS48X_5974,
   };
   printf("phase=instance\n");
   fflush(stdout);
   if (!r3v_public_rb2d_fill_scenario_open(&s, &config)) {
      snprintf(outcome_reason, sizeof(outcome_reason), "%s", s.failure);
      fprintf(stderr, "INFRASTRUCTURE_REFUSAL: %s\n", s.failure);
      const uint8_t *image = r3v_public_rb2d_fill_scenario_image(&s);
      finish(R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL, NULL, &s, image);
   }
   printf("device=%04x:%04x driver=%s\n", s.properties.vendorID,
          s.properties.deviceID, s.properties.deviceName);
   if (!r3v_public_rb2d_fill_dso_mapped(d.icd_dso)) {
      snprintf(outcome_reason, sizeof(outcome_reason),
               "the loader did not map %s", d.icd_dso);
      fprintf(stderr, "INFRASTRUCTURE_REFUSAL: %s\n", outcome_reason);
      finish(R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL, NULL, &s,
             r3v_public_rb2d_fill_scenario_image(&s));
   }
   if (s.memory_type_index != type_index) {
      snprintf(outcome_reason, sizeof(outcome_reason),
               "the cell bound memory type %u; the declaration says %llu",
               s.memory_type_index, (unsigned long long)type_index);
      fprintf(stderr, "INFRASTRUCTURE_REFUSAL: %s\n", outcome_reason);
      finish(R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL, NULL, &s,
             r3v_public_rb2d_fill_scenario_image(&s));
   }
   printf("icd_dso=%s memory_type_index=%u host_coherent=%d "
          "destination_protected=%d\n",
          d.icd_dso, s.memory_type_index, s.host_coherent ? 1 : 0,
          s.map_protected ? 1 : 0);
   if (!r3v_public_rb2d_fill_scenario_record(&s)) {
      snprintf(outcome_reason, sizeof(outcome_reason), "%s", s.failure);
      fprintf(stderr, "INFRASTRUCTURE_REFUSAL: %s\n", s.failure);
      finish(R3V_PUBLIC_RB2D_FILL_INFRASTRUCTURE_REFUSAL, NULL, &s,
             r3v_public_rb2d_fill_scenario_image(&s));
   }
   printf("phase=submit\n");
   fflush(stdout);

   const bool submitted = r3v_public_rb2d_fill_scenario_submit(&s);
   printf("submit_result=%s\n", r3v_public_rb2d_fill_result_name(s.submit_result));
   fflush(stdout);
   if (!submitted) {
      snprintf(outcome_reason, sizeof(outcome_reason), "%s", s.failure);
      finish(R3V_PUBLIC_RB2D_FILL_SUBMIT_FAILED, NULL, &s,
             r3v_public_rb2d_fill_scenario_image(&s));
   }
   const bool completed = r3v_public_rb2d_fill_scenario_wait(&s);
   printf("wait_result=%s\n", r3v_public_rb2d_fill_result_name(s.wait_result));
   fflush(stdout);
   if (!completed) {
      snprintf(outcome_reason, sizeof(outcome_reason), "%s", s.failure);
      finish(R3V_PUBLIC_RB2D_FILL_COMPLETION_FAILED, NULL, &s,
             r3v_public_rb2d_fill_scenario_image(&s));
   }

   /* Inspection without modification; the count invariants are checked
    * again here so a classifier verdict and the reported counts agree. */
   const uint8_t *image = r3v_public_rb2d_fill_scenario_image(&s);
   struct r3v_public_rb2d_fill_report report;
   enum r3v_public_rb2d_fill_outcome outcome =
      r3v_public_rb2d_fill_classify(cell, image, &report);
   if (outcome == R3V_PUBLIC_RB2D_FILL_CONTROL_PASS &&
       (report.changed_bytes != cell->fill_bytes ||
        report.changed_dwords != cell->fill_bytes / 4))
      outcome = R3V_PUBLIC_RB2D_FILL_PATTERN_MISMATCH;
   snprintf(outcome_reason, sizeof(outcome_reason),
            "destination classified by the oracle");
   printf("changed_bytes=%u changed_dwords=%u interval_pattern_dwords=%u "
          "interval_sentinel_dwords=%u interval_other_dwords=%u "
          "outside_changed_bytes=%u tail_changed_bytes=%u shifted=%d\n",
          report.changed_bytes, report.changed_dwords,
          report.interval_pattern_dwords, report.interval_sentinel_dwords,
          report.interval_other_dwords, report.outside_changed_bytes,
          report.tail_changed_bytes, report.shifted ? 1 : 0);
   finish(outcome, &report, &s, image);
}
