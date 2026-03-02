#include "core.h"

#include "common.h"
#include "timing.h"
#include "state_io.h"

namespace gbemu::core {

EmulatorCore::EmulatorCore() = default;

void EmulatorCore::set_system(System system) {
  if (system_ == system) {
    return;
  }
  system_ = system;
  ppu_.set_system(system);
}

System EmulatorCore::system() const {
  return system_;
}

int EmulatorCore::framebuffer_width() const {
  if (system_ == System::GBA) {
    return gba_.framebuffer_width();
  }
  return ppu_.width();
}

int EmulatorCore::framebuffer_height() const {
  if (system_ == System::GBA) {
    return gba_.framebuffer_height();
  }
  return ppu_.height();
}

int EmulatorCore::framebuffer_stride_bytes() const {
  if (system_ == System::GBA) {
    return gba_.framebuffer_stride_bytes();
  }
  return ppu_.stride_bytes();
}

const std::uint32_t* EmulatorCore::framebuffer() const {
  if (system_ == System::GBA) {
    return gba_.framebuffer();
  }
  return ppu_.framebuffer();
}

void EmulatorCore::step_frame() {
  if (system_ == System::GBA) {
    gba_.step_frame();
    return;
  }
  int cycles_target = 0;
  switch (system_) {
    case System::GBA:
      cycles_target = kGbaCyclesPerFrame;
      break;
    case System::GB:
    case System::GBC:
    default:
      cycles_target = kGbCyclesPerFrame;
      break;
  }

  int cycles = 0;
  while (cycles < cycles_target && !cpu_.faulted()) {
    int used = cpu_.step();
    if (used <= 0) {
      break;
    }
    mmu_.step(used);
    apu_.step(used, &mmu_);
    ppu_.step(used, &mmu_);
    cycles += used;
  }
}

double EmulatorCore::target_fps() const {
  switch (system_) {
    case System::GBA:
      return 60.0;
    case System::GB:
    case System::GBC:
    default:
      return 59.7275;
  }
}

std::string EmulatorCore::version() const {
  return gbemu::common::version();
}

bool EmulatorCore::cpu_faulted() const {
  if (system_ == System::GBA) {
    return gba_.cpu_faulted();
  }
  return cpu_.faulted();
}

std::string EmulatorCore::cpu_fault_reason() const {
  if (system_ == System::GBA) {
    return gba_.cpu_fault_reason();
  }
  return cpu_.fault_reason();
}

std::uint16_t EmulatorCore::cpu_pc() const {
  if (system_ == System::GBA) {
    return static_cast<std::uint16_t>(gba_.cpu_pc() & 0xFFFF);
  }
  return cpu_.last_pc();
}

std::uint8_t EmulatorCore::cpu_opcode() const {
  if (system_ == System::GBA) {
    return 0;
  }
  return cpu_.last_opcode();
}

Cpu::Registers EmulatorCore::gb_cpu_regs() const {
  if (system_ == System::GBA) {
    return {};
  }
  return cpu_.regs();
}

bool EmulatorCore::gb_cpu_halted() const {
  if (system_ == System::GBA) {
    return false;
  }
  return cpu_.halted();
}

std::string EmulatorCore::take_serial_output() {
  if (system_ == System::GBA) {
    return "";
  }
  return mmu_.take_serial_output();
}

void EmulatorCore::set_cpu_trace_enabled(bool enabled) {
  if (system_ == System::GBA) {
    return;
  }
  cpu_.set_trace_enabled(enabled);
}

void EmulatorCore::set_gba_trace(int steps, bool trace_io) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_trace(steps, trace_io);
}

void EmulatorCore::set_gba_trace_after_rom(int steps, bool trace_io) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_trace_after_rom(steps, trace_io);
}

std::vector<Cpu::TraceEntry> EmulatorCore::cpu_trace() const {
  if (system_ == System::GBA) {
    return {};
  }
  return cpu_.trace();
}

void EmulatorCore::set_debug_window_overlay(bool enabled) {
  ppu_.set_debug_window_overlay(enabled);
}

void EmulatorCore::set_cgb_color_correction(bool enabled) {
  ppu_.set_cgb_color_correction(enabled);
}

void EmulatorCore::set_gba_color_correction(bool enabled) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_gba_color_correction(enabled);
}

void EmulatorCore::set_gba_auto_handoff(bool enabled) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_auto_handoff(enabled);
}

void EmulatorCore::set_gba_fastboot(bool enabled) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_fastboot(enabled);
}

void EmulatorCore::set_gba_log_unimplemented(int limit) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_log_unimplemented(limit);
}

void EmulatorCore::set_gba_watch_video_io(int limit) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_watch_video_io(limit);
}

void EmulatorCore::set_gba_watch_io_reads(int limit) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_watch_io_reads(limit);
}

void EmulatorCore::set_gba_mem_watch(std::uint32_t start,
                                     std::uint32_t end,
                                     int count,
                                     bool read,
                                     bool write) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_mem_watch(start, end, count, read, write);
}

void EmulatorCore::set_gba_log_swi(int limit) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_log_swi(limit);
}

void EmulatorCore::set_gba_watchdog(int steps) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_watchdog_steps(steps);
}

void EmulatorCore::set_gba_pc_watch(std::uint32_t start, std::uint32_t end, int count) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_pc_watch(start, end, count);
}

void EmulatorCore::set_gba_auto_patch_hang(bool enabled) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_auto_patch_hang(enabled);
}

