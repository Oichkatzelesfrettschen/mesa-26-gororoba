#!/bin/sh
# Name the one package file a recipe just produced.  `makepkg --packagelist`
# reads the recipe's own pkgname, epoch, pkgver, pkgrel, and arch and prints
# the paths it writes, so the answer comes from the recipe rather than from a
# directory listing: a repository directory accumulates every package ever
# built there, and a glob over it hands `repo-add` the whole history in
# lexicographic order, leaving the database naming whichever version sorts
# last.  A debug companion package prints beside the primary one and is
# dropped here; anything other than exactly one remaining path refuses, so a
# recipe that splits into several packages is a decision the caller makes
# rather than a selection made silently.
set -u

usage() {
  echo "usage: select-built-package.sh <recipe-directory>" >&2
  exit 2
}

[ $# -eq 1 ] || usage
recipe_dir=$1

[ -f "$recipe_dir/PKGBUILD" ] || {
  echo "select-built-package: $recipe_dir holds no PKGBUILD" >&2
  exit 2
}

listing=$(cd "$recipe_dir" && makepkg --packagelist) || {
  echo "select-built-package: makepkg --packagelist failed in $recipe_dir" >&2
  exit 2
}

primary=$(printf '%s\n' "$listing" | grep -v -- '-debug-[^/]*$' | grep .)
count=$(printf '%s\n' "$primary" | grep -c .)
if [ "$count" -ne 1 ]; then
  echo "select-built-package: $recipe_dir names $count primary packages; the repository entry needs exactly one" >&2
  exit 2
fi

[ -f "$primary" ] || {
  echo "select-built-package: $primary was not built" >&2
  exit 2
}

printf '%s\n' "$primary"
