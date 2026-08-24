/*
 * SPDX-License-Identifier: MIT
 *
 * Plan tool: composes a sealed conformance plan from a captured
 * transcript and the run identities, and checks a plan file.
 */

#undef NDEBUG

#include "r3v_native_plan.h"

#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc(n > 0 ? (size_t)n : 1);
   if (buf == NULL || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
      free(buf);
      fclose(f);
      return NULL;
   }
   fclose(f);
   *size = (size_t)n;
   return buf;
}

static int
load_plan(const char *path, struct r3v_native_plan *plan)
{
   size_t size;
   char *text = read_file(path, &size);
   if (text == NULL) {
      fprintf(stderr, "FAIL: cannot read %s\n", path);
      return 2;
   }
   enum r3v_native_plan_parse_result r = r3v_native_plan_parse(text, size, plan);
   free(text);
   if (r != R3V_NATIVE_PLAN_PARSE_OK) {
      fprintf(stderr, "FAIL: %s: %s\n", path,
              r3v_native_plan_parse_result_name(r));
      return 2;
   }
   return 0;
}

static int
write_plan(const struct r3v_native_plan *plan, const char *path)
{
   long size = r3v_native_plan_write(plan, NULL, 0);
   if (size <= 0) {
      fprintf(stderr, "FAIL: plan outside the schema\n");
      return 2;
   }
   char *text = malloc((size_t)size);
   assert(text != NULL);
   long written = r3v_native_plan_write(plan, text, (size_t)size);
   assert(written == size);
   FILE *f = fopen(path, "wb");
   if (f == NULL || fwrite(text, 1, (size_t)size, f) != (size_t)size ||
       fclose(f) != 0) {
      fprintf(stderr, "FAIL: cannot write %s\n", path);
      free(text);
      return 2;
   }
   free(text);
   return 0;
}

static void
report(const struct r3v_native_plan *plan)
{
   printf("plan: %u submissions, ceilings ib %u dwords, %u relocs, %llu "
          "bytes, %u submissions, %u s; pci %04x:%04x; queue %s; seal "
          "%.12s\n", plan->submission_count, plan->max_ib_dwords,
          plan->max_relocs, (unsigned long long)plan->max_cumulative_bytes,
          plan->max_submissions, plan->max_runtime_seconds,
          plan->pci_vendor_id, plan->pci_device_id,
          r3v_native_plan_queue_claim_name(plan->queue_claim), plan->seal);
   for (uint32_t i = 0; i < plan->submission_count; i++) {
      const struct r3v_native_plan_submission *s = &plan->submissions[i];
      printf("  %u %.12s %u %s %u relocs\n", i, s->ib_blake3, s->ib_dwords,
             r3v_native_plan_cell_kind_name(s->cell_kind), s->reloc_count);
   }
}

/* Copies a declared value into its fixed field, refusing a value the
 * field cannot hold whole and a value the schema refuses, by option
 * name.
 */
static int
take(char *dst, size_t size, const char *value, bool hex, const char *name)
{
   size_t n = strlen(value);
   if (n == 0 || n >= size) {
      fprintf(stderr, "FAIL: %s: value of %zu bytes does not fit %zu\n",
              name, n, size - 1);
      return 2;
   }
   if (hex && (n != size - 1 || strspn(value, "0123456789abcdef") != n)) {
      fprintf(stderr, "FAIL: %s: expected %zu lowercase hex digits\n", name,
              size - 1);
      return 2;
   }
   memcpy(dst, value, n + 1);
   return 0;
}

/* compose replaces every placeholder identity of a transcript with the
 * declared run identity and the declared runtime ceiling, then seals
 * the result; a missing or over-length identity refuses by name.  The
 * other ceilings stay at the observed maxima, since the plan replays
 * the recorded sequence exactly.
 */
