#!/bin/sh
# Calibrate the source-commit pin against a recipe it must accept and four it
# must refuse.  Each refusal arm also compares the recipe bytes before and
# after, because a pin that edits and then reports failure leaves a recipe
# nobody inspects in a state nobody chose.
set -u
root=$(cd "$(dirname "$0")" && pwd)
pin="$root/pin-source-commit.sh"
recipe="$root/mesa-gororoba/PKGBUILD"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0
sha=0123456789abcdef0123456789abcdef01234567
other=fedcba9876543210fedcba9876543210fedcba98

fail() {
  echo "FAIL  $1" >&2
  status=1
}

# Positive: the branch-tracking recipe pins, and the pinned recipe re-pins.
cp "$recipe" "$work/accept"
if sh "$pin" "$work/accept" "$sha" >/dev/null; then
  grep -q "git+[^\"]*#commit=$sha\"" "$work/accept" || fail "pin left no commit fragment"
  grep -q 'git+[^"]*#branch=' "$work/accept" && fail "pin left a branch-tracking source"
  sh "$pin" "$work/accept" "$other" >/dev/null || fail "re-pin of a pinned recipe refused"
  grep -q "git+[^\"]*#commit=$other\"" "$work/accept" || fail "re-pin left the first object name"
else
  fail "pin refused the checked-in recipe"
fi

# Refusals: the recipe bytes survive each one unchanged.
refuses() {
  name=$1
  file=$2
  shift 2
  before=$(cksum < "$file")
  if sh "$pin" "$file" "$@" >/dev/null 2>&1; then
    fail "$name was accepted"
  fi
  after=$(cksum < "$file")
  [ "$before" = "$after" ] || fail "$name rewrote the recipe before refusing"
}

# A recipe whose source array lost its revision fragment.
sed -E 's,(git\+[^"]*)#(branch|commit)=[^"]*,\1,' "$recipe" > "$work/no-fragment"
refuses "a recipe without a revision fragment" "$work/no-fragment" "$sha"

# A recipe carrying two git sources: the pin names no single one.
awk '{ print } /git\+[^"]*#branch=/ { print }' "$recipe" > "$work/two-fragments"
refuses "a recipe with two revision fragments" "$work/two-fragments" "$sha"

# Object names outside the git spelling.
cp "$recipe" "$work/nonhex"
refuses "a non-hex object name" "$work/nonhex" deadbeefzz

cp "$recipe" "$work/short"
refuses "an object name shorter than seven characters" "$work/short" abc123

[ "$status" -eq 0 ] && echo "OK    source-commit pin: one accept arm, four fail-closed refusals"
exit "$status"
