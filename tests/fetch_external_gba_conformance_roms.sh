#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-Test-Games}"
EXTERNAL_ROOT="${ROOT}/external"
CLONE_ROOT="${EXTERNAL_ROOT}/sources"
STAGE_ROOT="${EXTERNAL_ROOT}/gba-conformance-dropin"
MAX_PER_PACK="${GBEMU_CONFORMANCE_GBA_EXTERNAL_MAX_PER_PACK:-4}"
INCLUDE_NON_VERDICT="${GBEMU_FETCH_INCLUDE_NON_VERDICT:-0}"

if [[ ! "${MAX_PER_PACK}" =~ ^[0-9]+$ ]]; then
  MAX_PER_PACK=4
fi
if [[ "${INCLUDE_NON_VERDICT}" != "0" && "${INCLUDE_NON_VERDICT}" != "1" ]]; then
  INCLUDE_NON_VERDICT=0
fi

require_tool() {
  local tool="$1"
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Missing required tool: ${tool}" >&2
    exit 1
  fi
}

require_tool git
require_tool find

mkdir -p "${CLONE_ROOT}" "${STAGE_ROOT}"
rm -f "${STAGE_ROOT}"/*.gba

sync_repo() {
  local name="$1"
  local url="$2"
  local dir="${CLONE_ROOT}/${name}"

  if [[ ! -d "${dir}/.git" ]]; then
    git clone --depth 1 "${url}" "${dir}"
    return
  fi

  git -C "${dir}" remote set-url origin "${url}"
  git -C "${dir}" fetch --depth 1 origin

  local ref
  ref="$(git -C "${dir}" symbolic-ref --quiet --short refs/remotes/origin/HEAD || true)"
  if [[ -z "${ref}" ]]; then
    if git -C "${dir}" show-ref --verify --quiet refs/remotes/origin/main; then
      ref="origin/main"
    elif git -C "${dir}" show-ref --verify --quiet refs/remotes/origin/master; then
      ref="origin/master"
    else
      echo "Warning: could not determine origin HEAD for ${name}; leaving existing checkout" >&2
      return
    fi
  fi

  git -C "${dir}" checkout -q --detach "${ref}"
  git -C "${dir}" clean -fdx
}

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
  shift
  local token_set
  for token_set in "$@"; do
    IFS=',' read -r -a tokens <<< "${token_set}"
    if path_has_all_tokens_ci "${lower_path}" "${tokens[@]}"; then
      return 0
    fi
  done
  return 1
}

declare -a repo_specs=(
  "jsmolka-gba-tests|https://github.com/jsmolka/gba-tests.git"
  "alyosha-tas-gba-tests|https://github.com/alyosha-tas/gba-tests.git"
)

declare -a source_dirs=()
for spec in "${repo_specs[@]}"; do
  name="${spec%%|*}"
  url="${spec#*|}"
  echo "Syncing ${name}..."
  sync_repo "${name}" "${url}"
  source_dirs+=("${CLONE_ROOT}/${name}")
done

declare -a all_roms=()
for dir in "${source_dirs[@]}"; do
  while IFS= read -r -d '' rom; do
    all_roms+=("$(realpath "${rom}")")
  done < <(find "${dir}" -type f -iname '*.gba' -print0 | sort -z)
done

if [[ "${#all_roms[@]}" -eq 0 ]]; then
  echo "No .gba ROM files found in fetched repos."
  echo "You may need to place prebuilt ROMs under ${ROOT} manually, then run preview/seed."
  exit 0
fi

stage_pack() {
  local pack_name="$1"
  local link_prefix="$2"
  shift 2
  local token_sets=("$@")
  local matched=0
  local staged=0
  local filtered_non_verdict=0
  local idx=1

  for rom in "${all_roms[@]}"; do
    local lower
    lower="$(printf '%s' "${rom}" | tr '[:upper:]' '[:lower:]')"
    if ! path_matches_any_token_set_ci "${lower}" "${token_sets[@]}"; then
      continue
    fi
    matched=$((matched + 1))
    if [[ "${INCLUDE_NON_VERDICT}" != "1" ]] && ! is_likely_verdict_rom "${rom}"; then
      filtered_non_verdict=$((filtered_non_verdict + 1))
      continue
    fi
    if [[ "${staged}" -ge "${MAX_PER_PACK}" ]]; then
      continue
    fi
    cp -f "${rom}" "${STAGE_ROOT}/${link_prefix}-${idx}.gba"
    staged=$((staged + 1))
    idx=$((idx + 1))
  done

  echo "Pack ${pack_name}: matched=${matched} staged=${staged} filtered_non_verdict=${filtered_non_verdict}"
}

rom_has_mgba_debug_signature() {
  local rom="$1"
  if grep -a -q $'\x00\xf6\xff\x04' "${rom}"; then
    return 0
  fi
  if grep -a -q $'\x00\xf7\xff\x04' "${rom}"; then
    return 0
  fi
  if grep -a -q $'\x80\xf7\xff\x04' "${rom}"; then
    return 0
  fi
  return 1
}

rom_has_verdict_text_signature() {
  local rom="$1"
  if strings -a "${rom}" | rg -qi '\b(pass|passed|fail|failed|success|error)\b'; then
    return 0
  fi
  return 1
}

is_likely_verdict_rom() {
  local rom="$1"
  rom_has_mgba_debug_signature "${rom}" && rom_has_verdict_text_signature "${rom}"
}

echo "Staging token-friendly ROM files in ${STAGE_ROOT} (max ${MAX_PER_PACK}/pack)..."
if [[ "${INCLUDE_NON_VERDICT}" != "1" ]]; then
  echo "Verdict filter: enabled (set GBEMU_FETCH_INCLUDE_NON_VERDICT=1 to disable)"
else
  echo "Verdict filter: disabled (including non-verdict ROMs)"
fi
stage_pack "gba-smoke" "mgba-conformance-smoke-gba-fetch" \
  "smoke" \
  "sanity" \
  "quick"
stage_pack "gba-cpu-arm" "mgba-gba-cpu-arm-fetch" \
  "cpu,arm" \
  "arm7" \
  "arm"
stage_pack "gba-cpu-thumb" "mgba-gba-cpu-thumb-fetch" \
  "cpu,thumb" \
  "thumb"
stage_pack "gba-dma-timer" "mgba-gba-dma-timer-fetch" \
  "dma" \
  "timer" \
  "irq" \
  "ime" \
  "pipeline"
stage_pack "gba-mem-timing" "mgba-gba-mem-timing-fetch" \
  "waitstate" \
  "waitcnt" \
  "prefetch" \
  "memory,timing" \
  "mem,timing" \
  "sram" \
  "ws0" \
  "ws1" \
  "ws2"
stage_pack "gba-ppu" "mgba-gba-ppu-fetch" \
  "ppu" \
  "mode" \
  "window" \
  "blend"
stage_pack "gba-swi-bios" "mgba-gba-swi-bios-fetch" \
  "swi,bios"
stage_pack "gba-swi-compat" "mgba-gba-swi-compat-fetch" \
  "swi,compat"
stage_pack "gba-swi-realbios" "mgba-gba-swi-realhw-fetch" \
  "swi,realhw" \
  "swi,realbios" \
  "swi,bios,real"

echo
echo "Fetched sources:"
for dir in "${source_dirs[@]}"; do
  echo "  - ${dir}"
done
echo "Staged files: ${STAGE_ROOT}"
echo
echo "Next:"
echo "  ./tests/preview_gba_conformance_matches.sh \"${ROOT}\""
echo "  ./tests/run_seed_gba_report.sh \"${ROOT}\""
