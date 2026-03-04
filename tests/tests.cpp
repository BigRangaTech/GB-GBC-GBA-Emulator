#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "core.h"
#include "cpu.h"
#include "gba_bus.h"
#include "gba_core.h"
#include "gba_cpu.h"
#include "mmu.h"
#include "ppu.h"
#include "rom.h"

namespace {

int failures = 0;

std::string to_lower(std::string value);
std::string serial_debug_text(const std::string& serial_bytes);
bool is_rom_extension(const std::filesystem::path& path);
gbemu::core::System detect_system(const std::vector<std::uint8_t>& data);

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cout << "FAIL: " << message << "\n";
    ++failures;
  }
}

int angle_distance_u16(std::uint16_t a, std::uint16_t b) {
  int da = static_cast<int>(a);
  int db = static_cast<int>(b);
  int diff = std::abs(da - db);
  return std::min(diff, 65536 - diff);
}

void test_read_file_missing() {
  std::vector<std::uint8_t> data;
  std::string error;
  bool ok = gbemu::common::read_file("/no/such/file.bin", &data, &error);
  expect(!ok, "read_file should fail for missing file");
  expect(!error.empty(), "read_file should return an error message");
}

void test_read_file_roundtrip() {
  std::filesystem::path path = std::filesystem::temp_directory_path() / "gbemu_test_rom.bin";
  {
    std::ofstream out(path, std::ios::binary);
    std::vector<std::uint8_t> bytes = {0x01, 0x02, 0x03, 0x04};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  std::vector<std::uint8_t> data;
  std::string error;
  bool ok = gbemu::common::read_file(path.string(), &data, &error);
  expect(ok, "read_file should succeed for valid file");
  expect(data.size() == 4, "read_file should read full file size");
  expect(data.size() >= 4 && data[0] == 0x01 && data[3] == 0x04, "read_file content should match");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void test_config_parse() {
  std::filesystem::path path = std::filesystem::temp_directory_path() / "gbemu_test_config.conf";
  {
    std::ofstream out(path);
    out << "# Sample config\n";
    out << "system = gbc\n";
    out << "fps = 59.7275\n";
    out << "scale = 3\n";
    out << "video_driver = wayland\n";
  }

  gbemu::common::Config config;
  std::string error;
  bool ok = config.load_file(path.string(), &error);
  expect(ok, "config should load successfully");
  expect(config.get_string("system", "") == "gbc", "config should parse system");
  expect(config.get_int("scale").value_or(0) == 3, "config should parse scale");
  double fps = config.get_double("fps").value_or(0.0);
  expect(fps > 59.7 && fps < 59.8, "config should parse fps");
  expect(config.get_string("video_driver", "") == "wayland", "config should parse video_driver");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void test_ppu_sizes() {
  gbemu::core::EmulatorCore core;
  core.set_system(gbemu::core::System::GB);
  expect(core.framebuffer_width() == 160 && core.framebuffer_height() == 144,
         "GB framebuffer should be 160x144");

  core.set_system(gbemu::core::System::GBC);
  expect(core.framebuffer_width() == 160 && core.framebuffer_height() == 144,
         "GBC framebuffer should be 160x144");

  core.set_system(gbemu::core::System::GBA);
  expect(core.framebuffer_width() == 240 && core.framebuffer_height() == 160,
         "GBA framebuffer should be 240x160");
}

void test_boot_rom_required() {
  gbemu::core::EmulatorCore core;
  core.set_system(gbemu::core::System::GB);
  std::vector<std::uint8_t> rom = {0x00, 0x01, 0x02};
  std::vector<std::uint8_t> boot_rom;
  std::string error;
  bool ok = core.load_rom(rom, boot_rom, &error);
  expect(!ok, "core.load_rom should fail without boot ROM");
}

void test_boot_rom_size() {
  gbemu::core::EmulatorCore core;
  core.set_system(gbemu::core::System::GB);
  std::vector<std::uint8_t> rom = {0x00, 0x01, 0x02};
  std::vector<std::uint8_t> boot_rom(0x100, 0xFF);
  std::string error;
  bool ok = core.load_rom(rom, boot_rom, &error);
  expect(ok, "core.load_rom should accept 0x100 boot ROM for GB");
}

void test_gb_serial_transfer_capture() {
  gbemu::core::Mmu mmu;
  std::vector<std::uint8_t> rom(0x200, 0);
  std::vector<std::uint8_t> boot_rom(0x100, 0);
  std::string error;
  bool ok = mmu.load(gbemu::core::System::GB, rom, boot_rom, &error);
  expect(ok, "MMU should load for GB serial capture test");
  if (!ok) {
    return;
  }

  mmu.write_u8(0xFF01, static_cast<std::uint8_t>('P'));
  mmu.write_u8(0xFF02, 0x81);
  std::string serial = mmu.take_serial_output();
  expect(serial == "P", "GB serial transfer should emit SB byte to serial output buffer");
  expect((mmu.read_u8(0xFF02) & 0x80u) == 0,
         "GB serial transfer completion should clear SC transfer-start bit");
}

void test_gb_ei_delay_before_interrupt_service() {
  gbemu::core::Mmu mmu;
  gbemu::core::Cpu cpu;
  std::vector<std::uint8_t> rom(0x8000, 0x00);
  std::vector<std::uint8_t> boot_rom(0x100, 0x00);
  boot_rom[0x00] = 0xFB; // EI
  boot_rom[0x01] = 0x00; // NOP
  boot_rom[0x02] = 0x76; // HALT (should not execute before IRQ service)
  boot_rom[0x40] = 0x00; // VBlank handler: NOP

  std::string error;
  bool ok = mmu.load(gbemu::core::System::GB, rom, boot_rom, &error);
  expect(ok, "MMU should load for EI delay regression test");
  if (!ok) {
    return;
  }

  cpu.connect(&mmu);
  cpu.reset();

  mmu.write_u8(0xFFFF, 0x01); // IE: VBlank enabled
  mmu.write_u8(0xFF0F, 0x01); // IF: VBlank requested

  cpu.step(); // EI
  expect(cpu.regs().pc == 0x0001,
         "After EI, CPU should advance to next instruction before servicing IRQ");

  cpu.step(); // NOP
  expect(cpu.regs().pc == 0x0002,
         "IRQ should not be serviced on the instruction immediately after EI");

  cpu.step(); // IRQ service should happen now, before HALT at 0x0002
  expect(cpu.regs().pc == 0x0040,
         "IRQ should be serviced only after EI one-instruction delay elapses");
  expect((mmu.interrupt_flags() & 0x01u) == 0,
         "IRQ service should clear the handled interrupt request bit");
}

void test_gb_interrupt_register_unused_bits() {
  gbemu::core::Mmu mmu;
  std::vector<std::uint8_t> rom(0x200, 0);
  std::vector<std::uint8_t> boot_rom(0x100, 0);
  std::string error;
  bool ok = mmu.load(gbemu::core::System::GB, rom, boot_rom, &error);
  expect(ok, "MMU should load for IF/IE unused-bit behavior test");
  if (!ok) {
    return;
  }

  mmu.write_u8(0xFF0F, 0x00);
  expect(mmu.read_u8(0xFF0F) == 0xE0u,
         "IF upper bits should read back as set");
  mmu.write_u8(0xFF0F, 0x1Fu);
  expect(mmu.read_u8(0xFF0F) == 0xFFu,
         "IF should preserve lower interrupt bits while forcing upper bits set");

  mmu.write_u8(0xFFFF, 0x00);
  expect(mmu.read_u8(0xFFFF) == 0xE0u,
         "IE upper bits should read back as set");
  mmu.write_u8(0xFFFF, 0x1Fu);
  expect(mmu.read_u8(0xFFFF) == 0xFFu,
         "IE should preserve lower interrupt bits while forcing upper bits set");
}

void test_gb_halt_ime0_resume_executes_next_instruction() {
  gbemu::core::Mmu mmu;
  gbemu::core::Cpu cpu;
  std::vector<std::uint8_t> rom(0x8000, 0x00);
  std::vector<std::uint8_t> boot_rom(0x100, 0x00);
  boot_rom[0x00] = 0x76; // HALT
  boot_rom[0x01] = 0x04; // INC B
  boot_rom[0x02] = 0x00; // NOP

  std::string error;
  bool ok = mmu.load(gbemu::core::System::GB, rom, boot_rom, &error);
  expect(ok, "MMU should load for HALT IME=0 resume test");
  if (!ok) {
    return;
  }

  cpu.connect(&mmu);
  cpu.reset();
  mmu.write_u8(0xFFFF, 0x01); // IE: VBlank enabled
  mmu.write_u8(0xFF0F, 0x00); // IF: none pending

  cpu.step(); // HALT
  expect(cpu.halted(), "CPU should enter HALT when no interrupt is pending");
  expect(cpu.regs().pc == 0x0001,
         "PC should point to next opcode after HALT");

  mmu.write_u8(0xFF0F, 0x01); // request interrupt while IME=0
  cpu.step(); // should unhalt and execute INC B in same step
  expect(!cpu.halted(), "CPU should leave HALT when an interrupt request is raised");
  expect(cpu.regs().b == 0x01,
         "CPU should execute next instruction immediately after leaving HALT with IME=0");
  expect(cpu.regs().pc == 0x0002,
         "PC should advance past resumed instruction");
  expect((mmu.interrupt_flags() & 0x01u) != 0,
         "Interrupt should remain pending when IME=0 and ISR is not serviced");
}

void test_gb_stat_irq_blocking_across_mode0_to_mode1() {
  gbemu::core::Mmu mmu;
  gbemu::core::Ppu ppu;
  std::vector<std::uint8_t> rom(0x200, 0);
  std::vector<std::uint8_t> boot_rom(0x100, 0);
  std::string error;
  bool ok = mmu.load(gbemu::core::System::GB, rom, boot_rom, &error);
  expect(ok, "MMU should load for STAT IRQ blocking test");
  if (!ok) {
    return;
  }

  ppu.set_system(gbemu::core::System::GB);
  mmu.write_u8(0xFF40, 0x80); // LCD on
  mmu.write_u8(0xFF41, 0x18); // STAT IRQ enable: mode 0 + mode 1
  mmu.write_u8(0xFF0F, 0x00); // clear IF

  auto step_until = [&ppu, &mmu](const std::function<bool()>& predicate, int max_steps) {
    for (int i = 0; i < max_steps; ++i) {
      ppu.step(1, &mmu);
      if (predicate()) {
        return true;
      }
    }
    return false;
  };

  bool reached_mode0_line143 = step_until(
      [&mmu]() {
        return mmu.read_u8(0xFF44) == 143u && (mmu.read_u8(0xFF41) & 0x03u) == 0u;
      },
      456 * 160);
  expect(reached_mode0_line143, "PPU should reach line 143 mode 0");
  if (!reached_mode0_line143) {
    return;
  }

  mmu.write_u8(0xFF0F, 0x00); // acknowledge STAT while mode 0 source is still active

  bool reached_mode1_line144 = step_until(
      [&mmu]() {
        return mmu.read_u8(0xFF44) == 144u && (mmu.read_u8(0xFF41) & 0x03u) == 1u;
      },
      456);
  expect(reached_mode1_line144, "PPU should transition to line 144 mode 1");
  if (!reached_mode1_line144) {
    return;
  }

  expect((mmu.read_u8(0xFF0F) & 0x02u) == 0u,
         "STAT IRQ should be blocked across mode 0->1 when STAT line never deasserts");

  // After STAT line deasserts (mode 2 on line 0), a later mode 0 rise should trigger again.
  mmu.write_u8(0xFF0F, 0x00);
  bool reached_mode2_line0 = step_until(
      [&mmu]() {
        return mmu.read_u8(0xFF44) == 0u && (mmu.read_u8(0xFF41) & 0x03u) == 2u;
      },
      456 * 12);
  expect(reached_mode2_line0, "PPU should wrap back to line 0 mode 2");
  if (!reached_mode2_line0) {
    return;
  }

  bool reached_mode0_line0 = step_until(
      [&mmu]() {
        return mmu.read_u8(0xFF44) == 0u && (mmu.read_u8(0xFF41) & 0x03u) == 0u;
      },
      456);
  expect(reached_mode0_line0, "PPU should enter line 0 mode 0");
  if (!reached_mode0_line0) {
    return;
  }

  expect((mmu.read_u8(0xFF0F) & 0x02u) != 0u,
         "STAT IRQ should retrigger once STAT line deasserts and rises again");
}

void test_gb_cb_bit_hl_timing() {
  gbemu::core::Mmu mmu;
  gbemu::core::Cpu cpu;
  std::vector<std::uint8_t> rom(0x8000, 0x00);
  std::vector<std::uint8_t> boot_rom(0x100, 0x00);
  boot_rom[0x00] = 0x21; // LD HL,d16
  boot_rom[0x01] = 0x00;
  boot_rom[0x02] = 0xC0;
  boot_rom[0x03] = 0xCB; // BIT 0,(HL)
  boot_rom[0x04] = 0x46;
  boot_rom[0x05] = 0xCB; // BIT 7,(HL)
  boot_rom[0x06] = 0x7E;

  std::string error;
  bool ok = mmu.load(gbemu::core::System::GB, rom, boot_rom, &error);
  expect(ok, "MMU should load for CB BIT (HL) timing test");
  if (!ok) {
    return;
  }

  cpu.connect(&mmu);
  cpu.reset();
  mmu.write_u8(0xC000, 0x01);

  int used = cpu.step(); // LD HL,d16
  expect(used == 12, "LD HL,d16 should consume 12 cycles");

  used = cpu.step(); // BIT 0,(HL)
  expect(used == 12, "CB BIT 0,(HL) should consume 12 cycles");
  expect((cpu.regs().f & 0x80u) == 0, "BIT 0,(HL) on bit-set value should clear Z");
  expect((cpu.regs().f & 0x40u) == 0, "BIT should clear N flag");
  expect((cpu.regs().f & 0x20u) != 0, "BIT should set H flag");

  used = cpu.step(); // BIT 7,(HL)
  expect(used == 12, "CB BIT 7,(HL) should consume 12 cycles");
  expect((cpu.regs().f & 0x80u) != 0, "BIT 7,(HL) on clear bit should set Z");
}

std::vector<std::uint8_t> build_gba_spin_rom() {
  std::vector<std::uint8_t> rom(0x200, 0);
  // ARM B . at 0x08000000
  rom[0] = 0xFE;
  rom[1] = 0xFF;
  rom[2] = 0xFF;
  rom[3] = 0xEA;
  const char* title = "STATETEST";
  for (int i = 0; title[i] != '\0' && (0xA0 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0xA0 + i] = static_cast<std::uint8_t>(title[i]);
  }
  rom[0xAC] = 'S';
  rom[0xAD] = 'T';
  rom[0xAE] = 'A';
  rom[0xAF] = 'T';
  rom[0xB2] = 0x96;
  return rom;
}

void test_gba_state_roundtrip() {
  gbemu::core::EmulatorCore core;
  core.set_system(gbemu::core::System::GBA);
  core.set_gba_fastboot(true);

  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load_rom(rom, bios, &error);
  expect(ok, "core.load_rom should accept GBA ROM + 16KB BIOS");
  if (!ok) {
    return;
  }

  core.step_frame();
  std::vector<std::uint8_t> state_a;
  expect(core.save_state(&state_a), "GBA save_state should succeed");
  expect(!state_a.empty(), "GBA save_state should produce non-empty data");

  core.step_frame();
  core.step_frame();

  std::string load_error;
  expect(core.load_state(state_a, &load_error), "GBA load_state should succeed");

  std::vector<std::uint8_t> state_b;
  expect(core.save_state(&state_b), "GBA save_state should still succeed after load_state");
  expect(state_a == state_b, "GBA save/load should roundtrip deterministically");
}

void write_le32(std::vector<std::uint8_t>* data, std::size_t offset, std::uint32_t value) {
  if (!data || offset + 4 > data->size()) {
    return;
  }
  (*data)[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
  (*data)[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  (*data)[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  (*data)[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

std::vector<std::uint8_t> build_gba_timing_rom() {
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  if (rom.size() < 0x400) {
    rom.resize(0x400, 0);
  }
  for (std::size_t off = 0; off < 0x100; off += 4) {
    write_le32(&rom, off, 0xE1A00000u); // ARM NOP
  }
  rom[0x20] = 0xC0; // THUMB NOP (MOV r8, r8)
  rom[0x21] = 0x46;
  rom[0x22] = 0xC0;
  rom[0x23] = 0x46;
  return rom;
}

int run_gba_data_access_probe(std::uint32_t opcode,
                              std::uint32_t data_addr,
                              std::uint16_t waitcnt) {
  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_timing_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for data-access timing probe");
  if (!ok) {
    return 0;
  }

  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  constexpr std::uint32_t kProbePc = 0x03000000u; // IWRAM probe to isolate data-side timing.
  core.debug_write32(kProbePc, opcode);
  core.debug_set_cpu_reg(1, data_addr);
  core.debug_set_cpu_reg(15, kProbePc);
  core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
  core.debug_write16(kRegWaitcnt, waitcnt);
  return core.debug_step_cpu_instruction();
}

void test_gba_mgba_debug_output_capture() {
  gbemu::core::GbaBus bus;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = bus.load(rom, bios, &error);
  expect(ok, "GbaBus should load for mGBA debug output capture test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegDebugEnable = 0x04FFF780u;
  constexpr std::uint32_t kRegDebugFlags = 0x04FFF700u;
  constexpr std::uint32_t kRegDebugString = 0x04FFF600u;

  bus.write16(kRegDebugEnable, 0xC0DEu);
  const char* text = "PASS";
  for (int i = 0; text[i] != '\0'; ++i) {
    bus.write8(kRegDebugString + static_cast<std::uint32_t>(i),
               static_cast<std::uint8_t>(text[i]));
  }
  bus.write8(kRegDebugString + 4u, 0u);
  bus.write16(kRegDebugFlags, 1u);

  std::string out = bus.take_debug_output();
  expect(out.find("PASS") != std::string::npos,
         "mGBA debug channel writes should be captured in GBA output stream");
}

void test_gba_cpu_swp_and_swpb() {
  gbemu::core::GbaBus bus;
  gbemu::core::GbaCpu cpu;
  std::vector<std::uint8_t> rom(4, 0);
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for SWP test");
  if (!loaded) {
    return;
  }

  // SWP r3, r1, [r2]
  write_le32(&bios, 0, 0xE1023091u);
  loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should reload BIOS for SWP instruction");
  if (!loaded) {
    return;
  }
  cpu.reset();
  cpu.set_reg(1, 0x11223344u);
  cpu.set_reg(2, 0x02000000u);
  bus.write32(0x02000000u, 0xAABBCCDDu);
  int used = cpu.step(&bus);
  expect(used > 0, "SWP instruction should execute");
  expect(cpu.reg(3) == 0xAABBCCDDu, "SWP should return old memory word in Rd");
  expect(bus.read32(0x02000000u) == 0x11223344u, "SWP should store Rm word to memory");

  // SWPB r3, r1, [r2]
  write_le32(&bios, 0, 0xE1423091u);
  loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should reload BIOS for SWPB instruction");
  if (!loaded) {
    return;
  }
  cpu.reset();
  cpu.set_reg(1, 0x000000C7u);
  cpu.set_reg(2, 0x02000004u);
  bus.write32(0x02000004u, 0x1122335Au);
  used = cpu.step(&bus);
  expect(used > 0, "SWPB instruction should execute");
  expect(cpu.reg(3) == 0x0000005Au, "SWPB should return old memory byte in Rd");
  expect(bus.read8(0x02000004u) == 0xC7u, "SWPB should store low byte from Rm");
  expect(bus.read8(0x02000005u) == 0x33u, "SWPB should preserve neighboring bytes");
}

void test_gba_cpu_arm_long_multiply() {
  gbemu::core::GbaBus bus;
  gbemu::core::GbaCpu cpu;
  std::vector<std::uint8_t> rom(4, 0);
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;

  // UMULL r0, r1, r2, r3
  write_le32(&bios, 0, 0xE0810392u);
  bool loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for UMULL instruction");
  if (!loaded) {
    return;
  }
  cpu.reset();
  cpu.set_reg(2, 0xFFFFFFFFu);
  cpu.set_reg(3, 0x00000002u);
  int used = cpu.step(&bus);
  expect(used > 0, "UMULL instruction should execute");
  expect(cpu.reg(0) == 0xFFFFFFFEu, "UMULL should write low 32 bits to RdLo");
  expect(cpu.reg(1) == 0x00000001u, "UMULL should write high 32 bits to RdHi");

  // UMLAL r0, r1, r2, r3
  write_le32(&bios, 0, 0xE0A10392u);
  loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for UMLAL instruction");
  if (!loaded) {
    return;
  }
  cpu.reset();
  cpu.set_reg(0, 0x00000001u);
  cpu.set_reg(1, 0x00000002u);
  cpu.set_reg(2, 0xFFFFFFFFu);
  cpu.set_reg(3, 0x00000002u);
  used = cpu.step(&bus);
  expect(used > 0, "UMLAL instruction should execute");
  expect(cpu.reg(0) == 0xFFFFFFFFu, "UMLAL should accumulate into RdLo");
  expect(cpu.reg(1) == 0x00000003u, "UMLAL should accumulate into RdHi");

  // SMULL r0, r1, r2, r3
  write_le32(&bios, 0, 0xE0C10392u);
  loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for SMULL instruction");
  if (!loaded) {
    return;
  }
  cpu.reset();
  cpu.set_reg(2, 0xFFFFFFFFu); // -1
  cpu.set_reg(3, 0x00000002u); // 2
  used = cpu.step(&bus);
  expect(used > 0, "SMULL instruction should execute");
  expect(cpu.reg(0) == 0xFFFFFFFEu, "SMULL should write signed low 32 bits to RdLo");
  expect(cpu.reg(1) == 0xFFFFFFFFu, "SMULL should write signed high 32 bits to RdHi");

  // SMLAL r0, r1, r2, r3
  write_le32(&bios, 0, 0xE0E10392u);
  loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for SMLAL instruction");
  if (!loaded) {
    return;
  }
  cpu.reset();
  cpu.set_reg(0, 0x00000003u); // accumulator low
  cpu.set_reg(1, 0x00000000u); // accumulator high
  cpu.set_reg(2, 0xFFFFFFFFu); // -1
  cpu.set_reg(3, 0x00000002u); // 2
  used = cpu.step(&bus);
  expect(used > 0, "SMLAL instruction should execute");
  expect(cpu.reg(0) == 0x00000001u, "SMLAL should accumulate signed result into RdLo");
  expect(cpu.reg(1) == 0x00000000u, "SMLAL should accumulate signed result into RdHi");
}

void test_gba_cpu_arm_load_store_rrx_offset() {
  gbemu::core::GbaBus bus;
  gbemu::core::GbaCpu cpu;
  std::vector<std::uint8_t> rom(4, 0);
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;

  // LDR r1, [r0, r2, ROR #0] -> RRX(r2)
  write_le32(&bios, 0, 0xE7901062u);
  bool loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for ARM RRX offset test");
  if (!loaded) {
    return;
  }
  cpu.reset();
  cpu.set_cpsr(cpu.cpsr() | (1u << 29)); // set carry for RRX
  cpu.set_reg(0, 0x02000000u);
  cpu.set_reg(2, 0x00000000u);
  bus.write32(0x02000000u, 0x12345678u);

  int used = cpu.step(&bus);
  expect(used > 0, "ARM LDR with RRX offset should execute");
  expect(cpu.reg(1) == 0xFFFFFFFFu,
         "LDR register-offset ROR #0 should use RRX with carry when computing offset");
}

void test_gba_waitcnt_nseq_seq_fetch_cycles() {
  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_timing_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for WAITCNT nseq/seq timing test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  core.debug_set_cpu_reg(15, 0x08000000u);
  core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
  core.debug_write16(kRegWaitcnt, 0x0030u); // WS0 first=8, second=2, prefetch off

  int first = core.debug_step_cpu_instruction();
  int second = core.debug_step_cpu_instruction();
  expect(first > second,
         "First ROM fetch should pay non-sequential wait and exceed sequential follow-up");
}

void test_gba_prefetch_reduces_sequential_fetch_penalty() {
  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  auto run_second_cycle = [kRegWaitcnt](std::uint16_t waitcnt) -> int {
    gbemu::core::GbaCore core;
    core.set_fastboot(true);
    std::vector<std::uint8_t> rom = build_gba_timing_rom();
    std::vector<std::uint8_t> bios(0x4000, 0);
    std::string error;
    bool ok = core.load(rom, bios, &error);
    expect(ok, "GbaCore load should succeed for prefetch timing test");
    if (!ok) {
      return 0;
    }
    core.debug_set_cpu_reg(15, 0x08000000u);
    core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
    core.debug_write16(kRegWaitcnt, waitcnt);
    int first = core.debug_step_cpu_instruction();
    int second = core.debug_step_cpu_instruction();
    expect(first > 0 && second > 0, "Prefetch timing test steps should execute");
    return second;
  };

  int second_no_prefetch = run_second_cycle(0x0030u);
  int second_with_prefetch = run_second_cycle(0x4030u); // +bit14 prefetch
  expect(second_with_prefetch < second_no_prefetch,
         "WAITCNT prefetch should reduce steady-state sequential ROM fetch cost");
}

void test_gba_waitcnt_write_flushes_prefetch_stream() {
  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_timing_rom();
  write_le32(&rom, 0x120, 0xE1A00000u); // NOP
  write_le32(&rom, 0x124, 0xE1A00000u); // NOP
  write_le32(&rom, 0x128, 0xE1A00000u); // NOP
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for WAITCNT write prefetch flush test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  constexpr std::uint32_t kProgPc = 0x08000120u;
  core.debug_set_cpu_reg(15, kProgPc);
  core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state

  core.debug_write16(kRegWaitcnt, 0x4060u); // prefetch on, fast WS0
  int first = core.debug_step_cpu_instruction();
  int steady_fast = core.debug_step_cpu_instruction();
  expect(first > 0 && steady_fast > 0,
         "WAITCNT write prefetch flush test should execute baseline steps");

  core.debug_write16(kRegWaitcnt, 0x4030u); // prefetch on, slow WS0
  int after_waitcnt_change = core.debug_step_cpu_instruction();
  expect(after_waitcnt_change > steady_fast,
         "WAITCNT writes should flush fetch stream/prefetch so next fetch pays updated wait cost");
}

void test_gba_prefetch_stream_break_on_dma_activity() {
  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  constexpr std::uint32_t kProgPc = 0x08000120u;
  constexpr std::uint32_t kDma0Base = 0x040000B0u;
  constexpr std::uint32_t kDmaSrc = 0x02000000u;
  constexpr std::uint32_t kDmaDst = 0x02000020u;

  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_timing_rom();
  write_le32(&rom, 0x120, 0xE1A00000u); // NOP
  write_le32(&rom, 0x124, 0xE1A00000u); // NOP
  write_le32(&rom, 0x128, 0xE1A00000u); // NOP
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for DMA prefetch break timing test");
  if (!ok) {
    return;
  }

  core.debug_set_cpu_reg(15, kProgPc);
  core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
  core.debug_write16(kRegWaitcnt, 0x4030u); // prefetch on, high WS0 first wait

  int first = core.debug_step_cpu_instruction();
  int steady = core.debug_step_cpu_instruction();
  expect(first > 0 && steady > 0,
         "DMA prefetch break timing test should execute warm-up steps");

  core.debug_write32(kDmaSrc, 0x89ABCDEFu);
  core.debug_write32(kDma0Base + 0u, kDmaSrc);
  core.debug_write32(kDma0Base + 4u, kDmaDst);
  core.debug_write16(kDma0Base + 8u, 1u);
  core.debug_write16(kDma0Base + 10u, 0x8000u); // immediate enable
  core.debug_step_dma();

  int after_dma = core.debug_step_cpu_instruction();
  expect(after_dma > steady,
         "DMA activity should invalidate fetch stream/prefetch so next ROM fetch refills");
}

void test_gba_prefetch_stream_break_on_irq_service_event() {
  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  constexpr std::uint32_t kRegIe = 0x04000200u;
  constexpr std::uint32_t kRegIme = 0x04000208u;
  constexpr std::uint32_t kProgPc = 0x08000120u;

  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_timing_rom();
  write_le32(&rom, 0x120, 0xE1A00000u); // NOP
  write_le32(&rom, 0x124, 0xE1A00000u); // NOP
  write_le32(&rom, 0x128, 0xE1A00000u); // NOP
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for IRQ-event prefetch break timing test");
  if (!ok) {
    return;
  }

  core.debug_set_cpu_reg(15, kProgPc);
  core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
  core.debug_write16(kRegWaitcnt, 0x4030u); // prefetch on, high WS0 first wait

  int first = core.debug_step_cpu_instruction();
  int steady = core.debug_step_cpu_instruction();
  expect(first > 0 && steady > 0,
         "IRQ-event prefetch break timing test should execute warm-up steps");

  core.debug_write16(kRegIe, 0x0001u);
  core.debug_set_if_bits(0x0001u);
  core.debug_write16(kRegIme, 0x0001u);
  core.debug_service_interrupts();

  // Emulate post-IRQ return boundary by restoring ROM PC for the next fetch probe.
  core.debug_set_cpu_reg(15, 0x08000128u);
  core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
  int after_irq = core.debug_step_cpu_instruction();
  expect(after_irq > steady,
         "IRQ service should invalidate fetch stream/prefetch so resumed ROM fetch refills");
}

void test_gba_waitcnt_ws1_ws2_region_timing() {
  // WAITCNT layout used by the current timing model:
  // WS0 nseq bits [5:4], WS0 seq [6], WS1 nseq [8:7], WS1 seq [9], WS2 nseq [11:10], WS2 seq [12].
  constexpr std::uint16_t kWaitcnt = 0x1E60u; // WS0 first=2, WS1 first=4, WS2 first=8, seq bits=1.
  constexpr std::uint32_t kArmLdrhR0FromR1 = 0xE1D100B0u; // LDRH r0, [r1]

  int ws0 = run_gba_data_access_probe(kArmLdrhR0FromR1, 0x08000100u, kWaitcnt);
  int ws1 = run_gba_data_access_probe(kArmLdrhR0FromR1, 0x0A000100u, kWaitcnt);
  int ws2 = run_gba_data_access_probe(kArmLdrhR0FromR1, 0x0C000100u, kWaitcnt);

  expect(ws0 > 0 && ws1 > 0 && ws2 > 0, "WS0/WS1/WS2 timing probe should execute");
  expect(ws0 < ws1 && ws1 < ws2,
         "Game Pak timing should scale with WAITCNT WS0/WS1/WS2 region wait selections");
}

void test_gba_data_access_width_and_sram_wait_timing() {
  constexpr std::uint32_t kArmLdrbR0FromR1 = 0xE5D10000u; // LDRB r0, [r1]
  constexpr std::uint32_t kArmLdrhR0FromR1 = 0xE1D100B0u; // LDRH r0, [r1]
  constexpr std::uint32_t kArmLdrR0FromR1 = 0xE5910000u;  // LDR  r0, [r1]

  // Game Pak width behavior: 32-bit should cost more than 8/16-bit due two halfword transfers.
  int gp_byte = run_gba_data_access_probe(kArmLdrbR0FromR1, 0x08000100u, 0x0030u);
  int gp_half = run_gba_data_access_probe(kArmLdrhR0FromR1, 0x08000100u, 0x0030u);
  int gp_word = run_gba_data_access_probe(kArmLdrR0FromR1, 0x08000100u, 0x0030u);
  expect(gp_byte > 0 && gp_half > 0 && gp_word > 0,
         "Game Pak width timing probe should execute");
  expect(gp_word > gp_half && gp_word > gp_byte,
         "32-bit Game Pak data access should incur higher wait penalty than 8/16-bit accesses");

  // SRAM wait control + width scaling.
  int sram_byte_fast = run_gba_data_access_probe(kArmLdrbR0FromR1, 0x0E000100u, 0x0002u);
  int sram_byte_slow = run_gba_data_access_probe(kArmLdrbR0FromR1, 0x0E000100u, 0x0003u);
  int sram_half_slow = run_gba_data_access_probe(kArmLdrhR0FromR1, 0x0E000100u, 0x0003u);
  int sram_word_slow = run_gba_data_access_probe(kArmLdrR0FromR1, 0x0E000100u, 0x0003u);
  expect(sram_byte_fast > 0 && sram_byte_slow > 0 && sram_half_slow > 0 && sram_word_slow > 0,
         "SRAM timing probe should execute");
  expect(sram_byte_slow > sram_byte_fast,
         "SRAM wait control should increase data-access timing when slower wait index is selected");
  expect(sram_byte_slow < sram_half_slow && sram_half_slow < sram_word_slow,
         "SRAM data timing should scale with access width (8-bit < 16-bit < 32-bit)");
}

void test_gba_gamepak_data_burst_sequential_wait_timing() {
  gbemu::core::GbaBus bus;
  std::vector<std::uint8_t> rom = build_gba_timing_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = bus.load(rom, bios, &error);
  expect(ok, "GbaBus should load for Game Pak data-burst timing test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  constexpr std::uint32_t kAddr = 0x08000100u;
  bus.write16(kRegWaitcnt, 0x0030u); // WS0 first=8, second=2, prefetch off

  auto one_read_step_cycles = [&bus](std::uint32_t address) -> int {
    bus.begin_cpu_step();
    (void)bus.read16(address);
    return bus.finish_cpu_step(0);
  };

  int single = one_read_step_cycles(kAddr);
  bus.begin_cpu_step();
  (void)bus.read16(kAddr);
  (void)bus.read16(kAddr + 2u);
  int burst = bus.finish_cpu_step(0);

  expect(single > 0 && burst > 0, "Game Pak data-burst timing probe should execute");
  {
    std::ostringstream msg;
    msg << "Back-to-back Game Pak data accesses in one CPU step should use sequential wait after first access"
        << " (single=" << single << " burst=" << burst << ")";
    expect(burst < single * 2, msg.str());
  }

  int separate_first = one_read_step_cycles(kAddr);
  int separate_second = one_read_step_cycles(kAddr + 2u);
  expect(separate_second >= separate_first,
         "Across CPU-step boundaries Game Pak data accesses should restart as non-sequential waits");
}

void test_gba_gamepak_data_stream_break_on_event_invalidation() {
  gbemu::core::GbaBus bus;
  std::vector<std::uint8_t> rom = build_gba_timing_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = bus.load(rom, bios, &error);
  expect(ok, "GbaBus should load for Game Pak data-stream invalidation test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  constexpr std::uint32_t kAddr = 0x08000100u;
  bus.write16(kRegWaitcnt, 0x0030u); // WS0 first=8, second=2, prefetch off

  bus.begin_cpu_step();
  (void)bus.read16(kAddr);
  (void)bus.read16(kAddr + 2u);
  int burst = bus.finish_cpu_step(0);

  bus.begin_cpu_step();
  (void)bus.read16(kAddr);
  bus.invalidate_prefetch_stream(); // models IRQ/DMA/event stream invalidation
  (void)bus.read16(kAddr + 2u);
  int invalidated = bus.finish_cpu_step(0);

  expect(burst > 0 && invalidated > 0, "Data-stream invalidation timing probe should execute");
  {
    std::ostringstream msg;
    msg << "Event invalidation should break Game Pak data sequential stream and force non-sequential refill cost"
        << " (burst=" << burst << " invalidated=" << invalidated << ")";
    expect(invalidated > burst, msg.str());
  }
}

void test_gba_prefetch_pipeline_refill_on_branch_and_mode_switch() {
  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;

  {
    gbemu::core::GbaCore core;
    core.set_fastboot(true);
    std::vector<std::uint8_t> rom = build_gba_timing_rom();
    write_le32(&rom, 0x00, 0xEA000002u); // B to 0x08000010
    write_le32(&rom, 0x10, 0xE1A00000u); // NOP at branch target
    write_le32(&rom, 0x14, 0xE1A00000u); // next sequential NOP
    std::vector<std::uint8_t> bios(0x4000, 0);
    std::string error;
    bool ok = core.load(rom, bios, &error);
    expect(ok, "GbaCore load should succeed for branch refill timing test");
    if (!ok) {
      return;
    }

    core.debug_set_cpu_reg(15, 0x08000000u);
    core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
    core.debug_write16(kRegWaitcnt, 0x4030u); // prefetch on, high WS0 first wait

    int branch = core.debug_step_cpu_instruction();
    int after_branch = core.debug_step_cpu_instruction();
    int steady = core.debug_step_cpu_instruction();
    expect(branch > 0 && after_branch > 0 && steady > 0,
           "Branch refill timing test steps should execute");
    expect(after_branch > steady,
           "First fetch after branch should pay pipeline refill penalty vs steady sequential fetch");
  }

  {
    gbemu::core::GbaCore core;
    core.set_fastboot(true);
    std::vector<std::uint8_t> rom = build_gba_timing_rom();
    write_le32(&rom, 0x00, 0xE12FFF10u); // BX r0 (ARM->THUMB)
    write_le32(&rom, 0x04, 0xE1A00000u); // ARM NOP fallback
    std::vector<std::uint8_t> bios(0x4000, 0);
    std::string error;
    bool ok = core.load(rom, bios, &error);
    expect(ok, "GbaCore load should succeed for ARM/THUMB refill timing test");
    if (!ok) {
      return;
    }

    core.debug_set_cpu_reg(15, 0x08000000u);
    core.debug_set_cpu_reg(0, 0x08000021u);
    core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
    core.debug_write16(kRegWaitcnt, 0x4030u); // prefetch on, high WS0 first wait

    int bx = core.debug_step_cpu_instruction();
    std::uint32_t cpsr_after_bx = core.debug_cpu_cpsr();
    std::uint32_t pc_after_bx = core.cpu_pc();
    {
      std::ostringstream msg;
      msg << "BX target with bit0 set should switch CPU to THUMB state"
          << " (cpsr=0x" << std::hex << cpsr_after_bx
          << " pc=0x" << pc_after_bx << ")";
      expect((cpsr_after_bx & (1u << 5)) != 0, msg.str());
    }
    int thumb_first = core.debug_step_cpu_instruction();
    int thumb_steady = core.debug_step_cpu_instruction();
    expect(bx > 0 && thumb_first > 0 && thumb_steady > 0,
           "ARM/THUMB refill timing test steps should execute");
    {
      std::ostringstream msg;
      msg << "First THUMB fetch after interworking should pay refill penalty vs steady THUMB fetch"
          << " (first=" << thumb_first << " steady=" << thumb_steady << ")";
      expect(thumb_first > thumb_steady, msg.str());
    }
  }
}

void test_gba_prefetch_stream_break_on_gamepak_data_access() {
  constexpr std::uint32_t kRegWaitcnt = 0x04000204u;
  constexpr std::uint32_t kLiteral = 0x12345678u;
  constexpr std::uint32_t kProgPc = 0x08000120u; // outside patched GBA header/logo region

  auto run_second_cycle = [kRegWaitcnt, kProgPc, kLiteral](bool with_rom_data_access,
                                                           std::uint32_t* loaded_out) -> int {
    gbemu::core::GbaCore core;
    core.set_fastboot(true);
    std::vector<std::uint8_t> rom = build_gba_timing_rom();
    if (with_rom_data_access) {
      // LDR r0, [pc, #0] reads literal from kProgPc + 8 (Game Pak data access).
      write_le32(&rom, 0x120, 0xE59F0000u);
      write_le32(&rom, 0x124, 0xE1A00000u); // NOP
      write_le32(&rom, 0x128, kLiteral);
    } else {
      write_le32(&rom, 0x120, 0xE1A00000u); // NOP
      write_le32(&rom, 0x124, 0xE1A00000u); // NOP
    }
    std::vector<std::uint8_t> bios(0x4000, 0);
    std::string error;
    bool ok = core.load(rom, bios, &error);
    expect(ok, "GbaCore load should succeed for data-access prefetch break timing test");
    if (!ok) {
      return 0;
    }

    core.debug_set_cpu_reg(15, kProgPc);
    core.debug_set_cpu_cpsr(core.debug_cpu_cpsr() & ~(1u << 5)); // ARM state
    core.debug_write16(kRegWaitcnt, 0x4030u); // prefetch on, high WS0 first wait

    int first = core.debug_step_cpu_instruction();
    int second = core.debug_step_cpu_instruction();
    expect(first > 0 && second > 0,
           "Data-access prefetch break timing test steps should execute");
    if (with_rom_data_access && loaded_out) {
      *loaded_out = core.debug_cpu_reg(0);
    }
    return second;
  };

  std::uint32_t loaded = 0;
  int steady = run_second_cycle(false, nullptr);
  int after_data_access = run_second_cycle(true, &loaded);
  expect(loaded == kLiteral, "ARM literal LDR should read Game Pak data value");
  expect(after_data_access > steady,
         "Game Pak data read should break fetch stream/prefetch and slow following fetch");
}

void test_gba_hle_swi_math_and_affine() {
  gbemu::core::GbaCore core;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for HLE SWI math/affine test");
  if (!ok) {
    return;
  }
  core.set_hle_swi(true);

  int cycles = 0;
  bool handled = false;

  core.debug_set_cpu_reg(0, 21u);
  core.debug_set_cpu_reg(1, 0xFFFFFFFCu); // -4
  handled = core.debug_handle_swi_hle(0x08000000u, false, 0xEF000006u, &cycles);
  expect(handled, "HLE SWI Div should be handled");
  expect(static_cast<std::int32_t>(core.debug_cpu_reg(0)) == -5,
         "SWI Div should return signed quotient in R0");
  expect(static_cast<std::int32_t>(core.debug_cpu_reg(1)) == 1,
         "SWI Div should return signed remainder in R1");
  expect(core.debug_cpu_reg(3) == 5u, "SWI Div should return abs(quotient) in R3");
  expect(core.cpu_pc() == 0x08000004u, "HLE SWI Div should advance ARM PC by 4");
  expect(cycles > 0, "HLE SWI Div should report cycle cost");

  core.debug_set_cpu_reg(0, 21u);
  core.debug_set_cpu_reg(1, 0xFFFFFFFCu); // -4
  handled = core.debug_handle_swi_hle(0x08000010u, false, 0xEF060000u, &cycles);
  expect(handled, "HLE SWI Div should accept ARM high-byte immediate form");
  expect(static_cast<std::int32_t>(core.debug_cpu_reg(0)) == -5,
         "ARM high-byte SWI Div should return signed quotient in R0");
  expect(static_cast<std::int32_t>(core.debug_cpu_reg(1)) == 1,
         "ARM high-byte SWI Div should return signed remainder in R1");
  expect(core.debug_cpu_reg(3) == 5u,
         "ARM high-byte SWI Div should return abs(quotient) in R3");
  expect(core.cpu_pc() == 0x08000014u,
         "ARM high-byte SWI Div should advance ARM PC by 4");

  core.debug_set_cpu_reg(0, 0xFFFFFFF9u); // -7 (denominator)
  core.debug_set_cpu_reg(1, 40u);         // numerator
  handled = core.debug_handle_swi_hle(0x08000020u, false, 0xEF000007u, &cycles);
  expect(handled, "HLE SWI DivArm should be handled");
  expect(static_cast<std::int32_t>(core.debug_cpu_reg(0)) == -5,
         "SWI DivArm should divide R1 by R0 and return quotient in R0");
  expect(static_cast<std::int32_t>(core.debug_cpu_reg(1)) == 5,
         "SWI DivArm should return remainder in R1");
  expect(core.debug_cpu_reg(3) == 5u, "SWI DivArm should return abs(quotient) in R3");

  core.debug_set_cpu_reg(0, 123u);
  core.debug_set_cpu_reg(1, 0u);
  handled = core.debug_handle_swi_hle(0x08000040u, false, 0xEF000006u, &cycles);
  expect(!handled, "HLE SWI Div should defer divide-by-zero to BIOS path");
  expect(core.debug_cpu_reg(0) == 123u, "Deferred divide-by-zero should leave registers untouched");

  core.debug_set_cpu_reg(0, 0x12345678u);
  handled = core.debug_handle_swi_hle(0x08000060u, false, 0xEF000008u, &cycles);
  expect(handled, "HLE SWI Sqrt should be handled");
  expect(core.debug_cpu_reg(0) == 17476u, "SWI Sqrt should return floor(sqrt(R0))");

  constexpr std::uint32_t kBgSrc = 0x02000000u;
  constexpr std::uint32_t kBgDst = 0x02000100u;
  core.debug_write32(kBgSrc + 0u, 0x00010000u); // texX (24.8)
  core.debug_write32(kBgSrc + 4u, 0x00020000u); // texY (24.8)
  core.debug_write16(kBgSrc + 8u, 2u);          // scrX
  core.debug_write16(kBgSrc + 10u, 3u);         // scrY
  core.debug_write16(kBgSrc + 12u, 0x0100u);    // sx (8.8)
  core.debug_write16(kBgSrc + 14u, 0x0200u);    // sy (8.8)
  core.debug_write16(kBgSrc + 16u, 0x0000u);    // angle (0 degrees)
  core.debug_set_cpu_reg(0, kBgSrc);
  core.debug_set_cpu_reg(1, kBgDst);
  core.debug_set_cpu_reg(2, 1u);
  handled = core.debug_handle_swi_hle(0x08000080u, false, 0xEF00000Eu, &cycles);
  expect(handled, "HLE SWI BgAffineSet should be handled");
  expect(core.debug_read16(kBgDst + 0u) == 0x0100u, "BgAffineSet should write PA");
  expect(core.debug_read16(kBgDst + 2u) == 0x0000u, "BgAffineSet should write PB");
  expect(core.debug_read16(kBgDst + 4u) == 0x0000u, "BgAffineSet should write PC");
  expect(core.debug_read16(kBgDst + 6u) == 0x0200u, "BgAffineSet should write PD");
  expect(core.debug_read32(kBgDst + 8u) == 0x0000FE00u, "BgAffineSet should write X reference");
  expect(core.debug_read32(kBgDst + 12u) == 0x0001FA00u, "BgAffineSet should write Y reference");

  constexpr std::uint32_t kObjSrc = 0x02000200u;
  constexpr std::uint32_t kObjDst = 0x02000300u;
  // Entry 0: identity.
  core.debug_write16(kObjSrc + 0u, 0x0100u);
  core.debug_write16(kObjSrc + 2u, 0x0100u);
  core.debug_write16(kObjSrc + 4u, 0x0000u);
  // Entry 1: 90 degrees with x-scale 2.0 and y-scale 1.0.
  core.debug_write16(kObjSrc + 8u, 0x0200u);
  core.debug_write16(kObjSrc + 10u, 0x0100u);
  core.debug_write16(kObjSrc + 12u, 0x4000u);
  core.debug_set_cpu_reg(0, kObjSrc);
  core.debug_set_cpu_reg(1, kObjDst);
  core.debug_set_cpu_reg(2, 2u);
  core.debug_set_cpu_reg(3, 8u); // output stride in bytes
  handled = core.debug_handle_swi_hle(0x080000A0u, false, 0xEF00000Fu, &cycles);
  expect(handled, "HLE SWI ObjAffineSet should be handled");
  expect(core.debug_read16(kObjDst + 0u) == 0x0100u, "ObjAffineSet matrix 0 PA should be identity");
  expect(core.debug_read16(kObjDst + 8u) == 0x0000u, "ObjAffineSet matrix 0 PB should be zero");
  expect(core.debug_read16(kObjDst + 16u) == 0x0000u, "ObjAffineSet matrix 0 PC should be zero");
  expect(core.debug_read16(kObjDst + 24u) == 0x0100u, "ObjAffineSet matrix 0 PD should be identity");
  expect(core.debug_read16(kObjDst + 32u) == 0x0000u, "ObjAffineSet matrix 1 PA should reflect 90-degree rotation");
  expect(core.debug_read16(kObjDst + 40u) == 0xFE00u, "ObjAffineSet matrix 1 PB should contain -sx");
  expect(core.debug_read16(kObjDst + 48u) == 0x0100u, "ObjAffineSet matrix 1 PC should contain +sy");
  expect(core.debug_read16(kObjDst + 56u) == 0x0000u, "ObjAffineSet matrix 1 PD should reflect 90-degree rotation");
}

void test_gba_hle_swi_wait_and_memcpy_paths() {
  gbemu::core::GbaCore core;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for HLE SWI wait/memcpy test");
  if (!ok) {
    return;
  }
  core.set_hle_swi(true);

  int cycles = 0;
  bool handled = false;

  // CpuSet 16-bit copy.
  constexpr std::uint32_t kSrc16 = 0x02001000u;
  constexpr std::uint32_t kDst16 = 0x02001100u;
  core.debug_write16(kSrc16 + 0u, 0x1111u);
  core.debug_write16(kSrc16 + 2u, 0x2222u);
  core.debug_write16(kSrc16 + 4u, 0x3333u);
  core.debug_set_cpu_reg(0, kSrc16);
  core.debug_set_cpu_reg(1, kDst16);
  core.debug_set_cpu_reg(2, 3u);
  handled = core.debug_handle_swi_hle(0x08000100u, false, 0xEF00000Bu, &cycles);
  expect(handled, "HLE SWI CpuSet should handle 16-bit copy");
  expect(core.debug_read16(kDst16 + 0u) == 0x1111u &&
             core.debug_read16(kDst16 + 2u) == 0x2222u &&
             core.debug_read16(kDst16 + 4u) == 0x3333u,
         "CpuSet 16-bit copy should transfer all requested halfwords");

  // CpuSet 32-bit fill.
  constexpr std::uint32_t kFillSrc = 0x02001200u;
  constexpr std::uint32_t kFillDst = 0x02001300u;
  core.debug_write32(kFillSrc, 0xA1B2C3D4u);
  core.debug_set_cpu_reg(0, kFillSrc);
  core.debug_set_cpu_reg(1, kFillDst);
  core.debug_set_cpu_reg(2, static_cast<std::uint32_t>((1u << 24) | (1u << 26) | 2u));
  handled = core.debug_handle_swi_hle(0x08000120u, false, 0xEF00000Bu, &cycles);
  expect(handled, "HLE SWI CpuSet should handle 32-bit fill");
  expect(core.debug_read32(kFillDst + 0u) == 0xA1B2C3D4u &&
             core.debug_read32(kFillDst + 4u) == 0xA1B2C3D4u,
         "CpuSet 32-bit fill should broadcast source word");

  // CpuFastSet fill rounds count down to multiples of 8 words.
  constexpr std::uint32_t kFastSrc = 0x02001400u;
  constexpr std::uint32_t kFastDst = 0x02001500u;
  core.debug_write32(kFastSrc, 0x55667788u);
  for (int i = 0; i < 10; ++i) {
    core.debug_write32(kFastDst + static_cast<std::uint32_t>(i * 4), 0xDEADBEEFu);
  }
  core.debug_set_cpu_reg(0, kFastSrc);
  core.debug_set_cpu_reg(1, kFastDst);
  core.debug_set_cpu_reg(2, static_cast<std::uint32_t>((1u << 24) | 10u));
  handled = core.debug_handle_swi_hle(0x08000140u, false, 0xEF00000Cu, &cycles);
  expect(handled, "HLE SWI CpuFastSet should handle fill");
  bool first_eight_filled = true;
  for (int i = 0; i < 8; ++i) {
    if (core.debug_read32(kFastDst + static_cast<std::uint32_t>(i * 4)) != 0x55667788u) {
      first_eight_filled = false;
      break;
    }
  }
  expect(first_eight_filled, "CpuFastSet should fill 8-word blocks");
  expect(core.debug_read32(kFastDst + 32u) == 0xDEADBEEFu,
         "CpuFastSet should leave non-block remainder words untouched");
}

void test_gba_hle_swi_halt_stop_paths() {
  auto run = [](std::uint32_t swi_imm, const std::string& label) {
    gbemu::core::GbaCore core;
    std::vector<std::uint8_t> rom = build_gba_spin_rom();
    std::vector<std::uint8_t> bios(0x4000, 0);
    std::string error;
    bool ok = core.load(rom, bios, &error);
    expect(ok, "GbaCore load should succeed for " + label + " test");
    if (!ok) {
      return;
    }
    core.set_hle_swi(true);

    int cycles = 0;
    std::uint32_t pc_before = 0x08000180u;
    bool handled = core.debug_handle_swi_hle(pc_before, false, 0xEF000000u | swi_imm, &cycles);
    expect(handled, label + " should be handled by HLE SWI path");
    expect(core.debug_halted(), label + " should place CPU in halted state");
    expect(core.cpu_pc() == pc_before + 4u, label + " should advance ARM PC by 4");
    expect(cycles > 0, label + " should report non-zero cycle cost");
  };

  run(0x02u, "SWI Halt");
  run(0x03u, "SWI Stop");
}

void test_gba_hle_swi_trig_decompress_and_bios_fallback() {
  gbemu::core::GbaCore core;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for SWI trig/decompress test");
  if (!ok) {
    return;
  }
  core.set_hle_swi(true);

  auto write_bytes = [](gbemu::core::GbaCore* target,
                        std::uint32_t addr,
                        const std::vector<std::uint8_t>& bytes) {
    if (!target) {
      return;
    }
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
      std::uint16_t lo = bytes[i];
      std::uint16_t hi = (i + 1 < bytes.size()) ? static_cast<std::uint16_t>(bytes[i + 1]) : 0u;
      target->debug_write16(addr + static_cast<std::uint32_t>(i),
                            static_cast<std::uint16_t>(lo | (hi << 8u)));
    }
  };
  auto read_bytes = [&core](std::uint32_t addr, std::size_t count) {
    std::vector<std::uint8_t> out(count, 0);
    for (std::size_t i = 0; i < count; i += 2) {
      std::uint16_t word = core.debug_read16(addr + static_cast<std::uint32_t>(i));
      out[i] = static_cast<std::uint8_t>(word & 0xFFu);
      if (i + 1 < count) {
        out[i + 1] = static_cast<std::uint8_t>((word >> 8u) & 0xFFu);
      }
    }
    return out;
  };

  int cycles = 0;
  bool handled = false;

  core.debug_set_cpu_reg(0, 0u);
  handled = core.debug_handle_swi_hle(0x08000200u, false, 0xEF000009u, &cycles);
  expect(handled, "HLE SWI ArcTan should be handled");
  expect(core.debug_cpu_reg(0) == 0u, "SWI ArcTan(0) should return 0 angle");

  core.debug_set_cpu_reg(0, 0x00010000u); // tan(45deg) in 16.16
  handled = core.debug_handle_swi_hle(0x08000204u, false, 0xEF000009u, &cycles);
  expect(handled, "HLE SWI ArcTan should handle positive fixed-point input");
  expect(angle_distance_u16(static_cast<std::uint16_t>(core.debug_cpu_reg(0) & 0xFFFFu), 0x2000u) < 0x0500,
         "SWI ArcTan should produce approximately 45-degree BIOS angle units");

  core.debug_set_cpu_reg(0, 0x00010000u); // y
  core.debug_set_cpu_reg(1, 0x00000000u); // x
  handled = core.debug_handle_swi_hle(0x08000208u, false, 0xEF00000Au, &cycles);
  expect(handled, "HLE SWI ArcTan2 should be handled");
  expect(angle_distance_u16(static_cast<std::uint16_t>(core.debug_cpu_reg(0) & 0xFFFFu), 0x4000u) < 0x0500,
         "SWI ArcTan2 should produce approximately 90-degree BIOS angle units");

  core.debug_set_cpu_reg(0, 0x00000000u); // y
  core.debug_set_cpu_reg(1, 0xFFFF0000u); // x < 0
  handled = core.debug_handle_swi_hle(0x0800020Cu, false, 0xEF00000Au, &cycles);
  expect(handled, "HLE SWI ArcTan2 should handle x<0 quadrant");
  expect(angle_distance_u16(static_cast<std::uint16_t>(core.debug_cpu_reg(0) & 0xFFFFu), 0x8000u) < 0x0600,
         "SWI ArcTan2 should produce approximately 180-degree BIOS angle units");

  constexpr std::uint32_t kLzSrc = 0x02002000u;
  constexpr std::uint32_t kRlSrc = 0x02002100u;
  constexpr std::uint32_t kWramDst = 0x02003000u;
  constexpr std::uint32_t kVramDst = 0x06000020u;

  const std::vector<std::uint8_t> lz_stream = {
      0x10u, 0x08u, 0x00u, 0x00u, // LZ77 header, output size 8
      0x20u,                      // flags: literal, literal, compressed
      'A', 'B',
      0x30u, 0x01u,               // len=6, disp=2
  };
  const std::vector<std::uint8_t> lz_expected = {'A', 'B', 'A', 'B', 'A', 'B', 'A', 'B'};
  write_bytes(&core, kLzSrc, lz_stream);

  core.debug_set_cpu_reg(0, kLzSrc);
  core.debug_set_cpu_reg(1, kWramDst);
  handled = core.debug_handle_swi_hle(0x08000210u, false, 0xEF000011u, &cycles);
  expect(handled, "HLE SWI LZ77UnCompWram should be handled");
  expect(read_bytes(kWramDst, lz_expected.size()) == lz_expected,
         "SWI LZ77UnCompWram should decode test stream correctly");

  core.debug_set_cpu_reg(0, kLzSrc);
  core.debug_set_cpu_reg(1, kVramDst);
  handled = core.debug_handle_swi_hle(0x08000214u, false, 0xEF000012u, &cycles);
  expect(handled, "HLE SWI LZ77UnCompVram should be handled");
  expect(read_bytes(kVramDst, lz_expected.size()) == lz_expected,
         "SWI LZ77UnCompVram should decode test stream correctly");

  const std::vector<std::uint8_t> rl_stream = {
      0x30u, 0x06u, 0x00u, 0x00u, // RL header, output size 6
      0x80u, 'C',                 // compressed run: 3x 'C'
      0x02u, 'D', 'D', 'E',       // raw run: 3 bytes
  };
  const std::vector<std::uint8_t> rl_expected = {'C', 'C', 'C', 'D', 'D', 'E'};
  write_bytes(&core, kRlSrc, rl_stream);

  core.debug_set_cpu_reg(0, kRlSrc);
  core.debug_set_cpu_reg(1, kWramDst + 0x20u);
  handled = core.debug_handle_swi_hle(0x08000218u, false, 0xEF000015u, &cycles);
  expect(handled, "HLE SWI RLUnCompWram should be handled");
  expect(read_bytes(kWramDst + 0x20u, rl_expected.size()) == rl_expected,
         "SWI RLUnCompWram should decode test stream correctly");

  core.debug_set_cpu_reg(0, kRlSrc);
  core.debug_set_cpu_reg(1, kVramDst + 0x20u);
  handled = core.debug_handle_swi_hle(0x0800021Cu, false, 0xEF000016u, &cycles);
  expect(handled, "HLE SWI RLUnCompVram should be handled");
  expect(read_bytes(kVramDst + 0x20u, rl_expected.size()) == rl_expected,
         "SWI RLUnCompVram should decode test stream correctly");

  // No real BIOS loaded: unsupported risky SWI should no-op in HLE path.
  core.debug_set_cpu_reg(15, 0x08000300u);
  handled = core.debug_handle_swi_hle(0x08000300u, false, 0xEF000013u, &cycles);
  expect(handled, "Unsupported risky SWI should no-op when no real BIOS is available");
  expect(core.cpu_pc() == 0x08000304u, "No-op risky SWI path should still advance PC");

  // Real BIOS present: risky/invalid SWI paths should fall back to BIOS execution.
  gbemu::core::GbaCore core_with_bios;
  std::vector<std::uint8_t> realish_bios(0x4000, 0);
  realish_bios[0] = 0xEA; // non-zero marker so fallback path treats BIOS as available
  ok = core_with_bios.load(rom, realish_bios, &error);
  expect(ok, "GbaCore load should succeed for BIOS fallback SWI test");
  if (!ok) {
    return;
  }
  core_with_bios.set_hle_swi(true);
  write_bytes(&core_with_bios, kLzSrc, {0x00u, 0x00u, 0x00u, 0x00u}); // invalid LZ header
  core_with_bios.debug_set_cpu_reg(0, kLzSrc);
  core_with_bios.debug_set_cpu_reg(1, kWramDst);
  handled = core_with_bios.debug_handle_swi_hle(0x08000400u, false, 0xEF000011u, &cycles);
  expect(!handled, "Malformed LZ77 stream should fall back to real BIOS path when available");
  handled = core_with_bios.debug_handle_swi_hle(0x08000404u, false, 0xEF000013u, &cycles);
  expect(!handled, "Unsupported risky SWI should fall back to real BIOS path when available");
}

void test_gba_timer_reload_shadow_and_cascade() {
  gbemu::core::GbaCore core;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for timer test");
  if (!ok) {
    return;
  }

  // Timer0 reload=0xFFFE, prescale=1, enable.
  core.debug_write16(0x04000100u, 0xFFFEu);
  core.debug_write16(0x04000102u, 0x0080u);
  core.debug_sync_timers();
  core.debug_step_timers(1);
  core.debug_sync_timers();
  core.debug_step_timers(1);
  expect(core.debug_read16(0x04000100u) == 0xFFFEu,
         "Timer reload should persist across counter mirror updates");

  // Timer1 count-up should increment once per Timer0 overflow (including multiple overflows in one step).
  core.debug_write16(0x04000100u, 0xFFFEu);
  core.debug_write16(0x04000102u, 0x0080u);
  core.debug_write16(0x04000104u, 0x0000u);
  core.debug_write16(0x04000106u, 0x0084u); // enable + count-up
  core.debug_sync_timers();
  core.debug_step_timers(4); // Timer0 overflows twice.
  expect(core.debug_read16(0x04000100u) == 0xFFFEu,
         "Timer0 should wrap and reload correctly across multiple ticks");
  expect(core.debug_read16(0x04000104u) == 0x0002u,
         "Cascaded timer should advance by number of upstream overflows");
}

void test_gba_dma_repeat_irq_and_start_modes() {
  gbemu::core::GbaCore core;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for DMA test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kSrc = 0x02000000u;
  constexpr std::uint32_t kDst = 0x02000100u;
  constexpr std::uint32_t kDma3Base = 0x040000D4u;
  constexpr std::uint32_t kDma0Base = 0x040000B0u;

  core.debug_write16(0x04000202u, 0xFFFFu); // clear IF

  core.debug_write16(kSrc + 0u, 0x1111u);
  core.debug_write16(kSrc + 2u, 0x2222u);
  core.debug_write16(kSrc + 4u, 0x3333u);
  core.debug_write16(kSrc + 6u, 0x4444u);

  core.debug_write32(kDma3Base + 0u, kSrc);
  core.debug_write32(kDma3Base + 4u, kDst);
  core.debug_write16(kDma3Base + 8u, 2u);
  // VBlank timing + repeat + dest reload + IRQ + enable.
  core.debug_write16(kDma3Base + 10u, static_cast<std::uint16_t>(0x8000u | 0x4000u | 0x0200u | 0x1000u | 0x0060u));

  core.debug_trigger_dma(1);
  expect(core.debug_read16(kDst + 0u) == 0x1111u && core.debug_read16(kDst + 2u) == 0x2222u,
         "DMA3 VBlank transfer should copy configured halfwords");
  expect(core.debug_read32(kDma3Base + 4u) == kDst,
         "DMA3 repeat with dest-reload should restore destination register");
  expect((core.debug_read16(kDma3Base + 10u) & 0x8000u) != 0,
         "DMA3 repeat transfer should remain enabled after trigger");
  expect((core.debug_read16(0x04000202u) & (1u << 11)) != 0,
         "DMA3 IRQ flag should be set in IF after completion");

  core.debug_trigger_dma(1);
  expect(core.debug_read16(kDst + 0u) == 0x3333u && core.debug_read16(kDst + 2u) == 0x4444u,
         "DMA3 repeat should continue from incremented source register on next trigger");

  core.debug_write16(0x04000202u, 0xFFFFu); // clear IF
  core.debug_write16(kSrc + 0x20u, 0xABCDu);
  core.debug_write32(kDma0Base + 0u, kSrc + 0x20u);
  core.debug_write32(kDma0Base + 4u, kDst + 0x20u);
  core.debug_write16(kDma0Base + 8u, 1u);
  // Immediate + repeat + IRQ + enable: repeat should be ignored for immediate one-shot.
  core.debug_write16(kDma0Base + 10u, static_cast<std::uint16_t>(0x8000u | 0x4000u | 0x0200u));
  core.debug_step_dma();
  expect(core.debug_read16(kDst + 0x20u) == 0xABCDu,
         "DMA0 immediate transfer should run on step_dma");
  expect((core.debug_read16(kDma0Base + 10u) & 0x8000u) == 0,
         "DMA0 immediate transfer should clear enable even when repeat is set");
  expect((core.debug_read16(0x04000202u) & (1u << 8)) != 0,
         "DMA0 IRQ flag should be set in IF after immediate transfer");

  core.debug_write16(0x04000202u, 0xFFFFu); // clear IF
  core.debug_write16(kSrc + 0x24u, 0xBCDEu);
  core.debug_write32(kDma0Base + 0u, kSrc + 0x24u);
  core.debug_write32(kDma0Base + 4u, kDst + 0x24u);
  core.debug_write16(kDma0Base + 8u, 1u);
  // DMA0 timing=3 is prohibited on hardware; core maps it to immediate for compatibility.
  core.debug_write16(kDma0Base + 10u, static_cast<std::uint16_t>(0x8000u | 0x3000u));
  core.debug_step_dma();
  expect(core.debug_read16(kDst + 0x24u) == 0xBCDEu,
         "DMA0 timing=3 compatibility path should execute as immediate");
}

void test_gba_irq_ack_priority_and_lr_entry() {
  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for IRQ ack/priority test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegIe = 0x04000200u;
  constexpr std::uint32_t kRegIf = 0x04000202u;
  constexpr std::uint32_t kRegIme = 0x04000208u;

  core.debug_set_cpu_reg(15, 0x08000000u);
  core.debug_write16(kRegIe, 0x0009u);  // VBlank + Timer0 enabled
  core.debug_set_if_bits(0x0009u);      // both pending
  core.debug_write16(kRegIme, 0x0001u); // IME enabled

  core.debug_service_interrupts();

  expect(core.debug_read16(kRegIf) == 0x0008u,
         "IRQ entry should clear only highest-priority pending IF bit");
  expect(core.debug_cpu_reg(14) == 0x08000004u,
         "IRQ entry should capture LR as PC+4 from inter-instruction boundary");
  expect(core.cpu_pc() < 0x08000000u,
         "After IRQ entry, CPU should execute from BIOS IRQ vector space");
}

void test_gba_irq_register_masking() {
  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for IRQ register masking test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegIe = 0x04000200u;
  constexpr std::uint32_t kRegIf = 0x04000202u;
  constexpr std::uint32_t kRegIme = 0x04000208u;

  core.debug_write16(kRegIe, 0xFFFFu);
  expect(core.debug_read16(kRegIe) == 0x3FFFu,
         "IE should mask to writable interrupt bits");

  core.debug_set_if_bits(0xFFFFu);
  expect(core.debug_read16(kRegIf) == 0x3FFFu,
         "IF set helper should only raise writable interrupt bits");

  core.debug_write16(kRegIme, 0xFFFFu);
  expect(core.debug_read16(kRegIme) == 0x0001u,
         "IME should only preserve bit 0");

  core.debug_write16(kRegIme, 0xFFFEu);
  expect(core.debug_read16(kRegIme) == 0x0000u,
         "IME bit 0 should clear when written as 0");
}

void test_gba_irq_ime_enable_gates_pending_service() {
  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for IME IRQ gating test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegIe = 0x04000200u;
  constexpr std::uint32_t kRegIf = 0x04000202u;
  constexpr std::uint32_t kRegIme = 0x04000208u;

  core.debug_set_cpu_reg(15, 0x08000000u);
  std::uint32_t cpsr = core.debug_cpu_cpsr();
  cpsr &= ~(1u << 7); // IRQ enabled in CPSR
  core.debug_set_cpu_cpsr(cpsr);
  core.debug_write16(kRegIe, 0x0001u);
  core.debug_set_if_bits(0x0001u);
  core.debug_write16(kRegIme, 0x0000u);

  core.debug_service_interrupts();
  expect(core.cpu_pc() == 0x08000000u,
         "IME=0 should block IRQ servicing even when IE/IF indicate pending interrupt");
  expect(core.debug_read16(kRegIf) == 0x0001u,
         "IME=0 should leave pending IF bit unchanged");

  core.debug_write16(kRegIme, 0x0001u);
  core.debug_service_interrupts();
  expect(core.cpu_pc() < 0x08000000u,
         "Re-enabling IME should allow pending interrupt to enter BIOS IRQ vector");
  expect(core.debug_read16(kRegIf) == 0x0000u,
         "IRQ service after IME enable should acknowledge the serviced IF bit");
}

void test_gba_irq_thumb_origin_lr_pipeline() {
  gbemu::core::GbaCore core;
  core.set_fastboot(true);
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for THUMB IRQ LR pipeline test");
  if (!ok) {
    return;
  }

  constexpr std::uint32_t kRegIe = 0x04000200u;
  constexpr std::uint32_t kRegIme = 0x04000208u;

  core.debug_set_cpu_reg(15, 0x08000002u); // next THUMB halfword at IRQ boundary
  std::uint32_t cpsr = core.debug_cpu_cpsr();
  cpsr &= ~(1u << 7); // IRQ enabled in CPSR
  cpsr |= (1u << 5);  // THUMB state
  core.debug_set_cpu_cpsr(cpsr);
  core.debug_write16(kRegIe, 0x0008u);     // Timer0 only
  core.debug_set_if_bits(0x0008u);         // pending timer interrupt
  core.debug_write16(kRegIme, 0x0001u);    // master IRQ enable

  core.debug_service_interrupts();

  expect(core.debug_cpu_reg(14) == 0x08000006u,
         "IRQ entry from THUMB state should capture LR as boundary PC+4");
  expect((core.debug_cpu_cpsr() & (1u << 5)) == 0,
         "IRQ entry should force ARM state in CPSR");
  expect((core.debug_cpu_cpsr() & (1u << 7)) != 0,
         "IRQ entry should set CPSR I bit (IRQ disable)");
}

void test_gba_ppu_mode5_bitmap_pages() {
  gbemu::core::GbaCore core;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for Mode 5 PPU test");
  if (!ok) {
    return;
  }

  // Mode 5 + BG2 enabled on page 0.
  core.debug_write16(0x04000000u, static_cast<std::uint16_t>(0x0400u | 0x0005u));
  core.debug_write16(0x06000000u, 0x7FFFu); // white at (0,0), page 0
  core.debug_render_line(0);

  const std::uint32_t* fb = core.framebuffer();
  expect(fb != nullptr, "Framebuffer should be available for Mode 5 test");
  if (!fb) {
    return;
  }
  expect(fb[0] == 0xFFFFFFFFu, "Mode 5 page 0 pixel should render as direct-color white");
  expect(fb[200] == 0xFF000000u,
         "Mode 5 should render backdrop outside 160px horizontal bitmap area");

  // Page flip to frame 1 and render different color at the same pixel.
  core.debug_write16(0x04000000u, static_cast<std::uint16_t>(0x0400u | 0x0010u | 0x0005u));
  core.debug_write16(0x0600A000u, 0x001Fu); // red at (0,0), page 1
  core.debug_render_line(0);
  expect(fb[0] == 0xFFFF0000u, "Mode 5 page bit should select frame 1 bitmap memory");
}

void test_gba_ppu_window_effect_mask() {
  gbemu::core::GbaCore core;
  std::vector<std::uint8_t> rom = build_gba_spin_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load(rom, bios, &error);
  expect(ok, "GbaCore load should succeed for window/effect mask test");
  if (!ok) {
    return;
  }

  // Mode 3 + BG2 + WIN0.
  core.debug_write16(0x04000000u, static_cast<std::uint16_t>(0x2000u | 0x0400u | 0x0003u));
  // WIN0 covers x=0,y=0 only (left=0,right=1, top=0,bottom=1).
  core.debug_write16(0x04000040u, 0x0001u);
  core.debug_write16(0x04000044u, 0x0001u);
  // WININ: inside WIN0 allow BG2 only (effects disabled).
  core.debug_write16(0x04000048u, 0x0004u);
  // WINOUT: outside allow BG2 + effects.
  core.debug_write16(0x0400004Au, 0x0024u);

  // BG2 pixels start black.
  core.debug_write16(0x06000000u, 0x0000u); // x=0
  core.debug_write16(0x06000002u, 0x0000u); // x=1

  // Brighten BG2 fully.
  core.debug_write16(0x04000050u, 0x0084u); // effect=2 (brighten), target1=BG2
  core.debug_write16(0x04000054u, 0x0010u); // EVY=16

  core.debug_render_line(0);
  const std::uint32_t* fb = core.framebuffer();
  expect(fb != nullptr, "Framebuffer should be available for window/effect test");
  if (!fb) {
    return;
  }
  expect(fb[0] == 0xFF000000u,
         "WIN0 with effects disabled should block brighten effect for covered pixel");
  expect(fb[1] == 0xFFFFFFFFu,
         "Outside WIN0 with effects enabled should apply brighten effect");
}

std::vector<std::uint8_t> build_gba_sram_rom() {
  std::vector<std::uint8_t> rom(0x400, 0);
  // ARM B . at 0x08000000
  rom[0] = 0xFE;
  rom[1] = 0xFF;
  rom[2] = 0xFF;
  rom[3] = 0xEA;
  const char* title = "SRAMTEST";
  for (int i = 0; title[i] != '\0' && (0xA0 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0xA0 + i] = static_cast<std::uint8_t>(title[i]);
  }
  rom[0xB2] = 0x96;
  const char* tag = "SRAM_V113";
  for (int i = 0; tag[i] != '\0' && (0x200 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0x200 + i] = static_cast<std::uint8_t>(tag[i]);
  }
  return rom;
}

std::vector<std::uint8_t> build_gba_flash_rom() {
  std::vector<std::uint8_t> rom(0x400, 0);
  rom[0] = 0xFE;
  rom[1] = 0xFF;
  rom[2] = 0xFF;
  rom[3] = 0xEA;
  const char* title = "FLASHTEST";
  for (int i = 0; title[i] != '\0' && (0xA0 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0xA0 + i] = static_cast<std::uint8_t>(title[i]);
  }
  rom[0xB2] = 0x96;
  const char* tag = "FLASH1M_V103";
  for (int i = 0; tag[i] != '\0' && (0x200 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0x200 + i] = static_cast<std::uint8_t>(tag[i]);
  }
  return rom;
}

std::vector<std::uint8_t> build_gba_eeprom_rom() {
  std::vector<std::uint8_t> rom(0x400, 0);
  rom[0] = 0xFE;
  rom[1] = 0xFF;
  rom[2] = 0xFF;
  rom[3] = 0xEA;
  const char* title = "EEPROMTEST";
  for (int i = 0; title[i] != '\0' && (0xA0 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0xA0 + i] = static_cast<std::uint8_t>(title[i]);
  }
  rom[0xB2] = 0x96;
  const char* tag = "EEPROM_V124";
  for (int i = 0; tag[i] != '\0' && (0x200 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0x200 + i] = static_cast<std::uint8_t>(tag[i]);
  }
  return rom;
}

void test_gba_flash_protocol() {
  gbemu::core::GbaBus bus;
  std::vector<std::uint8_t> rom = build_gba_flash_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for flash protocol test");
  if (!loaded) {
    return;
  }

  constexpr std::uint32_t kFlashBase = 0x0E000000u;
  auto write_flash = [&bus](std::uint32_t offset, std::uint8_t value) {
    bus.write8(0x0E000000u + offset, value);
  };
  auto unlock = [&write_flash]() {
    write_flash(0x5555u, 0xAAu);
    write_flash(0x2AAAu, 0x55u);
  };
  auto program = [&unlock, &write_flash](std::uint32_t offset, std::uint8_t value) {
    unlock();
    write_flash(0x5555u, 0xA0u);
    write_flash(offset, value);
  };

  expect(bus.read8(kFlashBase + 0x0123u) == 0xFFu, "Flash should initialize erased (0xFF)");
  program(0x0123u, 0x42u);
  expect(bus.read8(kFlashBase + 0x0123u) == 0x42u, "Flash program command should store byte");

  unlock();
  write_flash(0x5555u, 0x90u);
  expect(bus.read8(kFlashBase + 0x0000u) == 0xC2u, "Flash ID mode should expose manufacturer ID");
  expect(bus.read8(kFlashBase + 0x0001u) == 0x09u, "Flash ID mode should expose device ID");
  bus.write8(kFlashBase, 0xF0u);
  expect(bus.read8(kFlashBase + 0x0000u) == 0xFFu, "Flash ID mode should exit on 0xF0");

  program(0x0000u, 0x11u);
  unlock();
  write_flash(0x5555u, 0xB0u);
  write_flash(0x0000u, 0x01u);
  program(0x0000u, 0x22u);
  expect(bus.read8(kFlashBase + 0x0000u) == 0x22u, "Flash bank 1 should keep separate data");

  unlock();
  write_flash(0x5555u, 0xB0u);
  write_flash(0x0000u, 0x00u);
  expect(bus.read8(kFlashBase + 0x0000u) == 0x11u, "Flash bank 0 should restore prior banked data");

  program(0x0100u, 0x33u);
  unlock();
  write_flash(0x5555u, 0x80u);
  unlock();
  write_flash(0x0100u, 0x30u);
  expect(bus.read8(kFlashBase + 0x0100u) == 0xFFu, "Flash sector erase command should restore 0xFF");
}

void append_bits(std::vector<std::uint8_t>* bits, std::uint32_t value, int width) {
  if (!bits || width <= 0) {
    return;
  }
  for (int i = width - 1; i >= 0; --i) {
    bits->push_back(static_cast<std::uint8_t>((value >> i) & 0x1u));
  }
}

void test_gba_eeprom_protocol() {
  gbemu::core::GbaBus bus;
  std::vector<std::uint8_t> rom = build_gba_eeprom_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool loaded = bus.load(rom, bios, &error);
  expect(loaded, "GbaBus should load for EEPROM protocol test");
  if (!loaded) {
    return;
  }

  constexpr std::uint32_t kEepromAddr = 0x0D000000u;
  constexpr std::uint32_t kBlock = 3u;
  const std::vector<std::uint8_t> payload = {0xDEu, 0xADu, 0xBEu, 0xEFu, 0x12u, 0x34u, 0x56u, 0x78u};

  std::vector<std::uint8_t> write_bits = {1u, 0u};
  append_bits(&write_bits, kBlock, 14);
  for (std::uint8_t byte : payload) {
    append_bits(&write_bits, byte, 8);
  }
  write_bits.push_back(0u);
  for (std::uint8_t bit : write_bits) {
    bus.write16(kEepromAddr, static_cast<std::uint16_t>(bit));
  }

  std::vector<std::uint8_t> read_cmd = {1u, 1u};
  append_bits(&read_cmd, kBlock, 14);
  read_cmd.push_back(0u);
  for (std::uint8_t bit : read_cmd) {
    bus.write16(kEepromAddr, static_cast<std::uint16_t>(bit));
  }

  std::vector<std::uint8_t> read_bits;
  for (int i = 0; i < 68; ++i) {
    read_bits.push_back(static_cast<std::uint8_t>(bus.read16(kEepromAddr) & 0x1u));
  }

  expect(read_bits.size() == 68, "EEPROM read should return 68 bits (4 dummy + 64 data)");
  bool dummy_ok = read_bits.size() >= 4 && read_bits[0] == 0 && read_bits[1] == 0 &&
                  read_bits[2] == 0 && read_bits[3] == 0;
  expect(dummy_ok, "EEPROM read should include four leading dummy zero bits");

  std::vector<std::uint8_t> decoded(8, 0);
  if (read_bits.size() >= 68) {
    for (int i = 0; i < 64; ++i) {
      std::uint8_t bit = read_bits[4 + static_cast<std::size_t>(i)] & 0x1u;
      std::size_t byte_index = static_cast<std::size_t>(i / 8);
      decoded[byte_index] = static_cast<std::uint8_t>((decoded[byte_index] << 1) | bit);
    }
  }
  expect(decoded == payload, "EEPROM readback should match previously written 64-bit block");
}

void test_gba_save_persistence_api() {
  gbemu::core::EmulatorCore core;
  core.set_system(gbemu::core::System::GBA);
  core.set_gba_fastboot(true);

  std::vector<std::uint8_t> rom = build_gba_sram_rom();
  std::vector<std::uint8_t> bios(0x4000, 0);
  std::string error;
  bool ok = core.load_rom(rom, bios, &error);
  expect(ok, "core.load_rom should accept GBA SRAM-tagged ROM");
  if (!ok) {
    return;
  }

  expect(core.has_battery(), "GBA SRAM-tagged ROM should report battery-backed save");
  expect(core.has_ram(), "GBA SRAM-tagged ROM should report RAM save support");
  std::vector<std::uint8_t> initial = core.ram_data();
  expect(initial.size() == 65536, "GBA SRAM-tagged ROM should allocate 64KB save data");

  std::vector<std::uint8_t> inject(initial.size(), 0);
  inject[0] = 0x12;
  inject[1] = 0x34;
  inject[2] = 0x56;
  core.load_ram_data(inject);
  std::vector<std::uint8_t> after = core.ram_data();
  expect(after.size() == inject.size(), "GBA save data size should remain stable");
  expect(after[0] == 0x12 && after[1] == 0x34 && after[2] == 0x56,
         "GBA load_ram_data should persist injected bytes");
}

void test_state_version_compatibility() {
  gbemu::core::EmulatorCore gb;
  gb.set_system(gbemu::core::System::GB);
  std::vector<std::uint8_t> rom = {0x00, 0x00, 0x00};
  std::vector<std::uint8_t> boot(0x100, 0x00);
  std::string error;
  bool ok = gb.load_rom(rom, boot, &error);
  expect(ok, "GB load_rom should succeed for state version compatibility test");
  if (!ok) {
    return;
  }
  std::vector<std::uint8_t> state_v5;
  expect(gb.save_state(&state_v5), "GB save_state should succeed (v5)");
  if (state_v5.size() >= 6) {
    std::vector<std::uint8_t> state_v4 = state_v5;
    state_v4[4] = 0x04;
    state_v4[5] = 0x00;
    std::string load_error;
    expect(gb.load_state(state_v4, &load_error), "GB should accept legacy v4 state payload");
  }

  gbemu::core::EmulatorCore gba;
  gba.set_system(gbemu::core::System::GBA);
  gba.set_gba_fastboot(true);
  std::vector<std::uint8_t> gba_rom = build_gba_spin_rom();
  std::vector<std::uint8_t> gba_bios(0x4000, 0);
  ok = gba.load_rom(gba_rom, gba_bios, &error);
  expect(ok, "GBA load_rom should succeed for state version compatibility test");
  if (!ok) {
    return;
  }
  std::vector<std::uint8_t> gba_state_v5;
  expect(gba.save_state(&gba_state_v5), "GBA save_state should succeed (v5)");
  if (gba_state_v5.size() >= 6) {
    std::vector<std::uint8_t> gba_state_v4 = gba_state_v5;
    gba_state_v4[4] = 0x04;
    gba_state_v4[5] = 0x00;
    std::string load_error;
    bool loaded_v4 = gba.load_state(gba_state_v4, &load_error);
    expect(!loaded_v4, "GBA should reject legacy v4 states");
  }
}

struct ConformanceCase {
  std::string name;
  std::string pack;
  gbemu::core::System system = gbemu::core::System::GB;
  int frames = 120;
  bool gba_fastboot = false;
  std::vector<std::string> tokens;
};

struct ConformanceCaseStats {
  int executed = 0;
  int passed = 0;
  int failed = 0;
  int unknown = 0;
  int load_errors = 0;
  int cpu_faults = 0;
};

struct ConformancePackStats {
  int executed = 0;
  int passed = 0;
  int failed = 0;
  int unknown = 0;
};

struct ConformancePackBaseline {
  int min_executed = 0;
  int min_pass = 0;
  int max_fail = std::numeric_limits<int>::max();
  int max_unknown = std::numeric_limits<int>::max();
};

enum class ConformanceJudge {
  None,
  Blargg,
  Mooneye,
  GbaText,
};

enum class ConformanceVerdict {
  Unknown,
  Pass,
  Fail,
};

std::vector<ConformanceCase> default_conformance_cases() {
  return {
      {"blargg-smoke-instr-timing", "smoke", gbemu::core::System::GB, 240, false,
       {"conformance", "smoke", "blargg", "instr-timing"}},
      {"mooneye-smoke", "smoke", gbemu::core::System::GB, 240, false, {"conformance", "smoke", "mooneye"}},
      {"gba-smoke", "gba-smoke", gbemu::core::System::GBA, 120, true,
       {"conformance", "smoke", "gba", "mgba"}},
      {"gba-cpu-arm", "gba-cpu", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "cpu", "arm", "mgba"}},
      {"gba-cpu-thumb", "gba-cpu", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "cpu", "thumb", "mgba"}},
      {"gba-dma-timer", "gba-dma-timer", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "dma", "timer", "mgba"}},
      {"gba-mem-timing", "gba-mem-timing", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "mem", "timing", "mgba"}},
      {"gba-ppu-modes", "gba-ppu", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "ppu", "mgba"}},
      {"gba-swi-bios", "gba-swi-bios", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "swi", "bios", "mgba"}},
      {"gba-swi-compat", "gba-swi-compat", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "swi", "compat", "mgba"}},
      {"gba-swi-realbios", "gba-swi-realbios", gbemu::core::System::GBA, 180, true,
       {"conformance", "gba", "swi", "realhw", "mgba"}},
      {"gbc-ppu", "gbc-ppu", gbemu::core::System::GBC, 240, false, {"conformance", "gbc", "ppu"}},
      {"gb-timer-irq", "gb-timer-irq", gbemu::core::System::GB, 240, false, {"conformance", "gb", "timer", "irq"}},
  };
}

std::optional<int> parse_env_int(const char* name) {
  const char* value = std::getenv(name);
  if (!value || *value == '\0') {
    return std::nullopt;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::vector<std::uint8_t>> load_default_boot_rom(gbemu::core::System system) {
  std::filesystem::path path;
  switch (system) {
    case gbemu::core::System::GBA:
      path = "firmware/GBA/Game-Boy-Advance-Boot-ROM.bin";
      break;
    case gbemu::core::System::GBC:
      path = "firmware/GBC/Game-Boy-Color-Boot-ROM.gbc";
      break;
    case gbemu::core::System::GB:
    default:
      path = "firmware/GB/Game-Boy-Boot-ROM.gb";
      break;
  }
  std::vector<std::uint8_t> data;
  std::string error;
  if (!gbemu::common::read_file(path.string(), &data, &error)) {
    return std::nullopt;
  }
  return data;
}

bool path_has_tokens(std::string path, const std::vector<std::string>& tokens) {
  path = to_lower(path);
  for (const std::string& token : tokens) {
    if (path.find(to_lower(token)) == std::string::npos) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> split_tokens_csv(std::string raw) {
  std::vector<std::string> out;
  std::string token;
  auto flush = [&out, &token]() {
    if (token.empty()) {
      return;
    }
    std::string normalized = to_lower(token);
    if (std::find(out.begin(), out.end(), normalized) == out.end()) {
      out.push_back(normalized);
    }
    token.clear();
  };
  for (char c : raw) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (c == ',' || c == ';' || std::isspace(uc)) {
      flush();
    } else {
      token.push_back(c);
    }
  }
  flush();
  return out;
}

std::vector<std::string> parse_selected_conformance_packs(const char* packs_env) {
  std::string raw = packs_env ? packs_env : "";
  std::vector<std::string> packs = split_tokens_csv(raw);
  if (packs.empty()) {
    packs.push_back("smoke");
  }
  return packs;
}

std::vector<std::string> conformance_pack_names(const std::vector<ConformanceCase>& cases) {
  std::vector<std::string> names;
  for (const ConformanceCase& test_case : cases) {
    std::string pack = to_lower(test_case.pack);
    if (std::find(names.begin(), names.end(), pack) == names.end()) {
      names.push_back(pack);
    }
  }
  return names;
}

bool conformance_pack_selected(const std::string& pack, const std::vector<std::string>& selected) {
  std::string target = to_lower(pack);
  for (const std::string& pick : selected) {
    if (pick == "all" || pick == target) {
      return true;
    }
  }
  return false;
}

std::vector<ConformanceCase> filter_conformance_cases_by_pack(const std::vector<ConformanceCase>& cases,
                                                              const std::vector<std::string>& selected) {
  std::vector<ConformanceCase> filtered;
  for (const ConformanceCase& test_case : cases) {
    if (conformance_pack_selected(test_case.pack, selected)) {
      filtered.push_back(test_case);
    }
  }
  return filtered;
}

std::string join_strings(const std::vector<std::string>& values) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += ",";
    }
    out += values[i];
  }
  return out;
}

bool parse_env_bool(const char* name, bool default_value) {
  const char* raw = std::getenv(name);
  if (!raw || *raw == '\0') {
    return default_value;
  }
  std::string value = to_lower(raw);
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return default_value;
}

bool is_gba_swi_hle_conformance_pack(const ConformanceCase& test_case) {
  return test_case.system == gbemu::core::System::GBA &&
         (test_case.pack == "gba-swi-bios" || test_case.pack == "gba-swi-compat");
}

bool should_enable_gba_swi_hle_for_conformance(const ConformanceCase& test_case,
                                                bool force_real_bios_swi) {
  return is_gba_swi_hle_conformance_pack(test_case) && !force_real_bios_swi;
}

std::vector<std::uint8_t> build_conformance_boot_rom(
    gbemu::core::System system,
    const std::optional<std::vector<std::uint8_t>>& fallback_boot,
    bool prefer_real_boot = false) {
  if (system == gbemu::core::System::GBA) {
    return fallback_boot.value_or(std::vector<std::uint8_t>{});
  }

  bool force_synth_boot = parse_env_bool("GBEMU_CONFORMANCE_FORCE_SYNTH_BOOT", false);
  bool use_real_boot =
      !force_synth_boot && (parse_env_bool("GBEMU_CONFORMANCE_USE_REAL_BOOT", false) || prefer_real_boot);
  if (use_real_boot && fallback_boot.has_value()) {
    return *fallback_boot;
  }

  std::size_t size = (system == gbemu::core::System::GBC) ? 0x900 : 0x100;
  std::vector<std::uint8_t> boot(size, 0x00);
  std::vector<std::uint8_t> program;
  auto emit = [&program](std::initializer_list<std::uint8_t> bytes) {
    program.insert(program.end(), bytes.begin(), bytes.end());
  };
  auto emit_ldh_write = [&emit](std::uint8_t io_addr, std::uint8_t value) {
    emit({0x3E, value});    // LD A, d8
    emit({0xE0, io_addr});  // LDH (a8), A
  };

  // Approximate post-boot DMG/CGB state expected by test ROM harnesses.
  emit({0x31, 0xFE, 0xFF}); // LD SP,0xFFFE

  emit({0xAF});             // XOR A
  emit({0xE0, 0x05});       // TIMA = 0
  emit({0xE0, 0x06});       // TMA = 0
  emit({0xE0, 0x07});       // TAC = 0

  emit_ldh_write(0x10, 0x80); // NR10
  emit_ldh_write(0x11, 0xBF); // NR11
  emit_ldh_write(0x12, 0xF3); // NR12
  emit_ldh_write(0x14, 0xBF); // NR14
  emit_ldh_write(0x16, 0x3F); // NR21
  emit_ldh_write(0x17, 0x00); // NR22
  emit_ldh_write(0x19, 0xBF); // NR24
  emit_ldh_write(0x1A, 0x7F); // NR30
  emit_ldh_write(0x1B, 0xFF); // NR31
  emit_ldh_write(0x1C, 0x9F); // NR32
  emit_ldh_write(0x1E, 0xBF); // NR33/34
  emit_ldh_write(0x20, 0xFF); // NR41
  emit_ldh_write(0x21, 0x00); // NR42
  emit_ldh_write(0x22, 0x00); // NR43
  emit_ldh_write(0x23, 0xBF); // NR44
  emit_ldh_write(0x24, 0x77); // NR50
  emit_ldh_write(0x25, 0xF3); // NR51
  emit_ldh_write(0x26, (system == gbemu::core::System::GBC) ? 0xF0 : 0xF1); // NR52

  emit_ldh_write(0x40, 0x91); // LCDC
  emit_ldh_write(0x42, 0x00); // SCY
  emit_ldh_write(0x43, 0x00); // SCX
  emit_ldh_write(0x45, 0x00); // LYC
  emit_ldh_write(0x47, 0xFC); // BGP
  emit_ldh_write(0x48, 0xFF); // OBP0
  emit_ldh_write(0x49, 0xFF); // OBP1
  emit_ldh_write(0x4A, 0x00); // WY
  emit_ldh_write(0x4B, 0x00); // WX
  emit_ldh_write(0x0F, 0xE1); // IF

  // Recreate post-boot F=0xB0 (Z=1,N=0,H=1,C=1), then restore A.
  emit({0x3E, 0x8F}); // LD A,0x8F
  emit({0xC6, 0x71}); // ADD A,0x71 => A=0x00, F=0xB0

  std::uint8_t init_a = (system == gbemu::core::System::GBC) ? 0x11u : 0x01u;
  emit({0x3E, init_a}); // A
  emit({0x06, 0x00}); // B
  emit({0x0E, 0x13}); // C
  emit({0x16, 0x00}); // D
  emit({0x1E, 0xD8}); // E
  emit({0x26, 0x01}); // H
  emit({0x2E, 0x4D}); // L

  emit({0x3E, 0x00}); // IE = 0
  emit({0xEA, 0xFF, 0xFF});

  // Disable boot ROM and jump to cartridge entry at 0x0100.
  emit({0x3E, 0x01});
  emit({0xE0, 0x50});
  emit({0xC3, 0x00, 0x01});

  if (program.size() > boot.size()) {
    return {};
  }
  std::copy(program.begin(), program.end(), boot.begin());
  return boot;
}

bool contains_subsequence_bytes(const std::string& data, const std::vector<std::uint8_t>& seq) {
  if (seq.empty() || data.size() < seq.size()) {
    return false;
  }
  for (std::size_t i = 0; i + seq.size() <= data.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < seq.size(); ++j) {
      if (static_cast<std::uint8_t>(data[i + j]) != seq[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

bool contains_word_token_ci(const std::string& lower, const std::string& token) {
  if (token.empty() || lower.size() < token.size()) {
    return false;
  }
  std::size_t pos = lower.find(token);
  while (pos != std::string::npos) {
    bool left_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
    std::size_t right = pos + token.size();
    bool right_ok = (right >= lower.size()) ||
                    !std::isalnum(static_cast<unsigned char>(lower[right]));
    if (left_ok && right_ok) {
      return true;
    }
    pos = lower.find(token, pos + 1);
  }
  return false;
}

ConformanceJudge conformance_judge_for_path(const std::string& path) {
  std::string lower = to_lower(path);
  if (lower.find("blargg") != std::string::npos) {
    return ConformanceJudge::Blargg;
  }
  if (lower.find("mooneye") != std::string::npos) {
    return ConformanceJudge::Mooneye;
  }
  if (lower.find(".gba") != std::string::npos &&
      (lower.find("mgba") != std::string::npos ||
       lower.find("gba-tests") != std::string::npos ||
       lower.find("testsuite") != std::string::npos)) {
    return ConformanceJudge::GbaText;
  }
  return ConformanceJudge::None;
}

ConformanceVerdict evaluate_blargg_verdict(const std::string& serial_bytes) {
  std::string lower = to_lower(serial_bytes);
  if (lower.find("passed") != std::string::npos) {
    return ConformanceVerdict::Pass;
  }
  if (lower.find("failed") != std::string::npos || lower.find("fail") != std::string::npos) {
    return ConformanceVerdict::Fail;
  }
  return ConformanceVerdict::Unknown;
}

ConformanceVerdict evaluate_mooneye_verdict(const std::string& serial_bytes,
                                            const gbemu::core::EmulatorCore& core) {
  static const std::vector<std::uint8_t> kMooneyePass = {3, 5, 8, 13, 21, 34};
  static const std::vector<std::uint8_t> kMooneyeFail = {0x42, 0x42, 0x42, 0x42, 0x42, 0x42};
  if (contains_subsequence_bytes(serial_bytes, kMooneyePass)) {
    return ConformanceVerdict::Pass;
  }
  if (contains_subsequence_bytes(serial_bytes, kMooneyeFail)) {
    return ConformanceVerdict::Fail;
  }

  // Fallback for ROMs that expose pass/fail only via register signatures.
  gbemu::core::Cpu::Registers regs = core.gb_cpu_regs();
  bool pass_regs = regs.b == 3 && regs.c == 5 && regs.d == 8 && regs.e == 13 && regs.h == 21 &&
                   regs.l == 34;
  bool fail_regs = regs.b == 0x42 && regs.c == 0x42 && regs.d == 0x42 && regs.e == 0x42 &&
                   regs.h == 0x42 && regs.l == 0x42;
  if (pass_regs) {
    return ConformanceVerdict::Pass;
  }
  if (fail_regs) {
    return ConformanceVerdict::Fail;
  }
  return ConformanceVerdict::Unknown;
}

ConformanceVerdict evaluate_gba_text_verdict(const std::string& serial_bytes) {
  std::string lower = to_lower(serial_bytes);
  if (lower.find("not ok") != std::string::npos ||
      contains_word_token_ci(lower, "fail") ||
      contains_word_token_ci(lower, "failed") ||
      contains_word_token_ci(lower, "error")) {
    return ConformanceVerdict::Fail;
  }
  if (contains_word_token_ci(lower, "pass") ||
      contains_word_token_ci(lower, "passed") ||
      contains_word_token_ci(lower, "success") ||
      contains_word_token_ci(lower, "ok")) {
    return ConformanceVerdict::Pass;
  }
  return ConformanceVerdict::Unknown;
}

ConformanceVerdict evaluate_conformance_verdict(const std::string& rom_path,
                                                const std::string& serial_bytes,
                                                const gbemu::core::EmulatorCore& core) {
  ConformanceJudge judge = conformance_judge_for_path(rom_path);
  if (judge == ConformanceJudge::Blargg) {
    return evaluate_blargg_verdict(serial_bytes);
  }
  if (judge == ConformanceJudge::Mooneye) {
    return evaluate_mooneye_verdict(serial_bytes, core);
  }
  if (judge == ConformanceJudge::GbaText) {
    return evaluate_gba_text_verdict(serial_bytes);
  }
  return ConformanceVerdict::Unknown;
}

std::unordered_map<std::string, ConformancePackBaseline> load_conformance_pack_baselines(
    const std::filesystem::path& path) {
  std::unordered_map<std::string, ConformancePackBaseline> out;
  std::ifstream in(path);
  if (!in) {
    return out;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::stringstream ss(line);
    std::string pack;
    std::string min_executed_s;
    std::string min_pass_s;
    std::string max_fail_s;
    std::string max_unknown_s;
    if (!std::getline(ss, pack, ',')) {
      continue;
    }
    if (to_lower(pack) == "pack") {
      continue;
    }
    if (!std::getline(ss, min_executed_s, ',')) {
      continue;
    }
    if (!std::getline(ss, min_pass_s, ',')) {
      continue;
    }
    if (!std::getline(ss, max_fail_s, ',')) {
      continue;
    }
    if (!std::getline(ss, max_unknown_s, ',')) {
      continue;
    }
    try {
      ConformancePackBaseline b;
      b.min_executed = std::stoi(min_executed_s);
      b.min_pass = std::stoi(min_pass_s);
      b.max_fail = std::stoi(max_fail_s);
      b.max_unknown = std::stoi(max_unknown_s);
      out[to_lower(pack)] = b;
    } catch (...) {
      continue;
    }
  }
  return out;
}

void write_conformance_report(
    const std::filesystem::path& path,
    const std::vector<ConformanceCase>& cases,
    const std::unordered_map<std::string, ConformanceCaseStats>& case_stats,
    const std::unordered_map<std::string, ConformancePackStats>& pack_stats) {
  std::ofstream out(path);
  if (!out) {
    return;
  }
  out << "type,name,pack,executed,passed,failed,unknown,load_errors,cpu_faults\n";
  for (const ConformanceCase& test_case : cases) {
    auto it = case_stats.find(test_case.name);
    if (it == case_stats.end()) {
      continue;
    }
    const ConformanceCaseStats& s = it->second;
    out << "case," << test_case.name << "," << test_case.pack << "," << s.executed << "," << s.passed
        << "," << s.failed << "," << s.unknown << "," << s.load_errors << "," << s.cpu_faults << "\n";
  }
  for (const auto& [pack, s] : pack_stats) {
    out << "pack," << pack << "," << pack << "," << s.executed << "," << s.passed << "," << s.failed
        << "," << s.unknown << ",0,0\n";
  }
}

void test_conformance_pack_selection_and_filters() {
  std::vector<ConformanceCase> cases = default_conformance_cases();
  expect(!cases.empty(), "Conformance case list should not be empty");

  bool gba_cases_require_mgba = true;
  bool saw_gba_case = false;
  bool swi_bios_has_bios_token = false;
  bool swi_realbios_has_token = false;
  for (const ConformanceCase& test_case : cases) {
    bool is_gba_case = test_case.system == gbemu::core::System::GBA;
    if (!is_gba_case) {
      continue;
    }
    saw_gba_case = true;
    if (std::find(test_case.tokens.begin(), test_case.tokens.end(), "mgba") ==
        test_case.tokens.end()) {
      gba_cases_require_mgba = false;
    }
    if (test_case.pack == "gba-swi-bios" &&
        std::find(test_case.tokens.begin(), test_case.tokens.end(), "bios") !=
            test_case.tokens.end()) {
      swi_bios_has_bios_token = true;
    }
    if (test_case.pack == "gba-swi-realbios" &&
        std::find(test_case.tokens.begin(), test_case.tokens.end(), "realhw") !=
            test_case.tokens.end()) {
      swi_realbios_has_token = true;
    }
  }
  expect(saw_gba_case, "Conformance list should include GBA-focused cases");
  expect(gba_cases_require_mgba,
         "GBA conformance cases should target verdict-capable mgba-tagged ROM paths");
  expect(swi_bios_has_bios_token,
         "GBA SWI BIOS case should include a dedicated bios token to avoid overlap with compat pack");
  expect(swi_realbios_has_token,
         "GBA SWI real-BIOS case should include a dedicated realbios token");

  bool swi_hle_policy_ok = true;
  for (const ConformanceCase& test_case : cases) {
    bool is_swi = is_gba_swi_hle_conformance_pack(test_case);
    bool expect_default_hle = is_swi;
    bool expect_forced_real = false;
    bool actual_default_hle = should_enable_gba_swi_hle_for_conformance(test_case, false);
    bool actual_forced_real = should_enable_gba_swi_hle_for_conformance(test_case, true);
    if (actual_default_hle != expect_default_hle || actual_forced_real != expect_forced_real) {
      swi_hle_policy_ok = false;
      break;
    }
  }
  expect(swi_hle_policy_ok,
         "SWI conformance HLE policy should default-on for HLE SWI packs and disable when real BIOS mode is forced");

  std::vector<std::string> defaults = parse_selected_conformance_packs(nullptr);
  expect(defaults.size() == 1 && defaults[0] == "smoke",
         "Conformance pack selection should default to smoke");
  std::vector<ConformanceCase> smoke_only = filter_conformance_cases_by_pack(cases, defaults);
  expect(!smoke_only.empty(), "Smoke pack selection should produce at least one case");
  bool smoke_case_only = true;
  for (const ConformanceCase& test_case : smoke_only) {
    if (test_case.pack != "smoke") {
      smoke_case_only = false;
      break;
    }
  }
  expect(smoke_case_only, "Smoke selection should only include smoke-tagged conformance cases");

  std::vector<std::string> gba_packs = parse_selected_conformance_packs("gba-cpu, gba-dma-timer");
  std::vector<ConformanceCase> gba_filtered = filter_conformance_cases_by_pack(cases, gba_packs);
  expect(!gba_filtered.empty(), "Feature pack selection should include matching conformance cases");
  bool only_requested_packs = true;
  for (const ConformanceCase& test_case : gba_filtered) {
    if (test_case.pack != "gba-cpu" && test_case.pack != "gba-dma-timer") {
      only_requested_packs = false;
      break;
    }
  }
  expect(only_requested_packs, "Conformance filtering should include only requested packs");

  std::vector<std::string> all = parse_selected_conformance_packs("all");
  std::vector<ConformanceCase> all_cases = filter_conformance_cases_by_pack(cases, all);
  expect(all_cases.size() == cases.size(), "Pack=all should include every conformance case");

  expect(path_has_tokens("/tmp/Test-Games/Conformance/GBA/CPU/ARM/case.gba", {"gba", "cpu", "arm"}),
         "Conformance token matching should be case-insensitive");
  expect(!path_has_tokens("/tmp/Test-Games/Conformance/GBA/CPU/ARM/case.gba", {"gba", "ppu"}),
         "Conformance token matching should reject missing feature tokens");
}

void test_conformance_verdict_detection() {
  expect(conformance_judge_for_path("Test-Games/blargg/cpu_instrs/cpu_instrs.gb") ==
             ConformanceJudge::Blargg,
         "Conformance judge should classify blargg ROM paths");
  expect(conformance_judge_for_path("Test-Games/mooneye/acceptance/div_timing.gb") ==
             ConformanceJudge::Mooneye,
         "Conformance judge should classify mooneye ROM paths");
  expect(conformance_judge_for_path("Test-Games/Conformance/GBA/mgba/cpu-test.gba") ==
             ConformanceJudge::GbaText,
         "Conformance judge should classify GBA text-verdict ROM paths");

  expect(evaluate_blargg_verdict("CPU TESTS\nPassed\n") == ConformanceVerdict::Pass,
         "Blargg verdict detector should parse pass text");
  expect(evaluate_blargg_verdict("Failed #3\n") == ConformanceVerdict::Fail,
         "Blargg verdict detector should parse fail text");

  gbemu::core::EmulatorCore dummy;
  std::string mooneye_pass;
  mooneye_pass.push_back(static_cast<char>(3));
  mooneye_pass.push_back(static_cast<char>(5));
  mooneye_pass.push_back(static_cast<char>(8));
  mooneye_pass.push_back(static_cast<char>(13));
  mooneye_pass.push_back(static_cast<char>(21));
  mooneye_pass.push_back(static_cast<char>(34));
  expect(evaluate_mooneye_verdict(mooneye_pass, dummy) == ConformanceVerdict::Pass,
         "Mooneye verdict detector should parse fibonacci pass serial sequence");

  std::string mooneye_fail(6, static_cast<char>(0x42));
  expect(evaluate_mooneye_verdict(mooneye_fail, dummy) == ConformanceVerdict::Fail,
         "Mooneye verdict detector should parse repeated 0x42 fail serial sequence");

  expect(evaluate_gba_text_verdict("[PASS] all tests\n") == ConformanceVerdict::Pass,
         "GBA text verdict detector should parse pass markers");
  expect(evaluate_gba_text_verdict("suite failed: case 7\n") == ConformanceVerdict::Fail,
         "GBA text verdict detector should parse fail markers");
  expect(evaluate_gba_text_verdict("running...\n") == ConformanceVerdict::Unknown,
         "GBA text verdict detector should remain unknown without explicit verdict tokens");
}

void run_conformance_harness() {
  const char* run_env = std::getenv("GBEMU_RUN_CONFORMANCE");
  if (!run_env || std::string(run_env) != "1") {
    std::cout << "Conformance harness skipped. Set GBEMU_RUN_CONFORMANCE=1 to enable.\n";
    return;
  }

  std::filesystem::path root("Test-Games");
  const char* root_env = std::getenv("GBEMU_CONFORMANCE_ROOT");
  if (root_env && *root_env != '\0') {
    root = root_env;
  }
  if (!std::filesystem::exists(root)) {
    std::cout << "Conformance harness: root folder missing (" << root.string() << ").\n";
    return;
  }

  int max_per_case = parse_env_int("GBEMU_CONFORMANCE_MAX_PER_CASE").value_or(5);
  if (max_per_case <= 0) {
    max_per_case = 5;
  }

  std::vector<std::filesystem::path> roms;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (!is_rom_extension(entry.path())) {
      continue;
    }
    roms.push_back(entry.path());
  }

  std::vector<ConformanceCase> all_cases = default_conformance_cases();
  std::vector<std::string> selected_packs =
      parse_selected_conformance_packs(std::getenv("GBEMU_CONFORMANCE_PACKS"));
  std::vector<ConformanceCase> cases = filter_conformance_cases_by_pack(all_cases, selected_packs);
  std::vector<std::string> known_packs = conformance_pack_names(all_cases);
  std::cout << "Conformance packs selected: " << join_strings(selected_packs) << "\n";
  if (selected_packs.size() == 1 && selected_packs[0] == "all") {
    std::cout << "Conformance packs available: " << join_strings(known_packs) << "\n";
  } else {
    for (const std::string& pack : selected_packs) {
      if (pack == "all") {
        continue;
      }
      if (std::find(known_packs.begin(), known_packs.end(), pack) == known_packs.end()) {
        std::cout << "Conformance warning: unknown pack '" << pack << "'\n";
      }
    }
  }
  if (cases.empty()) {
    std::cout << "Conformance harness: no cases selected for requested packs.\n";
    return;
  }

  std::unordered_map<std::string, ConformanceCaseStats> case_stats;
  std::unordered_map<std::string, ConformancePackStats> pack_stats;
  bool debug_trace = parse_env_bool("GBEMU_CONFORMANCE_DEBUG_TRACE", false);
  bool auto_real_boot_mooneye = parse_env_bool("GBEMU_CONFORMANCE_MOONEYE_REAL_BOOT", true);
  bool force_real_bios_swi = parse_env_bool("GBEMU_CONFORMANCE_GBA_SWI_FORCE_REAL_BIOS", false);
  int real_boot_min_frames = parse_env_int("GBEMU_CONFORMANCE_REAL_BOOT_MIN_FRAMES").value_or(1200);
  if (real_boot_min_frames <= 0) {
    real_boot_min_frames = 1200;
  }
  if (force_real_bios_swi) {
    std::cout << "Conformance note: forcing real BIOS SWI execution for GBA SWI packs "
                 "(GBEMU_CONFORMANCE_GBA_SWI_FORCE_REAL_BIOS=1)\n";
  }

  for (const ConformanceCase& test_case : cases) {
    ConformanceCaseStats stats;
    std::optional<std::vector<std::uint8_t>> fallback_boot_rom = load_default_boot_rom(test_case.system);
    if (test_case.system == gbemu::core::System::GBA && !fallback_boot_rom.has_value()) {
      std::cout << "Conformance " << test_case.name << ": missing GBA BIOS, skipping.\n";
      continue;
    }
    int selected = 0;
    int frame_budget = parse_env_int("GBEMU_CONFORMANCE_FRAME_LIMIT").value_or(test_case.frames);
    if (frame_budget <= 0) {
      frame_budget = test_case.frames;
    }
    for (const auto& path : roms) {
      if (selected >= max_per_case) {
        break;
      }
      std::string full = path.string();
      if (!path_has_tokens(full, test_case.tokens)) {
        continue;
      }
      std::vector<std::uint8_t> data;
      std::string read_error;
      if (!gbemu::common::read_file(full, &data, &read_error) || data.empty()) {
        continue;
      }
      gbemu::core::System detected = detect_system(data);
      bool system_match = (detected == test_case.system);
      if (!system_match && test_case.system == gbemu::core::System::GB &&
          detected == gbemu::core::System::GBC) {
        system_match = true;
      }
      if (!system_match) {
        continue;
      }

      ConformanceJudge judge = conformance_judge_for_path(full);
      bool prefer_real_boot = auto_real_boot_mooneye && judge == ConformanceJudge::Mooneye;
      std::vector<std::uint8_t> boot_rom =
          build_conformance_boot_rom(test_case.system, fallback_boot_rom, prefer_real_boot);
      if (boot_rom.empty()) {
        ++stats.failed;
        ++stats.load_errors;
        expect(false, "Conformance boot ROM unavailable: " + full);
        continue;
      }

      ++selected;
      ++stats.executed;
      gbemu::core::EmulatorCore core;
      core.set_system(test_case.system);
      if (debug_trace && test_case.system != gbemu::core::System::GBA) {
        core.set_cpu_trace_enabled(true);
      }
      if (test_case.system == gbemu::core::System::GBA && test_case.gba_fastboot) {
        core.set_gba_fastboot(true);
      }
      if (should_enable_gba_swi_hle_for_conformance(test_case, force_real_bios_swi)) {
        // Keep SWI conformance deterministic even when the local BIOS image is missing
        // or not suitable for software-interrupt regression execution.
        core.set_gba_hle_swi(true);
      }
      std::string load_error;
      if (!core.load_rom(data, boot_rom, &load_error)) {
        ++stats.failed;
        ++stats.load_errors;
        expect(false, "Conformance load failed: " + full + " (" + load_error + ")");
        continue;
      }

      std::string serial_bytes;
      ConformanceVerdict verdict = ConformanceVerdict::Unknown;
      int per_rom_budget = frame_budget;
      if (prefer_real_boot && per_rom_budget < real_boot_min_frames) {
        per_rom_budget = real_boot_min_frames;
      }
      for (int i = 0; i < per_rom_budget && !core.cpu_faulted(); ++i) {
        core.step_frame();
        serial_bytes += core.take_serial_output();
        verdict = evaluate_conformance_verdict(full, serial_bytes, core);
        if (verdict != ConformanceVerdict::Unknown) {
          break;
        }
      }

      if (core.cpu_faulted()) {
        ++stats.failed;
        ++stats.cpu_faults;
        expect(false, "Conformance fault: " + full + " (" + core.cpu_fault_reason() + ")");
        continue;
      }

      if (verdict == ConformanceVerdict::Pass) {
        ++stats.passed;
      } else if (verdict == ConformanceVerdict::Fail) {
        ++stats.failed;
        std::cout << "Conformance fail signal: " << full << "\n";
        if (debug_trace) {
          gbemu::core::Cpu::Registers regs = core.gb_cpu_regs();
          std::cout << "Conformance debug fail verdict: " << full
                    << " serial_bytes=" << serial_bytes.size()
                    << " pc=0x" << std::hex << core.cpu_pc()
                    << " op=0x" << static_cast<int>(core.cpu_opcode())
                    << " halted=" << (core.gb_cpu_halted() ? "1" : "0")
                    << " b=0x" << static_cast<int>(regs.b)
                    << " c=0x" << static_cast<int>(regs.c)
                    << " d=0x" << static_cast<int>(regs.d)
                    << " e=0x" << static_cast<int>(regs.e)
                    << " h=0x" << static_cast<int>(regs.h)
                    << " l=0x" << static_cast<int>(regs.l)
                    << std::dec << "\n";
          std::cout << "Conformance debug serial: " << serial_debug_text(serial_bytes) << "\n";
          auto trace = core.cpu_trace();
          std::cout << "Conformance trace tail:";
          for (const auto& entry : trace) {
            std::cout << " [pc=0x" << std::hex << entry.pc
                      << " op=0x" << static_cast<int>(entry.opcode) << std::dec << "]";
          }
          std::cout << "\n";
        }
      } else if (judge != ConformanceJudge::None) {
        ++stats.unknown;
        gbemu::core::Cpu::Registers regs = core.gb_cpu_regs();
        std::cout << "Conformance debug missing verdict: " << full
                  << " serial_bytes=" << serial_bytes.size()
                  << " pc=0x" << std::hex << core.cpu_pc()
                  << " op=0x" << static_cast<int>(core.cpu_opcode())
                  << " halted=" << (core.gb_cpu_halted() ? "1" : "0")
                  << " b=0x" << static_cast<int>(regs.b)
                  << " c=0x" << static_cast<int>(regs.c)
                  << " d=0x" << static_cast<int>(regs.d)
                  << " e=0x" << static_cast<int>(regs.e)
                  << " h=0x" << static_cast<int>(regs.h)
                  << " l=0x" << static_cast<int>(regs.l)
                  << std::dec << "\n";
        std::cout << "Conformance debug serial: " << serial_debug_text(serial_bytes) << "\n";
        if (debug_trace) {
          auto trace = core.cpu_trace();
          std::cout << "Conformance trace tail:";
          for (const auto& entry : trace) {
            std::cout << " [pc=0x" << std::hex << entry.pc
                      << " op=0x" << static_cast<int>(entry.opcode) << std::dec << "]";
          }
          std::cout << "\n";
        }
      } else {
        ++stats.unknown;
      }
    }

    case_stats[test_case.name] = stats;
    ConformancePackStats& p = pack_stats[test_case.pack];
    p.executed += stats.executed;
    p.passed += stats.passed;
    p.failed += stats.failed;
    p.unknown += stats.unknown;
    std::cout << "Conformance " << test_case.name << ": exec=" << stats.executed
              << " pass=" << stats.passed << " fail=" << stats.failed
              << " unknown=" << stats.unknown << "\n";
  }

  std::filesystem::path baseline_path = "tests/conformance_pack_baseline.csv";
  const char* baseline_env = std::getenv("GBEMU_CONFORMANCE_BASELINE_FILE");
  if (baseline_env && *baseline_env != '\0') {
    baseline_path = baseline_env;
  }
  bool enforce_baseline = parse_env_bool("GBEMU_CONFORMANCE_ENFORCE_BASELINE", true);
  auto baselines = load_conformance_pack_baselines(baseline_path);
  if (enforce_baseline && !baselines.empty()) {
    for (const auto& [pack, stats] : pack_stats) {
      auto it = baselines.find(to_lower(pack));
      if (it == baselines.end()) {
        expect(false, "Conformance baseline missing pack entry: " + pack);
        continue;
      }
      const ConformancePackBaseline& b = it->second;
      if (stats.executed < b.min_executed) {
        expect(false, "Conformance regression (" + pack + "): executed " +
                          std::to_string(stats.executed) + " < baseline min_executed " +
                          std::to_string(b.min_executed));
      }
      if (stats.passed < b.min_pass) {
        expect(false, "Conformance regression (" + pack + "): passed " +
                          std::to_string(stats.passed) + " < baseline min_pass " +
                          std::to_string(b.min_pass));
      }
      if (stats.failed > b.max_fail) {
        expect(false, "Conformance regression (" + pack + "): failed " +
                          std::to_string(stats.failed) + " > baseline max_fail " +
                          std::to_string(b.max_fail));
      }
      if (stats.unknown > b.max_unknown) {
        expect(false, "Conformance regression (" + pack + "): unknown " +
                          std::to_string(stats.unknown) + " > baseline max_unknown " +
                          std::to_string(b.max_unknown));
      }
    }
  } else if (enforce_baseline && baselines.empty()) {
    std::cout << "Conformance warning: baseline file not found or empty at "
              << baseline_path.string() << "\n";
  }

  for (const auto& [pack, stats] : pack_stats) {
    std::cout << "Conformance pack " << pack << ": exec=" << stats.executed
              << " pass=" << stats.passed << " fail=" << stats.failed
              << " unknown=" << stats.unknown << "\n";
  }

  const char* report_env = std::getenv("GBEMU_CONFORMANCE_REPORT_PATH");
  if (report_env && *report_env != '\0') {
    std::filesystem::path report_path = report_env;
    write_conformance_report(report_path, cases, case_stats, pack_stats);
    std::cout << "Conformance report written: " << report_path.string() << "\n";
  }
}

std::string to_lower(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::string serial_debug_text(const std::string& serial_bytes) {
  std::string out;
  out.reserve(serial_bytes.size());
  for (unsigned char c : serial_bytes) {
    if (c == '\n' || c == '\r' || c == '\t') {
      out.push_back(static_cast<char>(c));
    } else if (c >= 32 && c <= 126) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('.');
    }
  }
  return out;
}

bool is_rom_extension(const std::filesystem::path& path) {
  std::string ext = to_lower(path.extension().string());
  return ext == ".gb" || ext == ".gbc" || ext == ".gba";
}

gbemu::core::System detect_system(const std::vector<std::uint8_t>& data) {
  if (data.size() >= 0xC0 && data[0xB2] == 0x96) {
    return gbemu::core::System::GBA;
  }
  if (data.size() >= 0x150) {
    std::uint8_t cgb_flag = data[0x0143];
    if (cgb_flag == 0x80 || cgb_flag == 0xC0) {
      return gbemu::core::System::GBC;
    }
  }
  return gbemu::core::System::GB;
}

std::string system_label(gbemu::core::System system) {
  switch (system) {
    case gbemu::core::System::GBA:
      return "GBA";
    case gbemu::core::System::GBC:
      return "GBC";
    case gbemu::core::System::GB:
    default:
      return "GB";
  }
}

std::string rom_title(const std::vector<std::uint8_t>& data) {
  if (data.size() >= 0xC0 && data[0xB2] == 0x96) {
    std::string title;
    for (std::size_t i = 0; i < 12; ++i) {
      char c = static_cast<char>(data[0xA0 + i]);
      if (c == '\0') break;
      title.push_back(c);
    }
    return title;
  }

  if (data.size() >= 0x150) {
    std::string title;
    for (std::size_t i = 0; i < 16; ++i) {
      char c = static_cast<char>(data[0x0134 + i]);
      if (c == '\0') break;
      title.push_back(c);
    }
    while (!title.empty() && title.back() == ' ') {
      title.pop_back();
    }
    return title;
  }

  return "";
}

void scan_test_games() {
  std::filesystem::path root("Test-Games");
  if (!std::filesystem::exists(root)) {
    std::cout << "Test-Games folder not found. Skipping ROM scan.\n";
    return;
  }

  std::size_t count = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (!is_rom_extension(entry.path())) {
      continue;
    }

    ++count;
    std::vector<std::uint8_t> data;
    std::string error;
    std::string path = entry.path().string();
    bool ok = gbemu::common::read_file(path, &data, &error);
    expect(ok, "should read ROM: " + path);
    expect(!data.empty(), "ROM should not be empty: " + path);

    if (ok && !data.empty()) {
      gbemu::core::System system = detect_system(data);
      std::string title = rom_title(data);
      std::cout << "ROM: " << system_label(system) << " | "
                << (title.empty() ? "(no title)" : title) << " | "
                << path << "\n";
    }
  }

  if (count == 0) {
    std::cout << "No ROMs found under Test-Games.\n";
  } else {
    std::cout << "Scanned " << count << " ROM(s) under Test-Games.\n";
  }
}

} // namespace

int main() {
  test_read_file_missing();
  test_read_file_roundtrip();
  test_config_parse();
  test_ppu_sizes();
  test_boot_rom_required();
  test_boot_rom_size();
  test_gb_serial_transfer_capture();
  test_gb_ei_delay_before_interrupt_service();
  test_gb_interrupt_register_unused_bits();
  test_gb_halt_ime0_resume_executes_next_instruction();
  test_gb_stat_irq_blocking_across_mode0_to_mode1();
  test_gb_cb_bit_hl_timing();
  test_gba_state_roundtrip();
  test_gba_cpu_swp_and_swpb();
  test_gba_cpu_arm_long_multiply();
  test_gba_cpu_arm_load_store_rrx_offset();
  test_gba_waitcnt_nseq_seq_fetch_cycles();
  test_gba_prefetch_reduces_sequential_fetch_penalty();
  test_gba_prefetch_pipeline_refill_on_branch_and_mode_switch();
  test_gba_prefetch_stream_break_on_gamepak_data_access();
  test_gba_waitcnt_write_flushes_prefetch_stream();
  test_gba_prefetch_stream_break_on_dma_activity();
  test_gba_prefetch_stream_break_on_irq_service_event();
  test_gba_waitcnt_ws1_ws2_region_timing();
  test_gba_data_access_width_and_sram_wait_timing();
  test_gba_gamepak_data_burst_sequential_wait_timing();
  test_gba_gamepak_data_stream_break_on_event_invalidation();
  test_gba_mgba_debug_output_capture();
  test_gba_hle_swi_math_and_affine();
  test_gba_hle_swi_wait_and_memcpy_paths();
  test_gba_hle_swi_halt_stop_paths();
  test_gba_hle_swi_trig_decompress_and_bios_fallback();
  test_gba_timer_reload_shadow_and_cascade();
  test_gba_dma_repeat_irq_and_start_modes();
  test_gba_irq_ack_priority_and_lr_entry();
  test_gba_irq_register_masking();
  test_gba_irq_ime_enable_gates_pending_service();
  test_gba_irq_thumb_origin_lr_pipeline();
  test_gba_ppu_mode5_bitmap_pages();
  test_gba_ppu_window_effect_mask();
  test_gba_flash_protocol();
  test_gba_eeprom_protocol();
  test_gba_save_persistence_api();
  test_state_version_compatibility();
  test_conformance_pack_selection_and_filters();
  test_conformance_verdict_detection();
  run_conformance_harness();
  scan_test_games();

  if (failures == 0) {
    std::cout << "All tests passed.\n";
    return 0;
  }

  std::cout << failures << " test(s) failed.\n";
  return 1;
}
