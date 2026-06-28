#!/bin/sh
# Refresh the repo-stable clangd compilation database from the newest built
# profile.  .clangd points at build/clangd-db so the language server keeps
# working across profile reconfigures and inside git worktrees (run this
# after `git worktree add`, pointing SRC_REPO at the main checkout).
#
# Scans both the build-infra profile directories (build/mesa-*) and the
# documented standalone Meson directories (builddir, builddir-*): `meson setup
# builddir` writes builddir/compile_commands.json, and a developer building a
# single profile there must still get a fresh clangd database or the language
# server serves stale diagnostics (a member that a reverted edit removed keeps
# erroring) against the current source.  The newest database across all of them
# wins, so clangd always reflects the most recently built tree.
set -eu
repo_root=$(git rev-parse --show-toplevel)
src_repo="${SRC_REPO:-$repo_root}"

newest=$(ls -t "$src_repo"/build/mesa-*/compile_commands.json \
                "$src_repo"/builddir/compile_commands.json \
                "$src_repo"/builddir-*/compile_commands.json \
                2>/dev/null | head -1)
[ -n "$newest" ] || { echo "no built compile_commands.json found (build a profile or run meson setup builddir)" >&2; exit 1; }

mkdir -p "$repo_root/build/clangd-db"
# When SRC_REPO points at a different checkout (a git worktree borrowing the
# main tree's build), rewrite the database's "directory" fields to this repo so
# clangd resolves relative paths against the worktree it is serving.
if [ "$src_repo" != "$repo_root" ]; then
   sed "s#\"$src_repo#\"$repo_root#g" "$newest" \
      > "$repo_root/build/clangd-db/compile_commands.json"
else
   cp "$newest" "$repo_root/build/clangd-db/compile_commands.json"
fi
echo "clangd-db <- $newest"
