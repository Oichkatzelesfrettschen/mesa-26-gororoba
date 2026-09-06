/* SPDX-License-Identifier: MIT */

#include "r3v_measurement_session.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const refusal_names[R3V_MEASUREMENT_SESSION_REFUSAL_COUNT] = {
   [R3V_MEASUREMENT_SESSION_ADMITTED] = "admitted",
   [R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE] = "session_inactive",
   [R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED] = "manifest_malformed",
   [R3V_MEASUREMENT_SESSION_REFUSE_SCHEMA] = "schema_unknown",
   [R3V_MEASUREMENT_SESSION_REFUSE_EPOCH] = "epoch_mismatch",
   [R3V_MEASUREMENT_SESSION_REFUSE_ROUTE_MISMATCH] = "route_mismatch",
   [R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED] = "case_undeclared",
   [R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH] = "role_mismatch",
   [R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND] = "destination_rebound",
   [R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH] = "identity_mismatch",
   [R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED] = "budget_exhausted",
   [R3V_MEASUREMENT_SESSION_REFUSE_CLOSED] = "session_closed",
};

const char *
r3v_measurement_session_refusal_name(enum r3v_measurement_session_refusal r)
{
   if (r < 0 || r >= R3V_MEASUREMENT_SESSION_REFUSAL_COUNT ||
       refusal_names[r] == NULL)
      return "unknown";
   return refusal_names[r];
}

/* One key = value line, trimmed on both sides.  Returns false at the end
 * of the text; a line carrying no '=' outside a comment refuses through
 * *malformed. */
struct line_cursor {
   const char *p;
   const char *end;
};

static bool
next_field(struct line_cursor *c, char *key, size_t key_size, char *value,
           size_t value_size, bool *malformed)
{
   *malformed = false;
   while (c->p < c->end) {
      const char *line = c->p;
      const char *nl = memchr(line, '\n', (size_t)(c->end - line));
      const char *line_end = nl != NULL ? nl : c->end;
      c->p = nl != NULL ? nl + 1 : c->end;

      while (line < line_end && (*line == ' ' || *line == '\t'))
         line++;
      while (line_end > line &&
             (line_end[-1] == ' ' || line_end[-1] == '\t' ||
              line_end[-1] == '\r'))
         line_end--;
      if (line == line_end || *line == '#')
         continue;

      const char *eq = memchr(line, '=', (size_t)(line_end - line));
      if (eq == NULL) {
         *malformed = true;
         return false;
      }
      const char *key_end = eq;
      while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t'))
         key_end--;
      const char *val = eq + 1;
      while (val < line_end && (*val == ' ' || *val == '\t'))
         val++;

      const size_t key_len = (size_t)(key_end - line);
      const size_t val_len = (size_t)(line_end - val);
      if (key_len == 0 || key_len >= key_size || val_len >= value_size) {
         *malformed = true;
         return false;
      }
      memcpy(key, line, key_len);
      key[key_len] = '\0';
      memcpy(value, val, val_len);
      value[val_len] = '\0';
      return true;
   }
   return false;
}

/* Decimal or 0x-prefixed hexadecimal, the whole value and nothing after
 * it.  The base is chosen here rather than passed as zero, because
 * strtoull's base zero reads a leading zero as octal and the declaration
 * grammar has no octal: 0644 would name 420.  A leading sign is rejected
 * before the conversion for the same reason -- strtoull accepts one and
 * negates into the unsigned range, so -1 would name 2^64 - 1 rather than
 * refuse. */
static bool
parse_u64(const char *text, uint64_t *out)
{
   if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == '+')
      return false;
   int base = 10;
   const char *digits = text;
   if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      base = 16;
      digits = text + 2;
   } else if (text[0] == '0' && text[1] != '\0') {
      return false;
   }
   if (digits[0] == '\0')
      return false;
   char *end = NULL;
   errno = 0;
   const unsigned long long value = strtoull(digits, &end, base);
   if (errno != 0 || end == NULL || *end != '\0')
      return false;
   *out = (uint64_t)value;
   return true;
}

static bool
parse_u32(const char *text, uint32_t *out)
{
   uint64_t wide;
   if (!parse_u64(text, &wide) || wide > UINT32_MAX)
      return false;
   *out = (uint32_t)wide;
   return true;
}

