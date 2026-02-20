# GB/GBC/GBA Emulator Plan (C++ | Linux + Android)

**Goals**
- Accurate, performant emulation for Game Boy, Game Boy Color, and Game Boy Advance.
- Linux desktop build and Android standalone APK.
- Shared core with thin platform frontends.
- Clean, testable architecture with reproducible builds.

**Non-Goals (for v1)**
- No online features (netplay, achievements, cloud sync).
- No BIOS distribution (user must provide BIOS or use HLE where possible).
- No dynamic recompilation (start with interpreter; consider JIT later).

**High-Level Architecture**
- `core/`: Pure emulation core, no platform dependencies.
- `frontend/`: Linux SDL2 frontend (window, input, audio, file picker).
- `android/`: NDK-based frontend, JNI glue, APK packaging.
- `common/`: Utilities (logging, config, file I/O, serialization).
- `tests/`: Unit tests + test ROM harness.

**Key Technical Decisions**
- C++20, CMake, clang/gcc on Linux, Android NDK with CMake toolchain.
- SDL2 for Linux frontend and audio; Android uses SDL2 or native AAudio/Oboe later.
- Fixed-point timing and cycle-accurate stepping where required for accuracy.
- Deterministic core (no platform timers inside emulation loop).

**Milestones**
1. Repo bootstrap
2. GB CPU + memory map
3. GB PPU + basic video output
4. GB input + audio (APU)
5. Save/load and battery-backed SRAM
6. GBC enhancements (double speed, palettes, VRAM banking)
7. GBA CPU (ARM7TDMI) + memory map
8. GBA PPU + basic video output
9. GBA APU (initial) + timers + DMA
10. Android frontend + packaging
11. Accuracy + compatibility pass

**Detailed Plan**
1. Repository setup
- Add CMake build with `core` library and `frontend` executable.
- Add formatting config and basic CI (build on Linux).
- Add dependency vendor instructions for SDL2.

2. Game Boy (DMG) core
- Implement LR35902 CPU (registers, opcodes, interrupts).
- Implement MMU with ROM, RAM, VRAM, HRAM, IO registers.
- Implement timers (DIV/TIMA), interrupts, and DMA (OAM).
- Implement PPU pipeline (mode timings, BG, sprites).
- Implement audio channels and mixer (APU).

3. GBC core
- Add double speed mode.
- Add VRAM banking, palette RAM, and CGB-only registers.
- Extend PPU for color palettes and attribute maps.

4. GBA core
- Implement ARM7TDMI CPU (ARM + THUMB), pipeline, IRQs.
- Implement memory map (WRAM, I/O, VRAM, OAM, ROM).
- Implement DMA, timers, and key I/O.
- Implement PPU for modes 0-5, affine, and sprites.
- Implement APU (basic mixing first, accuracy later).

5. Frontends
- Linux SDL2: window, renderer, input mapping, audio callback.
- Android: NDK + SDL2 (or JNI + native window) with on-screen controls.
- Shared config system with per-platform defaults.

6. Testing and validation
- Integrate test ROMs (Blargg, Mooneye, GBA tests) with harness.
- Per-component unit tests for CPU and MMU.
- Automated regression suite with known-good hash outputs.

7. Quality and compatibility
- Save states, SRAM persistence, and real-time clock (for GBC/GBA).
- Performance profiling and targeted optimization.
- Optional: shader-based scaling, filters, and frame pacing.

**Repo Layout (initial)**
- `core/`
- `frontend/`
- `android/`
- `common/`
- `tests/`
- `CMakeLists.txt`
- `README.md`

**Immediate Next Steps**
1. Create the skeleton CMake project and folder structure.
2. Add a minimal SDL2 Linux frontend that boots a ROM and prints header info.
3. Add stub GB CPU/MMU classes with tests.
