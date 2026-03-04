#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

ROM_ROOT="${1:-Test-Games}"
EXE="${GBEMU_UI_SMOKE_EXE:-./build/frontend/gbemu}"
TIMEOUT_SEC="${GBEMU_UI_SMOKE_TIMEOUT:-6}"
REQUIRE_VULKAN="${GBEMU_UI_SMOKE_REQUIRE_VULKAN:-0}"
DRIVER_MATRIX="${GBEMU_UI_SMOKE_DRIVER_MATRIX:-0}"

if [[ ! -x "$EXE" ]]; then
  echo "UI smoke FAIL: executable not found or not executable: $EXE" >&2
  echo "Build first: cmake --build build -j" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

normalize_name() {
  echo "$1" | tr ' /' '__'
}

run_case() {
  local name="$1"
  shift
  local log="$TMP_DIR/$(normalize_name "$name").log"
  local code=0
  if timeout "${TIMEOUT_SEC}s" "$@" >"$log" 2>&1; then
    code=0
  else
    code=$?
  fi

  if [[ $code -eq 0 || $code -eq 124 ]]; then
    echo "[PASS] $name"
    PASS_COUNT=$((PASS_COUNT + 1))
    return 0
  fi

  echo "[FAIL] $name (exit=$code)"
  tail -n 30 "$log" | sed 's/^/  /'
  FAIL_COUNT=$((FAIL_COUNT + 1))
  return 1
}

run_vulkan_case() {
  local name="$1"
  shift
  local log="$TMP_DIR/$(normalize_name "$name").log"
  local code=0
  if timeout "${TIMEOUT_SEC}s" "$@" >"$log" 2>&1; then
    code=0
  else
    code=$?
  fi

  if [[ $code -eq 0 || $code -eq 124 ]]; then
    if grep -q "Window created (Vulkan)." "$log"; then
      echo "[PASS] $name"
      PASS_COUNT=$((PASS_COUNT + 1))
      return 0
    fi
    if grep -Eiq "No Vulkan physical devices found|No suitable Vulkan GPU found|Failed to create Vulkan window|SDL_Vulkan_|vkCreateInstance failed|Failed to initialize Vulkan renderer|Falling back to SDL software renderer" "$log"; then
      if [[ "$REQUIRE_VULKAN" == "1" ]]; then
        echo "[FAIL] $name (Vulkan unavailable and required)"
        tail -n 30 "$log" | sed 's/^/  /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return 1
      fi
      echo "[SKIP] $name (Vulkan unavailable in this environment)"
      SKIP_COUNT=$((SKIP_COUNT + 1))
      return 2
    fi
    echo "[FAIL] $name (missing Vulkan runtime signal)"
    tail -n 30 "$log" | sed 's/^/  /'
    FAIL_COUNT=$((FAIL_COUNT + 1))
    return 1
  fi

  if grep -Eiq "No Vulkan physical devices found|No suitable Vulkan GPU found|Failed to create Vulkan window|SDL_Vulkan_|vkCreateInstance failed|Failed to initialize Vulkan renderer|Falling back to SDL software renderer" "$log"; then
    if [[ "$REQUIRE_VULKAN" == "1" ]]; then
      echo "[FAIL] $name (Vulkan unavailable and required)"
      tail -n 30 "$log" | sed 's/^/  /'
      FAIL_COUNT=$((FAIL_COUNT + 1))
      return 1
    fi
    echo "[SKIP] $name (Vulkan unavailable in this environment)"
    SKIP_COUNT=$((SKIP_COUNT + 1))
    return 2
  fi

  echo "[FAIL] $name (exit=$code)"
  tail -n 30 "$log" | sed 's/^/  /'
  FAIL_COUNT=$((FAIL_COUNT + 1))
  return 1
}

