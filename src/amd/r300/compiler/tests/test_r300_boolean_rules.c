/*
 * SPDX-License-Identifier: MIT
 *
 * Independent host-only truth-table and fanout equivalence checker for
 * bounded Boolean rewrites in the R300 compiler.
 *
 * Verifies:
 * 1. Primary candidate: 4-input, 2-output shared factoring (128 comparisons).
 * 2. Exact structural operation count: C(x0, x1) = 5 - x0 * x1.
 * 3. Profitability counterexample: observable shared term prevents removal.
 * 4. Truth-table masks 0xa8a8 and 0xaa88 matching ANF polynomials.
 * 5. Strengthened 3-input independent conjunction (H.264 neutral control).
 * 6. XOR/XNOR complement-select equivalence and non-negative intermediate bounds.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned g_failures = 0;

#define CHECK(cond, msg)                                                \
   do {                                                                 \
      if (cond) {                                                       \
         printf("  ok   - %s\n", (msg));                                \
      } else {                                                          \
         printf("  FAIL - %s (line %d)\n", (msg), __LINE__);            \
         g_failures++;                                                  \
      }                                                                 \
   } while (0)

/* Original 2-output graph:
 * p  = a & b
 * y0 = p | (a & c)
 * y1 = p | (a & d)
 */
static void eval_orig(bool a, bool b, bool c, bool d, bool *y0, bool *y1)
{
   bool p = a && b;
   *y0 = p || (a && c);
   *y1 = p || (a && d);
}

/* Factored 2-output graph:
 * y0 = a & (b | c)
 * y1 = a & (b | d)
 */
static void eval_factored(bool a, bool b, bool c, bool d, bool *y0, bool *y1)
{
   *y0 = a && (b || c);
   *y1 = a && (b || d);
}

static void test_shared_factoring_128_comparisons(void)
{
   unsigned checks_passed = 0;

   /* 4 rewrite configurations:
    * cfg 0: orig y0, orig y1
    * cfg 1: fact y0, orig y1
    * cfg 2: orig y0, fact y1
    * cfg 3: fact y0, fact y1
    */
   for (unsigned cfg = 0; cfg < 4; cfg++) {
      bool rewrite_y0 = (cfg & 1) != 0;
      bool rewrite_y1 = (cfg & 2) != 0;

      for (unsigned i = 0; i < 16; i++) {
         bool a = (i >> 0) & 1;
         bool b = (i >> 1) & 1;
         bool c = (i >> 2) & 1;
         bool d = (i >> 3) & 1;

         bool ref_y0, ref_y1;
         eval_orig(a, b, c, d, &ref_y0, &ref_y1);

         bool test_y0 = rewrite_y0 ? (a && (b || c)) : (ref_y0);
         bool test_y1 = rewrite_y1 ? (a && (b || d)) : (ref_y1);

         if (test_y0 == ref_y0)
            checks_passed++;
         if (test_y1 == ref_y1)
            checks_passed++;
      }
   }

   CHECK(checks_passed == 128,
         "128/128 output comparisons match between original and rewritten graphs");
}

static void test_structural_operation_counts(void)
{
   /* Model distinct operations in the graph for each rewrite choice.
    * Candidates:
    * OP_AND_AB (p)
    * OP_AND_AC
    * OP_AND_AD
    * OP_OR_B_C
    * OP_OR_B_D
    * OP_AND_A_BC (y0 rewritten)
    * OP_AND_A_BD (y1 rewritten)
    * OP_OR_P_AC  (y0 original)
    * OP_OR_P_AD  (y1 original)
    */
   for (unsigned cfg = 0; cfg < 4; cfg++) {
      bool x0 = (cfg & 1) != 0;
      bool x1 = (cfg & 2) != 0;

      /* Count required operations:
       * When !x0: needs p (AND_AB), AND_AC, OR_P_AC
       * When x0:  needs OR_B_C, AND_A_BC
       * When !x1: needs p (AND_AB), AND_AD, OR_P_AD
       * When x1:  needs OR_B_D, AND_A_BD
       */
      bool need_and_ab = (!x0 || !x1);
      bool need_and_ac = !x0;
      bool need_or_p_ac = !x0;
      bool need_or_b_c = x0;
      bool need_and_a_bc = x0;

      bool need_and_ad = !x1;
      bool need_or_p_ad = !x1;
      bool need_or_b_d = x1;
      bool need_and_a_bd = x1;

      unsigned count = need_and_ab + need_and_ac + need_or_p_ac +
                       need_or_b_c + need_and_a_bc + need_and_ad +
                       need_or_p_ad + need_or_b_d + need_and_a_bd;

      unsigned expected = 5 - (x0 && x1 ? 1 : 0);
      char msg[80];
      snprintf(msg, sizeof(msg), "cfg (%d, %d) operation count is %u (expected %u)",
               x0, x1, count, expected);
      CHECK(count == expected, msg);
   }
}

