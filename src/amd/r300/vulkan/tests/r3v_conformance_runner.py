# SPDX-License-Identifier: MIT
"""Identity-retaining dEQP-VK conformance runner for the R3V ICD.

A conformance result is decision-grade only when it binds to one exact
configuration, so the runner records the identity before the first case
and seals it with the results: source SHA against the declared SHA and
tree cleanliness, Meson build options, ICD manifest and DSO digest, the
dEQP binary digest and the release name the binary itself writes into
its log, kernel release and radeon module srcversion, installed
packages, GPU PCI identity and the render node that resolves to it, boot
id, the exact environment the driver reads, the case list, per-case
status, dmesg delta, the deadline, and digests of every raw artifact the
run wrote.  The seal is the SHA-256 of the canonical receipt with the
seal itself removed; `verify-receipt` recomputes it and re-digests the
artifacts.

The evidence class derives from the run: a preloaded drm-shim makes it
host-model, and only an unpreloaded run whose ICD is libvulkan_r3v.so
on a host whose render node resolves to an RS4xx PCI device is silicon;
a run without source identity, with a dirty tree, or with a SHA other
than the declared one is never decision-grade.

Verdicts are finite.  NotSupported never counts as a pass.  Every
non-pass status classifies against the non-pass ledger most-specific
row first over status, case name, and the untruncated result text; a
status the ledger does not name makes the run `unclassified_nonpass`.
A process the deadline kills is `runner_deadline` whatever the log
says; a dEQP process ending without `#endSession` truncates the run
(the case in flight becomes `crash` or `timeout`, the rest `not_run`);
a dEQP framework abort with no case reported is
`framework_precondition`; a dmesg delta matching a hazard pattern is
`dmesg_hazard`; a DSO or source SHA other than the expected one refuses
before launch.

`--process-per-case` runs each case of the shard in its own dEQP
process, sequentially, under the shard `--timeout` with `--case-timeout`
as each process's own deadline.  Plan replay binds one session to one
evidence directory, so a shard reaches plan replay exactly when one
case owns one process; plan capture instead assigns each device in a
process its own ordinal (dEQP creates devices freely, and a case's own
robustness tests create one ahead of the one they drive), so a case
that creates several devices still leaves that case's whole capture
inside its own process.  A declared `--env` value carries `{case}`,
`{index}`, and `{nonce}`, which each case resolves before its process
starts, giving that case its own transcript family, plan, and session
nonce; `{nonce}` resolves through the `--plan-nonce-file` TSV of case
and 32 hex digits.  The result stays one
shard receipt under one seal, one partition binding, and one journal
cursor, with each case's status taken from its own process's log, its
own artifacts digested under `cases/<case>/`, and its exit code, session
closure, resolved values, and resolved-path presence recorded per case.

The host-planning disposition is the planning pass admitted as its own
evidence class, `host-planning`, never decision-grade.  It opens on
exact conditions alone: the slice hazard is `submission`, the radeon
drm-shim (`libradeon_noop_drm_shim.so`) is in the preload path so it
interposes ioctl, `R3V_NATIVE_PLAN_CAPTURE_FILE` is declared,
`R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED` is closed (unset, empty, or `0`),
no attended evidence directory (`R3V_NATIVE_MANIFEST_DIR`) and no
replay plan (`R3V_NATIVE_PLAN_FILE`, `R3V_NATIVE_PLAN_NONCE`) is
declared, the shard runs one process apiece, and the per-process
strace witness counts zero kernel-entering DRM_IOCTL_RADEON_CS.  A
failed condition refuses the run by name
(`planning_disposition_refused`, `planning_witness_unavailable`,
`planning_cs_witnessed`, `planning_unwitnessed`), and the sealed receipt
records each case's outcome: `transcript` with the digest of every
transcript the device wrote, `no_nonempty_ib` when the device reported
that no executable submission ran, or `unresolved`.  The slice's
required evidence stays silicon whatever the disposition records.

Usage:
  r3v_conformance_runner.py run --deqp-binary BIN --icd MANIFEST \
      --caselist FILE --out DIR [--source-root DIR] [--build-root DIR] \
      [--expect-source-sha HEX] [--expect-dso-sha256 HEX] \
      [--timeout SEC] [--nonpass-ledger TSV] [--dmesg-command CMD] \
      [--env KEY=VALUE]... [--process-per-case] [--plan-nonce-file TSV] \
      [--strace-binary PATH]
  r3v_conformance_runner.py verify-receipt --receipt receipt.json
  r3v_conformance_runner.py check-ledgers --nonpass-ledger TSV --slices TSV
  r3v_conformance_runner.py run ... --partition-manifest partition_manifest.json
  r3v_conformance_runner.py selftest --fixture-qpa FILE
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import r3v_cs_ioctl_trace as cs_trace  # noqa: E402

RECEIPT_VERSION = 3

PASS_STATUS = {"Pass"}
DEQP_STATUSES = {"Pass", "Fail", "QualityWarning", "CompatibilityWarning",
                 "Pending", "NotSupported", "ResourceError", "InternalError",
                 "Crash", "Timeout", "Waiver"}
RUNNER_STATUSES = {"crash", "timeout", "not_run", "truncated"}
QPA_LOG_FORMAT = "0.3.4"
R3V_ICD_BASENAME = "libvulkan_r3v.so"

DRIVER_ENV_PREFIXES = ("R3V_", "VK_", "RADEON_", "LD_PRELOAD", "MESA_",
                       "DISPLAY", "XDG_RUNTIME_DIR")
# The environment a run inherits: the process needs these to start, and
# the driver reads other names.  Everything else enters only through an
# explicit --env declaration and lands whole in the sealed receipt.
INHERITED_ENV = ("PATH", "HOME", "USER", "LOGNAME", "SHELL", "LANG",
                 "TERM", "TMPDIR")
INHERITED_ENV_PREFIXES = ("LC_",)
# Declared values that open a submission, an experimental route, or an
# attended-evidence destination; only a submission-hazard slice admits
# them, and the driver's r3v_native_plan_gates_open refuses the same
# names as contamination in a plan run.  check-ledgers holds the pattern
# to every compute verb gate the ledger names.
SUBMISSION_GATE_PREFIXES = ("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
                            "R3V_NATIVE_AUTHORIZED_", "R3V_NATIVE_R2VB_",
                            "R3V_NATIVE_PLAN_", "R3V_NATIVE_MANIFEST_DIR")
SUBMISSION_GATE_PATTERN = re.compile(
    r"^R3V_NATIVE_COMPUTE_.*_GPU_EXPERIMENTAL$")
COMPUTE_VERB_SOURCE = "src/amd/r300/common/r300_compute_verb.c"
# A shard is a recovery unit: one process over this many cases at most.
MAX_SHARD_CASES = 20000
DMESG_HAZARD_PATTERNS = [
    r"GPU lockup", r"ring \d+ stalled", r"\[drm:.*\] \*ERROR\*",
    r"radeon.*CS.*(invalid|failed|error)", r"GPU reset",
    r"Unable to handle kernel", r"BUG:", r"Oops:", r"soft lockup",
]
RS4XX_PCI_DEVICES = {"0x5954", "0x5955", "0x5974", "0x5975"}
ARTIFACT_NAMES = ("run.qpa", "stdout.txt", "stderr.txt", "dmesg_delta.txt",
                  "caselist.txt", "runtime_event.json")
# One process apiece puts each case's dEQP artifacts in that case's own
# directory, and the shard keeps the artifacts the shard itself writes.
CASE_ARTIFACT_NAMES = ("run.qpa", "stdout.txt", "stderr.txt", "caselist.txt")
SHARD_ARTIFACT_NAMES = ("dmesg_delta.txt", "caselist.txt",
                        "runtime_event.json")
# A declared --env value carries these tokens, and each case resolves
# them before its own process starts.
ENV_CASE_TOKEN = re.compile(r"\{(case|index|nonce)\}")
CASE_NAME_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]")
PLAN_NONCE_PATTERN = re.compile(r"[0-9a-f]{32}")

# The host-planning disposition: the planning pass as its own evidence
# class.  The shim basename is the one r3v_native_plan_capture_host_model_present
# resolves the ioctl symbol to; the gate names are the ones
# r3v_native_device.c reads at device creation; the message is the one
# the device logs at destroy when it captured no executable submission.
PLANNING_EVIDENCE = "host-planning"
RADEON_DRM_SHIM_BASENAME = "libradeon_noop_drm_shim.so"
CAPTURE_FILE_NAME = "R3V_NATIVE_PLAN_CAPTURE_FILE"
SUBMIT_HAZARD_GATE = "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"
ATTENDED_EVIDENCE_DIR = "R3V_NATIVE_MANIFEST_DIR"
PLAN_REPLAY_NAMES = ("R3V_NATIVE_PLAN_FILE", "R3V_NATIVE_PLAN_NONCE")
NO_TRANSCRIPT_MESSAGE = ("no executable submission ran; no plan transcript "
                         "written")
EMPTY_CAPTURE_SUFFIX = ".no_nonempty_ib"
CASE_STRACE_NAME = "ioctl.strace"
PLANNING_OUTCOMES = ("transcript", "no_nonempty_ib", "unresolved")
PLANNING_GRADE_REASON = ("host-planning disposition captures transcripts on "
                         "the host model and proves nothing about "
                         "conformance; the slice's silicon requirement "
                         "stands")

LEDGER_HEADER = ["class", "status", "case_pattern", "detail_pattern",
                 "disposition", "authority", "witness"]
SLICE_HEADER = ["order", "slice", "groups", "hazard", "required_evidence"]
SLICE_HAZARDS = {"none", "submission", "display"}
SLICE_EVIDENCE = {"host-model", "silicon"}


class RunnerRefusal(Exception):
    pass


def partition_identity(manifest_path, caselist_path):
    """Bind the caselist to a slice of a verified partition manifest and
    refuse a slice whose hazard is unknown.  With no manifest the run is
    ad hoc: it names no slice and no corpus."""
    if manifest_path is None:
        return {"kind": "ad-hoc"}, None
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import r3v_conformance_partition as part
    try:
        manifest = part.verify_manifest(manifest_path)
        s, shard, subset = part.bind_caselist(manifest_path, manifest,
                                              caselist_path)
    except part.PartitionRefusal as e:
        raise RunnerRefusal(f"partition manifest: {e}")
    ident = {"kind": manifest["kind"],
             "cts_describe": manifest.get("cts_describe"),
             "manifest_sha256": manifest["manifest_sha256"],
             "corpus_sha256": manifest["corpus_sha256"],
             "corpus_case_count": manifest["corpus_case_count"],
             "covered_case_count": manifest["covered_case_count"],
             "executable_case_count": manifest["executable_case_count"],
             "uncovered_case_count": manifest["uncovered_case_count"],
             "slice": s["slice"], "order": s["order"], "hazard": s["hazard"],
             "required_evidence": s["required_evidence"],
             "case_count": s["case_count"],
             "caselist_sha256": s["caselist_sha256"],
             "shard_max_cases": s["shard_max_cases"],
             "shard_index": shard["index"], "shard_count": s["shard_count"],
             "shard_case_count": shard["case_count"],
             "shard_caselist_sha256": shard["caselist_sha256"],
             # A caselist that is a proper subset of its shard keeps the
             # shard identity above and records its own count and digest
             # here; a whole-shard run binds with subset None.
             "binding": "shard" if subset is None else "shard_subset",
             "subset_case_count":
                 None if subset is None else subset["case_count"],
             "subset_caselist_sha256":
                 None if subset is None else subset["caselist_sha256"]}
    # The hazard value itself opens execution; the stored flag is
    # derived for readers and verify_manifest holds it to the hazard.
    refusal = "blocked_slice" if s["hazard"] == "unknown" else None
    return ident, refusal


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_capture(argv, cwd=None, env=None):
    try:
        p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                           timeout=60, env=env)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)
    return p.returncode, p.stdout.strip()


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def load_json(path, what):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError) as e:
        raise RunnerRefusal(f"{what} {path} is unreadable: {e}")


def source_identity(root):
    if root is None:
        return {"available": False}
    rc, sha = run_capture(["git", "rev-parse", "HEAD"], cwd=root)
    if rc != 0:
        return {"available": False, "error": sha}
    _, status = run_capture(["git", "status", "--porcelain=v2"], cwd=root)
    return {"available": True, "sha": sha, "clean": status == "",
            "dirty_entries": len(status.splitlines())}


def build_identity(root):
    if root is None:
        return {"available": False}
    opts = Path(root) / "meson-info" / "intro-buildoptions.json"
    if not opts.is_file():
        return {"available": False, "error": f"{opts} absent"}
    data = load_json(opts, "build options")
    selected = {o["name"]: o["value"] for o in data
                if o.get("section") in ("user", "core") and
                o["name"] in ("buildtype", "vulkan-drivers", "gallium-drivers",
                              "werror", "b_sanitize", "tools", "optimization")}
    return {"available": True, "options": selected,
            "buildoptions_sha256": sha256_file(opts)}


def icd_identity(manifest):
    data = load_json(manifest, "ICD manifest")
    lib = data.get("ICD", {}).get("library_path")
    if not lib:
        raise RunnerRefusal(f"{manifest} names no ICD.library_path")
    lib_path = Path(lib)
    if not lib_path.is_absolute():
        lib_path = Path(manifest).resolve().parent / lib_path
    if not lib_path.is_file():
        raise RunnerRefusal(f"ICD library {lib_path} does not exist")
    return {"manifest": str(Path(manifest).resolve()),
            "library_path": str(lib_path.resolve()),
            "library_basename": lib_path.name,
            "dso_sha256": sha256_file(lib_path),
            "api_version": data.get("ICD", {}).get("api_version")}


def deqp_identity(binary):
    b = Path(binary)
    if not b.is_file():
        raise RunnerRefusal(f"dEQP binary {binary} does not exist")
    ident = {"binary": str(b.resolve()), "sha256": sha256_file(b)}
    repo = b.resolve()
    for _ in range(8):
        repo = repo.parent
        if (repo / ".git").exists():
            rc, desc = run_capture(["git", "describe", "--tags", "--always",
                                    "--dirty"], cwd=repo)
            ident["cts_worktree_describe"] = desc if rc == 0 else None
            break
    return ident


def render_nodes():
    nodes = []
    for node in sorted(Path("/sys/class/drm").glob("renderD*")):
        try:
            slot = (node / "device").resolve().name
        except OSError:
            slot = None
        nodes.append({"node": f"/dev/dri/{node.name}", "slot": slot})
    return nodes


def host_identity():
    ident = {"kernel_release": platform.release(),
             "machine": platform.machine(),
             "boot_id": read_optional("/proc/sys/kernel/random/boot_id"),
             "radeon_srcversion":
                 read_optional("/sys/module/radeon/srcversion"),
             "hostname_sha256":
                 hashlib.sha256(platform.node().encode()).hexdigest()[:16]}
    rc, pkgs = run_capture(["pacman", "-Q"])
    if rc == 0:
        ident["packages"] = sorted(
            l for l in pkgs.splitlines()
            if any(k in l for k in ("mesa", "vulkan", "radeon", "linux")))
    gpus = []
    for dev in sorted(Path("/sys/bus/pci/devices").glob("*")):
        cls = read_optional(dev / "class") or ""
        if cls.startswith("0x03"):
            gpus.append({"slot": dev.name,
                         "vendor": read_optional(dev / "vendor"),
                         "device": read_optional(dev / "device"),
                         "revision": read_optional(dev / "revision"),
                         "subsystem_vendor":
                             read_optional(dev / "subsystem_vendor"),
                         "subsystem_device":
                             read_optional(dev / "subsystem_device")})
    ident["gpus"] = gpus
    ident["render_nodes"] = render_nodes()
    return ident


def build_environment(declared, hazard):
    """The allowlisted process environment plus the declared values.  A
    declared submission gate is admitted on a submission-hazard slice
    alone; on every other slice it is contamination, refused by name."""
    env = {k: v for k, v in os.environ.items()
           if k in INHERITED_ENV or k.startswith(INHERITED_ENV_PREFIXES)}
    for kv in declared or []:
        k, sep, v = kv.partition("=")
        if not sep or not k:
            raise RunnerRefusal(f"--env {kv!r} is not KEY=VALUE")
        env[k] = v
    contaminating = sorted(k for k in env
                           if k.startswith(SUBMISSION_GATE_PREFIXES) or
                           SUBMISSION_GATE_PATTERN.match(k))
    if hazard != "submission" and contaminating:
        return env, contaminating
    return env, []


def environment_identity(env, declared):
    """The whole run environment, declared values verbatim and inherited
    values as digests: the closure claim needs every name, and the
    operator's paths and identity stay out of a retained receipt."""
    declared_names = {kv.partition("=")[0] for kv in declared or []}
    return {k: v if k in declared_names or k == "VK_DRIVER_FILES"
            else "sha256:" + hashlib.sha256(v.encode()).hexdigest()[:16]
            for k, v in sorted(env.items())}


