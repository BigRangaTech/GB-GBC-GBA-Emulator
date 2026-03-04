# Current Status

Updated: March 4, 2026

This file tracks the latest project capability snapshot. For chronological change history and test runs, see `CHANGELOG.md`.

## Core Emulation

- GB/GBC core: CPU, MMU, PPU, APU, input, SRAM/RTC handling.
- GBA core: ARM/THUMB CPU, memory bus, timers, DMA, IRQs, and video modes 0/1/2/3/4/5 (mixed maturity by feature path).

## Compatibility Highlights

- GB: LCD STAT IRQ edge/blocking behavior and CB-prefixed `BIT n,(HL)` timing corrected.
- GBA CPU: ARM `SWP/SWPB` support; long multiply family (`UMULL/UMLAL/SMULL/SMLAL`) and `RRX` register-offset edge behavior for LDR/STR handled.
- GBA IRQ: IE/IF/IME writable-bit masking aligned to hardware behavior.
- GBA timing: `WAITCNT` non-sequential/sequential waits, prefetch modeling, branch/interworking refill penalties, stream invalidation on Game Pak data access, `WAITCNT` writes, IRQ service, and DMA bus-master activity.
- GBA memory timing: Game Pak data-burst nseq->seq progression with stream reset on invalidation events.
- GBA PPU: Mode 5 bitmap rendering/page select with improved window/effect-mask and alpha-blend clamping behavior.
- GBA save protocols: Flash ID/program/erase/bank commands and EEPROM serial read/write command flows.
- GBA SWI/HLE: `Div`, `DivArm`, `Sqrt`, `ArcTan`, `ArcTan2`, `CpuSet`, `CpuFastSet`, `LZ77UnCompWram/Vram`, `RLUnCompWram/Vram`, `BgAffineSet`, `ObjAffineSet`, `Halt`, `Stop`; risky paths can fall back to real BIOS execution.
- GBA conformance verdict plumbing: mGBA debug output channel (`0x04FFF600`) captured for text-verdict ROM runs.

## Frontend

- SDL2 + Vulkan frontend options for Linux.
- Runtime backend policy: Vulkan is the default for ROM execution and launcher UI.
- Automatic fallback policy: when Vulkan is unavailable (or an SDL-only mode is requested), frontend falls back to SDL software rendering.
- Controller-first launcher UX for TV/couch usage, including configurable launcher density (`Auto`, `TV Compact`, `TV Large`) with persisted UI state.
- Vulkan runtime path now supports aspect-correct letterboxed integer upscaling (instead of stretch) and filter modes `none`, `scanlines`, `lcd`, and `crt`.
- Vulkan launcher path is implemented: `--renderer vulkan --launcher` runs in Vulkan when available and falls back to SDL software only when Vulkan init/probe fails.
- Vulkan runtime now includes pause-menu/HUD/help overlays with keyboard and controller navigation parity (`F10`/`Guide` menu, `F3` help, `F4` HUD).
- UI smoke now includes Vulkan runtime scripted parity checks for menu/HUD toggles and controller-driven pause menu navigation.

## Saves and Save States

- Save states:
  - GB/GBC save/load supported.
  - GBA save/load supported (state format version 5).
- Cartridge save persistence:
  - GB/GBC battery saves supported.
  - GBA save backing auto-detected via ROM tags (`SRAM_V`, `FLASH*_V`, `EEPROM_V`) and persisted via `.sav`.

## Test Coverage Snapshot

- Local CTest target covers ROM/config/core sanity checks, GBA state roundtrip, opcode coverage, timing regressions, IRQ edge behavior, PPU regressions, save-protocol behavior, save-state compatibility checks, verdict plumbing, and conformance pack/baseline logic.

## Immediate Engineering Priorities

1. Expand CPU/MMU/PPU/APU conformance tests with known ROM suites.
2. Tighten GBA compatibility for broader commercial/game-engine ROM coverage.
3. Add explicit save-state compatibility tests across format versions.
4. Keep docs synchronized whenever behavior/CLI/state format changes.
