/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_telemetry.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nir.h"
#include "nir_serialize.h"
#include "util/blob.h"
#include "util/hash_table.h"
#include "util/mesa-blake3.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"

#include "r300_context.h"
#include "r300_vs.h"

static struct r300_r2vb_telemetry_counters counters;
static uint32_t telemetry_temp_serial;

/* Contexts sharing the process share the counters; the summary prints when
 * the last live context goes away, so a multi-context run reports one
 * cumulative total per context epoch. */
static uint32_t live_contexts;

/* The per-event print gate takes the exact value 1; unset, empty, and every
 * other value keep it closed. */
static bool
telemetry_print_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("R300_R2VB_TELEMETRY");
        enabled = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    return enabled == 1;
}

/* The retain directory reads per event: retention runs only on the cold
 * once-per-cell classification path, and an uncached read lets the
 * calibration test exercise closed and open states in one process. */
static const char *
telemetry_retain_dir(void)
{
    const char *dir = getenv("R300_R2VB_TELEMETRY_RETAIN");
    return (dir && dir[0] && strcmp(dir, "0") != 0) ? dir : NULL;
}

enum r300_r2vb_telemetry_retain_scope
r300_r2vb_telemetry_retain_scope_value(const char *value)
{
    if (!value)
        return R300_R2VB_TELEMETRY_RETAIN_BUDGET;
    if (strcmp(value, "single") == 0)
        return R300_R2VB_TELEMETRY_RETAIN_SINGLE;
    if (strcmp(value, "structural") == 0)
        return R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL;
    if (strcmp(value, "all") == 0)
        return R300_R2VB_TELEMETRY_RETAIN_ALL;
    /* budget, unset, empty, and every unrecognized value keep the
     * established budget-only policy: a typo can never widen retention. */
    return R300_R2VB_TELEMETRY_RETAIN_BUDGET;
}

static enum r300_r2vb_telemetry_retain_scope
telemetry_retain_scope(void)
{
    static int scope = -1;
    if (scope < 0)
        scope = (int)r300_r2vb_telemetry_retain_scope_value(
            getenv("R300_R2VB_TELEMETRY_RETAIN_SCOPE"));
    return (enum r300_r2vb_telemetry_retain_scope)scope;
}

/* A structural reject's cause is a property of the shader's shape rather
 * than any budget: control flow, the intrinsic set, I/O shape, or a
 * non-budget backend rejection. */
static bool
telemetry_reject_is_structural(const struct r300_r2vb_producer_plan *plan)
{
    switch (plan->primary_reason) {
    case R300_R2VB_PLAN_CONTROL_FLOW:
    case R300_R2VB_PLAN_INTRINSIC:
    case R300_R2VB_PLAN_IO_SHAPE:
    case R300_R2VB_PLAN_BACKEND:
        return true;
    default:
        return false;
    }
}

bool
r300_r2vb_telemetry_retain_eligible_in_scope(
    const struct r300_r2vb_producer_plan *plan,
    enum r300_r2vb_telemetry_retain_scope scope)
{
    switch (scope) {
    case R300_R2VB_TELEMETRY_RETAIN_SINGLE:
        return plan->action == R300_R2VB_PLAN_SINGLE;
    case R300_R2VB_TELEMETRY_RETAIN_STRUCTURAL:
        return plan->action == R300_R2VB_PLAN_REJECT &&
               telemetry_reject_is_structural(plan);
    case R300_R2VB_TELEMETRY_RETAIN_ALL:
        return true;
    case R300_R2VB_TELEMETRY_RETAIN_BUDGET:
        break;
    }
    /* Budget scope: the plans whose split, range, or budget machinery
     * engaged -- the shapes the compaction mining needs.  A SINGLE plan
     * fits as is, and a structural or allocation reject carries no budget
     * signal. */
    if (plan->action == R300_R2VB_PLAN_SPLIT)
        return true;
    if (plan->action != R300_R2VB_PLAN_REJECT)
        return false;
    switch (plan->primary_reason) {
    case R300_R2VB_PLAN_OK:
    case R300_R2VB_PLAN_OUT_OF_MEMORY:
    case R300_R2VB_PLAN_CONTROL_FLOW:
    case R300_R2VB_PLAN_INTRINSIC:
    case R300_R2VB_PLAN_IO_SHAPE:
    case R300_R2VB_PLAN_BACKEND:
        return false;
    default:
        return true;
    }
}

