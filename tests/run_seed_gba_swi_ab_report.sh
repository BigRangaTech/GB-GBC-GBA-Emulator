#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-Test-Games}"
OUT_DIR="${2:-tests}"
TEST_BIN="${GBEMU_TEST_BIN:-./build/tests/gbemu_tests}"
PACKS="${GBEMU_CONFORMANCE_PACKS:-gba-swi-bios,gba-swi-compat,gba-swi-realbios}"
BASELINE_FILE="${GBEMU_CONFORMANCE_BASELINE_FILE:-tests/conformance_pack_baseline.csv}"
ENFORCE_BASELINE="${GBEMU_CONFORMANCE_ENFORCE_BASELINE:-0}"
FRAME_LIMIT="${GBEMU_CONFORMANCE_FRAME_LIMIT:-0}"
MAX_PER_CASE="${GBEMU_CONFORMANCE_MAX_PER_CASE:-5}"

DEFAULT_REPORT="${OUT_DIR}/conformance_gba_swi_default_report.csv"
REALBIOS_REPORT="${OUT_DIR}/conformance_gba_swi_realbios_report.csv"
DIFF_REPORT="${OUT_DIR}/conformance_gba_swi_ab_diff.csv"

if [[ ! -x "${TEST_BIN}" ]]; then
  echo "Missing test binary: ${TEST_BIN}" >&2
  echo "Build first: cmake --build build -j" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"
"$(dirname "$0")/seed_conformance_packs.sh" "${ROOT}"

GBEMU_RUN_CONFORMANCE=1 \
GBEMU_CONFORMANCE_ROOT="${ROOT}" \
GBEMU_CONFORMANCE_PACKS="${PACKS}" \
GBEMU_CONFORMANCE_MAX_PER_CASE="${MAX_PER_CASE}" \
GBEMU_CONFORMANCE_FRAME_LIMIT="${FRAME_LIMIT}" \
GBEMU_CONFORMANCE_BASELINE_FILE="${BASELINE_FILE}" \
GBEMU_CONFORMANCE_ENFORCE_BASELINE="${ENFORCE_BASELINE}" \
GBEMU_CONFORMANCE_REPORT_PATH="${DEFAULT_REPORT}" \
"${TEST_BIN}"

GBEMU_RUN_CONFORMANCE=1 \
GBEMU_CONFORMANCE_ROOT="${ROOT}" \
GBEMU_CONFORMANCE_PACKS="${PACKS}" \
GBEMU_CONFORMANCE_MAX_PER_CASE="${MAX_PER_CASE}" \
GBEMU_CONFORMANCE_FRAME_LIMIT="${FRAME_LIMIT}" \
GBEMU_CONFORMANCE_BASELINE_FILE="${BASELINE_FILE}" \
GBEMU_CONFORMANCE_ENFORCE_BASELINE="${ENFORCE_BASELINE}" \
GBEMU_CONFORMANCE_GBA_SWI_FORCE_REAL_BIOS=1 \
GBEMU_CONFORMANCE_REPORT_PATH="${REALBIOS_REPORT}" \
"${TEST_BIN}"

tmp_diff="$(mktemp)"
awk -F, '
BEGIN { OFS="," }
FNR == 1 { next }
FNR == NR && ($1 == "case" || $1 == "pack") {
  key = $1 OFS $2 OFS $3
  d_exec[key] = $4; d_pass[key] = $5; d_fail[key] = $6; d_unknown[key] = $7
  keys[key] = 1
  next
}
($1 == "case" || $1 == "pack") {
  key = $1 OFS $2 OFS $3
  r_exec[key] = $4; r_pass[key] = $5; r_fail[key] = $6; r_unknown[key] = $7
  keys[key] = 1
}
END {
  print "type,name,pack,default_executed,default_passed,default_failed,default_unknown,realbios_executed,realbios_passed,realbios_failed,realbios_unknown,delta_passed,delta_failed,delta_unknown"
  for (key in keys) {
    de = (key in d_exec) ? d_exec[key] : 0
    dp = (key in d_pass) ? d_pass[key] : 0
    df = (key in d_fail) ? d_fail[key] : 0
    du = (key in d_unknown) ? d_unknown[key] : 0
    re = (key in r_exec) ? r_exec[key] : 0
    rp = (key in r_pass) ? r_pass[key] : 0
    rf = (key in r_fail) ? r_fail[key] : 0
    ru = (key in r_unknown) ? r_unknown[key] : 0
    split(key, parts, FS)
    print parts[1], parts[2], parts[3], de, dp, df, du, re, rp, rf, ru, (rp - dp), (rf - df), (ru - du)
  }
}' "${DEFAULT_REPORT}" "${REALBIOS_REPORT}" > "${tmp_diff}"

{
  head -n 1 "${tmp_diff}"
  tail -n +2 "${tmp_diff}" | sort -t, -k1,1 -k2,2 -k3,3
} > "${DIFF_REPORT}"
rm -f "${tmp_diff}"

echo "Conformance SWI A/B reports:"
echo "  default:  ${DEFAULT_REPORT}"
echo "  realbios: ${REALBIOS_REPORT}"
echo "  diff:     ${DIFF_REPORT}"