static bool
copy_name(char *dst, size_t dst_size, const char *value)
{
   const size_t len = strlen(value);
   if (len == 0 || len >= dst_size)
      return false;
   memcpy(dst, value, len + 1);
   return true;
}

/* `case = id, offset, bytes, value, warmups, repetitions`, six fields,
 * every one required. */
static bool
parse_case(const char *value, struct r3v_measurement_case *out)
{
   char buffer[256];
   if (strlen(value) >= sizeof(buffer))
      return false;
   memcpy(buffer, value, strlen(value) + 1);

   char *fields[6];
   uint32_t count = 0;
   char *cursor = buffer;
   /* The sixth field ends the row, so a separator inside it names a
    * seventh the row does not carry.  It is read before the split
    * overwrites it, which is what a check after the loop can no longer
    * see. */
   bool trailing_separator = false;
   while (count < 6) {
      char *comma = strchr(cursor, ',');
      fields[count++] = cursor;
      if (comma == NULL)
         break;
      if (count == 6) {
         trailing_separator = true;
         break;
      }
      *comma = '\0';
      cursor = comma + 1;
      while (*cursor == ' ' || *cursor == '\t')
         cursor++;
   }
   if (count != 6 || trailing_separator)
      return false;

   uint64_t offset, bytes;
   if (!parse_u32(fields[0], &out->case_id) ||
       !parse_u64(fields[1], &offset) || !parse_u64(fields[2], &bytes) ||
       !parse_u32(fields[3], &out->fill_value) ||
       !parse_u32(fields[4], &out->warmups) ||
       !parse_u32(fields[5], &out->repetitions))
      return false;
   /* A case that runs nothing declares nothing, and vkCmdFillBuffer's
    * range counts whole dwords on a dword boundary. */
   if (bytes == 0 || bytes % R3V_FILL_ROUTE_ELEMENT_BYTES != 0 ||
       offset % R3V_FILL_ROUTE_ELEMENT_BYTES != 0 ||
       out->warmups + out->repetitions == 0)
      return false;
   out->fill_offset = offset;
   out->fill_bytes = bytes;
   return true;
}

