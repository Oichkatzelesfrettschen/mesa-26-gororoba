# SPDX-License-Identifier: MIT
"""Identity-retaining dEQP-VK conformance runner for the R3V ICD.

A conformance result is valid for qualification only when it binds to one exact
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

The evidence class derives from the run: a drm-shim observed in a child
process's memory map makes it host-model, and only an unpreloaded run whose ICD is libvulkan_r3v.so
on a host whose render node resolves to an RS4xx PCI device is silicon;
a run without source identity, with a dirty tree, or with a SHA other
than the declared one is never valid for qualification.

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
evidence class, `host-planning`, and is never valid for qualification.  It opens on
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
that no executable submission ran through a fresh per-device marker, or
`unresolved`.  A shard containing an unresolved outcome remains
`planning_capture_incomplete`.  The slice's
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
import r3v_cs_ioctl_trace as cs_trace

RECEIPT_VERSION = 4
LEGACY_RECEIPT_VERSION = 3
LEGACY_QUALIFICATION_VALIDITY_FIELD = "decision_grade"
LEGACY_QUALIFICATION_REASON_FIELD = "decision_grade_reason"

PASS_STATUS = {"Pass"}
DEQP_STATUSES = {
    "Pass",
    "Fail",
    "QualityWarning",
    "CompatibilityWarning",
    "Pending",
    "NotSupported",
    "ResourceError",
    "InternalError",
    "Crash",
    "Timeout",
    "Waiver",
}
RUNNER_STATUSES = {"crash", "timeout", "not_run", "truncated"}
QPA_LOG_FORMAT = "0.3.4"
R3V_ICD_BASENAME = "libvulkan_r3v.so"

DRIVER_ENV_PREFIXES = (
    "R3V_",
    "VK_",
    "RADEON_",
    "LD_PRELOAD",
    "MESA_",
    "DISPLAY",
    "XDG_RUNTIME_DIR",
)
# The environment a run inherits: the process needs these to start, and
# the driver reads other names.  Everything else enters only through an
# explicit --env declaration and lands whole in the sealed receipt.
INHERITED_ENV = ("PATH", "USER", "LOGNAME", "SHELL", "LANG", "TERM", "TMPDIR")
INHERITED_ENV_PREFIXES = ("LC_",)
# Declared values that open a submission, an experimental route, or an
# attended-evidence destination; only a submission-hazard slice admits
# them, and the driver's r3v_native_plan_gates_open refuses the same
# names as contamination in a plan run.  check-ledgers holds the pattern
# to every compute verb gate the ledger names.
SUBMISSION_GATE_PREFIXES = (
    "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED",
    "R3V_NATIVE_AUTHORIZED_",
    "R3V_NATIVE_R2VB_",
    "R3V_NATIVE_PLAN_",
    "R3V_NATIVE_MANIFEST_DIR",
)
SUBMISSION_GATE_PATTERN = re.compile(r"^R3V_NATIVE_COMPUTE_.*_GPU_EXPERIMENTAL$")
# Per-route compute gates live in the route ledger; the verb table carries
# the queue-claim gate alone.  Both are scanned so a gate cannot be moved
# into either file unnoticed.
COMPUTE_VERB_SOURCE = "src/amd/r300/common/r300_compute_verb.c"
COMPUTE_ROUTE_SOURCE = "src/amd/r300/common/r300_operation_route.c"
# A shard is a recovery unit: one process over this many cases at most.
MAX_SHARD_CASES = 20000
DMESG_HAZARD_PATTERNS = [
    r"GPU lockup",
    r"ring \d+ stalled",
    r"\[drm:.*\] \*ERROR\*",
    r"radeon.*CS.*(invalid|failed|error)",
    r"GPU reset",
    r"Unable to handle kernel",
    r"BUG:",
    r"Oops:",
    r"soft lockup",
]
RS4XX_PCI_DEVICES = {"0x5954", "0x5955", "0x5974", "0x5975"}
ARTIFACT_NAMES = (
    "run.qpa",
    "stdout.txt",
    "stderr.txt",
    "dmesg_delta.txt",
    "caselist.txt",
    "runtime_event.json",
)
# One process apiece puts each case's dEQP artifacts in that case's own
# directory, and the shard keeps the artifacts the shard itself writes.
CASE_ARTIFACT_NAMES = ("run.qpa", "stdout.txt", "stderr.txt", "caselist.txt")
SHARD_ARTIFACT_NAMES = ("dmesg_delta.txt", "caselist.txt", "runtime_event.json")
# A declared --env value carries these tokens, and each case resolves
# them before its own process starts.
ENV_CASE_TOKEN = re.compile(r"\{(case|index|nonce)\}")
CASE_NAME_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]")
MAX_CASE_DIRECTORY_BYTES = 120
PLAN_NONCE_PATTERN = re.compile(r"[0-9a-f]{32}")
REQUIRED_QPA_SESSION_FIELDS = ("releaseName", "logFormatVersion")

# The host-planning disposition: the planning pass as its own evidence
# class.  The shim basename is the one
# r3v_native_plan_capture_host_model_present resolves the ioctl symbol to
# (rg --fixed-strings r3v_native_plan_capture_host_model_present
# src/amd/r300/vulkan/); the gate names are the ones r3v_native_device.c
# reads at device creation (rg --fixed-strings R3V_NATIVE_PLAN_CAPTURE_FILE
# src/amd/r300/vulkan/); the message is the one the device logs at destroy
# when it captured no executable submission.
PLANNING_EVIDENCE = "host-planning"
RADEON_DRM_SHIM_BASENAME = "libradeon_noop_drm_shim.so"
CAPTURE_FILE_NAME = "R3V_NATIVE_PLAN_CAPTURE_FILE"
SUBMIT_HAZARD_GATE = "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"
ATTENDED_EVIDENCE_DIR = "R3V_NATIVE_MANIFEST_DIR"
PLAN_REPLAY_NAMES = ("R3V_NATIVE_PLAN_FILE", "R3V_NATIVE_PLAN_NONCE")
EMPTY_CAPTURE_SUFFIX = ".no_nonempty_ib"
CASE_STRACE_NAME = "ioctl.strace"
PLANNING_OUTCOMES = ("transcript", "no_nonempty_ib", "unresolved")
PLAN_CAPTURE_TERMINAL_ERRORS = (
    "r3v-native: plan capture refused:",
    "r3v-native: plan transcript write at destroy failed:",
    "no plan transcript written; marker:",
)
PLANNING_QUALIFICATION_REASON = (
    "host-planning disposition captures transcripts on the host model and "
    "proves nothing about conformance; the slice's silicon requirement stands"
)

LEDGER_HEADER = [
    "class",
    "status",
    "case_pattern",
    "detail_pattern",
    "disposition",
    "authority",
    "witness",
]
SLICE_HEADER = ["order", "slice", "groups", "hazard", "required_evidence"]
SLICE_HAZARDS = {"none", "submission", "display"}
SLICE_EVIDENCE = {"host-model", "silicon"}


class RunnerRefusal(Exception):
    pass


def partition_identity(manifest_path, caselist_path, ad_hoc_hazard):
    """Bind the caselist to a slice of a verified partition manifest and
    refuse a slice whose hazard is unknown.  With no manifest the run is
    ad hoc: it names no slice and no corpus."""
    if manifest_path is None:
        if ad_hoc_hazard is None:
            raise RunnerRefusal("an ad-hoc run requires --ad-hoc-hazard")
        return {
            "kind": "ad-hoc",
            "hazard": ad_hoc_hazard,
            "required_evidence": "host-model",
        }, None
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import r3v_conformance_partition as part

    try:
        manifest = part.verify_manifest(manifest_path)
        s, shard, subset = part.bind_caselist(manifest_path, manifest, caselist_path)
    except part.PartitionRefusal as e:
        raise RunnerRefusal(f"partition manifest: {e}")
    ident = {
        "kind": manifest["kind"],
        "cts_describe": manifest.get("cts_describe"),
        "manifest_sha256": manifest["manifest_sha256"],
        "corpus_sha256": manifest["corpus_sha256"],
        "corpus_case_count": manifest["corpus_case_count"],
        "covered_case_count": manifest["covered_case_count"],
        "executable_case_count": manifest["executable_case_count"],
        "uncovered_case_count": manifest["uncovered_case_count"],
        "slice": s["slice"],
        "order": s["order"],
        "hazard": s["hazard"],
        "required_evidence": s["required_evidence"],
        "case_count": s["case_count"],
        "caselist_sha256": s["caselist_sha256"],
        "shard_max_cases": s["shard_max_cases"],
        "shard_index": shard["index"],
        "shard_count": s["shard_count"],
        "shard_case_count": shard["case_count"],
        "shard_caselist_sha256": shard["caselist_sha256"],
        # A caselist that is a proper subset of its shard keeps the
        # shard identity above and records its own count and digest
        # here; a whole-shard run binds with subset None.
        "binding": "shard" if subset is None else "shard_subset",
        "subset_case_count": None if subset is None else subset["case_count"],
        "subset_caselist_sha256": None if subset is None else subset["caselist_sha256"],
    }
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
        p = subprocess.run(
            argv,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=60,
            env=env,
            check=False,
        )
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
    return {
        "available": True,
        "sha": sha,
        "clean": status == "",
        "dirty_entries": len(status.splitlines()),
    }


def build_identity(root):
    if root is None:
        return {"available": False}
    opts = Path(root) / "meson-info" / "intro-buildoptions.json"
    if not opts.is_file():
        return {"available": False, "error": f"{opts} absent"}
    data = load_json(opts, "build options")
    selected = {
        o["name"]: o["value"]
        for o in data
        if o.get("section") in ("user", "core")
        and o["name"]
        in (
            "buildtype",
            "vulkan-drivers",
            "gallium-drivers",
            "werror",
            "b_sanitize",
            "tools",
            "optimization",
        )
    }
    return {
        "available": True,
        "options": selected,
        "buildoptions_sha256": sha256_file(opts),
    }


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
    return {
        "manifest": str(Path(manifest).resolve()),
        "library_path": str(lib_path.resolve()),
        "library_basename": lib_path.name,
        "dso_sha256": sha256_file(lib_path),
        "api_version": data.get("ICD", {}).get("api_version"),
    }


def bundle_cts_describe(binary, digest):
    """Read the CTS revision a provisioned bundle carries.

    `r3v_deqp_provision.py bundle` writes a standalone directory -- binary,
    data, mustpass -- with no git worktree, and records the source checkout's
    `git describe` in `provenance.json` after refusing a checkout whose
    describe differs from the corpus pin.  The document seals itself with the
    SHA-256 the provisioner computed over its body and names the digest of the
    binary it describes, so the revision it reports holds for that executable
    alone.  A document that is absent, unparseable, unsealed, resealed over
    edited bytes, or describing another binary yields nothing, and the caller's
    comparison against the pin refuses the run.
    """
    document = Path(binary).resolve().parent / "provenance.json"
    if not document.is_file():
        return None
    try:
        provenance = json.loads(document.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(provenance, dict):
        return None
    seal = provenance.pop("provenance_sha256", None)
    body = json.dumps(provenance, sort_keys=True, separators=(",", ":")).encode()
    if not isinstance(seal, str) or hashlib.sha256(body).hexdigest() != seal:
        return None
    described = provenance.get("binary")
    if not isinstance(described, dict) or described.get("sha256") != digest:
        return None
    source = provenance.get("source")
    if not isinstance(source, dict):
        return None
    describe = source.get("describe")
    return describe if isinstance(describe, str) and describe else None


def cts_revision(deqp):
    """The observed CTS revision, worktree first.

    A binary inside its own checkout answers from the checkout, which also
    reports a dirty tree; a provisioned bundle answers from its sealed
    provenance.  Both authorities name the same `git describe` string the
    corpus pin carries."""
    return deqp.get("cts_worktree_describe") or deqp.get("cts_bundle_describe")


def deqp_identity(binary):
    b = Path(binary)
    if not b.is_file():
        raise RunnerRefusal(f"dEQP binary {binary} does not exist")
    ident = {"binary": str(b.resolve()), "sha256": sha256_file(b)}
    repo = b.resolve()
    for _ in range(8):
        repo = repo.parent
        if (repo / ".git").exists():
            rc, desc = run_capture(
                ["git", "describe", "--tags", "--always", "--dirty"], cwd=repo
            )
            ident["cts_worktree_describe"] = desc if rc == 0 else None
            break
    ident["cts_bundle_describe"] = bundle_cts_describe(b, ident["sha256"])
    ident["cts_identity_authority"] = (
        "worktree"
        if ident.get("cts_worktree_describe")
        else "bundle_provenance"
        if ident["cts_bundle_describe"]
        else None
    )
    return ident


def preload_identity(env, working_directory):
    """Prove that a declared drm-shim entered a process through the dynamic
    loader.  A declaration is metadata; the child process's memory map is the
    observation that makes host-model classification available."""
    declared = [
        entry
        for entry in re.split(r"[:\s]+", env.get("LD_PRELOAD", ""))
        if entry and ("drm_shim" in entry or "drm-shim" in entry)
    ]
    if not declared:
        return {"declared": False, "shim_loaded": False, "mapped": []}
    probe = (
        "from pathlib import Path; "
        "print('\\n'.join(sorted({line.rsplit(None, 1)[-1] "
        "for line in Path('/proc/self/maps').read_text().splitlines() "
        "if '/' in line})))"
    )
    try:
        completed = subprocess.run(
            [sys.executable, "-c", probe],
            cwd=working_directory,
            capture_output=True,
            text=True,
            timeout=30,
            env=env,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return {
            "declared": True,
            "shim_loaded": False,
            "mapped": [],
            "error": str(error),
        }
    mapped = sorted(
        line.strip() for line in completed.stdout.splitlines() if line.startswith("/")
    )
    declared_basenames = {Path(entry).name for entry in declared}
    bare_declarations = {entry for entry in declared if "/" not in entry}
    resolved_declarations = {
        str(
            (
                Path(entry)
                if Path(entry).is_absolute()
                else Path(working_directory) / entry
            ).resolve()
        )
        for entry in declared
        if "/" in entry
    }
    matched = [
        path
        for path in mapped
        if path in resolved_declarations or Path(path).name in bare_declarations
    ]
    loaded = completed.returncode == 0 and bool(matched)
    result = {
        "declared": True,
        "shim_loaded": loaded,
        "declared_basenames": sorted(declared_basenames),
        "mapped": matched,
    }
    if completed.returncode != 0 or not loaded:
        result["error"] = (
            completed.stderr or "declared shim absent from process maps"
        ).strip()
    return result


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
    ident = {
        "kernel_release": platform.release(),
        "machine": platform.machine(),
        "boot_id": read_optional("/proc/sys/kernel/random/boot_id"),
        "radeon_srcversion": read_optional("/sys/module/radeon/srcversion"),
        "hostname_sha256": hashlib.sha256(platform.node().encode()).hexdigest()[:16],
    }
    rc, pkgs = run_capture(["pacman", "-Q"])
    if rc == 0:
        ident["packages"] = sorted(
            package_line
            for package_line in pkgs.splitlines()
            if any(
                package_name in package_line
                for package_name in ("mesa", "vulkan", "radeon", "linux")
            )
        )
    gpus = []
    for dev in sorted(Path("/sys/bus/pci/devices").glob("*")):
        cls = read_optional(dev / "class") or ""
        if cls.startswith("0x03"):
            gpus.append(
                {
                    "slot": dev.name,
                    "vendor": read_optional(dev / "vendor"),
                    "device": read_optional(dev / "device"),
                    "revision": read_optional(dev / "revision"),
                    "subsystem_vendor": read_optional(dev / "subsystem_vendor"),
                    "subsystem_device": read_optional(dev / "subsystem_device"),
                }
            )
    ident["gpus"] = gpus
    ident["render_nodes"] = render_nodes()
    return ident


def build_environment(declared, hazard, isolated_home):
    """The allowlisted process environment plus the declared values.  A
    declared submission gate is admitted on a submission-hazard slice
    alone; on every other slice it is contamination, refused by name."""
    env = {
        k: v
        for k, v in os.environ.items()
        if k in INHERITED_ENV or k.startswith(INHERITED_ENV_PREFIXES)
    }
    isolated_home.mkdir()
    env["HOME"] = str(isolated_home)
    for kv in declared or []:
        k, sep, v = kv.partition("=")
        if not sep or not k:
            raise RunnerRefusal(f"--env {kv!r} is not KEY=VALUE")
        env[k] = v
    contaminating = sorted(
        k
        for k in env
        if k.startswith(SUBMISSION_GATE_PREFIXES) or SUBMISSION_GATE_PATTERN.match(k)
    )
    if hazard != "submission" and contaminating:
        return env, contaminating
    return env, []


def environment_identity(env, declared):
    """The whole run environment, declared values verbatim and inherited
    values as digests: the closure claim needs every name, and the
    operator's paths and identity stay out of a retained receipt."""
    declared_names = {kv.partition("=")[0] for kv in declared or []}
    return {
        k: (
            v
            if k in declared_names or k == "VK_DRIVER_FILES"
            else "sha256:" + hashlib.sha256(v.encode()).hexdigest()[:16]
        )
        for k, v in sorted(env.items())
    }


def evidence_class(env, host, icd, preload):
    """host-model under a drm-shim preload; silicon only when the ICD is
    the r3v DSO and a render node resolves to an RS4xx PCI device."""
    if preload["shim_loaded"]:
        return "host-model", None
    rs4xx_slots = {
        g["slot"]
        for g in host["gpus"]
        if g["vendor"] == "0x1002" and g["device"] in RS4XX_PCI_DEVICES
    }
    node = next((n for n in host["render_nodes"] if n["slot"] in rs4xx_slots), None)
    if node and icd["library_basename"] == R3V_ICD_BASENAME:
        return "silicon", node
    return "host-unknown", None


