#include "ppu.h"

namespace gbemu::core {

Ppu::Ppu() : backend_(CreatePpuBackend(system_)) {}

void Ppu::set_system(System system) {
  if (system_ == system) {
    return;
  }
  system_ = system;
  backend_ = CreatePpuBackend(system_);
}

System Ppu::system() const {
  return system_;
}

int Ppu::width() const {
  return backend_ ? backend_->width() : 0;
}

int Ppu::height() const {
  return backend_ ? backend_->height() : 0;
}

int Ppu::stride_bytes() const {
  return backend_ ? backend_->stride_bytes() : 0;
}

const std::uint32_t* Ppu::framebuffer() const {
  return backend_ ? backend_->framebuffer() : nullptr;
}

void Ppu::step_frame() {
  if (backend_) {
    backend_->step_frame();
  }
}

} // namespace gbemu::core
