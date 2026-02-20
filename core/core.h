#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apu.h"
#include "cpu.h"
#include "ppu.h"
#include "system.h"
#include "mmu.h"

namespace gbemu::core {

class EmulatorCore {
 public:
  EmulatorCore();

  void set_system(System system);
  System system() const;

  int framebuffer_width() const;
  int framebuffer_height() const;
  int framebuffer_stride_bytes() const;
  const std::uint32_t* framebuffer() const;

  void step_frame();
  double target_fps() const;
  bool cpu_faulted() const;
  std::string cpu_fault_reason() const;
  std::uint16_t cpu_pc() const;
  std::uint8_t cpu_opcode() const;
  void set_cpu_trace_enabled(bool enabled);
  std::vector<Cpu::TraceEntry> cpu_trace() const;
  void set_debug_window_overlay(bool enabled);
  void set_cgb_color_correction(bool enabled);
  void set_joypad_state(std::uint8_t state);
  void request_interrupt(std::uint8_t bit);
  void generate_audio(int sample_rate, int samples, std::vector<std::int16_t>* out);
  bool boot_rom_enabled() const;
  bool has_battery() const;
  bool has_ram() const;
  bool has_rtc() const;
  std::vector<std::uint8_t> ram_data() const;
  void load_ram_data(const std::vector<std::uint8_t>& data);
  std::vector<std::uint8_t> rtc_data() const;
  void load_rtc_data(const std::vector<std::uint8_t>& data);
  bool save_state(std::vector<std::uint8_t>* out) const;
  bool load_state(const std::vector<std::uint8_t>& data, std::string* error);

  std::string version() const;
  bool load_rom(const std::vector<std::uint8_t>& rom,
                const std::vector<std::uint8_t>& boot_rom,
                std::string* error);

 private:
  System system_ = System::GB;
  Ppu ppu_;
  Mmu mmu_;
  Cpu cpu_;
  Apu apu_;
};

} // namespace gbemu::core
