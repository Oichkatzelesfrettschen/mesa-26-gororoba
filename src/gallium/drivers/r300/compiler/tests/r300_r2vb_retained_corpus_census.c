/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Retained-corpus census loader: every r2vb-vs-<blake3>.nir blob in the
 * directory named by R300_R2VB_RETAINED_CORPUS runs through the production
 * plan chain (r300_r2vb_plan_producer) and emits one machine-readable census
 * row per (specimen, computed-varying key, position space).  The telemetry
 * retention path (r300_r2vb_telemetry.c) writes these blobs: nir_serialize
 * of the bound application vertex shader, filename carrying the full BLAKE3
 * content hash, so the loader verifies each file against its own name before
 * trusting the bytes.
 *
 * Serialized NIR is an internal representation bound to the producing
 * source tree, so the corpus directory carries a provenance manifest
 * (corpus-provenance.txt, `mesa_git_sha1=<short sha>`) and the loader
 * compares it against its own MESA_GIT_SHA1.  A nonempty corpus with a
 * missing or mismatching manifest refuses the corpus; the exact-value
 * research override
 * R300_R2VB_CORPUS_COMPAT_OVERRIDE=1 proceeds with the whole run marked
 * serialization_compatibility=unverified.
 *
 * Each specimen decodes and plans in a forked child, so a malformed blob
 * that trips nir_deserialize's internal fail-stop validation kills only
 * that entry and the walk continues (R300_R2VB_CORPUS_NO_FORK=1 runs
 * in-process for debugger use).  Before any decode, an entry must be a
 * regular non-symlink file within the blob size ceiling whose full BLAKE3
 * matches its filename.
 *
 * The census replans under this harness's recorded canonical nondegenerate
 * viewport, never the live viewport of the retaining process; the
 * census-provenance header states mode=canonical-replan with the exact key
 * values so no row reads as an event replay.
 *
 * The corpus gate takes an existing directory path holding at least one
 * corpus-shaped entry; unset, empty, and zero-specimen directories exit
 * with the meson skip code, so a run that proves nothing reports SKIP
 * instead of an empty PASS.
 */

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nir.h"
#include "nir_serialize.h"

#include "git_sha1.h"
#include "util/blob.h"
#include "util/mesa-blake3.h"

#include "r300_context.h"
#include "r300_r2vb_plan.h"
#include "r300_screen.h"
#include "radeon_regalloc.h"

static unsigned g_failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         g_failures++;                    \
      }                                   \
   } while (0)

static struct r300_screen g_screen;
static struct r300_context g_context;

/* The canonical nondegenerate replan viewport, printed in the provenance
 * header: window-space plans bake these values as immediates. */
#define CANON_VP_SCALE_X 320.0f
#define CANON_VP_SCALE_Y 240.0f
#define CANON_VP_SCALE_Z 0.5f

static void
fake_stack_init(void)
{
   memset(&g_screen, 0, sizeof(g_screen));
   g_screen.caps.has_tcl = false;
   g_screen.caps.is_r400 = false;
   g_screen.caps.is_r500 = false;
   r300_screen_init_nir_options(&g_screen);

   memset(&g_context, 0, sizeof(g_context));
   g_context.screen = &g_screen;
   rc_init_regalloc_state(&g_context.fs_regalloc_state, RC_FRAGMENT_PROGRAM);
   g_context.viewport.scale[0] = CANON_VP_SCALE_X;
   g_context.viewport.scale[1] = CANON_VP_SCALE_Y;
   g_context.viewport.scale[2] = CANON_VP_SCALE_Z;
   g_context.viewport.translate[0] = CANON_VP_SCALE_X;
   g_context.viewport.translate[1] = CANON_VP_SCALE_Y;
   g_context.viewport.translate[2] = CANON_VP_SCALE_Z;
}

/* Stable names for the machine-readable row: enum renumbering must never
 * silently reinterpret archived census output. */
