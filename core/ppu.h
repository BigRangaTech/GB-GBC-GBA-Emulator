#pragma once

#include <cstdint>
#include <memory>

#include <array>
#include <vector>

#include "ppu_backend.h"
#include "system.h"
#include "mmu.h"

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
 std::uint32_t* framebuffer_mut();

  void step(int cycles, Mmu* mmu);
  void set_debug_window_overlay(bool enabled);
  void set_cgb_color_correction(bool enabled);
  void serialize(std::vector<std::uint8_t>* out) const;
  bool deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error);

 private:
  void render_scanline(int line, Mmu* mmu);
  std::uint32_t dmg_color(int index) const;
  std::uint32_t cgb_color(Mmu* mmu, bool obj, int palette, int index) const;
  void render_sprites(int line, Mmu* mmu, std::uint8_t lcdc,
                      const std::array<std::uint8_t, 160>& bg_index,
                      const std::array<bool, 160>& bg_priority);

  System system_ = System::GB;
  std::unique_ptr<PpuBackend> backend_;
  int dot_counter_ = 0;
  int line_ = 0;
  int mode_ = 0;
  bool coincidence_ = false;
  bool debug_window_overlay_ = false;
  bool cgb_color_correction_ = false;
};

} // namespace gbemu::core
