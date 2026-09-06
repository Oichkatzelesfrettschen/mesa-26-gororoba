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
   [R3V_MEASUREMENT_SESSION_REFUSE_ALREADY_OPEN] = "session_already_open",
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
 * grammar has no octal: 0644 would name 420.  Every character after the
 * prefix is then held to the chosen base's alphabet before the
 * conversion runs, so a sign, a space, or any other character strtoull
 * would consume refuses instead: strtoull accepts a sign and negates
 * into the unsigned range, and it accepts one after a stripped prefix
 * too, so 0x-1 would name 2^64 - 1 on the strength of the prefix alone.
 * The alphabet admits every representable digit and no overflow, so
 * ERANGE still decides magnitude. */
static bool
parse_u64(const char *text, uint64_t *out)
{
   if (text == NULL || text[0] == '\0')
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
   for (const char *c = digits; *c != '\0'; c++) {
      const bool decimal = *c >= '0' && *c <= '9';
      const bool hex = base == 16 && ((*c >= 'a' && *c <= 'f') ||
                                      (*c >= 'A' && *c <= 'F'));
      if (!decimal && !hex)
         return false;
   }
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
   /* The row decodes here and means something in
    * manifest_structure_check: alignment, size, and execution count are
    * rules a case carries however it was assembled, so they hold a
    * manifest built in place exactly as they hold one read from text. */
   out->fill_offset = offset;
   out->fill_bytes = bytes;
   return true;
}

/* Lowercase hex of exactly the digest width, the only shape an identity
 * or a manifest digest takes.  The caller passes a fixed-width array, so
 * the scan walks that width and requires the terminator in its last
 * position rather than measuring a length that may not be there. */
