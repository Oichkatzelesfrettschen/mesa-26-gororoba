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
#include "r300_grid_fold.h"
#include "r300_reg.h"

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
                                    R300_PCI_DEVICE_RS48X_5974, &identity));
   assert(identity.family == CHIP_RS480);
   assert(identity.die_class == R300_DIE_CLASS_RS400_IGP);
}

static void
test_unknown_identity_refuses(void)
{
   struct r300_chip_identity identity;
   /* A vendor other than ATI refuses even with a known device id. */
   assert(!r300_chip_identity_lookup(0x10de, R300_PCI_DEVICE_RS48X_5974,
                                     &identity));
   /* A device outside the table refuses. */
   assert(!r300_chip_identity_lookup(R300_PCI_VENDOR_ATI, 0x9999,
                                     &identity));
   /* A null output cannot receive an identity, so the lookup refuses. */
   assert(!r300_chip_identity_lookup(R300_PCI_VENDOR_ATI,
                                     R300_PCI_DEVICE_RS48X_5974, NULL));
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
   r300_parse_chipset(R300_PCI_DEVICE_RS48X_5974, &caps);
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


static void
test_rs4xx_igp_family_facts(void)
{
   /* The facts record rides only the RS480 family rows, and its numeric
    * and register fields agree with their macro homes.
    */
   struct r300_chip_identity identity;
   assert(r300_chip_identity_lookup(R300_PCI_VENDOR_ATI,
                                    R300_PCI_DEVICE_RS48X_5974, &identity));
   assert(identity.family_facts == &r300_rs4xx_igp_family_facts);
   assert(r300_chip_identity_lookup(R300_PCI_VENDOR_ATI,
                                    R300_PCI_DEVICE_RS482M_5975, &identity));
   assert(identity.family_facts == &r300_rs4xx_igp_family_facts);
   assert(r300_chip_identity_lookup(R300_PCI_VENDOR_ATI, 0x4144,
                                    &identity));
   assert(identity.family_facts == NULL);

   const struct r300_family_facts *facts = &r300_rs4xx_igp_family_facts;
   assert(facts->vertex_engine_absent);
   assert(facts->fp24_exact_int_ceiling == R300_FP24_EXACT_INT_CEILING);
   assert(facts->dstcache_ctlstat_reg == R300_RB3D_DSTCACHE_CTLSTAT);
   assert(facts->zcache_ctlstat_reg == R300_ZB_ZCACHE_CTLSTAT);
   /* Sampler ceiling equals the register height mask plus one. */
   assert(facts->sampler_dimension_max == 2048);
   assert(facts->render_span_max == 2560);
   /* The vertex-engine absence agrees with the capability row. */
   struct r300_capabilities caps;
   r300_parse_chipset(R300_PCI_DEVICE_RS48X_5974, &caps);
   assert(facts->vertex_engine_absent == (caps.num_vert_fpus == 0));
}

static void
test_platform_identity(void)
{
   /* The Vostro 1000 tuple resolves the RS485 product through the
    * board, with the firmware's padded DMI spelling; the die id alone,
    * another subsystem, or another board name resolves nothing.
    */
   const struct r300_platform_identity *row = NULL;
   assert(r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                        R300_PCI_DEVICE_RS48X_5974, 0x1028,
                                        0x022a, "Vostro   1000 ", &row));
   assert(row == &r300_platform_vostro1000);
   /* The exact tuple, field by field.  A shared PCI id names a die class
    * and not a part, so the row is the agreement among the id, the board's
    * subsystem id, its DMI product, and the option-ROM strings; asserting
    * the whole tuple is what makes that agreement checkable. */
   assert(row->pci_vendor == R300_PCI_VENDOR_ATI);
   assert(row->pci_device == R300_PCI_DEVICE_RS48X_5974);
   assert(row->subsystem_vendor == 0x1028);
   assert(row->subsystem_device == 0x022a);
   assert(strcmp(row->dmi_product_name, "Vostro 1000") == 0);
   assert(strcmp(row->firmware_chip_name, "RS485/M") == 0);
   assert(strcmp(row->firmware_product_name, "ATI Radeon Xpress 1150") == 0);
   assert(strcmp(row->die_name, "RS485") == 0);
   assert(strcmp(row->part_name, "RS485M") == 0);
   assert(strcmp(row->product_name, "Radeon Xpress 1150") == 0);
   assert(strcmp(row->historical_alias, "rs482") == 0);
   assert(row->platform_id == R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M);
   /* What the lookup compares and what the identity rests on are separate
    * facts: the ROM is read once and retained, never on this path. */
   assert(row->runtime_match_basis == R300_PLATFORM_MATCH_PCI_SUBSYSTEM_DMI);
   assert(row->identity_evidence ==
          (R300_PLATFORM_EVIDENCE_OPTION_ROM_STRING |
           R300_PLATFORM_EVIDENCE_PRODUCT_STRING |
           R300_PLATFORM_EVIDENCE_RETAINED_ROM_DIGEST));

   /* Neither name is RS482, which is the desktop Xpress 1100 sharing this
    * PCI id. */
   assert(strcmp(row->die_name, "RS482") != 0);
   assert(strcmp(row->part_name, "RS482") != 0);

   /* The id alone resolves nothing: a specimen carrying it without the
    * board's subsystem id and DMI product is a different platform, and the
    * lookup leaves it unresolved rather than assuming this one. */
   const struct r300_platform_identity *absent = NULL;
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS48X_5974, 0x1028,
                                         0x0000, "Vostro 1000", &absent));
   assert(absent == NULL);
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS48X_5974, 0x1028,
                                         0x022a, "Inspiron 1501", &absent));
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS48X_5974, 0x1028,
                                         0x022a, NULL, &absent));
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS48X_5974, 0x0000,
                                         0x0000, "", &absent));
   assert(absent == NULL);
   assert(strcmp(row->historical_alias, "rs482") == 0);
   assert(row->subsystem_vendor ==
          r300_vostro1000_rs485m_specimen_facts.subsystem_vendor);
   assert(row->subsystem_device ==
          r300_vostro1000_rs485m_specimen_facts.subsystem_device);
   row = NULL;
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS48X_5974, 0x1028,
                                         0x0000, "Vostro 1000", &row));
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS48X_5974, 0x1028,
                                         0x022a, "Latitude D531", &row));
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS482M_5975, 0x1028,
                                         0x022a, "Vostro 1000", &row));
   assert(!r300_platform_identity_lookup(R300_PCI_VENDOR_ATI,
                                         R300_PCI_DEVICE_RS48X_5974, 0x1028,
                                         0x022a, NULL, &row));
   assert(row == NULL);
}

int
main(void)
{
   test_platform_identity();
   test_full_table_resolves();
   test_rs482_target_row();
   test_unknown_identity_refuses();
   test_die_class_partition();
   test_rs482_parse_chipset_caps();
   test_rs4xx_igp_family_facts();
   printf("r300_chip_identity: OK (%zu id rows)\n",
          ARRAY_SIZE(expected_rows));
   return 0;
}