def radeon_drm_shim_interposes(env):
    """The radeon drm-shim in the preload path by exact basename, the
    same test the driver's capture admission makes on the resolved
    ioctl symbol; any other preload leaves the CS ioctl unanswered."""
    return any(
        Path(p).name == RADEON_DRM_SHIM_BASENAME
        for p in re.split(r"[:\s]+", env.get("LD_PRELOAD", ""))
        if p
    )


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
        "radeon_drm_shim_interposes_ioctl": evidence == "host-model"
        and radeon_drm_shim_interposes(env),
        "plan_capture_file_declared": bool(env.get(CAPTURE_FILE_NAME)),
        "submit_hazard_gate_closed": gate_closed(env.get(SUBMIT_HAZARD_GATE)),
        "attended_evidence_directory_absent": not env.get(ATTENDED_EVIDENCE_DIR),
        "replay_plan_absent": not any(env.get(n) for n in PLAN_REPLAY_NAMES),
        "one_process_per_case": bool(process_per_case),
    }


def planning_tracer(declared):
    """The strace the planning witness runs each case under: the
    declared binary when it exists and executes, else the one
    r3v_cs_ioctl_trace proves can attach."""
    if declared:
        resolved = Path(declared).resolve()
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return str(resolved), None
        return None, f"{declared} is not an executable file"
    return cs_trace.strace_available()


def artifact_fingerprint(path):
    stat_result = path.stat()
    return {
        "device": stat_result.st_dev,
        "inode": stat_result.st_ino,
        "size": stat_result.st_size,
        "mtime_ns": stat_result.st_mtime_ns,
        "sha256": sha256_file(path),
    }


def planning_family(capture_path):
    if not capture_path:
        return {}
    base = Path(capture_path)
    return {
        str(path): artifact_fingerprint(path)
        for path in [base]
        + sorted(path for path in base.parent.glob(base.name + ".*") if path.is_file())
        if path.is_file()
    }


def planning_member_ordinal(base, path, marker):
    suffix = path.name[len(base.name) :]
    if marker:
        suffix = suffix[: -len(EMPTY_CAPTURE_SUFFIX)]
    if not suffix:
        return 0
    if re.fullmatch(r"\.\d+", suffix):
        return int(suffix[1:])
    return None


def planning_outcome(case_dir, capture_path, before, stderr_text):
    """One case's planning outcome from its own process: the transcripts
    the device wrote at the declared path and its `.N` ordinals, each
    digested; `no_nonempty_ib` when the device left its
    `.no_nonempty_ib` marker (r3v_native_plan_capture_mark_empty, written
    at destroy when zero entries were captured); `unresolved` otherwise,
    which covers stale artifacts, missing or conflicting device ordinals,
    a device the driver refused, and a process that died.  The per-process
    strace gives the kernel-entering ioctl counts."""
    strace = case_dir / CASE_STRACE_NAME
    witnessed = strace.is_file()
    counts, unparsed = cs_trace.parse_strace(strace) if witnessed else ({}, 0)
    terminal_errors = [
        message for message in PLAN_CAPTURE_TERMINAL_ERRORS if message in stderr_text
    ]
    transcripts = {}
    markers = []
    device_outcomes = {}
    if capture_path:
        base = Path(capture_path)
        after = planning_family(capture_path)
        fresh = {name: state for name, state in after.items() if name not in before}
        for name, state in sorted(fresh.items()):
            path = Path(name)
            marker = path.name.endswith(EMPTY_CAPTURE_SUFFIX)
            ordinal = planning_member_ordinal(base, path, marker)
            if ordinal is None:
                continue
            kind = "no_nonempty_ib" if marker else "transcript"
            if ordinal in device_outcomes:
                device_outcomes[ordinal] = "conflict"
            else:
                device_outcomes[ordinal] = kind
            if marker:
                markers.append(name)
            else:
                transcripts[name] = state["sha256"]
    ordinals_complete = bool(device_outcomes) and set(device_outcomes) == set(
        range(max(device_outcomes) + 1)
    )
    outcomes_consistent = (
        ordinals_complete and "conflict" not in device_outcomes.values()
    )
    if not terminal_errors and outcomes_consistent and transcripts:
        outcome = "transcript"
    elif (
        not terminal_errors
        and outcomes_consistent
        and markers
        and all(value == "no_nonempty_ib" for value in device_outcomes.values())
    ):
        outcome = "no_nonempty_ib"
    else:
        outcome = "unresolved"
    return {
        "outcome": outcome,
        "transcripts": transcripts,
        "empty_markers": markers,
        "device_outcomes": {
            str(key): value for key, value in sorted(device_outcomes.items())
        },
        "terminal_errors": terminal_errors,
        "witnessed": witnessed,
        "cs_ioctls": counts.get(cs_trace.CS_REQUEST, 0),
        "total_ioctls": sum(counts.values()),
        "unparsed_ioctl_lines": unparsed,
    }


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
    rc, out = run_capture(
        shlex.split(journal_command) + ["-k", "-n", "1", "-o", "cat", "--show-cursor"]
    )
    if rc != 0:
        return None
    m = re.search(r"^-- cursor: (\S+)", out, re.MULTILINE)
    return m.group(1) if m else None


def journal_after(journal_command, cursor):
    rc, out = run_capture(
        shlex.split(journal_command)
        + ["-k", "-o", "short-monotonic", f"--after-cursor={cursor}"]
    )
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
    if after[: len(before)] != before:
        return None, "broken"
    return after[len(before) :], "continuous"


def hazard_lines(delta):
    if not delta:
        return []
    return [
        log_line
        for log_line in delta
        if any(re.search(pattern, log_line) for pattern in DMESG_HAZARD_PATTERNS)
    ]


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
    completed_result = None
    partial_begin = False
    for line in text.splitlines():
        if line.startswith("#sessionInfo "):
            parts = line.split(" ", 2)
            if len(parts) == 3:
                session[parts[1]] = parts[2].strip().strip('"')
        elif line.startswith("#beginTestCaseResult"):
            name = line[len("#beginTestCaseResult") :].strip()
            if not name:
                partial_begin = True
                continue
            if name in results:
                raise RunnerRefusal(f"QPA log reports {name} more than once")
            in_flight = name
            results[in_flight] = {"status": "truncated", "detail": ""}
            completed_result = None
        elif line.startswith("#endTestCaseResult"):
            if in_flight and completed_result is not None:
                results[in_flight] = completed_result
            in_flight = None
            result_text = None
            completed_result = None
        elif line.startswith("#terminateTestCaseResult"):
            parts = line.split(" ", 1)
            status = parts[1].strip() if len(parts) > 1 else "Crash"
            if in_flight:
                results[in_flight] = {"status": status, "detail": "terminated"}
            in_flight = None
            result_text = None
            completed_result = None
        elif in_flight:
            if result_text is None:
                m = re.search(r'<Result StatusCode="(\w+)">(.*)', line)
                if m:
                    status, rest = m.group(1), m.group(2)
                    if "</Result>" in rest:
                        completed_result = {
                            "status": status,
                            "detail": rest.split("</Result>")[0],
                        }
                    else:
                        result_text = [status, rest]
            else:
                if "</Result>" in line:
                    result_text[1] += "\n" + line.split("</Result>")[0]
                    completed_result = {
                        "status": result_text[0],
                        "detail": result_text[1],
                    }
                    result_text = None
                else:
                    result_text[1] += "\n" + line
    return {
        "results": results,
        "in_flight": in_flight,
        "partial_begin": partial_begin,
        "session_closed": "#endSession" in text
        and in_flight is None
        and not partial_begin,
        "session": session,
    }


def missing_qpa_session_fields(parsed):
    return [
        field
        for field in REQUIRED_QPA_SESSION_FIELDS
        if not parsed["session"].get(field)
    ]


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
            raise RunnerRefusal(
                f"{path}:{n} is not a " f"{len(LEDGER_HEADER)}-field row"
            )
        rows.append(
            {
                "class": f[0],
                "status": f[1],
                "pattern": f[2],
                "detail": f[3],
                "disposition": f[4],
                "authority": f[5],
                "witness": f[6],
            }
        )
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
    if run.get("dmesg", {}).get("continuity") == "broken":
        return "kernel_log_continuity_broken"
    if run["hazard_lines"]:
        return "dmesg_hazard"
    refusal = run.get("refusal")
    if refusal is not None:
        if refusal not in REFUSAL_VALUES:
            raise ValueError(f"unknown refusal {refusal!r}")
        return refusal
    code = run.get("exit_code")
    if code == "case_timeout" and run.get("session_closed"):
        code = "timeout"
    if code == "case_timeout":
        return "case_deadline"
    if code in ("timeout", "shutdown_timeout"):
        return "runner_deadline"
    observed = any(k not in RUNNER_STATUSES for k in run["counts"])
    if run.get("framework_abort"):
        return "framework_abort" if observed else "framework_precondition"
    if not run["session_closed"]:
        return "truncated_run"
    if isinstance(code, int) and code != 0 and not run["blocking"]:
        return "dirty_exit"
    if run["unexpected_cases"]:
        return "unexpected_cases"
    if run["classes"].get("unclassified", 0):
        return "unclassified_nonpass"
    if run["blocking"]:
        return "classified_nonpass"
    if run["counts"].get("Pass", 0) == 0:
        return "no_pass_observed"
    return (
        "pass_with_accepted_nonpass"
        if any(k not in PASS_STATUS for k in run["counts"])
        else "pass"
    )


def canonical(receipt):
    body = {k: v for k, v in receipt.items() if k != "seal_sha256"}
    return json.dumps(body, sort_keys=True, separators=(",", ":"))


def seal(receipt):
    return hashlib.sha256(canonical(receipt).encode()).hexdigest()


class QpaMarkerReader:
    """Incrementally counts the QPA markers that govern runner deadlines.

    The reader retains only the longest possible split marker and validates
    those trailing bytes before each read.  File replacement, truncation, or
    an in-place rewrite resets the counters instead of joining two different
    QPA streams.
    """

    MARKERS = (
        b"#beginTestCaseResult",
        b"#endTestCaseResult",
        b"#terminateTestCaseResult",
        b"#endSession",
    )
    OVERLAP_BYTES = max(map(len, MARKERS))

    def __init__(self, log):
        self.log = Path(log)
        self.identity = None
        self.offset = 0
        self.trailing_bytes = b""
        self.counts = {marker: 0 for marker in self.MARKERS}

    def reset(self, identity):
        self.identity = identity
        self.offset = 0
        self.trailing_bytes = b""
        self.counts = {marker: 0 for marker in self.MARKERS}

    def state(self):
        try:
            with self.log.open("rb") as qpa_file:
                stat_result = os.fstat(qpa_file.fileno())
                identity = (stat_result.st_dev, stat_result.st_ino)
                if identity != self.identity:
                    self.reset(identity)
                elif self.trailing_bytes:
                    trailing_offset = self.offset - len(self.trailing_bytes)
                    qpa_file.seek(trailing_offset)
                    if qpa_file.read(len(self.trailing_bytes)) != self.trailing_bytes:
                        self.reset(identity)
                qpa_file.seek(self.offset)
                new_bytes = qpa_file.read()
        except OSError:
            return self.marker_state()

        if new_bytes:
            previous_overlap = len(self.trailing_bytes)
            searchable = self.trailing_bytes + new_bytes
            searchable_offset = self.offset - previous_overlap
            for marker in self.MARKERS:
                marker_offset = searchable.find(marker)
                while marker_offset >= 0:
                    line_start = (marker_offset == 0 and searchable_offset == 0) or (
                        marker_offset > 0
                        and searchable[marker_offset - 1 : marker_offset]
                        in (b"\n", b"\r")
                    )
                    if line_start and marker_offset + len(marker) > previous_overlap:
                        self.counts[marker] += 1
                    marker_offset = searchable.find(marker, marker_offset + 1)
            self.offset += len(new_bytes)
            self.trailing_bytes = searchable[-self.OVERLAP_BYTES :]
        return self.marker_state()

    def marker_state(self):
        begun = self.counts[b"#beginTestCaseResult"]
        closed = (
            self.counts[b"#endTestCaseResult"]
            + self.counts[b"#terminateTestCaseResult"]
        )
        return begun, closed, self.counts[b"#endSession"] > 0


def reap_finished_children():
    """Collect every child that has already exited, and report how many.

    A per-case sequence starts one traced process and one preload probe for
    each case.  Each caller waits on the child it owns, yet entries still
    accumulate in the process table across a shard, and at twenty thousand
    cases they reach the per-user process limit: the next `fork` raises
    `BlockingIOError: [Errno 11] Resource temporarily unavailable` and the
    shard ends mid-sequence with no receipt, so the cases behind it are lost
    and the ones ahead of it were paid for.  Collecting between cases keeps
    the table flat.  The sweep runs where no child is in flight -- every
    caller has already waited on its own -- so it takes no result another
    caller needs."""
    collected = 0
    while True:
        try:
            pid, _status = os.waitpid(-1, os.WNOHANG)
        except (ChildProcessError, OSError):
            return collected
        if pid == 0:
            return collected
        collected += 1


def supervise(
    argv,
    cwd,
    env,
    log,
    stdout_path,
    stderr_path,
    total_timeout,
    case_timeout,
    shutdown_timeout,
):
    """Runs dEQP under two deadlines: the whole shard, and the case in
    flight, starting only when the QPA begin marker appears.  The QPA end
    marker closes that deadline, and #endSession starts the bounded process
    shutdown.  stdout and stderr go to files, so a chatty process never blocks
    on a pipe or extends a case deadline.  A deadline kills the process group;
    the exit code names which deadline."""
    with open(stdout_path, "wb") as out_f, open(stderr_path, "wb") as err_f:
        marker_reader = QpaMarkerReader(log)
        p = subprocess.Popen(
            argv, cwd=cwd, env=env, stdout=out_f, stderr=err_f, start_new_session=True
        )
        started = time.monotonic()
        case_started = None
        shutdown_started = None
        previous_begun = 0
        exit_code = None
        while True:
            rc = p.poll()
            if rc is not None:
                exit_code = rc
                break
            now = time.monotonic()
            begun, closed, session_closed = marker_reader.state()
            if begun > previous_begun:
                case_started = now
                previous_begun = begun
            if closed >= begun:
                case_started = None
            if session_closed and shutdown_started is None:
                shutdown_started = now
            if now - started > total_timeout:
                exit_code = "timeout"
            elif case_started is not None and now - case_started > case_timeout:
                exit_code = "case_timeout"
            elif (
                shutdown_started is not None
                and now - shutdown_started > shutdown_timeout
            ):
                exit_code = "shutdown_timeout"
            if exit_code is not None:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
                p.wait()
                break
            time.sleep(0.2)
    return (
        exit_code,
        Path(stdout_path).read_text(errors="replace"),
        Path(stderr_path).read_text(errors="replace"),
    )


def runtime_event_identity(path, out, host_boot_id):
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
    if not isinstance(body, dict):
        raise RunnerRefusal(f"runtime event {path} is not a JSON object")
    if not host_boot_id or body.get("boot_id") != host_boot_id:
        raise RunnerRefusal(
            f"runtime event {path} boot_id does not match the current boot"
        )
    (out / "runtime_event.json").write_bytes(text)
    return {
        "available": True,
        "sha256": hashlib.sha256(text).hexdigest(),
        "run_id": body.get("run_id"),
        "boot_id": body.get("boot_id"),
    }


QUEUE_CLAIM_MODES = (
    "default_graphics_only",
    "experimental_compute_subset",
    "conformant",
)


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
        return {"available": False, "mode": None, "compute_claim_eligible": False}
    path = Path(report_binary).resolve()
    rc, out = run_capture([str(path)], env=env)
    fields = {}
    for line in out.splitlines():
        k, sep, v = line.partition("\t")
        if sep:
            fields[k] = v
    mode = fields.get("queue_claim_mode")
    gate = fields.get("queue_claim_gate") == "1"
    verb_table_blake3 = fields.get("verb_table_blake3")
    if (
        rc != 0
        or mode not in QUEUE_CLAIM_MODES
        or fields.get("claim_consistent") != "1"
        or (mode == "experimental_compute_subset" and not gate)
        or not re.fullmatch(r"[0-9a-f]{64}", verb_table_blake3 or "")
    ):
        raise RunnerRefusal(
            "queue-claim report refused: "
            f"exit {rc}, mode {mode!r}, gate {gate}, "
            f"consistent {fields.get('claim_consistent')!r}, "
            f"verb ledger {verb_table_blake3!r}"
            f"{': ' + out[:200] if rc is None else ''}"
        )
    digest = sha256_file(path)
    if expected_sha256 and expected_sha256 != digest:
        raise RunnerRefusal(
            "queue-claim report binary "
            f"{digest[:12]} is not the declared "
            f"{expected_sha256[:12]}"
        )
    return {
        "available": True,
        "mode": mode,
        "report_sha256": digest,
        "compute_bit": fields.get("compute_bit") == "1",
        "gate_declared": gate,
        "queue_flags": fields.get("queue_flags"),
        "verb_table_blake3": verb_table_blake3,
        "compute_claim_eligible": mode == "conformant",
    }


def parse_caselist(text, source):
    cases = []
    seen = set()
    for line in text.splitlines():
        c = line.strip()
        if not c.startswith("dEQP-VK."):
            continue
        if c in seen:
            raise RunnerRefusal(
                f"{source} lists {c} twice; a duplicate case "
                "collapses in the results"
            )
        seen.add(c)
        cases.append(c)
    if not cases:
        raise RunnerRefusal(f"{source} lists no dEQP-VK cases")
    return cases


def read_caselist(path):
    return parse_caselist(Path(path).read_text(), path)


