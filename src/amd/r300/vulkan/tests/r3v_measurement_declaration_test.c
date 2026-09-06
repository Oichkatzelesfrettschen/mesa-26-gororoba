/*
 * SPDX-License-Identifier: MIT
 *
 * The declaration load, boundary by boundary.  Every arm drives a real
 * file through the real loader: the open, the bounded read, the text
 * allocation, the parse, the epoch, the board, the route, and the
 * session open each refuse alone, and the whole load leaves nothing
 * durable behind.
 */

#include "r3v_measurement_declaration.h"
#include "r3v_submit_preflight.h"

#include "amd/r300/common/r300_chip_identity.h"
#include "amd/r300/common/r300_operation_route.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* The whole declared allowance: two cases at two warmups and thirty-two
 * repetitions apiece. */
#define DECLARED_ALLOWANCE 68u

/* The 4K case as a call site names it: the index the request cites and
 * the offset, size, and value it fills. */
#define CASE_4K 0u, 0u, 4096u, 287454020u
/* The object and stream one repetition binds: the destination's GEM
 * handle, its allocation generation, and the concrete fill identity. */
#define BOUND_A 7u, 101u, &digest_a

static const struct r3v_measurement_digest digest_a = {
   "1111111111111111111111111111111111111111111111111111111111111111"
};

/* The gate decisions a device hands the load, indexed the way the device
 * indexes them.  Only the windowed route's own opt-in stands open, so a
 * route naming a different gate reads false and refuses. */
static bool open_gates[R300_OPERATION_ROUTE_COUNT];
static bool closed_gates[R300_OPERATION_ROUTE_COUNT];

static char scratch_path[256];

/* Writes `bytes` of `text` to the scratch file and returns its path. */
static const char *
write_declaration(const char *text, size_t bytes)
{
   FILE *f = fopen(scratch_path, "wb");
   assert(f != NULL);
   assert(fwrite(text, 1, bytes, f) == bytes);
   assert(fclose(f) == 0);
   return scratch_path;
}

static struct r3v_measurement_deployment
specimen(void)
{
   uint32_t route_count = 0;
   const struct r300_operation_route_row *routes =
      r300_operation_route_rows(&route_count);
   return (struct r3v_measurement_deployment){
      .pci_vendor_id = 0x1002u,
      .pci_device_id = 0x5974u,
      .kernel_release = "7.1.8-1-cachyos",
      .module_srcversion = "729892A3F3530EB12B8D842",
      .platform_id = R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M,
      .routes = routes,
      .route_count = route_count,
      .route_gate_open = open_gates,
      .route_gate_count = R300_OPERATION_ROUTE_COUNT,
   };
}

/* Loads `text` under `deployment` and returns the status, with the reason
 * and the session left for the caller to inspect. */
static enum r3v_measurement_declaration_status
load(const char *text, const struct r3v_measurement_deployment *deployment,
     struct r3v_measurement_session *session, const char **reason)
{
   r3v_measurement_session_init(session);
   const char *path = write_declaration(text, strlen(text));
   return r3v_measurement_declaration_open(session, path, deployment, reason);
}

/* Replaces the first occurrence of `find` with `replace` in a copy of the
 * valid declaration, so one arm changes one field. */
static char *
edited(const char *find, const char *replace)
{
   const char *at = strstr(valid_manifest, find);
   assert(at != NULL);
   const size_t head = (size_t)(at - valid_manifest);
   const size_t tail = strlen(at + strlen(find));
   char *out = malloc(head + strlen(replace) + tail + 1);
   assert(out != NULL);
   memcpy(out, valid_manifest, head);
   memcpy(out + head, replace, strlen(replace));
   memcpy(out + head + strlen(replace), at + strlen(find), tail + 1);
   return out;
}