static bool
telemetry_retain_eligible(const struct r300_r2vb_producer_plan *plan)
{
    return r300_r2vb_telemetry_retain_eligible_in_scope(
        plan, telemetry_retain_scope());
}

/* Compare a published file byte-for-byte against the serialized blob. */
static bool
file_matches_blob(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t buf[4096];
    size_t off = 0;
    size_t got;
    bool match = true;
    while (match && (got = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (got > size - off || memcmp(buf, data + off, got) != 0)
            match = false;
        else
            off += got;
    }
    if (match)
        match = (off == size) && !ferror(f);
    fclose(f);
    return match;
}

/* Create a same-directory O_EXCL temporary inode with the requested creation
 * mode.  The process umask and directory policy determine the effective mode;
 * the optional output reports that mode to the matching-file synchronizer. */
static int
telemetry_open_unique(const char *path, char *tmp, size_t tmp_size,
                      mode_t *mode_out)
{
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        uint32_t serial = p_atomic_inc_return(&telemetry_temp_serial);
        int need = snprintf(tmp, tmp_size, "%s.tmp.%d.%" PRIu32, path,
                            (int)getpid(), serial);
        if (need < 0 || (size_t)need >= tmp_size)
            return -1;

        int fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return -1;
        }

        if (mode_out) {
            struct stat st;
            if (fstat(fd, &st) != 0) {
                close(fd);
                unlink(tmp);
                return -1;
            }
            *mode_out = st.st_mode & 0777;
        }
        return fd;
    }
    return -1;
}

/* Publish the blob at path through a same-directory temporary file and
 * rename, so the final pathname only ever names a complete blob.  O_EXCL
 * names use a process id and atomic serial, and every failure unlinks the
 * created file. */
static bool
telemetry_publish(const char *path, const uint8_t *data, size_t size)
{
    char tmp[1088];
    int fd = telemetry_open_unique(path, tmp, sizeof(tmp), NULL);
    if (fd < 0)
        return false;

    size_t off = 0;
    bool ok = true;
    while (off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0)
        ok = false;
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok)
        unlink(tmp);
    return ok;
}

/* Apply the effective creation mode to an existing matching blob.  The probe
 * inode lives beside the corpus file, so its mode includes the process umask
 * and directory policy that govern new publications. */