def snapshot_caselist(path, destination):
    """Read the caselist bytes once, derive cases and digest from that snapshot,
    and retain those exact bytes as the dEQP input and receipt artifact."""
    try:
        content = Path(path).read_bytes()
        text = content.decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise RunnerRefusal(f"caselist {path} is unreadable UTF-8: {error}")
    cases = parse_caselist(text, path)
    destination.write_bytes(content)
    return cases, hashlib.sha256(content).hexdigest()


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
        if len(name.encode()) > MAX_CASE_DIRECTORY_BYTES:
            suffix = hashlib.sha256(case.encode()).hexdigest()[:16]
            name = name[: MAX_CASE_DIRECTORY_BYTES - len(suffix) - 1] + "-" + suffix
        if name in owner:
            raise RunnerRefusal(
                f"{case} and {owner[name]} both sanitize to "
                f"{name}; a shard holds one directory per "
                "case"
            )
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
            raise RunnerRefusal(
                f"{path}:{n} is not a case name and 32 " "lowercase hex digits"
            )
        if case in nonces:
            raise RunnerRefusal(f"{path}:{n} names {case} twice")
        nonces[case] = nonce
    if len(set(nonces.values())) != len(nonces):
        raise RunnerRefusal(
            f"{path} reuses a nonce; one nonce binds one "
            "replay session, so two cases sharing one would "
            "let either plan bind the other's session"
        )
    missing = [c for c in cases if c not in nonces]
    if missing:
        raise RunnerRefusal(
            f"{path} declares no nonce for {len(missing)} "
            f"of the shard's cases, {missing[0]} first"
        )
    return nonces


def templated_env_names(declared):
    return [
        kv.partition("=")[0]
        for kv in declared or []
        if ENV_CASE_TOKEN.search(kv.partition("=")[2])
    ]


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
        resolved[k] = (
            v.replace("{case}", sanitized)
            .replace("{index}", f"{index:0{width}d}")
            .replace("{nonce}", nonce or "")
        )
    return resolved


def case_argv(args, caselist_file, log):
    return [
        str(Path(args.deqp_binary).resolve()),
        f"--deqp-caselist-file={caselist_file}",
        f"--deqp-log-filename={log}",
        "--deqp-watchdog=disable",
    ] + (args.deqp_arg or [])


def run_cases(
    args, out, cases, env, sanitized, nonces, kernel_probe, tracer=None, planning=False
):
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
    when every case emits its own #endSession and reaches a reaped
    process.  A missing per-case terminator leaves the shard truncated.

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
    missing_session_identity = False
    started = time.monotonic()
    shard_exit = 0
    argv = None
    stop = None
    for index, case in enumerate(cases, start=1):
        remaining = args.timeout - (time.monotonic() - started)
        if remaining <= 0:
            shard_exit = "timeout"
            stop = {
                "after_case": cases[index - 2] if index > 1 else None,
                "index": index - 1,
                "reason": "shard deadline expired before this case",
            }
            break
        case_dir = out / "cases" / sanitized[case]
        case_dir.mkdir(parents=True)
        caselist_file = case_dir / "caselist.txt"
        caselist_file.write_text(case + "\n")
        log = case_dir / "run.qpa"
        resolved = resolve_env_templates(
            args.env, sanitized[case], index, width, nonces.get(case)
        )
        case_env = dict(env)
        case_env.update(resolved)
        # A resolved absolute path is a plan the case reads or a
        # transcript it writes, so the receipt carries both whether it
        # existed before the case ran and whether it exists after.  A
        # case whose plan file is absent still runs: its device admits
        # no submission, the case fails, and the receipt names why.
        paths = {
            k: {"present_before": Path(v).exists()}
            for k, v in resolved.items()
            if v.startswith("/")
        }
        argv = case_argv(args, caselist_file, log)
        launch = argv
        if tracer:
            launch = (
                [tracer]
                + cs_trace.STRACE_ARGS
                + ["-o", str(case_dir / CASE_STRACE_NAME), "--"]
                + argv
            )
        planning_before = (
            planning_family(resolved.get(CAPTURE_FILE_NAME)) if planning else {}
        )
        exit_code, _, stderr_text = supervise(
            launch,
            Path(args.deqp_binary).resolve().parent,
            case_env,
            log,
            case_dir / "stdout.txt",
            case_dir / "stderr.txt",
            remaining,
            args.case_timeout,
            args.shutdown_timeout,
        )
        for k, state in paths.items():
            state["present_after"] = Path(resolved[k]).exists()
        parsed = (
            parse_qpa(log.read_text(errors="replace"))
            if log.is_file()
            else {
                "results": {},
                "in_flight": None,
                "session_closed": False,
                "session": {},
            }
        )
        if not session and parsed["session"]:
            session = parsed["session"]
        if parsed["session"].get("logFormatVersion") not in (None, QPA_LOG_FORMAT):
            unknown_format = True
        if missing_qpa_session_fields(parsed):
            missing_session_identity = True
        m = re.search(r"FATAL ERROR: [^\n]*", stderr_text)
        if framework_abort is None and m:
            framework_abort = m.group(0)
        result = parsed["results"].get(case, {"status": "not_run", "detail": ""})
        unexpected.update({c: r for c, r in parsed["results"].items() if c != case})
        # The single-case process leaves the case in flight exactly when
        # its process died mid-case, so the shard classifies it the way
        # the single-process runner classifies its in-flight case.
        if parsed["in_flight"] == case:
            if exit_code in ("timeout", "case_timeout"):
                result = {"status": "timeout", "detail": ""}
            elif isinstance(exit_code, int) and exit_code < 0:
                result = {
                    "status": "crash",
                    "detail": f"signal {signal.Signals(-exit_code).name}",
                }
            elif isinstance(exit_code, int) and exit_code != 0:
                result = {"status": "crash", "detail": f"exit {exit_code}"}
        results[case] = result
        record = {
            "index": index,
            "directory": sanitized[case],
            "exit_code": exit_code,
            "session_closed": parsed["session_closed"],
            "session": parsed["session"],
        }
        if resolved:
            record["environment"] = resolved
        if paths:
            record["paths"] = paths
        if planning:
            record["planning"] = planning_outcome(
                case_dir,
                resolved.get(CAPTURE_FILE_NAME),
                planning_before,
                stderr_text,
            )
        records[case] = record
        reaped = reap_finished_children()
        if reaped:
            record["reaped_children"] = reaped
        # A kernel hazard ends the sequence where it appeared: the cases
        # behind it would run on a wedged or reset GPU, so their results
        # would describe the hazard rather than themselves.
        kernel_state = kernel_probe()
        if kernel_state["continuity"] == "broken":
            stop = {
                "after_case": case,
                "index": index,
                "reason": "kernel log continuity broke after this case",
            }
            break
        if kernel_state["hazard_lines"]:
            stop = {
                "after_case": case,
                "index": index,
                "reason": "kernel hazard after this case",
                "hazard_lines": kernel_state["hazard_lines"],
            }
            break
        # The shard's own deadline firing inside a case ends the shard;
        # the case's deadline ends that case alone.
        if exit_code == "timeout":
            shard_exit = "timeout"
            stop = {
                "after_case": case,
                "index": index,
                "reason": "shard deadline expired during this case",
            }
            break
        if exit_code == "shutdown_timeout":
            shard_exit = "shutdown_timeout"
            stop = {
                "after_case": case,
                "index": index,
                "reason": "case process exceeded its shutdown deadline",
            }
            break
        if isinstance(exit_code, int) and exit_code != 0 and shard_exit == 0:
            shard_exit = exit_code
    return {
        "results": results,
        "cases": records,
        "session": session,
        "unexpected": unexpected,
        "session_closed": len(records) == len(cases)
        and all(record["session_closed"] for record in records.values()),
        "framework_abort": framework_abort,
        "unknown_format": unknown_format,
        "missing_session_identity": missing_session_identity,
        "exit_code": shard_exit,
        "argv": argv,
        "index_width": width,
        "stop": stop,
    }


def execute(args):
    out = Path(args.out)
    if out.exists() and any(out.iterdir()):
        raise RunnerRefusal(
            f"{out} is not empty; a run takes a fresh " "output directory"
        )
    out.mkdir(parents=True, exist_ok=True)
    caselist_file = out / "caselist.txt"
    cases, caselist_sha256 = snapshot_caselist(args.caselist, caselist_file)
    if len(cases) > args.max_cases:
        raise RunnerRefusal(
            f"{len(cases)} cases exceed the shard ceiling "
            f"{args.max_cases}; regenerate the partition at "
            "this ceiling"
        )
    # A `{case}` value names a per-case file, which one process apiece
    # is what gives it; a token declared for a single-process shard
    # would reach dEQP verbatim, so it refuses here.
    templated = templated_env_names(args.env)
    if templated and not args.process_per_case:
        raise RunnerRefusal(
            f"--env {templated} carries a per-case token " "outside --process-per-case"
        )
    sanitized = sanitize_case_names(cases) if args.process_per_case else {}
    wants_nonce = any("{nonce}" in kv.partition("=")[2] for kv in args.env or [])
    if wants_nonce and not args.plan_nonce_file:
        raise RunnerRefusal(
            "--env declares {nonce} with no " "--plan-nonce-file to resolve it"
        )
    nonces = (
        load_plan_nonces(args.plan_nonce_file, cases) if args.plan_nonce_file else {}
    )
    partition, part_refusal = partition_identity(
        args.partition_manifest, caselist_file, args.ad_hoc_hazard
    )
    env, contaminating = build_environment(
        args.env, partition.get("hazard"), out / "isolated-home"
    )
    host = host_identity()
    runtime_event = runtime_event_identity(args.runtime_event, out, host["boot_id"])
    icd = icd_identity(args.icd)
    # The report probes the ICD the run pins, so the pin enters the
    # environment ahead of it.
    env["VK_DRIVER_FILES"] = icd["manifest"]
    env.pop("VK_ICD_FILENAMES", None)
    cursor = journal_cursor(args.journal_command)
    dmesg_before = None if cursor else read_dmesg(args.dmesg_command)
    preload = preload_identity(env, Path(args.deqp_binary).resolve().parent)
    queue_claim = queue_claim_identity(
        args.queue_report, env, args.expect_report_sha256
    )
    evidence, node = evidence_class(env, host, icd, preload)
    deqp = deqp_identity(args.deqp_binary)
    # A declared capture file on a submission-hazard slice under the
    # drm-shim host model is a planning candidate: it records the
    # ordered submissions a later silicon replay binds to and states
    # nothing about conformance, so it runs below the slice's required
    # evidence and never becomes valid for qualification.  The candidate earns the
    # host-planning disposition when every named condition holds and the
    # per-process strace witnesses zero kernel-entering CS ioctls; a
    # failed condition refuses by name.  A capture session opens the CS
    # ioctl with the hazard gate closed, which the host model alone
    # answers, so the same declaration on silicon refuses.  On a
    # hazard-free slice the declaration stays contamination, refused by
    # name in build_environment.
    capture_declared = bool(env.get(CAPTURE_FILE_NAME))
    planning_candidate = (
        capture_declared
        and evidence == "host-model"
        and partition.get("hazard") == "submission"
    )
    conditions = planning_conditions(
        env, evidence, partition.get("hazard"), args.process_per_case
    )
    planning = {
        "candidate": planning_candidate,
        "disposition": None,
        "qualification_valid": False,
        "conditions": conditions,
        "refused_conditions": [n for n, ok in conditions.items() if not ok],
    }
    tracer = None
    if planning_candidate:
        tracer, tracer_error = planning_tracer(args.strace_binary)
        planning["tracer"] = {
            "binary": tracer,
            "error": tracer_error,
            "strace_args": cs_trace.STRACE_ARGS,
            "witness_scope": "syscall_boundary",
        }
    receipt = {
        "receipt_version": RECEIPT_VERSION,
        "source": source_identity(args.source_root),
        "build": build_identity(args.build_root),
        "icd": icd,
        "deqp": deqp,
        "host": host,
        "preload": preload,
        "environment": environment_identity(env, args.env),
        "evidence_class": evidence,
        "render_node": node,
        "case_count": len(cases),
        # The digest covers the caselist file's bytes, the form the
        # partition manifest publishes per shard and sha256sum prints, so
        # one declared value binds the receipt, the manifest, and the
        # file.
        "caselist_sha256": caselist_sha256,
        "partition": partition,
        "runtime_event": runtime_event,
        "queue_claim": queue_claim,
        "compute_claim_eligible": queue_claim["compute_claim_eligible"],
        "expected": {
            "source_sha": args.expect_source_sha,
            "dso_sha256": args.expect_dso_sha256,
            "deqp_sha256": args.expect_deqp_sha256,
            "caselist_sha256": args.expect_caselist_sha256,
            "partition_sha256": args.expect_partition_sha256,
            "runtime_event_sha256": args.expect_runtime_event_sha256,
            "queue_claim_mode": args.expect_queue_claim_mode,
            "queue_report_sha256": args.expect_report_sha256,
        },
        "timeouts": {
            "total_seconds": args.timeout,
            "case_seconds": args.case_timeout,
            "shutdown_seconds": args.shutdown_timeout,
        },
        "max_cases": args.max_cases,
        "process_per_case": bool(args.process_per_case),
        "planning": planning,
    }
    if args.process_per_case:
        receipt["case_directories"] = sanitized
        receipt["templated_env"] = templated
        receipt["plan_nonce_file"] = args.plan_nonce_file
    refusal = part_refusal
    if preload["declared"] and not preload["shim_loaded"]:
        refusal = refusal or "preload_unverified"
    pinned_cts = partition.get("cts_describe")
    if pinned_cts and cts_revision(deqp) != pinned_cts:
        refusal = refusal or "wrong_cts_revision"
    if refusal is None and contaminating:
        refusal = "gate_contamination"
        receipt["contaminating_gates"] = contaminating
    identity_refusals = identity_mismatch_refusals(receipt)
    if identity_refusals:
        refusal = refusal or identity_refusals[0]
    if refusal is None and capture_declared and evidence == "silicon":
        refusal = "capture_on_silicon"
    if refusal is None and planning_candidate and planning["refused_conditions"]:
        refusal = "planning_disposition_refused"
    if refusal is None and planning_candidate and tracer is None:
        refusal = "planning_witness_unavailable"
    if (
        refusal is None
        and partition.get("required_evidence") == "silicon"
        and evidence != "silicon"
        and not planning_candidate
    ):
        refusal = "evidence_below_required"
    if refusal is None and partition.get("shard_max_cases") not in (
        None,
        args.max_cases,
    ):
        refusal = "shard_ceiling_mismatch"
    ledger = load_nonpass_ledger(args.nonpass_ledger)
    results = {c: {"status": "not_run", "detail": ""} for c in cases}
    unexpected = {}
    exit_code = None
    parsed = {
        "results": {},
        "in_flight": None,
        "partial_begin": False,
        "session_closed": False,
        "session": {},
    }

    def kernel_probe():
        """The continuity and hazard lines since the shard's own baseline."""
        if cursor:
            delta = journal_after(args.journal_command, cursor)
            continuity = "continuous" if delta is not None else "unavailable"
        else:
            delta, continuity = dmesg_delta(
                dmesg_before, read_dmesg(args.dmesg_command)
            )
        return {"continuity": continuity, "hazard_lines": hazard_lines(delta)}

    log = out / "run.qpa"
    stderr_text = ""
    artifact_names = list(ARTIFACT_NAMES)
    if refusal is None:
        env["VK_DRIVER_FILES"] = receipt["icd"]["manifest"]
        env.pop("VK_ICD_FILENAMES", None)
        receipt["environment"] = environment_identity(env, args.env)
        started = time.monotonic()
    if refusal is None and args.process_per_case:
        shard = run_cases(
            args,
            out,
            cases,
            env,
            sanitized,
            nonces,
            kernel_probe,
            tracer=tracer,
            planning=planning_candidate and refusal is None,
        )
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
        parsed = {
            "results": shard["results"],
            "in_flight": None,
            "partial_begin": False,
            "session_closed": shard["session_closed"],
            "session": shard["session"],
        }
        if shard["unknown_format"]:
            refusal = refusal or "unknown_log_format"
        if shard["missing_session_identity"]:
            refusal = refusal or "missing_session_identity"
        receipt["unknown_log_format"] = shard["unknown_format"]
        artifact_names = list(SHARD_ARTIFACT_NAMES)
        case_names = list(CASE_ARTIFACT_NAMES)
        if tracer:
            case_names.append(CASE_STRACE_NAME)
        artifact_names += [
            f"cases/{d}/{n}"
            for d in (
                shard["cases"][c]["directory"] for c in cases if c in shard["cases"]
            )
            for n in case_names
        ]
        # The disposition closes on the run's own witness: every case
        # traced, every trace line parsed, and zero kernel-entering CS
        # ioctls over the shard; anything else refuses and the evidence
        # class stays host-model.
        if planning_candidate and refusal is None:
            per = {c: rec["planning"] for c, rec in shard["cases"].items()}
            unwitnessed = sorted(c for c, o in per.items() if not o["witnessed"])
            cs_total = sum(o["cs_ioctls"] for o in per.values())
            unparsed = sum(o["unparsed_ioctl_lines"] for o in per.values())
            planning["cs_witness"] = {
                "witness_scope": "syscall_boundary",
                "cs_ioctls": cs_total,
                "total_ioctls": sum(o["total_ioctls"] for o in per.values()),
                "unparsed_ioctl_lines": unparsed,
                "unwitnessed_cases": unwitnessed,
            }
            planning["outcomes"] = {
                o: sum(1 for r in per.values() if r["outcome"] == o)
                for o in PLANNING_OUTCOMES
            }
            planning["transcripts"] = {
                c: r["transcripts"] for c, r in per.items() if r["transcripts"]
            }
            zero = bool(per) and not unwitnessed and unparsed == 0 and cs_total == 0
            complete_cases = set(per) == set(cases)
            complete_outcomes = complete_cases and all(
                shard["cases"][case]["session_closed"]
                and outcome["outcome"] != "unresolved"
                and not outcome["terminal_errors"]
                for case, outcome in per.items()
            )
            conditions["every_shard_case_witnessed"] = complete_cases
            conditions["planning_outcomes_complete"] = complete_outcomes
            conditions["kernel_entering_cs_zero"] = zero and complete_cases
            planning["refused_conditions"] = [
                name
                for name, condition_holds in conditions.items()
                if not condition_holds
            ]
            if zero:
                if complete_cases and complete_outcomes:
                    planning["disposition"] = PLANNING_EVIDENCE
                    evidence = PLANNING_EVIDENCE
                    receipt["evidence_class"] = evidence
                else:
                    refusal = "planning_capture_incomplete"
            else:
                refusal = (
                    "planning_cs_witnessed" if cs_total else "planning_unwitnessed"
                )
    elif refusal is None:
        argv = case_argv(args, caselist_file, log)
        receipt["argv"] = argv
        exit_code, _stdout_text, stderr_text = supervise(
            argv,
            Path(args.deqp_binary).resolve().parent,
            env,
            log,
            out / "stdout.txt",
            out / "stderr.txt",
            args.timeout,
            args.case_timeout,
            args.shutdown_timeout,
        )
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
                results[in_flight][
                    "detail"
                ] = f"signal {signal.Signals(-exit_code).name}"
            elif isinstance(exit_code, int) and exit_code != 0:
                results[in_flight]["status"] = "crash"
                results[in_flight]["detail"] = f"exit {exit_code}"
        receipt["unknown_log_format"] = parsed["session"].get(
            "logFormatVersion"
        ) not in (None, QPA_LOG_FORMAT)
        if missing_qpa_session_fields(parsed):
            refusal = refusal or "missing_session_identity"
    else:
        receipt["unknown_log_format"] = False
    if cursor:
        delta = journal_after(args.journal_command, cursor)
        continuity = "continuous" if delta is not None else "unavailable"
        log_source = "journal"
    else:
        delta, continuity = dmesg_delta(dmesg_before, read_dmesg(args.dmesg_command))
        log_source = "dmesg"
    if delta:
        (out / "dmesg_delta.txt").write_text("\n".join(delta) + "\n")
    session = parsed["session"]
    if session.get("logFormatVersion") not in (None, QPA_LOG_FORMAT):
        refusal = refusal or "unknown_log_format"
        receipt["unknown_log_format"] = True
    m = re.search(r"FATAL ERROR: [^\n]*", stderr_text)
    receipt["framework_abort"] = m.group(0) if m else None
    receipt["exit_code"] = exit_code
    receipt["session"] = session
    receipt["session_closed"] = parsed["session_closed"]
    receipt["partial_begin"] = parsed["partial_begin"]
    receipt["refusal"] = refusal
    receipt["dmesg"] = {
        "available": delta is not None,
        "source": log_source,
        "continuity": continuity,
        "delta_lines": len(delta) if delta else 0,
    }
    qualification_valid, qualification_reason = expected_qualification_state(receipt)
    receipt["qualification_valid"] = qualification_valid
    receipt["qualification_reason"] = qualification_reason
    receipt["hazard_lines"] = hazard_lines(delta)
    counts, classes, blocking = summarize(results, ledger)
    receipt["counts"] = counts
    receipt["classes"] = classes
    receipt["blocking"] = blocking
    receipt["results"] = results
    receipt["unexpected_cases"] = unexpected
    receipt["result_count"] = sum(counts.values())
    receipt["verdict"] = verdict_for(receipt)
    receipt["artifacts"] = {
        n: sha256_file(out / n) for n in artifact_names if (out / n).is_file()
    }
    receipt["seal_sha256"] = seal(receipt)
    (out / "receipt.json").write_text(
        json.dumps(receipt, indent=1, sort_keys=True) + "\n"
    )
    print(
        f"verdict={receipt['verdict']} evidence={evidence} "
        f"queue_claim={queue_claim['mode']} "
        f"qualification_valid={qualification_valid} counts={counts} "
        f"classes={classes} "
        f"seal={receipt['seal_sha256'][:12]}"
    )
    return receipt