def evidence_class(env, host, icd):
    """host-model under a drm-shim preload; silicon only when the ICD is
    the r3v DSO and a render node resolves to an RS4xx PCI device."""
    preload = env.get("LD_PRELOAD", "")
    if "drm_shim" in preload or "drm-shim" in preload:
        return "host-model", None
    rs4xx_slots = {g["slot"] for g in host["gpus"]
                   if g["vendor"] == "0x1002" and
                   g["device"] in RS4XX_PCI_DEVICES}
    node = next((n for n in host["render_nodes"] if n["slot"] in rs4xx_slots),
                None)
    if node and icd["library_basename"] == R3V_ICD_BASENAME:
        return "silicon", node
    return "host-unknown", None


def radeon_drm_shim_interposes(env):
    """The radeon drm-shim in the preload path by exact basename, the
    same test the driver's capture admission makes on the resolved
    ioctl symbol; any other preload leaves the CS ioctl unanswered."""
    return any(Path(p).name == RADEON_DRM_SHIM_BASENAME
               for p in re.split(r"[:\s]+", env.get("LD_PRELOAD", ""))
               if p)


def gate_closed(value):
    """Unset, empty, and zero are the closed gate values; the driver
    opens on the exact value 1 alone."""
    return value in (None, "", "0")


def planning_conditions(env, evidence, hazard, process_per_case):
    """The pre-run conditions of the host-planning disposition, each
    named so a refusal states which one failed; the kernel-entering
    CS count joins after the run from the per-process strace."""
    return {
        "slice_hazard_submission": hazard == "submission",
        "radeon_drm_shim_interposes_ioctl":
            evidence == "host-model" and radeon_drm_shim_interposes(env),
        "plan_capture_file_declared": bool(env.get(CAPTURE_FILE_NAME)),
        "submit_hazard_gate_closed": gate_closed(env.get(SUBMIT_HAZARD_GATE)),
        "attended_evidence_directory_absent":
            not env.get(ATTENDED_EVIDENCE_DIR),
        "replay_plan_absent": not any(env.get(n) for n in PLAN_REPLAY_NAMES),
        "one_process_per_case": bool(process_per_case),
    }


def planning_tracer(declared):
    """The strace the planning witness runs each case under: the
    declared binary when it exists and executes, else the one
    r3v_cs_ioctl_trace proves can attach."""
    if declared:
        if Path(declared).is_file() and os.access(declared, os.X_OK):
            return declared, None
        return None, f"{declared} is not an executable file"
    return cs_trace.strace_available()


def planning_outcome(case_dir, capture_path, stderr_text):
    """One case's planning outcome from its own process: the transcripts
    the device wrote at the declared path and its `.N` ordinals, each
    digested; `no_nonempty_ib` when the device left its
    `.no_nonempty_ib` marker (r3v_native_plan_capture_mark_empty, written
    at destroy when zero entries were captured; vk_logw reaches stderr
    only under debug logging, so the marker is the observation and the
    message a fallback); `unresolved` otherwise, which covers a device
    the driver refused and a process that died.  The per-process strace
    gives the kernel-entering ioctl counts."""
    strace = case_dir / CASE_STRACE_NAME
    witnessed = strace.is_file()
    counts, unparsed = cs_trace.parse_strace(strace) if witnessed \
        else ({}, 0)
    transcripts = {}
    markers = []
    if capture_path:
        base = Path(capture_path)
        members = [base] + sorted(
            q for q in base.parent.glob(base.name + ".*")
            if re.fullmatch(r"\d+", q.name[len(base.name) + 1:]))
        transcripts = {str(q): sha256_file(q) for q in members
                       if q.is_file()}
        markers = sorted(str(q) for q in base.parent.glob(
            base.name + "*" + EMPTY_CAPTURE_SUFFIX) if q.is_file())
    if transcripts:
        outcome = "transcript"
    elif markers or NO_TRANSCRIPT_MESSAGE in stderr_text:
        outcome = "no_nonempty_ib"
    else:
        outcome = "unresolved"
    return {"outcome": outcome, "transcripts": transcripts,
            "empty_markers": markers,
            "witnessed": witnessed,
            "cs_ioctls": counts.get(cs_trace.CS_REQUEST, 0),
            "total_ioctls": sum(counts.values()),
            "unparsed_ioctl_lines": unparsed}


def read_dmesg(command):
    if not command:
        return None
    rc, out = run_capture(shlex.split(command))
    if rc != 0:
        return None
    return out.splitlines()


def journal_cursor(journal_command):
    """The kernel journal's cursor at this instant, or None when the
    journal is unavailable; a cursor makes the later read a continuation
    the journal itself guarantees."""
    if not journal_command:
        return None
    rc, out = run_capture(shlex.split(journal_command) +
                          ["-k", "-n", "1", "-o", "cat", "--show-cursor"])
    if rc != 0:
        return None
    m = re.search(r"^-- cursor: (\S+)", out, re.M)
    return m.group(1) if m else None


def journal_after(journal_command, cursor):
    rc, out = run_capture(shlex.split(journal_command) +
                          ["-k", "-o", "short-monotonic",
                           f"--after-cursor={cursor}"])
    if rc != 0:
        return None
    return [line for line in out.splitlines() if not line.startswith("-- ")]


def dmesg_delta(before, after):
    """The lines the kernel logged during the run.  The before stream
    is a prefix of the after stream when the ring buffer kept every
    line; a before stream that is not a prefix means lines fell out of
    the buffer or the buffer was cleared, and the delta cannot be
    reconstructed, which the caller reports as broken continuity."""
    if before is None or after is None:
        return None, "unavailable"
    if after[:len(before)] != before:
        return None, "broken"
    return after[len(before):], "continuous"


def hazard_lines(delta):
    if not delta:
        return []
    return [l for l in delta
            if any(re.search(p, l) for p in DMESG_HAZARD_PATTERNS)]


def parse_qpa(text):
    """Per-case status from a qpa log.

    The session block carries the release the binary was built from;
    each case is `#beginTestCaseResult NAME` through `#endTestCaseResult`
    around a TestCaseResult element whose <Result> may span lines.  A
    case whose begin marker has no end marker stays `truncated`, and a
    `#terminateTestCaseResult STATUS` closes the case with that status.
    """
    results = {}
    session = {}
    in_flight = None
    result_text = None
    partial_begin = False
    for line in text.splitlines():
        if line.startswith("#sessionInfo "):
            parts = line.split(" ", 2)
            if len(parts) == 3:
                session[parts[1]] = parts[2].strip().strip('"')
        elif line.startswith("#beginTestCaseResult"):
            name = line[len("#beginTestCaseResult"):].strip()
            if not name:
                partial_begin = True
                continue
            in_flight = name
            results[in_flight] = {"status": "truncated", "detail": ""}
        elif line.startswith("#endTestCaseResult"):
            in_flight = None
            result_text = None
        elif line.startswith("#terminateTestCaseResult"):
            parts = line.split(" ", 1)
            status = parts[1].strip() if len(parts) > 1 else "Crash"
            if in_flight:
                results[in_flight] = {"status": status,
                                      "detail": "terminated"}
            in_flight = None
            result_text = None
        elif in_flight:
            if result_text is None:
                m = re.search(r'<Result StatusCode="(\w+)">(.*)', line)
                if m:
                    status, rest = m.group(1), m.group(2)
                    if "</Result>" in rest:
                        results[in_flight] = {
                            "status": status,
                            "detail": rest.split("</Result>")[0]}
                    else:
                        result_text = [status, rest]
            else:
                if "</Result>" in line:
                    result_text[1] += "\n" + line.split("</Result>")[0]
                    results[in_flight] = {"status": result_text[0],
                                          "detail": result_text[1]}
                    result_text = None
                else:
                    result_text[1] += "\n" + line
    return {"results": results, "in_flight": in_flight,
            "partial_begin": partial_begin,
            "session_closed": "#endSession" in text, "session": session}


def load_nonpass_ledger(path):
    rows = []
    if path is None:
        return rows
    lines = Path(path).read_text().splitlines()
    if not lines or lines[0].split("\t") != LEDGER_HEADER:
        raise RunnerRefusal(f"{path} header is not {LEDGER_HEADER}")
    for n, line in enumerate(lines[1:], start=2):
        f = line.split("\t")
        if len(f) != len(LEDGER_HEADER) or not all(f):
            raise RunnerRefusal(f"{path}:{n} is not a "
                                f"{len(LEDGER_HEADER)}-field row")
        rows.append({"class": f[0], "status": f[1], "pattern": f[2],
                     "detail": f[3], "disposition": f[4], "authority": f[5],
                     "witness": f[6]})
    return rows


def classify(case, status, detail, ledger):
    """The ledger is ordered most-specific-first, so the first row whose
    status, case pattern, and result-text pattern match carries the
    class; a `-` detail pattern matches any result text, and a status
    no row names is unclassified and blocks."""
    if status in RUNNER_STATUSES:
        return f"runner_{status}", "blocks"
    for r in ledger:
        if r["status"] != status or not re.fullmatch(r["pattern"], case):
            continue
        if r["detail"] != "-" and not re.search(r["detail"], detail or ""):
            continue
        return r["class"], r["disposition"]
    return "unclassified", "blocks"


def summarize(results, ledger):
    counts = {}
    classes = {}
    blocking = 0
    for case, r in sorted(results.items()):
        st = r["status"]
        counts[st] = counts.get(st, 0) + 1
        if st in PASS_STATUS:
            continue
        cls, disp = classify(case, st, r.get("detail", ""), ledger)
        r["class"] = cls
        r["disposition"] = disp
        classes[cls] = classes.get(cls, 0) + 1
        if disp == "blocks":
            blocking += 1
    return counts, classes, blocking


