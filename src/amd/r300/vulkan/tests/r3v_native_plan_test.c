/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration for the ordered-submission plan: a sealed plan round-trips,
 * its identity binds, its entries replay in order to exhaustion, and each
 * single defect refuses with its own name.
 */

#undef NDEBUG

#include "r3v_native_plan.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define H64B "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
#define H40 "0123456789abcdef0123456789abcdef01234567"
#define H32 "00112233445566778899aabbccddeeff"

static struct r3v_native_plan_submission subs[2];

static struct r3v_native_plan
good_plan(void)
{
   subs[0] = (struct r3v_native_plan_submission){
      .ib_blake3 = H64, .ib_dwords = 231,
      .cell_kind = R3V_NATIVE_CELL_KIND_TRIANGLE, .emitter = "r3v",
      .reloc_count = 2,
      .relocs = {{"target", 0x4, 0x4, 16384, R3V_NATIVE_PLAN_DIRECTION_WRITE},
                 {"completion", 0x2, 0x2, 4,
                  R3V_NATIVE_PLAN_DIRECTION_READ_WRITE}},
   };
   subs[1] = (struct r3v_native_plan_submission){
      .ib_blake3 = H64B, .ib_dwords = 316,
      .cell_kind = R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED,
      .emitter = "r3v", .reloc_count = 1,
      .relocs = {{"source", 0x2, 0, 96, R3V_NATIVE_PLAN_DIRECTION_READ}},
   };
   struct r3v_native_plan p = {
      .schema_version = R3V_NATIVE_PLAN_SCHEMA_VERSION,
      .source_sha = H40, .source_clean = true, .dso_blake3 = H64,
      .deqp_sha256 = H64, .deqp_release = "opengl-cts-4.6.8.0-414-g43c65c132",
      .partition_sha256 = H64, .caselist_sha256 = H64B,
      .queue_claim = R3V_NATIVE_PLAN_QUEUE_DEFAULT_GRAPHICS_ONLY,
      .kernel_release = "7.1.8-1-cachyos",
      .module_srcversion = "088E045518D972727C1DD1C",
      .pci_vendor_id = R3V_NATIVE_ARMING_PCI_VENDOR,
      .pci_device_id = R3V_NATIVE_ARMING_PCI_DEVICE,
      .nonce = H32, .evidence_dir = "/var/tmp/plan-evidence",
      .max_ib_dwords = 1024, .max_relocs = 8,
      .max_cumulative_bytes = 1 << 20, .max_submissions = 4,
      .max_runtime_seconds = 600, .submission_count = 2,
      .submissions = subs,
   };
   return p;
}

static struct r3v_native_plan_identity
good_identity(void)
{
   return (struct r3v_native_plan_identity){
      .source_sha = H40, .source_clean = true, .dso_blake3 = H64,
      .deqp_sha256 = H64, .deqp_release = "opengl-cts-4.6.8.0-414-g43c65c132",
      .partition_sha256 = H64, .caselist_sha256 = H64B,
      .queue_claim = R3V_NATIVE_PLAN_QUEUE_DEFAULT_GRAPHICS_ONLY,
      .kernel_release = "7.1.8-1-cachyos",
      .module_srcversion = "088E045518D972727C1DD1C",
      .pci_vendor_id = R3V_NATIVE_ARMING_PCI_VENDOR,
      .pci_device_id = R3V_NATIVE_ARMING_PCI_DEVICE,
      .nonce = H32, .evidence_dir_present = true,
      .evidence_dir_empty = true, .gates_open = false,
   };
}

static char text[8192];
static size_t text_len;

static void
write_good(void)
{
   struct r3v_native_plan p = good_plan();
   long n = r3v_native_plan_write(&p, NULL, 0);
   assert(n > 0 && (size_t)n < sizeof(text));
   long m = r3v_native_plan_write(&p, text, sizeof(text));
   assert(m == n);
   text_len = (size_t)m;
}

static enum r3v_native_plan_parse_result
parse_text(const char *t, size_t n, struct r3v_native_plan *out)
{
   enum r3v_native_plan_parse_result r = r3v_native_plan_parse(t, n, out);
   return r;
}

