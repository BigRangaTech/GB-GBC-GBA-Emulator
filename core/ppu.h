#pragma once

#include <cstdint>
#include <memory>

#include "ppu_backend.h"
#include "system.h"

namespace gbemu::core {

class Ppu {
 public:
  Ppu();

  void set_system(System system);
  System system() const;

  int width() const;
  int height() const;
  int stride_bytes() const;
  const std::uint32_t* framebuffer() const;

  void step_frame();

 private:
  System system_ = System::GB;
  std::unique_ptr<PpuBackend> backend_;
};

} // namespace gbemu::core