void EmulatorCore::set_gba_auto_patch_threshold(int count) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_auto_patch_threshold(count);
}

void EmulatorCore::set_gba_auto_patch_span(std::uint32_t span) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_auto_patch_span(span);
}

void EmulatorCore::set_gba_auto_patch_range(std::uint32_t start, std::uint32_t end) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_auto_patch_range(start, end);
}

void EmulatorCore::set_gba_hle_swi(bool enabled) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_hle_swi(enabled);
}

void EmulatorCore::set_gba_trace_assert(bool enabled) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_trace_assert(enabled);
}

void EmulatorCore::set_gba_bypass_assert(bool enabled) {
  if (system_ != System::GBA) {
    return;
  }
  gba_.set_bypass_assert(enabled);
}

void EmulatorCore::set_joypad_state(std::uint8_t state) {
  if (system_ == System::GBA) {
    std::uint16_t keyinput = static_cast<std::uint16_t>(state) & 0xFF;
    keyinput |= 0x0300u;
    gba_.set_keyinput(keyinput);
    return;
  }
  mmu_.set_joypad_state(state);
}

void EmulatorCore::request_interrupt(std::uint8_t bit) {
  if (system_ == System::GBA) {
    return;
  }
  mmu_.request_interrupt(bit);
}

void EmulatorCore::generate_audio(int sample_rate, int samples, std::vector<std::int16_t>* out) {
  if (system_ == System::GBA) {
    if (out) {
      out->clear();
    }
    return;
  }
  apu_.generate_samples(sample_rate, samples, &mmu_, out);
}

bool EmulatorCore::boot_rom_enabled() const {
  if (system_ == System::GBA) {
    return false;
  }
  return mmu_.boot_rom_enabled();
}

bool EmulatorCore::has_battery() const {
  if (system_ == System::GBA) {
    return gba_.has_battery();
  }
  return mmu_.has_battery();
}

bool EmulatorCore::has_ram() const {
  if (system_ == System::GBA) {
    return gba_.has_ram();
  }
  return mmu_.has_ram();
}

bool EmulatorCore::has_rtc() const {
  if (system_ == System::GBA) {
    return false;
  }
  return mmu_.has_rtc();
}

std::vector<std::uint8_t> EmulatorCore::ram_data() const {
  if (system_ == System::GBA) {
    return gba_.ram_data();
  }
  return mmu_.ram_data();
}

void EmulatorCore::load_ram_data(const std::vector<std::uint8_t>& data) {
  if (system_ == System::GBA) {
    gba_.load_ram_data(data);
    return;
  }
  mmu_.load_ram_data(data);
}

std::vector<std::uint8_t> EmulatorCore::rtc_data() const {
  if (system_ == System::GBA) {
    return {};
  }
  return mmu_.rtc_data();
}

void EmulatorCore::load_rtc_data(const std::vector<std::uint8_t>& data) {
  if (system_ == System::GBA) {
    return;
  }
  mmu_.load_rtc_data(data);
}

bool EmulatorCore::save_state(std::vector<std::uint8_t>* out) const {
  if (!out) {
    return false;
  }
  out->clear();
  using namespace gbemu::core::state_io;
  out->push_back('G');
  out->push_back('B');
  out->push_back('S');
  out->push_back('T');
  write_u16(*out, 5);
  write_u8(*out, static_cast<std::uint8_t>(system_));
  if (system_ == System::GBA) {
    gba_.serialize(out);
    return true;
  }
  cpu_.serialize(out);
  mmu_.serialize(out);
  ppu_.serialize(out);
  apu_.serialize(out);
  return true;
}

bool EmulatorCore::load_state(const std::vector<std::uint8_t>& data, std::string* error) {
  using namespace gbemu::core::state_io;
  if (data.size() < 6) {
    if (error) *error = "State data too small";
    return false;
  }
  if (!(data[0] == 'G' && data[1] == 'B' && data[2] == 'S' && data[3] == 'T')) {
    if (error) *error = "Invalid state header";
    return false;
  }
  std::size_t offset = 4;
  std::uint16_t version = 0;
  if (!read_u16(data, offset, version)) return false;
  if (version != 4 && version != 5) {
    if (error) *error = "Unsupported state version";
    return false;
  }
  std::uint8_t sys = 0;
  if (!read_u8(data, offset, sys)) return false;
  if (sys != static_cast<std::uint8_t>(system_)) {
    if (error) *error = "State system mismatch";
    return false;
  }
  if (system_ == System::GBA) {
    if (version < 5) {
      if (error) *error = "GBA states require version 5+";
      return false;
    }
    return gba_.deserialize(data, offset, error);
  }
  if (!cpu_.deserialize(data, offset, error)) return false;
  if (!mmu_.deserialize(data, offset, error)) return false;
  if (!ppu_.deserialize(data, offset, error)) return false;
  if (!apu_.deserialize(data, offset, error)) return false;
  return true;
}

bool EmulatorCore::load_rom(const std::vector<std::uint8_t>& rom,
                            const std::vector<std::uint8_t>& boot_rom,
                            std::string* error) {
  if (system_ == System::GBA) {
    return gba_.load(rom, boot_rom, error);
  }
  if (!mmu_.load(system_, rom, boot_rom, error)) {
    return false;
  }
  cpu_.connect(&mmu_);
  cpu_.reset();
  apu_.reset();
  return true;
}

} // namespace gbemu::core