find_first_rom() {
  if [[ ! -d "$ROM_ROOT" ]]; then
    return 1
  fi
  find "$ROM_ROOT" \
    -type f \
    \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.gba' \) \
    ! -path '*/external/sources/*' \
    | sort | head -n 1
}

echo "UI smoke starting"
echo "  exe: $EXE"
echo "  rom root: $ROM_ROOT"

run_case "launcher-default" "$EXE" --launcher --rom-dir "$ROM_ROOT"
run_case "launcher-theme-retro" "$EXE" --launcher --ui-theme retro --rom-dir "$ROM_ROOT"
run_case "launcher-theme-minimal" "$EXE" --launcher --ui-theme minimal --rom-dir "$ROM_ROOT"
run_case "launcher-theme-deck" "$EXE" --launcher --ui-theme deck --rom-dir "$ROM_ROOT"

if [[ "$DRIVER_MATRIX" == "1" ]]; then
  run_case "launcher-wayland" "$EXE" --launcher --video-driver wayland --rom-dir "$ROM_ROOT"
  run_case "launcher-x11" "$EXE" --launcher --video-driver x11 --rom-dir "$ROM_ROOT"
fi

launcher_vk_log="$TMP_DIR/renderer_vulkan_launcher.log"
if timeout "${TIMEOUT_SEC}s" "$EXE" --renderer vulkan --launcher --rom-dir "$ROM_ROOT" >"$launcher_vk_log" 2>&1; then
  fallback_code=0
else
  fallback_code=$?
fi
if [[ $fallback_code -eq 0 || $fallback_code -eq 124 ]]; then
  if grep -q "Window created (Vulkan launcher)." "$launcher_vk_log"; then
    echo "[PASS] renderer-vulkan-launcher"
    PASS_COUNT=$((PASS_COUNT + 1))
  elif grep -q "Falling back to SDL software renderer." "$launcher_vk_log"; then
    echo "[PASS] renderer-vulkan-launcher-fallback"
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    echo "[FAIL] renderer-vulkan-launcher (missing Vulkan start/fallback signal)"
    tail -n 30 "$launcher_vk_log" | sed 's/^/  /'
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
else
  echo "[FAIL] renderer-vulkan-launcher (exit=$fallback_code)"
  tail -n 30 "$launcher_vk_log" | sed 's/^/  /'
  FAIL_COUNT=$((FAIL_COUNT + 1))
fi

ROM_PATH="$(find_first_rom || true)"
if [[ -z "$ROM_PATH" ]]; then
  echo "[SKIP] vulkan-filter-matrix (no ROM found under $ROM_ROOT)"
  SKIP_COUNT=$((SKIP_COUNT + 1))
else
  echo "  vulkan ROM: $ROM_PATH"
  if run_vulkan_case "vulkan-filter-none" "$EXE" --renderer vulkan --filter none "$ROM_PATH"; then
    run_vulkan_case "vulkan-filter-scanlines" "$EXE" --renderer vulkan --filter scanlines "$ROM_PATH" || true
    run_vulkan_case "vulkan-filter-lcd" "$EXE" --renderer vulkan --filter lcd "$ROM_PATH" || true
    run_vulkan_case "vulkan-filter-crt" "$EXE" --renderer vulkan --filter crt "$ROM_PATH" || true
  else
    rc=$?
    if [[ $rc -eq 2 ]]; then
      echo "[SKIP] vulkan-filter-matrix (skipped after unavailable probe)"
      SKIP_COUNT=$((SKIP_COUNT + 1))
    fi
  fi

  vk_menu_hud_log="$TMP_DIR/vulkan_ui_autotest_menu_hud.log"
  if GBEMU_VK_UI_AUTOTEST=menu-hud timeout "${TIMEOUT_SEC}s" "$EXE" --renderer vulkan --filter none "$ROM_PATH" >"$vk_menu_hud_log" 2>&1; then
    vk_menu_hud_code=0
  else
    vk_menu_hud_code=$?
  fi
  if [[ $vk_menu_hud_code -eq 0 || $vk_menu_hud_code -eq 124 ]]; then
    if grep -q "Window created (Vulkan)." "$vk_menu_hud_log"; then
      missing=0
      for pattern in \
        "Vulkan UI autotest sequence complete: menu-hud" \
        "HUD: OFF" \
        "HUD: ON" \
        "Help overlay: ON" \
        "Help overlay: OFF" \
        "Pause menu: OPEN" \
        "Pause menu: CLOSED"; do
        if ! grep -q "$pattern" "$vk_menu_hud_log"; then
          missing=1
          break
        fi
      done
      if [[ $missing -eq 0 ]]; then
        echo "[PASS] vulkan-ui-autotest-menu-hud"
        PASS_COUNT=$((PASS_COUNT + 1))
      else
        echo "[FAIL] vulkan-ui-autotest-menu-hud (missing expected log markers)"
        tail -n 40 "$vk_menu_hud_log" | sed 's/^/  /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
      fi
    elif grep -Eiq "No Vulkan physical devices found|No suitable Vulkan GPU found|Failed to create Vulkan window|SDL_Vulkan_|vkCreateInstance failed|Failed to initialize Vulkan renderer|Falling back to SDL software renderer" "$vk_menu_hud_log"; then
      if [[ "$REQUIRE_VULKAN" == "1" ]]; then
        echo "[FAIL] vulkan-ui-autotest-menu-hud (Vulkan unavailable and required)"
        tail -n 30 "$vk_menu_hud_log" | sed 's/^/  /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
      else
        echo "[SKIP] vulkan-ui-autotest-menu-hud (Vulkan unavailable in this environment)"
        SKIP_COUNT=$((SKIP_COUNT + 1))
      fi
    else
      echo "[FAIL] vulkan-ui-autotest-menu-hud (missing Vulkan runtime signal)"
      tail -n 40 "$vk_menu_hud_log" | sed 's/^/  /'
      FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
  else
    echo "[FAIL] vulkan-ui-autotest-menu-hud (exit=$vk_menu_hud_code)"
    tail -n 40 "$vk_menu_hud_log" | sed 's/^/  /'
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi

  vk_controller_menu_log="$TMP_DIR/vulkan_ui_autotest_controller_menu.log"
  if GBEMU_VK_UI_AUTOTEST=controller-menu timeout "${TIMEOUT_SEC}s" "$EXE" --renderer vulkan --filter none "$ROM_PATH" >"$vk_controller_menu_log" 2>&1; then
    vk_controller_menu_code=0
  else
    vk_controller_menu_code=$?
  fi
  if [[ $vk_controller_menu_code -eq 0 || $vk_controller_menu_code -eq 124 ]]; then
    if grep -q "Window created (Vulkan)." "$vk_controller_menu_log"; then
      missing=0
      for pattern in \
        "Vulkan UI autotest sequence complete: controller-menu" \
        "Pause menu: OPEN" \
        "Pause menu: CLOSED" \
        "Vulkan filter: SCANLINES"; do
        if ! grep -q "$pattern" "$vk_controller_menu_log"; then
          missing=1
          break
        fi
      done
      if [[ $missing -eq 0 ]]; then
        echo "[PASS] vulkan-ui-autotest-controller-menu"
        PASS_COUNT=$((PASS_COUNT + 1))
      else
        echo "[FAIL] vulkan-ui-autotest-controller-menu (missing expected log markers)"
        tail -n 40 "$vk_controller_menu_log" | sed 's/^/  /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
      fi
    elif grep -Eiq "No Vulkan physical devices found|No suitable Vulkan GPU found|Failed to create Vulkan window|SDL_Vulkan_|vkCreateInstance failed|Failed to initialize Vulkan renderer|Falling back to SDL software renderer" "$vk_controller_menu_log"; then
      if [[ "$REQUIRE_VULKAN" == "1" ]]; then
        echo "[FAIL] vulkan-ui-autotest-controller-menu (Vulkan unavailable and required)"
        tail -n 30 "$vk_controller_menu_log" | sed 's/^/  /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
      else
        echo "[SKIP] vulkan-ui-autotest-controller-menu (Vulkan unavailable in this environment)"
        SKIP_COUNT=$((SKIP_COUNT + 1))
      fi
    else
      echo "[FAIL] vulkan-ui-autotest-controller-menu (missing Vulkan runtime signal)"
      tail -n 40 "$vk_controller_menu_log" | sed 's/^/  /'
      FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
  else
    echo "[FAIL] vulkan-ui-autotest-controller-menu (exit=$vk_controller_menu_code)"
    tail -n 40 "$vk_controller_menu_log" | sed 's/^/  /'
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
fi

echo "UI smoke summary: pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"
if [[ $FAIL_COUNT -ne 0 ]]; then
  exit 1
fi
