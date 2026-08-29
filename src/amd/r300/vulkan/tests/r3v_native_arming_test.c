/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration for the native submission arming gate: the armed fact set
 * arms, and each single-factor defect refuses with its own verdict.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include "r3v_native_arming.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char authorized_digest[] =
   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/* Join a directory and a complete child suffix before any filesystem call.
 * The capacity check includes the terminator, so a token path never names a
 * truncated child. */
static bool
join_path(char *path, size_t capacity, const char *directory,
          const char *suffix)
{
   if (path == NULL || directory == NULL || suffix == NULL || capacity == 0)
      return false;

   size_t directory_length = strlen(directory);
   size_t suffix_length = strlen(suffix);
   if (directory_length >= capacity ||
       suffix_length > capacity - directory_length - 1)
      return false;

   memcpy(path, directory, directory_length);
   memcpy(path + directory_length, suffix, suffix_length + 1);
   return true;
}

static struct r3v_native_arming_facts
armed_facts(void)
{
   return (struct r3v_native_arming_facts){
      .hazard_gate = "1",
      .cell_kind = R3V_NATIVE_CELL_KIND_TRIANGLE,
      .authorized_ib_blake3 = authorized_digest,
      .actual_ib_blake3 = authorized_digest,
      .pci_vendor_id = R3V_NATIVE_ARMING_PCI_VENDOR,
      .pci_device_id = R3V_NATIVE_ARMING_PCI_DEVICE,
      .authorized_kernel_release = "7.1.3-2-cachyos",
      .running_kernel_release = "7.1.3-2-cachyos",
      .authorized_module_srcversion = "EA8E3BBBBA9E5580BDA7553",
      .running_module_srcversion = "EA8E3BBBBA9E5580BDA7553",
      .evidence_dir_present = true,
      .attempt_token_present = false,
   };
}

static void
test_complete_fact_set_arms(void)
{
   struct r3v_native_arming_facts facts = armed_facts();
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);
   assert(r3v_native_arming_evaluate(NULL) ==
          R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED);
}

static void
test_each_factor_refuses(void)
{
   struct r3v_native_arming_facts facts;

   /* The hazard gate takes the exact value; near-misses stay closed. */
   const char *closed[] = {NULL, "", "0", "true", "11", " 1"};
   for (unsigned i = 0; i < sizeof(closed) / sizeof(closed[0]); i++) {
      facts = armed_facts();
      facts.hazard_gate = closed[i];
      assert(r3v_native_arming_evaluate(&facts) ==
             R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED);
   }

   facts = armed_facts();
   facts.authorized_ib_blake3 = NULL;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_BUNDLE_UNDECLARED);

   /* A rebuilt or edited cell differs by digest and refuses. */
   facts = armed_facts();
   facts.actual_ib_blake3 =
      "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_BUNDLE_MISMATCH);
   facts = armed_facts();
   facts.actual_ib_blake3 = NULL;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_BUNDLE_MISMATCH);

   facts = armed_facts();
   facts.nonmaximum_extent = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_NONMAXIMUM_EXTENT);

   /* Each recorded kind carries its own frozen geometry and arms; an
    * undeclared or out-of-set kind has none and refuses ahead of the
    * extent factor, so an unrecognized cell cannot ride a false extent
    * fact into the ioctl.
    */
   const enum r3v_native_cell_kind kinds[] = {
      R3V_NATIVE_CELL_KIND_TRIANGLE,
      R3V_NATIVE_CELL_KIND_DIRECT_WRITE,
      R3V_NATIVE_CELL_KIND_R2VB_PRODUCER,
   };
   for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
      facts = armed_facts();
      facts.cell_kind = kinds[i];
      assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);
   }
   facts = armed_facts();
   facts.cell_kind = R3V_NATIVE_CELL_KIND_UNDECLARED;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_UNKNOWN_CELL_KIND);
   /* The kind check precedes the extent factor, so an undeclared kind
    * refuses by kind even while the extent fact reads unfrozen.
    */
   facts = armed_facts();
   facts.cell_kind = R3V_NATIVE_CELL_KIND_UNDECLARED;
   facts.nonmaximum_extent = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_UNKNOWN_CELL_KIND);

   /* RS485-marketed 0x5975 is a supported r3v identity but not the
    * authorized attended-run chip.
    */
   facts = armed_facts();
   facts.pci_device_id = 0x5975;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_CHIP_MISMATCH);
   facts = armed_facts();
   facts.pci_vendor_id = 0x10de;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_CHIP_MISMATCH);

   facts = armed_facts();
   facts.authorized_kernel_release = NULL;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_KERNEL_UNDECLARED);
   facts = armed_facts();
   facts.running_kernel_release = "6.18.38-2-cachyos-lts";
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_KERNEL_MISMATCH);

   facts = armed_facts();
   facts.authorized_module_srcversion = NULL;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_MODULE_UNDECLARED);

   /* The module transition this evidence lane observed: an authorization
    * naming the earlier srcversion refuses against the running one.
    */
   facts = armed_facts();
   facts.authorized_module_srcversion = "5834C69D07FBB06F5FB924E";
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_MODULE_MISMATCH);
   facts = armed_facts();
   facts.running_module_srcversion = "none";
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_MODULE_MISMATCH);

   facts = armed_facts();
   facts.evidence_dir_present = false;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_EVIDENCE_ABSENT);

   facts = armed_facts();
   facts.attempt_token_present = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_ALREADY_ATTEMPTED);
}