def receipt_mapping(receipt, field):
    value = receipt.get(field)
    if not isinstance(value, dict):
        raise RunnerRefusal(f"receipt has invalid {field}")
    return value


IDENTITY_REFUSAL_LABELS = {
    "wrong_queue_claim": "queue claim",
    "wrong_queue_report": "queue-claim report binary",
    "wrong_source": "source",
    "wrong_icd": "ICD shared object",
    "wrong_deqp": "dEQP binary",
    "wrong_caselist": "caselist",
    "wrong_partition": "partition manifest",
    "wrong_runtime_event": "runtime event",
    "wrong_runtime_boot": "runtime-event boot",
}
REFUSAL_VALUES = frozenset(
    {
        "blocked_slice",
        "gate_contamination",
        "capture_on_silicon",
        "planning_disposition_refused",
        "planning_witness_unavailable",
        "evidence_below_required",
        "shard_ceiling_mismatch",
        "unknown_log_format",
        "planning_cs_witnessed",
        "planning_unwitnessed",
        "planning_capture_incomplete",
        "preload_unverified",
        "wrong_cts_revision",
        "missing_session_identity",
        *IDENTITY_REFUSAL_LABELS,
    }
)


def identity_mismatch_refusals(receipt):
    expected = receipt_mapping(receipt, "expected")
    source = receipt_mapping(receipt, "source")
    icd = receipt_mapping(receipt, "icd")
    deqp = receipt_mapping(receipt, "deqp")
    partition = receipt_mapping(receipt, "partition")
    runtime_event = receipt_mapping(receipt, "runtime_event")
    queue_claim = receipt_mapping(receipt, "queue_claim")
    host = receipt_mapping(receipt, "host")
    comparisons = (
        (
            "wrong_queue_claim",
            expected.get("queue_claim_mode"),
            queue_claim.get("mode"),
        ),
        (
            "wrong_queue_report",
            expected.get("queue_report_sha256"),
            queue_claim.get("report_sha256"),
        ),
        (
            "wrong_source",
            expected.get("source_sha"),
            source.get("sha") if source.get("available") else None,
        ),
        ("wrong_icd", expected.get("dso_sha256"), icd.get("dso_sha256")),
        ("wrong_deqp", expected.get("deqp_sha256"), deqp.get("sha256")),
        (
            "wrong_caselist",
            expected.get("caselist_sha256"),
            receipt.get("caselist_sha256"),
        ),
        (
            "wrong_partition",
            expected.get("partition_sha256"),
            partition.get("manifest_sha256"),
        ),
        (
            "wrong_runtime_event",
            expected.get("runtime_event_sha256"),
            runtime_event.get("sha256"),
        ),
        (
            "wrong_runtime_boot",
            host.get("boot_id") if runtime_event.get("available") else None,
            runtime_event.get("boot_id") if runtime_event.get("available") else None,
        ),
    )
    return [
        refusal
        for refusal, declared_identity, observed_identity in comparisons
        if declared_identity and declared_identity != observed_identity
    ]


def receipt_planning_state(receipt):
    environment = receipt_mapping(receipt, "environment")
    partition = receipt_mapping(receipt, "partition")
    planning = receipt_mapping(receipt, "planning")
    evidence = receipt.get("evidence_class")
    candidate_evidence = "host-model" if evidence == PLANNING_EVIDENCE else evidence
    hazard = partition.get("hazard")
    capture_declared = bool(environment.get(CAPTURE_FILE_NAME))
    planning_candidate = (
        capture_declared
        and candidate_evidence == "host-model"
        and hazard == "submission"
    )
    if planning.get("candidate") is not planning_candidate:
        raise RunnerRefusal("planning candidate does not match the recorded run")

    expected_conditions = planning_conditions(
        environment,
        candidate_evidence,
        hazard,
        receipt.get("process_per_case"),
    )
    recorded_conditions = receipt_mapping(planning, "conditions")
    cs_witness = planning.get("cs_witness")
    if cs_witness is not None:
        if not isinstance(cs_witness, dict):
            raise RunnerRefusal("planning cs_witness is not a mapping")
        unwitnessed_cases = cs_witness.get("unwitnessed_cases")
        cs_ioctls = cs_witness.get("cs_ioctls")
        unparsed_lines = cs_witness.get("unparsed_ioctl_lines")
        if (
            not isinstance(unwitnessed_cases, list)
            or not isinstance(cs_ioctls, int)
            or isinstance(cs_ioctls, bool)
            or cs_ioctls < 0
            or not isinstance(unparsed_lines, int)
            or isinstance(unparsed_lines, bool)
            or unparsed_lines < 0
        ):
            raise RunnerRefusal("planning cs_witness has invalid counts")
        case_records = receipt_mapping(receipt, "cases")
        results = receipt_mapping(receipt, "results")
        planning_records = {
            case: record.get("planning")
            for case, record in case_records.items()
            if isinstance(record, dict) and isinstance(record.get("planning"), dict)
        }
        complete_cases = set(planning_records) == set(results)
        complete_outcomes = complete_cases and all(
            case_records[case].get("session_closed") is True
            and record.get("outcome") in ("transcript", "no_nonempty_ib")
            and not record.get("terminal_errors", [])
            for case, record in planning_records.items()
        )
        expected_conditions["every_shard_case_witnessed"] = complete_cases
        expected_conditions["planning_outcomes_complete"] = complete_outcomes
        expected_conditions["kernel_entering_cs_zero"] = (
            bool(case_records)
            and complete_cases
            and not unwitnessed_cases
            and unparsed_lines == 0
            and cs_ioctls == 0
        )
    if recorded_conditions != expected_conditions:
        raise RunnerRefusal("planning conditions do not match the recorded run")

    expected_refused_conditions = [
        name
        for name, condition_holds in expected_conditions.items()
        if not condition_holds
    ]
    if planning.get("refused_conditions") != expected_refused_conditions:
        raise RunnerRefusal("planning refused_conditions do not match the conditions")
    return planning_candidate, expected_conditions, cs_witness


def expected_receipt_refusal(receipt):
    environment = receipt_mapping(receipt, "environment")
    partition = receipt_mapping(receipt, "partition")
    planning = receipt_mapping(receipt, "planning")
    planning_candidate, planning_state, cs_witness = receipt_planning_state(receipt)
    hazard = partition.get("hazard")
    preload = receipt_mapping(receipt, "preload")
    deqp = receipt_mapping(receipt, "deqp")

    if hazard == "unknown":
        return "blocked_slice"
    if preload.get("declared") and not preload.get("shim_loaded"):
        return "preload_unverified"
    if partition.get("cts_describe") and cts_revision(deqp) != partition.get(
        "cts_describe"
    ):
        return "wrong_cts_revision"

    contaminating_gates = sorted(
        name
        for name in environment
        if name.startswith(SUBMISSION_GATE_PREFIXES)
        or SUBMISSION_GATE_PATTERN.match(name)
    )
    if hazard == "submission":
        contaminating_gates = []
    if receipt.get("contaminating_gates", []) != contaminating_gates:
        raise RunnerRefusal("contaminating gates do not match the environment")
    if contaminating_gates:
        return "gate_contamination"

    identity_refusals = identity_mismatch_refusals(receipt)
    if identity_refusals:
        return identity_refusals[0]

    capture_declared = bool(environment.get(CAPTURE_FILE_NAME))
    evidence = receipt.get("evidence_class")
    if capture_declared and evidence == "silicon":
        return "capture_on_silicon"

    preflight_refused = [
        name
        for name, condition_holds in planning_state.items()
        if name
        not in (
            "every_shard_case_witnessed",
            "planning_outcomes_complete",
            "kernel_entering_cs_zero",
        )
        and not condition_holds
    ]
    if planning_candidate and preflight_refused:
        return "planning_disposition_refused"
    if planning_candidate:
        tracer = planning.get("tracer")
        if not isinstance(tracer, dict) or tracer.get("binary") is None:
            return "planning_witness_unavailable"

    if (
        partition.get("required_evidence") == "silicon"
        and evidence != "silicon"
        and not planning_candidate
    ):
        return "evidence_below_required"
    if partition.get("shard_max_cases") not in (None, receipt.get("max_cases")):
        return "shard_ceiling_mismatch"

    unknown_log_format = receipt.get("unknown_log_format")
    if not isinstance(unknown_log_format, bool):
        session = receipt_mapping(receipt, "session")
        unknown_log_format = session.get("logFormatVersion") not in (
            None,
            QPA_LOG_FORMAT,
        )
    if unknown_log_format:
        return "unknown_log_format"
    session = receipt_mapping(receipt, "session")
    case_records = receipt.get("cases", {})
    if not isinstance(case_records, dict):
        raise RunnerRefusal("receipt has invalid cases")
    case_sessions = (
        []
        if receipt.get("receipt_version") == LEGACY_RECEIPT_VERSION
        else [
            record.get("session", {})
            for record in case_records.values()
            if isinstance(record, dict)
        ]
    )
    if any(not session.get(field) for field in REQUIRED_QPA_SESSION_FIELDS) or any(
        not case_session.get(field)
        for case_session in case_sessions
        for field in REQUIRED_QPA_SESSION_FIELDS
    ):
        return "missing_session_identity"

    if planning_candidate and not planning_state.get("every_shard_case_witnessed"):
        return "planning_capture_incomplete"
    if planning_candidate and planning_state.get("kernel_entering_cs_zero") is False:
        assert cs_witness is not None
        return (
            "planning_cs_witnessed"
            if cs_witness["cs_ioctls"]
            else "planning_unwitnessed"
        )
    if planning_candidate and not planning_state.get("planning_outcomes_complete"):
        return "planning_capture_incomplete"
    return None


def expected_qualification_state(receipt):
    source = receipt_mapping(receipt, "source")
    planning = receipt_mapping(receipt, "planning")
    expected = receipt_mapping(receipt, "expected")
    partition = receipt_mapping(receipt, "partition")
    queue_claim = receipt_mapping(receipt, "queue_claim")
    dmesg = receipt_mapping(receipt, "dmesg")
    identity_refusals = identity_mismatch_refusals(receipt)

    if planning.get("candidate"):
        qualification_valid = False
        qualification_reason = PLANNING_QUALIFICATION_REASON
    elif not source.get("available"):
        qualification_valid = False
        qualification_reason = "source identity unavailable"
    elif not source.get("clean"):
        qualification_valid = False
        qualification_reason = "source tree dirty"
    elif identity_refusals:
        qualification_valid = False
        qualification_reason = (
            "recorded identity does not match declared "
            + IDENTITY_REFUSAL_LABELS[identity_refusals[0]]
        )
    else:
        undeclared = [
            name
            for name, value in (
                ("source SHA", expected.get("source_sha")),
                ("DSO SHA-256", expected.get("dso_sha256")),
                ("dEQP SHA-256", expected.get("deqp_sha256")),
                ("caselist SHA-256", expected.get("caselist_sha256")),
                ("partition SHA-256", expected.get("partition_sha256")),
            )
            if not value
        ]
        if receipt.get("evidence_class") == "silicon" and not expected.get(
            "runtime_event_sha256"
        ):
            undeclared.append("runtime-event SHA-256")
        if not queue_claim.get("available"):
            undeclared.append("queue-claim report")
        elif not expected.get("queue_report_sha256"):
            undeclared.append("queue-claim report SHA-256")
        if undeclared:
            qualification_valid = False
            qualification_reason = "undeclared " + ", ".join(undeclared)
        elif receipt.get("evidence_class") == "host-unknown":
            qualification_valid = False
            qualification_reason = "evidence class unknown"
        elif partition.get("kind") == "ad-hoc":
            qualification_valid = False
            qualification_reason = "caselist bound to no partition slice"
        else:
            qualification_valid = True
            qualification_reason = None

    if receipt.get("evidence_class") == "silicon" and not dmesg.get("available"):
        qualification_valid = False
        qualification_reason = f"kernel log {dmesg.get('continuity')} on a silicon run"
    return qualification_valid, qualification_reason


def verify_receipt_semantics(receipt):
    host = receipt_mapping(receipt, "host")
    icd = receipt_mapping(receipt, "icd")
    preload = receipt_mapping(receipt, "preload")
    derived_evidence, derived_node = evidence_class(
        receipt_mapping(receipt, "environment"), host, icd, preload
    )
    planning = receipt_mapping(receipt, "planning")
    expected_evidence = (
        PLANNING_EVIDENCE
        if planning.get("disposition") == PLANNING_EVIDENCE
        else derived_evidence
    )
    if receipt.get("evidence_class") != expected_evidence:
        raise RunnerRefusal("evidence class does not match the recorded identities")
    if expected_evidence == "silicon" and receipt.get("render_node") != derived_node:
        raise RunnerRefusal("render node does not match the recorded silicon identity")
    qualification_valid, qualification_reason = expected_qualification_state(receipt)
    if (
        receipt.get("qualification_valid") != qualification_valid
        or receipt.get("qualification_reason") != qualification_reason
    ):
        raise RunnerRefusal(
            "qualification fields do not match the recorded run conditions"
        )

    recorded_refusal = receipt.get("refusal")
    if recorded_refusal is not None and recorded_refusal not in REFUSAL_VALUES:
        raise RunnerRefusal(f"receipt has unknown refusal {recorded_refusal!r}")
    expected_refusal = expected_receipt_refusal(receipt)
    if recorded_refusal == expected_refusal:
        pass
    elif expected_refusal in IDENTITY_REFUSAL_LABELS and recorded_refusal is None:
        raise RunnerRefusal("identity mismatch is not reflected in the receipt refusal")
    elif recorded_refusal in IDENTITY_REFUSAL_LABELS:
        raise RunnerRefusal("identity refusal does not match the recorded expectations")
    else:
        raise RunnerRefusal(
            "refusal does not match the recorded run conditions: "
            f"expected {expected_refusal!r}"
        )

    if planning.get("qualification_valid") is not False:
        raise RunnerRefusal("planning qualification_valid must remain false")

    queue_claim = receipt_mapping(receipt, "queue_claim")
    queue_eligibility = queue_claim.get("compute_claim_eligible")
    if not isinstance(queue_eligibility, bool):
        raise RunnerRefusal("queue claim lacks boolean compute_claim_eligible")
    if receipt.get("compute_claim_eligible") != queue_eligibility:
        raise RunnerRefusal("compute claim eligibility does not reconcile")
    if queue_eligibility != (queue_claim.get("mode") == "conformant"):
        raise RunnerRefusal("queue claim mode and eligibility contradict each other")

    results = receipt_mapping(receipt, "results")
    counts = receipt_mapping(receipt, "counts")
    classes = receipt_mapping(receipt, "classes")
    computed_counts = {}
    computed_classes = {}
    computed_blocking = 0
    for case_name, result in results.items():
        if not isinstance(case_name, str) or not isinstance(result, dict):
            raise RunnerRefusal("receipt has an invalid result entry")
        status = result.get("status")
        if not isinstance(status, str):
            raise RunnerRefusal(f"result {case_name} lacks a status")
        computed_counts[status] = computed_counts.get(status, 0) + 1
        if status not in PASS_STATUS:
            result_class = result.get("class")
            disposition = result.get("disposition")
            if not isinstance(result_class, str) or not isinstance(disposition, str):
                raise RunnerRefusal(f"non-pass result {case_name} lacks classification")
            if disposition not in {"accepted", "blocks"}:
                raise RunnerRefusal(
                    f"non-pass result {case_name} disposition must be accepted or blocks"
                )
            computed_classes[result_class] = computed_classes.get(result_class, 0) + 1
            if disposition == "blocks":
                computed_blocking += 1
    if counts != computed_counts:
        raise RunnerRefusal("result statuses do not reconcile with counts")
    if classes != computed_classes:
        raise RunnerRefusal("result classifications do not reconcile")
    if receipt.get("blocking") != computed_blocking:
        raise RunnerRefusal("blocking result count does not reconcile")
    if receipt.get("result_count") != len(results):
        raise RunnerRefusal("result count does not reconcile")
    if receipt.get("case_count") != len(results):
        raise RunnerRefusal("case count does not reconcile with results")

    try:
        expected_verdict = verdict_for(receipt)
    except (KeyError, TypeError, ValueError) as error:
        raise RunnerRefusal(f"receipt cannot produce a verdict: {error}") from error
    if receipt.get("verdict") != expected_verdict:
        raise RunnerRefusal(
            f"verdict does not match the recorded results: expected {expected_verdict}"
        )