static void
refuses(const char *find, const char *replace, const char *expected_reason)
{
   char *text = edited(find, replace);
   const struct r3v_measurement_deployment deployment = specimen();
   struct r3v_measurement_session session;
   const char *reason = NULL;
   assert(load(text, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(reason != NULL);
   assert(strcmp(reason, expected_reason) == 0);
   assert(!session.active);
   free(text);
}

/* A declaration nothing names leaves the device on its ordinary path, and
 * the session it did not open admits nothing. */
static void
test_an_unnamed_declaration_is_absent(void)
{
   const struct r3v_measurement_deployment deployment = specimen();
   struct r3v_measurement_session session;
   const char *reason = NULL;
   r3v_measurement_session_init(&session);
   assert(r3v_measurement_declaration_open(&session, NULL, &deployment,
                                           &reason) ==
          R3V_MEASUREMENT_DECLARATION_ABSENT);
   assert(reason == NULL);
   assert(!session.active);
   assert(r3v_measurement_declaration_open(&session, "", &deployment,
                                           &reason) ==
          R3V_MEASUREMENT_DECLARATION_ABSENT);
   assert(!session.active);
}

/* A named declaration that is not there is the operator's defect, not an
 * absent one: it refuses rather than falling back. */
static void
test_a_named_declaration_that_does_not_open_refuses(void)
{
   const struct r3v_measurement_deployment deployment = specimen();
   struct r3v_measurement_session session;
   const char *reason = NULL;
   r3v_measurement_session_init(&session);
   assert(r3v_measurement_declaration_open(
             &session, "/nonexistent/r3v-measurement-declaration",
             &deployment, &reason) == R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(reason != NULL);
   assert(strcmp(reason, "the declaration does not open") == 0);
   assert(!session.active);
}

/* A shortage of memory is not a defect in what the operator wrote, so it
 * carries its own status and the caller reports it as one. */
static void
test_a_refused_text_allocation_is_not_a_malformed_declaration(void)
{
   struct r3v_measurement_deployment deployment = specimen();
   deployment.refuse_text_allocation = true;
   struct r3v_measurement_session session;
   const char *reason = NULL;
   assert(load(valid_manifest, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_NO_MEMORY);
   assert(reason != NULL);
   assert(!session.active);
}

/* The parser admits a declaration of exactly the text bound, so the
 * reader has to admit that file: a buffer sized to the bound would report
 * the legal maximum as above it.  One byte more refuses. */
static void
test_the_text_bound_is_the_last_admitted_byte(void)
{
   const size_t bound = R3V_MEASUREMENT_SESSION_TEXT_MAX;
   char *text = malloc(bound + 2u);
   assert(text != NULL);
   const size_t body = strlen(valid_manifest);
   /* Comment padding carries the file to the bound without changing what
    * it declares. */
   memcpy(text, valid_manifest, body);
   text[body] = '#';
   memset(text + body + 1u, 'x', bound + 1u - (body + 1u));
   text[bound + 1u] = '\0';

   const struct r3v_measurement_deployment deployment = specimen();
   struct r3v_measurement_session session;
   const char *reason = NULL;

   const char *path = write_declaration(text, bound);
   r3v_measurement_session_init(&session);
   assert(r3v_measurement_declaration_open(&session, path, &deployment,
                                           &reason) ==
          R3V_MEASUREMENT_DECLARATION_OPENED);
   assert(session.active);

   path = write_declaration(text, bound + 1u);
   r3v_measurement_session_init(&session);
   assert(r3v_measurement_declaration_open(&session, path, &deployment,
                                           &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(reason != NULL);
   assert(strcmp(reason,
                 "the declaration does not read, or is above the text "
                 "bound") == 0);
   assert(!session.active);
   free(text);
}

/* The parse, the epoch, and the board each refuse on their own, and the
 * board's three facts are three refusals: an operator's typo and a board
 * the tables do not carry are separate defects, so two unresolved names
 * never agree. */
static void
test_each_check_refuses_alone(void)
{
   refuses("schema = r3v-measurement-session-v1",
           "schema = r3v-measurement-session-v2",
           "the declaration names another schema");
   /* Both halves of the PCI pair and both halves of the deployment share
    * one refusal apiece, so each half gets its own arm: a check that
    * dropped the vendor or the srcversion term would otherwise still
    * refuse through its partner. */
   refuses("pci_vendor_id = 0x1002", "pci_vendor_id = 0x1003",
           "the declaration names another device");
   refuses("pci_device_id = 0x5974", "pci_device_id = 0x5975",
           "the declaration names another device");
   refuses("kernel_release = 7.1.8-1-cachyos",
           "kernel_release = 7.2.2-1-cachyos",
           "the declaration names another deployment");
   refuses("module_srcversion = 729892A3F3530EB12B8D842",
           "module_srcversion = 46C05689F3530EB12B8D842",
           "the declaration names another deployment");
   refuses("platform = vostro1000_rs485m_5974",
           "platform = vostro1000_rs485m_5975",
           "the declaration names a platform no board row carries");

   char *text = edited("platform = vostro1000_rs485m_5974",
                       "platform = vostro1000_rs485m_5974");
   struct r3v_measurement_deployment deployment = specimen();
   deployment.platform_id = R300_PLATFORM_ID_NONE;
   struct r3v_measurement_session session;
   const char *reason = NULL;
   assert(load(text, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(strcmp(reason,
                 "the running board resolved to no qualified platform") == 0);
   assert(!session.active);

   /* The third platform fact: two identities that both resolve, and
    * disagree.  The shipped table carries one row, so the running id is
    * set to a second identity directly; a second board row makes this
    * reachable from a declaration name as well. */
   deployment.platform_id = R300_PLATFORM_ID_COUNT;
   assert(deployment.platform_id != R300_PLATFORM_ID_NONE);
   assert(deployment.platform_id !=
          R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M);
   assert(load(text, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(strcmp(reason,
                 "the declaration names another board than the one "
                 "running") == 0);
   assert(!session.active);
   free(text);
}

/* The route is six facts and each refuses alone.  The load reads the
 * device's gate cache and never writes it, so a declaration naming a
 * gated route under a closed gate refuses rather than opening it. */
static void
test_the_route_is_held_to_what_this_build_and_device_carry(void)
{
   refuses("route = rb2d_const_fill_v2", "route = no_such_route",
           "the declaration names a route this build does not carry");
   refuses("route = rb2d_const_fill_v2", "route = host_transfer_const_fill",
           "the declaration names a route that runs on the host");
   refuses("route = rb2d_const_fill_v2", "route = rb3d_clear_const_fill",
           "the declaration names a route that does not execute");
   refuses("route = rb2d_const_fill_v2", "route = r2vb_identity_map",
           "the declaration names a route that fills no constant");

   /* The executor, the evidence scope, and the operation are three fields.
    * A row carrying HOST beside this route's own delivery scope, or a GPU
    * row realizing another operation, is refused by the field that names
    * the defect rather than by whichever one the table happens to
    * correlate with it. */
   uint32_t route_count = 0;
   const struct r300_operation_route_row *rows =
      r300_operation_route_rows(&route_count);
   struct r300_operation_route_row mutated[R300_OPERATION_ROUTE_COUNT];
   assert(route_count <= R300_OPERATION_ROUTE_COUNT);
   memcpy(mutated, rows, route_count * sizeof(*rows));
   uint32_t v2 = route_count;
   for (uint32_t i = 0; i < route_count; i++) {
      if (mutated[i].route_id == R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2)
         v2 = i;
   }
   assert(v2 < route_count);

   struct r3v_measurement_deployment deployment = specimen();
   deployment.routes = mutated;
   deployment.route_count = route_count;
   struct r3v_measurement_session session;
   const char *reason = NULL;

   mutated[v2].evidence_scope =
      R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL;
   assert(load(valid_manifest, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(strcmp(reason,
                 "the declaration names a route with no delivery of its "
                 "own") == 0);
   mutated[v2].evidence_scope = rows[v2].evidence_scope;

   mutated[v2].uses &= ~(uint32_t)R300_ROUTE_USE_TRANSFER_BUFFER;
   assert(load(valid_manifest, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(strcmp(reason,
                 "the declaration names a route that serves no transfer "
                 "destination") == 0);
   mutated[v2].uses = rows[v2].uses;

   /* The whole ledger stands again, so the mutated copy admits what the
    * shipped table admits and the arms above changed one field each. */
   assert(load(valid_manifest, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_OPENED);

   /* A gate decision, not a gate string: the load consumes what
    * r3v_route_gate_state_from_cache decided, so a cache entry holding
    * "0" or "" never reaches it as an open gate. */
   deployment = specimen();
   deployment.route_gate_open = closed_gates;
   assert(load(valid_manifest, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(strcmp(reason,
                 "the declaration names a route whose opt-in stands "
                 "closed") == 0);
   assert(!session.active);

   deployment.route_gate_open = NULL;
   assert(load(valid_manifest, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(strcmp(reason,
                 "the declaration names a route whose opt-in stands "
                 "closed") == 0);
}

/* The device turns its gate cache into these decisions through the one
 * function that owns the rule, so a cached value that is not the literal
 * "1" closes the route the load is about to read. */
static void
test_a_gate_opens_on_the_literal_one_alone(void)
{
   const char *cache[R300_OPERATION_ROUTE_COUNT] = { NULL };
   bool state[R300_OPERATION_ROUTE_COUNT];
   const uint32_t v2 = R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2;

   static const char *const closed_values[] = { "0", "", "true", "01", " 1" };
   for (size_t i = 0; i < sizeof(closed_values) / sizeof(closed_values[0]);
        i++) {
      cache[v2] = closed_values[i];
      assert(r3v_route_gate_state_from_cache(cache, state,
                                             R300_OPERATION_ROUTE_COUNT));
      assert(!state[v2]);
   }
   cache[v2] = "1";
   assert(r3v_route_gate_state_from_cache(cache, state,
                                          R300_OPERATION_ROUTE_COUNT));
   assert(state[v2]);
}

/* The load opens one session over the declared allowance, and the digest
 * it stores is the digest of the bytes it parsed. */
static void
test_a_valid_declaration_opens_one_session_over_its_own_bytes(void)
{
   const struct r3v_measurement_deployment deployment = specimen();
   struct r3v_measurement_session first;
   struct r3v_measurement_session second;
   const char *reason = NULL;

   assert(load(valid_manifest, &deployment, &first, &reason) ==
          R3V_MEASUREMENT_DECLARATION_OPENED);
   assert(reason == NULL);
   assert(first.active);
   assert(!first.closed);
   assert(first.remaining_submissions == DECLARED_ALLOWANCE);
   assert(first.consumed_submissions == 0u);
   assert(strcmp(first.manifest.route, "rb2d_const_fill_v2") == 0);

   /* The same bytes read again hash the same, and one changed comment
    * byte does not, so the digest names the declaration rather than the
    * load. */
   assert(load(valid_manifest, &deployment, &second, &reason) ==
          R3V_MEASUREMENT_DECLARATION_OPENED);
   assert(strcmp(first.manifest_digest.hex, second.manifest_digest.hex) == 0);

   char *changed = edited("# the windowed route", "# the windowed ROUTE");
   struct r3v_measurement_session third;
   assert(load(changed, &deployment, &third, &reason) ==
          R3V_MEASUREMENT_DECLARATION_OPENED);
   assert(strcmp(first.manifest_digest.hex, third.manifest_digest.hex) != 0);
   free(changed);
}

/* Reloading over a live session restores no allowance.  The load never
 * touches the session before the open decides, so a second load into a
 * session that already spent part of its budget refuses and leaves the
 * bindings, the counters, and the digest exactly where the first load and
 * the spent executions left them.  A loader that reset first would hand a
 * campaign a fresh allowance for every reload, which is the budget bypass
 * the whole predicate exists to refuse. */
static void
test_a_reload_restores_no_allowance(void)
{
   const struct r3v_measurement_deployment deployment = specimen();
   struct r3v_measurement_session session;
   const char *reason = NULL;
   assert(load(valid_manifest, &deployment, &session, &reason) ==
          R3V_MEASUREMENT_DECLARATION_OPENED);

   /* Spend three of the declared executions against the 4K case, so the
    * counters carry a value a reset would visibly restore. */
   assert(r3v_measurement_session_bind(&session, CASE_4K, BOUND_A,
                                       &reason) ==
          R3V_MEASUREMENT_SESSION_ADMITTED);
   for (unsigned i = 0; i < 3u; i++) {
      assert(r3v_measurement_session_consume(&session, CASE_4K, BOUND_A,
                                             &reason) ==
             R3V_MEASUREMENT_SESSION_ADMITTED);
   }
   assert(session.consumed_submissions == 3u);
   assert(session.remaining_submissions == DECLARED_ALLOWANCE - 3u);

   const struct r3v_measurement_digest spent_digest = session.manifest_digest;
   const struct r3v_measurement_binding spent_binding = session.bindings[0];

   const char *path = write_declaration(valid_manifest,
                                        strlen(valid_manifest));
   assert(r3v_measurement_declaration_open(&session, path, &deployment,
                                           &reason) ==
          R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(reason != NULL);
   assert(strcmp(reason, "the session is already open over a "
                         "declaration") == 0);
   assert(session.active);
   assert(!session.closed);
   assert(session.consumed_submissions == 3u);
   assert(session.remaining_submissions == DECLARED_ALLOWANCE - 3u);
   assert(strcmp(session.manifest_digest.hex, spent_digest.hex) == 0);
   assert(memcmp(&session.bindings[0], &spent_binding,
                 sizeof(spent_binding)) == 0);

   /* An absent or refused declaration leaves that session standing too:
    * a reload naming nothing must not erase a campaign in flight. */
   assert(r3v_measurement_declaration_open(&session, NULL, &deployment,
                                           &reason) ==
          R3V_MEASUREMENT_DECLARATION_ABSENT);
   assert(session.active);
   assert(session.consumed_submissions == 3u);
   assert(r3v_measurement_declaration_open(
             &session, "/nonexistent/r3v-measurement-declaration",
             &deployment, &reason) == R3V_MEASUREMENT_DECLARATION_REFUSED);
   assert(session.active);
   assert(session.consumed_submissions == 3u);
   assert(session.remaining_submissions == DECLARED_ALLOWANCE - 3u);
}

/* A declaration name selects one platform.  Two rows publishing one name
 * would let a declaration resolve to whichever row the search reached
 * first, so the table is held to distinct names. */
static void
test_declaration_names_are_unique_and_resolve_exactly(void)
{
   assert(r300_platform_id_from_declaration_name("vostro1000_rs485m_5974") ==
          R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M);
   assert(r300_platform_id_from_declaration_name(NULL) ==
          R300_PLATFORM_ID_NONE);
   assert(r300_platform_id_from_declaration_name("") ==
          R300_PLATFORM_ID_NONE);
   /* Exact: the resolver folds no case and strips no space, because a
    * declaration is text an operator wrote for one row. */
   assert(r300_platform_id_from_declaration_name("VOSTRO1000_RS485M_5974") ==
          R300_PLATFORM_ID_NONE);
   assert(r300_platform_id_from_declaration_name(
             " vostro1000_rs485m_5974") == R300_PLATFORM_ID_NONE);
   assert(r300_platform_id_from_declaration_name("rs482") ==
          R300_PLATFORM_ID_NONE);

   uint32_t count = 0;
   const struct r300_platform_identity *const *rows =
      r300_platform_identity_rows(&count);
   for (uint32_t i = 0; i < count; i++) {
      if (rows[i]->declaration_name == NULL)
         continue;
      assert(r300_platform_id_from_declaration_name(
                rows[i]->declaration_name) == rows[i]->platform_id);
      for (uint32_t j = i + 1; j < count; j++) {
         if (rows[j]->declaration_name == NULL)
            continue;
         assert(strcmp(rows[i]->declaration_name,
                       rows[j]->declaration_name) != 0);
      }
   }
}

int
main(void)
{
   /* The scratch file lands in the temporary directory rather than the
    * working directory, which under the test runner is the source tree.
    * mkstemp creates it, so two runs of this binary never share one. */
   const char *tmp = getenv("TMPDIR");
   snprintf(scratch_path, sizeof(scratch_path),
            "%s/r3v-measurement-declaration-XXXXXX",
            tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
   const int fd = mkstemp(scratch_path);
   assert(fd >= 0);
   assert(close(fd) == 0);
   open_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2] = true;

   test_an_unnamed_declaration_is_absent();
   test_a_named_declaration_that_does_not_open_refuses();
   test_a_refused_text_allocation_is_not_a_malformed_declaration();
   test_the_text_bound_is_the_last_admitted_byte();
   test_each_check_refuses_alone();
   test_the_route_is_held_to_what_this_build_and_device_carry();
   test_a_gate_opens_on_the_literal_one_alone();
   test_a_valid_declaration_opens_one_session_over_its_own_bytes();
   test_a_reload_restores_no_allowance();
   test_declaration_names_are_unique_and_resolve_exactly();

   remove(scratch_path);
   printf("r3v measurement declaration load: all checks passed\n");
   return 0;
}
