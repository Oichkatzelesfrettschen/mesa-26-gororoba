/*
 * SPDX-License-Identifier: MIT
 *
 * Typed joins among the operation catalog, operational verb ledger, route
 * certificate, and candidate carrier-policy inventory.  String labels are
 * diagnostics only; every machine-consumed relationship uses an enum ID.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_carrier_policy.h"
#include "r300_compute_identity_carrier.h"
#include "r300_compute_verb.h"
#include "r300_operation_route.h"
#include "r300_numeric_domain.h"
#include "r300_reg.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool
catalog_rows_valid(const struct r300_virtual_op_info *table, unsigned count,
                   const char **reason)
{
   if (table == NULL || count != R300_OPERATION_ID_COUNT - 1) {
      *reason = "catalog count disagrees with the operation enum";
      return false;
   }

   for (unsigned i = 0; i < count; i++) {
      const struct r300_virtual_op_info *row = &table[i];
      if (row->operation_id == R300_OPERATION_ID_NONE ||
          (unsigned)row->operation_id >= R300_OPERATION_ID_COUNT) {
         *reason = "catalog row has an invalid operation identity";
         return false;
      }
      if (row->op_name == NULL || row->op_name[0] == '\0') {
         *reason = "catalog row lacks a diagnostic name";
         return false;
      }
      if ((unsigned)row->domain >= R300_NUM_DOMAIN_COUNT) {
         *reason = "catalog row has an invalid numeric domain";
         return false;
      }
      const struct r300_numeric_domain_info *domain =
         r300_numeric_domain_info(row->domain);
      if (domain == NULL || domain->domain != row->domain ||
          domain->name == NULL || domain->name[0] == '\0') {
         *reason = "catalog row lacks a stable numeric-domain name";
         return false;
      }
      if ((unsigned)row->status > R300_VOP_REJECTED) {
         *reason = "catalog row has an invalid operation status";
         return false;
      }
      for (unsigned j = 0; j < i; j++) {
         if (table[j].operation_id == row->operation_id) {
            *reason = "catalog rows share an operation identity";
            return false;
         }
         if (strcmp(table[j].op_name, row->op_name) == 0) {
            *reason = "catalog rows share a diagnostic name";
            return false;
         }
      }
      if (row->operation_id != (enum r300_operation_id)(i + 1)) {
         *reason = "catalog rows disagree with stable enum order";
         return false;
      }
   }

   *reason = NULL;
   return true;
}

static bool
identity_contract_valid(
   const struct r300_compute_identity_carrier_contract *contract,
   const struct r300_operation_route_row *route, const char **reason)
{
   /* The certificate binds to the R2VB route row: contracts, exactness, and
    * evidence are route facts, and the semantic verb carries none of them. */
   if (contract->operation_id != route->operation_id) {
      *reason = "route certificate and route operation disagree";
      return false;
   }
   if (contract->implementation_id != route->implementation_id) {
      *reason = "route certificate and route implementation disagree";
      return false;
   }
   if (contract->gpu_route_contract_id != route->gpu_route_contract_id) {
      *reason = "route certificate and route contract disagree";
      return false;
   }
   if (contract->admission_id != R300_ROUTE_ADMISSION_R2VB_FP24_IDENTITY ||
       route->admission_id != contract->admission_id) {
      *reason = "route certificate has an invalid admission identity";
      return false;
   }
   if (route->route_id != R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP ||
       route->executor != R300_OPERATION_ROUTE_EXECUTOR_GPU ||
       route->index_class != R300_GRID_INDEX_LINEAR ||
       route->unit != R300_EXECUTION_UNIT_R2VB_CARRIER ||
       route->exactness != R300_COMPUTE_VERB_FP24_EXACT_WINDOW ||
       route->state != R300_OPERATION_ROUTE_EXECUTING ||
       !r300_operation_has_executing_route(
          route->operation_id, R300_OPERATION_ROUTE_EXECUTOR_HOST,
          R300_ROUTE_USE_COMPUTE_STORAGE_BUFFER) ||
       route->evidence != R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED ||
       route->evidence_scope !=
          R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL) {
      *reason = "route certificate and route shape disagree";
      return false;
   }

   const struct r300_virtual_op_info *operation =
      r300_virtual_op_info_for_id(contract->operation_id);
   if (operation == NULL || operation->domain != contract->domain) {
      *reason = "route certificate and operation domain disagree";
      return false;
   }
   const struct r300_vertex_format_semantics *input_format =
      r300_vertex_format_semantics(contract->input_format_id);
   if (contract->record_dwords == 0 || input_format == NULL ||
       contract->input_format_id != R300_VERTEX_FORMAT_F32_4 ||
       contract->target.rb3d_color_format !=
          R300_COLOR_FORMAT_ARGB32323232 ||
       contract->target.us_out_fmt[0] !=
          (R300_US_OUT_FMT_C4_32_FP | R300_C0_SEL_B | R300_C1_SEL_G |
           R300_C2_SEL_R | R300_C3_SEL_A) ||
       contract->target.us_out_fmt[1] != R300_US_OUT_FMT_UNUSED ||
       contract->target.us_out_fmt[2] != R300_US_OUT_FMT_UNUSED ||
       contract->target.us_out_fmt[3] != R300_US_OUT_FMT_UNUSED ||
       contract->record_bytes != contract->record_dwords * sizeof(uint32_t) ||
       contract->record_bytes != R300_R2VB_PRODUCER_CPP_BYTES ||
       contract->record_bytes != input_format->semantic_record_bytes ||
       contract->max_records == 0 || contract->input_alignment == 0 ||
       contract->output_alignment == 0) {
      *reason = "route certificate has invalid carrier geometry";
      return false;
   }

   *reason = NULL;
   return true;
}

