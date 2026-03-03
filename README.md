# GB/GBC/GBA Emulator

C++20 emulator targeting Nintendo Game Boy (DMG), Game Boy Color (CGB), and Game Boy Advance (GBA), with a shared core and SDL2 frontends.

## Current Status (Updated March 3, 2026)

- GB/GBC core: CPU, MMU, PPU, APU, input, SRAM/RTC handling.
- GBA core: ARM/THUMB CPU, memory bus, timers, DMA, IRQs, and mode 0/1/2/3/4/5 video paths (with mixed maturity).
- GBA compatibility: ARM `SWP/SWPB` instruction support added.
- GB compatibility: LCD STAT IRQ edge/blocking behavior and CB-prefixed `BIT n,(HL)` timing were corrected.
- GBA compatibility: ARM long multiply family (`UMULL/UMLAL/SMULL/SMLAL`) and register-offset `RRX` edge behavior for LDR/STR are now handled.
- GBA compatibility: timer reload/cascade behavior and DMA start/repeat/IRQ handling tightened for better boot/runtime stability.
- GBA compatibility: IE/IF/IME interrupt register masking now matches writable hardware bits, preventing invalid-bit IRQ side effects.
- GBA compatibility: Game Pak timing now applies `WAITCNT` non-sequential/sequential waits, with basic prefetch-queue modeling and branch/interworking refill penalties.
- GBA compatibility: writes to `WAITCNT` now flush fetch-stream/prefetch state so the next Game Pak fetch pays updated wait costs.
- GBA compatibility: Game Pak data accesses now invalidate fetch-stream/prefetch state so post-load instruction timing pays proper refill penalties.
- GBA compatibility: PPU mode coverage includes Mode 5 bitmap rendering/page select, with improved window/effect-mask and alpha-blend clamping behavior.
- GBA compatibility: cartridge save protocol handling expanded for Flash (ID/program/erase/bank commands) and EEPROM serial read/write command flows.
- GBA compatibility: HLE SWI coverage expanded with `Div`, `DivArm`, `Sqrt`, `ArcTan`, `ArcTan2`, `CpuSet`, `CpuFastSet`, `LZ77UnCompWram/Vram`, `RLUnCompWram/Vram`, `BgAffineSet`, `ObjAffineSet`, `Halt`, and `Stop`; risky paths can now fall back to real BIOS execution.
- GBA compatibility: mGBA debug output channel (`0x04FFF600` region) is now captured for text verdict-driven conformance runs.
- Frontend: SDL2 launcher and runtime options for Linux.
- Save states:
  - GB/GBC save/load supported.
  - GBA save/load supported (state file format version 5).
- Cartridge save persistence:
  - GB/GBC battery saves: supported.
  - GBA cartridge save backing store: auto-detected from ROM tags (`SRAM_V`, `FLASH*_V`, `EEPROM_V`) and persisted via `.sav`.
- Tests: local CTest target with ROM/config/core sanity checks, GBA state roundtrip, opcode coverage (`SWP/SWPB`, long multiply, `RRX` offset behavior), GBA memory-timing regressions (`WAITCNT` nseq/seq fetch costs, prefetch benefit, branch/interworking refill penalties, fetch-stream break on Game Pak data access, and `WAITCNT`-write stream flush behavior), HLE SWI coverage (`Div`, `DivArm`, `Sqrt`, `ArcTan`, `ArcTan2`, `CpuSet`, `CpuFastSet`, `LZ77UnCompWram/Vram`, `RLUnCompWram/Vram`, `BgAffineSet`, `ObjAffineSet`, `Halt`, `Stop`, plus BIOS fallback behavior), timer/DMA edge-case coverage (cascade/repeat/IRQ/start-mode behavior), GBA IRQ entry regression coverage (IF acknowledge/priority/LR capture, THUMB-origin IRQ LR/pipeline behavior, and IE/IF/IME register masking), PPU coverage (Mode 5/page select + window/effect-mask corner behavior), Flash/EEPROM save-protocol coverage, save persistence API checks, save-state version compatibility checks, GB serial/verdict plumbing, GBA mGBA debug-output capture, EI interrupt-delay regression coverage, GB STAT IRQ blocking and CB `BIT n,(HL)` timing regressions, and conformance pack selection/verdict/baseline regression checks.

