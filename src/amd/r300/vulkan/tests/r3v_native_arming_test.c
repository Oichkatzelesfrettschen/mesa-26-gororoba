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

static struct r3v_native_arming_facts
armed_facts(void)
{
   return (struct r3v_native_arming_facts){
      .hazard_gate = "1",
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
   char dir[] = "/tmp/r3v-arming-XXXXXX";
   assert(mkdtemp(dir) != NULL);

   char kernel[128];
   char module[128];
   struct r3v_native_arming_facts facts;
   r3v_native_arming_collect(&facts, R3V_NATIVE_ARMING_PCI_VENDOR,
                             R3V_NATIVE_ARMING_PCI_DEVICE,
                             authorized_digest, dir, kernel, sizeof(kernel),
                             module, sizeof(module));
   assert(facts.evidence_dir_present);
   assert(!facts.attempt_token_present);
   /* An unreadable radeon srcversion reports the literal "none" rather
    * than an empty fact that an unset declaration could match.
    */
   assert(facts.running_module_srcversion != NULL &&
          facts.running_module_srcversion[0] != '\0');
   assert(facts.running_kernel_release != NULL &&
          facts.running_kernel_release[0] != '\0');

   assert(r3v_native_arming_disarm(dir, "0123456789abcdef") == 0);
   assert(r3v_native_arming_disarm(dir, "0123456789abcdef") != 0);

   /* The token binds the attempt to the declared digest and the wall-clock
    * instant, so a post-incident read of the directory names what was
    * armed and when.
    */
   {
      char token_path[1024];
      snprintf(token_path, sizeof(token_path), "%s/attempt.token", dir);
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
                             authorized_digest, dir, kernel, sizeof(kernel),
                             module, sizeof(module));
   assert(facts.attempt_token_present);

   assert(r3v_native_arming_disarm(NULL, "x") != 0);
   assert(r3v_native_arming_disarm("", "x") != 0);

   char token[1024];
   snprintf(token, sizeof(token), "%s/attempt.token", dir);
   remove(token);
   rmdir(dir);

   /* A directory that no longer exists refuses on the evidence factor. */
   r3v_native_arming_collect(&facts, R3V_NATIVE_ARMING_PCI_VENDOR,
                             R3V_NATIVE_ARMING_PCI_DEVICE,
                             authorized_digest, dir, kernel, sizeof(kernel),
                             module, sizeof(module));
   assert(!facts.evidence_dir_present);
}

static void
test_verdict_names_are_distinct(void)
{
   const char *names[R3V_NATIVE_ARMING_ALREADY_ATTEMPTED + 1];
   for (int i = 0; i <= R3V_NATIVE_ARMING_ALREADY_ATTEMPTED; i++) {
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
   test_disarm_is_one_shot();
   test_verdict_names_are_distinct();
   printf("r3v_native_arming_test: all checks passed\n");
   return 0;
}