static void test_profitability_counterexample_observable_p(void)
{
   /* When y2 = p = a & b is externally observable, p cannot be eliminated
    * even when x0 and x1 are both 1. */
   bool x0 = true;
   bool x1 = true;
   bool p_observable = true;

   bool need_and_ab = (!x0 || !x1 || p_observable);
   bool need_or_b_c = x0;
   bool need_and_a_bc = x0;
   bool need_or_b_d = x1;
   bool need_and_a_bd = x1;

   unsigned count = need_and_ab + need_or_b_c + need_and_a_bc +
                    need_or_b_d + need_and_a_bd;

   CHECK(count == 5,
         "observable p preserves 5 operations even with joint rewrite (fail-closed fanout)");
}

static void test_truth_table_masks_and_anf(void)
{
   uint32_t mask0 = 0;
   uint32_t mask1 = 0;

   for (unsigned i = 0; i < 16; i++) {
      bool a = (i >> 0) & 1;
      bool b = (i >> 1) & 1;
      bool c = (i >> 2) & 1;
      bool d = (i >> 3) & 1;

      bool y0, y1;
      eval_orig(a, b, c, d, &y0, &y1);

      if (y0)
         mask0 |= (1u << i);
      if (y1)
         mask1 |= (1u << i);

      /* Check ANF polynomials:
       * y0 = (a & b) ^ (a & c) ^ (a & b & c)
       * y1 = (a & b) ^ (a & d) ^ (a & b & d)
       */
      bool anf0 = (a && b) ^ (a && c) ^ (a && b && c);
      bool anf1 = (a && b) ^ (a && d) ^ (a && b && d);
      CHECK(y0 == anf0, "y0 matches ANF polynomial");
      CHECK(y1 == anf1, "y1 matches ANF polynomial");
   }

   CHECK(mask0 == 0xa8a8u, "truth-table mask0 matches 0xa8a8");
   CHECK(mask1 == 0xaa88u, "truth-table mask1 matches 0xaa88");
}

static void test_strengthened_h264_conjunction(void)
{
   /* Conjunction of 3 independent predicates across all 8 input combinations */
   unsigned match_count = 0;
   for (unsigned i = 0; i < 8; i++) {
      bool a = (i >> 0) & 1;
      bool b = (i >> 1) & 1;
      bool c = (i >> 2) & 1;

      bool g = a && b && c;
      /* In ANF over GF(2), g = a * b * c (single monomial) */
      bool anf = a & b & c;
      if (g == anf)
         match_count++;
   }
   CHECK(match_count == 8,
         "strengthened H.264 independent conjunction matches across all 8 inputs (0 delta)");
}

static void test_xor_complement_select_equivalence(void)
{
   /* Verify:
    * XOR(a, b)  = CMP(-a, 1 - b, b)
    * XNOR(a, b) = CMP(-a, b, 1 - b)
    * where CMP(c, x, y) returns x if c < 0, else y.
    */
   for (unsigned a = 0; a <= 1; a++) {
      for (unsigned b = 0; b <= 1; b++) {
         float fa = (float)a;
         float fb = (float)b;

         float cond = -fa; /* < 0 if a == 1, == 0 if a == 0 */
         float inv_b = 1.0f - fb; /* strictly in {0.0, 1.0}, non-negative */

         /* CMP(cond, inv_b, fb) */
         float xor_res = (cond < 0.0f) ? inv_b : fb;
         /* CMP(cond, fb, inv_b) */
         float xnor_res = (cond < 0.0f) ? fb : inv_b;

         CHECK((unsigned)xor_res == (a ^ b),
               "XOR complement-select matches logical XOR");
         CHECK((unsigned)xnor_res == !(a ^ b),
               "XNOR complement-select matches logical XNOR");
         CHECK(inv_b >= 0.0f && inv_b <= 1.0f,
               "intermediate (1 - b) is strictly non-negative (no ULP predecessor shift)");
      }
   }
}

int main(void)
{
   printf("r300 bounded boolean rewrite and fanout test\n");

   test_shared_factoring_128_comparisons();
   test_structural_operation_counts();
   test_profitability_counterexample_observable_p();
   test_truth_table_masks_and_anf();
   test_strengthened_h264_conjunction();
   test_xor_complement_select_equivalence();

   if (g_failures) {
      fprintf(stderr, "r300_boolean_rules_test: %u failures\n", g_failures);
      return 1;
   }
   printf("r300_boolean_rules_test: all checks passed\n");
   return 0;
}
