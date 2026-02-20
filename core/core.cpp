#include "core.h"

#include "common.h"

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
  return ppu_.width();
}

int EmulatorCore::framebuffer_height() const {
  return ppu_.height();
}

int EmulatorCore::framebuffer_stride_bytes() const {
  return ppu_.stride_bytes();
}

const std::uint32_t* EmulatorCore::framebuffer() const {
  return ppu_.framebuffer();
}

void EmulatorCore::step_frame() {
  ppu_.step_frame();
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

} // namespace gbemu::core
