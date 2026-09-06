#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Holds every registered R300 test's verdicts active under its own build.

assert(expr) expands to ((void)0) where NDEBUG stands, discarding the
call inside it, so a test whose verdicts live in assert() executes
nothing and reports success under a release profile.  Two files in this
tree shipped that way.

The audit reads the preprocessed translation unit produced by the
target's own compile command out of compile_commands.json, so it judges
what the compiler judged.  A source that calls assert must leave the
assertion machinery in its preprocessed output; one that carries
"#undef NDEBUG" after including <assert.h> does not, and a text search
cannot tell the two apart.

The audited set comes from the Meson introspection data -- registered
tests, their executable targets, and those targets' C sources under the
R300 tree -- rather than from a rule about how many assertions a file
holds, so a test whose single assertion is its whole verdict is covered.
That same inventory answers how many tests a profile registers, which is
where a run's totals come from.
"""

import json
import os
import shlex
import subprocess
import sys

# The tree this qualification covers, and inside it the test translation
# units alone.  A driver source linked into a test binary keeps the
# NDEBUG setting its profile gives it -- a release driver that aborted on
# an internal assertion would be a different driver from the one under
# test -- so the audit judges the files that carry verdicts, which this
# tree keeps under a tests directory.
SCOPE = os.path.join("src", "amd", "r300")
TEST_SOURCE_DIRECTORY = "tests"
# The calibration pair, audited as fixtures rather than as tests: the
# late-undef file must be refused and the early-undef file admitted.
FIXTURE_REFUSED = "r3v_ndebug_fixture_late_undef.c"
FIXTURE_ADMITTED = "r3v_ndebug_fixture_early_undef.c"
# glibc routes a live assertion to this symbol, and it survives
# preprocessing as a declaration and a call.  A translation unit whose
# asserts expanded to ((void)0) carries neither.
ASSERT_MACHINERY = "__assert_fail"


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def load(builddir, name):
    path = os.path.join(builddir, "meson-info", name)
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def compile_commands(builddir):
    path = os.path.join(builddir, "compile_commands.json")
    with open(path, encoding="utf-8") as handle:
        entries = json.load(handle)
    table = {}
    for entry in entries:
        source = os.path.normpath(
            os.path.join(entry.get("directory", builddir), entry["file"]))
        table.setdefault(source, entry)
    return table


def preprocess(entry, builddir, override_source=None):
    """Runs the recorded compile command as a preprocess and returns its
    output.  The command's own flags travel, so the verdict is the one
    the profile's compiler reached."""
    command = entry.get("command")
    arguments = (shlex.split(command) if command is not None
                 else list(entry["arguments"]))
    out = []
    skip = False
    for index, argument in enumerate(arguments):
        if skip:
            skip = False
            continue
        if argument in ("-o", "-MF", "-MQ", "-MT"):
            skip = True
            continue
        if argument in ("-c", "-MD", "-MMD"):
            continue
        if (override_source is not None and index == len(arguments) - 1):
            out.append(override_source)
            continue
        out.append(argument)
    out.append("-E")
    result = subprocess.run(out, cwd=entry.get("directory", builddir),
                            capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        return None, result.stderr
    return result.stdout, None


def code_text(text):
    """The source with comments and string literals removed, so a
    mention of assert in prose is not read as a call.  Two files in this
    tree describe assert in their comments and stake no verdict on it."""
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        pair = text[index:index + 2]
        if pair == "/*":
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
            out.append(" ")
        elif pair == "//":
            end = text.find("\n", index + 2)
            index = length if end < 0 else end
            out.append(" ")
        elif char in "\"'":
            quote = char
            index += 1
            while index < length and text[index] != quote:
                index += 2 if text[index] == "\\" else 1
            index += 1
            out.append(" ")
        else:
            out.append(char)
            index += 1
    return "".join(out)


def verdicts_are_active(source, entry, builddir, override_source=None):
    """True when the source calls assert and its expansion survives, or
    when it calls assert nowhere and therefore stakes no verdict on it."""
    path = override_source if override_source is not None else source
    with open(path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if "assert(" not in code_text(text):
        return True, "stakes no verdict on assert"
    output, error = preprocess(entry, builddir, override_source)
    if output is None:
        return None, f"the compile command did not preprocess: {error}"
    if ASSERT_MACHINERY in output:
        return True, "the assertion machinery survives preprocessing"
    return False, ("every assert expands to ((void)0), so the calls inside "
                   "them never run and the test judges nothing")


def registered_tests(builddir):
    """The profile's registered tests, by name.  A run's totals and a
    difference between two profiles both come from here, so a summary
    and a comparison cannot disagree about what was registered."""
    return {test["name"] for test in load(builddir, "intro-tests.json")}


def outcomes(builddir):
    """Each registered test's recorded outcome from the profile's own
    testlog, so a reported vector is read rather than transcribed."""
    path = os.path.join(builddir, "meson-logs", "testlog.json")
    table = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            # A log entry names its suite as "<suite> - <project>:<test>",
            # while the introspection data names the test alone.
            name = record["name"]
            if ":" in name:
                name = name.rsplit(":", 1)[1]
            table[name] = record["result"]
    return table


def summarize(builddir):
    registered = registered_tests(builddir)
    recorded = outcomes(builddir)
    selected = {name: result for name, result in recorded.items()
                if name in registered}
    tally = {}
    for result in selected.values():
        tally[result] = tally.get(result, 0) + 1
    total = sum(tally.values())
    print(f"registered {len(registered)} selected {total}")
    for result in sorted(tally):
        print(f"  {result} {tally[result]}")
    print(f"  SUM {total}")
    return registered, selected


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--summary":
        summarize(sys.argv[2])
        return
    if len(sys.argv) == 4 and sys.argv[1] == "--difference":
        left_registered, left = summarize(sys.argv[2])
        right_registered, right = summarize(sys.argv[3])
        only_left = sorted(left_registered - right_registered)
        only_right = sorted(right_registered - left_registered)
        for name in only_left:
            print(f"only in {sys.argv[2]}: {name}")
        for name in only_right:
            print(f"only in {sys.argv[3]}: {name}")
        for name in sorted(set(left) & set(right)):
            if left[name] != right[name]:
                print(f"differs {name}: {left[name]} against {right[name]}")
        return
    if len(sys.argv) != 3:
        fail("usage: r3v_release_verdict_audit.py <builddir> <source-root>\n"
             "       r3v_release_verdict_audit.py --summary <builddir>\n"
             "       r3v_release_verdict_audit.py --difference <a> <b>")
    builddir, source_root = sys.argv[1], sys.argv[2]
    tests = load(builddir, "intro-tests.json")
    targets = load(builddir, "intro-targets.json")
    table = compile_commands(builddir)

    # The registered tests' executables, and those executables' C sources
    # inside the audited tree.
    executables = set()
    for test in tests:
        for item in test.get("cmd", []):
            executables.add(os.path.normpath(item))
    audited = {}
    for target in targets:
        filenames = {os.path.normpath(f) for f in target.get("filename", [])}
        if not filenames & executables:
            continue
        for source_group in target.get("target_sources", []):
            for source in source_group.get("sources", []):
                source = os.path.normpath(source)
                if not source.endswith(".c"):
                    continue
                if SCOPE not in source:
                    continue
                parts = os.path.normpath(source).split(os.sep)
                if TEST_SOURCE_DIRECTORY not in parts[:-1]:
                    continue
                audited[source] = target["name"]

    fixtures = {}
    for source in list(table):
        base = os.path.basename(source)
        if base in (FIXTURE_REFUSED, FIXTURE_ADMITTED):
            fixtures[base] = source
            audited.pop(source, None)

    # The gate calibrates before it judges: a pair of independently built
    # fixtures whose verdicts differ only in where NDEBUG is undefined.
    for base, expected in ((FIXTURE_REFUSED, False), (FIXTURE_ADMITTED, True)):
        source = fixtures.get(base)
        if source is None:
            fail(f"the calibration fixture {base} is not built, so the "
                 f"audit cannot show it discriminates")
        active, why = verdicts_are_active(source, table[source], builddir)
        if active is None:
            fail(f"calibration fixture {base}: {why}")
        if active is not expected:
            fail(f"calibration fixture {base} is {'admitted' if active else 'refused'}"
                 f", and the audit requires the opposite: {why}")
    print(f"calibration: {FIXTURE_REFUSED} refused, {FIXTURE_ADMITTED} "
          f"admitted")

    if not audited:
        fail(f"the audit found no registered test source under {SCOPE}; an "
             f"empty audited set proves nothing")
    print(f"audited {len(audited)} test translation unit(s) under {SCOPE}")

    inert = []
    unreadable = []
    for source in sorted(audited):
        entry = table.get(source)
        if entry is None:
            unreadable.append((source, "no compile command"))
            continue
        active, why = verdicts_are_active(source, entry, builddir)
        if active is None:
            unreadable.append((source, why))
        elif not active:
            inert.append((source, audited[source], why))

    for source, why in unreadable:
        print(f"UNREADABLE {os.path.relpath(source, source_root)}: {why}",
              file=sys.stderr)
    for source, target, why in inert:
        print(f"INERT {os.path.relpath(source, source_root)} (target "
              f"{target}): {why}", file=sys.stderr)
    if unreadable:
        fail(f"{len(unreadable)} audited source(s) could not be judged")
    if inert:
        fail(f"{len(inert)} registered test source(s) stake their verdicts "
             f"on assertions their own build discards")
    print(f"release verdicts: {len(audited)} registered R300 test source(s) "
          f"keep their verdicts active under this profile")


if __name__ == "__main__":
    main()
