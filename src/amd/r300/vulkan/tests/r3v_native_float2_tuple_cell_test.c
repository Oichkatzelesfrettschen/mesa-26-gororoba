/*
 * SPDX-License-Identifier: MIT
 *
 * Host checks for the fetched FLOAT_4 + FLOAT_2 tuple cell surface: the
 * installed stream is the reference tuple pass byte for byte, the
 * carrier relocation carries both GTT domains while the vertex
 * relocation is read-only, and the arming gate admits the tuple kind.
 * The command buffer is stack storage: installation and emission touch
 * no device, so the checks run with no DRM node present.
 */

#undef NDEBUG

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <errno.h>
#include <radeon_drm.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The BO handles the fixture binds; installation records the handles and
 * reads no kernel object.
 */
#define FIXTURE_CARRIER_HANDLE 0x61u
#define FIXTURE_VERTEX_HANDLE 0x62u

#define FIXTURE_VERTEX_BYTES                    \
   (R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *    \
    (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES + \
     R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES))

static void
test_installed_stream_is_the_reference_pass(void)
{
   struct r300_r2vb_float2_tuple_ib reference;
   assert(r300_r2vb_float2_tuple_reference_emit(&reference) == 0);
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
   struct r3v_native_memory vertex = {
      .bo = {
         .handle = FIXTURE_VERTEX_HANDLE,
         .size = FIXTURE_VERTEX_BYTES,
      },
   };
   struct r3v_native_cmd_buffer cmd_buffer = {0};
   assert(r3v_native_float2_tuple_cell_install(&cmd_buffer, &carrier,
                                               &vertex) == 0);

   assert(cmd_buffer.cell_kind == R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE);
   assert(cmd_buffer.ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(cmd_buffer.ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   char installed_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cmd_buffer.ib, cmd_buffer.ib_size_dwords,
                               installed_digest);
   assert(strcmp(installed_digest, reference_digest) == 0);

   /* The carrier crosses the color backend and a later consumer's fetch,
    * so its relocation carries the GTT domain in both directions; the
    * vertex BO feeds the two LOAD_VBPNTR arrays and is device-read
    * alone, so a kernel validating a written fetch source rejects it.
    */
   assert(cmd_buffer.reference_count == R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT);
   const struct r3v_native_bo_reference *carrier_slot =
      &cmd_buffer.references[R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER];
   assert(carrier_slot->handle == FIXTURE_CARRIER_HANDLE);
   assert(carrier_slot->read_domains == RADEON_GEM_DOMAIN_GTT);
   assert(carrier_slot->write_domain == RADEON_GEM_DOMAIN_GTT);
   assert(carrier_slot->memory == &carrier);
   const struct r3v_native_bo_reference *vertex_slot =
      &cmd_buffer.references[R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX];
   assert(vertex_slot->handle == FIXTURE_VERTEX_HANDLE);
   assert(vertex_slot->read_domains == RADEON_GEM_DOMAIN_GTT);
   assert(vertex_slot->write_domain == 0);
   assert(vertex_slot->memory == &vertex);

   free(cmd_buffer.ib);
   free(cmd_buffer.references);
   r300_r2vb_float2_tuple_pass_release(&reference);
}

static void
test_tuple_kind_arms_and_undeclared_refuses(void)
{
   static const char digest[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   struct r3v_native_arming_facts facts = {
      .hazard_gate = "1",
      .cell_kind = R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE,
      .authorized_ib_blake3 = digest,
      .actual_ib_blake3 = digest,
      .pci_vendor_id = R3V_NATIVE_ARMING_PCI_VENDOR,
      .pci_device_id = R3V_NATIVE_ARMING_PCI_DEVICE,
      .authorized_kernel_release = "7.1.3-2-cachyos",
      .running_kernel_release = "7.1.3-2-cachyos",
      .authorized_module_srcversion = "A7F72BE636B52D7EED42415",
      .running_module_srcversion = "A7F72BE636B52D7EED42415",
      .evidence_dir_present = true,
   };
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);

   /* The tuple kind carries its own frozen geometry; a recorded geometry
    * deviation still refuses through the extent factor.
    */
   facts.nonmaximum_extent = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_NONMAXIMUM_EXTENT);
   facts.nonmaximum_extent = false;

   facts.cell_kind = R3V_NATIVE_CELL_KIND_UNDECLARED;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_UNKNOWN_CELL_KIND);

   /* The serial status-load kind rides the same frozen stream and
    * geometry contract; its own factor is the declared submission bound,
    * and the extent factor still refuses a deviating recording.
    */
   facts.cell_kind = R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_SERIAL_BOUND_UNDECLARED);
   facts.serial_authorized_submissions = 8;
   assert(r3v_native_arming_evaluate(&facts) == R3V_NATIVE_ARMING_ARMED);
   facts.nonmaximum_extent = true;
   assert(r3v_native_arming_evaluate(&facts) ==
          R3V_NATIVE_ARMING_NONMAXIMUM_EXTENT);
   facts.nonmaximum_extent = false;
   facts.serial_authorized_submissions = 0;
}

static void
test_vertex_stream_capacity_contract(void)
{
   uint8_t bytes[FIXTURE_VERTEX_BYTES];
   assert(r300_r2vb_float2_tuple_vertex_stream(
             r300_r2vb_float2_tuple_reference_records,
             R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, bytes,
             sizeof(bytes)) == 0);
   /* The recorder's allocation-size gate and the serializer's capacity
    * gate hold the same boundary: one byte short refuses.
    */
   assert(r300_r2vb_float2_tuple_vertex_stream(
             r300_r2vb_float2_tuple_reference_records,
             R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, bytes,
             sizeof(bytes) - 1) == -ENOSPC);
}

int
main(void)
{
   test_installed_stream_is_the_reference_pass();
   test_tuple_kind_arms_and_undeclared_refuses();
   test_vertex_stream_capacity_contract();
   printf("r3v-native-float2-tuple-cell: ok\n");
   return 0;
}
