/*
 * SPDX-License-Identifier: MIT
 *
 * Chip identity table proof: every r300_pci_ids row resolves to its family
 * and a die class, the RS482 target row carries the RS400-class IGP facts,
 * and identities outside the table refuse.
 */

/* The asserts carry the test's verdicts, so they stay live in NDEBUG
 * builds.
 */
#undef NDEBUG

#include "r300_chip_identity.h"
#include "r300_chipset.h"

#include "util/macros.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct expected_row {
   uint16_t pci_device;
   enum radeon_family family;
};

/* The same CHIPSET expansion the table uses; equality over this list makes
 * the coverage claim exact rather than sampled.
 */
static const struct expected_row expected_rows[] = {
#define CHIPSET(pci_id, name, chipfamily) \
   { pci_id, CHIP_##chipfamily },
#include "pci_ids/r300_pci_ids.h"
#undef CHIPSET
};

static void
test_full_table_resolves(void)
{
   for (size_t i = 0; i < ARRAY_SIZE(expected_rows); i++) {
      struct r300_chip_identity identity;
      memset(&identity, 0xa5, sizeof(identity));
      assert(r300_chip_identity_lookup(R300_PCI_VENDOR_ATI,
                                       expected_rows[i].pci_device,
                                       &identity));
      assert(identity.pci_device == expected_rows[i].pci_device);
      assert(identity.family == expected_rows[i].family);
   }
}

static void
test_rs482_target_row(void)
{
   struct r300_chip_identity identity;
   assert(r300_chip_identity_lookup(R300_PCI_VENDOR_ATI,
                                    R300_PCI_DEVICE_RS482, &identity));
   assert(identity.family == CHIP_RS480);
   assert(identity.die_class == R300_DIE_CLASS_RS400_IGP);
}

static void
test_unknown_identity_refuses(void)
{
   struct r300_chip_identity identity;
   /* A vendor other than ATI refuses even with a known device id. */
   assert(!r300_chip_identity_lookup(0x10de, R300_PCI_DEVICE_RS482,
                                     &identity));
   /* A device outside the table refuses. */
   assert(!r300_chip_identity_lookup(R300_PCI_VENDOR_ATI, 0x9999,
                                     &identity));
   /* A null output cannot receive an identity, so the lookup refuses. */
   assert(!r300_chip_identity_lookup(R300_PCI_VENDOR_ATI,
                                     R300_PCI_DEVICE_RS482, NULL));
}

static void
test_die_class_partition(void)
{
   /* Every family in the id table maps into exactly one die class; the
    * lookup would refuse a family the class map does not cover, so full
    * table resolution above already proves totality.  This leg pins the
    * class of one representative per generation.
    */
   static const struct {
      uint16_t pci_device;
      enum r300_die_class die_class;
   } representatives[] = {
      { 0x4144, R300_DIE_CLASS_R300 },      /* R300_AD */
      { 0x4150, R300_DIE_CLASS_RV350 },     /* RV350_AP */
      { 0x5974, R300_DIE_CLASS_RS400_IGP }, /* RS482 */
      { 0x4A48, R300_DIE_CLASS_R400 },      /* R420_JH */
      { 0x7100, R300_DIE_CLASS_R500 },      /* R520 */
   };
   for (size_t i = 0; i < ARRAY_SIZE(representatives); i++) {
      struct r300_chip_identity identity;
      assert(r300_chip_identity_lookup(R300_PCI_VENDOR_ATI,
                                       representatives[i].pci_device,
                                       &identity));
      assert(identity.die_class == representatives[i].die_class);
   }
}

static void
test_rs482_parse_chipset_caps(void)
{
   /* The RS482 capability row: the vertex transform engine is absent
    * (num_vert_fpus 0, no TCL), RV3xx-class zmask RAM, no HiZ or CMASK.
    */
   struct r300_capabilities caps;
   memset(&caps, 0xa5, sizeof(caps));
   r300_parse_chipset(R300_PCI_DEVICE_RS482, &caps);
   assert(caps.family == CHIP_RS480);
   assert(caps.num_vert_fpus == 0);
   assert(!caps.has_hardware_tcl);
   assert(!caps.has_tcl);
   assert(caps.hiz_ram == 0);
   assert(caps.zmask_ram == RV3xx_ZMASK_SIZE);
   assert(!caps.has_cmask);
   assert(!caps.is_r400);
   assert(!caps.is_r500);
   assert(caps.is_rv350);
}

int
main(void)
{
   test_full_table_resolves();
   test_rs482_target_row();
   test_unknown_identity_refuses();
   test_die_class_partition();
   test_rs482_parse_chipset_caps();
   printf("r300_chip_identity: OK (%zu id rows)\n",
          ARRAY_SIZE(expected_rows));
   return 0;
}
