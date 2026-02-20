#include "ppu_backend.h"

#include <vector>

namespace gbemu::core {

namespace {

class GbPpuBackend final : public PpuBackend {
 public:
  GbPpuBackend() { resize(); }

  System system() const override { return System::GB; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  int stride_bytes() const override { return width_ * static_cast<int>(sizeof(std::uint32_t)); }
  const std::uint32_t* framebuffer() const override { return framebuffer_.data(); }
  std::uint32_t* framebuffer_mut() override { return framebuffer_.data(); }

  void step_frame() override {
    ++frame_counter_;
    static constexpr std::uint32_t kPalette[4] = {
        0xFF0F380Fu,
        0xFF306230u,
        0xFF8BAC0Fu,
        0xFF9BBC0Fu,
    };

    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        std::uint8_t shade = static_cast<std::uint8_t>(((x + y + frame_counter_) / 12) % 4);
        framebuffer_[static_cast<std::size_t>(y) * width_ + x] = kPalette[shade];
      }
    }
  }

 private:
  void resize() {
    width_ = 160;
    height_ = 144;
    framebuffer_.assign(static_cast<std::size_t>(width_) * height_, 0xFF0F380Fu);
    frame_counter_ = 0;
  }

  int width_ = 160;
  int height_ = 144;
  std::vector<std::uint32_t> framebuffer_;
  std::uint64_t frame_counter_ = 0;
};

class GbcPpuBackend final : public PpuBackend {
 public:
  GbcPpuBackend() { resize(); }

  System system() const override { return System::GBC; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  int stride_bytes() const override { return width_ * static_cast<int>(sizeof(std::uint32_t)); }
  const std::uint32_t* framebuffer() const override { return framebuffer_.data(); }
  std::uint32_t* framebuffer_mut() override { return framebuffer_.data(); }

  void step_frame() override {
    ++frame_counter_;
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        std::uint8_t r = static_cast<std::uint8_t>((x * 2 + frame_counter_) % 256);
        std::uint8_t g = static_cast<std::uint8_t>((y * 2 + frame_counter_ * 3) % 256);
        std::uint8_t b = static_cast<std::uint8_t>((x + y + frame_counter_) % 256);
        framebuffer_[static_cast<std::size_t>(y) * width_ + x] =
            (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
      }
    }
  }

 private:
  void resize() {
    width_ = 160;
    height_ = 144;
    framebuffer_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
    frame_counter_ = 0;
  }

  int width_ = 160;
  int height_ = 144;
  std::vector<std::uint32_t> framebuffer_;
  std::uint64_t frame_counter_ = 0;
};

class GbaPpuBackend final : public PpuBackend {
 public:
  GbaPpuBackend() { resize(); }

  System system() const override { return System::GBA; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  int stride_bytes() const override { return width_ * static_cast<int>(sizeof(std::uint32_t)); }
  const std::uint32_t* framebuffer() const override { return framebuffer_.data(); }
  std::uint32_t* framebuffer_mut() override { return framebuffer_.data(); }

  void step_frame() override {
    ++frame_counter_;
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        std::uint8_t r = static_cast<std::uint8_t>((x + frame_counter_ * 2) % 256);
        std::uint8_t g = static_cast<std::uint8_t>((y + frame_counter_) % 256);
        std::uint8_t b = static_cast<std::uint8_t>((x ^ y ^ static_cast<int>(frame_counter_)) & 0xFF);
        framebuffer_[static_cast<std::size_t>(y) * width_ + x] =
            (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
      }
    }
  }

 private:
  void resize() {
    width_ = 240;
    height_ = 160;
    framebuffer_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
    frame_counter_ = 0;
  }

  int width_ = 240;
  int height_ = 160;
  std::vector<std::uint32_t> framebuffer_;
  std::uint64_t frame_counter_ = 0;
};

} // namespace

std::unique_ptr<PpuBackend> CreatePpuBackend(System system) {
  switch (system) {
    case System::GBA:
      return std::make_unique<GbaPpuBackend>();
    case System::GBC:
      return std::make_unique<GbcPpuBackend>();
    case System::GB:
    default:
      return std::make_unique<GbPpuBackend>();
  }
}

} // namespace gbemu::core