def verify_receipt(path):
    receipt = load_json(path, "receipt")
    receipt_version = receipt.get("receipt_version")
    if receipt_version not in (LEGACY_RECEIPT_VERSION, RECEIPT_VERSION):
        raise RunnerRefusal("receipt version unknown")
    # Version 3 predates semantic recomputation.  Its verifier sealed the
    # receipt body, re-digested every artifact, and reconciled result counts;
    # applying version-4 refusal precedence or qualification rules changes the
    # meaning of an archived receipt after publication.
    if receipt_version == RECEIPT_VERSION:
        if not isinstance(receipt.get("qualification_valid"), bool):
            raise RunnerRefusal(
                f"receipt version {receipt_version} lacks boolean "
                "qualification_valid"
            )
        reason = receipt.get("qualification_reason")
        if reason is not None and not isinstance(reason, str):
            raise RunnerRefusal(
                f"receipt version {receipt_version} has invalid " "qualification_reason"
            )
        verify_receipt_semantics(receipt)
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
    if receipt.get("result_count") != sum(receipt["counts"].values()) or receipt.get(
        "result_count"
    ) != len(receipt["results"]):
        raise RunnerRefusal("result count does not reconcile")
    print(
        f"receipt verified: seal {recorded[:12]}, "
        f"{len(receipt.get('artifacts', {}))} artifacts, verdict "
        f"{receipt['verdict']}"
    )


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
            raise RunnerRefusal(
                f"{r['class']}: status {r['status']} is not " "a dEQP status"
            )
        if r["disposition"] not in ("accepted", "blocks"):
            raise RunnerRefusal(
                f"{r['class']}: disposition must be accepted " "or blocks"
            )
        named.add(r["status"])
        case, _, detail = r["witness"].partition("|")
        got, _ = classify(case, r["status"], detail, ledger)
        if got != r["class"]:
            raise RunnerRefusal(
                f"{r['class']}: its witness {r['witness']!r} "
                f"classifies as {got}, so the row is "
                "unreachable or shadowed"
            )
    missing = sorted(DEQP_STATUSES - PASS_STATUS - named)
    if missing:
        raise RunnerRefusal(f"no ledger row names status {missing}")
    root = Path(__file__).resolve().parents[5]
    gates = []
    for relative in (COMPUTE_VERB_SOURCE, COMPUTE_ROUTE_SOURCE):
        source = root / relative
        if not source.is_file():
            raise RunnerRefusal(
                f"{relative} is not beside this "
                "runner; the gate pattern cannot be held to it"
            )
        gates += re.findall(
            r'"(R3V_NATIVE_COMPUTE_[A-Z0-9_]+)"', source.read_text()
        )
    unbound = sorted(
        g
        for g in set(gates)
        if g != "R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL"
        and not SUBMISSION_GATE_PATTERN.match(g)
    )
    if not gates:
        raise RunnerRefusal(
            f"{COMPUTE_VERB_SOURCE} and {COMPUTE_ROUTE_SOURCE} name no "
            "compute gate"
        )
    if unbound:
        raise RunnerRefusal(
            f"compute verb gates outside the contamination " f"pattern: {unbound}"
        )
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
            raise RunnerRefusal(
                f"{slices_path}:{n}: a hazardous slice " "requires silicon evidence"
            )
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
            cases.update(
                case_line.strip()
                for case_line in f.read_text().splitlines()
                if case_line.startswith("dEQP-VK.")
            )
        for g in groups:
            if not any(c == g or c.startswith(g + ".") for c in cases):
                raise RunnerRefusal(f"slice group {g} matches no mustpass case")
        for r in ledger:
            case = r["witness"].partition("|")[0]
            if case not in cases:
                raise RunnerRefusal(
                    f"{r['class']}: witness {case} is not a " "mustpass case"
                )
        corpus = f"{len(cases)} mustpass cases"
    print(
        f"ledgers hold: {len(ledger)} non-pass rows with reachable "
        f"witnesses, {len(set(gates))} compute gates inside the "
        f"contamination pattern, {len(orders)} slices, {len(groups)} "
        "groups, "
        f"{corpus or 'mustpass clause not run (no corpus named)'}"
    )


FAKE_DEQP = r"""#!/usr/bin/env python3
import os, sys, time, signal
mode = os.environ["FAKE_DEQP_MODE"]
log = [a.split("=",1)[1] for a in sys.argv if a.startswith("--deqp-log-filename=")][0]
cl = [a.split("=",1)[1] for a in sys.argv if a.startswith("--deqp-caselist-file=")][0]
cases = [l.strip() for l in open(cl) if l.strip()]
f = open(log, "w")
if not os.environ.get("FAKE_OMIT_RELEASE") and os.environ.get("FAKE_OMIT_RELEASE_CASE") not in cases:
    f.write('#sessionInfo releaseName fake-1\n')
if not os.environ.get("FAKE_OMIT_LOG_FORMAT") and os.environ.get("FAKE_OMIT_LOG_FORMAT_CASE") not in cases:
    f.write('#sessionInfo logFormatVersion "0.3.4"\n')
f.write('#beginSession\n'); f.flush()
def case(name, status, detail="x"):
    f.write(f"#beginTestCaseResult {name}\n<TestCaseResult CasePath=\"{name}\" Version=\"0.3.4\" CaseType=\"SelfValidate\">\n <Text>note</Text>\n <Result StatusCode=\"{status}\">{detail}</Result>\n</TestCaseResult>\n\n#endTestCaseResult\n"); f.flush()
if mode == "mixed":
    sts = ["Pass", "NotSupported", "Fail", "Pass", "QualityWarning"]
    for i, c in enumerate(cases): case(c, sts[i % len(sts)])
elif mode == "truncated":
    case(cases[0], "Pass")
    f.write(f"#beginTestCaseResult {cases[1]}\n"); f.flush()
    sys.exit(0)
elif mode == "incomplete_pass":
    name = cases[0]
    f.write(f"#beginTestCaseResult {name}\n<Result StatusCode=\"Pass\">x</Result>\n")
    f.write("#endSession\n"); f.flush()
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
elif mode == "startup_delay":
    time.sleep(1.2)
    for c in cases: case(c, "Pass")
elif mode == "dirty_exit":
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
    plan = os.environ.get("R3V_NATIVE_PLAN_CAPTURE_FILE", "")
    if plan and os.environ.get("FAKE_PLAN_ORDINAL_GAP"):
        open(plan, "w").write("device zero\n")
        open(plan + ".2", "w").write("device two\n")
    if plan and os.environ.get("FAKE_PLAN_CONFLICT"):
        open(plan, "w").write("transcript\n")
        open(plan + ".no_nonempty_ib", "w").write("no_nonempty_ib\n")
    if plan and os.environ.get("FAKE_PLAN_WRITE_FAILURE"):
        open(plan + ".no_nonempty_ib", "w").write("no_nonempty_ib\n")
        sys.stderr.write("MESA: warning: r3v-native: plan transcript write at destroy failed: Input/output error\n")
    if os.environ.get("FAKE_NO_IB_MSG"): sys.stderr.write("MESA: warning: r3v-native: no executable submission ran; no plan transcript written\n")
    if os.environ.get("FAKE_NO_IB_MARKER"): open(os.environ["R3V_NATIVE_PLAN_CAPTURE_FILE"] + ".no_nonempty_ib", "w").write("no_nonempty_ib\n")
    echo = os.environ.get("FAKE_ECHO_NAME", "")
    if echo: open(os.path.join(os.path.dirname(log), "env_echo.txt"), "w").write(os.environ.get(echo, ""))
    for c in cases:
        if c == os.environ.get("FAKE_HAZARD_CASE", ""):
            open(os.environ["FAKE_DMESG_FILE"], "a").write("[2.0] radeon 0000:01:05.0: GPU lockup\n")
        if c == os.environ.get("FAKE_BREAK_CONTINUITY_CASE", ""):
            open(os.environ["FAKE_DMESG_FILE"], "w").write("[0.5] replacement\n")
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
if mode == "per_case" and cases[0] == os.environ.get("FAKE_NO_END_SESSION_CASE", ""):
    f.close(); sys.exit(0)
f.write("#endSession\n"); f.close()
if mode == "dirty_exit" or (mode == "per_case" and cases[0] == os.environ.get("FAKE_NONZERO_CASE", "")):
    sys.exit(7)
"""

FAKE_QUEUE_REPORT = """#!/bin/sh
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
if [ "${FAKE_QUEUE_OMIT_LEDGER:-0}" != 1 ]; then
  printf 'verb_table_blake3\t%s\n' 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
fi
printf 'claim_consistent\t%s\n' "$consistent"
"""

# A stand-in tracer: it writes the census the real strace would (one
# GEM_CREATE line, then FAKE_STRACE_CS lines of DRM_IOCTL_RADEON_CS in
# the raw form r3v_cs_ioctl_trace parses) and executes the tracee, so
# the planning arms calibrate on a known-good zero and a known-bad count.
FAKE_STRACE = """#!/bin/sh
out=""
while [ "$#" -gt 0 ]; do
  case "$1" in -o) out="$2"; shift 2;; --) shift; break;; *) shift;; esac
done
n="${FAKE_STRACE_CS:-0}"
{ printf 'ioctl(3, 0xc020645d, 0x1) = 0\n'; i=0
  while [ "$i" -lt "$n" ]; do printf 'ioctl(3, 0xc0206466, 0x1) = 0\n'; i=$((i+1)); done
} > "$out"
exec "$@"
"""

FAKE_DMESG = """#!/bin/sh
cat "$FAKE_DMESG_FILE"
"""

SELFTEST_LEDGER = """class\tstatus\tcase_pattern\tdetail_pattern\tdisposition\tauthority\twitness
withheld_feature\tNotSupported\tdEQP-VK\\..*\t-\taccepted\tunimplemented optional path\tdEQP-VK.fake.a|
quality_warning\tQualityWarning\tdEQP-VK\\..*\t-\taccepted\tdEQP quality warning\tdEQP-VK.fake.a|
device_loss\tFail\tdEQP-VK\\.fake\\.b\tDEVICE_LOST\tblocks\tqueue loss classification\tdEQP-VK.fake.b|VK_ERROR_DEVICE_LOST
open_defect\tFail\tdEQP-VK\\.fake\\.[abd]\t-\tblocks\tcatch-all after the specific row\tdEQP-VK.fake.a|
deqp_crash\tCrash\tdEQP-VK\\..*\t-\tblocks\tdEQP-terminated case\tdEQP-VK.fake.c|
"""