static int
compose(int argc, char **argv)
{
   const char *transcript = NULL, *out = NULL, *source = NULL, *dso = NULL,
              *deqp = NULL, *release = NULL, *partition = NULL,
              *caselist = NULL, *claim = NULL, *kernel = NULL,
              *module = NULL, *nonce = NULL, *evidence = NULL,
              *clean = NULL;
   uint32_t runtime = 0;
   for (int i = 0; i + 1 < argc; i += 2) {
      const char *k = argv[i], *v = argv[i + 1];
      if (strcmp(k, "--transcript") == 0) transcript = v;
      else if (strcmp(k, "--out") == 0) out = v;
      else if (strcmp(k, "--source-sha") == 0) source = v;
      else if (strcmp(k, "--dso-blake3") == 0) dso = v;
      else if (strcmp(k, "--deqp-sha256") == 0) deqp = v;
      else if (strcmp(k, "--deqp-release") == 0) release = v;
      else if (strcmp(k, "--partition-sha256") == 0) partition = v;
      else if (strcmp(k, "--caselist-sha256") == 0) caselist = v;
      else if (strcmp(k, "--queue-claim") == 0) claim = v;
      else if (strcmp(k, "--kernel-release") == 0) kernel = v;
      else if (strcmp(k, "--module-srcversion") == 0) module = v;
      else if (strcmp(k, "--nonce") == 0) nonce = v;
      else if (strcmp(k, "--evidence-dir") == 0) evidence = v;
      else if (strcmp(k, "--source-clean") == 0) clean = v;
      else if (strcmp(k, "--max-runtime-seconds") == 0) {
         char *end;
         unsigned long parsed = strtoul(v, &end, 10);
         if (*v == '\0' || *end != '\0' || parsed == 0 ||
             parsed > R3V_NATIVE_PLAN_RUNTIME_SECONDS_MAX) {
            fprintf(stderr, "FAIL: --max-runtime-seconds: expected 1..%u\n",
                    R3V_NATIVE_PLAN_RUNTIME_SECONDS_MAX);
            return 2;
         }
         runtime = (uint32_t)parsed;
      } else {
         fprintf(stderr, "FAIL: unknown option %s\n", k);
         return 2;
      }
   }
   const char *const required[] = {transcript, out, source, dso, deqp,
                                   release, partition, caselist, claim,
                                   kernel, module, nonce, evidence, clean};
   const char *const names[] = {"--transcript", "--out", "--source-sha",
                                "--dso-blake3", "--deqp-sha256",
                                "--deqp-release", "--partition-sha256",
                                "--caselist-sha256", "--queue-claim",
                                "--kernel-release", "--module-srcversion",
                                "--nonce", "--evidence-dir",
                                "--source-clean"};
   for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
      if (required[i] == NULL) {
         fprintf(stderr, "FAIL: compose requires %s\n", names[i]);
         return 2;
      }
   }
   if (runtime == 0) {
      fprintf(stderr, "FAIL: compose requires --max-runtime-seconds\n");
      return 2;
   }
   struct r3v_native_plan plan;
   int rc = load_plan(transcript, &plan);
   if (rc != 0)
      return rc;
   if (strcmp(clean, "1") != 0) {
      fprintf(stderr, "FAIL: --source-clean must declare 1 for a plan; a "
                      "dirty tree seals no plan\n");
      r3v_native_plan_finish(&plan);
      return 2;
   }
   plan.source_clean = true;
   if (take(plan.source_sha, sizeof(plan.source_sha), source, true,
            "--source-sha") ||
       take(plan.dso_blake3, sizeof(plan.dso_blake3), dso, true,
            "--dso-blake3") ||
       take(plan.deqp_sha256, sizeof(plan.deqp_sha256), deqp, true,
            "--deqp-sha256") ||
       take(plan.deqp_release, sizeof(plan.deqp_release), release, false,
            "--deqp-release") ||
       take(plan.partition_sha256, sizeof(plan.partition_sha256), partition,
            true, "--partition-sha256") ||
       take(plan.caselist_sha256, sizeof(plan.caselist_sha256), caselist,
            true, "--caselist-sha256") ||
       take(plan.kernel_release, sizeof(plan.kernel_release), kernel, false,
            "--kernel-release") ||
       take(plan.module_srcversion, sizeof(plan.module_srcversion), module,
            false, "--module-srcversion") ||
       take(plan.nonce, sizeof(plan.nonce), nonce, true, "--nonce") ||
       take(plan.evidence_dir, sizeof(plan.evidence_dir), evidence, false,
            "--evidence-dir")) {
      r3v_native_plan_finish(&plan);
      return 2;
   }
   if (!r3v_native_plan_queue_claim_parse(claim, &plan.queue_claim)) {
      fprintf(stderr, "FAIL: queue claim %s unknown\n", claim);
      r3v_native_plan_finish(&plan);
      return 2;
   }
   plan.max_runtime_seconds = runtime;
   rc = write_plan(&plan, out);
   r3v_native_plan_finish(&plan);
   if (rc != 0)
      return rc;
   /* The written plan must parse back: the seal, the identities, and the
    * entries all validate on the way in.
    */
   rc = load_plan(out, &plan);
   if (rc != 0)
      return rc;
   report(&plan);
   r3v_native_plan_finish(&plan);
   return 0;
}

static int
check(int argc, char **argv)
{
   if (argc != 1) {
      fprintf(stderr, "usage: check PLAN\n");
      return 2;
   }
   struct r3v_native_plan plan;
   int rc = load_plan(argv[0], &plan);
   if (rc != 0)
      return rc;
   report(&plan);
   r3v_native_plan_finish(&plan);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc >= 2 && strcmp(argv[1], "compose") == 0)
      return compose(argc - 2, argv + 2);
   if (argc >= 2 && strcmp(argv[1], "check") == 0)
      return check(argc - 2, argv + 2);
   fprintf(stderr, "usage: r3v_native_plan_tool compose|check ...\n");
   return 2;
}
