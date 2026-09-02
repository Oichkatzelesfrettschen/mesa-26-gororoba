#!/bin/sh
# Calibrate the built-package selection against a listing it must accept and
# four it must refuse.  The refusal arms replace makepkg on PATH with a stub
# that prints a chosen listing, so each arm exercises the selection rule
# itself rather than a recipe the tree cannot hold.
set -u
root=$(cd "$(dirname "$0")" && pwd)
select_built="$root/select-built-package.sh"
recipe="$root/mesa-gororoba"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0

fail() {
  echo "FAIL  $1" >&2
  status=1
}

# The stub recipe directory: a PKGBUILD the selection reads for its existence
# and a makepkg on PATH that prints the listing the arm is testing.
stub=$work/stub
mkdir -p "$stub" "$work/bin"
: > "$stub/PKGBUILD"
PATH="$work/bin:$PATH"
export PATH

stub_listing() {
  {
    echo '#!/bin/sh'
    echo 'cat <<LIST'
    printf '%s\n' "$1"
    echo 'LIST'
  } > "$work/bin/makepkg"
  chmod +x "$work/bin/makepkg"
}

# Accept: one primary package, and a debug companion beside it.
: > "$stub/pkg-1-x86_64.pkg.tar.zst"
stub_listing "$stub/pkg-1-x86_64.pkg.tar.zst"
chosen=$(sh "$select_built" "$stub") \
  && [ "$chosen" = "$stub/pkg-1-x86_64.pkg.tar.zst" ] \
  || fail "the single-package listing was not selected"

stub_listing "$stub/pkg-1-x86_64.pkg.tar.zst
$stub/pkg-debug-1-x86_64.pkg.tar.zst"
chosen=$(sh "$select_built" "$stub") \
  && [ "$chosen" = "$stub/pkg-1-x86_64.pkg.tar.zst" ] \
  || fail "the debug companion was not dropped"

# Each refusal names the invariant it enforced, so an arm that refuses for a
# neighboring reason fails here instead of passing on the exit status alone.
refuses() {
  name=$1
  dir=$2
  expected=$3
  if sh "$select_built" "$dir" >/dev/null 2>"$work/refusal"; then
    fail "$name was accepted"
    return
  fi
  grep -q "$expected" "$work/refusal" \
    || fail "$name refused without reporting '$expected'"
}

# Two primary packages: the repository entry has no single answer.
: > "$stub/other-1-x86_64.pkg.tar.zst"
stub_listing "$stub/pkg-1-x86_64.pkg.tar.zst
$stub/other-1-x86_64.pkg.tar.zst"
refuses "a listing naming two primary packages" "$stub" "names 2 primary packages"

# A listing naming a package the build did not write.
stub_listing "$stub/absent-1-x86_64.pkg.tar.zst"
refuses "a listing naming an unbuilt package" "$stub" "was not built"

# An empty listing.
stub_listing ""
refuses "an empty listing" "$stub" "names 0 primary packages"

# A directory holding no recipe.
mkdir -p "$work/no-recipe"
refuses "a directory without a PKGBUILD" "$work/no-recipe" "holds no PKGBUILD"

# The checked-in recipe under the real makepkg: a copy of the recipe stands
# in for a build directory, its listing supplies the package name, and the
# named file is created so the selection has a built artifact to return.
# The recipe directory in the tree holds no package, so this arm runs on the
# copy and asserts a path rather than skipping on an absent file.
PATH=${PATH#"$work/bin:"}
export PATH
hash -r 2>/dev/null || true
if command -v makepkg >/dev/null 2>&1; then
  live=$work/recipe
  mkdir -p "$live"
  cp "$recipe"/* "$live/"
  listing=$(cd "$live" && makepkg --packagelist)
  named=$(printf '%s\n' "$listing" | grep -v -- '-debug-[^/]*$' | head -1)
  case $named in
    "$live"/mesa-gororoba-*.pkg.tar.zst) ;;
    *) fail "makepkg --packagelist named $named for the checked-in recipe" ;;
  esac
  refuses "the recipe before its package is built" "$live" "was not built"
  : > "$named"
  chosen=$(sh "$select_built" "$live") || fail "the built recipe copy was refused"
  [ "$chosen" = "$named" ] || fail "the built recipe copy selected $chosen"
else
  echo "not run: makepkg is absent, so the live-recipe arm has no lister" >&2
fi

[ "$status" -eq 0 ] && echo "OK    built-package selection: two accept arms, five fail-closed refusals"
exit "$status"
