/*
 * SPDX-License-Identifier: MIT
 *
 * Ordered-submission plan: sealed text schema, identity binding,
 * whole-entry matching, and the monotonic replay session.
 */

#include "r3v_native_plan.h"

#include "amd/r300/common/r300_compute_verb.h"
#include "util/mesa-blake3.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The plan is a sequence of tab-separated lines; the first line names the
 * schema and its version, every header field appears once, submissions and
 * their relocations follow in index order, and the last line seals every
 * byte before it with BLAKE3.  The layout keeps the file diffable by a
 * reviewer and parseable without a JSON reader in the driver.
 */

#define MAGIC "r3v-native-conformance-plan"

static const char *const queue_claim_names[] = {
   "default_graphics_only", "experimental_compute_subset", "conformant",
};
#define QUEUE_CLAIM_COUNT (sizeof(queue_claim_names) / sizeof(queue_claim_names[0]))
static_assert(QUEUE_CLAIM_COUNT == R3V_NATIVE_PLAN_QUEUE_CONFORMANT + 1,
              "queue claim names track the enum");

static const struct {
   enum r3v_native_cell_kind kind;
   const char *name;
} cell_kind_names[] = {
   {R3V_NATIVE_CELL_KIND_UNDECLARED, "undeclared"},
   {R3V_NATIVE_CELL_KIND_TRIANGLE, "triangle"},
   {R3V_NATIVE_CELL_KIND_DIRECT_WRITE, "direct_write"},
   {R3V_NATIVE_CELL_KIND_R2VB_PRODUCER, "r2vb_producer"},
   {R3V_NATIVE_CELL_KIND_R2VB_REINGEST, "r2vb_reingest"},
   {R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE, "r2vb_float2_tuple"},
   {R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL, "r2vb_status_load_serial"},
   {R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST, "r2vb_status_load_burst"},
   {R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC,
    "r2vb_gpu_producer_public"},
   {R3V_NATIVE_CELL_KIND_ZB_DEPTH_CONTROL, "zb_depth_control"},
   {R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED,
    "r2vb_gpu_producer_fetched"},
   {R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER,
    "compute_identity_carrier"},
   {R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE, "triangle_render_shape"},
   {R3V_NATIVE_CELL_KIND_TRIANGLE_SAMPLED, "triangle_sampled"},
};

const char *
r3v_native_plan_queue_claim_name(enum r3v_native_plan_queue_claim c)
{
   return (unsigned)c < QUEUE_CLAIM_COUNT ? queue_claim_names[c] : NULL;
}

bool
r3v_native_plan_queue_claim_parse(const char *name,
                                  enum r3v_native_plan_queue_claim *out)
{
   for (unsigned i = 0; i < QUEUE_CLAIM_COUNT; i++) {
      if (strcmp(name, queue_claim_names[i]) == 0) {
         *out = (enum r3v_native_plan_queue_claim)i;
         return true;
      }
   }
   return false;
}

const char *
r3v_native_plan_cell_kind_name(enum r3v_native_cell_kind k)
{
   for (size_t i = 0; i < sizeof(cell_kind_names) / sizeof(cell_kind_names[0]); i++) {
      if (cell_kind_names[i].kind == k)
         return cell_kind_names[i].name;
   }
   return NULL;
}

bool
r3v_native_plan_cell_kind_parse(const char *name,
                                enum r3v_native_cell_kind *out)
{
   for (size_t i = 0; i < sizeof(cell_kind_names) / sizeof(cell_kind_names[0]); i++) {
      if (strcmp(name, cell_kind_names[i].name) == 0) {
         *out = cell_kind_names[i].kind;
         return true;
      }
   }
   return false;
}

static bool
hex_ok(const char *s, size_t want)
{
   if (strlen(s) != want)
      return false;
   for (; *s; s++) {
      if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
         return false;
   }
   return true;
}

/* Strict decimal: the canonical spelling of one value at or below max,
 * which is the digits of the value alone with "0" the one value that
 * starts with a zero; a value outside that shape refuses rather than
 * saturates.  max is at least 10 at every call site, which keeps
 * (max - d) / 10 above zero.
 */
static bool
dec_u64(const char *s, uint64_t max, uint64_t *out)
{
   if (*s == '\0' || strlen(s) > 20 || (s[0] == '0' && s[1] != '\0'))
      return false;
   uint64_t v = 0;
   for (; *s; s++) {
      if (*s < '0' || *s > '9')
         return false;
      unsigned d = (unsigned)(*s - '0');
      if (v > (max - d) / 10)
         return false;
      v = v * 10 + d;
   }
   *out = v;
   return true;
}

static bool
dec_u32(const char *s, uint32_t max, uint32_t *out)
{
   uint64_t v;
   if (!dec_u64(s, max, &v))
      return false;
   *out = (uint32_t)v;
   return true;
}

/* Strict lowercase hex of one value inside a mask, canonical spelling: no
 * prefix, no leading zero except "0", so the writer's %x form is the one
 * spelling the parser admits.
 */
