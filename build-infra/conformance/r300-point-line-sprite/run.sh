#!/usr/bin/env bash
# r300 SWTCL point/line/sprite conformance regression harness (deqp-GLES2).
#
# WHY: the RS485M point + line + sprite fixes (per-vertex point size via the
# draw wide-point stage, the aliased-line-width clamp, gl_PointCoord, the
# point-size cap) each pass on real silicon but are easy to regress because they
# touch the shared gallium draw module and the GA rasterizer setup. This harness
# pins the validated per-case verdicts and fails on any Pass->Fail regression.
#
# WHAT: runs a fixed deqp-GLES2 case domain (caselist.txt) on the installed
# system driver, records per-case status, and diffs against baseline.tsv:
#   REGRESSION  = baseline Pass, now not Pass (Fail/crash/notrun)  -> exit 1
#   PROGRESSION = baseline not Pass, now Pass                      -> reported
#   NEW/GONE    = case present in one set only                     -> reported
#
# HOW:
#   run.sh --record           # run the domain, (re)write baseline.tsv
#   run.sh --check            # run the domain, diff vs baseline.tsv, gate exit
#   DEQP=/path/to/deqp-gles2 OUT=/var/tmp/dir run.sh --check
#
# The driver under test is whatever the system loader resolves (clear any stale
# loader override below); to test a scoped prefix, export the loader vars before
# calling. deqp must be the surfaceless build; config is pinned to the only
# combination that does not produce false fails on r300 (pbuffer + rgba8888d24s8).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEQP="${DEQP:-/home/eirikr/deqp_build/modules/gles2/deqp-gles2}"
OUT="${OUT:-/var/tmp/r300_conformance_run}"
CASELIST="${CASELIST:-$HERE/caselist.txt}"
BASELINE="${BASELINE:-$HERE/baseline.tsv}"
MODE="${1:---check}"