static const char *
plan_status_str(enum r300_r2vb_plan_status status)
{
   switch (status) {
   case R300_R2VB_PLAN_READY:             return "ready";
   case R300_R2VB_PLAN_SEMANTIC_REJECT:   return "semantic_reject";
   case R300_R2VB_PLAN_TRANSIENT_FAILURE: return "transient_failure";
   }
   return "unknown";
}

static const char *
typed_class_str(enum r300_r2vb_typed_source_class source_class)
{
   switch (source_class) {
   case R300_R2VB_TYPED_SOURCE_NONE: return "none";
   case R300_R2VB_TYPED_SOURCE_BOOL: return "bool";
   case R300_R2VB_TYPED_SOURCE_SINT: return "sint";
   case R300_R2VB_TYPED_SOURCE_UINT: return "uint";
   }
   return "unknown";
}

/* Order-independent carry signature of a SPLIT plan: one letter per carried
 * scalar component, sorted, so equal partitions print equal signatures
 * regardless of base order.  A base can be a vector, so its transport class
 * repeats for every component in the machine-readable row. */
static void
carry_sig(const struct r300_r2vb_producer_plan *plan, char *buf, size_t len)
{
   unsigned n = 0;
   for (unsigned i = 0; i < plan->partition.num_bases && n + 1 < len; i++) {
      char type;
      switch (plan->partition.r2vb_transport[i]) {
      case R300_MP_R2VB_SINT:   type = 'i'; break;
      case R300_MP_R2VB_UINT:   type = 'u'; break;
      case R300_MP_R2VB_BOOL1:
      case R300_MP_R2VB_BOOL32: type = 'b'; break;
      default:                  type = 'f'; break;
      }
      unsigned width = plan->partition.bases[i]->num_components;
      for (unsigned component = 0;
           component < width && n + 1 < len; component++) {
         buf[n++] = type;
      }
   }
   buf[n] = '\0';
   for (char *a = buf; *a; a++)
      for (char *b = a + 1; *b; b++)
         if (*b < *a) {
            char t = *a;
            *a = *b;
            *b = t;
         }
}

/* The retained filename shape: "r2vb-vs-" + 64 lowercase hex + ".nir".
 * _mesa_blake3_format emits lowercase hex (mesa_bytes_to_hex), so the
 * comparison below is exact, never case-folded. */
#define CORPUS_NAME_PREFIX "r2vb-vs-"
#define CORPUS_NAME_SUFFIX ".nir"
#define CORPUS_HEX_LEN     (BLAKE3_HEX_LEN - 1)
#define CORPUS_NAME_LEN                                       \
   (sizeof(CORPUS_NAME_PREFIX) - 1 + CORPUS_HEX_LEN +         \
    sizeof(CORPUS_NAME_SUFFIX) - 1)

/* Blob size ceiling: a retained application VS serializes to kilobytes, so
 * the ceiling guards the census against reading an arbitrarily large
 * foreign file while staying far above any real specimen. */
#define CORPUS_MAX_BLOB_BYTES (32u << 20)

#define CORPUS_PROVENANCE_NAME "corpus-provenance.txt"
#define CORPUS_PROVENANCE_KEY  "mesa_git_sha1="

