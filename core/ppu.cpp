#include "ppu.h"

#include <algorithm>

#include "state_io.h"

namespace gbemu::core {

Ppu::Ppu() : backend_(CreatePpuBackend(system_)) {}

void Ppu::set_system(System system) {
  if (system_ == system) {
    return;
  }
  system_ = system;
  backend_ = CreatePpuBackend(system_);
  dot_counter_ = 0;
  line_ = 0;
  mode_ = 0;
  coincidence_ = false;
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

std::uint32_t* Ppu::framebuffer_mut() {
  return backend_ ? backend_->framebuffer_mut() : nullptr;
}

void Ppu::set_debug_window_overlay(bool enabled) {
  debug_window_overlay_ = enabled;
}

void Ppu::set_cgb_color_correction(bool enabled) {
  cgb_color_correction_ = enabled;
}

void Ppu::serialize(std::vector<std::uint8_t>* out) const {
  if (!out || !backend_) {
    return;
  }
  using namespace gbemu::core::state_io;
  write_u8(*out, static_cast<std::uint8_t>(system_));
  write_u32(*out, static_cast<std::uint32_t>(dot_counter_));
  write_u32(*out, static_cast<std::uint32_t>(line_));
  write_u32(*out, static_cast<std::uint32_t>(mode_));
  write_bool(*out, coincidence_);
  write_bool(*out, debug_window_overlay_);
  write_bool(*out, cgb_color_correction_);

  int w = width();
  int h = height();
  write_u32(*out, static_cast<std::uint32_t>(w));
  write_u32(*out, static_cast<std::uint32_t>(h));

  const std::uint32_t* fb = framebuffer();
  std::uint32_t count = static_cast<std::uint32_t>(w * h);
  write_u32(*out, count);
  for (std::uint32_t i = 0; i < count; ++i) {
    write_u32(*out, fb[i]);
  }
}

bool Ppu::deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error) {
  using namespace gbemu::core::state_io;
  std::uint8_t sys = 0;
  if (!read_u8(data, offset, sys)) return false;
  if (sys != static_cast<std::uint8_t>(system_)) {
    if (error) *error = "PPU system mismatch";
    return false;
  }
  std::uint32_t v32 = 0;
  if (!read_u32(data, offset, v32)) return false;
  dot_counter_ = static_cast<int>(v32);
  if (!read_u32(data, offset, v32)) return false;
  line_ = static_cast<int>(v32);
  if (!read_u32(data, offset, v32)) return false;
  mode_ = static_cast<int>(v32);
  if (!read_bool(data, offset, coincidence_)) return false;
  if (!read_bool(data, offset, debug_window_overlay_)) return false;
  if (!read_bool(data, offset, cgb_color_correction_)) return false;

  std::uint32_t w = 0;
  std::uint32_t h = 0;
  if (!read_u32(data, offset, w)) return false;
  if (!read_u32(data, offset, h)) return false;
  std::uint32_t count = 0;
  if (!read_u32(data, offset, count)) return false;
  std::uint32_t expected = static_cast<std::uint32_t>(width() * height());
  if (w != static_cast<std::uint32_t>(width()) || h != static_cast<std::uint32_t>(height()) ||
      count != expected) {
    if (error) *error = "PPU framebuffer size mismatch";
    return false;
  }
  std::uint32_t* fb = framebuffer_mut();
  if (!fb) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!read_u32(data, offset, fb[i])) return false;
  }
  return true;
}

