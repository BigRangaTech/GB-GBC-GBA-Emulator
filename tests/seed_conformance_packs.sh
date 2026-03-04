#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-Test-Games}"
CONF_ROOT="${ROOT}/Conformance"

mkdir -p \
  "${CONF_ROOT}/SMOKE" \
  "${CONF_ROOT}/GBA/CPU/ARM" \
  "${CONF_ROOT}/GBA/CPU/THUMB" \
  "${CONF_ROOT}/GBA/DMA/TIMER" \
  "${CONF_ROOT}/GBA/MEM/TIMING" \
  "${CONF_ROOT}/GBA/PPU" \
  "${CONF_ROOT}/GBA/SWI" \
  "${CONF_ROOT}/GBA/SWI/COMPAT" \
  "${CONF_ROOT}/GBA/SWI/REALHW" \
  "${CONF_ROOT}/GBC/PPU" \
  "${CONF_ROOT}/GB/TIMER/IRQ"
CONF_ROOT_REAL="$(realpath "${CONF_ROOT}")"
EXTERNAL_SOURCES_REAL="$(realpath -m "${ROOT}/external/sources")"
INCLUDE_EXTERNAL_SOURCES="${GBEMU_CONFORMANCE_INCLUDE_EXTERNAL_SOURCES:-0}"

linked=0
GBA_EXTERNAL_MAX_PER_PACK="${GBEMU_CONFORMANCE_GBA_EXTERNAL_MAX_PER_PACK:-4}"
if [[ ! "${GBA_EXTERNAL_MAX_PER_PACK}" =~ ^[0-9]+$ ]]; then
  GBA_EXTERNAL_MAX_PER_PACK=4
fi

link_if_exists() {
  local src="$1"
  local dst="$2"
  if [[ ! -f "${src}" ]]; then
    return
  fi
  ln -sfn "$(realpath "${src}")" "${dst}"
  linked=$((linked + 1))
}

sanitize_stem() {
  local stem="$1"
  local sanitized
  sanitized="$(echo "${stem}" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//')"
  if [[ -z "${sanitized}" ]]; then
    sanitized="rom"
  fi
  printf '%s' "${sanitized}"
}

