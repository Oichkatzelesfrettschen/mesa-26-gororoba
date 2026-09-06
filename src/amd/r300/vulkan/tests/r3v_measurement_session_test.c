/*
 * SPDX-License-Identifier: MIT
 *
 * Calibrates the bounded measurement session against the arm it admits
 * and every arm it refuses, so a refusal in the driver names a rule this
 * file already exercised.  It reads no device: the session is a pure
 * predicate over a declaration, an observed role, and a computed
 * identity.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG
#include <assert.h>

#include "r3v_measurement_session.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define DIGEST_A                                                              \
   "0000000000000000000000000000000000000000000000000000000000000001"
#define DIGEST_B                                                              \
   "0000000000000000000000000000000000000000000000000000000000000002"
/* The digest parameters are fixed-width arrays the predicate scans whole,
 * so a malformed identity is that width carrying a character outside the
 * alphabet -- a short literal would be a different defect. */
#define DIGEST_NOT_HEX                                                        \
   "00000000000000000000000000000000000000000000000000000000deadbeeZ"

static const char valid_manifest[] =
   "# the windowed route over two sizes on the specimen\n"
   "schema = r3v-measurement-session-v1\n"
   "session_nonce = 7f3a19c2\n"
   "platform = vostro1000_rs485m_5974\n"
   "route = rb2d_const_fill_v2\n"
   "pci_vendor_id = 0x1002\n"
   "pci_device_id = 0x5974\n"
   "kernel_release = 7.1.8-1-cachyos\n"
   "module_srcversion = 729892A3F3530EB12B8D842\n"
   "allocation_bytes = 8388608\n"
   "buffer_bytes = 8388608\n"
   "binding_offset = 0\n"
   "memory_property_flags = 0x2\n"
   "buffer_usage = 0x2\n"
   "write_domain = 0x2\n"
   "max_total_submissions = 72\n"
   "completion_timeout_ns = 10000000000\n"
   "case = 1, 0, 4096, 287454020, 2, 32\n"
   "case = 2, 0, 65536, 287454020, 2, 32\n";

/* The two rows valid_manifest declares, as a call site names them: the
 * index the request cites and the operation it fills.  The predicate
 * holds the two against each other, so a test that passes one without
 * the other is not testing what the driver will call. */
#define CASE_4K_OPERATION 0u, 4096u, 287454020u
#define CASE_64K_OPERATION 0u, 65536u, 287454020u
#define CASE_4K 0u, CASE_4K_OPERATION
#define CASE_64K 1u, CASE_64K_OPERATION
/* The object and stream a case binds, as bind records them and consume
 * names them again.  A consume that carried the operation alone would let
 * one allocation's authorization pay for another's delivery. */
#define BOUND_A 7u, 101u, &digest_a
#define BOUND_B 8u, 102u, &digest_b

/* The declaration and identity digests as the predicate takes them: an
 * object of the digest width, not a pointer into a shorter one. */
static const struct r3v_measurement_digest digest_a = { DIGEST_A };
static const struct r3v_measurement_digest digest_b = { DIGEST_B };
static const struct r3v_measurement_digest digest_not_hex = { DIGEST_NOT_HEX };

static struct r3v_measurement_digest
digest_of(const char *hex)
{
   struct r3v_measurement_digest d;
   memset(&d, 0, sizeof(d));
   const size_t len = strlen(hex);
   assert(len < sizeof(d.hex));
   memcpy(d.hex, hex, len);
   return d;
}