void Ppu::step(int cycles, Mmu* mmu) {
  if (!mmu || !backend_) {
    return;
  }

  std::uint8_t lcdc = mmu->read_u8(0xFF40);
  if ((lcdc & 0x80) == 0) {
    dot_counter_ = 0;
    line_ = 0;
    mode_ = 0;
    mmu->set_ly(0);
    std::uint8_t stat = mmu->read_u8(0xFF41);
    stat = static_cast<std::uint8_t>((stat & 0xF8) | 0x00);
    mmu->set_stat(stat);
    return;
  }

  dot_counter_ += cycles;
  while (dot_counter_ >= 456) {
    dot_counter_ -= 456;
    ++line_;
    if (line_ == 144) {
      mmu->request_interrupt(0);
    }
    if (line_ > 153) {
      line_ = 0;
      if (system_ == System::GBA) {
        backend_->step_frame();
      }
    }
    if (line_ < 144 && system_ != System::GBA) {
      render_scanline(line_, mmu);
    }
  }

  int new_mode = 0;
  if (line_ >= 144) {
    new_mode = 1;
  } else if (dot_counter_ < 80) {
    new_mode = 2;
  } else if (dot_counter_ < 252) {
    new_mode = 3;
  } else {
    new_mode = 0;
  }

  std::uint8_t stat = mmu->read_u8(0xFF41);
  if (new_mode != mode_) {
    if (new_mode == 0 && (stat & 0x08)) {
      mmu->request_interrupt(1);
    } else if (new_mode == 1 && (stat & 0x10)) {
      mmu->request_interrupt(1);
    } else if (new_mode == 2 && (stat & 0x20)) {
      mmu->request_interrupt(1);
    }
  }
  mode_ = new_mode;

  std::uint8_t lyc = mmu->read_u8(0xFF45);
  bool now_coincident = (static_cast<std::uint8_t>(line_) == lyc);
  if (now_coincident && !coincidence_ && (stat & 0x40)) {
    mmu->request_interrupt(1);
  }
  coincidence_ = now_coincident;

  stat = static_cast<std::uint8_t>((stat & 0xF8) | (mode_ & 0x03));
  if (coincidence_) {
    stat = static_cast<std::uint8_t>(stat | 0x04);
  } else {
    stat = static_cast<std::uint8_t>(stat & ~0x04);
  }

  mmu->set_stat(stat);
  mmu->set_ly(static_cast<std::uint8_t>(line_));
}

void Ppu::render_scanline(int line, Mmu* mmu) {
  if (!mmu || !backend_) {
    return;
  }
  if (line < 0 || line >= height()) {
    return;
  }

  std::uint8_t lcdc = mmu->read_u8(0xFF40);
  std::uint8_t scy = mmu->read_u8(0xFF42);
  std::uint8_t scx = mmu->read_u8(0xFF43);
  std::uint8_t bgp = mmu->read_u8(0xFF47);
  std::uint8_t wy = mmu->read_u8(0xFF4A);
  std::uint8_t wx = mmu->read_u8(0xFF4B);

  std::uint32_t* fb = framebuffer_mut();
  if (!fb) {
    return;
  }

  bool bg_enable = (lcdc & 0x01) != 0;
  bool window_enable = (lcdc & 0x20) != 0;
  bool window_active = window_enable && bg_enable && line >= wy && wx <= 166;
  int window_x = static_cast<int>(wx) - 7;

  std::uint16_t bg_map_base = (lcdc & 0x08) ? 0x9C00 : 0x9800;
  std::uint16_t win_map_base = (lcdc & 0x40) ? 0x9C00 : 0x9800;
  bool tile_data_unsigned = (lcdc & 0x10) != 0;

  int bg_y = (static_cast<int>(scy) + line) & 0xFF;
  int bg_tile_row = (bg_y / 8) * 32;
  int bg_tile_y = bg_y & 7;

  int win_y = line - static_cast<int>(wy);
  int win_tile_row = ((win_y < 0 ? 0 : win_y) / 8) * 32;
  int win_tile_y = (win_y < 0 ? 0 : win_y) & 7;

  std::array<std::uint8_t, 160> bg_index{};
  std::array<bool, 160> bg_priority{};

  for (int x = 0; x < width(); ++x) {
    std::uint8_t shade = 0;
    std::uint8_t palette_index = 0;
    bool priority = false;

    if (bg_enable) {
      bool use_window = window_active && x >= window_x;
      int px = use_window ? (x - window_x) : ((static_cast<int>(scx) + x) & 0xFF);
      int tile_col = (px / 8) & 31;
      int tile_x = px & 7;
      std::uint16_t tile_map_base = use_window ? win_map_base : bg_map_base;
      int tile_row = use_window ? win_tile_row : bg_tile_row;
      int tile_y = use_window ? win_tile_y : bg_tile_y;
      std::uint16_t tile_index_addr = static_cast<std::uint16_t>(tile_map_base + tile_row + tile_col);

      std::uint8_t tile_index = mmu->read_vram(tile_index_addr, 0);
      std::uint8_t attr = (system_ == System::GBC) ? mmu->read_vram(tile_index_addr, 1) : 0;
      bool xflip = (attr & 0x20) != 0;
      bool yflip = (attr & 0x40) != 0;
      int tile_vram_bank = (attr & 0x08) ? 1 : 0;

      int tile_y_eff = yflip ? (7 - tile_y) : tile_y;
      int tile_x_eff = xflip ? tile_x : (7 - tile_x);

      std::int16_t tile_id = tile_data_unsigned
                                 ? static_cast<std::int16_t>(tile_index)
                                 : static_cast<std::int8_t>(tile_index);
      std::uint16_t tile_base = tile_data_unsigned ? 0x8000 : 0x9000;
      std::uint16_t tile_addr = static_cast<std::uint16_t>(tile_base + tile_id * 16);
      std::uint16_t row_addr = static_cast<std::uint16_t>(tile_addr + tile_y_eff * 2);
      std::uint8_t lo = mmu->read_vram(row_addr, tile_vram_bank);
      std::uint8_t hi = mmu->read_vram(static_cast<std::uint16_t>(row_addr + 1), tile_vram_bank);
      std::uint8_t bit = static_cast<std::uint8_t>(tile_x_eff & 7);
      shade = static_cast<std::uint8_t>(((hi >> bit) & 0x01) << 1 |
                                        ((lo >> bit) & 0x01));
      if (system_ == System::GBC) {
        palette_index = static_cast<std::uint8_t>(attr & 0x07);
        priority = (attr & 0x80) != 0;
      } else {
        palette_index = static_cast<std::uint8_t>((bgp >> (shade * 2)) & 0x03);
      }
    }

    bg_index[static_cast<std::size_t>(x)] = shade;
    bg_priority[static_cast<std::size_t>(x)] = priority;

    std::uint32_t color = dmg_color(0);
    if (system_ == System::GBC) {
      color = cgb_color(mmu, false, palette_index, shade);
    } else {
      color = dmg_color(palette_index);
    }

    fb[static_cast<std::size_t>(line) * width() + x] = color;
  }

  render_sprites(line, mmu, lcdc, bg_index, bg_priority);

  if (debug_window_overlay_ && window_enable && line >= wy && wx <= 166) {
    int left = window_x;
    if (left < 0) {
      left = 0;
    }
    if (left < width()) {
      int right = width() - 1;
      int top = static_cast<int>(wy);
      int bottom = height() - 1;
      if (line == top || line == bottom) {
        for (int x = left; x <= right; ++x) {
          fb[static_cast<std::size_t>(line) * width() + x] = 0xFFFF00FFu;
        }
      } else if (line > top && line < bottom) {
        fb[static_cast<std::size_t>(line) * width() + left] = 0xFFFF00FFu;
        fb[static_cast<std::size_t>(line) * width() + right] = 0xFFFF00FFu;
      }
    }
  }
}