static bool
digest_shaped(const struct r3v_measurement_digest *digest)
{
   if (digest == NULL ||
       digest->hex[R3V_FILL_ROUTE_DIGEST_HEX_SIZE - 1] != '\0')
      return false;
   for (size_t i = 0; i < R3V_FILL_ROUTE_DIGEST_HEX_SIZE - 1; i++) {
      const char c = digest->hex[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   return true;
}

/* A declared name is nonempty and terminates inside its own array.  The
 * width is passed rather than measured, so the scan stops at the field's
 * end even when no terminator is present. */
static bool
name_terminated(const char *field, size_t size)
{
   const char *nul = memchr(field, '\0', size);
   return nul != NULL && nul != field;
}

/* Every rule a declaration carries, independent of the text it came
 * from.  The reader calls it after decoding, and the session calls it
 * again on open, so a manifest assembled by any route -- parsed, built
 * in place, or edited after parsing -- passes one set of rules.  Parsing
 * supplies syntactic decoding alone.
 */
static enum r3v_measurement_session_refusal
manifest_structure_check(const struct r3v_measurement_manifest *m,
                         const char **reason)
{
   if (m == NULL) {
      *reason = "the declaration is unreadable";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   /* strcmp reads the schema below and the epoch check reads the release
    * and srcversion, so termination is established before the first read
    * rather than after it. */
   if (!name_terminated(m->schema, sizeof(m->schema)) ||
       !name_terminated(m->session_nonce, sizeof(m->session_nonce)) ||
       !name_terminated(m->platform, sizeof(m->platform)) ||
       !name_terminated(m->route, sizeof(m->route)) ||
       !name_terminated(m->kernel_release, sizeof(m->kernel_release)) ||
       !name_terminated(m->module_srcversion,
                        sizeof(m->module_srcversion))) {
      *reason = "a declared name is empty or runs past its field";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   if (strcmp(m->schema, R3V_MEASUREMENT_SESSION_SCHEMA) != 0) {
      *reason = "the declaration names another schema";
      return R3V_MEASUREMENT_SESSION_REFUSE_SCHEMA;
   }
   if (m->case_count == 0 ||
       m->case_count > R3V_MEASUREMENT_SESSION_MAX_CASES) {
      *reason = "the declaration carries no case, or more than the bound";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   /* The buffer's fit in its allocation is one fact about the role, so
    * it is decided once and names itself rather than reporting as a
    * property of whichever case the loop reached first. */
   if (m->role.binding_offset > m->role.allocation_bytes ||
       m->role.buffer_bytes >
          m->role.allocation_bytes - m->role.binding_offset) {
      *reason = "the declared buffer reaches outside its allocation";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }

   for (uint32_t i = 0; i < m->case_count; i++) {
      const struct r3v_measurement_case *a = &m->cases[i];
      /* vkCmdFillBuffer counts whole dwords from a dword boundary, and a
       * case that runs nothing declares nothing.  The count sums in 64
       * bits, so a pair like {0xffffffff, 1} is read as the four billion
       * executions it declares rather than wrapping to zero and refusing
       * here as empty; the session ceiling below refuses it for its
       * size, which is what is actually wrong with it. */
      if (a->fill_bytes == 0 ||
          a->fill_bytes % R3V_FILL_ROUTE_ELEMENT_BYTES != 0 ||
          a->fill_offset % R3V_FILL_ROUTE_ELEMENT_BYTES != 0) {
         *reason = "a declared case fills nothing, or off a dword boundary";
         return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
      }
      if ((uint64_t)a->warmups + (uint64_t)a->repetitions == 0) {
         *reason = "a declared case runs no warmup and no repetition";
         return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
      }
      if (a->fill_offset > m->role.buffer_bytes ||
          a->fill_bytes > m->role.buffer_bytes - a->fill_offset) {
         *reason = "a declared case reaches outside the declared buffer";
         return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
      }
      /* Two cases agreeing on offset, size, and value are the same case,
       * and a request matching both would consume an ambiguous budget. */
      for (uint32_t j = i + 1; j < m->case_count; j++) {
         const struct r3v_measurement_case *b = &m->cases[j];
         if (a->case_id == b->case_id ||
             (a->fill_offset == b->fill_offset &&
              a->fill_bytes == b->fill_bytes &&
              a->fill_value == b->fill_value)) {
            *reason = "two declared cases carry one identity";
            return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
         }
      }
   }

   /* What the cases themselves account for, summed in a width no case
    * count can overflow. */
   uint64_t case_total = 0;
   for (uint32_t i = 0; i < m->case_count; i++)
      case_total += (uint64_t)m->cases[i].warmups +
                    (uint64_t)m->cases[i].repetitions;
   if (case_total > R3V_MEASUREMENT_SESSION_MAX_SUBMISSIONS ||
       m->max_total_submissions > R3V_MEASUREMENT_SESSION_MAX_SUBMISSIONS) {
      *reason = "the declared budget is above the session bound";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   /* A declared total below what the cases account for cannot run the
    * campaign it declares, and would exhaust partway through with a
    * budget refusal that reads as a defect.  The declaration is refused
    * instead, which is the fail-closed direction and keeps the case rows
    * exact rather than advisory. */
   if (m->max_total_submissions < case_total) {
      *reason = "the declared total is below what the declared cases run";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   /* A zero timeout waits for nothing and would report every completion
    * as late; the ceiling keeps the declared interval inside what the
    * wait interface represents, which reads an absolute deadline at or
    * above INT64_MAX as unbounded. */
   if (m->completion_timeout_ns == 0 ||
       m->completion_timeout_ns > R3V_MEASUREMENT_SESSION_MAX_TIMEOUT_NS) {
      *reason = "the declared completion timeout is zero or above the bound";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
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
   /* The digest covers the whole byte range while the field reader stops
    * a value at its first terminator, so an embedded NUL would leave the
    * hashed bytes and the read declaration naming different campaigns.
    * The length is known here and nowhere below it. */
   if (length > 0 && memchr(text, '\0', length) != NULL) {
      *reason = "the declaration carries a terminator inside its text";
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
      uint32_t bit = 0;
      if (strcmp(key, "schema") == 0) {
         ok = copy_name(out->schema, sizeof(out->schema), value);
         bit = SEEN_SCHEMA;
      } else if (strcmp(key, "session_nonce") == 0) {
         ok = copy_name(out->session_nonce, sizeof(out->session_nonce), value);
         bit = SEEN_NONCE;
      } else if (strcmp(key, "platform") == 0) {
         ok = copy_name(out->platform, sizeof(out->platform), value);
         bit = SEEN_PLATFORM;
      } else if (strcmp(key, "route") == 0) {
         ok = copy_name(out->route, sizeof(out->route), value);
         bit = SEEN_ROUTE;
      } else if (strcmp(key, "pci_vendor_id") == 0) {
         ok = parse_u32(value, &out->pci_vendor_id);
         bit = SEEN_VENDOR;
      } else if (strcmp(key, "pci_device_id") == 0) {
         ok = parse_u32(value, &out->pci_device_id);
         bit = SEEN_DEVICE;
      } else if (strcmp(key, "kernel_release") == 0) {
         ok = copy_name(out->kernel_release, sizeof(out->kernel_release),
                        value);
         bit = SEEN_KERNEL;
      } else if (strcmp(key, "module_srcversion") == 0) {
         ok = copy_name(out->module_srcversion,
                        sizeof(out->module_srcversion), value);
         bit = SEEN_MODULE;
      } else if (strcmp(key, "allocation_bytes") == 0) {
         ok = parse_u64(value, &out->role.allocation_bytes);
         bit = SEEN_ALLOCATION;
      } else if (strcmp(key, "buffer_bytes") == 0) {
         ok = parse_u64(value, &out->role.buffer_bytes);
         bit = SEEN_BUFFER;
      } else if (strcmp(key, "binding_offset") == 0) {
         ok = parse_u64(value, &out->role.binding_offset);
         bit = SEEN_BINDING;
      } else if (strcmp(key, "memory_property_flags") == 0) {
         ok = parse_u32(value, &out->role.memory_property_flags);
         bit = SEEN_MEMORY_FLAGS;
      } else if (strcmp(key, "buffer_usage") == 0) {
         ok = parse_u32(value, &out->role.buffer_usage);
         bit = SEEN_USAGE;
      } else if (strcmp(key, "write_domain") == 0) {
         ok = parse_u32(value, &out->role.write_domain);
         bit = SEEN_WRITE_DOMAIN;
      } else if (strcmp(key, "max_total_submissions") == 0) {
         ok = parse_u32(value, &out->max_total_submissions);
         bit = SEEN_MAX_SUBMISSIONS;
      } else if (strcmp(key, "completion_timeout_ns") == 0) {
         ok = parse_u64(value, &out->completion_timeout_ns);
         bit = SEEN_TIMEOUT;
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
      /* A scalar key declares one value.  Two lines carrying the same key
       * leave the declaration's meaning to the reader's order, so the
       * second one refuses rather than overwriting the first. */
      if (bit != 0) {
         if ((seen & bit) != 0) {
            *reason = "the declaration carries one key twice";
            return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
         }
         seen |= bit;
      }
   }
   if (malformed) {
      *reason = "a declaration line carries no key and value";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   if (seen != SEEN_ALL) {
      *reason = "the declaration omits a required field";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }
   return manifest_structure_check(out, reason);
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
   /* The documented wiring order runs this check before the session
    * opens, so it is the first thing to read the declaration's names and
    * it establishes their termination itself rather than inheriting a
    * rule open has not run yet. */
   if (!name_terminated(manifest->kernel_release,
                        sizeof(manifest->kernel_release)) ||
       !name_terminated(manifest->module_srcversion,
                        sizeof(manifest->module_srcversion))) {
      *reason = "the declaration's deployment names run past their fields";
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
r3v_measurement_manifest_platform_check(
   const struct r3v_measurement_manifest *manifest,
   enum r300_platform_id resolved, const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (manifest == NULL) {
      *reason = "the platform check reads an absent declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   if (!name_terminated(manifest->platform, sizeof(manifest->platform))) {
      *reason = "the declaration's platform name runs past its field";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   /* Three facts, three refusals.  Two unresolved names are not a match:
    * an operator's typo and a board the tables do not carry are separate
    * defects, and NONE == NONE would admit both. */
   const enum r300_platform_id declared =
      r300_platform_id_from_declaration_name(manifest->platform);
   if (declared == R300_PLATFORM_ID_NONE) {
      *reason = "the declaration names a platform no board row carries";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   if (resolved == R300_PLATFORM_ID_NONE) {
      *reason = "the running board resolved to no qualified platform";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   if (declared != resolved) {
      *reason = "the declaration names another board than the one running";
      return R3V_MEASUREMENT_SESSION_REFUSE_EPOCH;
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

void
r3v_measurement_session_init(struct r3v_measurement_session *session)
{
   if (session != NULL)
      memset(session, 0, sizeof(*session));
}

enum r3v_measurement_session_refusal
r3v_measurement_session_open(
   struct r3v_measurement_session *session,
   const struct r3v_measurement_manifest *manifest,
   const struct r3v_measurement_digest *manifest_digest,
   const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || manifest == NULL || manifest_digest == NULL) {
      *reason = "the session opens over an unreadable declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }

   /* A second open over a live session would clear its bindings and
    * restore the allowance the first one spent, which is the budget
    * bypass this predicate exists to refuse.  A closed session stays
    * closed for the life of the process; the declaration names one
    * campaign and the campaign runs once. */
   if (session->active) {
      if (session->closed) {
         *reason = session->closed_reason;
         return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
      }
      *reason = "the session is already open over a declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_ALREADY_OPEN;
   }

   /* The declaration is held to every structural rule again here.  The
    * reader enforces them on the text it parses, and a manifest that
    * reached this call by another route -- assembled in place, or edited
    * after parsing -- meets the same rules before it opens anything. */
   enum r3v_measurement_session_refusal structure =
      manifest_structure_check(manifest, reason);
   if (structure != R3V_MEASUREMENT_SESSION_ADMITTED)
      return structure;

   uint64_t case_total = 0;
   for (uint32_t i = 0; i < manifest->case_count; i++)
      case_total += (uint64_t)manifest->cases[i].warmups +
                    (uint64_t)manifest->cases[i].repetitions;

   /* The digest is stored whole and compared whole later, so its shape is
    * established before the copy rather than trusted from the caller. */
   if (!digest_shaped(manifest_digest)) {
      *reason = "the declaration digest is not lowercase hex of its width";
      return R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED;
   }

   memset(session, 0, sizeof(*session));
   session->manifest = *manifest;
   session->manifest_digest = *manifest_digest;
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
   if (session == NULL || !session->active) {
      *reason = "no session stands over a declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   }
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
   if (session == NULL || !session->active) {
      *reason = "no session stands over a declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   }
   /* A terminated campaign answers every later question with the reason
    * it terminated, so the closed check stands ahead of anything that
    * reads the request. */
   if (session->closed) {
      *reason = session->closed_reason;
      return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
   }
   /* An absent observation is a role the caller never resolved, which is
    * a mismatch with the declared one rather than an absent session. */
   if (observed == NULL) {
      *reason = "the destination resolved to no observable role";
      return R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH;
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

/* The operation a case declares: offset, size, and value together.  The
 * three are the case's identity in find_case, so holding a request to
 * them keeps the index and the operation naming one row. */
static bool
case_matches_operation(const struct r3v_measurement_case *declared,
                       uint64_t fill_offset, uint64_t fill_bytes,
                       uint32_t fill_value)
{
   return declared->fill_offset == fill_offset &&
          declared->fill_bytes == fill_bytes &&
          declared->fill_value == fill_value;
}

enum r3v_measurement_session_refusal
r3v_measurement_session_bind(
   struct r3v_measurement_session *session, uint32_t case_index,
   uint64_t fill_offset, uint64_t fill_bytes, uint32_t fill_value,
   uint32_t destination_handle, uint64_t memory_generation,
   const struct r3v_measurement_digest *identity, const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || !session->active) {
      *reason = "no session stands over a declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   }
   if (session->closed) {
      *reason = session->closed_reason;
      return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
   }
   if (case_index >= session->manifest.case_count) {
      *reason = "the request names no declared case";
      return R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED;
   }
   /* The index selects a row; the operation decides whether that row is
    * the one this request runs.  A request that names one case and fills
    * another is undeclared, whatever its digest hashes to. */
   if (!case_matches_operation(&session->manifest.cases[case_index],
                               fill_offset, fill_bytes, fill_value)) {
      *reason = "the request fills something the named case does not "
                "declare";
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
      binding->identity = *identity;
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
   if (strcmp(binding->identity.hex, identity->hex) != 0) {
      r3v_measurement_session_close(
         session, "the recomputed submission identity left the binding");
      *reason = "the recomputed submission identity differs from the bound "
                "one";
      return R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH;
   }
   return R3V_MEASUREMENT_SESSION_ADMITTED;
}

enum r3v_measurement_session_refusal
r3v_measurement_session_consume(
   struct r3v_measurement_session *session, uint32_t case_index,
   uint64_t fill_offset, uint64_t fill_bytes, uint32_t fill_value,
   uint32_t destination_handle, uint64_t memory_generation,
   const struct r3v_measurement_digest *identity, const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   if (session == NULL || !session->active) {
      *reason = "no session stands over a declaration";
      return R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE;
   }
   if (session->closed) {
      *reason = session->closed_reason;
      return R3V_MEASUREMENT_SESSION_REFUSE_CLOSED;
   }
   if (case_index >= session->manifest.case_count) {
      *reason = "the request names no declared case";
      return R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED;
   }
   if (!case_matches_operation(&session->manifest.cases[case_index],
                               fill_offset, fill_bytes, fill_value)) {
      *reason = "the request fills something the named case does not "
                "declare";
      return R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED;
   }
   struct r3v_measurement_binding *binding = &session->bindings[case_index];
   if (!binding->bound) {
      *reason = "the case consumes an execution before binding its "
                "destination";
      return R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND;
   }
   /* The bind authorized one object and one stream; this call spends the
    * execution that submission runs, so it names them again and they are
    * held against the binding.  Splitting the two would let a bind
    * against one allocation stand while a consume and a submission ran
    * against another the role also admits, and the binding alone cannot
    * see that substitution.  Both mismatches terminate the campaign
    * rather than refusing one request, because a bound case whose
    * submission resolves elsewhere contradicts the declaration. */
   if (!digest_shaped(identity)) {
      *reason = "the consumed submission carries no identity";
      return R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH;
   }
   if (binding->destination_handle != destination_handle ||
       binding->memory_generation != memory_generation) {
      r3v_measurement_session_close(
         session, "the consumed submission resolved to another allocation");
      *reason = "the consumed submission names another allocation than the "
                "bound one";
      return R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND;
   }
   if (strcmp(binding->identity.hex, identity->hex) != 0) {
      r3v_measurement_session_close(
         session, "the consumed submission left the bound identity");
      *reason = "the consumed submission differs from the bound one";
      return R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH;
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
   const char *text = why != NULL ? why : "unnamed";
   size_t room = sizeof(session->closed_reason) - 1;
   size_t length = strlen(text);
   if (length > room)
      length = room;
   memcpy(session->closed_reason, text, length);
   session->closed_reason[length] = '\0';
}