[ -x "$DEQP" ] || { echo "FATAL: deqp-gles2 not found/executable at $DEQP" >&2; exit 2; }
[ -f "$CASELIST" ] || { echo "FATAL: caselist not found at $CASELIST" >&2; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

# Reproducible loader + surface config. Always clear stale Vulkan/loader-override
# vars. For the GL driver, honor a caller-exported LIBGL_DRIVERS_PATH (with its
# LD_LIBRARY_PATH) so a scoped /opt or builddir prefix can be the subject; when it
# is unset, clear both so the SYSTEM driver (/usr/lib/dri) is the default subject.
# pbuffer + rgba8888d24s8 is mandatory on r300 (auto-config and fbo surface types
# produce 100% false fails).
unset VK_ICD_FILENAMES VK_DRIVER_FILES MESA_LOADER_DRIVER_OVERRIDE
if [ -z "${LIBGL_DRIVERS_PATH:-}" ]; then
  unset LIBGL_DRIVERS_PATH LD_LIBRARY_PATH
fi
export EGL_PLATFORM=surfaceless
DEQP_ARGS=(--deqp-surface-type=pbuffer --deqp-gl-config-name=rgba8888d24s8
           --deqp-surface-width=256 --deqp-surface-height=256)

# Extract "STATUS<TAB>casename" for every executed case from a deqp run log.
parse() {
  awk '
    /^Test case '\''/ { c=$0; sub(/^Test case '\''/,"",c); sub(/'\''\.\.[[:space:]]*$/,"",c) }
    /^  (Pass|Fail|NotSupported|QualityWarning|CompatibilityWarning|InternalError|ResourceError|Timeout|Crash)[[:space:]]*\(/ {
      st=$1; sub(/\(.*/,"",st); print st "\t" c
    }
  ' "$1"
}

echo "=== r300 point/line/sprite conformance run  $(date -u +%FT%TZ) ==="
# LIBGL_DRIVERS_PATH may be colon-separated; the loader scans each entry, so the
# diagnostic resolves r300_dri.so from the first that has it (default /usr/lib/dri).
_gl_dir="${LIBGL_DRIVERS_PATH:-/usr/lib/dri}"
for _d in ${_gl_dir//:/ }; do [ -e "$_d/r300_dri.so" ] && { _gl_dir="$_d"; break; }; done
echo "driver: $(readlink -f "$_gl_dir/r300_dri.so" 2>/dev/null || echo '?')${LIBGL_DRIVERS_PATH:+  (override)}"
pacman -Q mesa-gororoba-debug mesa-gororoba 2>/dev/null | head -2 || true

RESULTS="$OUT/results.tsv"
: > "$RESULTS"
gi=0
while IFS= read -r glob; do
  case "$glob" in ''|\#*) continue ;; esac
  gi=$((gi+1))
  log="$OUT/group_${gi}.txt"
  "$DEQP" "${DEQP_ARGS[@]}" --deqp-case="$glob" --deqp-log-filename="$OUT/group_${gi}.qpa" \
      > "$log" 2>&1
  rc=$?
  parse "$log" >> "$RESULTS"
  np=$(grep -c "$(printf '^Pass\t')" "$log" 2>/dev/null || true)
  # crash truncates the group; record it as a sentinel so --check can see the gap
  if grep -q "Aborted\|core dumped\|Segmentation\|FATAL ERROR" "$log"; then
    echo "  [$gi] $glob : ABORTED rc=$rc (group truncated)"
  fi
  printf '  [%d] %-58s %s cases\n' "$gi" "$glob" "$(grep -c "	" <(parse "$log"))"
done < "$CASELIST"

# Deduplicate (a case can match multiple globs); keep the worst status per case
# (Fail outranks Pass) so a regression is never masked by a later Pass match.
sort -u "$RESULTS" | awk -F'\t' '
  { rank["Pass"]=0; rank["NotSupported"]=1; rank["QualityWarning"]=1; rank["CompatibilityWarning"]=1;
    rank["Fail"]=3; rank["Timeout"]=3; rank["Crash"]=4; rank["InternalError"]=4; rank["ResourceError"]=4 }
  { s=$1; c=$2; r=(s in rank)?rank[s]:2; if (!(c in best) || r>bestr[c]) { best[c]=s; bestr[c]=r } }
  END { for (c in best) print best[c]"\t"c }
' | sort -t'	' -k2 > "$OUT/status.tsv"

total=$(wc -l < "$OUT/status.tsv")
npass=$(grep -c "$(printf '^Pass\t')" "$OUT/status.tsv" || true)
echo "=== executed $total cases, $npass Pass ==="

if [ "$MODE" = "--record" ]; then
  cp "$OUT/status.tsv" "$BASELINE"
  echo "=== baseline written: $BASELINE ($total cases) ==="
  exit 0
fi

# --check: diff status vs baseline
[ -f "$BASELINE" ] || { echo "FATAL: no baseline at $BASELINE (run --record first)" >&2; exit 2; }
# join on case name: col1=case, col2=baseline status, col3=now status
join -t'	' -1 2 -2 2 -a1 -a2 -e MISSING -o '0,1.1,2.1' \
     <(sort -t'	' -k2 "$BASELINE") <(sort -t'	' -k2 "$OUT/status.tsv") > "$OUT/joined.tsv"

awk -F'\t' '
  { c=$1; b=$2; n=$3
    if (b=="Pass" && n!="Pass") { reg[c]=n; nreg++ }
    else if (b!="Pass" && b!="MISSING" && n=="Pass") { prog[c]=b; nprog++ }
    else if (b=="MISSING" && n!="MISSING") { new[c]=n; nnew++ }
    else if (n=="MISSING" && b!="MISSING") { gone[c]=b; ngone++ }
  }
  END {
    if (nreg)  { print "\n!!! REGRESSIONS ("nreg") -- baseline Pass, now not:"; for (c in reg)  print "  REGRESS  "reg[c]"\t"c }
    if (nprog) { print "\n+++ PROGRESSIONS ("nprog") -- baseline fail, now Pass:"; for (c in prog) print "  FIXED    "prog[c]"->Pass\t"c }
    if (nnew)  { print "\n... NEW cases ("nnew"):"; for (c in new)  print "  NEW      "new[c]"\t"c }
    if (ngone) { print "\n... GONE cases ("ngone"):"; for (c in gone) print "  GONE     "gone[c]"\t"c }
    print "\n=== verdict: "nreg+0" regressions, "nprog+0" progressions, "nnew+0" new, "ngone+0" gone ==="
    exit (nreg>0 ? 1 : 0)
  }
' "$OUT/joined.tsv"
verdict=$?
echo "=== DONE_SENTINEL r300_conformance rc=$verdict ==="
exit "$verdict"