def verdict_for(run):
    """The finite verdict over a receipt the function leaves unchanged:
    a kernel hazard outranks either deadline it most likely caused, a
    case deadline after the session closed is a shutdown hang with the
    runner deadline's shape, and the receipt keeps the exit code that
    named which deadline fired."""
    if run["refusal"]:
        return run["refusal"]
    if run.get("dmesg", {}).get("continuity") == "broken":
        return "kernel_log_continuity_broken"
    if run["hazard_lines"]:
        return "dmesg_hazard"
    code = run.get("exit_code")
    if code == "case_timeout" and run.get("session_closed"):
        code = "timeout"
    if code == "case_timeout":
        return "case_deadline"
    if code == "timeout":
        return "runner_deadline"
    observed = any(k not in RUNNER_STATUSES for k in run["counts"])
    if run.get("framework_abort") and not observed:
        return "framework_precondition"
    if not run["session_closed"]:
        return "truncated_run"
    if isinstance(code, int) and code != 0 and \
            not run["blocking"]:
        return "dirty_exit"
    if run["unexpected_cases"]:
        return "unexpected_cases"
    if run["classes"].get("unclassified", 0):
        return "unclassified_nonpass"
    if run["blocking"]:
        return "classified_nonpass"
    if run["counts"].get("Pass", 0) == 0:
        return "no_pass_observed"
    return "pass_with_accepted_nonpass" if any(
        k not in PASS_STATUS for k in run["counts"]) else "pass"


def canonical(receipt):
    body = {k: v for k, v in receipt.items() if k != "seal_sha256"}
    return json.dumps(body, sort_keys=True, separators=(",", ":"))


def seal(receipt):
    return hashlib.sha256(canonical(receipt).encode()).hexdigest()


def supervise(argv, cwd, env, log, stdout_path, stderr_path, total_timeout,
              case_timeout):
    """Runs dEQP under two deadlines: the whole shard, and the case in
    flight, judged by growth of the log (dEQP flushes it after every
    write) or of stdout within case_timeout seconds.  stdout and stderr
    go to files, so a chatty process never blocks on a pipe the
    supervisor reads later.  A deadline kills the process group; the
    exit code names which deadline."""
    with open(stdout_path, "wb") as out_f, open(stderr_path, "wb") as err_f:
        p = subprocess.Popen(argv, cwd=cwd, env=env, stdout=out_f,
                             stderr=err_f, start_new_session=True)
        started = time.monotonic()
        last_progress = started
        last_size = -1
        exit_code = None
        while True:
            rc = p.poll()
            if rc is not None:
                exit_code = rc
                break
            now = time.monotonic()
            size = 0
            for path in (log, stdout_path):
                try:
                    size += path.stat().st_size
                except OSError:
                    pass
            if size != last_size:
                last_size = size
                last_progress = now
            if now - started > total_timeout:
                exit_code = "timeout"
            elif now - last_progress > case_timeout:
                exit_code = "case_timeout"
            if exit_code is not None:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
                p.wait()
                break
            time.sleep(0.2)
    return (exit_code,
            Path(stdout_path).read_text(errors="replace"),
            Path(stderr_path).read_text(errors="replace"))


def runtime_event_identity(path, out):
    """The joined runtime event (the target-side capture of boot id,
    package and module identity, memory map, clocks, thermals, display
    state, watchdog and netconsole continuity) retained beside the
    receipt; its digest binds the receipt to that event."""
    if path is None:
        return {"available": False}
    p = Path(path)
    if not p.is_file():
        raise RunnerRefusal(f"runtime event {path} is not a file")
    text = p.read_bytes()
    try:
        body = json.loads(text)
    except ValueError:
        raise RunnerRefusal(f"runtime event {path} is not JSON")
    (out / "runtime_event.json").write_bytes(text)
    return {"available": True, "sha256": hashlib.sha256(text).hexdigest(),
            "run_id": body.get("run_id"), "boot_id": body.get("boot_id")}


QUEUE_CLAIM_MODES = ("default_graphics_only", "experimental_compute_subset",
                     "conformant")


def queue_claim_identity(report_binary, env, expected_sha256=None):
    """Runs the queue-claim report through the loader under the run's
    own environment, the pinned VK_DRIVER_FILES included, and records
    the queue flags the ICD advertises there, the claim mode behind the
    compute bit, the verb ledger digest of the source both were built
    from, and the report binary's own digest.  A report whose
    advertised bit, ledger claim, and gate state disagree refuses the
    run; the compute claim is eligible as a conformance statement about
    the queue only in the conformant mode, and a gate-assisted run
    reads as its mode."""
    if report_binary is None:
        return {"available": False, "mode": None,
                "compute_claim_eligible": False}
    path = Path(report_binary).resolve()
    rc, out = run_capture([str(path)], env=env)
    fields = {}
    for line in out.splitlines():
        k, sep, v = line.partition("\t")
        if sep:
            fields[k] = v
    mode = fields.get("queue_claim_mode")
    gate = fields.get("queue_claim_gate") == "1"
    if rc != 0 or mode not in QUEUE_CLAIM_MODES or \
            fields.get("claim_consistent") != "1" or \
            (mode == "experimental_compute_subset" and not gate):
        raise RunnerRefusal("queue-claim report refused: "
                            f"exit {rc}, mode {mode!r}, gate {gate}, "
                            f"consistent {fields.get('claim_consistent')!r}"
                            f"{': ' + out[:200] if rc is None else ''}")
    digest = sha256_file(path)
    if expected_sha256 and expected_sha256 != digest:
        raise RunnerRefusal("queue-claim report binary "
                            f"{digest[:12]} is not the declared "
                            f"{expected_sha256[:12]}")
    return {"available": True, "mode": mode,
            "report_sha256": digest,
            "compute_bit": fields.get("compute_bit") == "1",
            "gate_declared": gate,
            "queue_flags": fields.get("queue_flags"),
            "verb_table_blake3": fields.get("verb_table_blake3"),
            "compute_claim_eligible": mode == "conformant"}


def read_caselist(path):
    cases = []
    seen = set()
    for line in Path(path).read_text().splitlines():
        c = line.strip()
        if not c.startswith("dEQP-VK."):
            continue
        if c in seen:
            raise RunnerRefusal(f"{path} lists {c} twice; a duplicate case "
                                "collapses in the results")
        seen.add(c)
        cases.append(c)
    if not cases:
        raise RunnerRefusal(f"{path} lists no dEQP-VK cases")
    return cases


def sanitize_case_names(cases):
    """The directory name each case's artifacts take: every character
    outside the portable set becomes an underscore.  A collision
    refuses, since two cases sharing one directory overwrite each
    other's log, and the receipt seals the whole mapping so a reader
    resolves a directory back to its case."""
    mapping = {}
    owner = {}
    for case in cases:
        name = CASE_NAME_UNSAFE.sub("_", case)
        if name in owner:
            raise RunnerRefusal(f"{case} and {owner[name]} both sanitize to "
                                f"{name}; a shard holds one directory per "
                                "case")
        owner[name] = case
        mapping[case] = name
    return mapping


def load_plan_nonces(path, cases):
    """The per-case nonce a `{nonce}` token resolves through: a TSV of
    case name and 32 lowercase hex digits, one row per case the shard
    runs.  A nonce binds one plan to one replay session, so a shard
    that resolves the token declares a nonce for every case it runs."""
    nonces = {}
    for n, line in enumerate(Path(path).read_text().splitlines(), start=1):
        if not line.strip():
            continue
        case, sep, nonce = line.partition("\t")
        nonce = nonce.strip()
        if not sep or not PLAN_NONCE_PATTERN.fullmatch(nonce):
            raise RunnerRefusal(f"{path}:{n} is not a case name and 32 "
                                "lowercase hex digits")
        if case in nonces:
            raise RunnerRefusal(f"{path}:{n} names {case} twice")
        nonces[case] = nonce
    if len(set(nonces.values())) != len(nonces):
        raise RunnerRefusal(f"{path} reuses a nonce; one nonce binds one "
                            "replay session, so two cases sharing one would "
                            "let either plan bind the other's session")
    missing = [c for c in cases if c not in nonces]
    if missing:
        raise RunnerRefusal(f"{path} declares no nonce for {len(missing)} "
                            f"of the shard's cases, {missing[0]} first")
    return nonces


def templated_env_names(declared):
    return [kv.partition("=")[0] for kv in declared or []
            if ENV_CASE_TOKEN.search(kv.partition("=")[2])]


def resolve_env_templates(declared, sanitized, index, width, nonce):
    """The declared values this case resolves.  `{case}` takes the
    sanitized case name, `{index}` the ordinal zero-padded to the width
    of the shard's case count, and `{nonce}` the case's declared nonce,
    so one declaration gives every case its own capture path, plan
    file, and session nonce."""
    resolved = {}
    for kv in declared or []:
        k, _, v = kv.partition("=")
        if not ENV_CASE_TOKEN.search(v):
            continue
        resolved[k] = (v.replace("{case}", sanitized)
                        .replace("{index}", f"{index:0{width}d}")
                        .replace("{nonce}", nonce or ""))
    return resolved


def case_argv(args, caselist_file, log):
    return [str(Path(args.deqp_binary).resolve()),
            f"--deqp-caselist-file={caselist_file}",
            f"--deqp-log-filename={log}",
            "--deqp-watchdog=disable"] + (args.deqp_arg or [])


def run_cases(args, out, cases, env, sanitized, nonces, hazard_probe,
              tracer=None, planning=False):
    """Runs each case of the shard in its own dEQP process, in caselist
    order, under the shard deadline with --case-timeout as each
    process's own.  Plan replay binds one session per process, so a
    shard reaches replay exactly when one case owns one process; plan
    capture assigns each device in a process its own ordinal instead,
    so a case that creates several devices still keeps its whole
    capture inside its own process.  A case resolves its declared
    `{case}`, `{index}`, and `{nonce}` values before its process
    starts, which is what gives that case its own transcript family,
    plan, and session.  The shard deadline expiring mid-sequence leaves
    the cases behind it not_run and names the shard's exit `timeout`.

    The shard's session is the sequence of case processes: it closes
    when every case ran to a reaped process, so one case's crash
    classifies as that case's status and leaves the shard readable.

    The kernel log is read after every case, and a hazard line stops the
    sequence there: a lockup or a CS validation error makes every later
    case's result a consequence of the first failure, so the shard names
    the case the hazard followed and leaves the rest not_run.

    With a tracer, each case's process runs under strace so the
    kernel-entering ioctl census of that case alone lands in its own
    directory, and a planning shard records each case's outcome."""
    width = len(str(len(cases)))
    results = {}
    records = {}
    unexpected = {}
    session = {}
    framework_abort = None
    unknown_format = False
    started = time.monotonic()
    shard_exit = 0
    argv = None
    stop = None
    for index, case in enumerate(cases, start=1):
        remaining = args.timeout - (time.monotonic() - started)
        if remaining <= 0:
            shard_exit = "timeout"
            stop = {"after_case": cases[index - 2] if index > 1 else None,
                    "index": index - 1,
                    "reason": "shard deadline expired before this case"}
            break
        case_dir = out / "cases" / sanitized[case]
        case_dir.mkdir(parents=True)
        caselist_file = case_dir / "caselist.txt"
        caselist_file.write_text(case + "\n")
        log = case_dir / "run.qpa"
        resolved = resolve_env_templates(args.env, sanitized[case], index,
                                         width, nonces.get(case))
        case_env = dict(env)
        case_env.update(resolved)
        # A resolved absolute path is a plan the case reads or a
        # transcript it writes, so the receipt carries both whether it
        # existed before the case ran and whether it exists after.  A
        # case whose plan file is absent still runs: its device admits
        # no submission, the case fails, and the receipt names why.
        paths = {k: {"present_before": Path(v).exists()}
                 for k, v in resolved.items() if v.startswith("/")}
        argv = case_argv(args, caselist_file, log)
        launch = argv
        if tracer:
            launch = [tracer] + cs_trace.STRACE_ARGS + \
                ["-o", str(case_dir / CASE_STRACE_NAME), "--"] + argv
        exit_code, _, stderr_text = supervise(
            launch, Path(args.deqp_binary).resolve().parent, case_env, log,
            case_dir / "stdout.txt", case_dir / "stderr.txt", remaining,
            args.case_timeout)
        for k, state in paths.items():
            state["present_after"] = Path(resolved[k]).exists()
        parsed = parse_qpa(log.read_text(errors="replace")) \
            if log.is_file() else {"results": {}, "in_flight": None,
                                   "session_closed": False, "session": {}}
        if not session and parsed["session"]:
            session = parsed["session"]
        if parsed["session"].get("logFormatVersion") not in (None,
                                                            QPA_LOG_FORMAT):
            unknown_format = True
        m = re.search(r"FATAL ERROR: [^\n]*", stderr_text)
        if framework_abort is None and m:
            framework_abort = m.group(0)
        result = parsed["results"].get(case, {"status": "not_run",
                                              "detail": ""})
        unexpected.update({c: r for c, r in parsed["results"].items()
                           if c != case})
        # The single-case process leaves the case in flight exactly when
        # its process died mid-case, so the shard classifies it the way
        # the single-process runner classifies its in-flight case.
        if parsed["in_flight"] == case:
            if exit_code in ("timeout", "case_timeout"):
                result = {"status": "timeout", "detail": ""}
            elif isinstance(exit_code, int) and exit_code < 0:
                result = {"status": "crash",
                          "detail": f"signal {signal.Signals(-exit_code).name}"}
            elif isinstance(exit_code, int) and exit_code != 0:
                result = {"status": "crash", "detail": f"exit {exit_code}"}
        results[case] = result
        record = {"index": index, "directory": sanitized[case],
                  "exit_code": exit_code,
                  "session_closed": parsed["session_closed"]}
        if resolved:
            record["environment"] = resolved
        if paths:
            record["paths"] = paths
        if planning:
            record["planning"] = planning_outcome(
                case_dir, resolved.get(CAPTURE_FILE_NAME), stderr_text)
        records[case] = record
        # A kernel hazard ends the sequence where it appeared: the cases
        # behind it would run on a wedged or reset GPU, so their results
        # would describe the hazard rather than themselves.
        hazard = hazard_probe()
        if hazard:
            stop = {"after_case": case, "index": index,
                    "reason": "kernel hazard after this case",
                    "hazard_lines": hazard}
            break
        # The shard's own deadline firing inside a case ends the shard;
        # the case's deadline ends that case alone.
        if exit_code == "timeout":
            shard_exit = "timeout"
            stop = {"after_case": case, "index": index,
                    "reason": "shard deadline expired during this case"}
            break
    return {"results": results, "cases": records, "session": session,
            "unexpected": unexpected,
            "session_closed": len(records) == len(cases),
            "framework_abort": framework_abort,
            "unknown_format": unknown_format, "exit_code": shard_exit,
            "argv": argv, "index_width": width, "stop": stop}


