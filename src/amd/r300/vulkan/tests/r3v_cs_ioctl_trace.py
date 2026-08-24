# SPDX-License-Identifier: MIT
#
# Independent witness that a run entered the kernel with no command
# submission: strace records every ioctl the tracee issues as a syscall,
# and the parser counts them by request number, so a hazard-free slice
# run carries a machine-checked zero for DRM_IOCTL_RADEON_CS beside the
# driver's own refusal.
#
# The witness is bounded at the syscall boundary.  The radeon drm-shim
# interposes ioctl() in the tracee's own address space and answers every
# call on a shim file descriptor from user space, so a run against the
# shim issues no DRM ioctl syscall at all and a syscall tracer records
# none -- the submitting arm of the submit-order harness reports one
# DRM_RADEON_CS through the transport's own ioctl table while this tool
# reports zero.  Every syscall-boundary instrument shares that blindness,
# ltrace's PLT interception is defeated by the same preload, and a
# counter ahead of the shim in the preload order is what witnesses an
# absorbed call.  On a host-model run this tool's zero therefore states
# that nothing reached the kernel, which is the property a hazard-free
# slice must hold; on silicon, where every submission is a real syscall,
# the same zero states that no submission happened.

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# _IOC packs direction, type, command number, and argument size into the
# request word, so a number derives from the header constants alone:
# DRM_COMMAND_BASE 0x40 (include/drm-uapi/drm.h) plus the radeon command
# number (include/drm-uapi/radeon_drm.h) gives the _IOC command byte
# under type 'd' = 0x64, and the ioctl struct's size fills the size
# field.  DRM_RADEON_CS 0x26 lands at _IOWR('d', 0x66,
# struct drm_radeon_cs) = 0xc0206466.
DRM_COMMAND_BASE = 0x40
IOC_TYPE = ord('d')

IOC_WRITE, IOC_READ = 1, 2
IOC_NRSHIFT, IOC_TYPESHIFT, IOC_SIZESHIFT, IOC_DIRSHIFT = 0, 8, 16, 30


def ioc(direction, command, size):
    return ((direction << IOC_DIRSHIFT) | (IOC_TYPE << IOC_TYPESHIFT) |
            ((DRM_COMMAND_BASE + command) << IOC_NRSHIFT) |
            (size << IOC_SIZESHIFT))


# name -> (radeon command number, direction, sizeof(struct), request).
# The sizes are the ioctl structs in include/drm-uapi/radeon_drm.h on a
# 64-bit host, and the `derivation` subcommand prints the whole
# construction.
REQUESTS = {}
for _name, _command, _direction, _size in (
    ("DRM_IOCTL_RADEON_CS", 0x26, IOC_READ | IOC_WRITE, 32),
    ("DRM_IOCTL_RADEON_GEM_CREATE", 0x1d, IOC_READ | IOC_WRITE, 32),
    ("DRM_IOCTL_RADEON_GEM_MMAP", 0x1e, IOC_READ | IOC_WRITE, 32),
    ("DRM_IOCTL_RADEON_GEM_WAIT_IDLE", 0x24, IOC_WRITE, 8),
    ("DRM_IOCTL_RADEON_INFO", 0x27, IOC_READ | IOC_WRITE, 16),
):
    REQUESTS[_name] = (_command, _direction, _size,
                       ioc(_direction, _command, _size))

CS_REQUEST = REQUESTS["DRM_IOCTL_RADEON_CS"][3]
GEM_REQUESTS = {name: entry[3] for name, entry in REQUESTS.items()
                if name != "DRM_IOCTL_RADEON_CS"}
REQUEST_NAMES = {entry[3]: name for name, entry in REQUESTS.items()}

# strace prints the request word as a bare hex literal under
# `-e raw=ioctl`; without it the symbolic name is printed, and a number
# several drivers share prints as an ambiguous alternation, so the raw
# qualifier is what makes the count exact.  A call the tracer splits
# across a context switch carries its request on the entry line and
# `<... ioctl resumed>` on the return line, so matching the request word
# counts each call once.
IOCTL_LINE = re.compile(
    r"^(?:\d+\s+)?ioctl\((?:0x[0-9a-fA-F]+|-?\d+),\s*"
    r"(0x[0-9a-fA-F]+|\d+)[,)]")

