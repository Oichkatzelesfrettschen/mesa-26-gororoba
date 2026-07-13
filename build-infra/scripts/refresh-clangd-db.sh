#!/bin/sh
# Refresh the repo-stable clangd compilation database from the newest built
# profile. .clangd points at build/clangd-db so worktrees can borrow a build
# database while clangd still associates source files with the worktree.
#
# Scans build-infra profile directories (build/mesa-*) and standalone Meson
# directories (builddir, builddir-*). A borrowed database retains its build
# directory and generated-file paths. Source files under SRC_REPO are re-rooted to the checkout running this
# script, and compile command / -I paths that name that source tree are
# re-rooted the same way so clangd does not parse worktree .c against
# SRC_REPO headers.
set -eu

repo_root=$(git rev-parse --show-toplevel)
repo_root=$(cd "$repo_root" && pwd -P)
src_repo=${SRC_REPO:-$repo_root}
src_repo=$(cd "$src_repo" && pwd -P)

newest=$(for candidate in "$src_repo"/build/mesa-*/compile_commands.json \
                         "$src_repo"/builddir/compile_commands.json \
                         "$src_repo"/builddir-*/compile_commands.json; do
   [ -f "$candidate" ] || continue
   printf '%s:%s\n' "$(stat -c %Y "$candidate")" "$candidate"
done | sort -nr | sed -n '1s/^[^:]*://p')
[ -n "$newest" ] || {
   echo "no built compile_commands.json found (build a profile or run meson setup builddir)" >&2
   exit 1
}

mkdir -p "$repo_root/build/clangd-db"
output="$repo_root/build/clangd-db/compile_commands.json"

if [ "$src_repo" = "$repo_root" ]; then
   cp "$newest" "$output"
   echo "clangd-db <- $newest"
   exit 0
fi

python3 - "$src_repo" "$repo_root" "$newest" "$output" <<'PYTHON'
import json
import os
import sys
from pathlib import Path

source_root = Path(sys.argv[1]).resolve()
worktree_root = Path(sys.argv[2]).resolve()
database_path = Path(sys.argv[3]).resolve()
output_path = Path(sys.argv[4])

try:
    with database_path.open(encoding="utf-8") as source:
        entries = json.load(source)
except (OSError, json.JSONDecodeError) as error:
    sys.exit("cannot read compile database %s: %s" % (database_path, error))

if not isinstance(entries, list):
    sys.exit("compile database %s must contain a JSON list" % database_path)

def source_relative(path):
    try:
        return path.relative_to(source_root)
    except ValueError:
        return None

def generated_build_path(path):
    try:
        path.relative_to(database_path.parent)
    except ValueError:
        return False
    return True

def reroot_text(text, build_directory):
    """Rewrite absolute source-tree path mentions to the worktree.

    Paths under the borrowed build directory stay unchanged so generated
    includes (-I$build/generated) keep pointing at the Meson output.
    """
    if not isinstance(text, str) or not text:
        return text, 0
    build_resolved = Path(build_directory).resolve()
    source_prefix = str(source_root.resolve()) + os.sep
    worktree_prefix = str(worktree_root.resolve()) + os.sep
    build_prefix = str(build_resolved) + os.sep
    changed = 0
    # Token-split on spaces so we can skip build-dir absolute paths.
    parts = text.split(" ")
    out = []
    for part in parts:
        if part.startswith(build_prefix) or part == str(build_resolved):
            out.append(part)
            continue
        if part.startswith(source_prefix):
            out.append(worktree_prefix + part[len(source_prefix):])
            changed += 1
            continue
        # -I/abs/source/... or -isystem/abs/source/...
        for flag in ("-I", "-isystem"):
            if part.startswith(flag) and len(part) > len(flag):
                path_part = part[len(flag):]
                if path_part.startswith(build_prefix):
                    out.append(part)
                    break
                if path_part.startswith(source_prefix):
                    out.append(flag + worktree_prefix + path_part[len(source_prefix):])
                    changed += 1
                    break
        else:
            out.append(part)
    return " ".join(out), changed

def reroot_path_token(token, build_directory):
    path = Path(token)
    if not path.is_absolute():
        # -I../src or bare ../src/foo.c relative to the borrowed build dir
        candidate = (build_directory / path).resolve()
    else:
        candidate = path
    relative = source_relative(candidate)
    if relative is None or generated_build_path(candidate):
        return token, 0
    return str(worktree_root / relative), 1

rewritten = 0
for index, entry in enumerate(entries):
    if not isinstance(entry, dict):
        sys.exit("compile database entry %d is not an object" % index)
    directory = entry.get("directory")
    file_name = entry.get("file")
    if not isinstance(directory, str) or not isinstance(file_name, str):
        sys.exit("compile database entry %d lacks string directory or file" % index)

    build_directory = Path(directory)
    source_file = Path(file_name)
    if not source_file.is_absolute():
        source_file = build_directory / source_file
    source_file = source_file.resolve()
    relative = source_relative(source_file)
    if relative is None or generated_build_path(source_file):
        continue

    entry["file"] = str(worktree_root / relative)
    rewritten += 1

    # Keep command / arguments consistent with the re-rooted file: absolute
    # source-root paths and -I/-isystem paths under SRC_REPO move to the worktree.
    # Generated build-dir includes stay borrowed.
    command = entry.get("command")
    if isinstance(command, str):
        new_command, n = reroot_text(command, build_directory)
        if n:
            entry["command"] = new_command

    arguments = entry.get("arguments")
    if isinstance(arguments, list):
        new_args = []
        for token in arguments:
            if not isinstance(token, str):
                new_args.append(token)
                continue
            if token.startswith("-I") or token.startswith("-isystem"):
                flag = "-I" if token.startswith("-I") else "-isystem"
                path_part = token[len(flag):]
                # GNU accepts -Ipath; empty path_part means a split -I <path> form
                # handled as a bare token on a later iteration.
                if path_part:
                    new_path, n = reroot_path_token(path_part, build_directory)
                    new_args.append(flag + new_path if n else token)
                else:
                    new_args.append(token)
            elif token.startswith(str(source_root)):
                new_path, n = reroot_path_token(token, build_directory)
                new_args.append(new_path if n else token)
            else:
                new_path, n = reroot_path_token(token, build_directory)
                # only rewrite when the token is clearly a source path
                if n and (token.endswith(".c") or token.endswith(".cpp") or
                          token.endswith(".cc") or token.endswith(".cxx") or
                          token.endswith(".h") or "/" in token or token.startswith("..")):
                    new_args.append(new_path)
                else:
                    new_args.append(token)
        entry["arguments"] = new_args

temporary_output = output_path.with_name(output_path.name + ".tmp")
with temporary_output.open("w", encoding="utf-8") as output:
    json.dump(entries, output, indent=2)
    output.write("\n")
os.replace(temporary_output, output_path)
print("clangd-db <- %s (%d source entries re-rooted)" % (database_path, rewritten))
PYTHON
