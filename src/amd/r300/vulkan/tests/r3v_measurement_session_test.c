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

#include <stdio.h>
#include <string.h>

#define DIGEST_A                                                              \
   "0000000000000000000000000000000000000000000000000000000000000001"
#define DIGEST_B                                                              \
   "0000000000000000000000000000000000000000000000000000000000000002"

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
 * to the field it names. */
static void
refuse_manifest(const char *find, const char *replace,
                enum r3v_measurement_session_refusal expected)
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
   const char *reason = NULL;
   const enum r3v_measurement_session_refusal r =
      r3v_measurement_manifest_parse(text, head + replace_len + tail_len,
                                     &manifest, &reason);
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

static void
test_open_budget(void)
{
   struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   const char *reason = NULL;

   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.active && !session.closed);
   /* Two cases at two warmups plus thirty-two repetitions each. */
   assert(session.remaining_submissions == 68u);
   assert(session.consumed_submissions == 0u);

   /* A declared total below what the cases run cannot run the campaign
    * it declares. */
   m.max_total_submissions = 67u;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED);
   m.max_total_submissions = 68u;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.remaining_submissions == 68u);
}

static void
test_route_and_role(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
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
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
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
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* The first authorized preparation records the object and the
    * identity; a second identical one is admitted against them. */
   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.bindings[0].bound);
   assert(session.bindings[0].destination_handle == 7u);
   assert(session.bindings[0].memory_generation == 101u);
   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* A replaced destination of the same shape.  It contradicts the
    * declaration rather than naming a request the session declines, so it
    * terminates the session: a refusal alone would leave the binding
    * standing and admit the next repetition against it. */
   assert(r3v_measurement_session_bind(&session, 0u, 8u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(session.closed);
   assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);

   /* The handle recycled over another object: the number matches and the
    * generation does not. */
   struct r3v_measurement_session recycled;
   assert(r3v_measurement_session_open(&recycled, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&recycled, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&recycled, 0u, 7u, 102u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(recycled.closed);

   /* The same object under a stream that no longer hashes the same. */
   struct r3v_measurement_session drifted;
   assert(r3v_measurement_session_open(&drifted, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&drifted, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&drifted, 0u, 7u, 101u, DIGEST_B,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(drifted.closed);
   /* The next repetition finds a closed session rather than a standing
    * binding. */
   assert(r3v_measurement_session_consume(&drifted, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);

   memset(&session, 0, sizeof(session));
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* A caller that reached the bind without stamping a generation binds
    * nothing. */
   assert(r3v_measurement_session_bind(&session, 1u, 9u, 0u, DIGEST_B,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(!session.bindings[1].bound);
   /* A value that is not a digest is not a weaker identity. */
   assert(r3v_measurement_session_bind(&session, 1u, 9u, 103u, "0xdeadbeef",
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(r3v_measurement_session_bind(&session, 1u, 9u, 103u, NULL,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH);
   assert(!session.bindings[1].bound);

   /* A case index outside the declaration. */
   assert(r3v_measurement_session_bind(&session, 2u, 9u, 103u, DIGEST_B,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED);
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
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.remaining_submissions == 4u);

   /* A case consumes nothing before it binds its destination. */
   assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND);
   assert(session.consumed_submissions == 0u);

   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   /* Exactly the declared number passes. */
   for (uint32_t i = 0; i < 4u; i++) {
      assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
      assert(session.consumed_submissions == i + 1u);
      assert(session.remaining_submissions == 4u - (i + 1u));
   }
   /* The next one refuses. */
   assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(session.consumed_submissions == 4u);
   /* Binding again does not replenish it. */
   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(r3v_measurement_session_consume(&session, 1u, &reason) ==
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
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(session.remaining_submissions == 6u);
   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&session, 1u, 8u, 102u, DIGEST_B,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   for (uint32_t i = 0; i < 2u; i++) {
      assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
   }
   /* The first case is spent and the session is not. */
   assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(session.remaining_submissions == 4u);
   assert(!session.closed);

   /* The second case still runs its whole declaration. */
   for (uint32_t i = 0; i < 4u; i++) {
      assert(r3v_measurement_session_consume(&session, 1u, &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
   }
   assert(session.remaining_submissions == 0u);
   assert(r3v_measurement_session_consume(&session, 1u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED);
   assert(session.consumed_submissions == 6u);
}

static void
test_closed_session_admits_nothing(void)
{
   const struct r3v_measurement_manifest m = parse_valid();
   struct r3v_measurement_session session;
   const char *reason = NULL;
   assert(r3v_measurement_session_open(&session, &m, DIGEST_A, &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);

   r3v_measurement_session_close(&session, "the completion failed");
   /* A second close keeps the first reason: the first failure is the one
    * that ended the run. */
   r3v_measurement_session_close(&session, "a later reason");
   assert(strcmp(session.closed_reason, "the completion failed") == 0);

   const struct r3v_measurement_role role = declared_role(&m);
   assert(r3v_measurement_session_route_check(
             &session, "rb2d_const_fill_v2", &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);
   assert(r3v_measurement_session_role_check(&session, &role, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);
   assert(r3v_measurement_session_bind(&session, 0u, 7u, 101u, DIGEST_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);
   assert(r3v_measurement_session_consume(&session, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_CLOSED);

   /* An unopened session is inactive rather than closed, so the ordinary
    * one-shot path decides. */
   struct r3v_measurement_session inactive;
   memset(&inactive, 0, sizeof(inactive));
   assert(r3v_measurement_session_role_check(&inactive, &role, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(r3v_measurement_session_consume(&inactive, 0u, &reason) ==
          R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE);
   assert(r3v_measurement_session_find_case(&inactive, 0, 4096u, 287454020u,
                                            NULL) == NULL);
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
   test_open_budget();
   test_route_and_role();
   test_case_lookup();
   test_binding();
   test_consume_is_finite_and_never_refunded();
   test_one_case_exhausts_while_another_stays_funded();
   test_closed_session_admits_nothing();
   test_every_refusal_names_itself();
   printf("r3v_measurement_session: the declaration, the late binding, and "
          "every refusal hold\n");
   return 0;
}