static bool
is_corpus_name(const char *name)
{
   if (strlen(name) != CORPUS_NAME_LEN)
      return false;
   if (strncmp(name, CORPUS_NAME_PREFIX, sizeof(CORPUS_NAME_PREFIX) - 1))
      return false;
   const char *hex = name + sizeof(CORPUS_NAME_PREFIX) - 1;
   for (unsigned i = 0; i < CORPUS_HEX_LEN; i++) {
      char c = hex[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   return strcmp(hex + CORPUS_HEX_LEN, CORPUS_NAME_SUFFIX) == 0;
}

static int
name_cmp(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Read the whole file into a malloc'd buffer; returns NULL with *size zero
 * on any read failure, including an empty file (a zero-byte blob can never
 * verify against a content-hash filename). */
static uint8_t *
read_file(const char *path, size_t *size)
{
   *size = 0;
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0) {
      fclose(f);
      return NULL;
   }
   long len = ftell(f);
   if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
      fclose(f);
      return NULL;
   }
   uint8_t *data = malloc((size_t)len);
   if (!data) {
      fclose(f);
      return NULL;
   }
   if (fread(data, 1, (size_t)len, f) != (size_t)len) {
      free(data);
      fclose(f);
      return NULL;
   }
   fclose(f);
   *size = (size_t)len;
   return data;
}

/* The loader's own source identity: MESA_GIT_SHA1 renders as
 * " (git-<short sha>)", and an out-of-git build renders empty, which can
 * never verify a corpus. */
static void
loader_git_sha(char *buf, size_t len)
{
   const char *s = MESA_GIT_SHA1;
   const char *open = strstr(s, "(git-");
   buf[0] = '\0';
   if (!open)
      return;
   open += strlen("(git-");
   const char *close = strchr(open, ')');
   if (!close || (size_t)(close - open) >= len)
      return;
   memcpy(buf, open, (size_t)(close - open));
   buf[close - open] = '\0';
}

/* Corpus producer identity from corpus-provenance.txt: the value of the
 * first mesa_git_sha1= line, empty when the manifest or the line is
 * absent. */
static void
corpus_git_sha(const char *dir, char *buf, size_t len)
{
   buf[0] = '\0';
   char path[1024];
   int need = snprintf(path, sizeof(path), "%s/%s", dir,
                       CORPUS_PROVENANCE_NAME);
   if (need < 0 || (size_t)need >= sizeof(path))
      return;
   FILE *f = fopen(path, "r");
   if (!f)
      return;
   char line[256];
   while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, CORPUS_PROVENANCE_KEY,
                  strlen(CORPUS_PROVENANCE_KEY)) != 0)
         continue;
      const char *value = line + strlen(CORPUS_PROVENANCE_KEY);
      size_t n = strcspn(value, " \t\r\n");
      if (n == 0 || n >= len)
         break;
      memcpy(buf, value, n);
      buf[n] = '\0';
      break;
   }
   fclose(f);
}

struct census_totals {
   unsigned files;
   unsigned verified;
   unsigned guard_failures;
   unsigned hash_mismatches;
   unsigned deserialize_failures;
   unsigned decode_crashes;
   unsigned plan_records;
};

/* Per-specimen outcome, exit-code-encodable so the forked child reports
 * through waitpid: 10+records for a fully planned specimen, one code per
 * failure class otherwise. */
enum specimen_outcome {
   SPECIMEN_PLANNED_BASE = 10, /* + number of plan records (0..4) */
   SPECIMEN_GUARD_FAILURE = 20,
   SPECIMEN_HASH_MISMATCH = 21,
   SPECIMEN_DESERIALIZE_FAILURE = 22,
   SPECIMEN_PLANNER_TRANSIENT = 23,
};

/* Render and print the census row for one plan: the same stable view the
 * embedded-fixture census emits, keyed here by the retained filename and the
 * full (cv, space) plan key instead of a fixture name. */
static void
print_row(const char *specimen, bool cv, const char *space_name,
          const struct r300_r2vb_producer_plan *plan)
{
   char sig[R300_MP_MAX_CARRY_COMPS + 1] = "";
   if (plan->action == R300_R2VB_PLAN_SPLIT)
      carry_sig(plan, sig, sizeof(sig));
   printf("census specimen=%s cv=%d space=%s status=%s action=%s primary=%s "
          "mask=0x%" PRIx64 " inputs=%u typed=%d class=%s carries=%s "
          "baseline=%u/%u/%u passA=%u/%u/%u passB=%u/%u/%u\n",
          specimen, cv, space_name, plan_status_str(plan->status),
          r300_r2vb_plan_action_str(plan->action),
          r300_r2vb_plan_reason_str(plan->primary_reason),
          plan->observed_reason_mask, plan->num_position_inputs,
          plan->has_typed_source, typed_class_str(plan->typed_source_class),
          sig,
          plan->baseline.alu, plan->baseline.temps, plan->baseline.consts,
          plan->pass_a_cost.alu, plan->pass_a_cost.temps,
          plan->pass_a_cost.consts,
          plan->pass_b_cost.alu, plan->pass_b_cost.temps,
          plan->pass_b_cost.consts);
}

