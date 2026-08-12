/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "r300_r2vb_capture_gate.h"

static unsigned failures;

#define CHECK(condition, name)                                                \
   do {                                                                      \
      if (!(condition)) {                                                   \
         fprintf(stderr, "FAIL %s: %s\n", name, #condition);             \
         failures++;                                                       \
      }                                                                     \
   } while (0)

static enum r300_r2vb_selftest_action
select_action(const char *hb_tcl, const char *timing,
              const char *raw_submit_accepted, bool query_active)
{
   return r300_r2vb_select_selftest_action(
      hb_tcl, timing, raw_submit_accepted, true, true, false, query_active);
}

static void
check_exact_transport_values(void)
{
   CHECK(select_action(NULL, NULL, NULL, false) == R300_R2VB_SELFTEST_DECLINE,
          "unset gates decline");
   CHECK(select_action("1", NULL, NULL, false) == R300_R2VB_SELFTEST_DECLINE,
          "HB_TCL alone declines");
   CHECK(select_action(NULL, "capture", NULL, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "capture mode alone declines");
   CHECK(select_action("", "capture", NULL, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "empty HB_TCL declines");
   CHECK(select_action("0", "capture", NULL, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "zero HB_TCL declines");
   CHECK(select_action("1", "", NULL, false) == R300_R2VB_SELFTEST_DECLINE,
          "empty timing declines");
   CHECK(select_action("1", "0", NULL, false) == R300_R2VB_SELFTEST_DECLINE,
          "zero timing declines");
   CHECK(select_action("1", "captureX", NULL, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "invalid timing declines with a diagnostic");
   CHECK(select_action("1", "capture", NULL, false) ==
             R300_R2VB_SELFTEST_CAPTURE,
          "exact capture gates admit no-submit capture");
   CHECK(select_action("1", "submit", "1", false) ==
             R300_R2VB_SELFTEST_SUBMIT,
          "exact submit gates and consent admit submission");
}

static void
check_submit_consent(void)
{
   CHECK(select_action("1", "submit", NULL, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "unset raw-submit consent declines");
   CHECK(select_action("1", "submit", "", false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "empty raw-submit consent declines");
   CHECK(select_action("1", "submit", "0", false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "zero raw-submit consent declines");
   CHECK(select_action("1", "submit", "yes", false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "non-exact raw-submit consent declines");
}

static void
check_rs48x_capability(void)
{
   CHECK(r300_r2vb_select_selftest_action(
             "1", "capture", NULL, true, true, false, false) ==
             R300_R2VB_SELFTEST_CAPTURE,
          "RS48x capability keeps HB_TCL capture reachable");
   CHECK(r300_r2vb_select_selftest_action(
             "1", "capture", NULL, false, true, false, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "non-RS48x capability declines the self-test");
}

static void
check_flush_and_query_admission(void)
{
   CHECK(r300_r2vb_select_selftest_action(
             "1", "capture", NULL, true, false, false, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "non-flush entry declines");
   CHECK(r300_r2vb_select_selftest_action(
             "1", "capture", NULL, true, true, true, false) ==
             R300_R2VB_SELFTEST_DECLINE,
          "capture fires once");
   CHECK(select_action("1", "capture", NULL, true) ==
             R300_R2VB_SELFTEST_DECLINE,
          "capture declines an active query before query bookkeeping changes");
   CHECK(select_action("1", "submit", "1", true) ==
             R300_R2VB_SELFTEST_SUBMIT,
          "submitted query end retains its matching GPU write");
}

static void
check_nested_probe_and_readback_guards(void)
{
   CHECK(r300_r2vb_probe_dispatch_allowed(false),
         "outer flush admits probe dispatch");
   CHECK(!r300_r2vb_probe_dispatch_allowed(true),
         "nested flush suppresses probe dispatch");

   CHECK(r300_r2vb_transform_verify_allowed(true, true, true),
         "signalled submit admits transform verification");
   CHECK(!r300_r2vb_transform_verify_allowed(true, true, false),
         "unsignalled submit suppresses transform verification");
   CHECK(!r300_r2vb_transform_verify_allowed(false, true, true),
         "disabled transform suppresses verification");
   CHECK(!r300_r2vb_transform_verify_allowed(true, false, true),
         "non-submit mode suppresses verification");
}

int
main(void)
{
   check_exact_transport_values();
   check_submit_consent();
   check_rs48x_capability();
   check_flush_and_query_admission();
   check_nested_probe_and_readback_guards();

   if (failures) {
      fprintf(stderr, "r300_r2vb_capture_gate_test: %u failure(s)\n",
            failures);
      return 1;
   }

   printf("r300_r2vb_capture_gate_test: all checks passed\n");
   return 0;
}
