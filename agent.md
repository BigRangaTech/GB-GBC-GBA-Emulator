# GB/GBC/GBA Emulator Engineering Notes

## Project Goals

- Accurate and performant emulation for GB, GBC, and GBA.
- Shared C++ core with thin platform frontends.
- Linux-first workflow with Android path kept in tree.
- Deterministic emulation loop suitable for regression testing.

## Current Implementation Snapshot (Updated March 2, 2026)

### Core

- `core/` includes:
  - GB/GBC CPU (`Cpu`), MMU (`Mmu`), PPU (`Ppu`), APU (`Apu`)
  - GBA CPU (`GbaCpu`), bus (`GbaBus`), and system integration (`GbaCore`)
  - top-level orchestrator (`EmulatorCore`)
- Boot ROM-aware startup for GB/GBC/GBA.
- GBA debug features include traces, IO/memory watches, HLE SWI options, and optional auto patching helpers.
- GBA cartridge save type tags are detected from ROM content and mapped to persistent save buffers.
- GBA ARM `SWP/SWPB` decode/execute path is implemented.
- GBA ARM long multiply instructions (`UMULL/UMLAL/SMULL/SMLAL`) are implemented.
- GBA ARM register-offset load/store now handles `ROR #0` as `RRX` (carry-in sensitive addressing edge case).
- GBA timer behavior now tracks reload vs counter mirror correctly and supports multi-overflow cascade propagation in a single step window.
- GBA DMA behavior now tightens channel count masking, transfer alignment, repeat/destination reload handling, and IRQ signaling side effects.
- GBA PPU now includes Mode 5 bitmap path support (including frame page selection) and tighter window/effect-mask handling in blend decisions.
- GBA save protocol emulation now covers Flash command sequences (ID/program/erase/bank) and EEPROM serial command/readback flows.
- GBA HLE SWI coverage now includes `Div`, `DivArm`, `Sqrt`, `BgAffineSet`, and `ObjAffineSet` in addition to existing wait/memcpy paths.

### Frontend

- `frontend/main.cpp` provides SDL2 app + launcher UX and CLI options.
- Vulkan path is present (`gbemu_vk`) alongside the SDL path.

### State System

- Save-state header: `GBST`.
- Format versions:
  - v4: legacy GB/GBC states
  - v5: GB/GBC + GBA states
- GBA state now serializes and restores:
  - CPU internal and banked register state
  - bus memory/runtime flags plus save RAM state
  - core timing, DMA/timer state, watchdog/debug runtime state, framebuffer

### Conformance Harness

- `tests/tests.cpp` includes an opt-in ROM conformance harness with pack-based targeting.
- Enable with `GBEMU_RUN_CONFORMANCE=1`.
- Select packs with `GBEMU_CONFORMANCE_PACKS` (default `smoke`, supports `all`).
- Limit per-case samples with `GBEMU_CONFORMANCE_MAX_PER_CASE` (default 5).
- Optional `GBEMU_CONFORMANCE_FRAME_LIMIT` overrides frame budget per ROM.
- Override ROM scan root with `GBEMU_CONFORMANCE_ROOT` (default `Test-Games`).
- Known suite verdict checks (`blargg`, `mooneye`) require explicit pass/fail signals.
- Baseline regression gating is enabled via `tests/conformance_pack_baseline.csv` and
  `GBEMU_CONFORMANCE_ENFORCE_BASELINE=1`.
- CSV reports can be emitted with `GBEMU_CONFORMANCE_REPORT_PATH`.
- Shortcut runner: `tests/run_seed_smoke_report.sh` (seed + smoke + report).
- Pack definitions and layout guidance are documented in `tests/conformance_packs.md`.

## Build/Test Workflow

```bash
cmake -S . -B build -DGBEMU_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Testing Coverage Highlights

- File/config parsing sanity tests.
- Framebuffer dimension checks (GB/GBC/GBA).
- Boot ROM requirement checks.
- GB serial transfer capture regression check.
- GB EI one-instruction IME delay regression check.
- GBA save-state roundtrip test (`save -> mutate -> load -> save` equality).
- GBA opcode regression checks for `SWP/SWPB`.
- GBA opcode/edge regression checks for long multiply and `RRX` offset addressing behavior.
- GBA HLE SWI regression checks for arithmetic (`Div/DivArm/Sqrt`), affine setup (`BgAffineSet/ObjAffineSet`), and memcpy paths (`CpuSet/CpuFastSet`).
- GBA timer/DMA regression checks for counter mirror/reload stability, cascade overflows, repeat mode, start timing behavior, and DMA IRQ side effects.
- GBA PPU regression checks for Mode 5 direct-color/page selection and window/effect-mask blending corner behavior.
- GBA save protocol regression checks for Flash + EEPROM command behavior.
- GBA save persistence API coverage (`has_battery/has_ram/ram_data/load_ram_data`).
- Save-state compatibility checks covering v4/v5 expectations.
- Conformance selector/token matching, verdict classification, and baseline/report plumbing regression coverage.

## Immediate Engineering Priorities

1. Expand CPU/MMU/PPU/APU conformance tests with known ROM suites.
2. Tighten GBA compatibility for broader commercial/game-engine ROM coverage.
3. Add explicit save-state compatibility tests across format versions.
4. Keep docs synchronized whenever behavior/CLI/state format changes.

## Documentation Maintenance Rule

For every behavior change, update in the same patch:

- `README.md` for user-facing behavior
- `agent.md` for architecture/status notes
- corresponding tests under `tests/`