STRACE_ARGS = ["-f", "-qq", "-e", "raw=ioctl", "-e", "trace=ioctl"]

# meson reads exit 77 as a skip.
EXIT_SKIP = 77


def derivation_lines():
    lines = [
        "_IOC(dir, type, nr, size) packs dir<<30 | size<<16 | type<<8 | nr;",
        "radeon ioctls take type 'd' = 0x%02x and nr = DRM_COMMAND_BASE "
        "0x%02x + command." % (IOC_TYPE, DRM_COMMAND_BASE),
    ]
    for name, (command, direction, size, request) in REQUESTS.items():
        letters = "".join(letter for bit, letter in
                          ((IOC_READ, "R"), (IOC_WRITE, "W"))
                          if direction & bit)
        lines.append(
            "%-31s dir %s size %2d nr 0x%02x+0x%02x=0x%02x -> 0x%08x"
            % (name, letters, size, DRM_COMMAND_BASE, command,
               DRM_COMMAND_BASE + command, request))
    return lines


def strace_available():
    """Report the strace binary and whether it can actually trace.

    A binary on PATH is not a usable tracer: kernel.yama.ptrace_scope and
    a container without CAP_SYS_PTRACE both deny the attach, so the probe
    traces `true` and reads the exit status.
    """
    binary = shutil.which("strace")
    if binary is None:
        return None, "strace is absent from PATH"
    probe = subprocess.run([binary, "-o", os.devnull, "-qq", "true"],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        return None, ("strace at %s cannot trace: %s"
                      % (binary, (probe.stderr or "").strip() or
                         "exit %d" % probe.returncode))
    return binary, None


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_strace(path):
    """Count ioctl requests by request number over an strace log."""
    counts = {}
    unparsed = 0
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            if not line.startswith("ioctl") and " ioctl(" not in line:
                continue
            if "resumed>" in line:
                continue
            match = IOCTL_LINE.match(line.strip())
            if match is None:
                unparsed += 1
                continue
            request = int(match.group(1), 0)
            counts[request] = counts.get(request, 0) + 1
    return counts, unparsed


def run_trace(argv, strace_binary, strace_path, environment=None):
    """Run argv under strace and return its exit status."""
    completed = subprocess.run(
        [strace_binary] + STRACE_ARGS + ["-o", strace_path, "--"] + argv,
        env=environment)
    return completed.returncode


def summarize(argv, strace_path, tracee_status):
    counts, unparsed = parse_strace(strace_path)
    by_request = {}
    for request, count in sorted(counts.items()):
        name = REQUEST_NAMES.get(request)
        by_request["0x%08x" % request] = {
            "count": count,
            "name": name,
        }
    cs_ioctls = counts.get(CS_REQUEST, 0)
    gem_ioctls = sum(counts.get(request, 0)
                     for request in GEM_REQUESTS.values())
    named = {name: counts.get(request, 0)
             for name, request in
             [("DRM_IOCTL_RADEON_CS", CS_REQUEST)] +
             sorted(GEM_REQUESTS.items())}
    return {
        "argv": argv,
        "cs_ioctls": cs_ioctls,
        "gem_ioctls": gem_ioctls,
        "total_ioctls": sum(counts.values()),
        "radeon_ioctls_by_name": named,
        "ioctls_by_request": by_request,
        "unparsed_ioctl_lines": unparsed,
        "tracee_exit_status": tracee_status,
        "strace_args": STRACE_ARGS,
        "strace_sha256": sha256_file(strace_path),
        "witness_scope": "syscall-entering ioctls only; a user-space "
                         "interposer that answers without a syscall is "
                         "invisible to this witness",
    }


def command_trace(args):
    strace_binary, reason = strace_available()
    if strace_binary is None:
        print("skip: %s" % reason, file=sys.stderr)
        return EXIT_SKIP
    if not args.argv:
        print("trace requires a command after --", file=sys.stderr)
        return 2

    holder = None
    if args.strace_out is not None:
        strace_path = args.strace_out
    else:
        holder = tempfile.TemporaryDirectory()
        strace_path = os.path.join(holder.name, "ioctl.strace")
    try:
        status = run_trace(args.argv, strace_binary, strace_path)
        summary = summarize(args.argv, strace_path, status)
        summary["expect_cs"] = args.expect_cs
        summary["verdict"] = ("witnessed"
                              if summary["cs_ioctls"] == args.expect_cs
                              else "refused")
        text = json.dumps(summary, indent=2, sort_keys=True)
        if args.json is not None:
            with open(args.json, "w") as handle:
                handle.write(text + "\n")
        print(text)
        if summary["verdict"] == "refused":
            print("FAIL: %d strace-visible DRM_IOCTL_RADEON_CS, expected %d"
                  % (summary["cs_ioctls"], args.expect_cs), file=sys.stderr)
            return 1
        return 0
    finally:
        if holder is not None:
            holder.cleanup()


def command_derivation(args):
    for line in derivation_lines():
        print(line)
    return 0


def emitter_source(request, repeats):
    """A program issuing `repeats` ioctls of one request on /dev/null.

    /dev/null answers ENOTTY, and the syscall is what the tracer records,
    so the emitter calibrates the parser against a known count with no
    device and no driver in the path.
    """
    return (
        "import fcntl, os\n"
        "fd = os.open('/dev/null', os.O_RDWR)\n"
        "for _ in range(%d):\n"
        "    try:\n"
        "        fcntl.ioctl(fd, %d, bytearray(64))\n"
        "    except OSError:\n"
        "        pass\n" % (repeats, request))


def trace_here(argv, strace_binary, work_dir, tag, environment=None,
               expect_cs=0):
    strace_path = os.path.join(work_dir, "%s.strace" % tag)
    status = run_trace(argv, strace_binary, strace_path, environment)
    summary = summarize(argv, strace_path, status)
    summary["expect_cs"] = expect_cs
    return summary


def report(tag, summary):
    print("  %-28s cs=%d gem=%d total=%d status=%d"
          % (tag, summary["cs_ioctls"], summary["gem_ioctls"],
             summary["total_ioctls"], summary["tracee_exit_status"]))


def command_selftest(args):
    strace_binary, reason = strace_available()
    if strace_binary is None:
        print("skip: %s" % reason, file=sys.stderr)
        return EXIT_SKIP

    tool = os.path.abspath(__file__)
    failures = []

    print("derivation:")
    for line in derivation_lines():
        print("  " + line)
    if CS_REQUEST != 0xc0206466:
        failures.append("DRM_IOCTL_RADEON_CS derived 0x%08x" % CS_REQUEST)

    with tempfile.TemporaryDirectory() as work_dir:
        # The emitters calibrate the parser against real syscalls, so
        # they run with no interposer in the preload path.
        plain = dict(os.environ)
        plain.pop("LD_PRELOAD", None)
        print("parser calibration:")
        # Known-bad: three real CS-numbered syscalls.  The parser must
        # count exactly three, the default verdict must refuse them, and
        # --expect-cs 3 must admit the same run.
        bad = trace_here(
            [sys.executable, "-c", emitter_source(CS_REQUEST, 3)],
            strace_binary, work_dir, "known-bad", plain)
        report("known-bad emitter", bad)
        if bad["cs_ioctls"] != 3:
            failures.append("known-bad emitter counted %d CS ioctls, "
                            "expected 3" % bad["cs_ioctls"])
        if bad["unparsed_ioctl_lines"] != 0:
            failures.append("known-bad emitter left %d unparsed ioctl lines"
                            % bad["unparsed_ioctl_lines"])

        verdict = subprocess.run(
            [sys.executable, tool, "trace", "--",
             sys.executable, "-c", emitter_source(CS_REQUEST, 3)],
            env=plain, capture_output=True, text=True)
        if verdict.returncode == 0:
            failures.append("the default verdict admitted three CS ioctls")
        admitted = subprocess.run(
            [sys.executable, tool, "trace", "--expect-cs", "3", "--",
             sys.executable, "-c", emitter_source(CS_REQUEST, 3)],
            env=plain, capture_output=True, text=True)
        if admitted.returncode != 0:
            failures.append("--expect-cs 3 refused three CS ioctls")

        # Known-good: two GEM_CREATE-numbered syscalls and no CS.
        good = trace_here(
            [sys.executable, "-c",
             emitter_source(REQUESTS["DRM_IOCTL_RADEON_GEM_CREATE"][3], 2)],
            strace_binary, work_dir, "known-good", plain)
        report("known-good emitter", good)
        if good["cs_ioctls"] != 0 or good["gem_ioctls"] != 2:
            failures.append("known-good emitter counted cs=%d gem=%d, "
                            "expected cs=0 gem=2"
                            % (good["cs_ioctls"], good["gem_ioctls"]))

        if args.harness is None:
            print("shim calibration: not run (no --harness)")
        else:
            print("shim calibration (LD_PRELOAD=%s):"
                  % os.path.basename(args.shim or ""))
            environment = dict(os.environ)
            if args.shim is not None:
                environment["LD_PRELOAD"] = args.shim
                environment["DRM_SHIM_EXPECTED_DSO"] = args.shim
                environment["RADEON_GPU_ID"] = "0x5974"
            # The harness builds its own evidence directory per arm, so
            # an inherited one would redirect the retention the arm
            # asserts over.
            environment.pop("R3V_NATIVE_MANIFEST_DIR", None)
            closed = trace_here([args.harness, "gate-closed"], strace_binary,
                                work_dir, "gate-closed", environment)
            armed = trace_here([args.harness, "armed"], strace_binary,
                               work_dir, "armed", environment)
            report("harness gate-closed", closed)
            report("harness armed", armed)
            if closed["tracee_exit_status"] != 0:
                failures.append("the gate-closed arm exited %d"
                                % closed["tracee_exit_status"])
            if armed["tracee_exit_status"] != 0:
                failures.append("the armed arm exited %d"
                                % armed["tracee_exit_status"])
            # The armed arm drives one DRM_RADEON_CS through the
            # transport's ioctl table into the drm-shim, which answers it
            # -- and every GEM call beside it -- in the tracee's own
            # address space, so the run performs no DRM ioctl syscall and
            # the tracer records none.  Both shim arms therefore hold a
            # syscall-visible zero across the whole radeon request set,
            # and the harness's in-process counter is what separates
            # them.  Pinning both arms holds the scope of the witness:
            # an arm that starts entering the kernel breaks this.
            for tag, summary in (("gate-closed", closed), ("armed", armed)):
                if summary["cs_ioctls"] != 0 or summary["gem_ioctls"] != 0:
                    failures.append(
                        "the %s arm entered the kernel with cs=%d gem=%d; "
                        "the shim answers the radeon request set in the "
                        "tracee's address space"
                        % (tag, summary["cs_ioctls"], summary["gem_ioctls"]))
            print("  the shim answers the radeon request set in the "
                  "tracee's address space, so both arms hold a")
            print("  syscall-visible zero while the armed arm's own "
                  "counter reports one DRM_RADEON_CS")

    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("r3v-cs-ioctl-trace-selftest: PASS")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Witness DRM_IOCTL_RADEON_CS syscalls under strace.")
    commands = parser.add_subparsers(dest="command", required=True)

    trace = commands.add_parser(
        "trace", help="trace a command and count radeon ioctls")
    trace.add_argument("--expect-cs", type=int, default=0,
                       help="the exact CS ioctl count the run must hold")
    trace.add_argument("--json", help="write the JSON summary to this path")
    trace.add_argument("--strace-out",
                       help="retain the raw strace log at this path")
    trace.add_argument("argv", nargs=argparse.REMAINDER)
    trace.set_defaults(handler=command_trace)

    derivation = commands.add_parser(
        "derivation", help="print the ioctl request-number derivation")
    derivation.set_defaults(handler=command_derivation)

    selftest = commands.add_parser(
        "selftest", help="calibrate the parser and the shim arms")
    selftest.add_argument("--harness",
                          help="the submit-order harness to trace")
    selftest.add_argument("--shim", help="the drm-shim to preload")
    selftest.set_defaults(handler=command_selftest)

    args = parser.parse_args()
    if getattr(args, "argv", None) and args.argv[0] == "--":
        args.argv = args.argv[1:]
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
