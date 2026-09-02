#!/bin/sh
# Pin a packaging recipe's git source to one commit.  A recipe names its
# source revision through the URL fragment makepkg parses -- `#branch=main`
# tracks the branch tip, `#commit=<sha>` fetches one object -- so pinning is
# a rewrite of that fragment in the copied recipe.  The rewrite counts the
# fragments it finds before it edits and re-reads the result afterward: a
# recipe whose source array moved to another spelling refuses the pin and the
# caller learns the pin did not apply, rather than building the branch tip
# under a name that claims one commit.
set -u

usage() {
  echo "usage: pin-source-commit.sh <PKGBUILD> <commit>" >&2
  exit 2
}

[ $# -eq 2 ] || usage
pkgbuild=$1
commit=$2

[ -f "$pkgbuild" ] || {
  echo "pin-source-commit: $pkgbuild is not a file" >&2
  exit 2
}

case $commit in
  "" | *[!0-9a-f]*)
    echo "pin-source-commit: '$commit' is not a lowercase hex git object name" >&2
    exit 2
    ;;
esac

length=$(printf %s "$commit" | wc -c)
if [ "$length" -lt 7 ] || [ "$length" -gt 40 ]; then
  echo "pin-source-commit: '$commit' is $length characters; a git object name is 7 to 40" >&2
  exit 2
fi

# The prose above a source array mentions both fragment spellings, so the
# match is anchored on the `git+` scheme that only the array carries.
fragments=$(grep -c 'git+[^"]*#\(branch\|commit\)=' "$pkgbuild" || true)
if [ "$fragments" -ne 1 ]; then
  echo "pin-source-commit: $pkgbuild carries $fragments git source revision fragments; the pin needs exactly one" >&2
  exit 2
fi

sed -E -i "s,(git\\+[^\"]*)#(branch|commit)=[^\"]*,\\1#commit=$commit," "$pkgbuild"

pinned=$(grep -c "git+[^\"]*#commit=$commit\"" "$pkgbuild" || true)
remaining=$(grep -c 'git+[^"]*#branch=' "$pkgbuild" || true)
if [ "$pinned" -ne 1 ] || [ "$remaining" -ne 0 ]; then
  echo "pin-source-commit: $pkgbuild holds $pinned pinned and $remaining branch-tracking sources after the rewrite" >&2
  exit 2
fi

echo "pin-source-commit: $pkgbuild source pinned to $commit"
