#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-Test-Games}"
if [[ ! -d "${ROOT}" ]]; then
  echo "Missing ROM root: ${ROOT}" >&2
  exit 1
fi

MAX_PER_PACK="${GBEMU_CONFORMANCE_GBA_EXTERNAL_MAX_PER_PACK:-4}"
if [[ ! "${MAX_PER_PACK}" =~ ^[0-9]+$ ]]; then
  MAX_PER_PACK=4
fi

ROOT_REAL="$(realpath "${ROOT}")"
CONF_ROOT_REAL="$(realpath -m "${ROOT}/Conformance")"
EXTERNAL_SOURCES_REAL="$(realpath -m "${ROOT}/external/sources")"
INCLUDE_EXTERNAL_SOURCES="${GBEMU_CONFORMANCE_INCLUDE_EXTERNAL_SOURCES:-0}"

path_has_all_tokens_ci() {
  local lower_path="$1"
  shift
  local token
  for token in "$@"; do
    if [[ "${lower_path}" != *"${token}"* ]]; then
      return 1
    fi
  done
  return 0
}

path_matches_any_token_set_ci() {
  local lower_path="$1"
  local groups_csv="$2"
  local group
  local -a groups_arr=()
  local -a tokens=()
  IFS=';' read -r -a groups_arr <<< "${groups_csv}"
  for group in "${groups_arr[@]}"; do
    IFS=',' read -r -a tokens <<< "${group}"
    if path_has_all_tokens_ci "${lower_path}" "${tokens[@]}"; then
      return 0
    fi
  done
  return 1
}

format_rel_path() {
  local path="$1"
  if [[ "${path}" == "${ROOT_REAL}/"* ]]; then
    printf '%s' "${path#${ROOT_REAL}/}"
  else
    printf '%s' "${path}"
  fi
}

readarray -t PACKS <<'EOF'
gba-smoke|mgba,conformance,smoke,gba;gba-tests,conformance,smoke,gba;testsuite,conformance,smoke,gba
gba-cpu-arm|mgba,gba,cpu,arm;gba-tests,gba,cpu,arm;testsuite,gba,cpu,arm;conformance,gba,cpu,arm
gba-cpu-thumb|mgba,gba,cpu,thumb;gba-tests,gba,cpu,thumb;testsuite,gba,cpu,thumb;conformance,gba,cpu,thumb
gba-dma-timer|mgba,gba,dma,timer;gba-tests,gba,dma,timer;testsuite,gba,dma,timer;conformance,gba,irq,timing;conformance,gba,ime,pipeline
gba-mem-timing|mgba,gba,mem,timing;gba-tests,gba,mem,timing;testsuite,gba,mem,timing;conformance,gba,waitstate;conformance,gba,prefetch
gba-ppu|mgba,gba,ppu;gba-tests,gba,ppu;testsuite,gba,ppu;conformance,gba,ppu
gba-swi-bios|mgba,gba,swi,bios;gba-tests,gba,swi,bios;testsuite,gba,swi,bios;conformance,gba,swi,bios
gba-swi-compat|mgba,gba,swi,compat;gba-tests,gba,swi,compat;testsuite,gba,swi,compat;conformance,gba,swi,compat
gba-swi-realbios|mgba,gba,swi,realhw;mgba,gba,swi,realbios;gba-tests,gba,swi,realhw;gba-tests,gba,swi,realbios;testsuite,gba,swi,realhw;testsuite,gba,swi,realbios;conformance,gba,swi,realhw;conformance,gba,swi,realbios
EOF

declare -a CANDIDATES=()
while IFS= read -r -d '' src; do
  real_src="$(realpath "${src}")"
  if [[ "${real_src}" == "${CONF_ROOT_REAL}"/* ]]; then
    continue
  fi
  if [[ "${INCLUDE_EXTERNAL_SOURCES}" != "1" && "${real_src}" == "${EXTERNAL_SOURCES_REAL}"/* ]]; then
    continue
  fi
  lower_src="$(printf '%s' "${real_src}" | tr '[:upper:]' '[:lower:]')"
  CANDIDATES+=("${real_src}|${lower_src}")
done < <(find "${ROOT}" -type f -iname '*.gba' -print0 | sort -z)

echo "GBA conformance match preview"
echo "ROM root: ${ROOT_REAL}"
echo "External import cap per pack: ${MAX_PER_PACK}"
echo "Excluded subtree: ${CONF_ROOT_REAL}"
if [[ "${INCLUDE_EXTERNAL_SOURCES}" != "1" ]]; then
  echo "Excluded subtree: ${EXTERNAL_SOURCES_REAL} (set GBEMU_CONFORMANCE_INCLUDE_EXTERNAL_SOURCES=1 to include)"
fi
echo

for spec in "${PACKS[@]}"; do
  pack="${spec%%|*}"
  groups="${spec#*|}"
  total=0
  selected=0
  declare -a picked=()
  declare -a overflow=()
  for entry in "${CANDIDATES[@]}"; do
    real_src="${entry%%|*}"
    lower_src="${entry#*|}"
    if ! path_matches_any_token_set_ci "${lower_src}" "${groups}"; then
      continue
    fi
    total=$((total + 1))
    if [[ "${selected}" -lt "${MAX_PER_PACK}" ]]; then
      picked+=("${real_src}")
      selected=$((selected + 1))
    else
      overflow+=("${real_src}")
    fi
  done

  echo "[${pack}] matched=${total} selected=${selected}"
  if [[ "${#picked[@]}" -eq 0 ]]; then
    echo "  - (none)"
  else
    for p in "${picked[@]}"; do
      echo "  - $(format_rel_path "${p}")"
    done
  fi
  if [[ "${#overflow[@]}" -gt 0 ]]; then
    echo "  - (+${#overflow[@]} more over cap)"
  fi
  echo
done
