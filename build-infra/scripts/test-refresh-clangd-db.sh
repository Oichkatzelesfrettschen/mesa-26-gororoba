#!/bin/sh
# Fixture test for borrowed clangd compilation databases.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd -P)
REFRESH="$HERE/refresh-clangd-db.sh"
WORKDIR=$(mktemp -d "${TMPDIR:-/var/tmp}/refresh-clangd-db-test.XXXXXX")

cleanup() {
  case "$WORKDIR" in
    "${TMPDIR:-/var/tmp}"/refresh-clangd-db-test.*) rm -rf -- "$WORKDIR" ;;
    *) echo "refusing to remove unexpected test directory: $WORKDIR" >&2; exit 1 ;;
  esac
}
trap cleanup EXIT

SOURCE_REPO="$WORKDIR/source[repo]"
WORKTREE="$WORKDIR/work[tree]"
BUILD_DIR="$SOURCE_REPO/builddir"
mkdir -p "$BUILD_DIR/generated" "$SOURCE_REPO/src" "$WORKTREE/src"
: > "$SOURCE_REPO/src/main.c"
: > "$SOURCE_REPO/src/absolute.c"
: > "$BUILD_DIR/generated/config.h"
: > "$WORKTREE/src/main.c"
: > "$WORKTREE/src/absolute.c"
git -C "$WORKTREE" init -q

python3 - "$SOURCE_REPO" <<'PYTHON'
import json
import sys
from pathlib import Path

source_root = Path(sys.argv[1])
build_dir = source_root / "builddir"
entries = [
    {
        "directory": str(build_dir),
        "command": "cc -I%s -c ../src/main.c" % (build_dir / "generated"),
        "file": "../src/main.c",
        "output": "main.o",
    },
    {
        "directory": str(build_dir),
        "command": "cc -c %s" % (source_root / "src" / "absolute.c"),
        "file": str(source_root / "src" / "absolute.c"),
        "output": "absolute.o",
    },
    {
        "directory": str(build_dir),
        "command": "cc -c generated/config.h",
        "file": "generated/config.h",
        "output": "config.h.gch",
    },
]
with (build_dir / "compile_commands.json").open("w", encoding="utf-8") as output:
    json.dump(entries, output)
PYTHON

(
  cd "$WORKTREE"
  SRC_REPO="$SOURCE_REPO/" sh "$REFRESH"
)

python3 - "$WORKTREE/build/clangd-db/compile_commands.json" "$SOURCE_REPO" "$WORKTREE" <<'PYTHON'
import json
import sys
from pathlib import Path

database_path = Path(sys.argv[1])
source_root = Path(sys.argv[2])
worktree_root = Path(sys.argv[3])
with database_path.open(encoding="utf-8") as source:
    entries = json.load(source)

assert entries[0]["directory"] == str(source_root / "builddir")
assert entries[0]["file"] == str(worktree_root / "src" / "main.c")
assert entries[1]["directory"] == str(source_root / "builddir")
assert entries[1]["file"] == str(worktree_root / "src" / "absolute.c")
assert entries[2]["directory"] == str(source_root / "builddir")
assert entries[2]["file"] == "generated/config.h"
assert str(source_root / "builddir" / "generated") in entries[0]["command"]
PYTHON

echo "refresh-clangd-db fixtures: PASS"
