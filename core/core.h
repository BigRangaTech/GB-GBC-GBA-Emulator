#pragma once

#include <string>

#include "ppu.h"
#include "system.h"

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

 private:
  System system_ = System::GB;
  Ppu ppu_;
};

} // namespace gbemu::core