def execute(args):
    out = Path(args.out)
    if out.exists() and any(out.iterdir()):
        raise RunnerRefusal(f"{out} is not empty; a run takes a fresh "
                            "output directory")
    out.mkdir(parents=True, exist_ok=True)
    cases = read_caselist(args.caselist)
    if len(cases) > args.max_cases:
        raise RunnerRefusal(f"{len(cases)} cases exceed the shard ceiling "
                            f"{args.max_cases}; regenerate the partition at "
                            "this ceiling")
    # A `{case}` value names a per-case file, which one process apiece
    # is what gives it; a token declared for a single-process shard
    # would reach dEQP verbatim, so it refuses here.
    templated = templated_env_names(args.env)
    if templated and not args.process_per_case:
        raise RunnerRefusal(f"--env {templated} carries a per-case token "
                            "outside --process-per-case")
    sanitized = sanitize_case_names(cases) if args.process_per_case else {}
    wants_nonce = any("{nonce}" in kv.partition("=")[2]
                      for kv in args.env or [])
    if wants_nonce and not args.plan_nonce_file:
        raise RunnerRefusal("--env declares {nonce} with no "
                            "--plan-nonce-file to resolve it")
    nonces = load_plan_nonces(args.plan_nonce_file, cases) \
        if args.plan_nonce_file else {}
    partition, part_refusal = partition_identity(args.partition_manifest,
                                                 args.caselist)
    env, contaminating = build_environment(args.env, partition.get("hazard"))
    runtime_event = runtime_event_identity(args.runtime_event, out)
    host = host_identity()
    icd = icd_identity(args.icd)
    # The report probes the ICD the run pins, so the pin enters the
    # environment ahead of it.
    env["VK_DRIVER_FILES"] = icd["manifest"]
    env.pop("VK_ICD_FILENAMES", None)
    queue_claim = queue_claim_identity(args.queue_report, env,
                                       args.expect_report_sha256)
    evidence, node = evidence_class(env, host, icd)
    # A declared capture file on a submission-hazard slice under the
    # drm-shim host model is a planning candidate: it records the
    # ordered submissions a later silicon replay binds to and states
    # nothing about conformance, so it runs below the slice's required
    # evidence and never reaches decision grade.  The candidate earns the
    # host-planning disposition when every named condition holds and the
    # per-process strace witnesses zero kernel-entering CS ioctls; a
    # failed condition refuses by name.  A capture session opens the CS
    # ioctl with the hazard gate closed, which the host model alone
    # answers, so the same declaration on silicon refuses.  On a
    # hazard-free slice the declaration stays contamination, refused by
    # name in build_environment.
    capture_declared = bool(env.get(CAPTURE_FILE_NAME))
    planning_candidate = (capture_declared and evidence == "host-model" and
                          partition.get("hazard") == "submission")
    conditions = planning_conditions(env, evidence, partition.get("hazard"),
                                     args.process_per_case)
    planning = {"candidate": planning_candidate, "disposition": None,
                "decision_grade": False, "conditions": conditions,
                "refused_conditions": [n for n, ok in conditions.items()
                                       if not ok]}
    tracer = None
    if planning_candidate:
        tracer, tracer_error = planning_tracer(args.strace_binary)
        planning["tracer"] = {"binary": tracer, "error": tracer_error,
                              "strace_args": cs_trace.STRACE_ARGS,
                              "witness_scope": "syscall_boundary"}
    receipt = {
        "receipt_version": RECEIPT_VERSION,
        "source": source_identity(args.source_root),
        "build": build_identity(args.build_root),
        "icd": icd,
        "deqp": deqp_identity(args.deqp_binary),
        "host": host,
        "environment": environment_identity(env, args.env),
        "evidence_class": evidence,
        "render_node": node,
        "case_count": len(cases),
        # The digest covers the caselist file's bytes, the form the
        # partition manifest publishes per shard and sha256sum prints, so
        # one declared value binds the receipt, the manifest, and the
        # file.
        "caselist_sha256": sha256_file(args.caselist),
        "partition": partition,
        "runtime_event": runtime_event,
        "queue_claim": queue_claim,
        "compute_claim_eligible": queue_claim["compute_claim_eligible"],
        "expected": {"source_sha": args.expect_source_sha,
                     "dso_sha256": args.expect_dso_sha256,
                     "deqp_sha256": args.expect_deqp_sha256,
                     "caselist_sha256": args.expect_caselist_sha256,
                     "partition_sha256": args.expect_partition_sha256,
                     "runtime_event_sha256":
                         args.expect_runtime_event_sha256,
                     "queue_claim_mode": args.expect_queue_claim_mode},
        "timeouts": {"total_seconds": args.timeout,
                     "case_seconds": args.case_timeout},
        "max_cases": args.max_cases,
        "process_per_case": bool(args.process_per_case),
        "planning": planning,
    }
    if args.process_per_case:
        receipt["case_directories"] = sanitized
        receipt["templated_env"] = templated
        receipt["plan_nonce_file"] = args.plan_nonce_file
    refusal = part_refusal
    if refusal is None and contaminating:
        refusal = "gate_contamination"
        receipt["contaminating_gates"] = contaminating
    if args.expect_queue_claim_mode and \
            args.expect_queue_claim_mode != queue_claim["mode"]:
        refusal = refusal or "wrong_queue_claim"
    for key, expected, actual in (
            ("deqp_sha256", args.expect_deqp_sha256,
             receipt["deqp"].get("sha256")),
            ("caselist_sha256", args.expect_caselist_sha256,
             receipt["caselist_sha256"]),
            ("partition_sha256", args.expect_partition_sha256,
             partition.get("manifest_sha256")),
            ("runtime_event_sha256", args.expect_runtime_event_sha256,
             runtime_event.get("sha256"))):
        if expected and expected != actual:
            refusal = refusal or f"wrong_{key.replace('_sha256', '')}"
    if refusal is None and capture_declared and evidence == "silicon":
        refusal = "capture_on_silicon"
    if refusal is None and planning_candidate and \
            planning["refused_conditions"]:
        refusal = "planning_disposition_refused"
    if refusal is None and planning_candidate and tracer is None:
        refusal = "planning_witness_unavailable"
    if refusal is None and partition.get("required_evidence") == "silicon" \
            and evidence != "silicon" and not planning_candidate:
        refusal = "evidence_below_required"
    if refusal is None and partition.get("shard_max_cases") not in (
            None, args.max_cases):
        refusal = "shard_ceiling_mismatch"
    if args.expect_dso_sha256 and \
            receipt["icd"]["dso_sha256"] != args.expect_dso_sha256:
        refusal = refusal or "wrong_icd"
    src = receipt["source"]
    if args.expect_source_sha and (not src.get("available") or
                                   src["sha"] != args.expect_source_sha):
        refusal = refusal or "wrong_source"
    undeclared = [name for name, value in (
        ("source SHA", args.expect_source_sha),
        ("DSO SHA-256", args.expect_dso_sha256),
        ("dEQP SHA-256", args.expect_deqp_sha256),
        ("caselist SHA-256", args.expect_caselist_sha256),
        ("partition SHA-256", args.expect_partition_sha256)) if not value]
    if evidence == "silicon" and not args.expect_runtime_event_sha256:
        undeclared.append("runtime-event SHA-256")
    if not queue_claim["available"]:
        undeclared.append("queue-claim report")
    # A planning candidate carries its own reason ahead of the rest: it
    # is never decision-grade whatever the source identity says, and the
    # reason a reader needs is what the run is for.
    if planning_candidate:
        grade, reason = False, PLANNING_GRADE_REASON
    elif not src.get("available"):
        grade, reason = False, "source identity unavailable"
    elif not src["clean"]:
        grade, reason = False, "source tree dirty"
    elif undeclared:
        grade, reason = False, "undeclared " + ", ".join(undeclared)
    elif evidence == "host-unknown":
        grade, reason = False, "evidence class unknown"
    elif partition.get("kind") == "ad-hoc":
        grade, reason = False, "caselist bound to no partition slice"
    else:
        grade, reason = True, None
    ledger = load_nonpass_ledger(args.nonpass_ledger)
    results = {c: {"status": "not_run", "detail": ""} for c in cases}
    unexpected = {}
    exit_code = None
    parsed = {"results": {}, "in_flight": None, "partial_begin": False,
              "session_closed": False, "session": {}}
    cursor = journal_cursor(args.journal_command)
    dmesg_before = None if cursor else read_dmesg(args.dmesg_command)

    def hazard_probe():
        """The hazard lines the kernel has logged since the shard's own
        baseline, through whichever log source the run bound to."""
        if cursor:
            delta = journal_after(args.journal_command, cursor)
        else:
            delta, _ = dmesg_delta(dmesg_before,
                                   read_dmesg(args.dmesg_command))
        return hazard_lines(delta)

    log = out / "run.qpa"
    stderr_text = ""
    artifact_names = list(ARTIFACT_NAMES)
    if refusal is None:
        caselist_file = out / "caselist.txt"
        caselist_file.write_text("\n".join(cases) + "\n")
        env["VK_DRIVER_FILES"] = receipt["icd"]["manifest"]
        env.pop("VK_ICD_FILENAMES", None)
        receipt["environment"] = environment_identity(env, args.env)
        started = time.monotonic()
    if refusal is None and args.process_per_case:
        shard = run_cases(args, out, cases, env, sanitized, nonces,
                          hazard_probe, tracer=tracer,
                          planning=planning_candidate and refusal is None)
        receipt["wall_seconds"] = round(time.monotonic() - started, 3)
        # Every case's argv differs from this one in the caselist and
        # log paths alone, which name that case's own directory.
        receipt["argv"] = shard["argv"]
        receipt["cases"] = shard["cases"]
        receipt["index_width"] = shard["index_width"]
        results.update(shard["results"])
        unexpected.update(shard["unexpected"])
        if shard["stop"]:
            receipt["stop"] = shard["stop"]
            for case in cases:
                if case not in shard["results"]:
                    results[case]["detail"] = shard["stop"]["reason"]
        exit_code = shard["exit_code"]
        stderr_text = shard["framework_abort"] or ""
        parsed = {"results": shard["results"], "in_flight": None,
                  "partial_begin": False,
                  "session_closed": shard["session_closed"],
                  "session": shard["session"]}
        if shard["unknown_format"]:
            refusal = refusal or "unknown_log_format"
        artifact_names = list(SHARD_ARTIFACT_NAMES)
        case_names = list(CASE_ARTIFACT_NAMES)
        if tracer:
            case_names.append(CASE_STRACE_NAME)
        artifact_names += [f"cases/{d}/{n}"
                           for d in (shard["cases"][c]["directory"]
                                     for c in cases if c in shard["cases"])
                           for n in case_names]
        # The disposition closes on the run's own witness: every case
        # traced, every trace line parsed, and zero kernel-entering CS
        # ioctls over the shard; anything else refuses and the evidence
        # class stays host-model.
        if planning_candidate and refusal is None:
            per = {c: rec["planning"] for c, rec in shard["cases"].items()}
            unwitnessed = sorted(c for c, o in per.items()
                                 if not o["witnessed"])
            cs_total = sum(o["cs_ioctls"] for o in per.values())
            unparsed = sum(o["unparsed_ioctl_lines"] for o in per.values())
            planning["cs_witness"] = {
                "witness_scope": "syscall_boundary",
                "cs_ioctls": cs_total,
                "total_ioctls": sum(o["total_ioctls"] for o in per.values()),
                "unparsed_ioctl_lines": unparsed,
                "unwitnessed_cases": unwitnessed}
            planning["outcomes"] = {
                o: sum(1 for r in per.values() if r["outcome"] == o)
                for o in PLANNING_OUTCOMES}
            planning["transcripts"] = {
                c: r["transcripts"] for c, r in per.items()
                if r["transcripts"]}
            zero = bool(per) and not unwitnessed and unparsed == 0 and \
                cs_total == 0
            conditions["kernel_entering_cs_zero"] = zero
            if zero:
                planning["disposition"] = PLANNING_EVIDENCE
                evidence = PLANNING_EVIDENCE
                receipt["evidence_class"] = evidence
            else:
                planning["refused_conditions"].append(
                    "kernel_entering_cs_zero")
                refusal = "planning_cs_witnessed" if cs_total else \
                    "planning_unwitnessed"
    elif refusal is None:
        argv = case_argv(args, caselist_file, log)
        receipt["argv"] = argv
        exit_code, stdout_text, stderr_text = supervise(
            argv, Path(args.deqp_binary).resolve().parent, env, log,
            out / "stdout.txt", out / "stderr.txt", args.timeout,
            args.case_timeout)
        receipt["wall_seconds"] = round(time.monotonic() - started, 3)
        if log.is_file():
            parsed = parse_qpa(log.read_text(errors="replace"))
            for c, r in parsed["results"].items():
                if c in results:
                    results[c] = r
                else:
                    unexpected[c] = r
        in_flight = parsed["in_flight"]
        if in_flight and in_flight in results:
            if exit_code in ("timeout", "case_timeout"):
                results[in_flight]["status"] = "timeout"
            elif isinstance(exit_code, int) and exit_code < 0:
                results[in_flight]["status"] = "crash"
                results[in_flight]["detail"] = \
                    f"signal {signal.Signals(-exit_code).name}"
            elif isinstance(exit_code, int) and exit_code != 0:
                results[in_flight]["status"] = "crash"
                results[in_flight]["detail"] = f"exit {exit_code}"
    if cursor:
        delta = journal_after(args.journal_command, cursor)
        continuity = "continuous" if delta is not None else "unavailable"
        log_source = "journal"
    else:
        delta, continuity = dmesg_delta(dmesg_before,
                                        read_dmesg(args.dmesg_command))
        log_source = "dmesg"
    if delta:
        (out / "dmesg_delta.txt").write_text("\n".join(delta) + "\n")
    session = parsed["session"]
    if session.get("logFormatVersion") not in (None, QPA_LOG_FORMAT):
        refusal = refusal or "unknown_log_format"
    m = re.search(r"FATAL ERROR: [^\n]*", stderr_text)
    receipt["framework_abort"] = m.group(0) if m else None
    receipt["exit_code"] = exit_code
    receipt["session"] = session
    receipt["session_closed"] = parsed["session_closed"]
    receipt["partial_begin"] = parsed["partial_begin"]
    receipt["refusal"] = refusal
    receipt["dmesg"] = {"available": delta is not None,
                        "source": log_source, "continuity": continuity,
                        "delta_lines": len(delta) if delta else 0}
    if evidence == "silicon" and delta is None:
        grade, reason = False, f"kernel log {continuity} on a silicon run"
    receipt["decision_grade"] = grade
    receipt["decision_grade_reason"] = reason
    receipt["hazard_lines"] = hazard_lines(delta)
    counts, classes, blocking = summarize(results, ledger)
    receipt["counts"] = counts
    receipt["classes"] = classes
    receipt["blocking"] = blocking
    receipt["results"] = results
    receipt["unexpected_cases"] = unexpected
    receipt["result_count"] = sum(counts.values())
    receipt["verdict"] = verdict_for(receipt)
    receipt["artifacts"] = {n: sha256_file(out / n) for n in artifact_names
                            if (out / n).is_file()}
    receipt["seal_sha256"] = seal(receipt)
    (out / "receipt.json").write_text(json.dumps(receipt, indent=1,
                                                 sort_keys=True) + "\n")
    print(f"verdict={receipt['verdict']} evidence={evidence} "
          f"queue_claim={queue_claim['mode']} "
          f"decision_grade={grade} counts={counts} classes={classes} "
          f"seal={receipt['seal_sha256'][:12]}")
    return receipt


