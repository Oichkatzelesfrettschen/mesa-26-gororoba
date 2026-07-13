#!/bin/sh
# Refresh the repo-stable clangd compilation database from the newest built
# profile. .clangd points at build/clangd-db so worktrees can borrow a build
# database while clangd still associates source files with the worktree.
#
# Scans build-infra profile directories (build/mesa-*) and standalone Meson
# directories (builddir, builddir-*). A borrowed database retains its build
# directory and generated-file paths. Only source files under SRC_REPO are
# re-rooted to the checkout running this script.
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

temporary_output = output_path.with_name(output_path.name + ".tmp")
with temporary_output.open("w", encoding="utf-8") as output:
    json.dump(entries, output, indent=2)
    output.write("\n")
os.replace(temporary_output, output_path)
print("clangd-db <- %s (%d source entries re-rooted)" % (database_path, rewritten))
PYTHON