static void
test_round_trip(void)
{
   write_good();
   struct r3v_native_plan p;
   assert(parse_text(text, text_len, &p) == R3V_NATIVE_PLAN_PARSE_OK);
   struct r3v_native_plan g = good_plan();
   assert(p.submission_count == 2 && p.max_submissions == 4);
   assert(strcmp(p.nonce, g.nonce) == 0);
   assert(strcmp(p.evidence_dir, g.evidence_dir) == 0);
   assert(p.pci_device_id == R3V_NATIVE_ARMING_PCI_DEVICE);
   for (unsigned i = 0; i < 2; i++)
      assert(r3v_native_plan_match(&g.submissions[i], &p.submissions[i]) ==
             R3V_NATIVE_PLAN_MATCH_OK);
   /* Re-serializing the parsed plan yields the identical bytes and seal. */
   char again[8192];
   long n = r3v_native_plan_write(&p, again, sizeof(again));
   assert(n == (long)text_len && memcmp(again, text, text_len) == 0);
   assert(r3v_native_plan_bind(&p, &(struct r3v_native_plan_identity){0}) ==
          R3V_NATIVE_PLAN_BIND_SOURCE);
   struct r3v_native_plan_identity id = good_identity();
   assert(r3v_native_plan_bind(&p, &id) == R3V_NATIVE_PLAN_BIND_OK);
   r3v_native_plan_finish(&p);

   struct r3v_native_plan maximum_kernel_release = good_plan();
   memset(maximum_kernel_release.kernel_release, 'k',
          R3V_NATIVE_PLAN_KERNEL_RELEASE_MAX);
   maximum_kernel_release.kernel_release[R3V_NATIVE_PLAN_KERNEL_RELEASE_MAX] =
      '\0';
   long maximum_size = r3v_native_plan_write(&maximum_kernel_release, NULL, 0);
   assert(maximum_size > 0 && (size_t)maximum_size < sizeof(again));
   assert(r3v_native_plan_write(&maximum_kernel_release, again, sizeof(again)) ==
          maximum_size);
   assert(parse_text(again, (size_t)maximum_size, &p) ==
          R3V_NATIVE_PLAN_PARSE_OK);
   r3v_native_plan_finish(&p);
}

/* Applies one textual edit to the good plan, re-seals unless the edit
 * targets the seal, and expects the named parse refusal.
 */
static void
expect_parse(const char *from, const char *to, bool reseal,
             enum r3v_native_plan_parse_result want)
{
   write_good();
   char *at = strstr(text, from);
   assert(at != NULL);
   char edited[8192];
   size_t head = (size_t)(at - text);
   size_t tail = text_len - head - strlen(from);
   memcpy(edited, text, head);
   memcpy(edited + head, to, strlen(to));
   memcpy(edited + head + strlen(to), at + strlen(from), tail);
   size_t n = head + strlen(to) + tail;
   edited[n] = '\0';
   if (reseal) {
      /* Re-seal the edited body so the parser meets the defect and not
       * the seal.
       */
      char *seal = strstr(edited, "seal\t");
      assert(seal != NULL);
      n = r3v_native_plan_seal(edited, sizeof(edited),
                               (size_t)(seal - edited));
      assert(n != 0);
   }
   struct r3v_native_plan p;
   enum r3v_native_plan_parse_result r = parse_text(edited, n, &p);
   if (r != want) {
      fprintf(stderr, "edit %s -> %s: got %s, wanted %s\n", from, to,
              r3v_native_plan_parse_result_name(r),
              r3v_native_plan_parse_result_name(want));
      abort();
   }
   if (r == R3V_NATIVE_PLAN_PARSE_OK)
      r3v_native_plan_finish(&p);
}

