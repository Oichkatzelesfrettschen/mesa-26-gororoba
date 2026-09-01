#!/bin/sh
# Every local file a packaging recipe lists in source=() carries a sha256sums
# entry equal to the checked-in bytes, so a recipe edit that leaves the sums
# behind fails here rather than at makepkg on the target.
set -u
root=$(cd "$(dirname "$0")" && pwd)
status=0
for pkgbuild in "$root"/*/PKGBUILD; do
  dir=$(dirname "$pkgbuild")
  python3 - "$pkgbuild" "$dir" <<'PY' || status=1
import hashlib, re, sys
pkgbuild, directory = sys.argv[1], sys.argv[2]
text = open(pkgbuild, encoding="utf-8").read()
def array(name):
    m = re.search(r"^%s=\((.*?)\)" % name, text, re.S | re.M)
    if not m:
        return None
    return re.findall(r"""['"]([^'"]*)['"]""", m.group(1))
sources = array("source")
sums = array("sha256sums")
if sources is None or sums is None:
    sys.exit(0)
if len(sources) != len(sums):
    print("%s: %d source entries, %d sha256sums" % (pkgbuild, len(sources), len(sums)))
    sys.exit(1)
bad = 0
for src, expected in zip(sources, sums):
    if "::" in src or "://" in src or expected == "SKIP":
        continue
    actual = hashlib.sha256(open("%s/%s" % (directory, src), "rb").read()).hexdigest()
    if actual != expected:
        print("%s: %s sha256 %s differs from listed %s" % (pkgbuild, src, actual, expected))
        bad = 1
sys.exit(bad)
PY
done
[ "$status" -eq 0 ] && echo "packaging local source checksums: every listed file matches"
exit "$status"