/* Verify, decode, and plan one specimen; prints its own transcript lines.
 * Runs in the forked child under the default isolation mode, so a
 * fail-stop abort inside nir_deserialize kills only this entry. */
static int
run_specimen(const char *dir, const char *name)
{
   char label[192];
   char path[1024];

   int need = snprintf(path, sizeof(path), "%s/%s", dir, name);
   snprintf(label, sizeof(label), "%s path fits", name);
   if (need < 0 || (size_t)need >= sizeof(path)) {
      printf("  FAIL - %s\n", label);
      return SPECIMEN_GUARD_FAILURE;
   }

   /* Entry guards before any byte is trusted: a regular non-symlink file
    * within the blob ceiling.  lstat sees the link itself, so a symlink
    * pointing at a valid blob still fails closed. */
   struct stat st;
   snprintf(label, sizeof(label),
            "%s is a regular non-symlink file within %u bytes", name,
            CORPUS_MAX_BLOB_BYTES);
   if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
       st.st_size <= 0 || (uintmax_t)st.st_size > CORPUS_MAX_BLOB_BYTES) {
      printf("  FAIL - %s\n", label);
      return SPECIMEN_GUARD_FAILURE;
   }

   size_t size;
   uint8_t *data = read_file(path, &size);
   snprintf(label, sizeof(label), "%s reads", name);
   if (!data) {
      printf("  FAIL - %s\n", label);
      return SPECIMEN_GUARD_FAILURE;
   }

   /* The filename is the content hash; a mismatch means foreign or damaged
    * bytes, and the census never plans unverified data. */
   blake3_hash hash;
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_compute(data, size, hash);
   _mesa_blake3_format(hex, hash);
   snprintf(label, sizeof(label), "%s content hash matches filename", name);
   if (strncmp(hex, name + sizeof(CORPUS_NAME_PREFIX) - 1,
               CORPUS_HEX_LEN) != 0) {
      printf("  FAIL - %s\n", label);
      free(data);
      return SPECIMEN_HASH_MISMATCH;
   }
   printf("  ok   - %s\n", label);

   /* Deserialize under the screen's production vertex options, the options
    * the retained shader was built with (the blob carries no options).  The
    * reader must land exactly on the blob end with no overrun: a verified
    * hash proves the bytes, and the end check proves the decode consumed
    * them as one whole shader.  nir_deserialize trusts its input -- it runs
    * nir_validate_shader internally, and on an asserts-enabled build a
    * malformed blob is fail-stop there; child isolation converts that
    * abort into this entry's decode_crash while the walk continues. */
   struct blob_reader reader;
   blob_reader_init(&reader, data, size);
   nir_shader *vs = nir_deserialize(
      NULL, g_screen.screen.nir_options[MESA_SHADER_VERTEX], &reader);
   bool decoded = vs && !reader.overrun && reader.current == reader.end &&
                  vs->info.stage == MESA_SHADER_VERTEX;
   free(data);
   snprintf(label, sizeof(label), "%s deserializes as a vertex shader",
            name);
   if (!decoded) {
      printf("  FAIL - %s\n", label);
      if (vs)
         ralloc_free(vs);
      return SPECIMEN_DESERIALIZE_FAILURE;
   }
   printf("  ok   - %s\n", label);
   nir_validate_shader(vs, "r2vb retained corpus specimen");

   /* The planner is pure with respect to vs_nir (it works on clones), so
    * one deserialized shader serves all four plan keys. */
   static const enum r300_r2vb_position_space spaces[] = {
      R300_R2VB_POSITION_CLIP,
      R300_R2VB_POSITION_WINDOW,
   };
   unsigned records = 0;
   bool transient = false;
   for (unsigned cv = 0; cv < 2; cv++) {
      for (unsigned s = 0; s < ARRAY_SIZE(spaces); s++) {
         const char *space_name =
            spaces[s] == R300_R2VB_POSITION_WINDOW ? "window" : "clip";
         struct r300_r2vb_producer_plan plan;
         bool ran = r300_r2vb_plan_producer(&g_context, vs, cv == 1,
                                            spaces[s], &plan);
         if (!ran) {
            printf("  FAIL - %s cv=%u space=%s planner runs\n", name, cv,
                   space_name);
            transient = true;
            continue;
         }
         print_row(name, cv == 1, space_name, &plan);
         records++;
         r300_r2vb_plan_release(&plan);
      }
   }

   ralloc_free(vs);
   if (transient)
      return SPECIMEN_PLANNER_TRANSIENT;
   return SPECIMEN_PLANNED_BASE + (int)records;
}

