#!/bin/sh
# Refresh the repo-stable clangd compilation database from the newest built
# profile.  .clangd points at build/clangd-db so the language server keeps
# working across profile reconfigures and inside git worktrees (run this
# after `git worktree add`, pointing SRC_REPO at the main checkout).
set -eu
repo_root=$(git rev-parse --show-toplevel)
src_repo="${SRC_REPO:-$repo_root}"
newest=$(ls -t "$src_repo"/build/mesa-*/compile_commands.json 2>/dev/null | head -1)
[ -n "$newest" ] || { echo "no built profile database found" >&2; exit 1; }
mkdir -p "$repo_root/build/clangd-db"
cp "$newest" "$repo_root/build/clangd-db/compile_commands.json"
echo "clangd-db <- $newest"