link_many_from_dir() {
  local search_root="$1"
  local target_dir="$2"
  local suffix="$3"
  local max_count="${4:-0}"
  local count=0
  while IFS= read -r -d '' src; do
    if [[ "${max_count}" -gt 0 && "${count}" -ge "${max_count}" ]]; then
      break
    fi
    local base ext stem sanitized dst
    base="$(basename "${src}")"
    ext="${base##*.}"
    stem="${base%.*}"
    sanitized="$(sanitize_stem "${stem}")"
    dst="${target_dir}/${sanitized}-${suffix}.${ext,,}"
    ln -sfn "$(realpath "${src}")" "${dst}"
    linked=$((linked + 1))
    count=$((count + 1))
  done < <(find "${search_root}" -maxdepth 2 -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.gba' \) -print0 2>/dev/null)
}

link_first_by_path_pattern() {
  local search_root="$1"
  local pattern="$2"
  local dst="$3"
  local src
  src="$(find "${search_root}" -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.gba' \) -ipath "${pattern}" | sort | head -n 1 || true)"
  if [[ -z "${src}" ]]; then
    return
  fi
  ln -sfn "$(realpath "${src}")" "${dst}"
  linked=$((linked + 1))
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

target_dir_has_realpath_link() {
  local target_dir="$1"
  local real_src="$2"
  local entry
  for entry in "${target_dir}"/*.gba; do
    if [[ ! -e "${entry}" ]]; then
      continue
    fi
    if [[ "$(realpath "${entry}")" == "${real_src}" ]]; then
      return 0
    fi
  done
  return 1
}

link_many_gba_by_any_token_set() {
  local search_root="$1"
  local target_dir="$2"
  local name_prefix="$3"
  local max_count="$4"
  shift 4
  local token_sets=("$@")
  local count=0
  local index=1

  while IFS= read -r src; do
    if [[ "${count}" -ge "${max_count}" ]]; then
      break
    fi
    local real_src lower
    real_src="$(realpath "${src}")"
    lower="$(printf '%s' "${real_src}" | tr '[:upper:]' '[:lower:]')"
    if [[ "${real_src}" == "${CONF_ROOT_REAL}"/* ]]; then
      continue
    fi
    if [[ "${INCLUDE_EXTERNAL_SOURCES}" != "1" && "${real_src}" == "${EXTERNAL_SOURCES_REAL}"/* ]]; then
      continue
    fi
    if ! path_matches_any_token_set_ci "${lower}" "${token_sets[@]}"; then
      continue
    fi
    if target_dir_has_realpath_link "${target_dir}" "${real_src}"; then
      continue
    fi
    local dst="${target_dir}/${name_prefix}-${index}.gba"
    ln -sfn "${real_src}" "${dst}"
    linked=$((linked + 1))
    count=$((count + 1))
    index=$((index + 1))
  done < <(find "${search_root}" -type f -iname '*.gba' | sort)

  if [[ "${count}" -gt 0 ]]; then
    return 0
  fi
  return 1
}

GEN_DIR="${CONF_ROOT}/_Generated"
mkdir -p "${GEN_DIR}"
rm -f "${GEN_DIR}"/*.gba
"$(dirname "$0")/generate_gba_conformance_roms.sh" "${GEN_DIR}"

# Seed deterministic verdict-capable GBA ROMs into each targeted GBA pack.
rm -f "${CONF_ROOT}/GBA/CPU/ARM"/*.gba
rm -f "${CONF_ROOT}/GBA/CPU/THUMB"/*.gba
rm -f "${CONF_ROOT}/GBA/DMA/TIMER"/*.gba
rm -f "${CONF_ROOT}/GBA/MEM/TIMING"/*.gba
rm -f "${CONF_ROOT}/GBA/PPU"/*.gba
rm -f "${CONF_ROOT}/GBA/SWI"/*.gba
rm -f "${CONF_ROOT}/GBA/SWI/COMPAT"/*.gba
rm -f "${CONF_ROOT}/GBA/SWI/REALHW"/*.gba
rm -f "${CONF_ROOT}/GBA/SWI/REALBIOS"/*.gba
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/CPU/ARM" "mgba-gba-cpu-arm-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,cpu,arm" \
  "gba-tests,gba,cpu,arm" \
  "testsuite,gba,cpu,arm" \
  "conformance,gba,cpu,arm" || true
link_if_exists "${GEN_DIR}/gen_cpu_arm.gba" "${CONF_ROOT}/GBA/CPU/ARM/mgba-gba-cpu-arm-pass.gba"
link_if_exists "${GEN_DIR}/gen_cpu_arm_mul.gba" "${CONF_ROOT}/GBA/CPU/ARM/mgba-gba-cpu-arm-mul-pass.gba"
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/CPU/THUMB" "mgba-gba-cpu-thumb-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,cpu,thumb" \
  "gba-tests,gba,cpu,thumb" \
  "testsuite,gba,cpu,thumb" \
  "conformance,gba,cpu,thumb" || true
link_if_exists "${GEN_DIR}/gen_cpu_thumb.gba" "${CONF_ROOT}/GBA/CPU/THUMB/mgba-gba-cpu-thumb-pass.gba"
link_if_exists "${GEN_DIR}/gen_cpu_thumb_add.gba" "${CONF_ROOT}/GBA/CPU/THUMB/mgba-gba-cpu-thumb-add-pass.gba"
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/DMA/TIMER" "mgba-gba-dma-timer-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,dma,timer" \
  "gba-tests,gba,dma,timer" \
  "testsuite,gba,dma,timer" \
  "conformance,gba,irq,timing" \
  "conformance,gba,ime,pipeline" || true
link_if_exists "${GEN_DIR}/gen_dma_timer.gba" "${CONF_ROOT}/GBA/DMA/TIMER/mgba-gba-dma-timer-pass.gba"
link_if_exists "${GEN_DIR}/gen_dma_timer_irq.gba" "${CONF_ROOT}/GBA/DMA/TIMER/mgba-gba-dma-timer-irq-pass.gba"
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/MEM/TIMING" "mgba-gba-mem-timing-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,mem,timing" \
  "gba-tests,gba,mem,timing" \
  "testsuite,gba,mem,timing" \
  "conformance,gba,waitstate" \
  "conformance,gba,prefetch" || true
link_if_exists "${GEN_DIR}/gen_mem_timing.gba" "${CONF_ROOT}/GBA/MEM/TIMING/mgba-gba-mem-timing-pass.gba"
link_if_exists "${GEN_DIR}/gen_mem_timing_prefetch.gba" "${CONF_ROOT}/GBA/MEM/TIMING/mgba-gba-mem-timing-prefetch-pass.gba"
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/PPU" "mgba-gba-ppu-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,ppu" \
  "gba-tests,gba,ppu" \
  "testsuite,gba,ppu" \
  "conformance,gba,ppu" || true
link_if_exists "${GEN_DIR}/gen_ppu.gba" "${CONF_ROOT}/GBA/PPU/mgba-gba-ppu-pass.gba"
link_if_exists "${GEN_DIR}/gen_ppu_window.gba" "${CONF_ROOT}/GBA/PPU/mgba-gba-ppu-window-pass.gba"
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/SWI" "mgba-gba-swi-bios-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,swi,bios" \
  "gba-tests,gba,swi,bios" \
  "testsuite,gba,swi,bios" \
  "conformance,gba,swi,bios" || true
link_if_exists "${GEN_DIR}/gen_swi_bios.gba" "${CONF_ROOT}/GBA/SWI/mgba-gba-swi-bios-pass.gba"
link_if_exists "${GEN_DIR}/gen_swi_bios_divarm.gba" "${CONF_ROOT}/GBA/SWI/mgba-gba-swi-bios-divarm-pass.gba"
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/SWI/COMPAT" "mgba-gba-swi-compat-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,swi,compat" \
  "gba-tests,gba,swi,compat" \
  "testsuite,gba,swi,compat" \
  "conformance,gba,swi,compat" || true
link_if_exists "${GEN_DIR}/gen_swi_compat.gba" "${CONF_ROOT}/GBA/SWI/COMPAT/mgba-gba-swi-compat-pass.gba"
link_if_exists "${GEN_DIR}/gen_swi_compat_fastset.gba" "${CONF_ROOT}/GBA/SWI/COMPAT/mgba-gba-swi-compat-fastset-pass.gba"
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/GBA/SWI/REALHW" "mgba-gba-swi-realhw-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,gba,swi,realhw" \
  "mgba,gba,swi,realbios" \
  "gba-tests,gba,swi,realhw" \
  "gba-tests,gba,swi,realbios" \
  "testsuite,gba,swi,realhw" \
  "testsuite,gba,swi,realbios" \
  "conformance,gba,swi,realhw" \
  "conformance,gba,swi,realbios" || true
link_if_exists "${GEN_DIR}/gen_swi_bios.gba" "${CONF_ROOT}/GBA/SWI/REALHW/mgba-gba-swi-realhw-pass.gba"
link_if_exists "${GEN_DIR}/gen_swi_bios_divarm.gba" "${CONF_ROOT}/GBA/SWI/REALHW/mgba-gba-swi-realhw-divarm-pass.gba"

link_if_exists "${ROOT}/GBC/PAC-MAN (PD) [C].GBC" "${CONF_ROOT}/GBC/PPU/pacman-gbc-ppu.gbc"
rm -f "${CONF_ROOT}/GB/TIMER/IRQ"/*
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/intr_timing.gb" \
  "${CONF_ROOT}/GB/TIMER/IRQ/mooneye-intr-timing-gb-timer-irq.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/timer/div_write.gb" \
  "${CONF_ROOT}/GB/TIMER/IRQ/mooneye-timer-div-write-gb-timer-irq.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/timer/tima_reload.gb" \
  "${CONF_ROOT}/GB/TIMER/IRQ/mooneye-timer-tima-reload-gb-timer-irq.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/halt_ime0_nointr_timing.gb" \
  "${CONF_ROOT}/GB/TIMER/IRQ/mooneye-halt-ime0-nointr-gb-timer-irq.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/halt_ime1_timing.gb" \
  "${CONF_ROOT}/GB/TIMER/IRQ/mooneye-halt-ime1-timing-gb-timer-irq.gb"
link_first_by_path_pattern "${ROOT}" "*blargg*/interrupt_time/interrupt_time.gb" \
  "${CONF_ROOT}/GB/TIMER/IRQ/blargg-interrupt-time-gb-timer-irq.gb"
link_first_by_path_pattern "${ROOT}" "*blargg*/halt_bug.gb" \
  "${CONF_ROOT}/GB/TIMER/IRQ/blargg-halt-bug-gb-timer-irq.gb"

# Smoke path staging: verdict-focused GB smoke links + quick GBA smoke links.
rm -f "${CONF_ROOT}/SMOKE"/*
# Blargg curated smoke subset (smoke pack consumes instr_timing verdict path).
link_first_by_path_pattern "${ROOT}" "*blargg*/cpu_instrs/cpu_instrs.gb" \
  "${CONF_ROOT}/SMOKE/blargg-cpu-instrs-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*blargg*/instr_timing/instr_timing.gb" \
  "${CONF_ROOT}/SMOKE/blargg-instr-timing-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*blargg*/interrupt_time/interrupt_time.gb" \
  "${CONF_ROOT}/SMOKE/blargg-interrupt-time-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*blargg*/mem_timing-2/mem_timing.gb" \
  "${CONF_ROOT}/SMOKE/blargg-mem-timing-2-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*blargg*/halt_bug.gb" \
  "${CONF_ROOT}/SMOKE/blargg-halt-bug-smoke.gb"

# Mooneye curated smoke subset.
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/div_timing.gb" \
  "${CONF_ROOT}/SMOKE/mooneye-div-timing-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/intr_timing.gb" \
  "${CONF_ROOT}/SMOKE/mooneye-intr-timing-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/timer/div_write.gb" \
  "${CONF_ROOT}/SMOKE/mooneye-timer-div-write-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/timer/tima_reload.gb" \
  "${CONF_ROOT}/SMOKE/mooneye-timer-tima-reload-smoke.gb"
link_first_by_path_pattern "${ROOT}" "*mooneye*/acceptance/ppu/stat_irq_blocking.gb" \
  "${CONF_ROOT}/SMOKE/mooneye-ppu-stat-irq-blocking-smoke.gb"

# Keep GBA smoke quick by linking only a few local GBA ROMs (gba-smoke pack).
link_many_gba_by_any_token_set "${ROOT}" "${CONF_ROOT}/SMOKE" "mgba-gba-smoke-ext" "${GBA_EXTERNAL_MAX_PER_PACK}" \
  "mgba,conformance,smoke,gba" \
  "gba-tests,conformance,smoke,gba" \
  "testsuite,conformance,smoke,gba" || true
link_if_exists "${GEN_DIR}/gen_smoke.gba" "${CONF_ROOT}/SMOKE/mgba-gba-smoke-pass.gba"
link_if_exists "${GEN_DIR}/gen_smoke_io.gba" "${CONF_ROOT}/SMOKE/mgba-gba-smoke-io-pass.gba"
link_many_from_dir "${ROOT}/GBA" "${CONF_ROOT}/SMOKE" "gba-smoke" 3

echo "Conformance seed complete: ${linked} ROM link(s) refreshed under ${CONF_ROOT}"
echo "Use: GBEMU_RUN_CONFORMANCE=1 GBEMU_CONFORMANCE_PACKS=all ./build/tests/gbemu_tests"
