#!/usr/bin/env bash
# r300vk Vulkan extension-surface conformance regression harness (deqp-vk).
#
# WHY: r300vk advertises a set of promoted KHR extensions additively at
# apiVersion 1.0 (VK_KHR_bind_memory2, get_memory_requirements2,
# dedicated_allocation, driver_properties, format_feature_flags2,
# uniform_buffer_standard_layout, relaxed_block_layout,
# storage_buffer_storage_class, sampler_mirror_clamp_to_edge).  These are
# enumeration/metadata plus two requirements getters; they are easy to regress
# silently (a withdrawn extension bit, a dropped VkFormatProperties3 fill, a
# null-pipeline replay crash) because nothing in the GL path exercises them.
# This harness pins the validated state and fails on any Pass->Fail regression.
#
# WHAT: three gated checks, all recorded as synthetic or real cases that diff
# against baseline.tsv exactly like the deqp-GLES2 harness:
#   1. smoke.device_extension.<name> -- each of the nine extensions is present
#      in vulkaninfo's device extension list.
#   2. smoke.vkcube.no_crash         -- vkcube (a direct-VK xcb app) does not
#      SIGSEGV/abort the driver (the null-bound-pipeline replay guard).  Only
#      run when a display + vkcube are available; otherwise NotSupported.
#   3. dEQP-VK.api.info.* domain      -- per-case status from the deqp-vk
#      headless build (caselist.txt), diffed for Pass->Fail regressions.
#
# HOW:
#   run.sh --record           # run all checks, (re)write baseline.tsv
#   run.sh --check            # run all checks, diff vs baseline.tsv, gate exit
#   DEQP_VK=/path/to/deqp-vk OUT=/var/tmp/dir run.sh --check
#
# The driver under test is whatever the Vulkan loader resolves; export
# VK_ICD_FILENAMES before calling to test a scoped build (a /tmp devenv ICD).
# Verdict: REGRESSION (baseline Pass, now not Pass) -> exit 1.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEQP_VK="${DEQP_VK:-/home/eirikr/workspaces/mesa/deqp-vk/build-vostro-r300vk-headless/external/vulkancts/modules/vulkan/deqp-vk}"
OUT="${OUT:-/var/tmp/r300vk_vk_conformance_run}"
CASELIST="${CASELIST:-$HERE/caselist.txt}"
BASELINE="${BASELINE:-$HERE/baseline.tsv}"
MODE="${1:---check}"

EXTS=(VK_KHR_bind_memory2 VK_KHR_get_memory_requirements2 VK_KHR_dedicated_allocation
      VK_KHR_driver_properties VK_KHR_format_feature_flags2
      VK_KHR_uniform_buffer_standard_layout VK_KHR_relaxed_block_layout
      VK_KHR_storage_buffer_storage_class VK_KHR_sampler_mirror_clamp_to_edge)

rm -rf "$OUT"; mkdir -p "$OUT"
RESULTS="$OUT/results.tsv"
: > "$RESULTS"

echo "=== r300vk Vulkan surface conformance run  $(date -u +%FT%TZ) ==="
echo "ICD: ${VK_ICD_FILENAMES:-<system>}"

# --- check 1: extension presence (vulkaninfo) ---------------------------------
if command -v vulkaninfo >/dev/null 2>&1; then
  VI="$OUT/vulkaninfo.txt"
  vulkaninfo > "$VI" 2>/dev/null || true
  for e in "${EXTS[@]}"; do
    if grep -q "$e " "$VI"; then st=Pass; else st=Fail; fi
    printf '%s\tsmoke.device_extension.%s\n' "$st" "$e" >> "$RESULTS"
  done
else
  for e in "${EXTS[@]}"; do printf 'NotSupported\tsmoke.device_extension.%s\n' "$e" >> "$RESULTS"; done
fi

# --- check 2: vkcube no-crash (null-pipeline replay guard) --------------------
if command -v vkcube >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
  timeout 15 vkcube --c 3 > "$OUT/vkcube.txt" 2>&1
  rc=$?
  if [ "$rc" = 139 ] || [ "$rc" = 134 ] || grep -qi "core dumped\|Segmentation\|Aborted" "$OUT/vkcube.txt"; then
    printf 'Fail\tsmoke.vkcube.no_crash\n' >> "$RESULTS"
  else
    printf 'Pass\tsmoke.vkcube.no_crash\n' >> "$RESULTS"
  fi
else
  printf 'NotSupported\tsmoke.vkcube.no_crash\n' >> "$RESULTS"
fi

# --- check 3: deqp-vk api.info domain ----------------------------------------
parse() {
  awk '
    /^Test case '\''/ { c=$0; sub(/^Test case '\''/,"",c); sub(/'\''\.\.[[:space:]]*$/,"",c) }
    /^  (Pass|Fail|NotSupported|QualityWarning|CompatibilityWarning|InternalError|ResourceError|Timeout|Crash)[[:space:]]*\(/ {
      st=$1; sub(/\(.*/,"",st); print st "\t" c
    }
  ' "$1"
}
if [ -x "$DEQP_VK" ]; then
  DVKDIR="$(cd "$(dirname "$DEQP_VK")" && pwd)"
  gi=0
  while IFS= read -r glob; do
    case "$glob" in ''|\#*) continue ;; esac
    gi=$((gi+1))
    log="$OUT/group_${gi}.txt"
    ( cd "$DVKDIR" && "$DEQP_VK" --deqp-case="$glob" \
        --deqp-log-filename="$OUT/group_${gi}.qpa" ) > "$log" 2>&1
    parse "$log" >> "$RESULTS"
    printf '  [%d] %-52s %s cases\n' "$gi" "$glob" "$(parse "$log" | grep -c "	")"
  done < "$CASELIST"
else
  echo "WARN: deqp-vk not executable at $DEQP_VK -- domain skipped" >&2
fi

# Deduplicate, worst-status-wins (Fail outranks Pass).
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

[ -f "$BASELINE" ] || { echo "FATAL: no baseline at $BASELINE (run --record first)" >&2; exit 2; }
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
    if (nprog) { print "\n+++ PROGRESSIONS ("nprog"):"; for (c in prog) print "  FIXED    "prog[c]"->Pass\t"c }
    if (nnew)  { print "\n... NEW cases ("nnew"):"; for (c in new)  print "  NEW      "new[c]"\t"c }
    if (ngone) { print "\n... GONE cases ("ngone"):"; for (c in gone) print "  GONE     "gone[c]"\t"c }
    print "\n=== verdict: "nreg+0" regressions, "nprog+0" progressions, "nnew+0" new, "ngone+0" gone ==="
    exit (nreg>0 ? 1 : 0)
  }
' "$OUT/joined.tsv"
verdict=$?
echo "=== DONE_SENTINEL r300vk_vk_conformance rc=$verdict ==="
exit "$verdict"
