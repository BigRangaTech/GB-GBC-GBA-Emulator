#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

  std::string version() const;
  bool load_rom(const std::vector<std::uint8_t>& rom,
                const std::vector<std::uint8_t>& boot_rom,
                std::string* error);

 private:
  System system_ = System::GB;
  Ppu ppu_;
  Mmu mmu_;
  Cpu cpu_;
};

} // namespace gbemu::core