/* Map one specimen's outcome code into the totals and the run verdict. */
static void
tally_outcome(const char *name, int code, struct census_totals *totals)
{
   if (code >= SPECIMEN_PLANNED_BASE &&
       code <= SPECIMEN_PLANNED_BASE + 4) {
      totals->verified++;
      totals->plan_records += (unsigned)(code - SPECIMEN_PLANNED_BASE);
      char label[192];
      snprintf(label, sizeof(label), "%s planned %d records", name,
               code - SPECIMEN_PLANNED_BASE);
      CHECK(code - SPECIMEN_PLANNED_BASE == 4, label);
      return;
   }
   g_failures++;
   switch (code) {
   case SPECIMEN_GUARD_FAILURE:
      totals->guard_failures++;
      break;
   case SPECIMEN_HASH_MISMATCH:
      totals->hash_mismatches++;
      break;
   case SPECIMEN_DESERIALIZE_FAILURE:
      totals->deserialize_failures++;
      totals->verified++;
      break;
   case SPECIMEN_PLANNER_TRANSIENT:
      totals->verified++;
      break;
   default:
      totals->guard_failures++;
      break;
   }
}

int
main(void)
{
   /* Line-buffered stdout: every transcript line and census row lands
    * before a downstream fail-stop, so the output names the last entry
    * processed even when a child aborts. */
   setvbuf(stdout, NULL, _IOLBF, 0);

   /* Exact-value corpus gate: unset and empty read as no corpus, and the
    * meson exitcode protocol turns 77 into SKIP. */
   const char *dir = getenv("R300_R2VB_RETAINED_CORPUS");
   if (!dir || !dir[0]) {
      printf("no corpus directory; SKIP\n");
      return 77;
   }

   DIR *d = opendir(dir);
   if (!d) {
      printf("FAIL - corpus directory %s does not open\n", dir);
      return 1;
   }

   /* Collect the matching names first and sort them, so the census output
    * is deterministic across filesystems and repeat runs diff cleanly. */
   char **names = NULL;
   unsigned num_names = 0, cap_names = 0;
   struct dirent *e;
   bool oom = false;
   while ((e = readdir(d))) {
      if (!is_corpus_name(e->d_name))
         continue;
      if (num_names == cap_names) {
         unsigned new_cap = cap_names ? cap_names * 2 : 16;
         char **grown = realloc(names, new_cap * sizeof(*names));
         if (!grown) {
            oom = true;
            break;
         }
         names = grown;
         cap_names = new_cap;
      }
      names[num_names] = strdup(e->d_name);
      if (!names[num_names]) {
         oom = true;
         break;
      }
      num_names++;
   }
   closedir(d);
   if (oom) {
      for (unsigned i = 0; i < num_names; i++)
         free(names[i]);
      free(names);
      printf("FAIL - corpus name collection out of memory\n");
      return 1;
   }
   if (num_names == 0) {
      free(names);
      printf("corpus directory %s holds no specimens; SKIP\n", dir);
      return 77;
   }

   /* Serialization compatibility applies to evidence-bearing corpora only.
    * An empty directory proves nothing, so its skip verdict precedes the
    * manifest check and does not require a producer identity. */
   char own_sha[64], producer_sha[64];
   loader_git_sha(own_sha, sizeof(own_sha));
   corpus_git_sha(dir, producer_sha, sizeof(producer_sha));
   bool sha_match = own_sha[0] && producer_sha[0] &&
                    strcmp(own_sha, producer_sha) == 0;
   const char *override_env = getenv("R300_R2VB_CORPUS_COMPAT_OVERRIDE");
   bool override = override_env && strcmp(override_env, "1") == 0;
   if (!sha_match && !override) {
      printf("FAIL - corpus provenance %s/%s (producer mesa_git_sha1=%s) "
             "does not match this loader (%s); serialized NIR is source-"
             "tree-bound, so set R300_R2VB_CORPUS_COMPAT_OVERRIDE=1 only "
             "for research reads\n",
             dir, CORPUS_PROVENANCE_NAME,
             producer_sha[0] ? producer_sha : "absent",
             own_sha[0] ? own_sha : "absent");
      for (unsigned i = 0; i < num_names; i++)
         free(names[i]);
      free(names);
      return 1;
   }
   qsort(names, num_names, sizeof(*names), name_cmp);

   glsl_type_singleton_init_or_ref();
   fake_stack_init();

   /* The provenance header every reducer joins on: the census replans under
    * the canonical viewport recorded here, never the retaining process's
    * live viewport. */
   printf("census-provenance mode=canonical-replan "
          "viewport_scale=%g,%g,%g viewport_translate=%g,%g,%g "
          "loader_mesa_git_sha1=%s corpus_mesa_git_sha1=%s "
          "serialization_compatibility=%s specimens=%u\n",
          CANON_VP_SCALE_X, CANON_VP_SCALE_Y, CANON_VP_SCALE_Z,
          CANON_VP_SCALE_X, CANON_VP_SCALE_Y, CANON_VP_SCALE_Z,
          own_sha[0] ? own_sha : "absent",
          producer_sha[0] ? producer_sha : "absent",
          sha_match ? "verified" : "unverified", num_names);

   static int no_fork = -1;
   if (no_fork < 0) {
      const char *nf = getenv("R300_R2VB_CORPUS_NO_FORK");
      no_fork = (nf && strcmp(nf, "1") == 0) ? 1 : 0;
   }

   struct census_totals totals = { 0 };
   for (unsigned i = 0; i < num_names; i++) {
      totals.files++;
      if (no_fork) {
         tally_outcome(names[i], run_specimen(dir, names[i]), &totals);
      } else {
         fflush(stdout);
         pid_t pid = fork();
         if (pid == 0) {
            _exit(run_specimen(dir, names[i]));
         } else if (pid < 0) {
            CHECK(false, "specimen child forks");
         } else {
            int status = 0;
            if (waitpid(pid, &status, 0) != pid) {
               CHECK(false, "specimen child reaps");
            } else if (WIFSIGNALED(status)) {
               /* The fail-stop decode abort: the entry is counted and the
                * walk continues, exactly what child isolation buys. */
               printf("  FAIL - %s decode child died on signal %d\n",
                      names[i], WTERMSIG(status));
               g_failures++;
               totals.decode_crashes++;
            } else {
               tally_outcome(names[i], WEXITSTATUS(status), &totals);
            }
         }
      }
      free(names[i]);
   }
   free(names);

   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   glsl_type_singleton_decref();

   printf("census-totals files=%u verified=%u guard_failures=%u "
          "hash_mismatches=%u deserialize_failures=%u decode_crashes=%u "
          "plan_records=%u serialization_compatibility=%s\n",
          totals.files, totals.verified, totals.guard_failures,
          totals.hash_mismatches, totals.deserialize_failures,
          totals.decode_crashes, totals.plan_records,
          sha_match ? "verified" : "unverified");
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