## Build

```bash
cmake -S . -B build -DGBEMU_BUILD_TESTS=ON
cmake --build build -j
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Optional top-level shortcuts:

```bash
make build
make test
make conformance-smoke
make conformance-all
```

Optional conformance harness (ROM-suite based, off by default):

```bash
GBEMU_RUN_CONFORMANCE=1 \
GBEMU_CONFORMANCE_PACKS=smoke \
GBEMU_CONFORMANCE_MAX_PER_CASE=3 \
./build/tests/gbemu_tests
```

- `GBEMU_CONFORMANCE_PACKS` supports targeted groups (default: `smoke`): `smoke`, `gba-smoke`, `gba-cpu`, `gba-dma-timer`, `gba-mem-timing`, `gba-ppu`, `gba-swi-bios`, `gba-swi-compat`, `gbc-ppu`, `gb-timer-irq`, or `all`.
- `GBEMU_CONFORMANCE_ROOT` overrides the scan root (default: `Test-Games`).
- `GBEMU_CONFORMANCE_FRAME_LIMIT` optionally overrides per-ROM frame budget.
- Verdict-aware checks are enabled for known suites (`blargg`, `mooneye`) and GBA text-verdict ROM paths (`.gba` files containing `mgba`, `gba-tests`, or `testsuite`): explicit fail signals and missing verdicts are tracked as conformance results (not just CPU faults).
- By default, mooneye ROMs prefer real boot ROM startup when available (`GBEMU_CONFORMANCE_MOONEYE_REAL_BOOT=1`) with a raised minimum frame budget (`GBEMU_CONFORMANCE_REAL_BOOT_MIN_FRAMES`, default `1200`).
- `GBEMU_CONFORMANCE_FORCE_SYNTH_BOOT=1` disables real-boot usage and forces synthetic boot paths.
- `GBEMU_CONFORMANCE_BASELINE_FILE` + `GBEMU_CONFORMANCE_ENFORCE_BASELINE=1` enforce per-pack regression thresholds.
- `GBEMU_CONFORMANCE_REPORT_PATH` writes CSV case/pack results for CI artifacts.
- Pack definitions and recommended ROM folder layout are documented in `tests/conformance_packs.md`.
- To seed local pack folders from existing ROMs, run `./tests/seed_conformance_packs.sh`.
- One-command seed + smoke + report: `./tests/run_seed_smoke_report.sh`.
- Equivalent make target: `make conformance-smoke` (or `make conformance-all`).

## Run

```bash
./build/frontend/gbemu --help
./build/frontend/gbemu --launcher
./build/frontend/gbemu --system gba --boot-rom-gba firmware/GBA/Game-Boy-Advance-Boot-ROM.bin <path-to-rom.gba>
```

## Firmware

The emulator expects boot ROMs to be provided by the user.

- GB: `firmware/GB/Game-Boy-Boot-ROM.gb`
- GBC: `firmware/GBC/Game-Boy-Color-Boot-ROM.gbc`
- GBA: `firmware/GBA/Game-Boy-Advance-Boot-ROM.bin` (16 KB)

You can override with CLI options (`--boot-rom`, `--boot-rom-gb`, `--boot-rom-gbc`, `--boot-rom-gba`) or `gbemu.conf`.

## Save State Compatibility

- Version 4: GB/GBC only.
- Version 5: GB/GBC + GBA.
- GBA states include CPU/bus/framebuffer runtime data and save RAM contents.

## Documentation Policy

When behavior, CLI options, or state format changes, update at least:

- `README.md`
- `agent.md`
- tests that cover the changed behavior

## License

This project is licensed under **GNU GPL v2 or later** (`GPL-2.0-or-later`).

- See `LICENSE` for the project license declaration.
- See `COPYING` for the full GNU GPL v2 text.