std::uint32_t Ppu::dmg_color(int index) const {
  static constexpr std::uint32_t palette[4] = {
      0xFF9BBC0Fu,
      0xFF8BAC0Fu,
      0xFF306230u,
      0xFF0F380Fu,
  };
  if (index < 0 || index > 3) {
    return palette[0];
  }
  return palette[index];
}

std::uint32_t Ppu::cgb_color(Mmu* mmu, bool obj, int palette, int index) const {
  if (!mmu) {
    return 0xFF000000u;
  }
  int pal = palette & 0x07;
  int col = index & 0x03;
  int base = (pal * 4 + col) * 2;
  std::uint8_t lo = obj ? mmu->obj_palette_byte(base) : mmu->bg_palette_byte(base);
  std::uint8_t hi = obj ? mmu->obj_palette_byte(base + 1) : mmu->bg_palette_byte(base + 1);
  std::uint16_t raw = static_cast<std::uint16_t>(lo | (hi << 8));
  std::uint8_t r5 = static_cast<std::uint8_t>(raw & 0x1F);
  std::uint8_t g5 = static_cast<std::uint8_t>((raw >> 5) & 0x1F);
  std::uint8_t b5 = static_cast<std::uint8_t>((raw >> 10) & 0x1F);
  std::uint8_t r = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
  std::uint8_t g = static_cast<std::uint8_t>((g5 << 3) | (g5 >> 2));
  std::uint8_t b = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
  if (cgb_color_correction_) {
    int nr = (r * 82 + g * 18) / 100;
    int ng = (r * 12 + g * 82 + b * 6) / 100;
    int nb = (g * 12 + b * 88) / 100;
    r = static_cast<std::uint8_t>(nr < 0 ? 0 : (nr > 255 ? 255 : nr));
    g = static_cast<std::uint8_t>(ng < 0 ? 0 : (ng > 255 ? 255 : ng));
    b = static_cast<std::uint8_t>(nb < 0 ? 0 : (nb > 255 ? 255 : nb));
  }
  return (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
         (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

void Ppu::render_sprites(int line, Mmu* mmu, std::uint8_t lcdc,
                         const std::array<std::uint8_t, 160>& bg_index,
                         const std::array<bool, 160>& bg_priority) {
  if (!mmu || !backend_) {
    return;
  }
  if ((lcdc & 0x02) == 0) {
    return;
  }

  int sprite_height = (lcdc & 0x04) ? 16 : 8;

  struct Sprite {
    int index = 0;
    int x = 0;
    int y = 0;
    std::uint8_t tile = 0;
    std::uint8_t attr = 0;
  };

  std::array<Sprite, 40> sprites{};
  int count = 0;
  for (int i = 0; i < 40; ++i) {
    std::uint16_t base = static_cast<std::uint16_t>(0xFE00 + i * 4);
    int y = static_cast<int>(mmu->read_u8(base)) - 16;
    int x = static_cast<int>(mmu->read_u8(static_cast<std::uint16_t>(base + 1))) - 8;
    std::uint8_t tile = mmu->read_u8(static_cast<std::uint16_t>(base + 2));
    std::uint8_t attr = mmu->read_u8(static_cast<std::uint16_t>(base + 3));
    if (line < y || line >= y + sprite_height) {
      continue;
    }
    sprites[count++] = {i, x, y, tile, attr};
    if (count >= 10) {
      break;
    }
  }

  std::stable_sort(sprites.begin(), sprites.begin() + count, [](const Sprite& a, const Sprite& b) {
    if (a.x == b.x) {
      return a.index < b.index;
    }
    return a.x < b.x;
  });

  std::uint32_t* fb = framebuffer_mut();
  if (!fb) {
    return;
  }

  std::uint8_t obp0 = mmu->read_u8(0xFF48);
  std::uint8_t obp1 = mmu->read_u8(0xFF49);

  std::array<bool, 160> sprite_covered{};
  for (int s = 0; s < count; ++s) {
    const Sprite& sp = sprites[s];
    bool yflip = (sp.attr & 0x40) != 0;
    bool xflip = (sp.attr & 0x20) != 0;
    bool priority = (sp.attr & 0x80) != 0;
    int vram_bank = (system_ == System::GBC && (sp.attr & 0x08)) ? 1 : 0;

    int line_in_sprite = line - sp.y;
    if (yflip) {
      line_in_sprite = sprite_height - 1 - line_in_sprite;
    }

    std::uint8_t tile_index = sp.tile;
    if (sprite_height == 16) {
      tile_index &= 0xFE;
      if (line_in_sprite >= 8) {
        tile_index = static_cast<std::uint8_t>(tile_index + 1);
        line_in_sprite -= 8;
      }
    }

    std::uint16_t tile_addr = static_cast<std::uint16_t>(0x8000 + tile_index * 16);
    std::uint16_t row_addr = static_cast<std::uint16_t>(tile_addr + line_in_sprite * 2);
    std::uint8_t lo = mmu->read_vram(row_addr, vram_bank);
    std::uint8_t hi = mmu->read_vram(static_cast<std::uint16_t>(row_addr + 1), vram_bank);

    for (int px = 0; px < 8; ++px) {
      int screen_x = sp.x + px;
      if (screen_x < 0 || screen_x >= width()) {
        continue;
      }
      int bit = xflip ? px : (7 - px);
      std::uint8_t shade = static_cast<std::uint8_t>(((hi >> bit) & 0x01) << 1 |
                                                     ((lo >> bit) & 0x01));
      if (shade == 0) {
        continue;
      }

      if (sprite_covered[static_cast<std::size_t>(screen_x)]) {
        continue;
      }

      bool bg_pri = bg_priority[static_cast<std::size_t>(screen_x)];
      if (bg_index[static_cast<std::size_t>(screen_x)] != 0) {
        if (priority || (system_ == System::GBC && bg_pri)) {
          continue;
        }
      }

      std::uint32_t color = dmg_color(0);
      if (system_ == System::GBC) {
        int palette = sp.attr & 0x07;
        color = cgb_color(mmu, true, palette, shade);
      } else {
        std::uint8_t pal = (sp.attr & 0x10) ? obp1 : obp0;
        std::uint8_t palette_index = static_cast<std::uint8_t>((pal >> (shade * 2)) & 0x03);
        color = dmg_color(palette_index);
      }

      fb[static_cast<std::size_t>(line) * width() + screen_x] = color;
      sprite_covered[static_cast<std::size_t>(screen_x)] = true;
    }
  }
}

} // namespace gbemu::core
