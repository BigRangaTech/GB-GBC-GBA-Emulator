# Conformance ROM Packs

The conformance harness (`tests/tests.cpp`) supports targeted pack selection via:

- `GBEMU_RUN_CONFORMANCE=1`
- `GBEMU_CONFORMANCE_PACKS=<pack[,pack...]>`
- `GBEMU_CONFORMANCE_MAX_PER_CASE=<n>` (default: `5`)
- `GBEMU_CONFORMANCE_FRAME_LIMIT=<n>` (optional override per-ROM frame budget)
- `GBEMU_CONFORMANCE_ROOT=<path>` (default: `Test-Games`)
- `GBEMU_CONFORMANCE_BASELINE_FILE=<path>` (default: `tests/conformance_pack_baseline.csv`)
- `GBEMU_CONFORMANCE_ENFORCE_BASELINE=1|0` (default: `1`)
- `GBEMU_CONFORMANCE_REPORT_PATH=<path>` (optional CSV output)
- `GBEMU_CONFORMANCE_MOONEYE_REAL_BOOT=1|0` (default: `1`, prefer real boot ROM for mooneye)
- `GBEMU_CONFORMANCE_REAL_BOOT_MIN_FRAMES=<n>` (default: `1200`, minimum budget when real boot is used)
- `GBEMU_CONFORMANCE_FORCE_SYNTH_BOOT=1|0` (default: `0`, disable real boot usage)
- `GBEMU_CONFORMANCE_DEBUG_TRACE=1|0` (default: `0`, include GB trace tails on verdict debug lines)

## Available Packs

- `smoke`: verdict-focused quick pass (`blargg` instr-timing + curated `mooneye`)
- `gba-smoke`: quick GBA boot/runtime smoke ROMs (typically non-verdict)
- `gba-cpu`: GBA ARM/THUMB instruction-focused ROMs
- `gba-dma-timer`: GBA DMA/timer behavior ROMs
- `gba-mem-timing`: GBA Game Pak memory waitstate/prefetch timing ROMs
- `gba-ppu`: GBA PPU/video mode/window/blending ROMs
- `gba-swi-bios`: GBA SWI/BIOS behavior ROMs
- `gba-swi-compat`: GBA SWI compatibility-focused ROMs
- `gbc-ppu`: GBC video/PPU behavior ROMs
- `gb-timer-irq`: GB timer/interrupt behavior ROMs
- `all`: shorthand to run all packs

## File Matching

Each conformance case matches ROMs by required lowercase path tokens.
Examples:

- `gba-cpu`: path should include `gba`, `cpu`, and (`arm` or `thumb` depending on case)
- `gba-dma-timer`: path should include `gba`, `dma`, `timer`
- `gba-mem-timing`: path should include `gba`, `mem`, `timing`
- `gba-ppu`: path should include `gba`, `ppu`
- `gba-swi-bios`: path should include `gba`, `swi`
- `gba-swi-compat`: path should include `gba`, `swi`, `compat`

Verdict parsing is path-aware:

- GB `blargg`/`mooneye` suites use serial-pattern verdict detection.
- GBA text verdict detection is enabled for `.gba` paths containing `mgba`, `gba-tests`, or `testsuite`; explicit pass/fail tokens are required to avoid `unknown`.

Recommended layout under `Test-Games/Conformance/`:

- `Test-Games/Conformance/GBA/CPU/ARM/*.gba`
- `Test-Games/Conformance/GBA/CPU/THUMB/*.gba`
- `Test-Games/Conformance/GBA/DMA/TIMER/*.gba`
- `Test-Games/Conformance/GBA/MEM/TIMING/*.gba`
- `Test-Games/Conformance/GBA/PPU/*.gba`
- `Test-Games/Conformance/GBA/SWI/*.gba`
- `Test-Games/Conformance/GBA/SWI/COMPAT/*.gba`
- `Test-Games/Conformance/GBC/PPU/*.gbc`
- `Test-Games/Conformance/GB/TIMER/IRQ/*.gb`

## Examples

```bash
GBEMU_RUN_CONFORMANCE=1 GBEMU_CONFORMANCE_PACKS=smoke ./build/tests/gbemu_tests
GBEMU_RUN_CONFORMANCE=1 GBEMU_CONFORMANCE_PACKS=gba-cpu,gba-dma-timer ./build/tests/gbemu_tests
GBEMU_RUN_CONFORMANCE=1 GBEMU_CONFORMANCE_PACKS=all GBEMU_CONFORMANCE_MAX_PER_CASE=2 ./build/tests/gbemu_tests
GBEMU_RUN_CONFORMANCE=1 GBEMU_CONFORMANCE_PACKS=all GBEMU_CONFORMANCE_REPORT_PATH=tests/conformance_all_report.csv ./build/tests/gbemu_tests
```

## Local Seeding Helper

If you already have ROMs under `Test-Games/`, seed the pack folders with symlinks:

```bash
./tests/seed_conformance_packs.sh
```

Optional root override:

```bash
./tests/seed_conformance_packs.sh /path/to/rom-root
```

The helper seeds the `gb-timer-irq` pack using targeted mooneye/blargg
timer/interrupt/halt ROMs, and also seeds currently
known local GBA/GBC examples into their matching feature packs.

For `smoke`, it links a curated fast subset:

- Blargg: `instr_timing`
- Mooneye: `div_timing`, `intr_timing`, `timer/div_write`, `timer/tima_reload`, `ppu/stat_irq_blocking`
- For `gba-smoke`: up to 3 local GBA ROMs from `<root>/GBA/`

## Baseline Gating

`tests/conformance_pack_baseline.csv` defines per-pack minimum execution/pass and
maximum fail/unknown counts:

- `min_executed`: guard against missing/renamed pack ROM coverage
- `min_pass`: guard against pass-count regressions
- `max_fail`: guard against new explicit fail verdicts
- `max_unknown`: guard against loss of known verdicts

When baseline enforcement is enabled (`GBEMU_CONFORMANCE_ENFORCE_BASELINE=1`),
the harness fails if any selected pack regresses against these thresholds.

## Shortcut

Seed packs + run smoke + write report in one command:

```bash
./tests/run_seed_smoke_report.sh
```