static void
test_parse_refusals(void)
{
   expect_parse("r3v-native-conformance-plan\t1", "r3v-native-conformance-plan\t2",
                true, R3V_NATIVE_PLAN_PARSE_SCHEMA_VERSION);
   expect_parse("r3v-native-conformance-plan", "r3v-native-plan", true,
                R3V_NATIVE_PLAN_PARSE_NOT_A_PLAN);
   /* Seal tampering: a lengthened seal, a flipped seal byte, and a
    * flipped body byte.
    */
   expect_parse("seal\t", "seal\tf", false, R3V_NATIVE_PLAN_PARSE_SEAL_MISSING);
   {
      write_good();
      char *seal = strstr(text, "seal\t") + 5;
      seal[0] = seal[0] == '0' ? '1' : '0';
      struct r3v_native_plan p;
      assert(parse_text(text, text_len, &p) ==
             R3V_NATIVE_PLAN_PARSE_SEAL_MISMATCH);
   }
   expect_parse("max_runtime_seconds\t600", "max_runtime_seconds\t601",
                false, R3V_NATIVE_PLAN_PARSE_SEAL_MISMATCH);
   /* Truncation: the seal line is gone, or a submission is gone. */
   {
      write_good();
      char *seal = strstr(text, "seal\t");
      struct r3v_native_plan p;
      assert(parse_text(text, (size_t)(seal - text), &p) ==
             R3V_NATIVE_PLAN_PARSE_SEAL_MISSING);
   }
   expect_parse("submission\t1\t" H64B "\t316\tr2vb_gpu_producer_fetched\tr3v\t1\n"
                "reloc\t1\t0\tsource\t2\t0\t96\tr\n", "", true,
                R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT);
   expect_parse("submission_count\t2", "submission_count\t1", true,
                R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT);
   expect_parse("reloc\t1\t0\tsource\t2\t0\t96\tr\n", "", true,
                R3V_NATIVE_PLAN_PARSE_RELOC_COUNT);
   expect_parse("submission\t1\t", "submission\t0\t", true,
                R3V_NATIVE_PLAN_PARSE_SUBMISSION_ORDER);
   expect_parse("reloc\t0\t1\t", "reloc\t0\t0\t", true,
                R3V_NATIVE_PLAN_PARSE_SUBMISSION_ORDER);
   expect_parse("kernel_release\t7.1.8-1-cachyos\n", "", true,
                R3V_NATIVE_PLAN_PARSE_MISSING_FIELD);
   expect_parse("nonce\t" H32 "\n", "nonce\t" H32 "\nnonce\t" H32 "\n", true,
                R3V_NATIVE_PLAN_PARSE_DUPLICATE_FIELD);
   expect_parse("source_clean\t1", "source_clean\tyes", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("pci\t1002:5974", "pci\t1002-5974", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("max_relocs\t8", "max_relocs\t65", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("max_ib_dwords\t1024", "max_ib_dwords\t300", true,
                R3V_NATIVE_PLAN_PARSE_CEILING_EXCEEDED);
   expect_parse("max_submissions\t4", "max_submissions\t1", true,
                R3V_NATIVE_PLAN_PARSE_CEILING_EXCEEDED);
   expect_parse("\tcompletion\t2\t2\t4\trw\n",
                "\tcompletion\t2\t2\t1040000\trw\n", true,
                R3V_NATIVE_PLAN_PARSE_CEILING_EXCEEDED);
   expect_parse("\t231\ttriangle\t", "\t231\tundeclared\t", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("\t96\tr\n", "\t96\tx\n", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("evidence_dir\t/var/tmp/plan-evidence",
                "evidence_dir\tplan-evidence", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("max_runtime_seconds\t600\n",
                "max_runtime_seconds\t600\nextra\tline\n", true,
                R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE);
   /* A header field after the first submission line is malformed by
    * position, whatever its name.
    */
   expect_parse("reloc\t1\t0\tsource\t2\t0\t96\tr\n",
                "reloc\t1\t0\tsource\t2\t0\t96\tr\nnonce\t" H32 "\n", true,
                R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE);
   /* A complete header with no submission names the missing block. */
   expect_parse("submission\t0\t" H64 "\t231\ttriangle\tr3v\t2\n"
                "reloc\t0\t0\ttarget\t4\t4\t16384\tw\n"
                "reloc\t0\t1\tcompletion\t2\t2\t4\trw\n"
                "submission\t1\t" H64B "\t316\tr2vb_gpu_producer_fetched\tr3v\t1\n"
                "reloc\t1\t0\tsource\t2\t0\t96\tr\n", "", true,
                R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT);
   /* Canonical form: a blank line, a permuted header, a trailing tab,
    * and an alternate spelling of one domain each re-seal into a text
    * that parses field for field and is refused as noncanonical.
    */
   expect_parse("source_clean\t1\n", "source_clean\t1\n\n", true,
                R3V_NATIVE_PLAN_PARSE_NONCANONICAL);
   expect_parse("source_sha\t" H40 "\nsource_clean\t1\n",
                "source_clean\t1\nsource_sha\t" H40 "\n", true,
                R3V_NATIVE_PLAN_PARSE_NONCANONICAL);
   expect_parse("\t96\tr\n", "\t96\tr\t\n", true,
                R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE);
   expect_parse("\ttarget\t4\t4\t", "\ttarget\t0x4\t4\t", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("\ttarget\t4\t4\t", "\ttarget\t04\t4\t", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   /* A domain outside the RADEON_GEM_DOMAIN set refuses. */
   expect_parse("\ttarget\t4\t4\t", "\ttarget\t8\t4\t", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("\ttarget\t4\t4\t", "\ttarget\t-1\t4\t", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   /* Ceilings above the schema ceilings refuse. */
   expect_parse("max_ib_dwords\t1024", "max_ib_dwords\t1048577", true,
                R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   expect_parse("max_runtime_seconds\t600", "max_runtime_seconds\t86401",
                true, R3V_NATIVE_PLAN_PARSE_BAD_VALUE);
   /* A declared count the body cannot hold refuses before allocation. */
   expect_parse("submission_count\t2", "submission_count\t65536", true,
                R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT);
   struct r3v_native_plan p;
   assert(parse_text("", 0, &p) == R3V_NATIVE_PLAN_PARSE_NOT_A_PLAN);
   assert(parse_text("x", 1, &p) == R3V_NATIVE_PLAN_PARSE_NOT_A_PLAN);
}

static void
expect_bind(void (*edit)(struct r3v_native_plan_identity *),
            enum r3v_native_plan_bind_result want)
{
   struct r3v_native_plan g = good_plan();
   struct r3v_native_plan_identity id = good_identity();
   edit(&id);
   enum r3v_native_plan_bind_result r = r3v_native_plan_bind(&g, &id);
   if (r != want) {
      fprintf(stderr, "bind: got %s, wanted %s\n",
              r3v_native_plan_bind_result_name(r),
              r3v_native_plan_bind_result_name(want));
      abort();
   }
}

#define EDIT(name, body) \
   static void name(struct r3v_native_plan_identity *id) { body; }
EDIT(e_source, id->source_sha = "1123456789abcdef0123456789abcdef01234567")
EDIT(e_source_null, id->source_sha = NULL)
EDIT(e_dirty, id->source_clean = false)
EDIT(e_dso, id->dso_blake3 = H64B)
EDIT(e_deqp, id->deqp_sha256 = H64B)
EDIT(e_release, id->deqp_release = "opengl-cts-4.6.8.0-410-ga2482928")
EDIT(e_partition, id->partition_sha256 = H64B)
EDIT(e_caselist, id->caselist_sha256 = H64)
EDIT(e_claim, id->queue_claim = R3V_NATIVE_PLAN_QUEUE_EXPERIMENTAL_COMPUTE_SUBSET)
EDIT(e_kernel, id->kernel_release = "7.1.9-1-cachyos")
EDIT(e_module, id->module_srcversion = "0000000000000000000000")
EDIT(e_pci, id->pci_device_id = 0x5975)
EDIT(e_nonce, id->nonce = "ffeeddccbbaa99887766554433221100")
EDIT(e_dir_absent, id->evidence_dir_present = false)
EDIT(e_dir_used, id->evidence_dir_empty = false)
EDIT(e_gate, id->gates_open = true)

static void
test_bind_refusals(void)
{
   expect_bind(e_source, R3V_NATIVE_PLAN_BIND_SOURCE);
   expect_bind(e_source_null, R3V_NATIVE_PLAN_BIND_SOURCE);
   expect_bind(e_dirty, R3V_NATIVE_PLAN_BIND_SOURCE_DIRTY);
   expect_bind(e_dso, R3V_NATIVE_PLAN_BIND_DSO);
   expect_bind(e_deqp, R3V_NATIVE_PLAN_BIND_DEQP);
   expect_bind(e_release, R3V_NATIVE_PLAN_BIND_DEQP_RELEASE);
   expect_bind(e_partition, R3V_NATIVE_PLAN_BIND_PARTITION);
   expect_bind(e_caselist, R3V_NATIVE_PLAN_BIND_CASELIST);
   expect_bind(e_claim, R3V_NATIVE_PLAN_BIND_QUEUE_CLAIM);
   expect_bind(e_kernel, R3V_NATIVE_PLAN_BIND_KERNEL);
   expect_bind(e_module, R3V_NATIVE_PLAN_BIND_MODULE);
   expect_bind(e_pci, R3V_NATIVE_PLAN_BIND_PCI);
   expect_bind(e_nonce, R3V_NATIVE_PLAN_BIND_NONCE);
   expect_bind(e_dir_absent, R3V_NATIVE_PLAN_BIND_EVIDENCE_DIR);
   expect_bind(e_dir_used, R3V_NATIVE_PLAN_BIND_EVIDENCE_DIR);
   expect_bind(e_gate, R3V_NATIVE_PLAN_BIND_GATE_CONTAMINATION);
   /* A plan whose own PCI identity is another chip refuses even against
    * a matching live identity: the plan authorizes RS482 alone.
    */
   struct r3v_native_plan g = good_plan();
   struct r3v_native_plan_identity id = good_identity();
   g.pci_device_id = id.pci_device_id = 0x5975;
   assert(r3v_native_plan_bind(&g, &id) == R3V_NATIVE_PLAN_BIND_PCI);
   g = good_plan();
   g.source_clean = false;
   assert(r3v_native_plan_bind(&g, &id) == R3V_NATIVE_PLAN_BIND_SOURCE_DIRTY);
}

static void
test_match_refusals(void)
{
   struct r3v_native_plan g = good_plan();
   struct r3v_native_plan_submission a = g.submissions[0];
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_OK);
   a.ib_blake3[0] = 'f';
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_DIGEST);
   a = g.submissions[0]; a.ib_dwords++;
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_DWORDS);
   a = g.submissions[0]; a.cell_kind = R3V_NATIVE_CELL_KIND_DIRECT_WRITE;
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_CELL_KIND);
   a = g.submissions[0]; strcpy(a.emitter, "other");
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_EMITTER);
   a = g.submissions[0]; a.reloc_count = 1;
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_RELOC_COUNT);
   a = g.submissions[0]; strcpy(a.relocs[1].role, "vertex");
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_RELOC_ROLE);
   a = g.submissions[0]; a.relocs[0].read_domains = 0x2;
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_RELOC_DOMAINS);
   a = g.submissions[0]; a.relocs[0].size *= 2;
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_RELOC_SIZE);
   a = g.submissions[0]; a.relocs[0].direction = R3V_NATIVE_PLAN_DIRECTION_READ;
   assert(r3v_native_plan_match(&g.submissions[0], &a) ==
          R3V_NATIVE_PLAN_MATCH_RELOC_DIRECTION);
}

static void
test_session(void)
{
   struct r3v_native_plan g = good_plan();
   struct r3v_native_plan_session s;
   r3v_native_plan_session_init(&s);
   /* Unbound refuses admission and finish. */
   assert(r3v_native_plan_session_admit(&s, &g.submissions[0], 1, 0) ==
          R3V_NATIVE_PLAN_SESSION_UNBOUND);
   assert(r3v_native_plan_session_finish(&s, 0) ==
          R3V_NATIVE_PLAN_SESSION_UNBOUND);
   assert(r3v_native_plan_session_bind(&s, &g) ==
          R3V_NATIVE_PLAN_SESSION_ADMITTED);
   /* A second bind on a live session refuses: one plan, one session. */
   assert(r3v_native_plan_session_bind(&s, &g) ==
          R3V_NATIVE_PLAN_SESSION_CONSUMED);
   /* The ordered replay admits each entry once and proves exhaustion. */
   assert(r3v_native_plan_session_admit(&s, &g.submissions[0], 1, 1) ==
          R3V_NATIVE_PLAN_SESSION_ADMITTED);
   assert(r3v_native_plan_session_finish(&s, 1) ==
          R3V_NATIVE_PLAN_SESSION_INCOMPLETE);
   /* An incomplete finish is itself terminal. */
   assert(s.terminal);
   assert(r3v_native_plan_session_admit(&s, &g.submissions[1], 1, 2) ==
          R3V_NATIVE_PLAN_SESSION_TERMINAL);

   /* Each defect against a fresh session latches, and the latch holds. */
   struct {
      const char *name;
      uint32_t first_index;
      uint32_t executable;
      uint64_t elapsed;
      enum r3v_native_plan_session_result want;
   } arms[] = {
      {"reordered", 1, 1, 0, R3V_NATIVE_PLAN_SESSION_MISMATCH},
      {"two executable buffers", 0, 2, 0,
       R3V_NATIVE_PLAN_SESSION_EXECUTABLE_COUNT},
      {"zero executable buffers", 0, 0, 0,
       R3V_NATIVE_PLAN_SESSION_EXECUTABLE_COUNT},
      {"deadline", 0, 1, 601, R3V_NATIVE_PLAN_SESSION_RUNTIME_EXCEEDED},
   };
   for (unsigned i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
      struct r3v_native_plan_session t;
      r3v_native_plan_session_init(&t);
      assert(r3v_native_plan_session_bind(&t, &g) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      enum r3v_native_plan_session_result r = r3v_native_plan_session_admit(
         &t, &g.submissions[arms[i].first_index], arms[i].executable,
         arms[i].elapsed);
      if (r != arms[i].want) {
         fprintf(stderr, "%s: got %s\n", arms[i].name,
                 r3v_native_plan_session_result_name(r));
         abort();
      }
      assert(t.terminal && t.next_index == 0);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_TERMINAL);
      assert(r3v_native_plan_session_finish(&t, 0) ==
             R3V_NATIVE_PLAN_SESSION_TERMINAL);
      assert(r3v_native_plan_session_bind(&t, &g) ==
             R3V_NATIVE_PLAN_SESSION_CONSUMED);
   }
   /* Duplicated submission: the first entry twice mismatches the second. */
   {
      struct r3v_native_plan_session t;
      r3v_native_plan_session_init(&t);
      r3v_native_plan_session_bind(&t, &g);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_MISMATCH);
      assert(t.last_mismatch == R3V_NATIVE_PLAN_MATCH_DIGEST);
   }
   /* Extra submission after the plan is exhausted. */
   {
      struct r3v_native_plan_session t;
      r3v_native_plan_session_init(&t);
      r3v_native_plan_session_bind(&t, &g);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[1], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[1], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_EXHAUSTED);
      assert(t.terminal);
      assert(r3v_native_plan_session_finish(&t, 0) ==
             R3V_NATIVE_PLAN_SESSION_TERMINAL);
   }
   /* The complete replay finishes once and refuses reuse. */
   {
      struct r3v_native_plan_session t;
      r3v_native_plan_session_init(&t);
      r3v_native_plan_session_bind(&t, &g);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[1], 1, 5) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(t.referenced_bytes == 16384 + 4 + 96);
      assert(r3v_native_plan_session_finish(&t, 5) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_finish(&t, 5) ==
             R3V_NATIVE_PLAN_SESSION_CONSUMED);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_TERMINAL);
      assert(r3v_native_plan_session_bind(&t, &g) ==
             R3V_NATIVE_PLAN_SESSION_CONSUMED);
   }
   /* Failure after admission latches; continuation refuses. */
   {
      struct r3v_native_plan_session t;
      r3v_native_plan_session_init(&t);
      r3v_native_plan_session_bind(&t, &g);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      r3v_native_plan_session_fail(&t, R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(t.terminal &&
             t.terminal_reason == R3V_NATIVE_PLAN_SESSION_TERMINAL);
      assert(r3v_native_plan_session_admit(&t, &g.submissions[1], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_TERMINAL);
   }
   /* Byte ceiling: a plan whose entries sum past the ceiling refuses at
    * the entry that crosses it.
    */
   {
      struct r3v_native_plan small = good_plan();
      small.max_cumulative_bytes = 16384 + 4 + 10;
      struct r3v_native_plan_session t;
      r3v_native_plan_session_init(&t);
      r3v_native_plan_session_bind(&t, &small);
      assert(r3v_native_plan_session_admit(&t, &small.submissions[0], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_admit(&t, &small.submissions[1], 1, 0) ==
             R3V_NATIVE_PLAN_SESSION_BYTES_EXCEEDED);
   }
   /* The final time check catches a session that crossed its deadline
    * after its last admission.
    */
   {
      struct r3v_native_plan deadline = good_plan();
      deadline.max_runtime_seconds = 5;
      struct r3v_native_plan_session t;
      r3v_native_plan_session_init(&t);
      assert(r3v_native_plan_session_bind(&t, &deadline) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_admit(&t, &deadline.submissions[0], 1, 4) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_admit(&t, &deadline.submissions[1], 1, 5) ==
             R3V_NATIVE_PLAN_SESSION_ADMITTED);
      assert(r3v_native_plan_session_finish(&t, 6) ==
             R3V_NATIVE_PLAN_SESSION_RUNTIME_EXCEEDED);
   }
}

/* The writer holds every field to the parser's predicates, so a plan it
 * refuses is one the parser would refuse; each single defect refuses.
 */
static void
test_writer_refuses_out_of_schema(void)
{
   struct r3v_native_plan g = good_plan();
   g.submission_count = 5;
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   g.submissions[0].cell_kind = R3V_NATIVE_CELL_KIND_UNDECLARED;
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   g.submissions[0].relocs[0].direction = 7;
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   g.queue_claim = 9;
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   g.max_cumulative_bytes = 0;
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   g.dso_blake3[3] = 'z';
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   strcpy(g.submissions[0].emitter, "r3\tv");
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   strcpy(g.evidence_dir, "relative/dir");
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   g.pci_vendor_id = 0x10000;
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   g = good_plan();
   g.submissions[0].relocs[0].read_domains = 0x8;
   assert(r3v_native_plan_write(&g, NULL, 0) == -1);
   /* A buffer too small for the whole plan stays untouched. */
   g = good_plan();
   char tiny[64];
   memset(tiny, '#', sizeof(tiny));
   long need = r3v_native_plan_write(&g, tiny, sizeof(tiny));
   assert(need > 64);
   for (unsigned i = 0; i < sizeof(tiny); i++)
      assert(tiny[i] == '#');
}

static void
test_seal_refuses_out_of_range_bodies(void)
{
   char text[128] = {0};
   assert(r3v_native_plan_seal(text, sizeof(text), sizeof(text)) == 0);
   assert(r3v_native_plan_seal(text, sizeof(text), SIZE_MAX) == 0);
}

/* The gate enumeration names any open gate: the hazard gate, an
 * authorization value, an R2VB route gate, the compute queue gate, and
 * a compute verb gate each open it; an empty value stays closed.
 */
static const char *fake_env_name;
static const char *fake_env_value;

static const char *
fake_read_env(void *ctx, const char *name)
{
   (void)ctx;
   return strcmp(name, fake_env_name) == 0 ? fake_env_value : NULL;
}

static void
test_gates_open(void)
{
   const char *gate = "x";
   fake_env_name = "R3V_NATIVE_NOTHING";
   fake_env_value = "1";
   assert(!r3v_native_plan_gates_open(fake_read_env, NULL, &gate) &&
          gate == NULL);
   static const char *const open[] = {
      "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
      "R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS",
      "R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL",
      "R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL",
      "R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL",
   };
   for (unsigned i = 0; i < sizeof(open) / sizeof(open[0]); i++) {
      fake_env_name = open[i];
      fake_env_value = "0";
      assert(r3v_native_plan_gates_open(fake_read_env, NULL, &gate) &&
             strcmp(gate, open[i]) == 0);
      fake_env_value = "";
      assert(!r3v_native_plan_gates_open(fake_read_env, NULL, &gate));
   }
}

int
main(void)
{
   test_round_trip();
   test_parse_refusals();
   test_bind_refusals();
   test_match_refusals();
   test_session();
   test_writer_refuses_out_of_schema();
   test_seal_refuses_out_of_range_bodies();
   test_gates_open();
   printf("r3v-native-plan: round trip, seal, truncation, order, count, "
          "field, value, and ceiling refusals; sixteen identity binds; "
          "nine entry mismatches; ordered replay to exhaustion with "
          "reordered, duplicated, extra, missing, multi-buffer, deadline, "
          "byte-ceiling, post-failure, and reuse refusals; writer field "
          "refusals and untouched short buffer; gate enumeration\n");
   return 0;
}