/* The disarm is exclusive creation, so the second call over the same
 * directory fails and the collected facts then refuse.
 */
static void
test_disarm_is_one_shot(void)
{
   const char *temp_root = getenv("TMPDIR");
   if (!temp_root || !temp_root[0])
      temp_root = ".";

   char dir[R3V_NATIVE_ARMING_PATH_MAX];
   assert(join_path(dir, sizeof(dir), temp_root, "/r3v-arming-XXXXXX"));
   assert(mkdtemp(dir) != NULL);

   char kernel[128];
   char module[128];
   struct r3v_native_arming_facts facts;
   r3v_native_arming_collect(&facts, R3V_NATIVE_ARMING_PCI_VENDOR,
                             R3V_NATIVE_ARMING_PCI_DEVICE,
                             R3V_NATIVE_CELL_KIND_TRIANGLE,
                             authorized_digest, dir, kernel, sizeof(kernel),
                             module, sizeof(module));
   assert(facts.evidence_dir_present);
   assert(!facts.attempt_token_present);
   assert(facts.running_kernel_release != NULL &&
          facts.running_kernel_release[0] != '\0');

   assert(r3v_native_arming_disarm(dir, "0123456789abcdef") == 0);
   assert(r3v_native_arming_disarm(dir, "0123456789abcdef") != 0);

   /* The token binds the attempt to the declared digest and the wall-clock
    * instant, so a post-incident read of the directory names what was
    * armed and when.
    */
   {
      char token_path[R3V_NATIVE_ARMING_PATH_MAX];
      assert(join_path(token_path, sizeof(token_path), dir,
                       "/attempt.token"));
      FILE *token_file = fopen(token_path, "r");
      assert(token_file != NULL);
      char contents[512] = "";
      size_t got = fread(contents, 1, sizeof(contents) - 1, token_file);
      contents[got] = '\0';
      fclose(token_file);
      assert(strstr(contents,
                    "declared_ib_blake3: 0123456789abcdef") != NULL);
      assert(strstr(contents, "unix_time: ") != NULL);
   }

   r3v_native_arming_collect(&facts, R3V_NATIVE_ARMING_PCI_VENDOR,
                             R3V_NATIVE_ARMING_PCI_DEVICE,
                             R3V_NATIVE_CELL_KIND_TRIANGLE,
                             authorized_digest, dir, kernel, sizeof(kernel),
                             module, sizeof(module));
   assert(facts.attempt_token_present);

   assert(r3v_native_arming_disarm(NULL, "x") != 0);
   assert(r3v_native_arming_disarm("", "x") != 0);

   char token[R3V_NATIVE_ARMING_PATH_MAX];
   assert(join_path(token, sizeof(token), dir, "/attempt.token"));
   remove(token);
   rmdir(dir);

   /* A directory that no longer exists refuses on the evidence factor. */
   r3v_native_arming_collect(&facts, R3V_NATIVE_ARMING_PCI_VENDOR,
                             R3V_NATIVE_ARMING_PCI_DEVICE,
                             R3V_NATIVE_CELL_KIND_TRIANGLE,
                             authorized_digest, dir, kernel, sizeof(kernel),
                             module, sizeof(module));
   assert(!facts.evidence_dir_present);
}

