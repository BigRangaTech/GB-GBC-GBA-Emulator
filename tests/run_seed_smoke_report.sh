#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-Test-Games}"
REPORT_PATH="${2:-tests/conformance_smoke_report.csv}"
TEST_BIN="${GBEMU_TEST_BIN:-./build/tests/gbemu_tests}"

if [[ ! -x "${TEST_BIN}" ]]; then
  echo "Missing test binary: ${TEST_BIN}" >&2
  echo "Build first: cmake --build build -j" >&2
  exit 1
fi

"$(dirname "$0")/seed_conformance_packs.sh" "${ROOT}"

GBEMU_RUN_CONFORMANCE=1 \
GBEMU_CONFORMANCE_ROOT="${ROOT}" \
GBEMU_CONFORMANCE_PACKS="${GBEMU_CONFORMANCE_PACKS:-smoke}" \
GBEMU_CONFORMANCE_MAX_PER_CASE="${GBEMU_CONFORMANCE_MAX_PER_CASE:-5}" \
GBEMU_CONFORMANCE_FRAME_LIMIT="${GBEMU_CONFORMANCE_FRAME_LIMIT:-0}" \
GBEMU_CONFORMANCE_BASELINE_FILE="${GBEMU_CONFORMANCE_BASELINE_FILE:-tests/conformance_pack_baseline.csv}" \
GBEMU_CONFORMANCE_ENFORCE_BASELINE="${GBEMU_CONFORMANCE_ENFORCE_BASELINE:-1}" \
GBEMU_CONFORMANCE_REPORT_PATH="${REPORT_PATH}" \
"${TEST_BIN}"

echo "Conformance smoke report: ${REPORT_PATH}"