static bool
telemetry_sync_existing_mode(const char *path)
{
    char probe[1088];
    mode_t mode;
    int probe_fd = telemetry_open_unique(path, probe, sizeof(probe), &mode);
    if (probe_fd < 0)
        return false;

    bool ok = close(probe_fd) == 0;
    if (unlink(probe) != 0)
        ok = false;
    if (!ok)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ok = fchmod(fd, mode) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

/* Serialize the application VS into <dir>/r2vb-vs-<blake3>.nir.  The
 * filename carries the full content hash, so a shape that recurs across
 * draws, contexts, and processes lands once.  Atomic publication makes an
 * existing final name a complete blob; its bytes still verify against the
 * fresh serialization, and a mismatching file (foreign or damaged) is
 * republished in place. */
static void
telemetry_retain(const char *dir, const struct nir_shader *vs_nir)
{
    struct blob blob;
    blob_init(&blob);
    nir_serialize(&blob, (nir_shader *)vs_nir, false);
    if (blob.out_of_memory) {
        blob_finish(&blob);
        p_atomic_inc(&counters.retain_failures);
        return;
    }

    blake3_hash hash;
    char hex[BLAKE3_HEX_LEN];
    _mesa_blake3_compute(blob.data, blob.size, hash);
    _mesa_blake3_format(hex, hash);

    char path[1024];
    int need = snprintf(path, sizeof(path), "%s/r2vb-vs-%s.nir", dir, hex);
    if (need < 0 || (size_t)need >= sizeof(path)) {
        blob_finish(&blob);
        p_atomic_inc(&counters.retain_failures);
        fprintf(stderr, "r2vb_telemetry retain path too long, dropped\n");
        return;
    }

    if (access(path, F_OK) == 0 &&
        file_matches_blob(path, blob.data, blob.size)) {
        bool mode_ok = telemetry_sync_existing_mode(path);
        blob_finish(&blob);
        if (!mode_ok)
            p_atomic_inc(&counters.retain_failures);
        return;
    }

    bool ok = telemetry_publish(path, blob.data, blob.size);
    blob_finish(&blob);

    if (ok) {
        p_atomic_inc(&counters.retained);
        if (telemetry_print_enabled())
            fprintf(stderr, "r2vb_telemetry retained=%s\n", path);
    } else {
        p_atomic_inc(&counters.retain_failures);
        fprintf(stderr, "r2vb_telemetry retain failed: %s\n", path);
    }
}

/* Content hash of the bound application VS as lowercase hex, computed once
 * per shader (one nir_serialize on the first event) and cached on the VS so
 * per-draw accounting never re-serializes.  Returns "-" when the shader is
 * unavailable or serialization fails. */
static const char *
telemetry_vs_hex(struct r300_context *r300);

const char *r300_r2vb_telemetry_vs_content_hex(struct r300_context *r300)
{
    return telemetry_vs_hex(r300);
}

static const char *
telemetry_vs_hex(struct r300_context *r300)
{
    struct r300_vertex_shader *vs = r300_vs(r300);
    if (!vs || vs->state.type != PIPE_SHADER_IR_NIR || !vs->state.ir.nir)
        return "-";
    if (vs->r2vb_content_hex[0])
        return vs->r2vb_content_hex;

    struct blob blob;
    blob_init(&blob);
    nir_serialize(&blob, (nir_shader *)vs->state.ir.nir, false);
    if (blob.out_of_memory) {
        blob_finish(&blob);
        return "-";
    }
    blake3_hash hash;
    _mesa_blake3_compute(blob.data, blob.size, hash);
    blob_finish(&blob);
    static_assert(sizeof(vs->r2vb_content_hex) >= BLAKE3_HEX_LEN,
                  "r2vb_content_hex holds a full BLAKE3 hex string");
    _mesa_blake3_format(vs->r2vb_content_hex, hash);
    return vs->r2vb_content_hex;
}

/* Dynamic workload weight per (VS content hash, plan action): contexts share
 * the table (like the counters), a mutex guards it, and the teardown summary
 * flushes one line per entry.  The table's keys are the entries' own copies. */
struct telemetry_workload_entry {
    char key[68];
    char hex[65];
    char action;
    struct r300_r2vb_workload_stats stats;
};

/* Workload summaries use one code per route action.  The action names for
 * SINGLE and SPLIT share an initial (per
 * (rg --fixed-strings r300_r2vb_plan_action_str src/gallium/drivers/r300)),
 * so storing the first name character would merge distinct route policies in
 * the retained summary. */
static char
telemetry_workload_action_code(enum r300_r2vb_plan_action action)
{
    switch (action) {
    case R300_R2VB_PLAN_REJECT:
        return 'R';
    case R300_R2VB_PLAN_SINGLE:
        return 'N';
    case R300_R2VB_PLAN_SPLIT:
        return 'P';
    }
    return '?';
}

static simple_mtx_t workload_mtx = SIMPLE_MTX_INITIALIZER;
static struct hash_table *workload_table;

void
r300_r2vb_telemetry_note(struct r300_context *r300,
                         const struct r300_r2vb_producer_plan *plan)
{
    if (plan->action < ARRAY_SIZE(counters.by_action))
        p_atomic_inc(&counters.by_action[plan->action]);
    if (plan->primary_reason < R300_R2VB_PLAN_REASON_COUNT)
        p_atomic_inc(&counters.by_reason[plan->primary_reason]);
    if (plan->has_typed_source &&
        plan->typed_source_class < ARRAY_SIZE(counters.typed))
        p_atomic_inc(&counters.typed[plan->typed_source_class]);

    if (telemetry_print_enabled()) {
        fprintf(stderr,
                "r2vb_telemetry action=%s primary=%s mask=0x%" PRIx64
                " typed=%d inputs=%u space=%s cv=%d baseline=%u/%u/%u "
                "passA=%u/%u/%u passB=%u/%u/%u vs_blake3=%s\n",
                r300_r2vb_plan_action_str(plan->action),
                r300_r2vb_plan_reason_str(plan->primary_reason),
                plan->observed_reason_mask, plan->has_typed_source,
                plan->num_position_inputs,
                plan->key.space == R300_R2VB_POSITION_WINDOW ? "window"
                                                             : "clip",
                plan->key.allow_computed_varying,
                plan->baseline.alu, plan->baseline.temps,
                plan->baseline.consts, plan->pass_a_cost.alu,
                plan->pass_a_cost.temps, plan->pass_a_cost.consts,
                plan->pass_b_cost.alu, plan->pass_b_cost.temps,
                plan->pass_b_cost.consts, telemetry_vs_hex(r300));
    }

    const char *dir = telemetry_retain_dir();
    if (dir && telemetry_retain_eligible(plan)) {
        struct r300_vertex_shader *vs = r300_vs(r300);
        if (vs && vs->state.type == PIPE_SHADER_IR_NIR && vs->state.ir.nir)
            telemetry_retain(dir, vs->state.ir.nir);
    }
}

void
r300_r2vb_telemetry_draw(struct r300_context *r300,
                         const struct r300_r2vb_producer_plan *plan,
                         const struct pipe_draw_info *info,
                         const struct pipe_draw_start_count_bias *draw)
{
    const char *hex = telemetry_vs_hex(r300);
    if (hex[0] == '-')
        return;

    simple_mtx_lock(&workload_mtx);
    if (!workload_table)
        workload_table = _mesa_hash_table_create(NULL, _mesa_hash_string,
                                                 _mesa_key_string_equal);
    struct telemetry_workload_entry *e = NULL;
    if (workload_table) {
        char key[68];
        char action = telemetry_workload_action_code(plan->action);
        int key_length = snprintf(key, sizeof(key), "%s/%c", hex, action);
        struct hash_entry *he = key_length < 0 ||
                                        (size_t)key_length >= sizeof(key) ?
            NULL : _mesa_hash_table_search(workload_table, key);
        if (he) {
            e = he->data;
        } else if (key_length >= 0 && (size_t)key_length < sizeof(key)) {
            e = calloc(1, sizeof(*e));
            if (e) {
                memcpy(e->key, key, (size_t)key_length + 1);
                memcpy(e->hex, hex, sizeof(e->hex));
                e->action = action;
                e->stats.action = e->action;
                e->stats.draw_min = UINT32_MAX;
                _mesa_hash_table_insert(workload_table, e->key, e);
            }
        }
    }
    if (e) {
        e->stats.draws++;
        e->stats.vertices += (uint64_t)draw->count * info->instance_count;
        e->stats.instances += info->instance_count;
        if (draw->count < e->stats.draw_min)
            e->stats.draw_min = draw->count;
        if (draw->count > e->stats.draw_max)
            e->stats.draw_max = draw->count;
        if (info->mode < 32)
            e->stats.topology_mask |= 1u << info->mode;
        if (info->index_size)
            e->stats.indexed_draws++;
    }
    simple_mtx_unlock(&workload_mtx);
}

bool
r300_r2vb_telemetry_workload_stats(
    const char *hex, enum r300_r2vb_plan_action action,
    struct r300_r2vb_workload_stats *out)
{
    bool found = false;
    simple_mtx_lock(&workload_mtx);
    if (workload_table) {
        char key[68];
        char action_code = telemetry_workload_action_code(action);
        int key_length = snprintf(key, sizeof(key), "%s/%c", hex,
                                  action_code);
        if (key_length >= 0 && (size_t)key_length < sizeof(key)) {
            struct hash_entry *he =
                _mesa_hash_table_search(workload_table, key);
            if (he) {
                *out = ((struct telemetry_workload_entry *)he->data)->stats;
                found = true;
            }
        }
    }
    simple_mtx_unlock(&workload_mtx);
    return found;
}

bool
r300_r2vb_telemetry_observation_enabled(void)
{
    return telemetry_print_enabled() || telemetry_retain_dir() != NULL;
}

const struct r300_r2vb_telemetry_counters *
r300_r2vb_telemetry_get(void)
{
    return &counters;
}

void
r300_r2vb_telemetry_context_created(void)
{
    p_atomic_inc(&live_contexts);
}

void
r300_r2vb_telemetry_context_destroyed(void)
{
    if (p_atomic_dec_return(&live_contexts) == 0)
        r300_r2vb_telemetry_print_summary();
}

void
r300_r2vb_telemetry_print_summary(void)
{
    if (!telemetry_print_enabled())
        return;

    /* Snapshot with acquire loads: another context can still classify while
     * this one tears down, and each printed count is then a value the
     * counter actually held. */
    uint32_t by_action[ARRAY_SIZE(counters.by_action)];
    uint32_t by_reason[R300_R2VB_PLAN_REASON_COUNT];
    uint32_t typed[ARRAY_SIZE(counters.typed)];
    for (unsigned i = 0; i < ARRAY_SIZE(by_action); i++)
        by_action[i] = p_atomic_read(&counters.by_action[i]);
    for (unsigned i = 0; i < ARRAY_SIZE(by_reason); i++)
        by_reason[i] = p_atomic_read(&counters.by_reason[i]);
    for (unsigned i = 0; i < ARRAY_SIZE(typed); i++)
        typed[i] = p_atomic_read(&counters.typed[i]);
    uint32_t retained = p_atomic_read(&counters.retained);
    uint32_t retain_failures = p_atomic_read(&counters.retain_failures);

    uint32_t total = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(by_action); i++)
        total += by_action[i];
    if (!total)
        return;
    fprintf(stderr, "r2vb_telemetry summary cells=%u", total);
    for (unsigned i = 0; i < ARRAY_SIZE(by_action); i++) {
        if (by_action[i])
            fprintf(stderr, " %s=%u",
                    r300_r2vb_plan_action_str((enum r300_r2vb_plan_action)i),
                    by_action[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(by_reason); i++) {
        if (by_reason[i])
            fprintf(stderr, " reason:%s=%u",
                    r300_r2vb_plan_reason_str((enum r300_r2vb_plan_reason)i),
                    by_reason[i]);
    }
    static const char *const typed_names[] = { "none", "bool", "sint",
                                               "uint" };
    static_assert(ARRAY_SIZE(typed_names) == R300_R2VB_TYPED_SOURCE_UINT + 1,
                  "typed_names covers enum r300_r2vb_typed_source_class");
    for (unsigned i = 0; i < ARRAY_SIZE(typed); i++) {
        if (typed[i])
            fprintf(stderr, " typed:%s=%u", typed_names[i], typed[i]);
    }
    if (retained || retain_failures)
        fprintf(stderr, " retained=%u retain_failures=%u", retained,
                retain_failures);
    fprintf(stderr, "\n");

    /* One workload line per VS content hash: the dynamic weight that turns
     * cell incidence into route-policy evidence.  The table survives the
     * flush so a later context epoch keeps accumulating. */
    simple_mtx_lock(&workload_mtx);
    if (workload_table) {
        hash_table_foreach(workload_table, he) {
            const struct telemetry_workload_entry *e = he->data;
            fprintf(stderr,
                    "r2vb_telemetry workload hash=%s action=%c draws=%" PRIu64
                    " vertices=%" PRIu64 " instances=%" PRIu64
                    " draw_min=%u draw_max=%u topo_mask=0x%x indexed=%" PRIu64
                    "\n",
                    e->hex, e->action, e->stats.draws, e->stats.vertices,
                    e->stats.instances, e->stats.draw_min, e->stats.draw_max,
                    e->stats.topology_mask, e->stats.indexed_draws);
        }
    }
    simple_mtx_unlock(&workload_mtx);
}
