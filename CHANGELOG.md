# Changelog

All notable project changes are tracked here with date/time and validation results.

## 2026-03-04 22:55:35 AEST (+1000)

### Changed
- Vulkan runtime UI parity improved:
  - Added runtime help/HUD overlays (`F3` help toggle, `F4` HUD toggle).
  - Added pause-menu entries for `HELP OVERLAY` and `HUD`.
  - Pause-menu footer/control hints now reflect controller support (`Guide`, `A`).
- Vulkan controller pause-menu navigation added:
  - `Guide` toggles pause menu open/close.
  - While paused: D-pad up/down navigation, left/right side actions, `A/Start` apply, `B/Back` close.
- Added deterministic Vulkan UI automation hooks for smoke testing:
  - `GBEMU_VK_UI_AUTOTEST=menu-hud`
  - `GBEMU_VK_UI_AUTOTEST=controller-menu`
- Expanded `tests/ui_smoke.sh`:
  - Vulkan runtime cases now require real Vulkan startup signal.
  - Added pass/fail checks for menu/HUD scripted sequence and controller-menu scripted navigation.
- Updated docs to reflect runtime control parity and smoke coverage (`README.md`, `currentstatus.md`, `agent.md`).

### Validation
- `cmake --build build -j4` passed.
- `ctest --test-dir build --output-on-failure -j4` passed (`1/1`).
- `./tests/ui_smoke.sh Test-Games` passed (`pass=11 fail=0 skip=0`).

## 2026-03-04 22:43:07 AEST (+1000)

### Changed
- Vulkan frontend launcher path is now implemented:
  - `--launcher` runs through Vulkan frontend when Vulkan is available.
  - `--rom-dir <path>` is supported in Vulkan launcher mode for ROM discovery.
- Frontend backend guardrails updated:
  - `--launcher` no longer forces SDL-only dispatch.
  - SDL software fallback still applies when Vulkan probe/init fails or SDL-only options are requested (`--headless`, `--gba-test`, boot ROM override flags, `--boot-trace`, `--cgb-color-correct`).
- UI smoke coverage updated:
  - `renderer-vulkan-launcher` now treats successful Vulkan launcher start as the primary pass condition, with fallback still accepted on systems without Vulkan.
- Documentation updated to match current behavior (`README.md`, `currentstatus.md`, `agent.md`).

### Validation
- `ctest --test-dir build --output-on-failure -j4` passed (`1/1`).
- `./tests/ui_smoke.sh Test-Games` passed (`pass=9 fail=0 skip=0`).
- Manual sanity check:
  - `timeout 5s ./build/frontend/gbemu --launcher --rom-dir Test-Games` reported `Window created (Vulkan launcher).`

## 2026-03-04 22:33:15 AEST (+1000)

### Changed
- Frontend backend policy updated:
  - Default backend is now Vulkan for ROM runtime.
  - Automatic fallback is now SDL software rendering when Vulkan is unavailable.
- Added SDL-only option guardrails for Vulkan default mode:
  - Requests using SDL-only paths (`--launcher`, `--headless`, `--gba-test`, `--rom-dir`, boot ROM override flags, and related SDL-only UI flags) now bypass Vulkan and fall back cleanly to SDL software.
- Added Vulkan probe path before Vulkan dispatch:
  - If Vulkan probe fails, runtime falls back to SDL software with an explicit reason message.
- SDL renderer creation in fallback path is now forced to software mode.
- Added config-file renderer parsing (`renderer=sdl|vulkan`) while keeping Vulkan as default when unset.

### Validation
- `cmake --build build -j4` passed.
- `ctest --test-dir build --output-on-failure -j4` passed (`1/1`).
- `./tests/ui_smoke.sh Test-Games` passed (`pass=9 fail=0 skip=0`).
- Manual sanity checks:
  - Default ROM run starts Vulkan path.
  - Default launcher run reports Vulkan bypass and starts SDL software path.
  - Default headless run reports Vulkan bypass and completes via SDL path.

## 2026-03-04 22:12:58 AEST (+1000)

### Changed
- Added `tests/ui_smoke.sh` for repeatable UI startup smoke checks across:
  - SDL launcher theme matrix
  - renderer fallback check (`--renderer vulkan --launcher`)
  - Vulkan runtime filter matrix (`none`, `scanlines`, `lcd`, `crt`)
- Added `make ui-smoke` shortcut in `Makefile`.
- Vulkan frontend filter behavior improved:
  - `--filter scanlines` and `--filter lcd` are now real filter modes (not aliases for `none`).
  - Filter cycling now correctly supports forward/backward traversal across all filter modes.
- Vulkan renderer scaling improved:
  - Added aspect-correct letterboxed presentation region.
  - Added integer upscaling when window size allows, removing stretched output distortion.
- Main frontend renderer dispatch improved:
  - `--renderer vulkan --launcher` now falls back to SDL launcher with an explicit message.

### Validation
- `cmake --build build -j4` passed.
- `ctest --test-dir build --output-on-failure -j4` passed (`1/1`).
- `./tests/ui_smoke.sh Test-Games` passed (`pass=9 fail=0 skip=0`).

## 2026-03-04 21:59:17 AEST (+1000)

### Changed
- Added `currentstatus.md` to keep a dedicated, tidy capability snapshot outside README.
- Reduced README's status section to pointers for:
  - `currentstatus.md` (current snapshot)
  - `CHANGELOG.md` (chronological updates + validation)
- Updated documentation maintenance rules in `README.md` and `agent.md` to include `currentstatus.md`.

### Validation
- Not run (documentation-only change).

## 2026-03-04 21:57:06 AEST (+1000)

### Changed
- Documentation workflow is now changelog-first for incremental updates:
  - Add dated behavior/test notes here in `CHANGELOG.md`.
  - Keep `README.md` focused on stable usage/build/run guidance.
- README now points readers to this file for chronological update history and validation status.

### Validation
- `cmake --build build -j4` passed.
- `ctest --test-dir build --output-on-failure -j4` passed (`1/1`).

## 2026-03-04 21:54:14 AEST (+1000)

### Changed
- Frontend launcher: added `Launcher Density` setting with three modes:
  - `Auto` (uses TV profile when window height is 900px or higher)
  - `TV Compact`
  - `TV Large`
- Launcher density mode is persisted in `saves/ui_state.conf` as `launcher_density`.
- Frontend launcher already in-progress from earlier 2026-03-04 work is now captured in one dated entry:
  - Controller-first gaming-mode layout (hero + horizontal carousel + action rail)
  - Library tabs (`All`, `Recents`, `Favorites`, `GB`, `GBC`, `GBA`)
  - Focus-driven navigation improvements for keyboard/controller/mouse
  - Smooth shelf/hero/action transitions
  - Controller-style glyph hints in action rail
  - 10-foot readability tuning (larger text/sizing and stronger focus contrast)

### Validation
- `cmake --build build -j4` passed.
- `ctest --test-dir build --output-on-failure -j4` passed (`1/1`).
