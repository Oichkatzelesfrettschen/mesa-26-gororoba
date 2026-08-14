/*
 * SPDX-License-Identifier: MIT
 *
 * Host checks for the producer-only cell surface: the installed stream is
 * the reference producer pass byte for byte, the carrier relocation
 * carries both GTT domains, the carrier oracle refuses a poison that
 * collides with a delivered dword, and the arming gate refuses a cell
 * kind with no frozen geometry.  The command buffer is stack storage:
 * installation and emission touch no device, so the checks run with no
 * DRM node present.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <errno.h>
#include <radeon_drm.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The BO handle the fixture binds; installation records the handle and
 * reads no kernel object.
 */
#define FIXTURE_CARRIER_HANDLE 0x51u

static void
test_installed_stream_is_the_reference_pass(void)
{
   struct r300_r2vb_producer_ib reference;
   assert(r300_r2vb_producer_reference_emit(&reference) == 0);
   char reference_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);

   uint32_t carrier_bytes = 0;
   assert(r3v_native_producer_carrier_bytes(&carrier_bytes) == 0);

   struct r3v_native_memory carrier = {
      .bo = {
         .handle = FIXTURE_CARRIER_HANDLE,
         .size = carrier_bytes,
      },
   };
   struct r3v_native_cmd_buffer cmd_buffer = {0};
   assert(r3v_native_producer_cell_install(&cmd_buffer, &carrier) == 0);

   assert(cmd_buffer.cell_kind == R3V_NATIVE_CELL_KIND_R2VB_PRODUCER);
   assert(cmd_buffer.ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(cmd_buffer.ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   char installed_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cmd_buffer.ib, cmd_buffer.ib_size_dwords,
                               installed_digest);
   assert(strcmp(installed_digest, reference_digest) == 0);

   /* The carrier crosses the color backend and the vertex fetch, so its
    * one relocation carries the GTT domain in both directions; a kernel
    * validating a write-only carrier rejects the consuming fetch.
    */
   assert(cmd_buffer.reference_count == R300_R2VB_PRODUCER_SLOT_COUNT);
   const struct r3v_native_bo_reference *slot =
      &cmd_buffer.references[R300_R2VB_PRODUCER_SLOT_CARRIER];
   assert(slot->handle == FIXTURE_CARRIER_HANDLE);
   assert(slot->read_domains == RADEON_GEM_DOMAIN_GTT);
   assert(slot->write_domain == RADEON_GEM_DOMAIN_GTT);
   assert(slot->memory == &carrier);

   free(cmd_buffer.ib);
   free(cmd_buffer.references);
   r300_r2vb_producer_pass_release(&reference);
}

static void
test_carrier_oracle_poison_contract(void)
{
   uint32_t carrier_bytes = 0;
   assert(r3v_native_producer_carrier_bytes(&carrier_bytes) == 0);

   uint32_t expected[R300_R2VB_PRODUCER_REFERENCE_COUNT * 4];
   assert(r300_r2vb_producer_reference_expected(
             expected, (uint32_t)(sizeof(expected) / sizeof(expected[0]))) ==
          0);

   const uint32_t expected_dwords =
      (uint32_t)(sizeof(expected) / sizeof(expected[0]));
   const uint32_t carrier_dwords = carrier_bytes / 4;
   uint32_t carrier[64];
   assert(carrier_dwords <= sizeof(carrier) / sizeof(carrier[0]));

   /* The delivered extent plus a poisoned tail is the passing read-back:
    * every written slot matches and the padding slot still holds poison.
    */
   for (uint32_t i = 0; i < carrier_dwords; i++)
      carrier[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   memcpy(carrier, expected, sizeof(expected));
   struct r300_r2vb_producer_carrier_verdict verdict;
   assert(r300_r2vb_producer_carrier_check(
             expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
             carrier, carrier_bytes, &verdict) == 0);
   assert(verdict.expected_pass && verdict.tail_poison_pass);
   assert(verdict.mismatched_dwords == 0 &&
          verdict.disturbed_tail_dwords == 0);

   /* An unwritten carrier fails on both sides, so the check has a
    * decidable negative.
    */
   for (uint32_t i = 0; i < carrier_dwords; i++)
      carrier[i] = R300_R2VB_PRODUCER_POISON_DWORD;
   assert(r300_r2vb_producer_carrier_check(
             expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
             carrier, carrier_bytes, &verdict) == 0);
   assert(!verdict.expected_pass);
   assert(verdict.mismatched_dwords == expected_dwords);

   /* A disturbed tail dword is an out-of-extent device write. */
   memcpy(carrier, expected, sizeof(expected));
   carrier[carrier_dwords - 1] = 0u;
   assert(r300_r2vb_producer_carrier_check(
             expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
             carrier, carrier_bytes, &verdict) == 0);
   assert(verdict.expected_pass);
   assert(!verdict.tail_poison_pass && verdict.disturbed_tail_dwords == 1);

   /* A poison equal to a delivered dword makes the unwritten case read as
    * delivered, so the pairing refuses before the carrier is read.
    */
   assert(r300_r2vb_producer_carrier_check(expected, expected_dwords,
                                           expected[0], carrier,
                                           carrier_bytes, &verdict) ==
          -EINVAL);
   /* A carrier shorter than the delivered extent has no verdict. */
   assert(r300_r2vb_producer_carrier_check(
             expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
             carrier, (expected_dwords - 1) * 4, &verdict) == -EINVAL);
}

static void
test_unknown_cell_kind_refuses_arming(void)
{
   static const char digest[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   struct r3v_native_arming_facts facts = {
      .hazard_gate = "1",
      .cell_kind = R3V_NATIVE_CELL_KIND_R2VB_PRODUCER,
      .authorized_ib_blake3 = digest,
      .actual_ib_blake3 = digest,
      .pci_vendor_id = R3V_NATIVE_ARMING_PCI_VENDOR,
      .pci_device_id = R3V_NATIVE_ARMING_PCI_DEVICE,
      .authorized_kernel_release = "7.1.3-2-cachyos",
      .running_kernel_release = "7.1.3-2-cachyos",
      .authorized_module_srcversion = "EA8E3BBBBA9E5580BDA7553",
      .running_module_srcversion = "EA8E3BBBBA9E5580BDA7553",
      .evidence_dir_present = true,
   };
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);

   facts.cell_kind = R3V_NATIVE_CELL_KIND_UNDECLARED;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_UNKNOWN_CELL_KIND);
}

int
main(void)
{
   test_installed_stream_is_the_reference_pass();
   test_carrier_oracle_poison_contract();
   test_unknown_cell_kind_refuses_arming();
   printf("r3v-native-producer-cell: ok\n");
   return 0;
}