static struct r3v_measurement_manifest
parse_valid(void)
{
   struct r3v_measurement_manifest manifest;
   const char *reason = NULL;
   assert(r3v_measurement_manifest_parse(valid_manifest,
                                         sizeof(valid_manifest) - 1,
                                         &manifest, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(reason == NULL);
   return manifest;
}

static struct r3v_measurement_role
declared_role(const struct r3v_measurement_manifest *manifest)
{
   return manifest->role;
}

static void
test_manifest_reads_every_field(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   assert(strcmp(m.schema, R3V_MEASUREMENT_SESSION_SCHEMA) == 0);
   assert(strcmp(m.platform, "vostro1000_rs485m_5974") == 0);
   assert(strcmp(m.route, "rb2d_const_fill_v2") == 0);
   assert(m.pci_vendor_id == 0x1002u && m.pci_device_id == 0x5974u);
   assert(strcmp(m.kernel_release, "7.1.8-1-cachyos") == 0);
   assert(m.role.allocation_bytes == 8388608u);
   assert(m.role.buffer_bytes == 8388608u);
   assert(m.role.memory_property_flags == 0x2u);
   assert(m.role.buffer_usage == 0x2u);
   assert(m.case_count == 2u);
   assert(m.cases[0].case_id == 1u && m.cases[0].fill_bytes == 4096u);
   assert(m.cases[0].fill_value == 287454020u);
   assert(m.cases[0].warmups == 2u && m.cases[0].repetitions == 32u);
   assert(m.cases[1].fill_bytes == 65536u);
   assert(m.max_total_submissions == 72u);
   assert(m.completion_timeout_ns == 10000000000ull);
}

/* One replacement in the declaration text, so each refusal is isolated
 * to the field it names.  The reason travels back out, because most
 * semantic rules share one refusal code and the reason is what tells
 * them apart. */
static enum r3v_measurement_session_refusal
parse_edited_text(const char *find, const char *replace,
                  const char **reason_out)
{
   char text[sizeof(valid_manifest) + 256];
   const char *at = strstr(valid_manifest, find);
   assert(at != NULL);
   const size_t head = (size_t)(at - valid_manifest);
   memcpy(text, valid_manifest, head);
   const size_t replace_len = strlen(replace);
   memcpy(text + head, replace, replace_len);
   const char *tail = at + strlen(find);
   const size_t tail_len = strlen(tail);
   assert(head + replace_len + tail_len < sizeof(text));
   memcpy(text + head + replace_len, tail, tail_len + 1);

   struct r3v_measurement_manifest manifest;
   return r3v_measurement_manifest_parse(
      text, head + replace_len + tail_len, &manifest, reason_out);
}

static void
refuse_manifest(const char *find, const char *replace,
                enum r3v_measurement_session_refusal expected)
{
   const char *reason = NULL;
   const enum r3v_measurement_session_refusal r =
      parse_edited_text(find, replace, &reason);
   if (r != expected)
      fprintf(stderr, "arm %s -> %s : %s (%s)\n", replace,
              r3v_measurement_session_refusal_name(r),
              reason != NULL ? reason : "(none)",
              r3v_measurement_session_refusal_name(expected));
   assert(r == expected);
   assert((r == R3V_MEASUREMENT_SESSION_ADMITTED) == (reason == NULL));
}

static void
test_manifest_refusals(void)
{
   const char *reason = NULL;
   struct r3v_measurement_manifest manifest;

   assert(r3v_measurement_manifest_parse(NULL, 0, &manifest, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   assert(r3v_measurement_manifest_parse(valid_manifest,
                                         sizeof(valid_manifest) - 1, NULL,
                                         &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);

   /* A declaration for another reader is not a weaker declaration. */
   refuse_manifest("schema = r3v-measurement-session-v1",
                   "schema = r3v-measurement-session-v2",
                   R3V_MEASUREMENT_SESSION_REFUSE_SCHEMA);
   /* An omitted required field refuses rather than defaulting to zero. */
   refuse_manifest("write_domain = 0x2\n", "",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("platform = vostro1000_rs485m_5974\n", "",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A scalar key declares one value.  A second line carrying it leaves
    * the meaning to the reader's order, so it refuses. */
   refuse_manifest("write_domain = 0x2\n",
                   "write_domain = 0x2\nwrite_domain = 0x4\n",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("route = rb2d_const_fill_v2\n",
                   "route = rb2d_const_fill_v2\nroute = rb2d_const_fill\n",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A repeated `case` line is a further case, not a repeated key. */
   assert(r3v_measurement_manifest_parse(valid_manifest,
                                         sizeof(valid_manifest) - 1,
                                         &manifest, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(manifest.case_count == 2u);
   /* A key this reader does not read is a declaration it cannot honor. */
   refuse_manifest("write_domain = 0x2", "write_domains = 0x2",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A line with no key and value. */
   refuse_manifest("write_domain = 0x2", "write_domain",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* Six fields, every one required, and no seventh. */
   refuse_manifest("case = 1, 0, 4096, 287454020, 2, 32",
                   "case = 1, 0, 4096, 287454020, 2",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("case = 1, 0, 4096, 287454020, 2, 32",
                   "case = 1, 0, 4096, 287454020, 2, 32, 9",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("case = 1, 0, 4096, 287454020, 2, 32",
                   "case = 1, 0, 4096, 287454020, 2, 32,",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A range that counts no whole dword on a dword boundary. */
   refuse_manifest("case = 1, 0, 4096, 287454020, 2, 32",
                   "case = 1, 0, 4094, 287454020, 2, 32",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("case = 1, 0, 4096, 287454020, 2, 32",
                   "case = 1, 2, 4096, 287454020, 2, 32",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A case that runs nothing declares nothing. */
   refuse_manifest("case = 1, 0, 4096, 287454020, 2, 32",
                   "case = 1, 0, 4096, 287454020, 0, 0",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* Two cases carrying one identity: a request would match both. */
   refuse_manifest("case = 2, 0, 65536, 287454020, 2, 32",
                   "case = 2, 0, 4096, 287454020, 2, 32",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("case = 2, 0, 65536, 287454020, 2, 32",
                   "case = 1, 0, 65536, 287454020, 2, 32",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A case reaching past the declared buffer. */
   refuse_manifest("case = 2, 0, 65536, 287454020, 2, 32",
                   "case = 2, 8388608, 65536, 287454020, 2, 32",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A value outside its field. */
   refuse_manifest("pci_vendor_id = 0x1002", "pci_vendor_id = 0x100000000",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("pci_vendor_id = 0x1002", "pci_vendor_id = 0x1002x",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* A sign wraps into the unsigned range under strtoull, so a negative
    * declaration would name a huge value rather than refuse. */
   refuse_manifest("allocation_bytes = 8388608", "allocation_bytes = -1",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("max_total_submissions = 72",
                   "max_total_submissions = +72",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* The grammar is decimal or 0x hex.  strtoull's base zero would read
    * a leading zero as octal, so 0100 would name 64 rather than 100. */
   refuse_manifest("max_total_submissions = 72",
                   "max_total_submissions = 072",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("pci_vendor_id = 0x1002", "pci_vendor_id = 0x",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* The sign guard on the original string does not reach past a
    * stripped prefix.  strtoull reads a sign after 0x as readily as
    * before it, so 0x-1 would name 2^64 - 1 on the strength of the
    * prefix alone; the alphabet check after base selection is what
    * refuses these. */
   refuse_manifest("allocation_bytes = 8388608", "allocation_bytes = 0x-1",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("allocation_bytes = 8388608", "allocation_bytes = 0x+1",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   refuse_manifest("allocation_bytes = 8388608", "allocation_bytes = 0x 1",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* Hex digits are hex digits in both cases, and a decimal field reads
    * no hex letter. */
   refuse_manifest("memory_property_flags = 0x2",
                   "memory_property_flags = 0xa",
                   R3V_MEASUREMENT_SESSION_ADMITTED);
   refuse_manifest("memory_property_flags = 0x2",
                   "memory_property_flags = 0xA",
                   R3V_MEASUREMENT_SESSION_ADMITTED);
   refuse_manifest("max_total_submissions = 72",
                   "max_total_submissions = 7a",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* The alphabet admits every representable digit, so magnitude is
    * still decided by the conversion. */
   refuse_manifest("allocation_bytes = 8388608",
                   "allocation_bytes = 99999999999999999999999",
                   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   /* The digest covers the whole byte range while a field stops at its
    * first terminator, so a NUL inside the text would leave the hashed
    * bytes and the read declaration naming different campaigns. */
   {
      char text[sizeof(valid_manifest) + 8];
      memcpy(text, valid_manifest, sizeof(valid_manifest) - 1);
      const size_t length = sizeof(valid_manifest) - 1;
      text[length - 1] = '\0';
      struct r3v_measurement_manifest embedded;
      assert(r3v_measurement_manifest_parse(text, length, &embedded,
                                            &reason) ==
             R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
      assert(reason != NULL);
   }
   /* A bare zero is still a zero, and hex still reads as hex. */
   refuse_manifest("binding_offset = 0", "binding_offset = 0x0",
                   R3V_MEASUREMENT_SESSION_ADMITTED);
}

static void
test_epoch(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   const char *reason = NULL;
   assert(r3v_measurement_manifest_epoch_check(
             &m, 0x1002u, 0x5974u, "7.1.8-1-cachyos",
             "729892A3F3530EB12B8D842", &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* Each epoch field alone refuses: another device, another kernel,
    * another module build. */
   assert(r3v_measurement_manifest_epoch_check(
             &m, 0x1002u, 0x5975u, "7.1.8-1-cachyos",
             "729892A3F3530EB12B8D842", &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_EPOCH);
   assert(r3v_measurement_manifest_epoch_check(
             &m, 0x1002u, 0x5974u, "7.1.9-1-cachyos",
             "729892A3F3530EB12B8D842", &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_EPOCH);
   assert(r3v_measurement_manifest_epoch_check(&m, 0x1002u, 0x5974u,
                                               "7.1.8-1-cachyos", "0000",
                                               &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_EPOCH);
   assert(r3v_measurement_manifest_epoch_check(&m, 0x1002u, 0x5974u, NULL,
                                               "729892A3F3530EB12B8D842",
                                               &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_EPOCH);
}

/* The documented wiring order runs the epoch check before the session
 * opens, so it is the first thing to read the declaration's names and it
 * establishes their termination itself.  The reason is asserted because a
 * strcmp over an unterminated field would also report a mismatch, and the
 * two are the same verdict for different facts. */
static void
test_epoch_reads_only_terminated_names(void)
{
   const char *reason = NULL;
   static const size_t offsets[] = {
      offsetof(struct r3v_measurement_manifest, kernel_release),
      offsetof(struct r3v_measurement_manifest, module_srcversion),
   };
   static const size_t widths[] = {
      sizeof(((struct r3v_measurement_manifest *)0)->kernel_release),
      sizeof(((struct r3v_measurement_manifest *)0)->module_srcversion),
   };
   for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
      struct r3v_measurement_manifest edited = parse_valid();
      memset((char *)&edited + offsets[i], 'a', widths[i]);
      reason = NULL;
      assert(r3v_measurement_manifest_epoch_check(
                &edited, 0x1002u, 0x5974u, "7.1.8-1-cachyos",
                "729892A3F3530EB12B8D842", &reason) ==
             R3V_MEASUREMENT_SESSION_REFUSE_EPOCH);
      assert(reason != NULL);
      assert(strcmp(reason,
                    "the declaration's deployment names run past their "
                    "fields") == 0);
   }
}

static void
edit_schema(struct r3v_measurement_manifest *m)
{
   strcpy(m->schema, "r3v-measurement-session-v2");
}

static void
edit_duplicate_case(struct r3v_measurement_manifest *m)
{
   m->cases[1] = m->cases[0];
   m->cases[1].case_id = 9u;
}

static void
edit_case_past_buffer(struct r3v_measurement_manifest *m)
{
   m->cases[0].fill_offset = m->role.buffer_bytes;
   m->cases[0].fill_bytes = 4096u;
}

static void
edit_buffer_past_allocation(struct r3v_measurement_manifest *m)
{
   m->role.binding_offset = 4096u;
}

static void
edit_no_cases(struct r3v_measurement_manifest *m)
{
   m->case_count = 0u;
}

static void
test_open_budget(void)
{
   struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   r3v_measurement_session_init(&session);
   const char *reason = NULL;

   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.active && !session.closed);
   /* Two cases at two warmups plus thirty-two repetitions each. */
   assert(session.remaining_submissions == 68u);
   assert(session.consumed_submissions == 0u);

   /* A second open over a live session would clear its bindings and
    * hand back the allowance the first one spent.  It names that fact
    * rather than reporting a declaration it read without complaint. */
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ALREADY_OPEN);
   assert(reason != NULL);
   assert(session.remaining_submissions == 68u);

   /* A closed session stays closed, and names why. */
   struct r3v_measurement_session spent;
   r3v_measurement_session_init(&spent);
   assert(r3v_measurement_session_open(&spent, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   r3v_measurement_session_close(&spent, "the oracle read a wrong byte");
   assert(r3v_measurement_session_open(&spent, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);
   assert(strcmp(reason, "the oracle read a wrong byte") == 0);

   /* A declared total below what the cases run cannot run the campaign
    * it declares. */
   struct r3v_measurement_session underfunded;
   memset(&underfunded, 0, sizeof(underfunded));
   m.max_total_submissions = 67u;
   assert(r3v_measurement_session_open(&underfunded, &m, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   m.max_total_submissions = 68u;
   assert(r3v_measurement_session_open(&underfunded, &m, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(underfunded.remaining_submissions == 68u);

   /* Every structural rule the reader enforces on text, the session
    * enforces again on the struct: a manifest edited after parsing, or
    * assembled without the reader, opens nothing the reader would have
    * refused. */
   static const struct {
      const char *what;
      void (*damage)(struct r3v_measurement_manifest *);
      enum r3v_measurement_session_refusal expected;
   } edits[] = {
      { "another schema",
        edit_schema, R3V_MEASUREMENT_SESSION_REFUSE_SCHEMA },
      { "two cases at one identity",
        edit_duplicate_case,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED },
      { "a case outside its buffer",
        edit_case_past_buffer,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED },
      { "a buffer outside its allocation",
        edit_buffer_past_allocation,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED },
      { "no case at all",
        edit_no_cases, R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED },
   };
   for (size_t i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
      struct r3v_measurement_manifest edited = parse_valid();
      struct r3v_measurement_session refused;
      memset(&refused, 0, sizeof(refused));
      edits[i].damage(&edited);
      reason = NULL;
      assert(r3v_measurement_session_open(&refused, &edited, &digest_a,
                                          &reason) == edits[i].expected);
      assert(reason != NULL);
      assert(!refused.active);
   }
}

static void
edit_case_size_unaligned(struct r3v_measurement_manifest *m)
{
   m->cases[0].fill_bytes = 4094u;
}

static void
edit_case_offset_unaligned(struct r3v_measurement_manifest *m)
{
   m->cases[0].fill_offset = 2u;
}

static void
edit_case_runs_nothing(struct r3v_measurement_manifest *m)
{
   m->cases[0].warmups = 0u;
   m->cases[0].repetitions = 0u;
}

/* A count that wraps to zero in 32 bits and does not in 64.  The case
 * declares four billion executions, which is a size defect, not an empty
 * one, so the reason it refuses under names its actual fault. */
static void
edit_case_count_wraps_in_32_bits(struct r3v_measurement_manifest *m)
{
   m->cases[0].warmups = 0xffffffffu;
   m->cases[0].repetitions = 1u;
}

static void
edit_total_below_cases(struct r3v_measurement_manifest *m)
{
   m->max_total_submissions = 67u;
}

static void
edit_budget_above_bound(struct r3v_measurement_manifest *m)
{
   m->max_total_submissions = R3V_MEASUREMENT_SESSION_MAX_SUBMISSIONS + 1u;
}

static void
edit_timeout_zero(struct r3v_measurement_manifest *m)
{
   m->completion_timeout_ns = 0u;
}

static void
edit_timeout_above_bound(struct r3v_measurement_manifest *m)
{
   m->completion_timeout_ns = R3V_MEASUREMENT_SESSION_MAX_TIMEOUT_NS + 1u;
}

/* Every rule a declaration carries, reached from the text a reader
 * decodes and from a manifest struct handed straight to the session.
 * Both arms are required to name the same reason: most of these rules
 * share one refusal code, so an arm that only compared the code would
 * pass while the two paths enforced different things.
 */
static void
test_semantic_rules_hold_on_both_paths(void)
{
   static const struct {
      const char *find;
      const char *replace;
      void (*damage)(struct r3v_measurement_manifest *);
      enum r3v_measurement_session_refusal expected;
      const char *reason;
   } rules[] = {
      { "schema = r3v-measurement-session-v1",
        "schema = r3v-measurement-session-v2", edit_schema,
        R3V_MEASUREMENT_SESSION_REFUSE_SCHEMA,
        "the declaration names another schema" },
      { "case = 1, 0, 4096, 287454020, 2, 32\n"
        "case = 2, 0, 65536, 287454020, 2, 32\n",
        "", edit_no_cases, R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "the declaration carries no case, or more than the bound" },
      { "binding_offset = 0", "binding_offset = 4096",
        edit_buffer_past_allocation,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "the declared buffer reaches outside its allocation" },
      { "case = 1, 0, 4096, 287454020, 2, 32",
        "case = 1, 0, 4094, 287454020, 2, 32", edit_case_size_unaligned,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "a declared case fills nothing, or off a dword boundary" },
      { "case = 1, 0, 4096, 287454020, 2, 32",
        "case = 1, 2, 4096, 287454020, 2, 32", edit_case_offset_unaligned,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "a declared case fills nothing, or off a dword boundary" },
      { "case = 1, 0, 4096, 287454020, 2, 32",
        "case = 1, 0, 4096, 287454020, 0, 0", edit_case_runs_nothing,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "a declared case runs no warmup and no repetition" },
      { "case = 1, 0, 4096, 287454020, 2, 32",
        "case = 1, 8388608, 4096, 287454020, 2, 32", edit_case_past_buffer,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "a declared case reaches outside the declared buffer" },
      { "case = 2, 0, 65536, 287454020, 2, 32",
        "case = 9, 0, 4096, 287454020, 2, 32", edit_duplicate_case,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "two declared cases carry one identity" },
      { "case = 1, 0, 4096, 287454020, 2, 32",
        "case = 1, 0, 4096, 287454020, 4294967295, 1",
        edit_case_count_wraps_in_32_bits,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "the declared budget is above the session bound" },
      { "max_total_submissions = 72", "max_total_submissions = 67",
        edit_total_below_cases,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "the declared total is below what the declared cases run" },
      { "max_total_submissions = 72", "max_total_submissions = 4097",
        edit_budget_above_bound,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "the declared budget is above the session bound" },
      { "completion_timeout_ns = 10000000000",
        "completion_timeout_ns = 0", edit_timeout_zero,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "the declared completion timeout is zero or above the bound" },
      { "completion_timeout_ns = 10000000000",
        "completion_timeout_ns = 300000000001", edit_timeout_above_bound,
        R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
        "the declared completion timeout is zero or above the bound" },
   };

   for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
      const char *text_reason = NULL;
      assert(parse_edited_text(rules[i].find, rules[i].replace,
                               &text_reason) == rules[i].expected);
      assert(text_reason != NULL);
      assert(strcmp(text_reason, rules[i].reason) == 0);

      struct r3v_measurement_manifest edited = parse_valid();
      struct r3v_measurement_session refused;
      memset(&refused, 0, sizeof(refused));
      rules[i].damage(&edited);
      const char *open_reason = NULL;
      assert(r3v_measurement_session_open(&refused, &edited, &digest_a,
                                          &open_reason) ==
             rules[i].expected);
      assert(open_reason != NULL);
      assert(strcmp(open_reason, rules[i].reason) == 0);
      assert(!refused.active);
   }
}

/* A parsed declaration terminates every name by construction, so an
 * unterminated field reaches the session only from a manifest assembled
 * in place.  The session reads these names with strcmp, so the rule
 * stands ahead of the first read rather than behind it. */
static void
test_names_terminate_inside_their_fields(void)
{
   static const size_t offsets[] = {
      offsetof(struct r3v_measurement_manifest, schema),
      offsetof(struct r3v_measurement_manifest, session_nonce),
      offsetof(struct r3v_measurement_manifest, platform),
      offsetof(struct r3v_measurement_manifest, route),
   };
   static const size_t widths[] = {
      sizeof(((struct r3v_measurement_manifest *)0)->schema),
      sizeof(((struct r3v_measurement_manifest *)0)->session_nonce),
      sizeof(((struct r3v_measurement_manifest *)0)->platform),
      sizeof(((struct r3v_measurement_manifest *)0)->route),
   };

   for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
      struct r3v_measurement_manifest edited = parse_valid();
      char *field = (char *)&edited + offsets[i];
      memset(field, 'a', widths[i]);
      struct r3v_measurement_session refused;
      r3v_measurement_session_init(&refused);
      const char *reason = NULL;
      assert(r3v_measurement_session_open(&refused, &edited, &digest_a,
                                          &reason) ==
             R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
      assert(strcmp(reason,
                    "a declared name is empty or runs past its field") == 0);
      assert(!refused.active);

      /* An empty name is a declared field carrying nothing. */
      struct r3v_measurement_manifest emptied = parse_valid();
      *((char *)&emptied + offsets[i]) = '\0';
      r3v_measurement_session_init(&refused);
      assert(r3v_measurement_session_open(&refused, &emptied, &digest_a,
                                          &reason) ==
             R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
      assert(!refused.active);
   }
}

/* The digest is stored whole and compared whole, so its shape is
 * established before the copy rather than trusted from the caller. */
static void
test_manifest_digest_shape(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   static const char *const malformed[] = {
      "",
      "0000000000000000000000000000000000000000000000000000000000000",
      "000000000000000000000000000000000000000000000000000000000000000G",
      "000000000000000000000000000000000000000000000000000000000000000A",
   };
   for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
      const struct r3v_measurement_digest digest = digest_of(malformed[i]);
      struct r3v_measurement_session refused;
      r3v_measurement_session_init(&refused);
      const char *reason = NULL;
      assert(r3v_measurement_session_open(&refused, &m, &digest, &reason) ==
             R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
      assert(reason != NULL);
      assert(!refused.active);
   }
   /* An unterminated digest array carries no width at all. */
   struct r3v_measurement_digest unterminated;
   memset(unterminated.hex, '0', sizeof(unterminated.hex));
   struct r3v_measurement_session refused;
   r3v_measurement_session_init(&refused);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&refused, &m, &unterminated,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   assert(!refused.active);
}

static void
test_route_and_role(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   r3v_measurement_session_init(&session);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   assert(r3v_measurement_session_route_check(
             &session, "rb2d_const_fill_v2", &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   /* A campaign written for the windowed route authorizes no submission
    * the frozen route performs. */
   assert(r3v_measurement_session_route_check(&session, "rb2d_const_fill",
                                              &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROUTE_MISMATCH);
   assert(r3v_measurement_session_route_check(&session, NULL, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROUTE_MISMATCH);

   const struct r3v_measurement_role role = declared_role(&m);
   assert(r3v_measurement_session_role_check(&session, &role, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* Each role field alone: a destination differing in one of them is a
    * destination the declaration did not describe. */
   struct r3v_measurement_role observed = role;
   observed.allocation_bytes += 4096u;
   assert(r3v_measurement_session_role_check(&session, &observed, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH);
   observed = role;
   observed.buffer_bytes -= 4u;
   assert(r3v_measurement_session_role_check(&session, &observed, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH);
   observed = role;
   observed.binding_offset = 4096u;
   assert(r3v_measurement_session_role_check(&session, &observed, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH);
   observed = role;
   observed.memory_property_flags = 0x1u;
   assert(r3v_measurement_session_role_check(&session, &observed, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH);
   observed = role;
   observed.buffer_usage = 0x1u;
   assert(r3v_measurement_session_role_check(&session, &observed, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH);
   observed = role;
   observed.write_domain = 0x4u;
   assert(r3v_measurement_session_role_check(&session, &observed, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH);
}

static void
test_case_lookup(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   r3v_measurement_session_init(&session);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   uint32_t index = UINT32_MAX;
   assert(r3v_measurement_session_find_case(&session, 0, 4096u, 287454020u,
                                            &index) != NULL);
   assert(index == 0u);
   assert(r3v_measurement_session_find_case(&session, 0, 65536u, 287454020u,
                                            &index) != NULL);
   assert(index == 1u);

   /* A case is the exact fill, not a range a request falls inside: one
    * differing field makes it undeclared. */
   assert(r3v_measurement_session_find_case(&session, 0, 4100u, 287454020u,
                                            &index) == NULL);
   assert(r3v_measurement_session_find_case(&session, 4u, 4096u, 287454020u,
                                            &index) == NULL);
   assert(r3v_measurement_session_find_case(&session, 0, 4096u, 287454021u,
                                            &index) == NULL);

   r3v_measurement_session_close(&session, "terminated by the test");
   assert(r3v_measurement_session_find_case(&session, 0, 4096u, 287454020u,
                                            &index) == NULL);
}

static void
test_binding(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   r3v_measurement_session_init(&session);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* The first authorized preparation records the object and the
    * identity; a second identical one is admitted against them. */
   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.bindings[0].bound);
   assert(session.bindings[0].destination_handle == 7u);
   assert(session.bindings[0].memory_generation == 101u);
   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* A replaced destination of the same shape.  It contradicts the
    * declaration rather than naming a request the session declines, so it
    * terminates the session: a refusal alone would leave the binding
    * standing and admit the next repetition against it. */
   assert(r3v_measurement_session_bind(&session, CASE_4K, 8u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(session.closed);
   assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);

   /* The handle recycled over another object: the number matches and the
    * generation does not. */
   struct r3v_measurement_session recycled;
   r3v_measurement_session_init(&recycled);
   assert(r3v_measurement_session_open(&recycled, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&recycled, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&recycled, CASE_4K, 7u, 102u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(recycled.closed);

   /* The same object under a stream that no longer hashes the same. */
   struct r3v_measurement_session drifted;
   r3v_measurement_session_init(&drifted);
   assert(r3v_measurement_session_open(&drifted, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&drifted, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&drifted, CASE_4K, 7u, 101u, &digest_b,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(drifted.closed);
   /* The next repetition finds a closed session rather than a standing
    * binding. */
   assert(r3v_measurement_session_consume(&drifted, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);

   r3v_measurement_session_init(&session);
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* A caller that reached the bind without stamping a generation binds
    * nothing. */
   assert(r3v_measurement_session_bind(&session, CASE_64K, 9u, 0u, &digest_b,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(!session.bindings[1].bound);
   /* A value that is not a digest is not a weaker identity. */
   assert(r3v_measurement_session_bind(&session, CASE_64K, 9u, 103u,
                                       &digest_not_hex, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(r3v_measurement_session_bind(&session, CASE_64K, 9u, 103u, NULL,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(!session.bindings[1].bound);

   /* A case index outside the declaration. */
   assert(r3v_measurement_session_bind(&session, 2u, CASE_4K_OPERATION, 9u,
                                       103u, &digest_b,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED);
}

/* The index selects a row; the operation decides whether that row is the
 * one the request runs.  Neither binding nor consumption takes the index
 * on its own word. */
static void
test_index_alone_authorizes_nothing(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   memset(&session, 0, sizeof(session));
   const char *reason = NULL;
   r3v_measurement_session_init(&session);
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* Case 0 declares 4096 bytes at offset 0 with one value.  Each field
    * refuses on its own, so no single substitution slips through. */
   static const struct {
      uint64_t offset;
      uint64_t bytes;
      uint32_t value;
   } wrong[] = {
      { 4096u, 4096u, 287454020u },
      { 0u, 65536u, 287454020u },
      { 0u, 4096u, 287454021u },
   };
   for (size_t i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
      reason = NULL;
      assert(r3v_measurement_session_bind(&session, 0u, wrong[i].offset,
                                          wrong[i].bytes, wrong[i].value, 7u,
                                          101u, &digest_a, &reason) ==
             R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED);
      assert(reason != NULL);
      assert(!session.bindings[0].bound);
   }

   /* Case 1's operation under case 0's index is the substitution the
    * index-only contract would have admitted. */
   assert(r3v_measurement_session_bind(&session, 0u, 0u, 65536u, 287454020u,
                                       7u, 101u, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED);

   assert(r3v_measurement_session_bind(&session, CASE_4K,
                                       7u, 101u, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* Consumption holds the same line, so the budget a submission spends
    * is the budget of the case that submission runs. */
   assert(r3v_measurement_session_consume(&session, 0u, 0u, 65536u,
                                          287454020u, BOUND_A, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED);
   assert(session.bindings[0].executions_consumed == 0u);
   assert(session.remaining_submissions == 68u);
   assert(r3v_measurement_session_consume(&session, 0u, 0u, 4096u,
                                          287454020u, BOUND_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.bindings[0].executions_consumed == 1u);

   /* The session stays open: a mismatched request names a submission the
    * declaration does not carry, which is a refusal, not evidence that
    * the campaign was tampered with. */
   assert(!session.closed);
}

/* The bind authorizes one object and one stream; the consume spends the
 * execution that submission runs.  A consume carrying the operation alone
 * would let a bind against one allocation stand while the submission ran
 * against another the role also admits, and the standing binding cannot
 * see that substitution.  Both substitutions terminate the campaign. */
static void
test_consume_authorizes_the_bound_resource(void)
{
   const struct r3v_measurement_manifest m = parse_valid();

   struct r3v_measurement_session substituted;
   r3v_measurement_session_init(&substituted);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&substituted, &m, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&substituted, CASE_4K, BOUND_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   /* Another allocation of the same declared role. */
   assert(r3v_measurement_session_consume(&substituted, CASE_4K, 8u, 101u,
                                          &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(substituted.closed);
   assert(substituted.consumed_submissions == 0u);

   /* The bound handle recycled over a later allocation. */
   struct r3v_measurement_session recycled;
   r3v_measurement_session_init(&recycled);
   assert(r3v_measurement_session_open(&recycled, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&recycled, CASE_4K, BOUND_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_consume(&recycled, CASE_4K, 7u, 102u,
                                          &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(recycled.closed);

   /* The bound object under a stream that no longer hashes the same. */
   struct r3v_measurement_session drifted;
   r3v_measurement_session_init(&drifted);
   assert(r3v_measurement_session_open(&drifted, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&drifted, CASE_4K, BOUND_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_consume(&drifted, CASE_4K, 7u, 101u,
                                          &digest_b, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(drifted.closed);
   assert(drifted.consumed_submissions == 0u);

   /* A digest of the wrong shape is not a weaker identity. */
   struct r3v_measurement_session shapeless;
   r3v_measurement_session_init(&shapeless);
   assert(r3v_measurement_session_open(&shapeless, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&shapeless, CASE_4K, BOUND_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_consume(&shapeless, CASE_4K, 7u, 101u,
                                          &digest_not_hex, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(r3v_measurement_session_consume(&shapeless, CASE_4K, 7u, 101u,
                                          NULL, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   /* A shape refusal names a request the session cannot read, not a
    * contradicted declaration, so the campaign stands. */
   assert(!shapeless.closed);
   assert(r3v_measurement_session_consume(&shapeless, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
}

/* What this predicate does not decide.  Its counters live in one struct
 * and die with it, so a fresh struct over the same declaration receives
 * the whole allowance again.  Bounding a campaign across devices and
 * processes is the durable claim's job, and this test pins the boundary
 * so the predicate is not read as the whole one. */
static void
test_a_fresh_session_is_a_fresh_allowance(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session first;
   r3v_measurement_session_init(&first);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&first, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&first, CASE_4K, BOUND_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_consume(&first, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   r3v_measurement_session_close(&first, "the campaign ended");

   struct r3v_measurement_session second;
   r3v_measurement_session_init(&second);
   assert(r3v_measurement_session_open(&second, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(second.remaining_submissions == 68u);
   assert(second.consumed_submissions == 0u);
}

static void
test_consume_is_finite_and_never_refunded(void)
{
   struct r3v_measurement_manifest m = parse_valid();
   /* One case, four executions, so the bound is reached inside a test
    * rather than after seventy submissions. */
   m.case_count = 1u;
   m.cases[0].warmups = 1u;
   m.cases[0].repetitions = 3u;
   m.max_total_submissions = 4u;

   struct r3v_measurement_session session;
   r3v_measurement_session_init(&session);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.remaining_submissions == 4u);

   /* A case consumes nothing before it binds its destination. */
   assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(session.consumed_submissions == 0u);

   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* Exactly the declared number passes. */
   for (uint32_t i = 0; i < 4u; i++) {
      assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
      assert(session.consumed_submissions == i + 1u);
      assert(session.remaining_submissions == 4u - (i + 1u));
   }
   /* The next one refuses. */
   assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(session.consumed_submissions == 4u);
   /* Binding again does not replenish it. */
   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(r3v_measurement_session_consume(&session, CASE_64K, BOUND_B,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED);
}

/* One case exhausts while another stays funded, so the case bound is
 * shown to be the one a campaign meets.  The session allowance is the sum
 * of the case allowances, so the session counter reaches zero only when
 * every case has: session-only exhaustion is unreachable under this
 * accounting, and the second operand of the budget check stands against a
 * declaration form that would carry a smaller total. */
static void
test_one_case_exhausts_while_another_stays_funded(void)
{
   struct r3v_measurement_manifest m = parse_valid();
   m.cases[0].warmups = 1u;
   m.cases[0].repetitions = 1u;
   m.cases[1].warmups = 1u;
   m.cases[1].repetitions = 3u;
   m.max_total_submissions = 6u;

   struct r3v_measurement_session session;
   r3v_measurement_session_init(&session);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.remaining_submissions == 6u);
   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&session, CASE_64K, 8u, 102u, &digest_b,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   for (uint32_t i = 0; i < 2u; i++) {
      assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
   }
   /* The first case is spent and the session is not. */
   assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(session.remaining_submissions == 4u);
   assert(!session.closed);

   /* The second case still runs its whole declaration. */
   for (uint32_t i = 0; i < 4u; i++) {
      assert(r3v_measurement_session_consume(&session, CASE_64K, BOUND_B,
                                          &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
   }
   assert(session.remaining_submissions == 0u);
   assert(r3v_measurement_session_consume(&session, CASE_64K, BOUND_B,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(session.consumed_submissions == 6u);
}

/* Writes the reason into a frame that returns before the reason is read,
 * which is the shape a queue-tail helper takes. */
static void
close_with_a_stack_reason(struct r3v_measurement_session *session,
                          unsigned case_id)
{
   char why[64];
   snprintf(why, sizeof(why), "case %u read a wrong byte", case_id);
   r3v_measurement_session_close(session, why);
}

static void
test_closed_session_admits_nothing(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   r3v_measurement_session_init(&session);
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   r3v_measurement_session_close(&session, "the completion failed");
   /* A second close keeps the first reason: the first failure is the one
    * that ended the run. */
   r3v_measurement_session_close(&session, "a later reason");
   assert(strcmp(session.closed_reason, "the completion failed") == 0);

   /* The session copies the characters, so a reason formatted on a
    * caller's stack survives the frame it was written in. */
   struct r3v_measurement_session formatted;
   r3v_measurement_session_init(&formatted);
   assert(r3v_measurement_session_open(&formatted, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   close_with_a_stack_reason(&formatted, 7);
   assert(strcmp(formatted.closed_reason, "case 7 read a wrong byte") == 0);

   const struct r3v_measurement_role role = declared_role(&m);
   assert(r3v_measurement_session_route_check(
             &session, "rb2d_const_fill_v2", &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);
   assert(r3v_measurement_session_role_check(&session, &role, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);
   assert(r3v_measurement_session_bind(&session, CASE_4K, 7u, 101u, &digest_a,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);
   assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);

   /* An unopened session is inactive rather than closed, so the ordinary
    * one-shot path decides. */
   struct r3v_measurement_session inactive;
   memset(&inactive, 0, sizeof(inactive));
   assert(r3v_measurement_session_role_check(&inactive, &role, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(r3v_measurement_session_consume(&inactive, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(r3v_measurement_session_find_case(&inactive, 0, 4096u, 287454020u,
                                            NULL) == NULL);
}

static void
test_every_refusal_carries_a_reason(void)
{
   /* A refusal code with no sentence beside it tells the caller that
    * something was refused and not what.  Every entry point states its
    * fact, including the ones that refuse before reading a request. */
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session inactive;
   r3v_measurement_session_init(&inactive);
   struct r3v_measurement_role observed = declared_role(&m);
   const char *reason = NULL;

   assert(r3v_measurement_session_route_check(&inactive, "rb2d_const_fill_v2",
                                              &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(reason != NULL);
   reason = NULL;
   assert(r3v_measurement_session_role_check(&inactive, &observed,
                                             &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(reason != NULL);
   reason = NULL;
   assert(r3v_measurement_session_bind(&inactive, CASE_4K, BOUND_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(reason != NULL);
   reason = NULL;
   assert(r3v_measurement_session_consume(&inactive, CASE_4K, BOUND_A,
                                          &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(reason != NULL);

   /* An observation the caller never resolved is a role mismatch on a
    * live session, not an absent session. */
   struct r3v_measurement_session live;
   r3v_measurement_session_init(&live);
   assert(r3v_measurement_session_open(&live, &m, &digest_a, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   reason = NULL;
   assert(r3v_measurement_session_role_check(&live, NULL, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH);
   assert(reason != NULL);
}

static void
test_every_refusal_names_itself(void)
{
   for (int r = 0; r < R3V_MEASUREMENT_SESSION_REFUSAL_COUNT; r++) {
      const char *name = r3v_measurement_session_refusal_name(
         (enum r3v_measurement_session_refusal)r);
      assert(name != NULL && strcmp(name, "unknown") != 0);
   }
   assert(strcmp(r3v_measurement_session_refusal_name(
                    (enum r3v_measurement_session_refusal)
                       R3V_MEASUREMENT_SESSION_REFUSAL_COUNT),
                 "unknown") == 0);
}

int
main(void)
{
   test_manifest_reads_every_field();
   test_manifest_refusals();
   test_epoch();
   test_epoch_reads_only_terminated_names();
   test_open_budget();
   test_semantic_rules_hold_on_both_paths();
   test_names_terminate_inside_their_fields();
   test_manifest_digest_shape();
   test_route_and_role();
   test_case_lookup();
   test_binding();
   test_index_alone_authorizes_nothing();
   test_consume_authorizes_the_bound_resource();
   test_a_fresh_session_is_a_fresh_allowance();
   test_consume_is_finite_and_never_refunded();
   test_one_case_exhausts_while_another_stays_funded();
   test_closed_session_admits_nothing();
   test_every_refusal_names_itself();
   test_every_refusal_carries_a_reason();
   printf("r3v_measurement_session: the declaration, the late binding, and "
          "every refusal hold\n");
   return 0;
}