def verify_receipt(path):
    receipt = load_json(path, "receipt")
    if receipt.get("receipt_version") != RECEIPT_VERSION:
        raise RunnerRefusal("receipt version unknown")
    recorded = receipt.get("seal_sha256")
    if recorded != seal(receipt):
        raise RunnerRefusal("seal does not match the receipt body")
    base = Path(path).parent
    for name, digest in receipt.get("artifacts", {}).items():
        p = base / name
        if not p.is_file():
            raise RunnerRefusal(f"artifact {name} is missing")
        if sha256_file(p) != digest:
            raise RunnerRefusal(f"artifact {name} does not match its digest")
    if receipt.get("result_count") != sum(receipt["counts"].values()) or \
            receipt.get("result_count") != len(receipt["results"]):
        raise RunnerRefusal("result count does not reconcile")
    print(f"receipt verified: seal {recorded[:12]}, "
          f"{len(receipt.get('artifacts', {}))} artifacts, verdict "
          f"{receipt['verdict']}")


def check_ledgers(nonpass_path, slices_path, mustpass_dir=None):
    """Hold both TSVs to their contracts: every row compiles, every dEQP
    status is named, every row's witness (case|detail) classifies to
    that row through the real classify() so it is reachable and not
    shadowed, each slice is well-formed with a hazardous slice requiring
    silicon, and with a corpus every slice group and witness case is a
    mustpass case."""
    ledger = load_nonpass_ledger(nonpass_path)
    named = set()
    for r in ledger:
        for key in ("pattern", "detail"):
            if r[key] != "-":
                try:
                    re.compile(r[key])
                except re.error as e:
                    raise RunnerRefusal(f"{r['class']}: {key} {r[key]!r}: {e}")
        if r["status"] not in DEQP_STATUSES:
            raise RunnerRefusal(f"{r['class']}: status {r['status']} is not "
                                "a dEQP status")
        if r["disposition"] not in ("accepted", "blocks"):
            raise RunnerRefusal(f"{r['class']}: disposition must be accepted "
                                "or blocks")
        named.add(r["status"])
        case, _, detail = r["witness"].partition("|")
        got, _ = classify(case, r["status"], detail, ledger)
        if got != r["class"]:
            raise RunnerRefusal(f"{r['class']}: its witness {r['witness']!r} "
                                f"classifies as {got}, so the row is "
                                "unreachable or shadowed")
    missing = sorted(DEQP_STATUSES - PASS_STATUS - named)
    if missing:
        raise RunnerRefusal(f"no ledger row names status {missing}")
    verb_source = Path(__file__).resolve().parents[5] / COMPUTE_VERB_SOURCE
    if not verb_source.is_file():
        raise RunnerRefusal(f"{COMPUTE_VERB_SOURCE} is not beside this "
                            "runner; the gate pattern cannot be held to it")
    gates = re.findall(r'"(R3V_NATIVE_COMPUTE_[A-Z0-9_]+)"',
                       verb_source.read_text())
    unbound = sorted(g for g in set(gates)
                     if g != "R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL" and
                     not SUBMISSION_GATE_PATTERN.match(g))
    if not gates:
        raise RunnerRefusal(f"{COMPUTE_VERB_SOURCE} names no compute gate")
    if unbound:
        raise RunnerRefusal(f"compute verb gates outside the contamination "
                            f"pattern: {unbound}")
    lines = Path(slices_path).read_text().splitlines()
    if lines[0].split("\t") != SLICE_HEADER:
        raise RunnerRefusal(f"{slices_path} header is not {SLICE_HEADER}")
    orders = []
    groups = []
    for n, line in enumerate(lines[1:], start=2):
        f = line.split("\t")
        if len(f) != 5 or not all(f):
            raise RunnerRefusal(f"{slices_path}:{n} is not a five-field row")
        orders.append(int(f[0]))
        if f[3] not in SLICE_HAZARDS or f[4] not in SLICE_EVIDENCE:
            raise RunnerRefusal(f"{slices_path}:{n} hazard/evidence unknown")
        if f[3] != "none" and f[4] != "silicon":
            raise RunnerRefusal(f"{slices_path}:{n}: a hazardous slice "
                                "requires silicon evidence")
        for g in f[2].split(" "):
            if not re.fullmatch(r"dEQP-VK(\.[A-Za-z0-9_]+)+", g):
                raise RunnerRefusal(f"{slices_path}:{n}: {g!r} is not a group")
            groups.append(g)
    if orders != list(range(1, len(orders) + 1)):
        raise RunnerRefusal("slice order is not 1..N")
    corpus = None
    if mustpass_dir:
        cases = set()
        for f in Path(mustpass_dir).rglob("*.txt"):
            cases.update(l.strip() for l in f.read_text().splitlines()
                         if l.startswith("dEQP-VK."))
        for g in groups:
            if not any(c == g or c.startswith(g + ".") for c in cases):
                raise RunnerRefusal(f"slice group {g} matches no mustpass case")
        for r in ledger:
            case = r["witness"].partition("|")[0]
            if case not in cases:
                raise RunnerRefusal(f"{r['class']}: witness {case} is not a "
                                    "mustpass case")
        corpus = f"{len(cases)} mustpass cases"
    print(f"ledgers hold: {len(ledger)} non-pass rows with reachable "
          f"witnesses, {len(set(gates))} compute gates inside the "
          f"contamination pattern, {len(orders)} slices, {len(groups)} "
          "groups, "
          f"{corpus or 'mustpass clause not run (no corpus named)'}")


FAKE_DEQP = r'''#!/usr/bin/env python3
import os, sys, time, signal
mode = os.environ["FAKE_DEQP_MODE"]
log = [a.split("=",1)[1] for a in sys.argv if a.startswith("--deqp-log-filename=")][0]
cl = [a.split("=",1)[1] for a in sys.argv if a.startswith("--deqp-caselist-file=")][0]
cases = [l.strip() for l in open(cl) if l.strip()]
f = open(log, "w")
f.write('#sessionInfo releaseName fake-1\n#sessionInfo logFormatVersion "0.3.4"\n#beginSession\n'); f.flush()
def case(name, status, detail="x"):
    f.write(f"#beginTestCaseResult {name}\n<TestCaseResult CasePath=\"{name}\" Version=\"0.3.4\" CaseType=\"SelfValidate\">\n <Text>note</Text>\n <Result StatusCode=\"{status}\">{detail}</Result>\n</TestCaseResult>\n\n#endTestCaseResult\n"); f.flush()
if mode == "mixed":
    sts = ["Pass", "NotSupported", "Fail", "Pass", "QualityWarning"]
    for i, c in enumerate(cases): case(c, sts[i % len(sts)])
elif mode == "truncated":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n"); f.flush()
    sys.exit(0)
elif mode == "timeout":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n"); f.flush()
    time.sleep(30)
elif mode == "hang_after_session":
    for c in cases: case(c, "Pass")
    f.write("#endSession\n"); f.flush()
    time.sleep(30)
elif mode == "crash":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n"); f.flush()
    os.kill(os.getpid(), signal.SIGSEGV)
elif mode == "device_loss":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n <Result StatusCode=\"Fail\">vkQueueSubmit returned VK_ERROR_DEVICE_LOST</Result>\n#endTestCaseResult\n")
    f.write(f"#beginTestCaseResult {cases[2]}\n#terminateTestCaseResult Crash\n"); f.flush()
    sys.exit(1)
elif mode == "multiline":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n <Result StatusCode=\"Fail\">line one\nline two</Result>\n#endTestCaseResult\n")
    for c in cases[2:]: case(c, "Pass")
elif mode == "all_pass":
    for c in cases: case(c, "Pass")
elif mode == "late_abort":
    for c in cases: case(c, "Pass")
    sys.stderr.write("FATAL ERROR: late abort at foo.cpp:1\n")
elif mode == "framework":
    sys.stderr.write("FATAL ERROR: No matching queue found: findQueueFamilyIndexWithCaps(requiredCaps=0x3, excludedCaps=0x0) at vktTestCase.cpp:508\n")
    sys.exit(1)
elif mode == "per_case":
    cap = os.environ.get("FAKE_CAPTURE_FILE", "")
    if cap: open(cap, "w").write("transcript\n")
    if os.environ.get("FAKE_NO_IB_MSG"): sys.stderr.write("MESA: warning: r3v-native: no executable submission ran; no plan transcript written\n")
    if os.environ.get("FAKE_NO_IB_MARKER"): open(os.environ["R3V_NATIVE_PLAN_CAPTURE_FILE"] + ".no_nonempty_ib", "w").write("no_nonempty_ib\n")
    echo = os.environ.get("FAKE_ECHO_NAME", "")
    if echo: open(os.path.join(os.path.dirname(log), "env_echo.txt"), "w").write(os.environ.get(echo, ""))
    for c in cases:
        if c == os.environ.get("FAKE_HAZARD_CASE", ""):
            open(os.environ["FAKE_DMESG_FILE"], "a").write("[2.0] radeon 0000:01:05.0: GPU lockup\n")
        if c == os.environ.get("FAKE_CRASH_CASE", ""):
            f.write(f"#beginTestCaseResult {c}\n"); f.flush()
            os.kill(os.getpid(), signal.SIGSEGV)
        if c == os.environ.get("FAKE_SLOW_CASE", ""):
            f.write(f"#beginTestCaseResult {c}\n"); f.flush()
            time.sleep(30)
        case(c, "Pass")
elif mode == "replay":
    f.close()
    import shutil; shutil.copyfile(os.environ["FAKE_DEQP_REPLAY"], log)
    sys.exit(0)
f.write("#endSession\n"); f.close()
'''

FAKE_QUEUE_REPORT = '''#!/bin/sh
mode="$FAKE_QUEUE_MODE"
bit=1
consistent=1
gate=0
[ "$mode" = default_graphics_only ] && bit=0
[ "$mode" = experimental_compute_subset ] && gate=1
if [ "$mode" = gateless ]; then mode=experimental_compute_subset; fi
if [ "$mode" = inconsistent ]; then mode=conformant; consistent=0; fi
printf 'queue_family_count\t1\nqueue_flags\t0\t0x%x\tcount\t1\ttimestamp_bits\t0\n' $((bit ? 3 : 1))
printf 'compute_bit\t%s\nqueue_claim_mode\t%s\nqueue_claim_gate\t%s\n' "$bit" "$mode" "$gate"
printf 'verb_table_blake3\t%s\nclaim_consistent\t%s\n' 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "$consistent"
'''