static bool
hex_u32(const char *s, uint32_t mask, uint32_t *out)
{
   size_t n = strlen(s);
   if (n == 0 || n > 8 || (s[0] == '0' && n != 1))
      return false;
   uint32_t v = 0;
   for (; *s; s++) {
      unsigned d;
      if (*s >= '0' && *s <= '9')
         d = (unsigned)(*s - '0');
      else if (*s >= 'a' && *s <= 'f')
         d = 10u + (unsigned)(*s - 'a');
      else
         return false;
      v = v << 4 | d;
   }
   if (v & ~mask)
      return false;
   *out = v;
   return true;
}

static bool
name_ok(const char *s, size_t max)
{
   size_t n = strlen(s);
   if (n == 0 || n > max)
      return false;
   for (; *s; s++) {
      if (*s == '\t' || *s == '\n' || *s == '\r')
         return false;
   }
   return true;
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

enum field {
   F_SOURCE_SHA, F_SOURCE_CLEAN, F_DSO, F_DEQP, F_DEQP_RELEASE,
   F_PARTITION, F_CASELIST, F_QUEUE_CLAIM, F_KERNEL, F_MODULE, F_PCI,
   F_NONCE, F_EVIDENCE_DIR, F_MAX_IB_DWORDS, F_MAX_RELOCS, F_MAX_BYTES,
   F_MAX_SUBMISSIONS, F_MAX_RUNTIME, F_SUBMISSION_COUNT, F_COUNT,
};

static const char *const field_names[F_COUNT] = {
   "source_sha", "source_clean", "dso_blake3", "deqp_sha256",
   "deqp_release", "partition_sha256", "caselist_sha256", "queue_claim",
   "kernel_release", "module_srcversion", "pci", "nonce", "evidence_dir",
   "max_ib_dwords", "max_relocs", "max_cumulative_bytes",
   "max_submissions", "max_runtime_seconds", "submission_count",
};

static bool
copy_hex(char *dst, const char *src, size_t want)
{
   if (!hex_ok(src, want))
      return false;
   memcpy(dst, src, want + 1);
   return true;
}

static bool
copy_name(char *dst, const char *src, size_t max)
{
   if (!name_ok(src, max))
      return false;
   memcpy(dst, src, strlen(src) + 1);
   return true;
}

static bool
set_field(struct r3v_native_plan *p, enum field f, const char *v)
{
   uint64_t u64;
   uint32_t u32;
   switch (f) {
   case F_SOURCE_SHA:
      return copy_hex(p->source_sha, v, 40);
   case F_SOURCE_CLEAN:
      if (strcmp(v, "1") != 0 && strcmp(v, "0") != 0)
         return false;
      p->source_clean = v[0] == '1';
      return true;
   case F_DSO:
      return copy_hex(p->dso_blake3, v, R3V_NATIVE_PLAN_HEX64);
   case F_DEQP:
      return copy_hex(p->deqp_sha256, v, R3V_NATIVE_PLAN_HEX64);
   case F_DEQP_RELEASE:
      return copy_name(p->deqp_release, v, R3V_NATIVE_PLAN_NAME_MAX);
   case F_PARTITION:
      return copy_hex(p->partition_sha256, v, R3V_NATIVE_PLAN_HEX64);
   case F_CASELIST:
      return copy_hex(p->caselist_sha256, v, R3V_NATIVE_PLAN_HEX64);
   case F_QUEUE_CLAIM:
      return r3v_native_plan_queue_claim_parse(v, &p->queue_claim);
   case F_KERNEL:
      return copy_name(p->kernel_release, v,
                       R3V_NATIVE_PLAN_KERNEL_RELEASE_MAX);
   case F_MODULE:
      return copy_name(p->module_srcversion, v, R3V_NATIVE_PLAN_NAME_MAX);
   case F_PCI: {
      unsigned vendor, device;
      char tail;
      if (strlen(v) != 9 || v[4] != ':' ||
          sscanf(v, "%4x:%4x%c", &vendor, &device, &tail) != 2)
         return false;
      p->pci_vendor_id = vendor;
      p->pci_device_id = device;
      return true;
   }
   case F_NONCE:
      return copy_hex(p->nonce, v, 32);
   case F_EVIDENCE_DIR:
      return v[0] == '/' && copy_name(p->evidence_dir, v,
                                      R3V_NATIVE_PLAN_PATH_MAX);
   case F_MAX_IB_DWORDS:
      if (!dec_u32(v, R3V_NATIVE_PLAN_IB_DWORDS_MAX, &u32) || u32 == 0)
         return false;
      p->max_ib_dwords = u32;
      return true;
   case F_MAX_RELOCS:
      if (!dec_u32(v, R3V_NATIVE_PLAN_RELOC_MAX, &u32) || u32 == 0)
         return false;
      p->max_relocs = u32;
      return true;
   case F_MAX_BYTES:
      if (!dec_u64(v, R3V_NATIVE_PLAN_CUMULATIVE_BYTES_MAX, &u64) || u64 == 0)
         return false;
      p->max_cumulative_bytes = u64;
      return true;
   case F_MAX_SUBMISSIONS:
      if (!dec_u32(v, R3V_NATIVE_PLAN_SUBMISSION_MAX, &u32) || u32 == 0)
         return false;
      p->max_submissions = u32;
      return true;
   case F_MAX_RUNTIME:
      if (!dec_u32(v, R3V_NATIVE_PLAN_RUNTIME_SECONDS_MAX, &u32) || u32 == 0)
         return false;
      p->max_runtime_seconds = u32;
      return true;
   case F_SUBMISSION_COUNT:
      if (!dec_u32(v, R3V_NATIVE_PLAN_SUBMISSION_MAX, &u32) || u32 == 0)
         return false;
      p->submission_count = u32;
      return true;
   default:
      return false;
   }
}

/* Splits one line into at most max tab-separated columns in place. */
static unsigned
split(char *line, char **cols, unsigned max)
{
   unsigned n = 0;
   char *p = line;
   while (n < max) {
      cols[n++] = p;
      char *tab = strchr(p, '\t');
      if (tab == NULL)
         return n;
      *tab = '\0';
      p = tab + 1;
   }
   return max + 1;
}

static bool
parse_direction(const char *s, enum r3v_native_plan_direction *out)
{
   if (strcmp(s, "r") == 0)
      *out = R3V_NATIVE_PLAN_DIRECTION_READ;
   else if (strcmp(s, "w") == 0)
      *out = R3V_NATIVE_PLAN_DIRECTION_WRITE;
   else if (strcmp(s, "rw") == 0)
      *out = R3V_NATIVE_PLAN_DIRECTION_READ_WRITE;
   else
      return false;
   return true;
}

static const char *
direction_name(enum r3v_native_plan_direction d)
{
   switch (d) {
   case R3V_NATIVE_PLAN_DIRECTION_READ:
      return "r";
   case R3V_NATIVE_PLAN_DIRECTION_WRITE:
      return "w";
   case R3V_NATIVE_PLAN_DIRECTION_READ_WRITE:
      return "rw";
   default:
      return NULL;
   }
}

enum r3v_native_plan_parse_result
r3v_native_plan_parse(const char *text, size_t size,
                      struct r3v_native_plan *plan)
{
   memset(plan, 0, sizeof(*plan));
   if (size == 0 || text[size - 1] != '\n' || memchr(text, '\0', size))
      return R3V_NATIVE_PLAN_PARSE_NOT_A_PLAN;

   /* The seal line is the last line; every byte before it is sealed. */
   const char *seal_line = NULL;
   for (size_t i = size - 1; i > 0; i--) {
      if (text[i - 1] == '\n') {
         seal_line = text + i;
         break;
      }
   }
   if (seal_line == NULL)
      return R3V_NATIVE_PLAN_PARSE_NOT_A_PLAN;
   size_t seal_len = (size_t)(text + size - seal_line) - 1;
   if (seal_len != 5 + R3V_NATIVE_PLAN_HEX64 || strncmp(seal_line, "seal\t", 5) != 0)
      return R3V_NATIVE_PLAN_PARSE_SEAL_MISSING;
   char seal[R3V_NATIVE_PLAN_HEX64 + 1];
   memcpy(seal, seal_line + 5, R3V_NATIVE_PLAN_HEX64);
   seal[R3V_NATIVE_PLAN_HEX64] = '\0';
   if (!hex_ok(seal, R3V_NATIVE_PLAN_HEX64))
      return R3V_NATIVE_PLAN_PARSE_SEAL_MISSING;
   char actual[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(text, (size_t)(seal_line - text), actual);
   if (strcmp(seal, actual) != 0)
      return R3V_NATIVE_PLAN_PARSE_SEAL_MISMATCH;
   memcpy(plan->seal, seal, sizeof(seal));

   size_t body_size = (size_t)(seal_line - text);
   char *body = malloc(body_size + 1);
   if (body == NULL)
      return R3V_NATIVE_PLAN_PARSE_OUT_OF_MEMORY;
   memcpy(body, text, body_size);
   body[body_size] = '\0';

   enum r3v_native_plan_parse_result result = R3V_NATIVE_PLAN_PARSE_OK;
   bool seen[F_COUNT] = {0};
   bool header_done = false;
   struct r3v_native_plan_submission *cur = NULL;
   uint32_t submissions_seen = 0;
   uint32_t relocs_seen = 0;
   uint64_t referenced_bytes = 0;
   bool first = true;
   char *save = NULL;
   for (char *line = strtok_r(body, "\n", &save); line != NULL;
        line = strtok_r(NULL, "\n", &save)) {
      char *cols[9];
      unsigned n = split(line, cols, 8);
      if (n > 8 || n == 0) {
         result = R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE;
         break;
      }
      if (first) {
         first = false;
         if (n != 2 || strcmp(cols[0], MAGIC) != 0) {
            result = R3V_NATIVE_PLAN_PARSE_NOT_A_PLAN;
            break;
         }
         uint32_t v;
         if (!dec_u32(cols[1], UINT32_MAX, &v) ||
             v != R3V_NATIVE_PLAN_SCHEMA_VERSION) {
            result = R3V_NATIVE_PLAN_PARSE_SCHEMA_VERSION;
            break;
         }
         plan->schema_version = v;
         continue;
      }
      if (strcmp(cols[0], "submission") == 0) {
         if (!header_done) {
            for (unsigned f = 0; f < F_COUNT; f++) {
               if (!seen[f]) {
                  result = R3V_NATIVE_PLAN_PARSE_MISSING_FIELD;
                  goto out;
               }
            }
            /* A submission line is at least 84 bytes, so the declared
             * count cannot exceed the body's capacity; the allocation
             * follows the text, never the declaration alone.
             */
            if ((size_t)plan->submission_count > body_size / 84) {
               result = R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT;
               goto out;
            }
            plan->submissions = calloc(plan->submission_count,
                                       sizeof(*plan->submissions));
            if (plan->submissions == NULL) {
               result = R3V_NATIVE_PLAN_PARSE_OUT_OF_MEMORY;
               goto out;
            }
            header_done = true;
         }
         if (cur != NULL && relocs_seen != cur->reloc_count) {
            result = R3V_NATIVE_PLAN_PARSE_RELOC_COUNT;
            break;
         }
         if (n != 7) {
            result = R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE;
            break;
         }
         uint32_t index;
         if (!dec_u32(cols[1], R3V_NATIVE_PLAN_SUBMISSION_MAX, &index) ||
             index != submissions_seen) {
            result = R3V_NATIVE_PLAN_PARSE_SUBMISSION_ORDER;
            break;
         }
         if (submissions_seen == plan->submission_count) {
            result = R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT;
            break;
         }
         cur = &plan->submissions[submissions_seen++];
         relocs_seen = 0;
         if (!copy_hex(cur->ib_blake3, cols[2], R3V_NATIVE_PLAN_HEX64) ||
             !dec_u32(cols[3], UINT32_MAX, &cur->ib_dwords) ||
             cur->ib_dwords == 0 ||
             !r3v_native_plan_cell_kind_parse(cols[4], &cur->cell_kind) ||
             cur->cell_kind == R3V_NATIVE_CELL_KIND_UNDECLARED ||
             !copy_name(cur->emitter, cols[5], R3V_NATIVE_PLAN_NAME_MAX) ||
             !dec_u32(cols[6], R3V_NATIVE_PLAN_RELOC_MAX,
                      &cur->reloc_count) ||
             cur->reloc_count == 0) {
            result = R3V_NATIVE_PLAN_PARSE_BAD_VALUE;
            break;
         }
         if (cur->ib_dwords > plan->max_ib_dwords ||
             cur->reloc_count > plan->max_relocs ||
             submissions_seen > plan->max_submissions) {
            result = R3V_NATIVE_PLAN_PARSE_CEILING_EXCEEDED;
            break;
         }
         continue;
      }
      if (strcmp(cols[0], "reloc") == 0) {
         if (cur == NULL || n != 8) {
            result = R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE;
            break;
         }
         uint32_t sub, slot;
         if (!dec_u32(cols[1], R3V_NATIVE_PLAN_SUBMISSION_MAX, &sub) ||
             sub != submissions_seen - 1 ||
             !dec_u32(cols[2], R3V_NATIVE_PLAN_RELOC_MAX, &slot) ||
             slot != relocs_seen) {
            result = R3V_NATIVE_PLAN_PARSE_SUBMISSION_ORDER;
            break;
         }
         if (relocs_seen == cur->reloc_count) {
            result = R3V_NATIVE_PLAN_PARSE_RELOC_COUNT;
            break;
         }
         struct r3v_native_plan_reloc *r = &cur->relocs[relocs_seen++];
         if (!copy_name(r->role, cols[3], R3V_NATIVE_PLAN_NAME_MAX) ||
             !hex_u32(cols[4], R3V_NATIVE_PLAN_DOMAIN_MASK,
                      &r->read_domains) ||
             !hex_u32(cols[5], R3V_NATIVE_PLAN_DOMAIN_MASK,
                      &r->write_domain) ||
             !dec_u64(cols[6], R3V_NATIVE_PLAN_CUMULATIVE_BYTES_MAX,
                      &r->size) ||
             r->size == 0 || !parse_direction(cols[7], &r->direction)) {
            result = R3V_NATIVE_PLAN_PARSE_BAD_VALUE;
            break;
         }
         if (r->size > UINT64_MAX - referenced_bytes ||
             referenced_bytes + r->size > plan->max_cumulative_bytes) {
            result = R3V_NATIVE_PLAN_PARSE_CEILING_EXCEEDED;
            break;
         }
         referenced_bytes += r->size;
         continue;
      }
      if (header_done || n != 2) {
         result = R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE;
         break;
      }
      unsigned f;
      for (f = 0; f < F_COUNT; f++) {
         if (strcmp(cols[0], field_names[f]) == 0)
            break;
      }
      if (f == F_COUNT) {
         result = R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE;
         break;
      }
      if (seen[f]) {
         result = R3V_NATIVE_PLAN_PARSE_DUPLICATE_FIELD;
         break;
      }
      if (!set_field(plan, (enum field)f, cols[1])) {
         result = R3V_NATIVE_PLAN_PARSE_BAD_VALUE;
         break;
      }
      seen[f] = true;
   }
   if (result == R3V_NATIVE_PLAN_PARSE_OK) {
      bool header_complete = true;
      for (unsigned f = 0; f < F_COUNT; f++)
         header_complete = header_complete && seen[f];
      if (!header_complete)
         result = R3V_NATIVE_PLAN_PARSE_MISSING_FIELD;
      else if (!header_done)
         result = R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT;
      else if (cur != NULL && relocs_seen != cur->reloc_count)
         result = R3V_NATIVE_PLAN_PARSE_RELOC_COUNT;
      else if (submissions_seen != plan->submission_count)
         result = R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT;
   }
   /* The canonical-form clause: the parsed plan re-serializes to the
    * bytes it came from, so a blank line, a permuted header, or an
    * alternate spelling of one value cannot give one authorization a
    * second digest.
    */
   if (result == R3V_NATIVE_PLAN_PARSE_OK) {
      char *again = malloc(size);
      if (again == NULL) {
         result = R3V_NATIVE_PLAN_PARSE_OUT_OF_MEMORY;
      } else {
         long n = r3v_native_plan_write(plan, again, size);
         if (n != (long)size || memcmp(again, text, size) != 0)
            result = R3V_NATIVE_PLAN_PARSE_NONCANONICAL;
         free(again);
      }
   }
out:
   free(body);
   if (result != R3V_NATIVE_PLAN_PARSE_OK)
      r3v_native_plan_finish(plan);
   return result;
}

void
r3v_native_plan_finish(struct r3v_native_plan *plan)
{
   free(plan->submissions);
   memset(plan, 0, sizeof(*plan));
}

/* Appends formatted text, tracking the byte count even past the buffer
 * so the caller learns the size it needs.
 */
static void
emit(char *out, size_t out_size, size_t *pos, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   size_t room = *pos < out_size ? out_size - *pos : 0;
   int n = vsnprintf(room ? out + *pos : NULL, room, fmt, ap);
   va_end(ap);
   if (n > 0)
      *pos += (size_t)n;
}

/* Every predicate the parser applies, over the struct, so the writer
 * refuses what the parser would refuse instead of sealing it.
 */
static bool
plan_in_schema(const struct r3v_native_plan *plan)
{
   if (plan->schema_version != R3V_NATIVE_PLAN_SCHEMA_VERSION ||
       !hex_ok(plan->source_sha, 40) ||
       !hex_ok(plan->dso_blake3, R3V_NATIVE_PLAN_HEX64) ||
       !hex_ok(plan->deqp_sha256, R3V_NATIVE_PLAN_HEX64) ||
       !name_ok(plan->deqp_release, R3V_NATIVE_PLAN_NAME_MAX) ||
       !hex_ok(plan->partition_sha256, R3V_NATIVE_PLAN_HEX64) ||
       !hex_ok(plan->caselist_sha256, R3V_NATIVE_PLAN_HEX64) ||
       r3v_native_plan_queue_claim_name(plan->queue_claim) == NULL ||
       !name_ok(plan->kernel_release, R3V_NATIVE_PLAN_KERNEL_RELEASE_MAX) ||
       !name_ok(plan->module_srcversion, R3V_NATIVE_PLAN_NAME_MAX) ||
       plan->pci_vendor_id > 0xffff || plan->pci_device_id > 0xffff ||
       !hex_ok(plan->nonce, 32) || plan->evidence_dir[0] != '/' ||
       !name_ok(plan->evidence_dir, R3V_NATIVE_PLAN_PATH_MAX) ||
       plan->max_ib_dwords == 0 ||
       plan->max_ib_dwords > R3V_NATIVE_PLAN_IB_DWORDS_MAX ||
       plan->max_relocs == 0 || plan->max_relocs > R3V_NATIVE_PLAN_RELOC_MAX ||
       plan->max_cumulative_bytes == 0 ||
       plan->max_cumulative_bytes > R3V_NATIVE_PLAN_CUMULATIVE_BYTES_MAX ||
       plan->max_submissions == 0 ||
       plan->max_submissions > R3V_NATIVE_PLAN_SUBMISSION_MAX ||
       plan->max_runtime_seconds == 0 ||
       plan->max_runtime_seconds > R3V_NATIVE_PLAN_RUNTIME_SECONDS_MAX ||
       plan->submission_count == 0 || plan->submissions == NULL ||
       plan->submission_count > plan->max_submissions)
      return false;
   for (uint32_t i = 0; i < plan->submission_count; i++) {
      const struct r3v_native_plan_submission *s = &plan->submissions[i];
      if (!hex_ok(s->ib_blake3, R3V_NATIVE_PLAN_HEX64) ||
          s->ib_dwords == 0 || s->ib_dwords > plan->max_ib_dwords ||
          s->cell_kind == R3V_NATIVE_CELL_KIND_UNDECLARED ||
          r3v_native_plan_cell_kind_name(s->cell_kind) == NULL ||
          !name_ok(s->emitter, R3V_NATIVE_PLAN_NAME_MAX) ||
          s->reloc_count == 0 || s->reloc_count > plan->max_relocs)
         return false;
      for (uint32_t r = 0; r < s->reloc_count; r++) {
         const struct r3v_native_plan_reloc *rl = &s->relocs[r];
         if (!name_ok(rl->role, R3V_NATIVE_PLAN_NAME_MAX) ||
             (rl->read_domains & ~R3V_NATIVE_PLAN_DOMAIN_MASK) ||
             (rl->write_domain & ~R3V_NATIVE_PLAN_DOMAIN_MASK) ||
             rl->size == 0 ||
             rl->size > R3V_NATIVE_PLAN_CUMULATIVE_BYTES_MAX ||
             direction_name(rl->direction) == NULL)
            return false;
      }
   }
   return true;
}

static size_t
emit_body(const struct r3v_native_plan *plan, char *out, size_t out_size);

long
r3v_native_plan_write(const struct r3v_native_plan *plan, char *out,
                      size_t out_size)
{
   if (!plan_in_schema(plan))
      return -1;
   /* Size first, then emit: a buffer too small for the whole plan stays
    * untouched, and the seal is written last over the exact body.
    */
   size_t body = emit_body(plan, NULL, 0);
   size_t total = body + 5 + R3V_NATIVE_PLAN_HEX64 + 1;
   if (out == NULL || total > out_size)
      return (long)total;
   size_t again = emit_body(plan, out, out_size);
   if (again != body)
      return -1;
   return (long)r3v_native_plan_seal(out, out_size, body);
}

static size_t
emit_body(const struct r3v_native_plan *plan, char *out, size_t out_size)
{
   size_t pos = 0;
   emit(out, out_size, &pos, "%s\t%u\n", MAGIC, plan->schema_version);
   emit(out, out_size, &pos, "source_sha\t%s\n", plan->source_sha);
   emit(out, out_size, &pos, "source_clean\t%u\n", plan->source_clean ? 1u : 0u);
   emit(out, out_size, &pos, "dso_blake3\t%s\n", plan->dso_blake3);
   emit(out, out_size, &pos, "deqp_sha256\t%s\n", plan->deqp_sha256);
   emit(out, out_size, &pos, "deqp_release\t%s\n", plan->deqp_release);
   emit(out, out_size, &pos, "partition_sha256\t%s\n", plan->partition_sha256);
   emit(out, out_size, &pos, "caselist_sha256\t%s\n", plan->caselist_sha256);
   emit(out, out_size, &pos, "queue_claim\t%s\n",
        r3v_native_plan_queue_claim_name(plan->queue_claim));
   emit(out, out_size, &pos, "kernel_release\t%s\n", plan->kernel_release);
   emit(out, out_size, &pos, "module_srcversion\t%s\n", plan->module_srcversion);
   emit(out, out_size, &pos, "pci\t%04x:%04x\n", plan->pci_vendor_id,
        plan->pci_device_id);
   emit(out, out_size, &pos, "nonce\t%s\n", plan->nonce);
   emit(out, out_size, &pos, "evidence_dir\t%s\n", plan->evidence_dir);
   emit(out, out_size, &pos, "max_ib_dwords\t%u\n", plan->max_ib_dwords);
   emit(out, out_size, &pos, "max_relocs\t%u\n", plan->max_relocs);
   emit(out, out_size, &pos, "max_cumulative_bytes\t%llu\n",
        (unsigned long long)plan->max_cumulative_bytes);
   emit(out, out_size, &pos, "max_submissions\t%u\n", plan->max_submissions);
   emit(out, out_size, &pos, "max_runtime_seconds\t%u\n",
        plan->max_runtime_seconds);
   emit(out, out_size, &pos, "submission_count\t%u\n", plan->submission_count);
   for (uint32_t i = 0; i < plan->submission_count; i++) {
      const struct r3v_native_plan_submission *s = &plan->submissions[i];
      emit(out, out_size, &pos, "submission\t%u\t%s\t%u\t%s\t%s\t%u\n", i,
           s->ib_blake3, s->ib_dwords,
           r3v_native_plan_cell_kind_name(s->cell_kind), s->emitter,
           s->reloc_count);
      for (uint32_t r = 0; r < s->reloc_count; r++) {
         const struct r3v_native_plan_reloc *rl = &s->relocs[r];
         emit(out, out_size, &pos, "reloc\t%u\t%u\t%s\t%x\t%x\t%llu\t%s\n",
              i, r, rl->role, rl->read_domains, rl->write_domain,
              (unsigned long long)rl->size, direction_name(rl->direction));
      }
   }
   return pos;
}

size_t
r3v_native_plan_seal(char *text, size_t text_size, size_t body_size)
{
   const size_t seal_size = 5 + R3V_NATIVE_PLAN_HEX64 + 1;
   if (body_size > text_size || body_size > SIZE_MAX - seal_size)
      return 0;
   size_t total = body_size + seal_size;
   if (total > text_size)
      return 0;
   char seal[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(text, body_size, seal);
   memcpy(text + body_size, "seal\t", 5);
   memcpy(text + body_size + 5, seal, R3V_NATIVE_PLAN_HEX64);
   text[total - 1] = '\n';
   return total;
}

static bool
str_eq(const char *plan_value, const char *live)
{
   return live != NULL && strcmp(plan_value, live) == 0;
}

enum r3v_native_plan_bind_result
r3v_native_plan_bind(const struct r3v_native_plan *plan,
                     const struct r3v_native_plan_identity *id)
{
   if (id == NULL)
      return R3V_NATIVE_PLAN_BIND_SOURCE;
   if (!str_eq(plan->source_sha, id->source_sha))
      return R3V_NATIVE_PLAN_BIND_SOURCE;
   if (!plan->source_clean || !id->source_clean)
      return R3V_NATIVE_PLAN_BIND_SOURCE_DIRTY;
   if (!str_eq(plan->dso_blake3, id->dso_blake3))
      return R3V_NATIVE_PLAN_BIND_DSO;
   if (!str_eq(plan->deqp_sha256, id->deqp_sha256))
      return R3V_NATIVE_PLAN_BIND_DEQP;
   if (!str_eq(plan->deqp_release, id->deqp_release))
      return R3V_NATIVE_PLAN_BIND_DEQP_RELEASE;
   if (!str_eq(plan->partition_sha256, id->partition_sha256))
      return R3V_NATIVE_PLAN_BIND_PARTITION;
   if (!str_eq(plan->caselist_sha256, id->caselist_sha256))
      return R3V_NATIVE_PLAN_BIND_CASELIST;
   if (plan->queue_claim != id->queue_claim)
      return R3V_NATIVE_PLAN_BIND_QUEUE_CLAIM;
   if (!str_eq(plan->kernel_release, id->kernel_release))
      return R3V_NATIVE_PLAN_BIND_KERNEL;
   if (!str_eq(plan->module_srcversion, id->module_srcversion))
      return R3V_NATIVE_PLAN_BIND_MODULE;
   if (plan->pci_vendor_id != id->pci_vendor_id ||
       plan->pci_device_id != id->pci_device_id ||
       plan->pci_vendor_id != R3V_NATIVE_ARMING_PCI_VENDOR ||
       plan->pci_device_id != R3V_NATIVE_ARMING_PCI_DEVICE)
      return R3V_NATIVE_PLAN_BIND_PCI;
   if (!str_eq(plan->nonce, id->nonce))
      return R3V_NATIVE_PLAN_BIND_NONCE;
   if (!id->evidence_dir_present || !id->evidence_dir_empty)
      return R3V_NATIVE_PLAN_BIND_EVIDENCE_DIR;
   if (id->gates_open)
      return R3V_NATIVE_PLAN_BIND_GATE_CONTAMINATION;
   return R3V_NATIVE_PLAN_BIND_OK;
}

bool
r3v_native_plan_gates_open(const char *(*read_env)(void *ctx,
                                                   const char *name),
                           void *ctx, const char **gate_out)
{
   static const char *const fixed[] = {
      "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
      "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
      "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE",
      "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
      "R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS",
      "R3V_NATIVE_AUTHORIZED_BURST_DRAWS",
      "R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL",
      "R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL",
      "R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL",
      R300_COMPUTE_QUEUE_CLAIM_GATE,
   };
   for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
      const char *v = read_env(ctx, fixed[i]);
      if (v != NULL && v[0] != '\0') {
         *gate_out = fixed[i];
         return true;
      }
   }
   uint32_t verb_count = 0;
   const struct r300_compute_verb_row *rows =
      r300_compute_verb_rows(&verb_count);
   for (uint32_t v = 0; v < verb_count; v++) {
      const char *value = read_env(ctx, rows[v].gpu_gate);
      if (value != NULL && value[0] != '\0') {
         *gate_out = rows[v].gpu_gate;
         return true;
      }
   }
   *gate_out = NULL;
   return false;
}

enum r3v_native_plan_match_result
r3v_native_plan_match(const struct r3v_native_plan_submission *e,
                      const struct r3v_native_plan_submission *a)
{
   if (strcmp(e->ib_blake3, a->ib_blake3) != 0)
      return R3V_NATIVE_PLAN_MATCH_DIGEST;
   if (e->ib_dwords != a->ib_dwords)
      return R3V_NATIVE_PLAN_MATCH_DWORDS;
   if (e->cell_kind != a->cell_kind)
      return R3V_NATIVE_PLAN_MATCH_CELL_KIND;
   if (strcmp(e->emitter, a->emitter) != 0)
      return R3V_NATIVE_PLAN_MATCH_EMITTER;
   if (e->reloc_count != a->reloc_count)
      return R3V_NATIVE_PLAN_MATCH_RELOC_COUNT;
   for (uint32_t i = 0; i < e->reloc_count; i++) {
      const struct r3v_native_plan_reloc *x = &e->relocs[i];
      const struct r3v_native_plan_reloc *y = &a->relocs[i];
      if (strcmp(x->role, y->role) != 0)
         return R3V_NATIVE_PLAN_MATCH_RELOC_ROLE;
      if (x->read_domains != y->read_domains ||
          x->write_domain != y->write_domain)
         return R3V_NATIVE_PLAN_MATCH_RELOC_DOMAINS;
      if (x->size != y->size)
         return R3V_NATIVE_PLAN_MATCH_RELOC_SIZE;
      if (x->direction != y->direction)
         return R3V_NATIVE_PLAN_MATCH_RELOC_DIRECTION;
   }
   return R3V_NATIVE_PLAN_MATCH_OK;
}

static enum r3v_native_plan_session_result
latch(struct r3v_native_plan_session *s,
      enum r3v_native_plan_session_result why)
{
   if (!s->terminal) {
      s->terminal = true;
      s->terminal_reason = why;
   }
   return why;
}

void
r3v_native_plan_session_init(struct r3v_native_plan_session *s)
{
   memset(s, 0, sizeof(*s));
}

enum r3v_native_plan_session_result
r3v_native_plan_session_bind(struct r3v_native_plan_session *s,
                             const struct r3v_native_plan *plan)
{
   if (s->bound || s->terminal || s->completed)
      return R3V_NATIVE_PLAN_SESSION_CONSUMED;
   if (plan == NULL || plan->submission_count == 0)
      return R3V_NATIVE_PLAN_SESSION_UNBOUND;
   memset(s, 0, sizeof(*s));
   s->plan = plan;
   s->bound = true;
   return R3V_NATIVE_PLAN_SESSION_ADMITTED;
}

enum r3v_native_plan_session_result
r3v_native_plan_session_admit(struct r3v_native_plan_session *s,
                              const struct r3v_native_plan_submission *a,
                              uint32_t executable_count,
                              uint64_t elapsed_seconds)
{
   if (!s->bound)
      return R3V_NATIVE_PLAN_SESSION_UNBOUND;
   if (s->terminal || s->completed)
      return R3V_NATIVE_PLAN_SESSION_TERMINAL;
   if (executable_count != 1)
      return latch(s, R3V_NATIVE_PLAN_SESSION_EXECUTABLE_COUNT);
   if (s->next_index >= s->plan->submission_count)
      return latch(s, R3V_NATIVE_PLAN_SESSION_EXHAUSTED);
   if (elapsed_seconds > s->plan->max_runtime_seconds)
      return latch(s, R3V_NATIVE_PLAN_SESSION_RUNTIME_EXCEEDED);
   const struct r3v_native_plan_submission *e =
      &s->plan->submissions[s->next_index];
   enum r3v_native_plan_match_result m = r3v_native_plan_match(e, a);
   if (m != R3V_NATIVE_PLAN_MATCH_OK) {
      s->last_mismatch = m;
      return latch(s, R3V_NATIVE_PLAN_SESSION_MISMATCH);
   }
   uint64_t bytes = 0;
   for (uint32_t i = 0; i < a->reloc_count; i++) {
      if (a->relocs[i].size > UINT64_MAX - bytes)
         return latch(s, R3V_NATIVE_PLAN_SESSION_BYTES_EXCEEDED);
      bytes += a->relocs[i].size;
   }
   if (bytes > s->plan->max_cumulative_bytes ||
       bytes > UINT64_MAX - s->referenced_bytes ||
       s->referenced_bytes + bytes > s->plan->max_cumulative_bytes)
      return latch(s, R3V_NATIVE_PLAN_SESSION_BYTES_EXCEEDED);
   s->referenced_bytes += bytes;
   s->next_index++;
   return R3V_NATIVE_PLAN_SESSION_ADMITTED;
}

void
r3v_native_plan_session_fail(struct r3v_native_plan_session *s,
                             enum r3v_native_plan_session_result why)
{
   if (why == R3V_NATIVE_PLAN_SESSION_ADMITTED)
      why = R3V_NATIVE_PLAN_SESSION_TERMINAL;
   latch(s, why);
}

enum r3v_native_plan_session_result
r3v_native_plan_session_finish(struct r3v_native_plan_session *s,
                               uint64_t elapsed_seconds)
{
   if (!s->bound)
      return R3V_NATIVE_PLAN_SESSION_UNBOUND;
   if (s->terminal)
      return R3V_NATIVE_PLAN_SESSION_TERMINAL;
   if (s->completed)
      return R3V_NATIVE_PLAN_SESSION_CONSUMED;
   if (s->next_index != s->plan->submission_count)
      return latch(s, R3V_NATIVE_PLAN_SESSION_INCOMPLETE);
   if (elapsed_seconds > s->plan->max_runtime_seconds)
      return latch(s, R3V_NATIVE_PLAN_SESSION_RUNTIME_EXCEEDED);
   s->completed = true;
   return R3V_NATIVE_PLAN_SESSION_ADMITTED;
}

#define NAME_TABLE(fn, type, last, ...)                                  \
   const char *fn(type v)                                                \
   {                                                                     \
      static const char *const names[] = {__VA_ARGS__};                  \
      static_assert(sizeof(names) / sizeof(names[0]) == (last) + 1,      \
                    "name table tracks its enum");                       \
      return (unsigned)v < sizeof(names) / sizeof(names[0]) ? names[v]  \
                                                             : "?";      \
   }

NAME_TABLE(r3v_native_plan_parse_result_name,
           enum r3v_native_plan_parse_result,
           R3V_NATIVE_PLAN_PARSE_OUT_OF_MEMORY, "ok", "not_a_plan",
           "schema_version", "malformed_line", "duplicate_field",
           "missing_field", "bad_value", "submission_order",
           "submission_count", "reloc_count", "ceiling_exceeded",
           "seal_missing", "seal_mismatch", "noncanonical",
           "out_of_memory")
NAME_TABLE(r3v_native_plan_bind_result_name,
           enum r3v_native_plan_bind_result,
           R3V_NATIVE_PLAN_BIND_GATE_CONTAMINATION, "ok", "source", "source_dirty",
           "dso", "deqp", "deqp_release", "partition", "caselist",
           "queue_claim", "kernel", "module", "pci", "nonce",
           "evidence_dir", "gate_contamination")
NAME_TABLE(r3v_native_plan_match_result_name,
           enum r3v_native_plan_match_result,
           R3V_NATIVE_PLAN_MATCH_RELOC_DIRECTION, "ok", "digest", "dwords",
           "cell_kind", "emitter", "reloc_count", "reloc_role",
           "reloc_domains", "reloc_size", "reloc_direction")
NAME_TABLE(r3v_native_plan_session_result_name,
           enum r3v_native_plan_session_result,
           R3V_NATIVE_PLAN_SESSION_CONSUMED, "admitted", "unbound",
           "terminal", "exhausted", "mismatch", "executable_count",
           "runtime_exceeded", "bytes_exceeded", "incomplete", "consumed")