/* The directory template and its token child share one capacity.  The
 * boundary fixture admits the largest root that fits both names and rejects
 * the next root byte before mkdtemp can create an unaddressable directory. */
static void
test_attempt_token_path_budget(void)
{
   char root[R3V_NATIVE_ARMING_PATH_MAX];
   char directory[R3V_NATIVE_ARMING_PATH_MAX];
   char token[R3V_NATIVE_ARMING_PATH_MAX];
   const size_t directory_suffix_length = strlen("/r3v-arming-XXXXXX");
   const size_t token_suffix_length = strlen("/attempt.token");
   const size_t maximum_root_length =
      sizeof(root) - 1 - directory_suffix_length - token_suffix_length;

   memset(root, 'r', maximum_root_length);
   root[maximum_root_length] = '\0';
   assert(join_path(directory, sizeof(directory), root,
                    "/r3v-arming-XXXXXX"));
   assert(join_path(token, sizeof(token), directory, "/attempt.token"));

   root[maximum_root_length] = 'r';
   root[maximum_root_length + 1] = '\0';
   assert(join_path(directory, sizeof(directory), root,
                    "/r3v-arming-XXXXXX"));
   assert(!join_path(token, sizeof(token), directory, "/attempt.token"));
}

/* The serial kind's predicate over the same fact set: the bound must be
 * declared inside 1..64, each admission counts against it, and the token
 * pairs with the instance that wrote it -- a foreign token, a missing
 * token mid-run, and an exhausted or undeclared bound each refuse.
 */
static void
test_serial_bound_predicate(void)
{
   struct r3v_native_arming_facts facts = armed_facts();
   facts.cell_kind = R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL;

   /* Undeclared and out-of-range bounds refuse before any admission. */
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_SERIAL_BOUND_UNDECLARED);
   facts.serial_authorized_submissions =
      R3V_NATIVE_ARMING_SERIAL_MAX_SUBMISSIONS + 1;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_SERIAL_BOUND_UNDECLARED);

   /* First admission: no token, none consumed. */
   facts.serial_authorized_submissions = 2;
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);

   /* A token this instance did not write is another run's disarm. */
   facts.attempt_token_present = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_ALREADY_ATTEMPTED);

   /* Continuation under the instance's own token stays inside the bound. */
   facts.serial_submissions_consumed = 1;
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);
   facts.serial_submissions_consumed = 2;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_SERIAL_BOUND_EXHAUSTED);

   /* A consumed count without the token means the token vanished mid-run. */
   facts.serial_submissions_consumed = 1;
   facts.attempt_token_present = false;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_SERIAL_CONTINUITY_BROKEN);

   /* The one-shot kinds ignore serial authority: a declared bound
    * weakens nothing outside the serial kind.
    */
   facts = armed_facts();
   facts.serial_authorized_submissions = 64;
   facts.attempt_token_present = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_ALREADY_ATTEMPTED);
}

/* The env spelling parses as an exact decimal: 1..64 admits, and empty,
 * zero, over-bound, sign, leading-zero, and trailing-byte spellings all
 * collect as undeclared.
 */
