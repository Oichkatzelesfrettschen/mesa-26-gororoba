/*
 * SPDX-License-Identifier: MIT
 */

/*
 * Calibration for the standing-route R2VB telemetry: known-good and
 * known-bad plans drive r300_r2vb_telemetry_note directly and the counters,
 * retention gate, and deduplication must respond exactly.  The retention
 * gate is fail-closed -- with R300_R2VB_TELEMETRY_RETAIN unset, an
 * over-budget plan writes nothing -- and the gate reads per event, so one
 * process exercises the closed and open states in order.  A structural
 * reject (control flow) carries no budget signal and never retains even
 * with the gate open.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nir.h"
#include "nir_builder.h"

#include "r300_context.h"
#include "r300_fs.h"
#include "r300_r2vb.h"
#include "r300_r2vb_plan.h"
#include "r300_r2vb_telemetry.h"
#include "r300_screen.h"
#include "r300_vs.h"
#include "radeon_regalloc.h"

/* The route owns these helpers in r300_r2vb.c.  Keep their declarations local
 * to this calibration so the production header surface stays unchanged. */
extern void r300_r2vb_telemetry_note_cell(
   struct r300_context *r300,
   const struct r300_r2vb_producer_plan *plan);
extern bool r300_r2vb_telemetry_allow_computed_varying(
   struct r300_context *r300, nir_shader *vs_nir);
extern unsigned r300_r2vb_telemetry_format_dwords(enum pipe_format format);
extern bool r300_r2vb_admits_producer_for_test(
   struct r300_context *r300, bool allow_computed_varying,
   enum r300_r2vb_position_space space);

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
static struct r300_vertex_shader g_vs;

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
   g_context.viewport.scale[0] = 320.0f;
   g_context.viewport.scale[1] = 240.0f;
   g_context.viewport.scale[2] = 0.5f;
   g_context.viewport.translate[0] = 320.0f;
   g_context.viewport.translate[1] = 240.0f;
   g_context.viewport.translate[2] = 0.5f;
}

/* Telemetry retention serializes the bound application VS, so the fake
 * context binds each specimen through the same pointer production uses. */
static void
bind_vs(nir_shader *nir)
{
   memset(&g_vs, 0, sizeof(g_vs));
   g_vs.state.type = PIPE_SHADER_IR_NIR;
   g_vs.state.ir.nir = nir;
   g_context.vs_state.state = &g_vs;
}

struct vs_build {
   nir_builder b;
   nir_def *pos;
   nir_variable *in_pos;
   nir_variable *out_pos;
};

static struct vs_build
begin_vs(const char *name)
{
   struct vs_build v;
   v.b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, g_screen.screen.nir_options[MESA_SHADER_VERTEX],
      "%s", name);
   v.in_pos = nir_variable_create(
      v.b.shader, nir_var_shader_in, glsl_vec4_type(), "in_pos");
   v.in_pos->data.location = VERT_ATTRIB_GENERIC0;
   v.in_pos->data.driver_location = 0;
   v.out_pos = nir_variable_create(v.b.shader, nir_var_shader_out,
                                   glsl_vec4_type(), "gl_Position");
   v.out_pos->data.location = VARYING_SLOT_POS;
   v.out_pos->data.driver_location = 0;
   v.pos = nir_load_var(&v.b, v.in_pos);
   return v;
}

static nir_shader *
end_vs(struct vs_build *v, nir_def *result)
{
   nir_store_var(&v->b, v->out_pos, result, 0xf);
   nir_validate_shader(v->b.shader, "r2vb telemetry VS");
   return v->b.shader;
}

static nir_def *
fmad_chain(nir_builder *b, nir_def *base, unsigned length)
{
   nir_def *v = base;
   for (unsigned i = 0; i < length; i++)
      v = nir_ffma(b, v, nir_imm_float(b, 1.0001f + (float)(i % 7) * 0.001f),
                   base);
   return v;
}