static bool
policy_join_valid(const struct r300_carrier_policy *policy,
                  const char **reason)
{
   if ((unsigned)policy->domain >= R300_NUM_DOMAIN_COUNT) {
      *reason = "carrier policy has an invalid numeric domain";
      return false;
   }
   if (policy->operation_id != R300_OPERATION_ID_NONE) {
      const struct r300_virtual_op_info *operation =
         r300_virtual_op_info_for_id(policy->operation_id);
      if (operation == NULL) {
         *reason = "carrier policy operation does not resolve";
         return false;
      }
      if (operation->domain != policy->domain) {
         *reason = "carrier policy and operation domain disagree";
         return false;
      }
   }

   *reason = NULL;
   return true;
}

static void
test_catalog_identity(void)
{
   const unsigned count = r300_virtual_op_count();
   const char *reason = NULL;
   assert(catalog_rows_valid(r300_virtual_op_catalog, count, &reason));
   assert(reason == NULL);
   assert(r300_virtual_op_catalog[count].operation_id ==
          R300_OPERATION_ID_NONE);
   assert(r300_virtual_op_catalog[count].op_name == NULL);

   for (unsigned i = 0; i < count; i++) {
      const enum r300_operation_id id = (enum r300_operation_id)(i + 1);
      assert(r300_virtual_op_info_for_id(id) ==
             &r300_virtual_op_catalog[i]);
   }
   assert(r300_virtual_op_info_for_id(R300_OPERATION_ID_NONE) == NULL);
   assert(r300_virtual_op_info_for_id(R300_OPERATION_ID_COUNT) == NULL);
}

static void
test_catalog_calibration(void)
{
   struct r300_virtual_op_info mutated[R300_OPERATION_ID_COUNT - 1];
   const unsigned count = r300_virtual_op_count();
   const char *reason = NULL;

   memcpy(mutated, r300_virtual_op_catalog, sizeof(mutated));
   mutated[0].operation_id = R300_OPERATION_ID_NONE;
   assert(!catalog_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "catalog row has an invalid operation identity") ==
          0);

   memcpy(mutated, r300_virtual_op_catalog, sizeof(mutated));
   mutated[0].status = (enum r300_vop_status)(R300_VOP_REJECTED + 1);
   assert(!catalog_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "catalog row has an invalid operation status") ==
          0);

   memcpy(mutated, r300_virtual_op_catalog, sizeof(mutated));
   mutated[1].operation_id = mutated[0].operation_id;
   assert(!catalog_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "catalog rows share an operation identity") == 0);

   memcpy(mutated, r300_virtual_op_catalog, sizeof(mutated));
   const enum r300_operation_id id = mutated[2].operation_id;
   mutated[2].operation_id = mutated[3].operation_id;
   mutated[3].operation_id = id;
   assert(!catalog_rows_valid(mutated, count, &reason));
   assert(strcmp(reason, "catalog rows disagree with stable enum order") ==
          0);
}

static void
test_verb_joins(void)
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *verbs =
      r300_compute_verb_rows(&count);
   const char *reason = NULL;
   assert(r300_compute_verb_rows_valid(verbs, count, &reason));
   assert(reason == NULL);

   for (uint32_t i = 0; i < count; i++) {
      assert(verbs[i].operation_id != R300_OPERATION_ID_NONE);
      assert(r300_virtual_op_info_for_id(verbs[i].operation_id) != NULL);
   }

   const struct r300_operation_route_row *identity =
      r300_operation_route(R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP);
   assert(identity_contract_valid(&r300_compute_identity_carrier_contract,
                                  identity, &reason));
   assert(reason == NULL);
}

