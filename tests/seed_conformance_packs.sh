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
  "${CONF_ROOT}/GBC/PPU" \
  "${CONF_ROOT}/GB/TIMER/IRQ"

linked=0

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

# Seed currently available local ROMs into targeted feature buckets.
link_if_exists "${ROOT}/GBA/sips.gba" "${CONF_ROOT}/GBA/CPU/ARM/sips-gba-cpu-arm.gba"
link_if_exists "${ROOT}/GBA/aladdin.gba" "${CONF_ROOT}/GBA/CPU/THUMB/aladdin-gba-cpu-thumb.gba"
link_if_exists "${ROOT}/GBA/sips.gba" "${CONF_ROOT}/GBA/DMA/TIMER/sips-gba-dma-timer.gba"
link_if_exists "${ROOT}/GBA/sips.gba" "${CONF_ROOT}/GBA/MEM/TIMING/sips-gba-mem-timing.gba"
link_if_exists "${ROOT}/GBA/aladdin.gba" "${CONF_ROOT}/GBA/PPU/aladdin-gba-ppu.gba"
link_if_exists "${ROOT}/GBA/sips.gba" "${CONF_ROOT}/GBA/SWI/sips-gba-swi.gba"
link_if_exists "${ROOT}/GBA/sips.gba" "${CONF_ROOT}/GBA/SWI/COMPAT/sips-gba-swi-compat.gba"
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
link_many_from_dir "${ROOT}/GBA" "${CONF_ROOT}/SMOKE" "gba-smoke" 3

echo "Conformance seed complete: ${linked} ROM link(s) refreshed under ${CONF_ROOT}"
echo "Use: GBEMU_RUN_CONFORMANCE=1 GBEMU_CONFORMANCE_PACKS=all ./build/tests/gbemu_tests"