enum r3v_measurement_session_refusal
r3v_measurement_manifest_parse(const char *text, size_t length,
                               struct r3v_measurement_manifest *out,
                               const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (text == NULL || out == NULL ||
       length > R3V_MEASUREMENT_SESSION_TEXT_MAX) {
      *reason = "the declaration is absent or above the text bound";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   memset(out, 0, sizeof(*out));

   /* Every scalar field is required, so a bitmask of what was seen
    * decides the refusal rather than a zero standing in for a
    * declaration. */
   enum {
      SEEN_SCHEMA = 1u << 0,
      SEEN_NONCE = 1u << 1,
      SEEN_PLATFORM = 1u << 2,
      SEEN_ROUTE = 1u << 3,
      SEEN_VENDOR = 1u << 4,
      SEEN_DEVICE = 1u << 5,
      SEEN_KERNEL = 1u << 6,
      SEEN_MODULE = 1u << 7,
      SEEN_ALLOCATION = 1u << 8,
      SEEN_BUFFER = 1u << 9,
      SEEN_BINDING = 1u << 10,
      SEEN_MEMORY_FLAGS = 1u << 11,
      SEEN_USAGE = 1u << 12,
      SEEN_WRITE_DOMAIN = 1u << 13,
      SEEN_MAX_SUBMISSIONS = 1u << 14,
      SEEN_TIMEOUT = 1u << 15,
      SEEN_ALL = (1u << 16) - 1u,
   };
   uint32_t seen = 0;

   struct line_cursor cursor = { .p = text, .end = text + length };
   char key[64];
   char value[256];
   bool malformed = false;
   while (next_field(&cursor, key, sizeof(key), value, sizeof(value),
                     &malformed)) {
      bool ok = true;
      if (strcmp(key, "schema") == 0) {
         ok = copy_name(out->schema, sizeof(out->schema), value);
         seen |= SEEN_SCHEMA;
      } else if (strcmp(key, "session_nonce") == 0) {
         ok = copy_name(out->session_nonce, sizeof(out->session_nonce), value);
         seen |= SEEN_NONCE;
      } else if (strcmp(key, "platform") == 0) {
         ok = copy_name(out->platform, sizeof(out->platform), value);
         seen |= SEEN_PLATFORM;
      } else if (strcmp(key, "route") == 0) {
         ok = copy_name(out->route, sizeof(out->route), value);
         seen |= SEEN_ROUTE;
      } else if (strcmp(key, "pci_vendor_id") == 0) {
         ok = parse_u32(value, &out->pci_vendor_id);
         seen |= SEEN_VENDOR;
      } else if (strcmp(key, "pci_device_id") == 0) {
         ok = parse_u32(value, &out->pci_device_id);
         seen |= SEEN_DEVICE;
      } else if (strcmp(key, "kernel_release") == 0) {
         ok = copy_name(out->kernel_release, sizeof(out->kernel_release),
                        value);
         seen |= SEEN_KERNEL;
      } else if (strcmp(key, "module_srcversion") == 0) {
         ok = copy_name(out->module_srcversion,
                        sizeof(out->module_srcversion), value);
         seen |= SEEN_MODULE;
      } else if (strcmp(key, "allocation_bytes") == 0) {
         ok = parse_u64(value, &out->role.allocation_bytes);
         seen |= SEEN_ALLOCATION;
      } else if (strcmp(key, "buffer_bytes") == 0) {
         ok = parse_u64(value, &out->role.buffer_bytes);
         seen |= SEEN_BUFFER;
      } else if (strcmp(key, "binding_offset") == 0) {
         ok = parse_u64(value, &out->role.binding_offset);
         seen |= SEEN_BINDING;
      } else if (strcmp(key, "memory_property_flags") == 0) {
         ok = parse_u32(value, &out->role.memory_property_flags);
         seen |= SEEN_MEMORY_FLAGS;
      } else if (strcmp(key, "buffer_usage") == 0) {
         ok = parse_u32(value, &out->role.buffer_usage);
         seen |= SEEN_USAGE;
      } else if (strcmp(key, "write_domain") == 0) {
         ok = parse_u32(value, &out->role.write_domain);
         seen |= SEEN_WRITE_DOMAIN;
      } else if (strcmp(key, "max_total_submissions") == 0) {
         ok = parse_u32(value, &out->max_total_submissions);
         seen |= SEEN_MAX_SUBMISSIONS;
      } else if (strcmp(key, "completion_timeout_ns") == 0) {
         ok = parse_u64(value, &out->completion_timeout_ns);
         seen |= SEEN_TIMEOUT;
      } else if (strcmp(key, "case") == 0) {
         if (out->case_count == R3V_MEASUREMENT_SESSION_MAX_CASES) {
            *reason = "more cases than the declaration bound admits";
            return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
         }
         ok = parse_case(value, &out->cases[out->case_count]);
         if (ok)
            out->case_count++;
      } else {
         *reason = "the declaration carries a key this reader does not read";
         return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
      }
      if (!ok) {
         *reason = "a declared value is unreadable or outside its field";
         return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
      }
   }
   if (malformed) {
      *reason = "a declaration line carries no key and value";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   if (seen != SEEN_ALL || out->case_count == 0) {
      *reason = "the declaration omits a required field or every case";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   if (strcmp(out->schema, R3V_MEASUREMENT_SESSION_SCHEMA) != 0) {
      *reason = "the declaration names another schema";
      return R3V_MEASUREMENT_SESSION_REFUSE_SCHEMA;
   }

   /* Two cases agreeing on offset, size, and value are the same case,
    * and a request matching both would consume an ambiguous budget. */
   for (uint32_t i = 0; i < out->case_count; i++) {
      const struct r3v_measurement_case *a = &out->cases[i];
      /* The case closes inside the buffer, and the buffer inside its
       * allocation, so a declaration names no fill the memory contract
       * would refuse anyway. */
      if (a->fill_offset > out->role.buffer_bytes ||
          a->fill_bytes > out->role.buffer_bytes - a->fill_offset ||
          out->role.binding_offset > out->role.allocation_bytes ||
          out->role.buffer_bytes >
             out->role.allocation_bytes - out->role.binding_offset) {
         *reason = "a declared case reaches outside the declared role";
         return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
      }
      for (uint32_t j = i + 1; j < out->case_count; j++) {
         const struct r3v_measurement_case *b = &out->cases[j];
         if (a->case_id == b->case_id ||
             (a->fill_offset == b->fill_offset &&
              a->fill_bytes == b->fill_bytes &&
              a->fill_value == b->fill_value)) {
            *reason = "two declared cases carry one identity";
            return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
         }
      }
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

enum r3v_measurement_session_refusal
r3v_measurement_manifest_epoch_check(
   const struct r3v_measurement_manifest *manifest, uint32_t pci_vendor_id,
   uint32_t pci_device_id, const char *kernel_release,
   const char *module_srcversion, const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (manifest == NULL || kernel_release == NULL ||
       module_srcversion == NULL) {
      *reason = "the epoch check reads an absent declaration or fact";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   if (manifest->pci_vendor_id != pci_vendor_id ||
       manifest->pci_device_id != pci_device_id) {
      *reason = "the declaration names another device";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   if (strcmp(manifest->kernel_release, kernel_release) != 0 ||
       strcmp(manifest->module_srcversion, module_srcversion) != 0) {
      *reason = "the declaration names another deployment";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

enum r3v_measurement_session_refusal
r3v_measurement_session_open(
   struct r3v_measurement_session *session,
   const struct r3v_measurement_manifest *manifest,
   const char manifest_digest[R3V_FILL_ROUTE_DIGEST_HEX_SIZE],
   const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || manifest == NULL || manifest_digest == NULL ||
       manifest->case_count == 0 ||
       manifest->case_count > R3V_MEASUREMENT_SESSION_MAX_CASES) {
      *reason = "the session opens over an unreadable declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }

   /* What the cases themselves account for, summed in a width no case
    * count can overflow. */
   uint64_t case_total = 0;
   for (uint32_t i = 0; i < manifest->case_count; i++)
      case_total += (uint64_t)manifest->cases[i].warmups +
                    (uint64_t)manifest->cases[i].repetitions;
   if (case_total == 0 ||
       case_total > R3V_MEASUREMENT_SESSION_MAX_SUBMISSIONS ||
       manifest->max_total_submissions >
          R3V_MEASUREMENT_SESSION_MAX_SUBMISSIONS) {
      *reason = "the declared budget is empty or above the session bound";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }

   /* A declared total below what the cases account for cannot run the
    * campaign it declares, and would exhaust partway through with a
    * budget refusal that reads as a defect.  The declaration is refused
    * instead, which is the fail-closed direction and keeps the case rows
    * exact rather than advisory. */
   if (manifest->max_total_submissions < case_total) {
      *reason = "the declared total is below what the declared cases run";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }

   memset(session, 0, sizeof(*session));
   session->manifest = *manifest;
   memcpy(session->manifest_digest, manifest_digest,
          sizeof(session->manifest_digest));
   session->manifest_digest[sizeof(session->manifest_digest) - 1] = '\0';
   /* The cases account for the whole allowance, so a submission this
    * budget admits is a submission some case names. */
   session->remaining_submissions = (uint32_t)case_total;
   session->active = true;
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

const struct r3v_measurement_case *
r3v_measurement_session_find_case(
   const struct r3v_measurement_session *session, uint64_t fill_offset,
   uint64_t fill_bytes, uint32_t fill_value, uint32_t *index_out)
{
   if (session == NULL || !session->active || session->closed)
      return NULL;
   for (uint32_t i = 0; i < session->manifest.case_count; i++) {
      const struct r3v_measurement_case *c = &session->manifest.cases[i];
      if (c->fill_offset == fill_offset && c->fill_bytes == fill_bytes &&
          c->fill_value == fill_value) {
         if (index_out != NULL)
            *index_out = i;
         return c;
      }
   }
   return NULL;
}

enum r3v_measurement_session_refusal
r3v_measurement_session_route_check(
   const struct r3v_measurement_session *session, const char *route_name,
   const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || !session->active)
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   if (session->closed) {
      *reason = session->closed_reason;
      return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
   }
   if (route_name == NULL ||
       strcmp(session->manifest.route, route_name) != 0) {
      *reason = "the device resolved another executor for the request";
      return R3V_MEASUREMENT_SESSION_REFUSE_ROUTE_MISMATCH;
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

enum r3v_measurement_session_refusal
r3v_measurement_session_role_check(
   const struct r3v_measurement_session *session,
   const struct r3v_measurement_role *observed, const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || observed == NULL)
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   if (!session->active)
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   if (session->closed) {
      *reason = session->closed_reason;
      return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
   }

   const struct r3v_measurement_role *declared = &session->manifest.role;
   if (declared->allocation_bytes != observed->allocation_bytes ||
       declared->buffer_bytes != observed->buffer_bytes ||
       declared->binding_offset != observed->binding_offset ||
       declared->memory_property_flags != observed->memory_property_flags ||
       declared->buffer_usage != observed->buffer_usage ||
       declared->write_domain != observed->write_domain) {
      *reason = "the destination resolves outside the declared role";
      return R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH;
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

/* Lowercase hex of the digest width, the only shape an identity takes. */
static bool
digest_shaped(const char *digest)
{
   if (digest == NULL ||
       strlen(digest) != R3V_FILL_ROUTE_DIGEST_HEX_SIZE - 1)
      return false;
   for (const char *c = digest; *c != '\0'; c++) {
      if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f')))
         return false;
   }
   return true;
}

enum r3v_measurement_session_refusal
r3v_measurement_session_bind(
   struct r3v_measurement_session *session, uint32_t case_index,
   uint32_t destination_handle, uint64_t memory_generation,
   const char identity[R3V_FILL_ROUTE_DIGEST_HEX_SIZE], const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || !session->active)
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   if (session->closed) {
      *reason = session->closed_reason;
      return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
   }
   if (case_index >= session->manifest.case_count) {
      *reason = "the request names no declared case";
      return R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED;
   }
   if (!digest_shaped(identity)) {
      *reason = "the computed submission identity is not a digest";
      return R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH;
   }
   /* A generation of zero names no allocation, so a caller that reached
    * here without stamping one binds nothing. */
   if (memory_generation == 0) {
      *reason = "the destination carries no allocation generation";
      return R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND;
   }

   struct r3v_measurement_binding *binding = &session->bindings[case_index];
   if (!binding->bound) {
      binding->bound = true;
      binding->destination_handle = destination_handle;
      binding->memory_generation = memory_generation;
      memcpy(binding->identity, identity, sizeof(binding->identity));
      binding->identity[sizeof(binding->identity) - 1] = '\0';
      return R3V_MEASUREMENT_SESSION_ADMITTED;
   }

   /* The handle alone is a DRM-file index the kernel recycles, so the
    * generation decides whether the number still names the object the
    * case bound. */
   /* A bound case that resolves elsewhere, or whose stream no longer
    * hashes the same, contradicts what the campaign declared rather than
    * naming a request it declines.  Both terminate the session here: a
    * refusal alone would leave the binding standing and admit the next
    * repetition against it, which is the failure this check exists to
    * catch. */
   if (binding->destination_handle != destination_handle ||
       binding->memory_generation != memory_generation) {
      r3v_measurement_session_close(session,
                                    "the case resolved to another "
                                    "allocation");
      *reason = "the case is bound to another allocation";
      return R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND;
   }
   if (strcmp(binding->identity, identity) != 0) {
      r3v_measurement_session_close(
         session, "the recomputed submission identity left the binding");
      *reason = "the recomputed submission identity differs from the bound "
                "one";
      return R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH;
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

enum r3v_measurement_session_refusal
r3v_measurement_session_consume(struct r3v_measurement_session *session,
                                uint32_t case_index, const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || !session->active)
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   if (session->closed) {
      *reason = session->closed_reason;
      return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
   }
   if (case_index >= session->manifest.case_count) {
      *reason = "the request names no declared case";
      return R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED;
   }
   struct r3v_measurement_binding *binding = &session->bindings[case_index];
   if (!binding->bound) {
      *reason = "the case consumes an execution before binding its "
                "destination";
      return R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND;
   }

   /* The session allowance is the sum of the case allowances, so the
    * session counter reaches zero only when every case has, and the case
    * bound is the one a campaign actually meets.  The session bound is
    * kept as the second operand because it is what a future declaration
    * form with a smaller total would trip. */
   const struct r3v_measurement_case *declared =
      &session->manifest.cases[case_index];
   const uint32_t case_budget = declared->warmups + declared->repetitions;
   if (binding->executions_consumed >= case_budget ||
       session->remaining_submissions == 0) {
      *reason = "the case or the session has spent its declared executions";
      return R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED;
   }

   /* Consumed here, at the last point before the kernel boundary, and
    * never refunded: an attempt that entered the ioctl is an execution
    * whatever the ioctl returns. */
   binding->executions_consumed++;
   session->remaining_submissions--;
   session->consumed_submissions++;
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

void
r3v_measurement_session_close(struct r3v_measurement_session *session,
                              const char *why)
{
   if (session == NULL || !session->active || session->closed)
      return;
   session->closed = true;
   session->closed_reason = why != NULL ? why : "unnamed";
}