def selftest(fixture_qpa):
    # The fixture repository's cleanliness is judged by the runner's git
    # status call, so the host's global and system git configuration
    # (a core.excludesFile listing *.so hides the relative shim below)
    # stays out of every git invocation the selftest makes.
    os.environ["GIT_CONFIG_GLOBAL"] = os.devnull
    os.environ["GIT_CONFIG_SYSTEM"] = os.devnull
    if fixture_qpa:
        fixture_qpa = str(Path(fixture_qpa).resolve())
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        cts_repo = d / "cts"
        cts_repo.mkdir()
        fake = cts_repo / "deqp-vk"
        # The relative-preload case copies the shim beside deqp-vk, and
        # the source-clean verdict has to survive that copy on a host
        # whose git configuration ignores nothing.
        (cts_repo / ".gitignore").write_text(RADEON_DRM_SHIM_BASENAME + "\n")
        fake.write_text(FAKE_DEQP)
        fake.chmod(0o755)
        subprocess.run(["git", "init", "-q", str(cts_repo)], check=True)
        subprocess.run(
            [
                "git",
                "-C",
                str(cts_repo),
                "-c",
                "user.name=R3V runner selftest",
                "-c",
                "user.email=r3v-runner-selftest.invalid",
                "add",
                "deqp-vk",
                ".gitignore",
            ],
            check=True,
        )
        subprocess.run(
            [
                "git",
                "-C",
                str(cts_repo),
                "-c",
                "user.name=R3V runner selftest",
                "-c",
                "user.email=r3v-runner-selftest.invalid",
                "commit",
                "-qm",
                "fixture",
            ],
            check=True,
        )
        subprocess.run(["git", "-C", str(cts_repo), "tag", "fixture"], check=True)
        shim_source = d / "drm_shim.c"
        shim_source.write_text("int r3v_runner_selftest_drm_shim;\n")
        shim = d / RADEON_DRM_SHIM_BASENAME
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + [
                "-shared",
                "-fPIC",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-o",
                str(shim),
                str(shim_source),
            ],
            check=True,
        )
        alternate_shim = d / "libalternate_drm_shim.so"
        alternate_shim.write_bytes(shim.read_bytes())
        relative_shim = cts_repo / RADEON_DRM_SHIM_BASENAME
        relative_shim.write_bytes(shim.read_bytes())
        host_boot_id = read_optional("/proc/sys/kernel/random/boot_id")
        assert host_boot_id
        lib = d / "libvulkan_fake.so"
        lib.write_bytes(b"fake dso")
        manifest = d / "fake_icd.json"
        manifest.write_text(
            json.dumps(
                {
                    "file_format_version": "1.0.0",
                    "ICD": {"library_path": str(lib), "api_version": "1.0.0"},
                }
            )
        )
        caselist = d / "cases.txt"
        caselist.write_text(
            "dEQP-VK.fake.a\ndEQP-VK.fake.b\ndEQP-VK.fake.c\n"
            "dEQP-VK.fake.d\ndEQP-VK.fake.e\n"
        )
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

        def run(
            mode,
            expect,
            dmesg_after=None,
            dso=None,
            cases=caselist,
            timeout=5.0,
            manifest_json=None,
            env=None,
            case_timeout=120.0,
            max_cases=MAX_SHARD_CASES,
            replace_dmesg=False,
            runtime_event=None,
            expect_runtime_event=None,
            outdir=None,
            queue_report=None,
            expect_queue_claim=None,
            expect_caselist=None,
            process_per_case=False,
            plan_nonce_file=None,
            force_evidence=None,
            strace_binary=str(fake_strace),
            expect_report=None,
            ad_hoc_hazard="none",
            shutdown_timeout=1.0,
            source_root=None,
            expect_source=None,
            expect_deqp=None,
            expect_partition=None,
        ):
            os.environ["FAKE_DEQP_MODE"] = mode
            os.environ["FAKE_DMESG_FILE"] = str(dmesg_file)
            os.environ["FAKE_DEQP_REPLAY"] = str(fixture_qpa or "")
            run_index[0] += 1
            outdir = outdir or d / f"out-{run_index[0]}-{mode}-{expect}"
            args = argparse.Namespace(
                deqp_binary=str(fake),
                icd=str(manifest),
                caselist=str(cases),
                out=str(outdir),
                source_root=source_root,
                build_root=None,
                expect_source_sha=expect_source,
                expect_dso_sha256=dso,
                expect_deqp_sha256=expect_deqp,
                expect_caselist_sha256=expect_caselist,
                expect_partition_sha256=expect_partition,
                expect_runtime_event_sha256=expect_runtime_event,
                runtime_event=runtime_event,
                timeout=timeout,
                case_timeout=case_timeout,
                max_cases=max_cases,
                journal_command="",
                nonpass_ledger=str(ledger),
                queue_report=queue_report,
                expect_queue_claim_mode=expect_queue_claim,
                expect_report_sha256=expect_report,
                dmesg_command=str(dmesg_cmd),
                env=(env if env is not None else [f"LD_PRELOAD={shim}"])
                + [f"FAKE_DEQP_MODE={mode}", f"FAKE_DEQP_REPLAY={fixture_qpa or ''}"],
                deqp_arg=None,
                partition_manifest=manifest_json,
                ad_hoc_hazard=ad_hoc_hazard,
                process_per_case=process_per_case,
                plan_nonce_file=plan_nonce_file,
                strace_binary=strace_binary,
                shutdown_timeout=shutdown_timeout,
            )
            if dmesg_after is not None:
                orig = dmesg_file.read_text()
                r = _run_with_dmesg_change(
                    args,
                    dmesg_file,
                    orig,
                    dmesg_after if replace_dmesg else orig + dmesg_after,
                )
            elif force_evidence:
                r = _run_with_evidence(args, force_evidence, outdir / "receipt.json")
            else:
                r = execute(args)
            if r["verdict"] != expect:
                raise SystemExit(
                    f"selftest {mode}: verdict {r['verdict']}, expected "
                    f"{expect}: {r['counts']} {r['classes']}"
                )
            if not force_evidence:
                verify_receipt(outdir / "receipt.json")
            return r

        r = run("all_pass", "pass")
        assert r["counts"] == {"Pass": 5}, r["counts"]
        assert r["evidence_class"] == "host-model"
        assert (
            r["qualification_valid"] is False
            and r["qualification_reason"] == "source identity unavailable"
        )
        assert r["session"]["releaseName"] == "fake-1"
        current_receipt_path = d / "out-1-all_pass-pass" / "receipt.json"
        # A shim declaration earns host-model identity only when the
        # dynamic loader maps that exact object into the probe process.
        missing_shim = d / "missing_drm_shim.so"
        r = run(
            "all_pass",
            "preload_unverified",
            env=[f"LD_PRELOAD={missing_shim}"],
        )
        assert not r["preload"]["shim_loaded"] and "argv" not in r
        assert r["evidence_class"] == "host-unknown"
        # The preload probe and dEQP share the binary directory as their
        # working directory, so the loader resolves a relative declaration
        # to the same mapped object in both processes.
        r = run("all_pass", "pass", env=[f"LD_PRELOAD=./{relative_shim.name}"])
        assert r["preload"]["shim_loaded"]
        assert r["preload"]["mapped"] == [str(relative_shim.resolve())]
        # An ad-hoc caselist declares its hardware hazard before the
        # runner constructs an execution receipt.
        try:
            run("all_pass", "pass", ad_hoc_hazard=None)
        except RunnerRefusal as error:
            assert "requires --ad-hoc-hazard" in str(error)
        else:
            raise SystemExit("selftest: an ad-hoc run omitted its hazard")
        # The deadline reader counts markers across read boundaries without
        # recounting unchanged bytes.  An in-place replacement resets the
        # stream instead of combining marker counts from two QPA logs.
        marker_log = d / "marker-reader.qpa"
        marker_reader = QpaMarkerReader(marker_log)
        assert marker_reader.state() == (0, 0, False)
        marker_log.write_bytes(b"#beginTestCase")
        assert marker_reader.state() == (0, 0, False)
        with marker_log.open("ab") as marker_file:
            marker_file.write(b"Result dEQP-VK.fake.a\n#endTestCaseResult\n")
        assert marker_reader.state() == (1, 1, False)
        assert marker_reader.state() == (1, 1, False)
        with marker_log.open("ab") as marker_file:
            marker_file.write(b"result detail contains #beginTestCaseResult\n")
        assert marker_reader.state() == (1, 1, False)
        with marker_log.open("ab") as marker_file:
            marker_file.write(b"#endSession\n")
        assert marker_reader.state() == (1, 1, True)
        marker_log.write_bytes(b"#beginTestCaseResult dEQP-VK.fake.b\n")
        assert marker_reader.state() == (1, 0, False)
        # A second result record for one case is ambiguous even when both
        # records claim the same status.
        duplicate_qpa = (
            "#sessionInfo releaseName fake-1\n"
            '#sessionInfo logFormatVersion "0.3.4"\n'
            "#beginTestCaseResult dEQP-VK.fake.a\n"
            "#endTestCaseResult\n"
            "#beginTestCaseResult dEQP-VK.fake.a\n"
        )
        try:
            parse_qpa(duplicate_qpa)
        except RunnerRefusal as error:
            assert "more than once" in str(error)
        else:
            raise SystemExit("selftest: duplicate QPA results were admitted")
        legacy_receipt = json.loads(current_receipt_path.read_text())
        legacy_receipt["receipt_version"] = LEGACY_RECEIPT_VERSION
        legacy_receipt[LEGACY_QUALIFICATION_VALIDITY_FIELD] = legacy_receipt.pop(
            "qualification_valid"
        )
        legacy_receipt[LEGACY_QUALIFICATION_REASON_FIELD] = legacy_receipt.pop(
            "qualification_reason"
        )
        legacy_planning = legacy_receipt.get("planning")
        if isinstance(legacy_planning, dict):
            legacy_planning[LEGACY_QUALIFICATION_VALIDITY_FIELD] = legacy_planning.pop(
                "qualification_valid"
            )
        legacy_receipt["seal_sha256"] = seal(legacy_receipt)
        legacy_receipt_path = current_receipt_path.with_name("receipt-v3.json")
        legacy_receipt_path.write_text(json.dumps(legacy_receipt))
        verify_receipt(legacy_receipt_path)

        arbitrary_refusal_receipt = json.loads(current_receipt_path.read_text())
        arbitrary_refusal_receipt["refusal"] = "banana"
        arbitrary_refusal_receipt["verdict"] = "banana"
        arbitrary_refusal_receipt["seal_sha256"] = seal(arbitrary_refusal_receipt)
        arbitrary_refusal_path = current_receipt_path.with_name(
            "receipt-v4-arbitrary-refusal.json"
        )
        arbitrary_refusal_path.write_text(json.dumps(arbitrary_refusal_receipt))
        try:
            verify_receipt(arbitrary_refusal_path)
        except RunnerRefusal as error:
            assert "unknown refusal" in str(error)
        else:
            raise SystemExit("selftest: arbitrary version-4 refusal verified")

        masked_identity_receipt = json.loads(current_receipt_path.read_text())
        masked_identity_receipt["expected"]["source_sha"] = "f" * 40
        masked_identity_receipt["refusal"] = "gate_contamination"
        masked_identity_receipt["verdict"] = "gate_contamination"
        masked_identity_receipt["seal_sha256"] = seal(masked_identity_receipt)
        masked_identity_path = current_receipt_path.with_name(
            "receipt-v4-masked-identity.json"
        )
        masked_identity_path.write_text(json.dumps(masked_identity_receipt))
        try:
            verify_receipt(masked_identity_path)
        except RunnerRefusal as error:
            assert "refusal does not match" in str(error)
        else:
            raise SystemExit("selftest: version-4 identity mismatch was masked")

        contaminating_gate = "R3V_NATIVE_COMPUTE_TEST_GPU_EXPERIMENTAL"
        prioritized_refusal_receipt = json.loads(current_receipt_path.read_text())
        prioritized_refusal_receipt["environment"][contaminating_gate] = "1"
        prioritized_refusal_receipt["contaminating_gates"] = [contaminating_gate]
        prioritized_refusal_receipt["expected"]["source_sha"] = "f" * 40
        prioritized_refusal_receipt["refusal"] = "gate_contamination"
        prioritized_refusal_receipt["verdict"] = "gate_contamination"
        prioritized_refusal_receipt["seal_sha256"] = seal(prioritized_refusal_receipt)
        prioritized_refusal_path = current_receipt_path.with_name(
            "receipt-v4-prioritized-refusal.json"
        )
        prioritized_refusal_path.write_text(json.dumps(prioritized_refusal_receipt))
        verify_receipt(prioritized_refusal_path)

        # Version 3 validates only the historical seal, artifact digests, and
        # result denominator.  Its producer could retain a positive legacy
        # qualification flag beside an identity refusal, and its verifier never
        # recomputed either
        # the refusal or verdict.
        legacy_historical_receipt = json.loads(legacy_receipt_path.read_text())
        legacy_historical_receipt[LEGACY_QUALIFICATION_VALIDITY_FIELD] = True
        legacy_historical_receipt[LEGACY_QUALIFICATION_REASON_FIELD] = None
        legacy_historical_receipt["expected"]["source_sha"] = "f" * 40
        legacy_historical_receipt["refusal"] = "wrong_icd"
        legacy_historical_receipt["verdict"] = "wrong_icd"
        legacy_historical_receipt["seal_sha256"] = seal(legacy_historical_receipt)
        legacy_historical_path = current_receipt_path.with_name(
            "receipt-v3-historical-semantics.json"
        )
        legacy_historical_path.write_text(json.dumps(legacy_historical_receipt))
        verify_receipt(legacy_historical_path)

        incomplete_receipt = json.loads(current_receipt_path.read_text())
        del incomplete_receipt["qualification_valid"]
        incomplete_receipt["seal_sha256"] = seal(incomplete_receipt)
        incomplete_receipt_path = current_receipt_path.with_name(
            "receipt-v4-incomplete.json"
        )
        incomplete_receipt_path.write_text(json.dumps(incomplete_receipt))
        try:
            verify_receipt(incomplete_receipt_path)
        except RunnerRefusal as error:
            assert "lacks boolean qualification_valid" in str(error)
        else:
            raise SystemExit("selftest: an incomplete version-4 receipt verified")

        contradictory_receipt = json.loads(current_receipt_path.read_text())
        contradictory_receipt["qualification_valid"] = True
        contradictory_receipt["qualification_reason"] = "source tree dirty"
        contradictory_receipt["seal_sha256"] = seal(contradictory_receipt)
        contradictory_receipt_path = current_receipt_path.with_name(
            "receipt-v4-contradictory.json"
        )
        contradictory_receipt_path.write_text(json.dumps(contradictory_receipt))
        try:
            verify_receipt(contradictory_receipt_path)
        except RunnerRefusal as error:
            assert "qualification fields do not match" in str(error)
        else:
            raise SystemExit("selftest: contradictory qualification fields verified")

        false_verdict_receipt = json.loads(current_receipt_path.read_text())
        false_verdict_receipt["verdict"] = "classified_nonpass"
        false_verdict_receipt["seal_sha256"] = seal(false_verdict_receipt)
        false_verdict_receipt_path = current_receipt_path.with_name(
            "receipt-v4-false-verdict.json"
        )
        false_verdict_receipt_path.write_text(json.dumps(false_verdict_receipt))
        try:
            verify_receipt(false_verdict_receipt_path)
        except RunnerRefusal as error:
            assert "verdict does not match" in str(error)
        else:
            raise SystemExit("selftest: a false version-4 verdict verified")

        identity_mutations = (
            ("source_sha", "f" * 40),
            ("dso_sha256", "f" * 64),
            ("deqp_sha256", "f" * 64),
            ("caselist_sha256", "f" * 64),
            ("partition_sha256", "f" * 64),
            ("runtime_event_sha256", "f" * 64),
            ("queue_claim_mode", "conformant"),
        )
        for identity_field, wrong_identity in identity_mutations:
            mismatch_receipt = json.loads(current_receipt_path.read_text())
            mismatch_receipt["expected"][identity_field] = wrong_identity
            mismatch_receipt["seal_sha256"] = seal(mismatch_receipt)
            mismatch_path = current_receipt_path.with_name(
                f"receipt-v4-wrong-{identity_field}.json"
            )
            mismatch_path.write_text(json.dumps(mismatch_receipt))
            try:
                verify_receipt(mismatch_path)
            except RunnerRefusal as error:
                assert "identity mismatch is not reflected" in str(error)
            else:
                raise SystemExit(f"selftest: wrong {identity_field} identity verified")
        # The process environment is allowlisted: the ambient gate below
        # never reaches the run, while the declared preload does.
        os.environ["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"] = "1"
        r = run("all_pass", "pass")
        del os.environ["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED"]
        assert "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED" not in r["environment"]
        assert r["environment"]["LD_PRELOAD"] == str(shim)
        assert set(r["environment"]) <= set(INHERITED_ENV) | {
            "HOME",
            "LD_PRELOAD",
            "FAKE_DEQP_MODE",
            "FAKE_DEQP_REPLAY",
            "VK_DRIVER_FILES",
        } | {k for k in r["environment"] if k.startswith("LC_")}
        assert all(
            r["environment"][k].startswith("sha256:")
            for k in r["environment"]
            if k in INHERITED_ENV
        )
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
        joined = hashlib.sha256("\n".join(read_caselist(caselist)).encode()).hexdigest()
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
        r = run(
            "all_pass",
            "kernel_log_continuity_broken",
            dmesg_after="[0.5] earlier\n",
            replace_dmesg=True,
        )
        assert r["dmesg"]["continuity"] == "broken" and r["dmesg"]["available"] is False
        # The in-flight case's deadline kills the process and names it;
        # a process that hangs after closing its session is the runner
        # deadline's shape even under the case deadline; a kernel hazard
        # outranks the case deadline it most likely caused.
        r = run("timeout", "case_deadline", timeout=30.0, case_timeout=1.5)
        assert r["exit_code"] == "case_timeout" and r["counts"].get("timeout") == 1, r[
            "counts"
        ]
        r = run("hang_after_session", "runner_deadline", timeout=30.0, case_timeout=1.5)
        assert r["exit_code"] == "shutdown_timeout" and r["session_closed"]
        r = run(
            "timeout",
            "dmesg_hazard",
            timeout=30.0,
            case_timeout=1.5,
            dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n",
        )
        assert r["exit_code"] == "case_timeout"
        r = run(
            "hang_after_session",
            "dmesg_hazard",
            timeout=30.0,
            case_timeout=1.5,
            dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n",
        )
        assert r["exit_code"] == "shutdown_timeout" and r["session_closed"]
        r = run(
            "hang_after_session",
            "dmesg_hazard",
            timeout=2.0,
            shutdown_timeout=30.0,
            dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n",
        )
        assert r["exit_code"] == "timeout"
        # The queue-claim report is recorded under the run's environment:
        # the mode names what the compute bit rests on, only the
        # conformant mode makes the receipt conformance-eligible, an
        # expected mode other than the reported one refuses, and an
        # inconsistent report refuses.
        report = d / "report.sh"
        report.write_text(FAKE_QUEUE_REPORT)
        report.chmod(0o755)
        r = run(
            "all_pass",
            "pass",
            queue_report=str(report),
            env=[
                f"LD_PRELOAD={shim}",
                "FAKE_QUEUE_MODE=experimental_compute_subset",
            ],
        )
        assert (
            r["queue_claim"]["mode"] == "experimental_compute_subset"
            and r["queue_claim"]["compute_bit"]
            and r["queue_claim"]["gate_declared"]
            and not r["compute_claim_eligible"]
            and r["queue_claim"]["report_sha256"]
        )
        r = run(
            "all_pass",
            "pass",
            queue_report=str(report),
            env=[f"LD_PRELOAD={shim}", "FAKE_QUEUE_MODE=conformant"],
        )
        assert r["compute_claim_eligible"]
        # The gated mode without the gate is inconsistent.
        try:
            run(
                "all_pass",
                "pass",
                queue_report=str(report),
                env=[f"LD_PRELOAD={shim}", "FAKE_QUEUE_MODE=gateless"],
            )
        except RunnerRefusal as e:
            assert "gate False" in str(e)
        else:
            raise SystemExit("selftest: a gated mode without its gate was " "admitted")
        r = run(
            "all_pass",
            "wrong_queue_claim",
            queue_report=str(report),
            env=[
                f"LD_PRELOAD={shim}",
                "FAKE_QUEUE_MODE=default_graphics_only",
            ],
            expect_queue_claim="conformant",
        )
        assert "argv" not in r and not r["queue_claim"]["compute_bit"]
        try:
            run(
                "all_pass",
                "pass",
                queue_report=str(report),
                env=[f"LD_PRELOAD={shim}", "FAKE_QUEUE_MODE=inconsistent"],
            )
        except RunnerRefusal as e:
            assert "queue-claim report refused" in str(e)
        else:
            raise SystemExit("selftest: an inconsistent queue report was " "admitted")
        try:
            run(
                "all_pass",
                "pass",
                queue_report=str(report),
                env=[
                    f"LD_PRELOAD={shim}",
                    "FAKE_QUEUE_MODE=conformant",
                    "FAKE_QUEUE_OMIT_LEDGER=1",
                ],
            )
        except RunnerRefusal as error:
            assert "verb ledger None" in str(error)
        else:
            raise SystemExit("selftest: a queue report omitted its verb ledger")
        # A runtime event joins by digest; a wrong digest refuses.
        event = d / "event.json"
        event.write_text(
            json.dumps({"run_id": "rs482-001", "boot_id": host_boot_id}) + "\n"
        )
        digest = hashlib.sha256(event.read_bytes()).hexdigest()
        r = run(
            "all_pass", "pass", runtime_event=str(event), expect_runtime_event=digest
        )
        assert (
            r["runtime_event"]["run_id"] == "rs482-001"
            and r["artifacts"].get("runtime_event.json") == digest
        )
        r = run(
            "all_pass",
            "wrong_runtime_event",
            runtime_event=str(event),
            expect_runtime_event="0" * 64,
        )
        assert "argv" not in r
        for event_name, event_body, message in (
            ("scalar", "7\n", "not a JSON object"),
            (
                "wrong-boot",
                json.dumps({"run_id": "rs482-002", "boot_id": "0" * 36}) + "\n",
                "does not match the current boot",
            ),
        ):
            invalid_event = d / f"event-{event_name}.json"
            invalid_event.write_text(event_body)
            try:
                run("all_pass", "pass", runtime_event=str(invalid_event))
            except RunnerRefusal as error:
                assert message in str(error)
            else:
                raise SystemExit(f"selftest: {event_name} runtime event was admitted")
        mixed_output = d / "out-version-4-disposition"
        r = run("mixed", "unclassified_nonpass", outdir=mixed_output)
        assert r["counts"] == {
            "Pass": 2,
            "NotSupported": 1,
            "Fail": 1,
            "QualityWarning": 1,
        }, r["counts"]
        assert r["classes"] == {
            "withheld_feature": 1,
            "quality_warning": 1,
            "unclassified": 1,
        }, r["classes"]
        invalid_disposition_receipt = json.loads(
            (mixed_output / "receipt.json").read_text()
        )
        invalid_disposition_receipt["results"]["dEQP-VK.fake.b"][
            "disposition"
        ] = "banana"
        invalid_disposition_receipt["seal_sha256"] = seal(invalid_disposition_receipt)
        invalid_disposition_path = mixed_output / "receipt-invalid-disposition.json"
        invalid_disposition_path.write_text(json.dumps(invalid_disposition_receipt))
        try:
            verify_receipt(invalid_disposition_path)
        except RunnerRefusal as error:
            assert "disposition must be accepted or blocks" in str(error)
        else:
            raise SystemExit("selftest: an invalid non-pass disposition verified")
        r = run("truncated", "truncated_run")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "truncated"
        assert r["results"]["dEQP-VK.fake.c"]["status"] == "not_run"
        r = run("incomplete_pass", "truncated_run")
        assert r["results"]["dEQP-VK.fake.a"]["status"] == "truncated"
        assert not r["session_closed"]
        r = run("timeout", "runner_deadline")
        assert r["results"]["dEQP-VK.fake.b"]["status"] == "timeout"
        r = run("hang_after_session", "runner_deadline")
        assert r["session_closed"] and r["counts"] == {"Pass": 5}
        # Startup time precedes the first case marker and consumes only
        # the shard deadline, while a completed nonzero process exit
        # remains a dirty run even when every case reports Pass.
        r = run("startup_delay", "pass", case_timeout=0.3)
        assert r["exit_code"] == 0 and r["counts"] == {"Pass": 5}
        r = run("dirty_exit", "dirty_exit")
        assert r["exit_code"] == 7 and r["session_closed"]
        # Both QPA identity fields bind the results to the emitting CTS
        # framework.  Either missing field refuses the completed run.
        for flag, field in (
            ("FAKE_OMIT_RELEASE=1", "releaseName"),
            ("FAKE_OMIT_LOG_FORMAT=1", "logFormatVersion"),
        ):
            r = run(
                "all_pass",
                "missing_session_identity",
                env=[f"LD_PRELOAD={shim}", flag],
            )
            assert field not in r["session"]
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
        r = run("late_abort", "framework_abort")
        assert r["framework_abort"] and "late abort" in r["framework_abort"]
        r = run("framework", "framework_precondition")
        assert r["counts"] == {"not_run": 5}, r["counts"]
        assert r["classes"] == {"runner_not_run": 5}, r["classes"]
        r = run("all_pass", "wrong_icd", dso="0" * 64)
        assert r["counts"] == {"not_run": 5}
        r = run(
            "all_pass",
            "dmesg_hazard",
            dmesg_after="[2.0] radeon 0000:01:05.0: GPU lockup\n",
        )
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
        (corpus / "m.txt").write_text(caselist.read_text() + "dEQP-VK.other.x\n")
        table = d / "partition.tsv"
        table.write_text(
            "\t".join(part.HEADER) + "\n"
            "1\tfake\tdEQP-VK.fake\tnone\thost-model\n"
            "2\tother\tdEQP-VK.other\tunknown\tsilicon\n"
        )
        pdir = d / "partition"
        cases_all = sorted(
            x.strip() for x in (corpus / "m.txt").read_text().splitlines() if x.strip()
        )
        ppin = d / "pin.tsv"
        ppin.write_text(
            f"cts_describe\tfixture\ncase_count\t"
            f"{len(cases_all)}\ncorpus_sha256\t"
            f"{part.sha256_lines(cases_all)}\n"
        )
        part.generate(table, corpus, pdir, "exhaustive", pin_path=ppin)
        mj = str(pdir / "partition_manifest.json")
        r = run("all_pass", "pass", cases=pdir / "fake.txt", manifest_json=mj)
        assert (
            r["partition"]["slice"] == "fake" and r["partition"]["kind"] == "exhaustive"
        )
        r = run("all_pass", "blocked_slice", cases=pdir / "other.txt", manifest_json=mj)
        assert r["partition"]["hazard"] == "unknown" and "argv" not in r
        pm = json.loads((pdir / "partition_manifest.json").read_text())
        assert (
            r["partition"]["caselist_sha256"] == pm["slices"][1]["caselist_sha256"]
            and r["partition"]["executable_case_count"]
            == pm["executable_case_count"]
            == pm["slices"][0]["case_count"]
        )
        # The partition's CTS tag binds the executable revision, not
        # merely its byte digest.
        wrong_pin = d / "wrong-pin.tsv"
        wrong_pin.write_text(
            f"cts_describe\twrong-fixture\ncase_count\t"
            f"{len(cases_all)}\ncorpus_sha256\t"
            f"{part.sha256_lines(cases_all)}\n"
        )
        wrong_cts_dir = d / "partition-wrong-cts"
        part.generate(table, corpus, wrong_cts_dir, "exhaustive", pin_path=wrong_pin)
        r = run(
            "all_pass",
            "wrong_cts_revision",
            cases=wrong_cts_dir / "fake.txt",
            manifest_json=str(wrong_cts_dir / "partition_manifest.json"),
        )
        assert "argv" not in r
        # A shard collects the children its cases leave behind, because a
        # process table that grows with the case count reaches the per-user
        # limit and ends a long shard mid-sequence.
        import subprocess as _subprocess

        abandoned = _subprocess.Popen(
            ["/bin/sh", "-c", "exit 7"],
            stdout=_subprocess.DEVNULL,
            stderr=_subprocess.DEVNULL,
        )
        abandoned_pid = abandoned.pid
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if Path(f"/proc/{abandoned_pid}/stat").read_text().split()[2] == "Z":
                break
            time.sleep(0.05)
        assert reap_finished_children() >= 1
        assert not Path(f"/proc/{abandoned_pid}").exists()
        # A sweep with nothing to collect reports nothing and refuses to
        # block; a sweep that waited here would stall every shard.
        assert reap_finished_children() == 0
        abandoned.returncode = 7
        # A provisioned bundle carries no worktree, so its sealed
        # provenance is the CTS-revision authority.  The document holds
        # only for the binary it names and only while its seal recomputes.
        bundle = d / "bundle"
        bundle.mkdir()
        bundle_binary = bundle / "deqp-vk"
        bundle_binary.write_bytes(b"bundled dEQP\n")
        bundle_digest = sha256_file(bundle_binary)
        other_binary = bundle / "other-deqp-vk"
        other_binary.write_bytes(b"another dEQP\n")

        def seal_provenance(document):
            body = json.dumps(
                document, sort_keys=True, separators=(",", ":")
            ).encode()
            sealed = dict(document)
            sealed["provenance_sha256"] = hashlib.sha256(body).hexdigest()
            (bundle / "provenance.json").write_text(json.dumps(sealed, indent=1))
            return sealed

        good = {
            "source": {"describe": "fixture"},
            "binary": {"sha256": bundle_digest},
        }
        seal_provenance(good)
        assert bundle_cts_describe(bundle_binary, bundle_digest) == "fixture"
        assert deqp_identity(str(bundle_binary))["cts_identity_authority"] == (
            "bundle_provenance"
        )
        assert cts_revision({"cts_bundle_describe": "fixture"}) == "fixture"
        # The worktree answers ahead of a bundle document.
        assert (
            cts_revision(
                {
                    "cts_worktree_describe": "worktree",
                    "cts_bundle_describe": "fixture",
                }
            )
            == "worktree"
        )
        # A document describing another binary names no revision for this one.
        assert bundle_cts_describe(other_binary, sha256_file(other_binary)) is None
        # An edited document whose seal was not recomputed names nothing.
        tampered = seal_provenance(good)
        tampered["source"]["describe"] = "edited-after-seal"
        (bundle / "provenance.json").write_text(json.dumps(tampered, indent=1))
        assert bundle_cts_describe(bundle_binary, bundle_digest) is None
        # An unsealed document names nothing.
        (bundle / "provenance.json").write_text(json.dumps(good, indent=1))
        assert bundle_cts_describe(bundle_binary, bundle_digest) is None
        # An unparseable document names nothing.
        (bundle / "provenance.json").write_text("{ not json")
        assert bundle_cts_describe(bundle_binary, bundle_digest) is None
        # An absent document names nothing, which is the state every
        # non-bundle binary is in.
        (bundle / "provenance.json").unlink()
        assert bundle_cts_describe(bundle_binary, bundle_digest) is None
        assert deqp_identity(str(bundle_binary))["cts_identity_authority"] is None
        # A queue-report digest remains mandatory for qualification even
        # when every other source, binary, caselist, and partition
        # identity matches.  Declaring the report digest closes the last
        # identity gap for this host-model fixture.
        source_rc, fixture_source_sha = run_capture(
            ["git", "rev-parse", "HEAD"], cwd=cts_repo
        )
        assert source_rc == 0
        qualification_arguments = {
            "cases": pdir / "fake.txt",
            "manifest_json": mj,
            "queue_report": str(report),
            "env": [f"LD_PRELOAD={shim}", "FAKE_QUEUE_MODE=conformant"],
            "source_root": str(cts_repo),
            "expect_source": fixture_source_sha,
            "dso": sha256_file(lib),
            "expect_deqp": sha256_file(fake),
            "expect_caselist": sha256_file(pdir / "fake.txt"),
            "expect_partition": pm["manifest_sha256"],
            "expect_queue_claim": "conformant",
        }
        r = run("all_pass", "pass", **qualification_arguments)
        assert (
            r["qualification_valid"] is False
            and r["qualification_reason"] == "undeclared queue-claim report SHA-256"
        )
        r = run(
            "all_pass",
            "pass",
            expect_report=sha256_file(report),
            **qualification_arguments,
        )
        assert r["qualification_valid"] is True and r["qualification_reason"] is None
        # A silicon-required slice under the drm-shim host model refuses
        # before dEQP starts.
        table.write_text(
            "\t".join(part.HEADER) + "\n"
            "1\tfake\tdEQP-VK.fake\tsubmission\tsilicon\n"
            "2\tother\tdEQP-VK.other\tunknown\tsilicon\n"
        )
        sdir = d / "partition-silicon"
        part.generate(table, corpus, sdir, "exhaustive", pin_path=ppin)
        r = run(
            "all_pass",
            "evidence_below_required",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
        )
        assert "argv" not in r
        # A declared submission or experimental-route gate on a
        # hazard-free slice is contamination and refuses before dEQP.
        for gate in (
            "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1",
            "R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL=1",
            "R3V_NATIVE_COMPUTE_IDENTITY_GPU_EXPERIMENTAL=1",
            "R3V_NATIVE_PLAN_CAPTURE_FILE=/x",
        ):
            r = run(
                "all_pass",
                "gate_contamination",
                cases=pdir / "fake.txt",
                manifest_json=mj,
                env=[f"LD_PRELOAD={shim}", gate],
            )
            assert "argv" not in r and r["contaminating_gates"] == [gate.split("=")[0]]
        # A display-hazard slice refuses a submission gate as well.
        table.write_text(
            "\t".join(part.HEADER) + "\n"
            "1\tfake\tdEQP-VK.fake\tdisplay\tsilicon\n"
            "2\tother\tdEQP-VK.other\tunknown\tsilicon\n"
        )
        ddir = d / "partition-display"
        part.generate(table, corpus, ddir, "exhaustive", pin_path=ppin)
        r = run(
            "all_pass",
            "gate_contamination",
            cases=ddir / "fake.txt",
            manifest_json=str(ddir / "partition_manifest.json"),
            env=[
                f"LD_PRELOAD={shim}",
                "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1",
            ],
        )
        assert "argv" not in r
        # A shard of a split slice binds by its own digest and records
        # its index; the shard ceiling matches the manifest's.
        pdir2 = d / "partition-sharded"
        table.write_text(
            "\t".join(part.HEADER) + "\n"
            "1\tfake\tdEQP-VK.fake\tnone\thost-model\n"
            "2\tother\tdEQP-VK.other\tunknown\tsilicon\n"
        )
        part.generate(table, corpus, pdir2, "exhaustive", pin_path=ppin, shard_max=2)
        r = run(
            "all_pass",
            "pass",
            cases=pdir2 / "fake.0001.txt",
            manifest_json=str(pdir2 / "partition_manifest.json"),
            max_cases=2,
        )
        assert (
            r["partition"]["shard_index"] == 1
            and r["partition"]["shard_count"] == 3
            and r["partition"]["shard_case_count"] == 2
        )
        # A runner ceiling other than the manifest's refuses, sealed.
        r = run(
            "all_pass",
            "shard_ceiling_mismatch",
            cases=pdir2 / "fake.0001.txt",
            manifest_json=str(pdir2 / "partition_manifest.json"),
            max_cases=3,
        )
        assert "argv" not in r
        # The compute-queue framework gate is a declared, recorded value.
        r = run(
            "all_pass",
            "pass",
            cases=pdir / "fake.txt",
            manifest_json=mj,
            env=[
                f"LD_PRELOAD={shim}",
                "R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1",
            ],
        )
        assert r["environment"]["R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL"] == "1"
        assert r["qualification_reason"] == "source identity unavailable"
        # A proper subset of one shard binds as that shard's subset and
        # records its own count and digest; a case outside every shard
        # refuses under the manifest.
        subset = d / "subset.txt"
        subset.write_text("dEQP-VK.fake.a\n")
        r = run("all_pass", "pass", cases=subset, manifest_json=mj)
        assert (
            r["partition"]["binding"] == "shard_subset"
            and r["partition"]["slice"] == "fake"
            and r["partition"]["subset_case_count"] == 1
            and r["partition"]["subset_caselist_sha256"] == part.sha256_file(subset)
        ), r["partition"]
        assert r["partition"]["shard_case_count"] > 1
        stray = d / "stray.txt"
        stray.write_text("dEQP-VK.fake.a\ndEQP-VK.nowhere\n")
        try:
            run("all_pass", "pass", cases=stray, manifest_json=mj)
        except RunnerRefusal as e:
            assert "outside every manifest shard" in str(e)
        else:
            raise SystemExit(
                "selftest: an unbound caselist was admitted " "under a manifest"
            )
        if fixture_qpa:
            real_cases = d / "real.txt"
            names = re.findall(
                r"^#beginTestCaseResult (\S+)",
                Path(fixture_qpa).read_text(),
                re.MULTILINE,
            )
            real_cases.write_text("\n".join(names) + "\n")
            r = run("replay", "unclassified_nonpass", cases=real_cases)
            assert r["counts"] == {"Pass": 17, "NotSupported": 3, "Fail": 1}, r[
                "counts"
            ]
            assert r["session"]["logFormatVersion"] == QPA_LOG_FORMAT
            assert r["session"]["deviceID"] == "0x5974"
            assert (
                "feature limits failed"
                in r["results"]["dEQP-VK.info.device_properties"]["detail"]
            )
        # One process apiece: the shard receipt stays one receipt, each
        # case's status comes from its own process's log, and the
        # per-case artifacts digest under that case's directory.
        base_env = [f"LD_PRELOAD={shim}"]
        r = run("per_case", "pass", process_per_case=True, env=base_env)
        assert r["process_per_case"] and r["counts"] == {"Pass": 5}
        assert r["session_closed"] and r["exit_code"] == 0
        assert r["case_directories"]["dEQP-VK.fake.a"] == "dEQP-VK.fake.a"
        assert r["cases"]["dEQP-VK.fake.e"]["index"] == 5
        assert "cases/dEQP-VK.fake.c/run.qpa" in r["artifacts"]
        assert "run.qpa" not in r["artifacts"]
        assert len(r["artifacts"]) == 5 * len(CASE_ARTIFACT_NAMES) + 1
        # Long case names retain a unique digest suffix while every
        # directory component stays within the runner's byte ceiling.
        long_cases = d / "long-cases.txt"
        long_prefix = "dEQP-VK.fake." + "x" * 240
        long_cases.write_text(long_prefix + "a\n" + long_prefix + "b\n")
        r = run(
            "per_case",
            "pass",
            cases=long_cases,
            process_per_case=True,
            env=base_env,
        )
        long_directories = list(r["case_directories"].values())
        assert len(set(long_directories)) == 2
        assert all(
            len(directory.encode()) <= MAX_CASE_DIRECTORY_BYTES
            for directory in long_directories
        )
        # A clean QPA session with a completed nonzero exit stays dirty;
        # an omitted per-case session terminator stays truncated.
        r = run(
            "per_case",
            "dirty_exit",
            process_per_case=True,
            env=base_env + ["FAKE_NONZERO_CASE=dEQP-VK.fake.c"],
        )
        assert r["exit_code"] == 7 and r["session_closed"]
        r = run(
            "per_case",
            "truncated_run",
            process_per_case=True,
            env=base_env + ["FAKE_NO_END_SESSION_CASE=dEQP-VK.fake.c"],
        )
        assert not r["cases"]["dEQP-VK.fake.c"]["session_closed"]
        # Every per-case process carries its own CTS session identity;
        # an identity missing from a later process cannot hide behind
        # the first process's complete session block.
        r = run(
            "per_case",
            "missing_session_identity",
            process_per_case=True,
            env=base_env + ["FAKE_OMIT_RELEASE_CASE=dEQP-VK.fake.c"],
        )
        assert "releaseName" not in r["cases"]["dEQP-VK.fake.c"]["session"]
        # A case whose process dies keeps its own crash status while the
        # missing session terminator leaves the shard truncated.
        r = run(
            "per_case",
            "truncated_run",
            process_per_case=True,
            env=base_env + ["FAKE_CRASH_CASE=dEQP-VK.fake.c"],
        )
        assert r["results"]["dEQP-VK.fake.c"]["status"] == "crash"
        assert "SIGSEGV" in r["results"]["dEQP-VK.fake.c"]["detail"]
        assert r["counts"] == {"Pass": 4, "crash": 1}, r["counts"]
        assert not r["session_closed"] and r["exit_code"] == -11
        assert r["cases"]["dEQP-VK.fake.c"]["exit_code"] == -11
        # The case deadline kills that case's process alone, and its
        # missing session terminator leaves the shard truncated.
        r = run(
            "per_case",
            "truncated_run",
            process_per_case=True,
            timeout=60.0,
            case_timeout=1.5,
            env=base_env + ["FAKE_SLOW_CASE=dEQP-VK.fake.c"],
        )
        assert r["counts"] == {"Pass": 4, "timeout": 1}, r["counts"]
        assert r["cases"]["dEQP-VK.fake.c"]["exit_code"] == "case_timeout"
        # The shard deadline ends the sequence; the cases behind it
        # never ran.
        r = run(
            "per_case",
            "runner_deadline",
            process_per_case=True,
            timeout=3.0,
            case_timeout=60.0,
            env=base_env + ["FAKE_SLOW_CASE=dEQP-VK.fake.b"],
        )
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
        r = run(
            "per_case",
            "pass",
            process_per_case=True,
            outdir=templated_out,
            env=base_env
            + [
                f"FAKE_CAPTURE_FILE={caps}/{{index}}-{{case}}.transcript",
                f"FAKE_PLAN_FILE={plans}/{{case}}.plan",
                "FAKE_ECHO_NAME=FAKE_CAPTURE_FILE",
            ],
        )
        assert r["templated_env"] == ["FAKE_CAPTURE_FILE", "FAKE_PLAN_FILE"]
        assert r["index_width"] == 1
        rec = r["cases"]["dEQP-VK.fake.d"]
        assert (
            rec["environment"]["FAKE_CAPTURE_FILE"]
            == f"{caps}/4-dEQP-VK.fake.d.transcript"
        )
        assert rec["paths"]["FAKE_CAPTURE_FILE"] == {
            "present_before": False,
            "present_after": True,
        }
        # A case whose plan file is absent still runs; the receipt names
        # which case resolved one that existed.
        assert (
            r["cases"]["dEQP-VK.fake.b"]["paths"]["FAKE_PLAN_FILE"]["present_before"]
            is True
        )
        assert (
            r["cases"]["dEQP-VK.fake.a"]["paths"]["FAKE_PLAN_FILE"]["present_before"]
            is False
        )
        assert r["counts"] == {"Pass": 5}
        assert (caps / "4-dEQP-VK.fake.d.transcript").is_file()
        # The resolved value reaches the case's own process, which
        # echoes it back into that case's directory.
        assert (
            templated_out / "cases" / "dEQP-VK.fake.d" / "env_echo.txt"
        ).read_text() == f"{caps}/4-dEQP-VK.fake.d.transcript"
        # `{nonce}` resolves through the declared per-case nonce file.
        nonce_tsv = d / "nonces.tsv"
        nonce_tsv.write_text(
            "".join(
                f"dEQP-VK.fake.{c}\t{i:032x}\n" for i, c in enumerate("abcde", start=1)
            )
        )
        nonce_out = d / "out-nonce"
        r = run(
            "per_case",
            "pass",
            process_per_case=True,
            plan_nonce_file=str(nonce_tsv),
            outdir=nonce_out,
            env=base_env + ["FAKE_NONCE={nonce}", "FAKE_ECHO_NAME=FAKE_NONCE"],
        )
        assert r["cases"]["dEQP-VK.fake.c"]["environment"]["FAKE_NONCE"] == f"{3:032x}"
        assert r["plan_nonce_file"] == str(nonce_tsv)
        assert (
            nonce_out / "cases" / "dEQP-VK.fake.c" / "env_echo.txt"
        ).read_text() == f"{3:032x}"
        # A nonce token with no file, a malformed nonce, and a case the
        # file leaves out each refuse before dEQP starts.
        for bad, text, message in (
            ("no-file", None, "no --plan-nonce-file"),
            ("short", "dEQP-VK.fake.a\tabcd\n", "32 lowercase hex"),
            ("missing", f"dEQP-VK.fake.a\t{1:032x}\n", "declares no nonce"),
            (
                "reused",
                "".join(f"dEQP-VK.fake.{c}\t{7:032x}\n" for c in "abcde"),
                "reuses a nonce",
            ),
        ):
            path = None
            if text is not None:
                path = d / f"nonce-{bad}.tsv"
                path.write_text(text)
            try:
                run(
                    "per_case",
                    "pass",
                    process_per_case=True,
                    plan_nonce_file=str(path) if path else None,
                    env=base_env + ["FAKE_NONCE={nonce}"],
                )
            except RunnerRefusal as e:
                assert message in str(e), str(e)
            else:
                raise SystemExit(f"selftest: nonce defect {bad} admitted")
        # A per-case token outside --process-per-case refuses: a single
        # process would pass the token to dEQP verbatim.
        try:
            run("all_pass", "pass", env=base_env + ["FAKE_CAPTURE_FILE=/x/{case}"])
        except RunnerRefusal as e:
            assert "outside --process-per-case" in str(e)
        else:
            raise SystemExit("selftest: a token reached a single process")
        # Two cases sharing one directory name refuse before the shard
        # runs; the second would overwrite the first's log.
        clash = d / "clash.txt"
        clash.write_text("dEQP-VK.fake.a b\ndEQP-VK.fake.a_b\n")
        try:
            run("per_case", "pass", cases=clash, process_per_case=True, env=base_env)
        except RunnerRefusal as e:
            assert "one directory per case" in str(e)
        else:
            raise SystemExit("selftest: colliding case directories admitted")
        # A kernel hazard stops the sequence where it appeared: the
        # cases behind it never run, and each names why.
        dmesg_file.write_text("[1.0] boot\n")
        r = run(
            "per_case",
            "dmesg_hazard",
            process_per_case=True,
            env=base_env
            + ["FAKE_HAZARD_CASE=dEQP-VK.fake.b", f"FAKE_DMESG_FILE={dmesg_file}"],
        )
        assert r["hazard_lines"] and r["counts"] == {"Pass": 2, "not_run": 3}, r[
            "counts"
        ]
        assert r["stop"]["after_case"] == "dEQP-VK.fake.b" and r["stop"]["index"] == 2
        assert (
            r["results"]["dEQP-VK.fake.e"]["detail"] == "kernel hazard after this case"
        )
        assert len(r["cases"]) == 2 and not r["session_closed"]
        dmesg_file.write_text("[1.0] boot\n")
        # Kernel-log continuity loss stops the same per-case sequence;
        # later case results would lack the kernel hazard boundary that
        # makes their verdict interpretable.
        r = run(
            "per_case",
            "kernel_log_continuity_broken",
            process_per_case=True,
            env=base_env
            + [
                "FAKE_BREAK_CONTINUITY_CASE=dEQP-VK.fake.b",
                f"FAKE_DMESG_FILE={dmesg_file}",
            ],
        )
        assert r["counts"] == {"Pass": 2, "not_run": 3}
        assert r["stop"]["reason"] == "kernel log continuity broke after this case"
        assert len(r["cases"]) == 2
        dmesg_file.write_text("[1.0] boot\n")
        # The host-planning disposition: a capture file declared on a
        # submission-hazard slice under the radeon drm-shim, gates closed,
        # one process apiece, and a per-process witness of zero
        # kernel-entering CS ioctls.  The receipt carries the evidence
        # class host-planning, no valid qualification result, each case's outcome, and
        # every transcript's digest.
        shim_env = [f"LD_PRELOAD={d}/{RADEON_DRM_SHIM_BASENAME}"]
        arm_index = [0]

        def plan_env_for_arm():
            """Each arm gets its own capture root, so a transcript one
            arm wrote never answers for the next."""
            arm_index[0] += 1
            root = caps / f"arm{arm_index[0]}"
            root.mkdir()
            return (
                shim_env + [f"R3V_NATIVE_PLAN_CAPTURE_FILE={root}/p_{{case}}.t"],
                root,
            )

        plan_env, root = plan_env_for_arm()
        relative_tracer = os.path.relpath(fake_strace)
        r = run(
            "per_case",
            "pass",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=plan_env + [f"FAKE_CAPTURE_FILE={root}/p_{{case}}.t"],
            strace_binary=relative_tracer,
        )
        pl = r["planning"]
        assert (
            r["evidence_class"] == PLANNING_EVIDENCE
            and pl["disposition"] == PLANNING_EVIDENCE
            and pl["candidate"]
            and pl["tracer"]["binary"] == str(fake_strace.resolve())
        )
        assert (
            r["qualification_valid"] is False
            and r["qualification_reason"] == PLANNING_QUALIFICATION_REASON
        )
        assert r["partition"]["required_evidence"] == "silicon"
        assert all(pl["conditions"].values()) and pl["refused_conditions"] == [], pl
        assert (
            pl["cs_witness"]["cs_ioctls"] == 0
            and pl["cs_witness"]["total_ioctls"] == 5
            and pl["cs_witness"]["unwitnessed_cases"] == []
        )
        assert pl["outcomes"] == {
            "transcript": 5,
            "no_nonempty_ib": 0,
            "unresolved": 0,
        }, pl["outcomes"]
        assert f"{root}/p_dEQP-VK.fake.a.t" in pl["transcripts"]["dEQP-VK.fake.a"]
        assert "cases/dEQP-VK.fake.a/ioctl.strace" in r["artifacts"]
        assert r["cases"]["dEQP-VK.fake.a"]["planning"]["outcome"] == "transcript"
        # A transcript or marker that predates the case cannot answer
        # for the current process.  The fingerprint remains unchanged,
        # so every outcome stays unresolved.
        for stale_suffix in ("", EMPTY_CAPTURE_SUFFIX):
            plan_env, root = plan_env_for_arm()
            for case_name in read_caselist(sdir / "fake.txt"):
                (root / f"p_{case_name}.t{stale_suffix}").write_text("stale\n")
            r = run(
                "per_case",
                "planning_capture_incomplete",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True,
                env=plan_env,
            )
            assert r["planning"]["outcomes"]["unresolved"] == 5
            assert all(
                not record["planning"]["transcripts"]
                and not record["planning"]["empty_markers"]
                for record in r["cases"].values()
            )
        # A gap in device ordinals and two outcomes for the same device
        # both leave the per-case capture unresolved.
        for planning_flag, expected_devices in (
            (
                "FAKE_PLAN_ORDINAL_GAP=1",
                {"0": "transcript", "2": "transcript"},
            ),
            ("FAKE_PLAN_CONFLICT=1", {"0": "conflict"}),
        ):
            plan_env, _ = plan_env_for_arm()
            r = run(
                "per_case",
                "planning_capture_incomplete",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True,
                env=plan_env + [planning_flag],
            )
            planning_record = r["cases"]["dEQP-VK.fake.a"]["planning"]
            assert planning_record["outcome"] == "unresolved"
            assert planning_record["device_outcomes"] == expected_devices
        # A terminal capture diagnostic invalidates an otherwise complete
        # ordinal family, and a case session that never closes cannot carry a
        # complete planning outcome.
        for planning_flag in (
            "FAKE_PLAN_WRITE_FAILURE=1",
            "FAKE_NO_END_SESSION_CASE=dEQP-VK.fake.a",
        ):
            plan_env, _ = plan_env_for_arm()
            r = run(
                "per_case",
                "planning_capture_incomplete",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True,
                env=plan_env + [planning_flag],
            )
            assert not r["planning"]["conditions"]["planning_outcomes_complete"]
        assert r["cases"]["dEQP-VK.fake.a"]["session_closed"] is False
        # Kernel continuity and hazard observations outrank the ordinary
        # incomplete-planning refusal while the partial shard remains
        # in the host-model evidence class.
        for kernel_flag, expected_verdict in (
            (
                "FAKE_BREAK_CONTINUITY_CASE=dEQP-VK.fake.b",
                "kernel_log_continuity_broken",
            ),
            ("FAKE_HAZARD_CASE=dEQP-VK.fake.b", "dmesg_hazard"),
        ):
            dmesg_file.write_text("[1.0] boot\n")
            plan_env, root = plan_env_for_arm()
            r = run(
                "per_case",
                expected_verdict,
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True,
                env=plan_env
                + [
                    f"FAKE_CAPTURE_FILE={root}/p_{{case}}.t",
                    kernel_flag,
                    f"FAKE_DMESG_FILE={dmesg_file}",
                ],
            )
            assert len(r["cases"]) == 2
            assert r["planning"]["disposition"] is None
            assert r["evidence_class"] == "host-model"
            assert not r["planning"]["conditions"]["every_shard_case_witnessed"]
        dmesg_file.write_text("[1.0] boot\n")
        # A device that captured no executable submission writes no
        # transcript and leaves its marker; the receipt records
        # no_nonempty_ib as a fresh per-device outcome.
        plan_env, root = plan_env_for_arm()
        r = run(
            "per_case",
            "pass",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=plan_env + ["FAKE_NO_IB_MARKER=1"],
        )
        assert r["evidence_class"] == PLANNING_EVIDENCE
        assert r["planning"]["outcomes"] == {
            "transcript": 0,
            "no_nonempty_ib": 5,
            "unresolved": 0,
        }
        assert (
            r["cases"]["dEQP-VK.fake.a"]["planning"]["empty_markers"]
            and (root / "p_dEQP-VK.fake.a.t").exists() is False
        )
        assert (
            r["cases"]["dEQP-VK.fake.a"]["paths"]["R3V_NATIVE_PLAN_CAPTURE_FILE"][
                "present_after"
            ]
            is False
        )
        # A log message carries no device ordinal and cannot replace the
        # fresh marker witness.
        plan_env, root = plan_env_for_arm()
        r = run(
            "per_case",
            "planning_capture_incomplete",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=plan_env + ["FAKE_NO_IB_MSG=1"],
        )
        assert r["planning"]["outcomes"]["unresolved"] == 5
        # A process that reports neither outcome also leaves the planning
        # capture incomplete.
        plan_env, _ = plan_env_for_arm()
        r = run(
            "per_case",
            "planning_capture_incomplete",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=plan_env,
        )
        assert r["planning"]["outcomes"]["unresolved"] == 5
        # Known-bad witness: one kernel-entering CS ioctl refuses the
        # disposition and the evidence class stays host-model.
        plan_env, root = plan_env_for_arm()
        r = run(
            "per_case",
            "planning_cs_witnessed",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=plan_env
            + [f"FAKE_CAPTURE_FILE={root}/p_{{case}}.t", "FAKE_STRACE_CS=1"],
        )
        assert (
            r["evidence_class"] == "host-model" and r["planning"]["disposition"] is None
        )
        assert r["planning"]["cs_witness"]["cs_ioctls"] == 5 and r["planning"][
            "refused_conditions"
        ] == ["kernel_entering_cs_zero"]
        assert r["qualification_valid"] is False
        # Each pre-run condition refuses by name.
        for extra, failed in (
            (["R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=1"], "submit_hazard_gate_closed"),
            ([f"R3V_NATIVE_MANIFEST_DIR={d}"], "attended_evidence_directory_absent"),
            ([f"R3V_NATIVE_PLAN_FILE={d}/x.plan"], "replay_plan_absent"),
            (["R3V_NATIVE_PLAN_NONCE=" + "0" * 32], "replay_plan_absent"),
        ):
            r = run(
                "per_case",
                "planning_disposition_refused",
                cases=sdir / "fake.txt",
                manifest_json=str(sdir / "partition_manifest.json"),
                process_per_case=True,
                env=plan_env + extra,
            )
            assert r["planning"]["refused_conditions"] == [failed], (
                extra,
                r["planning"]["refused_conditions"],
            )
            assert "argv" not in r and r["evidence_class"] == "host-model"
        # A zero-valued gate is closed.
        plan_env, root = plan_env_for_arm()
        r = run(
            "per_case",
            "pass",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=plan_env
            + [
                f"FAKE_CAPTURE_FILE={root}/p_{{case}}.t",
                "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED=0",
            ],
        )
        assert r["evidence_class"] == PLANNING_EVIDENCE
        # A preload other than the radeon drm-shim is host-model without
        # the interposer the capture session needs.
        r = run(
            "per_case",
            "planning_disposition_refused",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=[
                f"LD_PRELOAD={alternate_shim}",
                f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/q_{{case}}.t",
            ],
        )
        assert r["planning"]["refused_conditions"] == [
            "radeon_drm_shim_interposes_ioctl"
        ]
        # One process for the whole shard gives no per-case capture.
        r = run(
            "per_case",
            "planning_disposition_refused",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            env=shim_env + [f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/one.t"],
        )
        assert r["planning"]["refused_conditions"] == ["one_process_per_case"]
        # No usable tracer leaves the CS count unwitnessed, which refuses.
        r = run(
            "per_case",
            "planning_witness_unavailable",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=plan_env,
            strace_binary=str(d / "absent-strace"),
        )
        assert r["planning"]["tracer"]["binary"] is None and "argv" not in r
        # The same slice without a capture file stays below its required
        # evidence.
        r = run(
            "per_case",
            "evidence_below_required",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            env=base_env,
        )
        assert not r["planning"]["candidate"] and "argv" not in r
        # A capture file on a silicon run refuses: a capture session
        # opens the CS ioctl with the hazard gate closed, which the
        # drm-shim host model alone answers.
        r = run(
            "per_case",
            "capture_on_silicon",
            cases=sdir / "fake.txt",
            manifest_json=str(sdir / "partition_manifest.json"),
            process_per_case=True,
            force_evidence="silicon",
            env=base_env + [f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/s_{{case}}.t"],
        )
        assert not r["planning"]["candidate"] and "argv" not in r
        # The submission-gate allowlist holds under templating: a
        # templated plan value on a hazard-free slice is contamination.
        r = run(
            "per_case",
            "gate_contamination",
            cases=pdir / "fake.txt",
            manifest_json=mj,
            process_per_case=True,
            env=base_env + [f"R3V_NATIVE_PLAN_CAPTURE_FILE={caps}/{{case}}.t"],
        )
        assert "argv" not in r and r["contaminating_gates"] == [
            "R3V_NATIVE_PLAN_CAPTURE_FILE"
        ]
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
    print(
        "selftest: pass, mixed (NotSupported never a pass; Fail "
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
        "verdict"
    )


def _run_with_evidence(args, forced, receipt_path):
    """The evidence class a run would carry on other hardware; the
    selftest host answers no RS4xx render node, so the silicon branch
    is reachable only by naming the class the classifier would give."""
    global evidence_class
    orig = evidence_class

    def forced_class(env, host, icd, preload):
        return forced, None

    evidence_class = forced_class
    try:
        receipt = execute(args)
        verify_receipt(receipt_path)
        return receipt
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
    r.add_argument("--ad-hoc-hazard", choices=("none", "submission", "display"))
    r.add_argument("--process-per-case", action="store_true")
    r.add_argument("--plan-nonce-file")
    r.add_argument("--strace-binary")
    r.add_argument("--shutdown-timeout", type=float, default=5.0)
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
            check_ledgers(
                args.nonpass_ledger,
                args.slices,
                os.environ.get("R3V_DEQP_MUSTPASS_DIR"),
            )
        elif args.cmd == "verify-receipt":
            verify_receipt(args.receipt)
        else:
            receipt = execute(args)
            sys.exit(
                0 if receipt["verdict"] in ("pass", "pass_with_accepted_nonpass") else 1
            )
    except RunnerRefusal as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
