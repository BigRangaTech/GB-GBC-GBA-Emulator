# GB/GBC/GBA Emulator Engineering Notes

## Project Goals

- Accurate and performant emulation for GB, GBC, and GBA.
- Shared C++ core with thin platform frontends.
- Linux-first workflow with Android path kept in tree.
- Deterministic emulation loop suitable for regression testing.

## Current Implementation Snapshot (Updated March 3, 2026)

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
- GBA interrupt register handling now masks IE/IF/IME writes to valid hardware bits, preventing invalid-bit IRQ servicing.
- GBA Game Pak timing now applies `WAITCNT` non-sequential/sequential waitstate costs, with basic prefetch queue tracking and refill penalties after control-flow/mode changes.
- GBA timing now flushes fetch-stream/prefetch state on `WAITCNT` writes so timing changes take effect on the next Game Pak fetch.
- GBA timing now invalidates fetch-stream/prefetch state after Game Pak data accesses to model post-load refill penalties.
- GBA PPU now includes Mode 5 bitmap path support (including frame page selection) and tighter window/effect-mask handling in blend decisions.
- GBA save protocol emulation now covers Flash command sequences (ID/program/erase/bank) and EEPROM serial command/readback flows.
- GBA HLE SWI coverage now includes `Div`, `DivArm`, `Sqrt`, `ArcTan`, `ArcTan2`, `CpuSet`, `CpuFastSet`, `LZ77UnCompWram/Vram`, `RLUnCompWram/Vram`, `BgAffineSet`, `ObjAffineSet`, `Halt`, and `Stop`, with risky paths preferring real BIOS fallback when available.
- GBA bus now captures mGBA debug I/O text output (`0x04FFF600` region) for conformance verdict plumbing.
- GB PPU STAT IRQ behavior now models source-line edge/blocking semantics to prevent spurious retriggers.
- GB CB-prefixed `BIT n,(HL)` opcodes now use correct 12-cycle timing.

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
- Pack split note: `smoke` is verdict-focused GB quick coverage; quick non-verdict GBA checks live in `gba-smoke`.
- Limit per-case samples with `GBEMU_CONFORMANCE_MAX_PER_CASE` (default 5).
- Optional `GBEMU_CONFORMANCE_FRAME_LIMIT` overrides frame budget per ROM.
- Mooneye ROMs prefer real boot startup by default (`GBEMU_CONFORMANCE_MOONEYE_REAL_BOOT=1`)
  with elevated minimum frame budget (`GBEMU_CONFORMANCE_REAL_BOOT_MIN_FRAMES`, default 1200).
- `GBEMU_CONFORMANCE_FORCE_SYNTH_BOOT=1` disables real-boot usage.
- Override ROM scan root with `GBEMU_CONFORMANCE_ROOT` (default `Test-Games`).
- Known suite verdict checks (`blargg`, `mooneye`) plus GBA text-verdict path checks (`.gba` ROM path includes `mgba`/`gba-tests`/`testsuite`) require explicit pass/fail signals.
- Baseline regression gating is enabled via `tests/conformance_pack_baseline.csv` and
  `GBEMU_CONFORMANCE_ENFORCE_BASELINE=1`.
- CSV reports can be emitted with `GBEMU_CONFORMANCE_REPORT_PATH`.
- `GBEMU_CONFORMANCE_DEBUG_TRACE=1` prints GB trace tails and serial text for verdict debug.
- Shortcut runner: `tests/run_seed_smoke_report.sh` (seed + smoke + report).
- Top-level make shortcuts: `make build`, `make test`, `make conformance-smoke`, `make conformance-all`.
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
- GB IF/IE unused-bit behavior regression check.
- GB HALT resume-with-IME0 timing regression check.
- GB STAT IRQ blocking regression check across mode 0->1 transitions and re-arm behavior.
- GB CB-prefixed `BIT n,(HL)` cycle-timing regression check.
- GBA save-state roundtrip test (`save -> mutate -> load -> save` equality).
- GBA opcode regression checks for `SWP/SWPB`.
- GBA opcode/edge regression checks for long multiply and `RRX` offset addressing behavior.
- GBA HLE SWI regression checks for arithmetic (`Div/DivArm/Sqrt`), trigonometric helpers (`ArcTan/ArcTan2`), decompress paths (`LZ77`/`RL` WRAM+VRAM), affine setup (`BgAffineSet/ObjAffineSet`), memcpy paths (`CpuSet/CpuFastSet`), and BIOS fallback behavior for risky SWIs.
- GBA HLE SWI regression check for `Halt/Stop` handled-path behavior (halt latch + PC advance/cycle accounting).
- GBA timer/DMA regression checks for counter mirror/reload stability, cascade overflows, repeat mode, start timing behavior, and DMA IRQ side effects.
- GBA memory-timing regression checks for `WAITCNT` non-sequential/sequential fetch timing, prefetch gains, and refill penalties after branch/interworking boundaries.
- GBA memory-timing regression check for fetch-stream break/refill after Game Pak data loads.
- GBA memory-timing regression check for fetch-stream/prefetch flush on `WAITCNT` writes.
- GBA IRQ entry regression checks for IF acknowledge semantics, IRQ priority selection, and LR capture on exception entry (including THUMB-origin IRQ LR/pipeline edge behavior).
- GBA IRQ register masking regression check for IE/IF/IME writable-bit semantics.
- GBA PPU regression checks for Mode 5 direct-color/page selection and window/effect-mask blending corner behavior.
- GBA save protocol regression checks for Flash + EEPROM command behavior.
- GBA save persistence API coverage (`has_battery/has_ram/ram_data/load_ram_data`).
- Save-state compatibility checks covering v4/v5 expectations.
- Conformance selector/token matching, verdict classification, and baseline/report plumbing regression coverage.
- GBA mGBA debug text capture regression check feeding conformance verdict parsing.

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
