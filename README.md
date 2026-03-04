# GB/GBC/GBA Emulator

C++20 emulator targeting Nintendo Game Boy (DMG), Game Boy Color (CGB), and Game Boy Advance (GBA), with a shared core and SDL2 frontends.

## Current Status

- Detailed implementation status is maintained in `currentstatus.md`.
- Chronological change/test history is maintained in `CHANGELOG.md`.

## Build

```bash
cmake -S . -B build -DGBEMU_BUILD_TESTS=ON
cmake --build build -j
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Changelog

- See `CHANGELOG.md` for chronological updates with timestamp and validation results.

Optional top-level shortcuts:

```bash
make build
make test
make ui-smoke
make conformance-smoke
make conformance-gba
make conformance-all
```

Optional conformance harness (ROM-suite based, off by default):

```bash
GBEMU_RUN_CONFORMANCE=1 \
GBEMU_CONFORMANCE_PACKS=smoke \
GBEMU_CONFORMANCE_MAX_PER_CASE=3 \
./build/tests/gbemu_tests
```

UI launcher/render smoke check:

```bash
./tests/ui_smoke.sh Test-Games
```

- Equivalent make target: `make ui-smoke`.
- UI smoke also exercises Vulkan runtime overlays/controller menu navigation via scripted input (`GBEMU_VK_UI_AUTOTEST=menu-hud` and `GBEMU_VK_UI_AUTOTEST=controller-menu`).

- `GBEMU_CONFORMANCE_PACKS` supports targeted groups (default: `smoke`): `smoke`, `gba-smoke`, `gba-cpu`, `gba-dma-timer`, `gba-mem-timing`, `gba-ppu`, `gba-swi-bios`, `gba-swi-compat`, `gba-swi-realbios`, `gbc-ppu`, `gb-timer-irq`, or `all`.
- `GBEMU_CONFORMANCE_ROOT` overrides the scan root (default: `Test-Games`).
- `GBEMU_CONFORMANCE_FRAME_LIMIT` optionally overrides per-ROM frame budget.
- `GBEMU_CONFORMANCE_GBA_SWI_FORCE_REAL_BIOS=1` disables the SWI-pack HLE override and forces real BIOS SWI execution for `gba-swi-bios`/`gba-swi-compat` A/B comparisons.
- `gba-swi-realbios` always executes through real BIOS SWI paths (no SWI HLE override).
- Verdict-aware checks are enabled for known suites (`blargg`, `mooneye`) and GBA text-verdict ROM paths (`.gba` files containing `mgba`, `gba-tests`, or `testsuite`): explicit fail signals and missing verdicts are tracked as conformance results (not just CPU faults).
- GBA pack cases now target `mgba`-tagged verdict ROM paths by default, so GBA conformance baselines track real pass/fail instead of mostly `unknown`.
- By default, mooneye ROMs prefer real boot ROM startup when available (`GBEMU_CONFORMANCE_MOONEYE_REAL_BOOT=1`) with a raised minimum frame budget (`GBEMU_CONFORMANCE_REAL_BOOT_MIN_FRAMES`, default `1200`).
- `GBEMU_CONFORMANCE_FORCE_SYNTH_BOOT=1` disables real-boot usage and forces synthetic boot paths.
- `GBEMU_CONFORMANCE_BASELINE_FILE` + `GBEMU_CONFORMANCE_ENFORCE_BASELINE=1` enforce per-pack regression thresholds.
- `GBEMU_CONFORMANCE_REPORT_PATH` writes CSV case/pack results for CI artifacts.
- Pack definitions and recommended ROM folder layout are documented in `tests/conformance_packs.md`.
- Fetch/update external GBA test repos and stage token-matching candidates: `./tests/fetch_external_gba_conformance_roms.sh Test-Games`.
- To seed local pack folders from existing ROMs, run `./tests/seed_conformance_packs.sh` (this now generates multiple deterministic per-pack GBA verdict ROMs with built-in feature assertions, links those fallbacks when external verdict ROMs are missing, and imports up to `GBEMU_CONFORMANCE_GBA_EXTERNAL_MAX_PER_PACK` external verdict candidates per GBA pack via `mgba`/`gba-tests`/`testsuite` token matches).
- Preview which external GBA ROMs currently match each pack before seeding: `./tests/preview_gba_conformance_matches.sh Test-Games`.
- One-command seed + smoke + report: `./tests/run_seed_smoke_report.sh`.
- One-command seed + GBA packs + report: `./tests/run_seed_gba_report.sh`.
- Optional baseline ratchet on top of GBA run: `GBEMU_CONFORMANCE_TIGHTEN_BASELINE=1 ./tests/run_seed_gba_report.sh` (or `make conformance-gba-tighten`).
- One-command SWI A/B run (default policy vs forced real BIOS) + CSV diff: `./tests/run_seed_gba_swi_ab_report.sh`.
- Equivalent make targets: `make conformance-smoke`, `make conformance-gba`, `make conformance-gba-tighten`, `make conformance-gba-swi-ab`, and `make conformance-all`.

GBA auto-import token map (`tests/seed_conformance_packs.sh`, case-insensitive):
- Tokens can appear anywhere in the ROM path/filename; a ROM is imported when it matches any token group for that pack.
- `gba-smoke`: `mgba+conformance+smoke+gba` OR `gba-tests+conformance+smoke+gba` OR `testsuite+conformance+smoke+gba`
- `gba-cpu` ARM: `mgba+gba+cpu+arm` OR `gba-tests+gba+cpu+arm` OR `testsuite+gba+cpu+arm` OR `conformance+gba+cpu+arm`
- `gba-cpu` THUMB: `mgba+gba+cpu+thumb` OR `gba-tests+gba+cpu+thumb` OR `testsuite+gba+cpu+thumb` OR `conformance+gba+cpu+thumb`
- `gba-dma-timer`: `mgba+gba+dma+timer` OR `gba-tests+gba+dma+timer` OR `testsuite+gba+dma+timer` OR `conformance+gba+irq+timing` OR `conformance+gba+ime+pipeline`
- `gba-mem-timing`: `mgba+gba+mem+timing` OR `gba-tests+gba+mem+timing` OR `testsuite+gba+mem+timing` OR `conformance+gba+waitstate` OR `conformance+gba+prefetch`
- `gba-ppu`: `mgba+gba+ppu` OR `gba-tests+gba+ppu` OR `testsuite+gba+ppu` OR `conformance+gba+ppu`
- `gba-swi-bios`: `mgba+gba+swi+bios` OR `gba-tests+gba+swi+bios` OR `testsuite+gba+swi+bios` OR `conformance+gba+swi+bios`
- `gba-swi-compat`: `mgba+gba+swi+compat` OR `gba-tests+gba+swi+compat` OR `testsuite+gba+swi+compat` OR `conformance+gba+swi+compat`
- `gba-swi-realbios`: `mgba+gba+swi+realhw` OR `mgba+gba+swi+realbios` OR `gba-tests+gba+swi+realhw` OR `gba-tests+gba+swi+realbios` OR `testsuite+gba+swi+realhw` OR `testsuite+gba+swi+realbios` OR `conformance+gba+swi+realhw` OR `conformance+gba+swi+realbios`

Example:

```bash
./tests/fetch_external_gba_conformance_roms.sh Test-Games
GBEMU_CONFORMANCE_GBA_EXTERNAL_MAX_PER_PACK=8 ./tests/seed_conformance_packs.sh Test-Games
./tests/preview_gba_conformance_matches.sh Test-Games
```

Notes:
- `fetch_external_gba_conformance_roms.sh` defaults to staging only ROMs that look verdict-capable for this harness (mGBA debug signature + pass/fail-like strings).
- Set `GBEMU_FETCH_INCLUDE_NON_VERDICT=1` to also stage non-verdict tests, but expect higher `unknown` counts (which may fail baseline gating).
- `seed_conformance_packs.sh` and `preview_gba_conformance_matches.sh` exclude `Test-Games/external/sources` by default to avoid accidental pickup of raw cloned test trees; set `GBEMU_CONFORMANCE_INCLUDE_EXTERNAL_SOURCES=1` to include it intentionally.

Recommended filename examples (drop anywhere under `Test-Games/`):
- `mgba-gba-cpu-arm-alu-pass.gba`
- `mgba-gba-cpu-thumb-alu-pass.gba`
- `mgba-gba-dma-timer-irq-pass.gba`
- `mgba-gba-mem-timing-waitstate-pass.gba`
- `mgba-gba-ppu-mode5-pass.gba`
- `mgba-gba-swi-bios-pass.gba`
- `mgba-gba-swi-compat-pass.gba`
- `mgba-gba-swi-realhw-pass.gba` (or `mgba-gba-swi-realbios-pass.gba`)
- `mgba-conformance-smoke-gba-pass.gba` (for `gba-smoke`)

## Run

```bash
./build/frontend/gbemu --help
./build/frontend/gbemu --launcher
./build/frontend/gbemu --renderer vulkan --launcher --rom-dir Test-Games
./build/frontend/gbemu --system gba --boot-rom-gba firmware/GBA/Game-Boy-Advance-Boot-ROM.bin <path-to-rom.gba>
```

- Default render backend is Vulkan for ROM runtime and launcher UI.
- If Vulkan is unavailable, or SDL-only options are requested (`--headless`, `--gba-test`, boot ROM override flags, `--boot-trace`, `--cgb-color-correct`), the app automatically falls back to SDL software rendering.
- `--renderer sdl` forces SDL software rendering.

Launcher controls (`--launcher`):

- Keyboard: arrow keys navigate focus/shelf/tabs (`Up/Down` move between top controls, tab row, shelf, and action buttons; `Left/Right` moves inside focused row), `Enter`/`Space` activates focused item, `/` (or `Ctrl+F`) focuses search, `Backspace` edits search, `Tab` cycles library tabs, `Shift+Tab` cycles tabs backward, `[`/`]` changes theme, `S` opens settings, `F` toggles favorite, `O` toggles per-ROM override, `R` rescans ROMs, `Esc` backs out.
- Controller: D-pad navigates focus/shelf/tabs, `A`/`Start` activates focused item, `B` backs out, `X` cycles library tabs forward, `Y` toggles favorite, `LB`/`RB` cycle tabs backward/forward, `L3`/`R3` change theme, `Back` opens settings.
- Launcher density mode is configurable in Settings (`LAUNCHER DENSITY: AUTO / TV COMPACT / TV LARGE`).

Vulkan runtime controls:

- Keyboard: `F10` pause menu, `F3` help overlay toggle, `F4` HUD toggle, `F5` save state, `F9` load state, `Esc` quit.
- Controller: `Guide` toggles pause menu, D-pad navigates menu, `A`/`Start` applies menu action, `B`/`Back` closes menu.

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
- `CHANGELOG.md` (include date/time and test results)
- `currentstatus.md` (refresh capability snapshot/priorities when status shifts)
- `agent.md`
- tests that cover the changed behavior

## License

This project is licensed under **GNU GPL v2 or later** (`GPL-2.0-or-later`).

- See `LICENSE` for the project license declaration.
- See `COPYING` for the full GNU GPL v2 text.