# A stand-in tracer: it writes the census the real strace would (one
# GEM_CREATE line, then FAKE_STRACE_CS lines of DRM_IOCTL_RADEON_CS in
# the raw form r3v_cs_ioctl_trace parses) and executes the tracee, so
# the planning arms calibrate on a known-good zero and a known-bad count.
FAKE_STRACE = '''#!/bin/sh
out=""
while [ "$#" -gt 0 ]; do
  case "$1" in -o) out="$2"; shift 2;; --) shift; break;; *) shift;; esac
done
n="${FAKE_STRACE_CS:-0}"
{ printf 'ioctl(3, 0xc020645d, 0x1) = 0\n'; i=0
  while [ "$i" -lt "$n" ]; do printf 'ioctl(3, 0xc0206466, 0x1) = 0\n'; i=$((i+1)); done
} > "$out"
exec "$@"
'''

FAKE_DMESG = '''#!/bin/sh
cat "$FAKE_DMESG_FILE"
'''

SELFTEST_LEDGER = """class\tstatus\tcase_pattern\tdetail_pattern\tdisposition\tauthority\twitness
withheld_feature\tNotSupported\tdEQP-VK\\..*\t-\taccepted\tunimplemented optional path\tdEQP-VK.fake.a|
quality_warning\tQualityWarning\tdEQP-VK\\..*\t-\taccepted\tdEQP quality warning\tdEQP-VK.fake.a|
device_loss\tFail\tdEQP-VK\\.fake\\.b\tDEVICE_LOST\tblocks\tqueue loss classification\tdEQP-VK.fake.b|VK_ERROR_DEVICE_LOST
open_defect\tFail\tdEQP-VK\\.fake\\.[abd]\t-\tblocks\tcatch-all after the specific row\tdEQP-VK.fake.a|
deqp_crash\tCrash\tdEQP-VK\\..*\t-\tblocks\tdEQP-terminated case\tdEQP-VK.fake.c|
"""


