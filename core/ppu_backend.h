#pragma once

#include <cstdint>
#include <memory>

#include "system.h"

namespace gbemu::core {

class PpuBackend {
 public:
  virtual ~PpuBackend() = default;

  virtual System system() const = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual int stride_bytes() const = 0;
  virtual const std::uint32_t* framebuffer() const = 0;

  virtual void step_frame() = 0;
};

std::unique_ptr<PpuBackend> CreatePpuBackend(System system);

} // namespace gbemu::core
