# GB/GBC/GBA Emulator

C++20 emulator targeting Nintendo Game Boy (DMG), Game Boy Color (CGB), and Game Boy Advance (GBA), with a shared core and SDL2 frontends.

## Current Status (Updated March 2, 2026)

- GB/GBC core: CPU, MMU, PPU, APU, input, SRAM/RTC handling.
- GBA core: ARM/THUMB CPU, memory bus, timers, DMA, IRQs, and mode 0/1/2/3/4/5 video paths (with mixed maturity).
- GBA compatibility: ARM `SWP/SWPB` instruction support added.
- GBA compatibility: ARM long multiply family (`UMULL/UMLAL/SMULL/SMLAL`) and register-offset `RRX` edge behavior for LDR/STR are now handled.
- GBA compatibility: timer reload/cascade behavior and DMA start/repeat/IRQ handling tightened for better boot/runtime stability.
- GBA compatibility: PPU mode coverage includes Mode 5 bitmap rendering/page select, with improved window/effect-mask and alpha-blend clamping behavior.
- GBA compatibility: cartridge save protocol handling expanded for Flash (ID/program/erase/bank commands) and EEPROM serial read/write command flows.
- GBA compatibility: HLE SWI coverage expanded with `Div`, `DivArm`, `Sqrt`, `BgAffineSet`, and `ObjAffineSet` (with divide-by-zero delegated to BIOS behavior).
- Frontend: SDL2 launcher and runtime options for Linux.
- Save states:
  - GB/GBC save/load supported.
  - GBA save/load supported (state file format version 5).
- Cartridge save persistence:
  - GB/GBC battery saves: supported.
  - GBA cartridge save backing store: auto-detected from ROM tags (`SRAM_V`, `FLASH*_V`, `EEPROM_V`) and persisted via `.sav`.
- Tests: local CTest target with ROM/config/core sanity checks, GBA state roundtrip, opcode coverage (`SWP/SWPB`, long multiply, `RRX` offset behavior), HLE SWI coverage (`Div`, `DivArm`, `Sqrt`, `BgAffineSet`, `ObjAffineSet`, `CpuSet`, `CpuFastSet`), timer/DMA edge-case coverage (cascade/repeat/IRQ/start-mode behavior), PPU coverage (Mode 5/page select + window/effect-mask corner behavior), Flash/EEPROM save-protocol coverage, save persistence API checks, save-state version compatibility checks, GB serial/verdict plumbing, EI interrupt-delay regression coverage, and conformance pack selection/verdict/baseline regression checks.

## Build

```bash
cmake -S . -B build -DGBEMU_BUILD_TESTS=ON
cmake --build build -j
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Optional conformance harness (ROM-suite based, off by default):

```bash
GBEMU_RUN_CONFORMANCE=1 \
GBEMU_CONFORMANCE_PACKS=smoke \
GBEMU_CONFORMANCE_MAX_PER_CASE=3 \
./build/tests/gbemu_tests
```

- `GBEMU_CONFORMANCE_PACKS` supports targeted groups (default: `smoke`): `smoke`, `gba-cpu`, `gba-dma-timer`, `gba-ppu`, `gba-swi-bios`, `gbc-ppu`, `gb-timer-irq`, or `all`.
- `GBEMU_CONFORMANCE_ROOT` overrides the scan root (default: `Test-Games`).
- `GBEMU_CONFORMANCE_FRAME_LIMIT` optionally overrides per-ROM frame budget.
- Verdict-aware checks are enabled for known suites (`blargg`, `mooneye`): explicit fail signals and missing verdicts are tracked as conformance results (not just CPU faults).
- `GBEMU_CONFORMANCE_BASELINE_FILE` + `GBEMU_CONFORMANCE_ENFORCE_BASELINE=1` enforce per-pack regression thresholds.
- `GBEMU_CONFORMANCE_REPORT_PATH` writes CSV case/pack results for CI artifacts.
- Pack definitions and recommended ROM folder layout are documented in `tests/conformance_packs.md`.
- To seed local pack folders from existing ROMs, run `./tests/seed_conformance_packs.sh`.
- One-command seed + smoke + report: `./tests/run_seed_smoke_report.sh`.

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