static void
test_serial_env_parse(void)
{
   static const struct {
      const char *spelling;
      uint32_t parsed;
   } cases[] = {
      { "1", 1 },   { "64", 64 }, { "0", 0 },    { "65", 0 },
      { "007", 0 }, { "+4", 0 },  { "-1", 0 },   { "16x", 0 },
      { "", 0 },    { " 8", 0 },  { "640", 0 },  { "8 ", 0 },
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      assert(setenv("R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS",
                    cases[i].spelling, 1) == 0);
      char kernel[128];
      char module[128];
      struct r3v_native_arming_facts facts;
      r3v_native_arming_collect(
         &facts, R3V_NATIVE_ARMING_PCI_VENDOR, R3V_NATIVE_ARMING_PCI_DEVICE,
         R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL, authorized_digest,
         NULL, kernel, sizeof(kernel), module, sizeof(module));
      assert(facts.serial_authorized_submissions == cases[i].parsed);
   }
   unsetenv("R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS");
}

/* The burst kind's predicate: the declared member count must sit inside
 * 1..64 and equal the recorded composition depth, on top of the
 * one-shot token; a declared burst count weakens nothing outside the
 * burst kind.
 */
static void
test_burst_draws_predicate(void)
{
   struct r3v_native_arming_facts facts = armed_facts();
   facts.cell_kind = R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST;

   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_BURST_DRAWS_UNDECLARED);
   facts.burst_authorized_draws = R3V_NATIVE_ARMING_BURST_MAX_DRAWS + 1;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_BURST_DRAWS_UNDECLARED);

   facts.burst_authorized_draws = 32;
   facts.burst_recorded_draws = 16;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_BURST_DRAWS_MISMATCH);
   facts.burst_recorded_draws = 32;
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);

   /* The burst keeps the one-shot token semantics. */
   facts.attempt_token_present = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_ALREADY_ATTEMPTED);

   /* Outside the burst kind, burst authority changes nothing. */
   facts = armed_facts();
   facts.burst_authorized_draws = 8;
   facts.burst_recorded_draws = 0;
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);
}

/* The burst env shares the serial bound's exact-decimal spelling. */
static void
test_burst_env_parse(void)
{
   static const struct {
      const char *spelling;
      uint32_t parsed;
   } cases[] = {
      { "1", 1 },   { "64", 64 }, { "0", 0 },   { "65", 0 },
      { "007", 0 }, { "+4", 0 },  { "32x", 0 }, { "", 0 },
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      assert(setenv("R3V_NATIVE_AUTHORIZED_BURST_DRAWS",
                    cases[i].spelling, 1) == 0);
      char kernel[128];
      char module[128];
      struct r3v_native_arming_facts facts;
      r3v_native_arming_collect(
         &facts, R3V_NATIVE_ARMING_PCI_VENDOR, R3V_NATIVE_ARMING_PCI_DEVICE,
         R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST, authorized_digest,
         NULL, kernel, sizeof(kernel), module, sizeof(module));
      assert(facts.burst_authorized_draws == cases[i].parsed);
   }
   unsetenv("R3V_NATIVE_AUTHORIZED_BURST_DRAWS");
}

static void
test_verdict_names_are_distinct(void)
{
   const char *names[R3V_NATIVE_ARMING_BURST_DRAWS_MISMATCH + 1];
   for (int i = 0; i <= R3V_NATIVE_ARMING_BURST_DRAWS_MISMATCH; i++) {
      names[i] = r3v_native_arming_verdict_name(
         (enum r3v_native_arming_verdict)i);
      assert(names[i] != NULL && names[i][0] != '\0');
      for (int j = 0; j < i; j++)
         assert(strcmp(names[i], names[j]) != 0);
   }
}

int
main(void)
{
   test_complete_fact_set_arms();
   test_each_factor_refuses();
   test_attempt_token_path_budget();
   test_disarm_is_one_shot();
   test_serial_bound_predicate();
   test_serial_env_parse();
   test_burst_draws_predicate();
   test_burst_env_parse();
   test_verdict_names_are_distinct();
   printf("r3v_native_arming_test: all checks passed\n");
   return 0;
}