static nir_shader *
build_producer(unsigned chain_length, const char *name)
{
   struct vs_build v = begin_vs(name);
   nir_def *c = fmad_chain(&v.b, nir_channel(&v.b, v.pos, 0), chain_length);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

static nir_shader *
build_computed_varying(void)
{
   struct vs_build v = begin_vs("telemetry_computed_varying");
   nir_variable *out_varying = nir_variable_create(
      v.b.shader, nir_var_shader_out, glsl_vec4_type(), "computed_varying");
   out_varying->data.location = VARYING_SLOT_VAR0;
   nir_def *x = nir_fmul_imm(&v.b, v.pos, 2.0f);
   nir_store_var(&v.b, out_varying, x, 0xf);
   return end_vs(&v, v.pos);
}

/* A branch on a non-constant input survives every folding pass, so the plan
 * rejects with CONTROL_FLOW: the structural class retention must skip. */
static nir_shader *
build_control_flow(void)
{
   struct vs_build v = begin_vs("telemetry_control_flow");
   nir_def *x = nir_channel(&v.b, v.pos, 0);
   nir_def *cond = nir_flt_imm(&v.b, x, 0.5);
   nir_if *nif = nir_push_if(&v.b, cond);
   nir_def *then_v = nir_fmul_imm(&v.b, x, 2.0);
   nir_push_else(&v.b, nif);
   nir_def *else_v = nir_fadd_imm(&v.b, x, 1.0);
   nir_pop_if(&v.b, nif);
   nir_def *c = nir_if_phi(&v.b, then_v, else_v);
   return end_vs(&v, nir_replicate(&v.b, c, 4));
}

static unsigned
count_dir_files(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   unsigned n = 0;
   struct dirent *e;
   while ((e = readdir(d)))
      if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
         n++;
   closedir(d);
   return n;
}

/* Return the single retained file's path, or false when the directory does
 * not hold exactly one entry. */
static bool
single_dir_file(const char *dir, char *path, size_t path_size,
                size_t *name_len)
{
   DIR *d = opendir(dir);
   if (!d)
      return false;
   unsigned n = 0;
   struct dirent *e;
   char name[256] = { 0 };
   while ((e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;
      n++;
      snprintf(name, sizeof(name), "%s", e->d_name);
   }
   closedir(d);
   if (n != 1)
      return false;
   *name_len = strlen(name);
   snprintf(path, path_size, "%s/%s", dir, name);
   return true;
}

static long
file_size(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static mode_t
file_mode(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0 ? st.st_mode & 0777 : 0;
}

static bool
file_is_regular_nofollow(const char *path)
{
   struct stat st;
   return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Plan one specimen, assert the expected action (calibrating the specimen
 * itself against the planner), record it in telemetry, release it. */
static void
plan_and_note(const char *label, nir_shader *vs,
              enum r300_r2vb_plan_action expected_action)
{
   char name[96];
   bind_vs(vs);
   struct r300_r2vb_producer_plan plan;
   bool ran = r300_r2vb_plan_producer(&g_context, vs, false,
                                      R300_R2VB_POSITION_CLIP, &plan);
   snprintf(name, sizeof(name), "%s plans %s", label,
            r300_r2vb_plan_action_str(expected_action));
   CHECK(ran && plan.action == expected_action, name);
   if (ran) {
      g_vs.r2vb_admission[0][0] = R300_R2VB_ADMIT_FITS;
      r300_r2vb_telemetry_note_cell(&g_context, &plan);
      CHECK(g_vs.r2vb_admission[0][0] == R300_R2VB_ADMIT_FITS,
            "cell note leaves route memo unchanged");
      r300_r2vb_plan_release(&plan);
   }
}

static void
note_structural_admission_reject(const char *label, nir_shader *vs,
                                 enum r300_r2vb_plan_reason reason)
{
   bind_vs(vs);
   r300_r2vb_telemetry_cell_remove(&g_vs);
   const struct r300_r2vb_telemetry_counters *c =
      r300_r2vb_telemetry_get();
   unsigned before = c->by_reason[reason];
   bool admitted = r300_r2vb_admits_producer_for_test(
      &g_context, false, R300_R2VB_POSITION_CLIP);
   char name[96];
   snprintf(name, sizeof(name), "%s remains rejected by production admission",
            label);
   CHECK(!admitted, name);
   snprintf(name, sizeof(name), "%s records the production reject reason",
            label);
   CHECK(c->by_reason[reason] == before + 1, name);
   unsigned after = c->by_reason[reason];
   admitted = r300_r2vb_admits_producer_for_test(
      &g_context, false, R300_R2VB_POSITION_CLIP);
   snprintf(name, sizeof(name), "%s remains rejected on repeat", label);
   CHECK(!admitted, name);
   snprintf(name, sizeof(name), "%s records one cell on repeat", label);
   CHECK(c->by_reason[reason] == after, name);
}

static void
note_structural_admission_parity(const char *label, nir_shader *vs)
{
   bind_vs(vs);
   r300_r2vb_telemetry_cell_remove(&g_vs);
   const struct r300_r2vb_telemetry_counters *c =
      r300_r2vb_telemetry_get();
   struct r300_r2vb_telemetry_counters before = *c;
   const struct r300_r2vb_producer_plan *plan =
      r300_r2vb_producer_plan_get(&g_context, false,
                                  R300_R2VB_POSITION_CLIP);
   char name[96];
   snprintf(name, sizeof(name), "%s matches the SINGLE planner action", label);
   CHECK(plan && plan->action == R300_R2VB_PLAN_SINGLE, name);
   bool admitted = r300_r2vb_admits_producer_for_test(
      &g_context, false, R300_R2VB_POSITION_CLIP);
   snprintf(name, sizeof(name), "%s remains admitted by production", label);
   CHECK(admitted, name);
   snprintf(name, sizeof(name), "%s records a FITS route memo", label);
   CHECK(g_vs.r2vb_admission[0][0] == R300_R2VB_ADMIT_FITS, name);
   snprintf(name, sizeof(name), "%s records the planner OK reason", label);
   CHECK(c->by_reason[R300_R2VB_PLAN_OK] ==
            before.by_reason[R300_R2VB_PLAN_OK] + 1, name);
   snprintf(name, sizeof(name), "%s records no IO-shape rejection", label);
   CHECK(c->by_reason[R300_R2VB_PLAN_IO_SHAPE] ==
            before.by_reason[R300_R2VB_PLAN_IO_SHAPE], name);
}

static void
note_structural_admission_good_plan(const char *label, nir_shader *vs)
{
   bind_vs(vs);
   r300_r2vb_telemetry_cell_remove(&g_vs);
   const struct r300_r2vb_telemetry_counters *c =
      r300_r2vb_telemetry_get();
   struct r300_r2vb_telemetry_counters before = *c;
   r300_r2vb_telemetry_note_structural_admission_reject(
      &g_context, false, R300_R2VB_POSITION_CLIP);
   char name[96];
   snprintf(name, sizeof(name), "%s leaves the cached plan unchanged", label);
   CHECK(memcmp(c, &before, sizeof(before)) == 0, name);
}

static void
case_sparse_telemetry_input_ranks(void)
{
   struct r300_r2vb_producer_plan plan = {0};
   plan.position_source.valid = true;
   plan.position_source.location_rank = 1;
   plan.varying_source.valid = true;
   plan.varying_source.location_rank = 2;
   uint8_t ranks[2] = {0, 0};
   unsigned count = r300_r2vb_telemetry_source_ranks_for_test(
      &plan, true, ranks, 2);
   CHECK(count == 2 && ranks[0] == 1 && ranks[1] == 2,
         "telemetry follows measured sparse input ranks");
   count = r300_r2vb_telemetry_source_ranks_for_test(
      &plan, false, ranks, 2);
   CHECK(count == 1 && ranks[0] == 1,
         "position-only telemetry omits varying rank");
}

static void
case_multi_position_telemetry_sources(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX,
      g_screen.screen.nir_options[MESA_SHADER_VERTEX], "telemetry_multi_position");
   nir_variable *late = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "late");
   late->data.location = VERT_ATTRIB_GENERIC0 + 3;
   late->data.driver_location = 7;
   nir_variable *early = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "early");
   early->data.location = VERT_ATTRIB_GENERIC0 + 1;
   early->data.driver_location = 2;
   nir_variable *out_pos = nir_variable_create(
      b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
   out_pos->data.location = VARYING_SLOT_POS;
   nir_store_var(&b, out_pos,
                 nir_fadd(&b, nir_load_var(&b, late),
                          nir_load_var(&b, early)), 0xf);
   nir_validate_shader(b.shader, "multi position telemetry VS");

   struct r300_r2vb_producer_plan plan = {0};
   plan.num_position_inputs = 2;
   uint8_t drivers[2] = {0, 0};
   uint8_t ranks[2] = {0, 0};
   unsigned count = r300_r2vb_telemetry_position_sources_for_test(
      &plan, b.shader, drivers, ranks, ARRAY_SIZE(drivers));
   CHECK(count == 2 && drivers[0] == 2 && drivers[1] == 7 &&
            ranks[0] == 0 && ranks[1] == 1,
         "multi-input telemetry keeps every measured physical source");
   CHECK(r300_r2vb_input_velem_index_for_test(b.shader, late) == 7 &&
            r300_r2vb_input_velem_index_for_test(b.shader, early) == 2,
         "telemetry source identity maps velems by driver location");
   ralloc_free(b.shader);
}

int
main(void)
{
   /* The retention calibration owns the print-gate state. */
   setenv("R300_R2VB_TELEMETRY", "0", 1);
   glsl_type_singleton_init_or_ref();
   fake_stack_init();
   mode_t saved_umask = umask(022);

   char retain_dir[] = "r2vb-telemetry-retain-XXXXXX";
   CHECK(mkdtemp(retain_dir) != NULL, "retain directory created");

   nir_shader *fits = build_producer(8, "telemetry_fits");
   nir_shader *over = build_producer(90, "telemetry_over");
   nir_shader *over_renamed = build_producer(90, "telemetry_over_renamed");
   nir_shader *cflow = build_control_flow();
   nir_shader *computed = build_computed_varying();

   CHECK(r300_r2vb_telemetry_format_dwords(PIPE_FORMAT_R8_UNORM) == 1,
         "format dwords rounds one-byte blocks up");
   CHECK(r300_r2vb_telemetry_format_dwords(PIPE_FORMAT_R16G16_FLOAT) == 1,
         "format dwords preserves four-byte blocks");
   CHECK(r300_r2vb_telemetry_format_dwords(PIPE_FORMAT_R32G32_FLOAT) == 2,
         "format dwords preserves eight-byte blocks");

   unsetenv("R300_R2VB_VARYING");
   CHECK(!r300_r2vb_telemetry_allow_computed_varying(&g_context, computed),
         "computed-varying cell stays closed by default");
   setenv("R300_R2VB_VARYING", "1", 1);
   CHECK(r300_r2vb_telemetry_allow_computed_varying(&g_context, computed),
         "computed-varying cell follows the exact route gate");
   setenv("R300_R2VB_VARYING", "0", 1);
   CHECK(!r300_r2vb_telemetry_allow_computed_varying(&g_context, computed),
         "computed-varying near miss stays closed");
   unsetenv("R300_R2VB_VARYING");

   const struct r300_r2vb_telemetry_counters *c =
      r300_r2vb_telemetry_get();
   struct r300_r2vb_telemetry_counters base = *c;

   /* Retention closed: the gate is unset, so even the over-budget plan
    * writes nothing. */
   unsetenv("R300_R2VB_TELEMETRY_RETAIN");
   plan_and_note("fits", fits, R300_R2VB_PLAN_SINGLE);
   plan_and_note("over/closed", over, R300_R2VB_PLAN_SPLIT);
   CHECK(c->by_action[R300_R2VB_PLAN_SINGLE] ==
            base.by_action[R300_R2VB_PLAN_SINGLE] + 1,
         "single counter counts");
   CHECK(c->by_action[R300_R2VB_PLAN_SPLIT] ==
            base.by_action[R300_R2VB_PLAN_SPLIT] + 1,
         "split counter counts");
   CHECK(c->retained == base.retained && count_dir_files(retain_dir) == 0,
         "closed gate retains nothing");

   /* The exact value "0" disables the retention directory. */
   setenv("R300_R2VB_TELEMETRY_RETAIN", "0", 1);
   struct r300_r2vb_telemetry_counters zero_base = *c;
   CHECK(!r300_r2vb_telemetry_observation_enabled(),
         "zero gate disables retention observation");
   plan_and_note("over/zero", over, R300_R2VB_PLAN_SPLIT);
   CHECK(c->retained == zero_base.retained &&
            c->retain_failures == zero_base.retain_failures &&
            count_dir_files(retain_dir) == 0,
         "zero gate retains nothing");

   /* Retention open: the split plan writes one content-hash file, and a
    * repeat of the same shape deduplicates on the existing file. */
   setenv("R300_R2VB_TELEMETRY_RETAIN", retain_dir, 1);
   plan_and_note("over/open", over, R300_R2VB_PLAN_SPLIT);
   CHECK(c->retained == base.retained + 1 &&
            count_dir_files(retain_dir) == 1,
         "open gate retains the over-budget producer once");
   plan_and_note("over/dedup", over, R300_R2VB_PLAN_SPLIT);
   CHECK(c->retained == base.retained + 1 &&
            count_dir_files(retain_dir) == 1,
         "recurring shape deduplicates on the content hash");
   plan_and_note("over/renamed", over_renamed, R300_R2VB_PLAN_SPLIT);
   CHECK(c->retained == base.retained + 1 &&
            count_dir_files(retain_dir) == 1,
         "renamed structural twin deduplicates on the stripped hash");

   bind_vs(over);
   struct r300_r2vb_producer_plan dedup_plan;
   bool dedup_ran = r300_r2vb_plan_producer(
      &g_context, over, false, R300_R2VB_POSITION_CLIP, &dedup_plan);
   CHECK(dedup_ran, "dedup plan remains available");
   if (dedup_ran) {
      unsigned before = c->by_action[R300_R2VB_PLAN_SPLIT];
      r300_r2vb_telemetry_cell_remove(&g_vs);
      r300_r2vb_telemetry_note_cell(&g_context, &dedup_plan);
      CHECK(c->by_action[R300_R2VB_PLAN_SPLIT] == before + 1,
            "destroyed shader identity can publish a new telemetry cell");

      struct pipe_draw_info split_info = { 0 };
      struct pipe_draw_start_count_bias split_draw = { 0 };
      split_info.mode = MESA_PRIM_TRIANGLES;
      split_info.instance_count = 1;
      split_draw.count = 3;
      r300_r2vb_telemetry_draw(&g_context, &dedup_plan, &split_info,
                               &split_draw);
      struct r300_r2vb_workload_stats split_stats;
      CHECK(r300_r2vb_telemetry_workload_stats(
               g_vs.r2vb_content_hex, R300_R2VB_PLAN_SPLIT, &split_stats) &&
               split_stats.action == 'P',
            "workload: split action uses a distinct code");
      r300_r2vb_plan_release(&dedup_plan);
   }

   /* The filename carries the full BLAKE3 hex digest:
    * "r2vb-vs-" + 64 hex + ".nir". */
   char retained_path[512];
   size_t retained_name_len = 0;
   bool have_file = single_dir_file(retain_dir, retained_path,
                                    sizeof(retained_path),
                                    &retained_name_len);
   CHECK(have_file && retained_name_len == strlen("r2vb-vs-") + 64 +
                                              strlen(".nir"),
         "retained filename carries the full content hash");
   CHECK(have_file && file_mode(retained_path) == 0644,
         "retained blob is readable by shared corpus consumers");

   /* Matching blobs receive the effective mode for the current publisher. */
   CHECK(have_file && chmod(retained_path, 0600) == 0,
         "dedup setup restricts the retained blob");
   bind_vs(over);
   struct r300_r2vb_producer_plan mode_plan;
   bool mode_plan_ran = r300_r2vb_plan_producer(
      &g_context, over, false, R300_R2VB_POSITION_CLIP, &mode_plan);
   CHECK(mode_plan_ran, "dedup mode plan remains available");
   if (mode_plan_ran) {
      r300_r2vb_telemetry_note(&g_context, &mode_plan);
      CHECK(c->retain_failures == base.retain_failures &&
               file_mode(retained_path) == 0644,
            "matching blob receives the effective sharing mode");
      r300_r2vb_plan_release(&mode_plan);
   }

   /* A matching effective mode keeps the descriptor-bound deduplication path
    * free of an ownership-changing fchmod operation. */
   unsigned matching_mode_failures = c->retain_failures;
   bind_vs(over);
   struct r300_r2vb_producer_plan matching_mode_plan;
   bool matching_mode_ran = r300_r2vb_plan_producer(
      &g_context, over, false, R300_R2VB_POSITION_CLIP, &matching_mode_plan);
   CHECK(matching_mode_ran, "matching mode plan remains available");
   if (matching_mode_ran) {
      r300_r2vb_telemetry_note(&g_context, &matching_mode_plan);
      CHECK(c->retain_failures == matching_mode_failures &&
               file_mode(retained_path) == 0644,
            "matching mode skips the ownership-changing operation");
      r300_r2vb_plan_release(&matching_mode_plan);
   }

   /* Known-bad file at the final name: dedup verifies bytes, so a damaged
    * entry is republished with the correct content. */
   long good_size = have_file ? file_size(retained_path) : -1;
   if (have_file) {
      FILE *f = fopen(retained_path, "wb");
      CHECK(f && fputs("damaged", f) >= 0 && fclose(f) == 0,
            "damaged the retained file in place");
   }
   plan_and_note("over/republish", over, R300_R2VB_PLAN_SPLIT);
   /* Cell observation deduplicates route facts, while retention still
    * verifies an existing file when the caller explicitly records the
    * specimen again. */
   bind_vs(over);
   struct r300_r2vb_producer_plan republish_plan;
   bool republish_ran = r300_r2vb_plan_producer(
      &g_context, over, false, R300_R2VB_POSITION_CLIP, &republish_plan);
   CHECK(republish_ran, "republish plan remains available");
   if (republish_ran) {
      mode_t old_umask = umask(077);
      r300_r2vb_telemetry_note(&g_context, &republish_plan);
      umask(old_umask);
      r300_r2vb_plan_release(&republish_plan);
   }
   CHECK(c->retained == base.retained + 2 &&
            count_dir_files(retain_dir) == 1 &&
            have_file && file_size(retained_path) == good_size &&
            file_mode(retained_path) == 0600,
         "mismatching existing file republishes atomically with umask");

   /* A final-name symlink enters publication through O_NOFOLLOW.  The
    * unrelated target keeps its bytes and mode while rename replaces the
    * symlink with the validated blob. */
   char symlink_target[512];
   int target_need = snprintf(symlink_target, sizeof(symlink_target),
                              "%s/unrelated-target", retain_dir);
   CHECK(target_need >= 0 && (size_t)target_need < sizeof(symlink_target),
         "symlink target path fits");
   if (target_need >= 0 && (size_t)target_need < sizeof(symlink_target)) {
      FILE *target = fopen(symlink_target, "wb");
      CHECK(target && fputs("unrelated", target) >= 0 && fclose(target) == 0,
            "unrelated target is created");
      CHECK(chmod(symlink_target, 0600) == 0,
            "unrelated target mode is restricted");
      CHECK(unlink(retained_path) == 0 &&
               symlink("unrelated-target", retained_path) == 0,
            "retained name becomes a symlink to the unrelated target");
      unsigned symlink_failures = c->retain_failures;
      plan_and_note("over/symlink", over, R300_R2VB_PLAN_SPLIT);
      bind_vs(over);
      struct r300_r2vb_producer_plan symlink_plan;
      bool symlink_plan_ran = r300_r2vb_plan_producer(
         &g_context, over, false, R300_R2VB_POSITION_CLIP, &symlink_plan);
      CHECK(symlink_plan_ran, "symlink publication plan remains available");
      if (symlink_plan_ran) {
         r300_r2vb_telemetry_note(&g_context, &symlink_plan);
         CHECK(c->retain_failures == symlink_failures &&
                  file_size(symlink_target) == (long)strlen("unrelated") &&
                  file_mode(symlink_target) == 0600 &&
                  file_size(retained_path) == good_size &&
                  file_is_regular_nofollow(retained_path),
               "symlink target stays untouched during publication");
         r300_r2vb_plan_release(&symlink_plan);
      }
      unlink(symlink_target);
   }

   /* Structural reject: control flow carries no budget signal. */
   plan_and_note("control-flow", cflow, R300_R2VB_PLAN_REJECT);
   CHECK(c->by_reason[R300_R2VB_PLAN_CONTROL_FLOW] ==
            base.by_reason[R300_R2VB_PLAN_CONTROL_FLOW] + 1,
         "control-flow reason counts");
   CHECK(count_dir_files(retain_dir) == 1,
         "structural reject retains nothing");
   CHECK(c->retain_failures == base.retain_failures,
         "no retention failures");

   /* A structurally rejected VS records the cached reject plan.  A fitting
    * plan is a negative control: the bridge records no event when the cached
    * plan is not a reject. */
   note_structural_admission_reject(
      "control-flow admission", cflow, R300_R2VB_PLAN_CONTROL_FLOW);
   note_structural_admission_parity("uniform-free admission", fits);
   note_structural_admission_good_plan("fitting admission", fits);

   case_sparse_telemetry_input_ranks();
   case_multi_position_telemetry_sources();

   /* Retention-scope parser: unset, empty, budget, and every unrecognized
    * value keep the established budget-only policy; single, structural,
    * and all are exact. */
   CHECK(r300_r2vb_telemetry_retain_scope_value(NULL) ==
            R300_R2VB_TELEMETRY_RETAIN_BUDGET,
         "scope: unset keeps budget");
   CHECK(r300_r2vb_telemetry_retain_scope_value("") ==
            R300_R2VB_TELEMETRY_RETAIN_BUDGET,
         "scope: empty keeps budget");
   CHECK(r300_r2vb_telemetry_retain_scope_value("budget") ==
            R300_R2VB_TELEMETRY_RETAIN_BUDGET,
         "scope: budget names budget");
   CHECK(r300_r2vb_telemetry_retain_scope_value("Single") ==
            R300_R2VB_TELEMETRY_RETAIN_BUDGET,
         "scope: near-miss keeps budget");
   CHECK(r300_r2vb_telemetry_retain_scope_value("single") ==
            R300_R2VB_TELEMETRY_RETAIN_SINGLE,
         "scope: single exact");
   CHECK(r300_r2vb_telemetry_retain_scope_value("structural") ==
            R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL,
         "scope: structural exact");
   CHECK(r300_r2vb_telemetry_retain_scope_value("all") ==
            R300_R2VB_TELEMETRY_RETAIN_ALL,
         "scope: all exact");

   /* Eligibility matrix over synthetic plans: each scope admits exactly its
    * class.  A SPLIT plan is the budget shape; a SINGLE plan is the fitting
    * shape; a control-flow reject is the structural shape. */
   {
      struct r300_r2vb_producer_plan p;
      memset(&p, 0, sizeof(p));
      p.action = R300_R2VB_PLAN_SPLIT;
      CHECK(r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_BUDGET) &&
            !r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_SINGLE) &&
            !r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL) &&
            r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_ALL),
            "eligibility: split -> budget+all only");
      p.action = R300_R2VB_PLAN_SINGLE;
      CHECK(!r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_BUDGET) &&
            r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_SINGLE) &&
            !r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL),
            "eligibility: single -> single scope only");
      p.action = R300_R2VB_PLAN_REJECT;
      p.primary_reason = R300_R2VB_PLAN_CONTROL_FLOW;
      CHECK(!r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_BUDGET) &&
            !r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_SINGLE) &&
            r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL),
            "eligibility: control-flow reject -> structural scope only");
      p.primary_reason = R300_R2VB_PLAN_SIGNED_RANGE;
      CHECK(r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_BUDGET) &&
            !r300_r2vb_telemetry_retain_eligible_in_scope(
               &p, R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL),
            "eligibility: range reject stays budget-scoped");
   }

   /* Content hash and workload weight: the first note cached the bound
    * shader's full BLAKE3 on the VS, and per-draw accounting keyed on it
    * sums draws, vertices, and instances, tracks draw-size extrema and the
    * topology mask, and counts indexed draws. */
   {
      bind_vs(fits);
      struct r300_r2vb_producer_plan plan;
      bool ran = r300_r2vb_plan_producer(&g_context, fits, false,
                                         R300_R2VB_POSITION_CLIP, &plan);
      CHECK(ran, "workload: plan for the fitting specimen");
      if (ran) {
         r300_r2vb_telemetry_note(&g_context, &plan);
         const char *hex = g_vs.r2vb_content_hex;

         struct pipe_draw_info info = { 0 };
         struct pipe_draw_start_count_bias dr = { 0 };
         info.mode = MESA_PRIM_TRIANGLES;
         info.instance_count = 1;
         dr.count = 3;
         r300_r2vb_telemetry_draw(&g_context, &plan, &info, &dr);
         info.mode = MESA_PRIM_TRIANGLE_STRIP;
         info.instance_count = 2;
         info.index_size = 2;
         dr.count = 300;
         r300_r2vb_telemetry_draw(&g_context, &plan, &info, &dr);

         /* The first draw computed and cached the full BLAKE3 on the VS
          * (the print-gated note skips the hash when the gate is closed). */
         CHECK(hex[0] != 0 && strlen(hex) == 64,
               "workload: full BLAKE3 cached on the VS");

         struct r300_r2vb_workload_stats st;
         CHECK(r300_r2vb_telemetry_workload_stats(
                  hex, R300_R2VB_PLAN_SINGLE, &st),
               "workload: stats found by hash");
         CHECK(st.draws == 2 && st.vertices == 3 + 300 * 2 &&
                  st.instances == 3 && st.draw_min == 3 &&
                  st.draw_max == 300 && st.indexed_draws == 1 &&
                  st.action == 'N' &&
                  st.topology_mask ==
                     ((1u << MESA_PRIM_TRIANGLES) |
                      (1u << MESA_PRIM_TRIANGLE_STRIP)),
               "workload: draws/vertices/extrema/topology/indexed sum");
         CHECK(!r300_r2vb_telemetry_workload_stats(
                  "0000000000000000000000000000000000000000000000000000000000000000",
                  R300_R2VB_PLAN_SINGLE, &st),
               "workload: unseen hash reports absent");
         r300_r2vb_plan_release(&plan);
      }
   }

   /* Summary is print-gated; ungated it must be silent and safe. */
   r300_r2vb_telemetry_print_summary();

   /* Context-epoch accounting: with two live contexts the first destruction
    * stays quiet and the last one prints the cumulative summary (silent here
    * with the gate closed; the walk itself must be safe). */
   r300_r2vb_telemetry_context_created();
   r300_r2vb_telemetry_context_created();
   r300_r2vb_telemetry_context_destroyed();
   r300_r2vb_telemetry_context_destroyed();

   ralloc_free(fits);
   ralloc_free(over);
   ralloc_free(over_renamed);
   ralloc_free(cflow);
   ralloc_free(computed);
   rc_destroy_regalloc_state(&g_context.fs_regalloc_state);
   umask(saved_umask);
   glsl_type_singleton_decref();
   printf(g_failures ? "FAILED (%u)\n" : "PASSED\n", g_failures);
   return g_failures ? 1 : 0;
}
