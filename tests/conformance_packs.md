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
- `GBEMU_CONFORMANCE_GBA_SWI_FORCE_REAL_BIOS=1|0` (default: `0`, force real BIOS SWI execution for `gba-swi-bios`/`gba-swi-compat`)
- `GBEMU_CONFORMANCE_DEBUG_TRACE=1|0` (default: `0`, include GB trace tails on verdict debug lines)
- `GBEMU_CONFORMANCE_TIGHTEN_BASELINE=1|0` (runner helper only: tighten baseline thresholds from the produced report)

## Available Packs

- `smoke`: verdict-focused quick pass (`blargg` instr-timing + curated `mooneye`)
- `gba-smoke`: quick GBA verdict-focused smoke (`mgba`-tagged text-verdict ROMs)
- `gba-cpu`: GBA ARM/THUMB instruction-focused ROMs
- `gba-dma-timer`: GBA DMA/timer behavior ROMs
- `gba-mem-timing`: GBA Game Pak memory waitstate/prefetch timing ROMs
- `gba-ppu`: GBA PPU/video mode/window/blending ROMs
- `gba-swi-bios`: GBA SWI/BIOS behavior ROMs
- `gba-swi-compat`: GBA SWI compatibility-focused ROMs
- `gba-swi-realbios`: GBA SWI ROMs that always run through real BIOS paths (no SWI HLE override)
- `gbc-ppu`: GBC video/PPU behavior ROMs
- `gb-timer-irq`: GB timer/interrupt behavior ROMs
- `all`: shorthand to run all packs

## File Matching

Each conformance case matches ROMs by required lowercase path tokens.
Examples:

- `gba-smoke`: path should include `conformance`, `smoke`, `gba`, `mgba`
- `gba-cpu`: path should include `gba`, `cpu`, `mgba`, and (`arm` or `thumb` depending on case)
- `gba-dma-timer`: path should include `gba`, `dma`, `timer`, `mgba`
- `gba-mem-timing`: path should include `gba`, `mem`, `timing`, `mgba`
- `gba-ppu`: path should include `gba`, `ppu`, `mgba`
- `gba-swi-bios`: path should include `gba`, `swi`, `bios`, `mgba`
- `gba-swi-compat`: path should include `gba`, `swi`, `compat`, `mgba`
- `gba-swi-realbios`: path should include `gba`, `swi`, `realhw`, `mgba`

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
- `Test-Games/Conformance/GBA/SWI/REALHW/*.gba`
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

Fetch/update known external GBA test repos and stage token-matching candidates:

```bash
./tests/fetch_external_gba_conformance_roms.sh
```

Optional root override:

```bash
./tests/seed_conformance_packs.sh /path/to/rom-root
```

The helper seeds the `gb-timer-irq` pack using targeted mooneye/blargg
timer/interrupt/halt ROMs, and also seeds currently
known local GBA/GBC examples into their matching feature packs.
It also generates deterministic per-pack GBA verdict ROMs
(`tests/generate_gba_conformance_roms.sh`) and links them as fallback coverage
for each GBA pack when external verdict ROMs are unavailable.
Current generated coverage targets at least two deterministic verdict ROMs per
GBA pack so baseline gating can enforce more than single-ROM smoke checks.
Seeding now also tries `gba-tests`/`testsuite` token patterns so real external
GBA verdict ROMs can populate pack links even if source filenames
do not include `mgba`. For each GBA pack, up to
`GBEMU_CONFORMANCE_GBA_EXTERNAL_MAX_PER_PACK` external candidates are linked
(default: `4`), plus deterministic generated fallback ROMs.
The fetch helper stages likely verdict-capable candidates by default; set
`GBEMU_FETCH_INCLUDE_NON_VERDICT=1` to include non-verdict tests too.
By default, seeding excludes `<root>/external/sources` (raw cloned repo trees)
to avoid accidental conformance pollution; set
`GBEMU_CONFORMANCE_INCLUDE_EXTERNAL_SOURCES=1` to include that subtree.

For `gba-swi-bios` and `gba-swi-compat`, the harness enables HLE SWI mode during
conformance execution so verdict ROM behavior stays deterministic even with
varying local BIOS files. `gba-swi-realbios` always runs real BIOS SWI paths.
Set `GBEMU_CONFORMANCE_GBA_SWI_FORCE_REAL_BIOS=1` to force real BIOS SWI paths
for A/B comparison runs across the HLE SWI packs.
The generated `gba-swi-compat` ROM now reloads destination base after `CpuSet`
for validation, so it remains valid when real BIOS post-increments `R0/R1`.

For `smoke`, it links a curated fast subset:

- Blargg: `instr_timing`
- Mooneye: `div_timing`, `intr_timing`, `timer/div_write`, `timer/tima_reload`, `ppu/stat_irq_blocking`
- For `gba-smoke`: up to 3 local GBA ROMs from `<root>/GBA/`

## Baseline Gating

`tests/conformance_pack_baseline.csv` defines per-pack minimum execution/pass and
maximum fail/unknown counts (including stricter multi-ROM minimums for GBA packs):

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

Seed packs + run GBA packs + write report:

```bash
./tests/run_seed_gba_report.sh
```

Seed + run GBA packs + tighten baseline thresholds from that report:

```bash
GBEMU_CONFORMANCE_TIGHTEN_BASELINE=1 ./tests/run_seed_gba_report.sh
```

Seed packs + run SWI packs in A/B mode (default SWI policy vs forced real BIOS),
then emit a CSV diff:

```bash
./tests/run_seed_gba_swi_ab_report.sh
```
