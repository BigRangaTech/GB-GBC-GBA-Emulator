#!/usr/bin/env bash
set -euo pipefail

REPORT_PATH="${1:-tests/conformance_gba_report.csv}"
BASELINE_PATH="${2:-tests/conformance_pack_baseline.csv}"

if [[ ! -f "${REPORT_PATH}" ]]; then
  echo "Missing conformance report: ${REPORT_PATH}" >&2
  exit 1
fi
if [[ ! -f "${BASELINE_PATH}" ]]; then
  echo "Missing baseline file: ${BASELINE_PATH}" >&2
  exit 1
fi

tmp_report="$(mktemp)"
tmp_out="$(mktemp)"
trap 'rm -f "${tmp_report}" "${tmp_out}"' EXIT

awk -F, '
  $1 == "pack" {
    # pack,pack_name,pack_key,executed,passed,failed,unknown
    pack = $3
    if (pack == "") {
      next
    }
    exec[pack] = int($4)
    pass[pack] = int($5)
    fail[pack] = int($6)
    unknown[pack] = int($7)
  }
  END {
    for (p in exec) {
      printf "%s,%d,%d,%d,%d\n", p, exec[p], pass[p], fail[p], unknown[p]
    }
  }
' "${REPORT_PATH}" | sort > "${tmp_report}"

declare -A rep_exec
declare -A rep_pass
declare -A rep_fail
declare -A rep_unknown

while IFS=, read -r pack exec pass fail unknown; do
  rep_exec["${pack}"]="${exec}"
  rep_pass["${pack}"]="${pass}"
  rep_fail["${pack}"]="${fail}"
  rep_unknown["${pack}"]="${unknown}"
done < "${tmp_report}"

changed=0
while IFS= read -r line || [[ -n "${line}" ]]; do
  if [[ -z "${line}" || "${line}" == \#* ]]; then
    printf '%s\n' "${line}" >> "${tmp_out}"
    continue
  fi

  IFS=, read -r pack min_executed min_pass max_fail max_unknown <<< "${line}"
  if [[ -n "${rep_exec[${pack}]:-}" ]]; then
    new_min_executed="${min_executed}"
    new_min_pass="${min_pass}"
    new_max_fail="${max_fail}"
    new_max_unknown="${max_unknown}"

    if (( rep_exec["${pack}"] > new_min_executed )); then
      new_min_executed="${rep_exec[${pack}]}"
    fi
    if (( rep_pass["${pack}"] > new_min_pass )); then
      new_min_pass="${rep_pass[${pack}]}"
    fi
    if (( rep_fail["${pack}"] < new_max_fail )); then
      new_max_fail="${rep_fail[${pack}]}"
    fi
    if (( rep_unknown["${pack}"] < new_max_unknown )); then
      new_max_unknown="${rep_unknown[${pack}]}"
    fi

    if [[ "${new_min_executed}" != "${min_executed}" ||
          "${new_min_pass}" != "${min_pass}" ||
          "${new_max_fail}" != "${max_fail}" ||
          "${new_max_unknown}" != "${max_unknown}" ]]; then
      changed=1
    fi
    printf '%s,%s,%s,%s,%s\n' \
      "${pack}" "${new_min_executed}" "${new_min_pass}" "${new_max_fail}" "${new_max_unknown}" >> "${tmp_out}"
  else
    printf '%s\n' "${line}" >> "${tmp_out}"
  fi
done < "${BASELINE_PATH}"

if (( changed == 1 )); then
  mv "${tmp_out}" "${BASELINE_PATH}"
  echo "Tightened baseline thresholds using report: ${REPORT_PATH}"
else
  echo "No baseline tightening opportunities from report: ${REPORT_PATH}"
fi