static void
test_contract_calibration(void)
{
   const struct r300_operation_route_row *identity =
      r300_operation_route(R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP);
   struct r300_compute_identity_carrier_contract mutated =
      r300_compute_identity_carrier_contract;
   const char *reason = NULL;

   mutated.operation_id = R300_OPERATION_ID_BINARY_MAP;
   assert(!identity_contract_valid(&mutated, identity, &reason));
   assert(strcmp(reason, "route certificate and route operation disagree") ==
          0);

   mutated = r300_compute_identity_carrier_contract;
   mutated.implementation_id = R300_OPERATION_IMPLEMENTATION_NONE;
   assert(!identity_contract_valid(&mutated, identity, &reason));
   assert(strcmp(reason,
                 "route certificate and route implementation disagree") ==
          0);

   mutated = r300_compute_identity_carrier_contract;
   mutated.gpu_route_contract_id = R300_GPU_ROUTE_CONTRACT_NONE;
   assert(!identity_contract_valid(&mutated, identity, &reason));
   assert(strcmp(reason, "route certificate and route contract disagree") ==
          0);

   mutated = r300_compute_identity_carrier_contract;
   mutated.admission_id = R300_ROUTE_ADMISSION_NONE;
   assert(!identity_contract_valid(&mutated, identity, &reason));
   assert(strcmp(reason,
                 "route certificate has an invalid admission identity") ==
          0);

   struct r300_operation_route_row mutated_verb = *identity;
   mutated_verb.unit = R300_EXECUTION_UNIT_RB3D_CLEAR;
   assert(!identity_contract_valid(&r300_compute_identity_carrier_contract,
                                   &mutated_verb, &reason));
   assert(strcmp(reason, "route certificate and route shape disagree") == 0);

   mutated_verb = *identity;
   mutated_verb.evidence_scope =
      R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL;
   assert(!identity_contract_valid(&r300_compute_identity_carrier_contract,
                                   &mutated_verb, &reason));
   assert(strcmp(reason, "route certificate and route shape disagree") == 0);

   mutated = r300_compute_identity_carrier_contract;
   mutated.domain = R300_NUM_DOMAIN_U7_DOT;
   assert(!identity_contract_valid(&mutated, identity, &reason));
   assert(strcmp(reason, "route certificate and operation domain disagree") ==
          0);

   mutated = r300_compute_identity_carrier_contract;
   mutated.record_bytes--;
   assert(!identity_contract_valid(&mutated, identity, &reason));
   assert(strcmp(reason, "route certificate has invalid carrier geometry") ==
          0);
}

static void
test_policy_joins(void)
{
   unsigned count = 0;
   const struct r300_carrier_policy *const *policies =
      r300_carrier_policies(&count);
   const char *reason = NULL;

   assert(count == 9);
   for (unsigned i = 0; i < count; i++) {
      assert(policy_join_valid(policies[i], &reason));
      assert(reason == NULL);
   }

   struct r300_carrier_policy mutated = r300_carrier_dp4_u7;
   mutated.domain = R300_NUM_DOMAIN_FP24_RTZ;
   assert(!policy_join_valid(&mutated, &reason));
   assert(strcmp(reason, "carrier policy and operation domain disagree") ==
          0);

   mutated = r300_carrier_ieee16_result;
   mutated.domain = R300_NUM_DOMAIN_COUNT;
   assert(!policy_join_valid(&mutated, &reason));
   assert(strcmp(reason, "carrier policy has an invalid numeric domain") ==
          0);
}

/* The domain lookup carries the exactness contract a route admits against,
 * so an unrecognized value resolves to no row rather than to index 0.  The
 * refusal arms below are the calibration: each names a value outside
 * [0, COUNT) and asserts the lookup declines it, and the FP24_RTZ arm proves
 * a declined value does not arrive at the first row by another name. */
static void
test_domain_lookup_refusal(void)
{
   const struct r300_numeric_domain_info *info = NULL;

   for (unsigned d = 0; d < R300_NUM_DOMAIN_COUNT; d++) {
      assert(r300_numeric_domain_info_checked((enum r300_numeric_domain)d,
                                              &info));
      assert(info != NULL && info->domain == (enum r300_numeric_domain)d);
   }

   /* COUNT is the first value past the table; -1 reaches the same guard
    * through the unsigned cast the lookup performs. */
   static const enum r300_numeric_domain outside[] = {
      R300_NUM_DOMAIN_COUNT,
      (enum r300_numeric_domain)(R300_NUM_DOMAIN_COUNT + 1),
      (enum r300_numeric_domain)-1,
   };
   for (unsigned i = 0; i < 3; i++) {
      /* Seed a live row so the false path is shown to clear *out. */
      info = r300_numeric_domain_info(R300_NUM_DOMAIN_FP24_RTZ);
      assert(info != NULL);
      assert(!r300_numeric_domain_info_checked(outside[i], &info));
      assert(info == NULL);
      assert(r300_numeric_domain_info(outside[i]) == NULL);
   }

   /* A declined value never resolves to the FP24_RTZ window. */
   const struct r300_numeric_domain_info *fp24 =
      r300_numeric_domain_info(R300_NUM_DOMAIN_FP24_RTZ);
   assert(fp24 != NULL && fp24->exact_int_bound == R300_FP24_EXACT_INT_CEILING);
   assert(r300_numeric_domain_info(R300_NUM_DOMAIN_COUNT) != fp24);
}

int
main(void)
{
   test_catalog_identity();
   test_catalog_calibration();
   test_verb_joins();
   test_contract_calibration();
   test_policy_joins();
   test_domain_lookup_refusal();
   printf("r300_operation_ledger_test: all checks passed\n");
   return 0;
}