def selftest(fixture_qpa):
    if fixture_qpa:
        fixture_qpa = str(Path(fixture_qpa).resolve())
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        fake = d / "deqp-vk"
        fake.write_text(FAKE_DEQP)
        fake.chmod(0o755)
        lib = d / "libvulkan_fake.so"
        lib.write_bytes(b"fake dso")
        manifest = d / "fake_icd.json"
        manifest.write_text(json.dumps({"file_format_version": "1.0.0",
                                        "ICD": {"library_path": str(lib),
                                                "api_version": "1.0.0"}}))
        caselist = d / "cases.txt"
        caselist.write_text("dEQP-VK.fake.a\ndEQP-VK.fake.b\ndEQP-VK.fake.c\n"
                            "dEQP-VK.fake.d\ndEQP-VK.fake.e\n")
        ledger = d / "ledger.tsv"
        ledger.write_text(SELFTEST_LEDGER)
        dmesg_file = d / "dmesg.txt"
        dmesg_file.write_text("[1.0] boot\n")
        dmesg_cmd = d / "dmesg.sh"
        dmesg_cmd.write_text(FAKE_DMESG)
        dmesg_cmd.chmod(0o755)
        fake_strace = d / "strace"
        fake_strace.write_text(FAKE_STRACE)
        fake_strace.chmod(0o755)

        run_index = [0]

        def run(mode, expect, dmesg_after=None, dso=None, cases=caselist,
                timeout=5.0, manifest_json=None, env=None,
                case_timeout=120.0, max_cases=MAX_SHARD_CASES,
                replace_dmesg=False, runtime_event=None,
                expect_runtime_event=None, outdir=None,
                queue_report=None, expect_queue_claim=None,
                expect_caselist=None, process_per_case=False,
                plan_nonce_file=None, force_evidence=None,
                strace_binary=str(fake_strace)):
            os.environ["FAKE_DEQP_MODE"] = mode
            os.environ["FAKE_DMESG_FILE"] = str(dmesg_file)
            os.environ["FAKE_DEQP_REPLAY"] = str(fixture_qpa or "")
            run_index[0] += 1
            outdir = outdir or d / f"out-{run_index[0]}-{mode}-{expect}"
            args = argparse.Namespace(
                deqp_binary=str(fake), icd=str(manifest),
                caselist=str(cases), out=str(outdir), source_root=None,
                build_root=None, expect_source_sha=None,
                expect_dso_sha256=dso, expect_deqp_sha256=None,
                expect_caselist_sha256=expect_caselist,
                expect_partition_sha256=None,
                expect_runtime_event_sha256=expect_runtime_event,
                runtime_event=runtime_event, timeout=timeout,
                case_timeout=case_timeout, max_cases=max_cases,
                journal_command="", nonpass_ledger=str(ledger),
                queue_report=queue_report,
                expect_queue_claim_mode=expect_queue_claim,
                expect_report_sha256=None,
                dmesg_command=str(dmesg_cmd),
                env=(env if env is not None else ["LD_PRELOAD=drm_shim"]) +
                    [f"FAKE_DEQP_MODE={mode}",
                     f"FAKE_DEQP_REPLAY={fixture_qpa or ''}"],
                deqp_arg=None, partition_manifest=manifest_json,
                process_per_case=process_per_case,
                plan_nonce_file=plan_nonce_file,
                strace_binary=strace_binary)
            if dmesg_after is not None:
                orig = dmesg_file.read_text()
                r = _run_with_dmesg_change(
                    args, dmesg_file, orig,
                    dmesg_after if replace_dmesg else orig + dmesg_after)
            elif force_evidence:
                r = _run_with_evidence(args, force_evidence)
            else:
                r = execute(args)
            if r["verdict"] != expect:
                raise SystemExit(
                    f"selftest {mode}: verdict {r['verdict']}, expected "
                    f"{expect}: {r['counts']} {r['classes']}")
            verify_receipt(outdir / "receipt.json")
            return r

        r = run("all_pass", "pass")
        assert r["counts"] == {"Pass": 5}, r["counts"]
        assert r["evidence_class"] == "host-model"
        assert r["decision_grade"] is False and \
            r["decision_grade_reason"] == "source identity unavailable"
        assert r["session"]["releaseName"] == "fake-1"
        # The process environment is allowlisted: the ambient gate below
        # never reaches the run, while the declared preload does.
        os.environ["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        r = run("all_pass", "pass")
        del os.environ["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"]
        assert "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED" not in r["environment"]
        assert r["environment"]["LD_PRELOAD"] == "drm_shim"
        assert set(r["environment"]) <= set(INHERITED_ENV) | {
            "LD_PRELOAD", "FAKE_DEQP_MODE", "FAKE_DEQP_REPLAY",
            "VK_DRIVER_FILES"} | {k for k in r["environment"]
                                  if k.startswith("LC_")}
        assert all(r["environment"][k].startswith("sha256:")
                   for k in r["environment"] if k in INHERITED_ENV)
        assert r["dmesg"]["continuity"] == "continuous"
        # A used output directory refuses before anything runs.
        used = d / "out-used"
        run("all_pass", "pass", outdir=used)
        try:
            run("all_pass", "pass", outdir=used)
        except RunnerRefusal as e:
            assert "not empty" in str(e)
        else:
            raise SystemExit("selftest: a used output directory was admitted")
        # The declared caselist digest is the file's bytes, the value the
        # partition manifest and sha256sum publish; the digest of the
        # parsed names alone names a different artifact and refuses.
        run("all_pass", "pass", expect_caselist=sha256_file(caselist))
        joined = hashlib.sha256(
            "\n".join(read_caselist(caselist)).encode()).hexdigest()
        r = run("all_pass", "wrong_caselist", expect_caselist=joined)
        assert r["caselist_sha256"] == sha256_file(caselist) != joined
        # A caselist above the shard ceiling refuses.
        try:
            run("all_pass", "pass", max_cases=2)
        except RunnerRefusal as e:
            assert "shard ceiling" in str(e)
        else:
            raise SystemExit("selftest: an oversized shard was admitted")
        # A kernel log whose before stream is not a prefix of the after
        # stream is broken continuity, never an invented delta.
        r = run("all_pass", "kernel_log_continuity_broken",
                dmesg_after="[0.5] earlier\n", replace_dmesg=True)
        assert r["dmesg"]["continuity"] == "broken" and \
            r["dmesg"]["available"] is False
        # The in-flight case's deadline kills the process and names it;
        # a process that hangs after closing its session is the runner
        # deadline's shape even under the case deadline; a kernel hazard
        # outranks the case deadline it most likely caused.
        r = run("timeout", "case_deadline", timeout=30.0, case_timeout=1.5)
        assert r["exit_code"] == "case_timeout" and \
            r["counts"].get("timeout") == 1, r["counts"]
        r = run("hang_after_session", "runner_deadline", timeout=30.0,
                case_timeout=1.5)
        assert r["session_closed"]
        r = run("timeout", "dmesg_hazard", timeout=30.0, case_timeout=1.5,
                dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n")
        assert r["exit_code"] == "case_timeout"
        r = run("hang_after_session", "dmesg_hazard", timeout=30.0,
                case_timeout=1.5,
                dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n")
        assert r["exit_code"] == "case_timeout" and r["session_closed"]
        r = run("hang_after_session", "dmesg_hazard", timeout=2.0,
                dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n")
        assert r["exit_code"] == "timeout"
        # The queue-claim report is recorded under the run's environment:
        # the mode names what the compute bit rests on, only the
        # conformant mode makes the receipt conformance-eligible, an
        # expected mode other than the reported one refuses, and an
        # inconsistent report refuses.
        report = d / "report.sh"
        report.write_text(FAKE_QUEUE_REPORT)
        report.chmod(0o755)
        r = run("all_pass", "pass", queue_report=str(report),
                env=["LD_PRELOAD=drm_shim", "FAKE_QUEUE_MODE="
                     "experimental_compute_subset"])
        assert r["queue_claim"]["mode"] == "experimental_compute_subset" \
            and r["queue_claim"]["compute_bit"] and \
            r["queue_claim"]["gate_declared"] and \
            not r["compute_claim_eligible"] and \
            r["queue_claim"]["report_sha256"]
        r = run("all_pass", "pass", queue_report=str(report),
                env=["LD_PRELOAD=drm_shim", "FAKE_QUEUE_MODE=conformant"])
        assert r["compute_claim_eligible"]
        # The gated mode without the gate is inconsistent.
        try:
            run("all_pass", "pass", queue_report=str(report),
                env=["LD_PRELOAD=drm_shim", "FAKE_QUEUE_MODE=gateless"])
        except RunnerRefusal as e:
            assert "gate False" in str(e)
        else:
            raise SystemExit("selftest: a gated mode without its gate was "
                             "admitted")
        r = run("all_pass", "wrong_queue_claim", queue_report=str(report),
                env=["LD_PRELOAD=drm_shim",
                     "FAKE_QUEUE_MODE=default_graphics_only"],
                expect_queue_claim="conformant")
        assert "argv" not in r and not r["queue_claim"]["compute_bit"]
        try:
            run("all_pass", "pass", queue_report=str(report),
                env=["LD_PRELOAD=drm_shim", "FAKE_QUEUE_MODE=inconsistent"])
        except RunnerRefusal as e:
            assert "queue-claim report refused" in str(e)
        else:
            raise SystemExit("selftest: an inconsistent queue report was "
                             "admitted")
        # A runtime event joins by digest; a wrong digest refuses.
        event = d / "event.json"
        event.write_text('{"run_id": "rs482-001", "boot_id": "b"}\n')
        digest = hashlib.sha256(event.read_bytes()).hexdigest()
        r = run("all_pass", "pass", runtime_event=str(event),
                expect_runtime_event=digest)
        assert r["runtime_event"]["run_id"] == "rs482-001" and \
            r["artifacts"].get("runtime_event.json") == digest
        r = run("all_pass", "wrong_runtime_event", runtime_event=str(event),
                expect_runtime_event="0" * 64)
        assert "argv" not in r
        r = run("mixed", "unclassified_nonpass")
        assert r["counts"] == {"Pass": 2, "NotSupported": 1, "Fail": 1,
                               "QualityWarning": 1}, r["counts"]
        assert r["classes"] == {"withheld_feature": 1, "quality_warning": 1,
                                "unclassified": 1}, r["classes"]
        r = run("truncated", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "truncated"
        assert r["results"]["dEQP-VK.fake.c"]["status"] == "not_run"
        r = run("timeout", "runner_deadline")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "timeout"
        r = run("hang_after_session", "runner_deadline")
        assert r["session_closed"] and r["counts"] == {"Pass": 5}
        r = run("crash", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "crash"
        assert "SIGSEGV" in r["results"]["dEQP-VK.fake.b"]["detail"]
        r = run("device_loss", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["class"] == "device_loss"
        assert r["results"]["dEQP-VK.fake.c"]["status"] == "Crash"
        assert r["results"]["dEQP-VK.fake.c"]["class"] == "deqp_crash"
        r = run("multiline", "classified_nonpass")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "Fail"
        assert r["results"]["dEQP-VK.fake.b"]["detail"] == "line one\nline two"
        r = run("late_abort", "pass")
        assert r["framework_abort"] and "late abort" in r["framework_abort"]
        r = run("framework", "framework_precondition")
        assert r["counts"] == {"not_run": 5}, r["counts"]
        assert r["classes"] == {"runner_not_run": 5}, r["classes"]
        r = run("all_pass", "wrong_icd", dso="0" * 64)
        assert r["counts"] == {"not_run": 5}
        r = run("all_pass", "dmesg_hazard",
                dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n")
        assert r["hazard_lines"]
        dup = d / "dup.txt"
        dup.write_text("dEQP-VK.fake.a\ndEQP-VK.fake.a\n")
        try:
            run("all_pass", "pass", cases=dup)
        except RunnerRefusal as e:
            assert "twice" in str(e)
        else:
            raise SystemExit("selftest: a duplicated caselist was admitted")
        # A partition manifest binds the caselist to its slice: a blocked
        # (unknown-hazard) slice refuses before dEQP starts, an executable
        # slice records its identity, and a caselist outside the manifest
        # refuses.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import r3v_conformance_partition as part
        corpus = d / "corpus"
        corpus.mkdir()
        (corpus / "m.txt").write_text(caselist.read_text() +
                                      "dEQP-VK.other.x\n")
        table = d / "partition.tsv"
        table.write_text("\t".join(part.HEADER) + "\n"
                         "1\tfake\tdEQP-VK.fake\tnone\thost-model\n"
                         "2\tother\tdEQP-VK.other\tunknown\tsilicon\n")
        pdir = d / "partition"
        cases_all = sorted(x.strip() for x in (corpus / "m.txt").read_text()
                           .splitlines() if x.strip())
        ppin = d / "pin.tsv"
        ppin.write_text(f"cts_describe\tfixture\ncase_count\t"
                        f"{len(cases_all)}\ncorpus_sha256\t"
                        f"{part.sha256_lines(cases_all)}\n")
        part.generate(table, corpus, pdir, "exhaustive", pin_path=ppin)
        mj = str(pdir / "partition_manifest.json")
        r = run("all_pass", "pass", cases=pdir / "fake.txt", manifest_json=mj)
        assert r["partition"]["slice"] == "fake" and \
            r["partition"]["kind"] == "exhaustive"
        r = run("all_pass", "blocked_slice", cases=pdir / "other.txt",
                manifest_json=mj)
        assert r["partition"]["hazard"] == "unknown" and "argv" not in r
        pm = json.loads((pdir / "partition_manifest.json").read_text())
        assert r["partition"]["caselist_sha256"] == \
            pm["slices"][1]["caselist_sha256"] and \
            r["partition"]["executable_case_count"] == \
            pm["executable_case_count"] == pm["slices"][0]["case_count"]
        # A silicon-required slice under the drm-shim host model refuses
        # before dEQP starts.
        table.write_text("\t".join(part.HEADER) + "\n"
                         "1\tfake\tdEQP-VK.fake\tsubmission\tsilicon\n"
                         "2\tother\tdEQP-VK.other\tunknown\tsilicon\n")
        sdir = d / "partition-silicon"
        part.generate(table, corpus, sdir, "exhaustive", pin_path=ppin)
        r = run("all_pass", "evidence_below_required",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"))
        assert "argv" not in r
        # A declared submission or experimental-route gate on a
        # hazard-free slice is contamination and refuses before dEQP.
        for gate in ("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1",
                     "R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL=1",
                     "R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL=1",
                     "R3V_NATIVE_PLAN_CAPTURE_FILE=/x"):
            r = run("all_pass", "gate_contamination", cases=pdir / "fake.txt",
                    manifest_json=mj, env=["LD_PRELOAD=drm_shim", gate])
            assert "argv" not in r and \
                r["contaminating_gates"] == [gate.split("=")[0]]
        # A display-hazard slice refuses a submission gate as well.
        table.write_text("\t".join(part.HEADER) + "\n"
                         "1\tfake\tdEQP-VK.fake\tdisplay\tsilicon\n"
                         "2\tother\tdEQP-VK.other\tunknown\tsilicon\n")
        ddir = d / "partition-display"
        part.generate(table, corpus, ddir, "exhaustive", pin_path=ppin)
        r = run("all_pass", "gate_contamination", cases=ddir / "fake.txt",
                manifest_json=str(ddir / "partition_manifest.json"),
                env=["LD_PRELOAD=drm_shim",
                     "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1"])
        assert "argv" not in r
        # A shard of a split slice binds by its own digest and records
        # its index; the shard ceiling matches the manifest's.
        pdir2 = d / "partition-sharded"
        table.write_text("\t".join(part.HEADER) + "\n"
                         "1\tfake\tdEQP-VK.fake\tnone\thost-model\n"
                         "2\tother\tdEQP-VK.other\tunknown\tsilicon\n")
        part.generate(table, corpus, pdir2, "exhaustive", pin_path=ppin,
                      shard_max=2)
        r = run("all_pass", "pass", cases=pdir2 / "fake.0001.txt",
                manifest_json=str(pdir2 / "partition_manifest.json"),
                max_cases=2)
        assert r["partition"]["shard_index"] == 1 and \
            r["partition"]["shard_count"] == 3 and \
            r["partition"]["shard_case_count"] == 2
        # A runner ceiling other than the manifest's refuses, sealed.
        r = run("all_pass", "shard_ceiling_mismatch",
                cases=pdir2 / "fake.0001.txt",
                manifest_json=str(pdir2 / "partition_manifest.json"),
                max_cases=3)
        assert "argv" not in r
        # The compute-queue framework gate is a declared, recorded value.
        r = run("all_pass", "pass", cases=pdir / "fake.txt", manifest_json=mj,
                env=["LD_PRELOAD=drm_shim",
                     "R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1"])
        assert r["environment"]["R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL"] == "1"
        assert r["decision_grade_reason"] == "source identity unavailable"
        # A proper subset of one shard binds as that shard's subset and
        # records its own count and digest; a case outside every shard
        # refuses under the manifest.
        subset = d / "subset.txt"
        subset.write_text("dEQP-VK.fake.a\n")
        r = run("all_pass", "pass", cases=subset, manifest_json=mj)
        assert r["partition"]["binding"] == "shard_subset" and \
            r["partition"]["slice"] == "fake" and \
            r["partition"]["subset_case_count"] == 1 and \
            r["partition"]["subset_caselist_sha256"] == \
            part.sha256_file(subset), r["partition"]
        assert r["partition"]["shard_case_count"] > 1
        stray = d / "stray.txt"
        stray.write_text("dEQP-VK.fake.a\ndEQP-VK.nowhere\n")
        try:
            run("all_pass", "pass", cases=stray, manifest_json=mj)
        except RunnerRefusal as e:
            assert "outside every manifest shard" in str(e)
        else:
            raise SystemExit("selftest: an unbound caselist was admitted "
                             "under a manifest")
        if fixture_qpa:
            real_cases = d / "real.txt"
            names = re.findall(r"^#beginTestCaseResult (\S+)",
                               Path(fixture_qpa).read_text(), re.M)
            real_cases.write_text("\n".join(names) + "\n")
            r = run("replay", "unclassified_nonpass", cases=real_cases)
            assert r["counts"] == {"Pass": 17, "NotSupported": 3,
                                   "Fail": 1}, r["counts"]
            assert r["session"]["logFormatVersion"] == QPA_LOG_FORMAT
            assert r["session"]["deviceID"] == "0x5974"
            assert "feature limits failed" in \
                r["results"]["dEQP-VK.info.device_properties"]["detail"]
        # One process apiece: the shard receipt stays one receipt, each
        # case's status comes from its own process's log, and the
        # per-case artifacts digest under that case's directory.
        base_env = ["LD_PRELOAD=drm_shim"]
        r = run("per_case", "pass", process_per_case=True, env=base_env)
        assert r["process_per_case"] and r["counts"] == {"Pass": 5}
        assert r["session_closed"] and r["exit_code"] == 0
        assert r["case_directories"]["dEQP-VK.fake.a"] == "dEQP-VK.fake.a"
        assert r["cases"]["dEQP-VK.fake.e"]["index"] == 5
        assert "cases/dEQP-VK.fake.c/run.qpa" in r["artifacts"]
        assert "run.qpa" not in r["artifacts"]
        assert len(r["artifacts"]) == 5 * len(CASE_ARTIFACT_NAMES) + 1
        # A case whose process dies keeps its own status, and the shard
        # stays readable: the crash classifies as that case's result
        # rather than truncating the shard.
        r = run("per_case", "classified_nonpass", process_per_case=True,
                env=base_env + ["FAKE_CRASH_CASE=dEQP-VK.fake.c"])
        assert r["results"]["dEQP-VK.fake.c"]["status"] == "crash"
        assert "SIGSEGV" in r["results"]["dEQP-VK.fake.c"]["detail"]
        assert r["counts"] == {"Pass": 4, "crash": 1}, r["counts"]
        assert r["session_closed"] and r["exit_code"] == 0
        assert r["cases"]["dEQP-VK.fake.c"]["exit_code"] == -11
        # The case deadline kills that case's process alone.
        r = run("per_case", "classified_nonpass", process_per_case=True,
                timeout=60.0, case_timeout=1.5,
                env=base_env + ["FAKE_SLOW_CASE=dEQP-VK.fake.c"])
        assert r["counts"] == {"Pass": 4, "timeout": 1}, r["counts"]
        assert r["cases"]["dEQP-VK.fake.c"]["exit_code"] == "case_timeout"
        # The shard deadline ends the sequence; the cases behind it
        # never ran.
        r = run("per_case", "runner_deadline", process_per_case=True,
                timeout=3.0, case_timeout=60.0,
                env=base_env + ["FAKE_SLOW_CASE=dEQP-VK.fake.b"])
        assert r["counts"]["not_run"] == 3 and not r["session_closed"]
        # A `{case}` value gives each case its own file: the receipt
        # records the resolved value and whether it existed before and
        # after the case ran, and the process reads the resolved value.
        caps = d / "caps"
        caps.mkdir()
        plans = d / "plans"
        plans.mkdir()
        (plans / "dEQP-VK.fake.b.plan").write_text("plan\n")
        templated_out = d / "out-templated"
        r = run("per_case", "pass", process_per_case=True,
                outdir=templated_out, env=base_env + [
                    f"FAKE_CAPTURE_FILE={caps}/{{index}}-{{case}}.transcript",
                    f"FAKE_PLAN_FILE={plans}/{{case}}.plan",
                    "FAKE_ECHO_NAME=FAKE_CAPTURE_FILE"])
        assert r["templated_env"] == ["FAKE_CAPTURE_FILE", "FAKE_PLAN_FILE"]
        assert r["index_width"] == 1
        rec = r["cases"]["dEQP-VK.fake.d"]
        assert rec["environment"]["FAKE_CAPTURE_FILE"] == \
            f"{caps}/4-dEQP-VK.fake.d.transcript"
        assert rec["paths"]["FAKE_CAPTURE_FILE"] == \
            {"present_before": False, "present_after": True}
        # A case whose plan file is absent still runs; the receipt names
        # which case resolved one that existed.
        assert r["cases"]["dEQP-VK.fake.b"]["paths"]["FAKE_PLAN_FILE"][
            "present_before"] is True
        assert r["cases"]["dEQP-VK.fake.a"]["paths"]["FAKE_PLAN_FILE"][
            "present_before"] is False
        assert r["counts"] == {"Pass": 5}
        assert (caps / "4-dEQP-VK.fake.d.transcript").is_file()
        # The resolved value reaches the case's own process, which
        # echoes it back into that case's directory.
        assert (templated_out / "cases" / "dEQP-VK.fake.d" /
                "env_echo.txt").read_text() == \
            f"{caps}/4-dEQP-VK.fake.d.transcript"
        # `{nonce}` resolves through the declared per-case nonce file.
        nonce_tsv = d / "nonces.tsv"
        nonce_tsv.write_text("".join(
            f"dEQP-VK.fake.{c}\t{i:032x}\n"
            for i, c in enumerate("abcde", start=1)))
        nonce_out = d / "out-nonce"
        r = run("per_case", "pass", process_per_case=True,
                plan_nonce_file=str(nonce_tsv), outdir=nonce_out,
                env=base_env + ["FAKE_NONCE={nonce}",
                                "FAKE_ECHO_NAME=FAKE_NONCE"])
        assert r["cases"]["dEQP-VK.fake.c"]["environment"]["FAKE_NONCE"] == \
            f"{3:032x}"
        assert r["plan_nonce_file"] == str(nonce_tsv)
        assert (nonce_out / "cases" / "dEQP-VK.fake.c" /
                "env_echo.txt").read_text() == f"{3:032x}"
        # A nonce token with no file, a malformed nonce, and a case the
        # file leaves out each refuse before dEQP starts.
        for bad, text, message in (
                ("no-file", None, "no --plan-nonce-file"),
                ("short", "dEQP-VK.fake.a\tabcd\n", "32 lowercase hex"),
                ("missing", f"dEQP-VK.fake.a\t{1:032x}\n",
                 "declares no nonce"),
                ("reused", "".join(f"dEQP-VK.fake.{c}\t{7:032x}\n"
                                   for c in "abcde"), "reuses a nonce")):
            path = None
            if text is not None:
                path = d / f"nonce-{bad}.tsv"
                path.write_text(text)
            try:
                run("per_case", "pass", process_per_case=True,
                    plan_nonce_file=str(path) if path else None,
                    env=base_env + ["FAKE_NONCE={nonce}"])
            except RunnerRefusal as e:
                assert message in str(e), str(e)
            else:
                raise SystemExit(f"selftest: nonce defect {bad} admitted")
        # A per-case token outside --process-per-case refuses: a single
        # process would pass the token to dEQP verbatim.
        try:
            run("all_pass", "pass",
                env=base_env + ["FAKE_CAPTURE_FILE=/x/{case}"])
        except RunnerRefusal as e:
            assert "outside --process-per-case" in str(e)
        else:
            raise SystemExit("selftest: a token reached a single process")
        # Two cases sharing one directory name refuse before the shard
        # runs; the second would overwrite the first's log.
        clash = d / "clash.txt"
        clash.write_text("dEQP-VK.fake.a b\ndEQP-VK.fake.a_b\n")
        try:
            run("per_case", "pass", cases=clash, process_per_case=True,
                env=base_env)
        except RunnerRefusal as e:
            assert "one directory per case" in str(e)
        else:
            raise SystemExit("selftest: colliding case directories admitted")
        # A kernel hazard stops the sequence where it appeared: the
        # cases behind it never run, and each names why.
        dmesg_file.write_text("[1.0] boot\n")
        r = run("per_case", "dmesg_hazard", process_per_case=True,
                env=base_env + ["FAKE_HAZARD_CASE=dEQP-VK.fake.b",
                                f"FAKE_DMESG_FILE={dmesg_file}"])
        assert r["hazard_lines"] and r["counts"] == {"Pass": 2,
                                                     "not_run": 3}, r["counts"]
        assert r["stop"]["after_case"] == "dEQP-VK.fake.b" and \
            r["stop"]["index"] == 2
        assert r["results"]["dEQP-VK.fake.e"]["detail"] == \
            "kernel hazard after this case"
        assert len(r["cases"]) == 2 and not r["session_closed"]
        dmesg_file.write_text("[1.0] boot\n")
        # The host-planning disposition: a capture file declared on a
        # submission-hazard slice under the radeon drm-shim, gates closed,
        # one process apiece, and a per-process witness of zero
        # kernel-entering CS ioctls.  The receipt carries the evidence
        # class host-planning, no decision grade, each case's outcome, and
        # every transcript's digest.
        shim_env = [f"LD_PRELOAD={d}/{RADEON_DRM_SHIM_BASENAME}"]
        arm_index = [0]

        def plan_env_for_arm():
            """Each arm gets its own capture root, so a transcript one
            arm wrote never answers for the next."""
            arm_index[0] += 1
            root = caps / f"arm{arm_index[0]}"
            root.mkdir()
            return shim_env + [
                f"R3V_NATIVE_PLAN_CAPTURE_FILE={root}/p_{{case}}.t"], root

        plan_env, root = plan_env_for_arm()
        r = run("per_case", "pass", cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, env=plan_env + [
                    f"FAKE_CAPTURE_FILE={root}/p_{{case}}.t"])
        pl = r["planning"]
        assert r["evidence_class"] == PLANNING_EVIDENCE and \
            pl["disposition"] == PLANNING_EVIDENCE and pl["candidate"]
        assert r["decision_grade"] is False and \
            r["decision_grade_reason"] == PLANNING_GRADE_REASON
        assert r["partition"]["required_evidence"] == "silicon"
        assert all(pl["conditions"].values()) and \
            pl["refused_conditions"] == [], pl
        assert pl["cs_witness"]["cs_ioctls"] == 0 and \
            pl["cs_witness"]["total_ioctls"] == 5 and \
            pl["cs_witness"]["unwitnessed_cases"] == []
        assert pl["outcomes"] == {"transcript": 5, "no_nonempty_ib": 0,
                                  "unresolved": 0}, pl["outcomes"]
        assert f"{root}/p_dEQP-VK.fake.a.t" in \
            pl["transcripts"]["dEQP-VK.fake.a"]
        assert "cases/dEQP-VK.fake.a/ioctl.strace" in r["artifacts"]
        assert r["cases"]["dEQP-VK.fake.a"]["planning"]["outcome"] == \
            "transcript"
        # A device that captured no executable submission writes no
        # transcript and leaves its marker; the receipt records
        # no_nonempty_ib as the outcome rather than an absence, from the
        # marker, and from the log line alone as the fallback.
        for signal_env in (["FAKE_NO_IB_MARKER=1"], ["FAKE_NO_IB_MSG=1"]):
            plan_env, root = plan_env_for_arm()
            r = run("per_case", "pass", cases=sdir / "fake.txt",
                    manifest_json=str(sdir / "partition_manifest.json"),
                    process_per_case=True, env=plan_env + signal_env)
            assert r["evidence_class"] == PLANNING_EVIDENCE
            assert r["planning"]["outcomes"] == {"transcript": 0,
                                                 "no_nonempty_ib": 5,
                                                 "unresolved": 0}
        assert r["cases"]["dEQP-VK.fake.a"]["planning"]["empty_markers"] \
            == [] and (root / "p_dEQP-VK.fake.a.t").exists() is False
        assert r["cases"]["dEQP-VK.fake.a"]["paths"][
            "R3V_NATIVE_PLAN_CAPTURE_FILE"]["present_after"] is False
        # Neither a transcript nor the message is unresolved.
        plan_env, _ = plan_env_for_arm()
        r = run("per_case", "pass", cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, env=plan_env)
        assert r["planning"]["outcomes"]["unresolved"] == 5
        # Known-bad witness: one kernel-entering CS ioctl refuses the
        # disposition and the evidence class stays host-model.
        plan_env, _ = plan_env_for_arm()
        r = run("per_case", "planning_cs_witnessed", cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, env=plan_env + ["FAKE_STRACE_CS=1"])
        assert r["evidence_class"] == "host-model" and \
            r["planning"]["disposition"] is None
        assert r["planning"]["cs_witness"]["cs_ioctls"] == 5 and \
            r["planning"]["refused_conditions"] == ["kernel_entering_cs_zero"]
        assert r["decision_grade"] is False
        # Each pre-run condition refuses by name.
        for extra, failed in (
                (["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1"],
                 "submit_hazard_gate_closed"),
                ([f"R3V_NATIVE_MANIFEST_DIR={d}"],
                 "attended_evidence_directory_absent"),
                ([f"R3V_NATIVE_PLAN_FILE={d}/x.plan"], "replay_plan_absent"),
                (["R3V_NATIVE_PLAN_NONCE=" + "0" * 32], "replay_plan_absent")):
            r = run("per_case", "planning_disposition_refused",
                    cases=sdir / "fake.txt",
                    manifest_json=str(sdir / "partition_manifest.json"),
                    process_per_case=True, env=plan_env + extra)
            assert r["planning"]["refused_conditions"] == [failed], \
                (extra, r["planning"]["refused_conditions"])
            assert "argv" not in r and r["evidence_class"] == "host-model"
        # A zero-valued gate is closed.
        plan_env, _ = plan_env_for_arm()
        r = run("per_case", "pass", cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, env=plan_env + [
                    "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=0"])
        assert r["evidence_class"] == PLANNING_EVIDENCE
        # A preload other than the radeon drm-shim is host-model without
        # the interposer the capture session needs.
        r = run("per_case", "planning_disposition_refused",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, env=[
                    "LD_PRELOAD=drm_shim",
                    f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/q_{{case}}.t"])
        assert r["planning"]["refused_conditions"] == \
            ["radeon_drm_shim_interposes_ioctl"]
        # One process for the whole shard gives no per-case capture.
        r = run("per_case", "planning_disposition_refused",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                env=shim_env + [f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/one.t"])
        assert r["planning"]["refused_conditions"] == ["one_process_per_case"]
        # No usable tracer leaves the CS count unwitnessed, which refuses.
        r = run("per_case", "planning_witness_unavailable",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, env=plan_env,
                strace_binary=str(d / "absent-strace"))
        assert r["planning"]["tracer"]["binary"] is None and \
            "argv" not in r
        # The same slice without a capture file stays below its required
        # evidence.
        r = run("per_case", "evidence_below_required", cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, env=base_env)
        assert not r["planning"]["candidate"] and "argv" not in r
        # A capture file on a silicon run refuses: a capture session
        # opens the CS ioctl with the hazard gate closed, which the
        # drm-shim host model alone answers.
        r = run("per_case", "capture_on_silicon", cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True, force_evidence="silicon",
                env=base_env + [
                    f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/s_{{case}}.t"])
        assert not r["planning"]["candidate"] and "argv" not in r
        # The submission-gate allowlist holds under templating: a
        # templated plan value on a hazard-free slice is contamination.
        r = run("per_case", "gate_contamination", cases=pdir / "fake.txt",
                manifest_json=mj, process_per_case=True,
                env=base_env + [
                    f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/{{case}}.t"])
        assert "argv" not in r and \
            r["contaminating_gates"] == ["R3V_NATIVE_PLAN_CAPTURE_FILE"]
        tampered = d / "out-1-all_pass-pass" / "receipt.json"
        body = json.loads(tampered.read_text())
        body["counts"]["Pass"] = 6
        tampered.write_text(json.dumps(body))
        try:
            verify_receipt(tampered)
        except RunnerRefusal:
            pass
        else:
            raise SystemExit("selftest: a tampered receipt verified")
    fixture_note = "real-qpa replay, " if fixture_qpa else ""
    print("selftest: pass, mixed (NotSupported never a pass; Fail "
          "unclassified blocks), truncated, timeout, hang-after-session, "
          "crash, device-loss with a terminated case, multi-line result, "
          "late abort, framework-abort, wrong-ICD, dmesg-hazard, duplicate "
          f"caselist, {fixture_note}tampered-receipt, and partition "
          "(bound slice, blocked slice, silicon-required slice under the "
          "host model, unbound caselist, bound shard), allowlisted "
          "environment, gate contamination (hazard-free and display "
          "slices), used output directory, shard ceiling, kernel-log "
          "continuity, case deadline with its runner-deadline and "
          "kernel-hazard precedences, runtime-event join, and queue-claim "
          "report (mode, eligibility, wrong mode, gateless, inconsistent), "
          "and one process apiece (shard receipt, a case's own crash and "
          "case deadline, the shard deadline, per-case templating with "
          "resolved-path presence, nonce-file resolution and its four "
          "refusals, a token outside the mode, colliding case "
          "directories, a kernel hazard stopping the sequence, the "
          "host-planning disposition (transcript, no_nonempty_ib, and "
          "unresolved outcomes, a witnessed CS ioctl, each gate condition "
          "refusing by name, a zero-valued gate closed, a foreign preload, "
          "a single-process shard, an absent tracer) with the planning "
          "candidate's silicon and hazard-free refusals, "
          "templated gate contamination) fixtures each yield their "
          "verdict")


def _run_with_evidence(args, forced):
    """The evidence class a run would carry on other hardware; the
    selftest host answers no RS4xx render node, so the silicon branch
    is reachable only by naming the class the classifier would give."""
    global evidence_class
    orig = evidence_class

    def forced_class(env, host, icd):
        return forced, None

    evidence_class = forced_class
    try:
        return execute(args)
    finally:
        evidence_class = orig


def _run_with_dmesg_change(args, dmesg_file, before, after):
    """The fake dmesg reads a file; swap its content between the two
    reads by wrapping read_dmesg."""
    global read_dmesg
    orig = read_dmesg
    state = {"n": 0}

    def swapped(command):
        state["n"] += 1
        dmesg_file.write_text(before if state["n"] == 1 else after)
        return orig(command)
    read_dmesg = swapped
    try:
        return execute(args)
    finally:
        read_dmesg = orig


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--deqp-binary", required=True)
    r.add_argument("--icd", required=True)
    r.add_argument("--caselist", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--source-root")
    r.add_argument("--build-root")
    r.add_argument("--expect-source-sha")
    r.add_argument("--expect-dso-sha256")
    r.add_argument("--expect-deqp-sha256")
    r.add_argument("--expect-caselist-sha256")
    r.add_argument("--expect-partition-sha256")
    r.add_argument("--expect-runtime-event-sha256")
    r.add_argument("--runtime-event")
    r.add_argument("--queue-report")
    r.add_argument("--expect-queue-claim-mode", choices=QUEUE_CLAIM_MODES)
    r.add_argument("--expect-report-sha256")
    r.add_argument("--timeout", type=float, default=3600.0)
    r.add_argument("--case-timeout", type=float, default=120.0)
    r.add_argument("--max-cases", type=int, default=MAX_SHARD_CASES)
    r.add_argument("--journal-command", default="journalctl")
    r.add_argument("--nonpass-ledger")
    r.add_argument("--dmesg-command", default="dmesg")
    r.add_argument("--env", action="append")
    r.add_argument("--deqp-arg", action="append")
    r.add_argument("--partition-manifest")
    r.add_argument("--process-per-case", action="store_true")
    r.add_argument("--plan-nonce-file")
    r.add_argument("--strace-binary")
    s = sub.add_parser("selftest")
    s.add_argument("--fixture-qpa")
    c = sub.add_parser("check-ledgers")
    c.add_argument("--nonpass-ledger", required=True)
    c.add_argument("--slices", required=True)
    v = sub.add_parser("verify-receipt")
    v.add_argument("--receipt", required=True)
    args = p.parse_args()
    try:
        if args.cmd == "selftest":
            selftest(args.fixture_qpa)
        elif args.cmd == "check-ledgers":
            check_ledgers(args.nonpass_ledger, args.slices,
                          os.environ.get("R3V_DEQP_MUSTPASS_DIR"))
        elif args.cmd == "verify-receipt":
            verify_receipt(args.receipt)
        else:
            receipt = execute(args)
            sys.exit(0 if receipt["verdict"] in
                     ("pass", "pass_with_accepted_nonpass") else 1)
    except RunnerRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
