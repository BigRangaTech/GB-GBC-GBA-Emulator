#include "gba_core.h"

#include <algorithm>

#include "timing.h"

namespace gbemu::core {

namespace {

constexpr std::uint32_t kRegDispstat = 0x04000004;
constexpr std::uint32_t kRegVcount = 0x04000006;
constexpr std::uint32_t kRegIe = 0x04000200;
constexpr std::uint32_t kRegIf = 0x04000202;
constexpr std::uint32_t kRegIme = 0x04000208;
constexpr int kGbaLinesPerFrame = 228;
constexpr int kGbaVblankStart = 160;
constexpr int kGbaVisibleCycles = 960;
constexpr int kGbaCyclesPerLine = 1232;

std::uint32_t bgr555_to_argb(std::uint16_t pixel) {
  std::uint8_t r = static_cast<std::uint8_t>(((pixel >> 0) & 0x1F) * 255 / 31);
  std::uint8_t g = static_cast<std::uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
  std::uint8_t b = static_cast<std::uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
  return (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
         (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

} // namespace

bool GbaCore::load(const std::vector<std::uint8_t>& rom,
                   const std::vector<std::uint8_t>& bios,
                   std::string* error) {
  if (!bus_.load(rom, bios, error)) {
    return false;
  }
  reset();
  return true;
}

void GbaCore::reset() {
  cpu_.reset();
  framebuffer_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
  frame_counter_ = 0;
  bios_handoff_done_ = false;
  bios_watchdog_cycles_ = 0;
  line_cycles_ = 0;
  vcount_ = 0;
  vblank_ = false;
  hblank_ = false;
  for (auto& timer : timers_) {
    timer = Timer{};
  }
  for (auto& chan : dma_) {
    chan.active = false;
  }
  bus_.write_io16_raw(kRegDispstat, 0);
  bus_.write_io16_raw(kRegVcount, 0);
  bus_.write_io16_raw(kRegIe, 0);
  bus_.write_io16_raw(kRegIf, 0);
  bus_.write_io16_raw(kRegIme, 0);
}

void GbaCore::step_frame() {
  int cycles = 0;
  while (cycles < kGbaCyclesPerFrame && !cpu_.faulted()) {
    sync_timers_from_io();
    int used = cpu_.step(&bus_);
    if (used <= 0) {
      break;
    }
    step_timers(used);
    step_dma();
    step_ppu(used);
    service_interrupts();
    cycles += used;
    if (!bios_handoff_done_) {
      bios_watchdog_cycles_ += used;
      if (cpu_.pc() >= 0x08000000u) {
        bios_handoff_done_ = true;
        cpu_.clear_unimplemented_count();
      } else if (cpu_.unimplemented_count() > 512 || bios_watchdog_cycles_ > 200000) {
        fast_boot_to_rom();
      }
    }
  }
  render_placeholder();
  ++frame_counter_;
}

void GbaCore::render_placeholder() {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  if (dispcnt & 0x0080) {
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0xFF000000u);
    return;
  }
  std::uint16_t mode = dispcnt & 0x0007;
  if (mode == 0 || mode == 3 || mode == 4) {
    return;
  }
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

void GbaCore::render_line(int y) {
  if (y < 0 || y >= height_) {
    return;
  }
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  if (dispcnt & 0x0080) {
    std::fill_n(framebuffer_.begin() + static_cast<std::size_t>(y) * width_, width_, 0xFF000000u);
    return;
  }
  std::uint16_t mode = dispcnt & 0x0007;
  switch (mode) {
    case 0:
      render_line_mode0(y);
      return;
    case 3:
      render_line_mode3(y);
      return;
    case 4:
      render_line_mode4(y);
      return;
    default:
      break;
  }
  for (int x = 0; x < width_; ++x) {
    std::uint8_t r = static_cast<std::uint8_t>((x + frame_counter_ * 2) % 256);
    std::uint8_t g = static_cast<std::uint8_t>((y + frame_counter_) % 256);
    std::uint8_t b = static_cast<std::uint8_t>((x ^ y ^ static_cast<int>(frame_counter_)) & 0xFF);
    framebuffer_[static_cast<std::size_t>(y) * width_ + x] =
        (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
        (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }
}

void GbaCore::render_line_mode3(int y) {
  std::uint32_t base = 0x06000000u + static_cast<std::uint32_t>(y) * width_ * 2u;
  for (int x = 0; x < width_; ++x) {
    std::uint16_t pixel = bus_.read16(base + static_cast<std::uint32_t>(x) * 2u);
    framebuffer_[static_cast<std::size_t>(y) * width_ + x] = bgr555_to_argb(pixel);
  }
}

void GbaCore::render_line_mode4(int y) {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  std::uint32_t page = (dispcnt & 0x0010) ? 0x0000A000u : 0x00000000u;
  std::uint32_t base = 0x06000000u + page + static_cast<std::uint32_t>(y) * width_;
  for (int x = 0; x < width_; ++x) {
    std::uint8_t index = bus_.read8(base + static_cast<std::uint32_t>(x));
    std::uint16_t pal = bus_.read16(0x05000000u + static_cast<std::uint32_t>(index) * 2u);
    framebuffer_[static_cast<std::size_t>(y) * width_ + x] = bgr555_to_argb(pal);
  }
}

void GbaCore::render_line_mode0(int y) {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  bool bg_enabled[4] = {
      (dispcnt & 0x0100) != 0,
      (dispcnt & 0x0200) != 0,
      (dispcnt & 0x0400) != 0,
      (dispcnt & 0x0800) != 0,
  };

  struct BgState {
    bool enabled = false;
    std::uint16_t cnt = 0;
    std::uint16_t hofs = 0;
    std::uint16_t vofs = 0;
    int priority = 3;
    int char_base = 0;
    int screen_base = 0;
    int size = 0;
    bool color_8bpp = false;
    int width = 256;
    int height = 256;
    int blocks_w = 1;
    int blocks_h = 1;
  };

  BgState bg[4];
  for (int i = 0; i < 4; ++i) {
    bg[i].enabled = bg_enabled[i];
    if (!bg[i].enabled) {
      continue;
    }
    std::uint32_t cnt_addr = 0x04000008u + static_cast<std::uint32_t>(i) * 2;
    std::uint32_t hofs_addr = 0x04000010u + static_cast<std::uint32_t>(i) * 4;
    std::uint32_t vofs_addr = hofs_addr + 2;
    bg[i].cnt = bus_.read_io16(cnt_addr);
    bg[i].hofs = static_cast<std::uint16_t>(bus_.read_io16(hofs_addr) & 0x01FF);
    bg[i].vofs = static_cast<std::uint16_t>(bus_.read_io16(vofs_addr) & 0x01FF);
    bg[i].priority = bg[i].cnt & 0x3;
    bg[i].char_base = (bg[i].cnt >> 2) & 0x3;
    bg[i].color_8bpp = (bg[i].cnt & 0x0080) != 0;
    bg[i].screen_base = (bg[i].cnt >> 8) & 0x1F;
    bg[i].size = (bg[i].cnt >> 14) & 0x3;
    bg[i].width = (bg[i].size == 1 || bg[i].size == 3) ? 512 : 256;
    bg[i].height = (bg[i].size == 2 || bg[i].size == 3) ? 512 : 256;
    bg[i].blocks_w = bg[i].width / 256;
    bg[i].blocks_h = bg[i].height / 256;
  }

  std::uint16_t backdrop = bus_.read16(0x05000000u);
  std::uint32_t backdrop_color = bgr555_to_argb(backdrop);
  for (int x = 0; x < width_; ++x) {
    std::uint32_t best_color = backdrop_color;
    int best_priority = 4;
    for (int i = 0; i < 4; ++i) {
      if (!bg[i].enabled) {
        continue;
      }
      int sx = (x + bg[i].hofs) % bg[i].width;
      int sy = (y + bg[i].vofs) % bg[i].height;
      int tile_x = sx / 8;
      int tile_y = sy / 8;
      int block_x = tile_x / 32;
      int block_y = tile_y / 32;
      int block_index = bg[i].screen_base + block_y * bg[i].blocks_w + block_x;
      std::uint32_t map_addr = 0x06000000u +
                               static_cast<std::uint32_t>(block_index) * 2048u +
                               static_cast<std::uint32_t>(tile_y % 32) * 64u +
                               static_cast<std::uint32_t>(tile_x % 32) * 2u;
      std::uint16_t entry = bus_.read16(map_addr);
      int tile_index = entry & 0x03FF;
      bool hflip = (entry & 0x0400) != 0;
      bool vflip = (entry & 0x0800) != 0;
      int palette_bank = (entry >> 12) & 0xF;

      int px = sx & 7;
      int py = sy & 7;
      if (hflip) {
        px = 7 - px;
      }
      if (vflip) {
        py = 7 - py;
      }

      std::uint32_t tile_base = 0x06000000u + static_cast<std::uint32_t>(bg[i].char_base) * 0x4000u;
      int color_index = 0;
      if (bg[i].color_8bpp) {
        std::uint32_t tile_addr = tile_base + static_cast<std::uint32_t>(tile_index) * 64u +
                                  static_cast<std::uint32_t>(py) * 8u +
                                  static_cast<std::uint32_t>(px);
        color_index = bus_.read8(tile_addr);
      } else {
        std::uint32_t tile_addr = tile_base + static_cast<std::uint32_t>(tile_index) * 32u +
                                  static_cast<std::uint32_t>(py) * 4u +
                                  static_cast<std::uint32_t>(px / 2);
        std::uint8_t byte = bus_.read8(tile_addr);
        int color = (px & 1) ? (byte >> 4) : (byte & 0x0F);
        if (color == 0) {
          continue;
        }
        color_index = (palette_bank << 4) | color;
      }

      if (color_index == 0) {
        continue;
      }
      std::uint16_t pal = bus_.read16(0x05000000u + static_cast<std::uint32_t>(color_index) * 2u);
      std::uint32_t color = bgr555_to_argb(pal);
      if (bg[i].priority < best_priority) {
        best_priority = bg[i].priority;
        best_color = color;
      }
    }
    framebuffer_[static_cast<std::size_t>(y) * width_ + x] = best_color;
  }
}

void GbaCore::sync_timers_from_io() {
  static constexpr std::uint32_t kBase = 0x04000100;
  for (int i = 0; i < 4; ++i) {
    std::uint32_t addr = kBase + static_cast<std::uint32_t>(i) * 4;
    std::uint16_t reload = bus_.read16(addr);
    std::uint16_t control = bus_.read16(addr + 2);
    if (reload != timers_[i].reload) {
      timers_[i].reload = reload;
    }
    if (control != timers_[i].control) {
      bool was_enabled = (timers_[i].control & 0x0080) != 0;
      bool now_enabled = (control & 0x0080) != 0;
      timers_[i].control = control;
      if (!was_enabled && now_enabled) {
        timers_[i].counter = timers_[i].reload;
        timers_[i].accum = 0;
        bus_.write16(addr, static_cast<std::uint16_t>(timers_[i].counter));
      }
    }
  }
}

void GbaCore::step_timers(int cycles) {
  static constexpr int prescale[4] = {1, 64, 256, 1024};
  bool overflowed[4] = {false, false, false, false};
  for (int i = 0; i < 4; ++i) {
    auto& timer = timers_[i];
    bool enabled = (timer.control & 0x0080) != 0;
    bool count_up = (timer.control & 0x0004) != 0;
    if (!enabled) {
      continue;
    }
    if (count_up) {
      if (i == 0) {
        continue;
      }
      if (overflowed[i - 1]) {
        ++timer.counter;
        if (timer.counter >= 0x10000) {
          timer.counter = timer.reload;
          overflowed[i] = true;
          if (timer.control & 0x0040) {
            request_interrupt(3 + i);
          }
        }
      }
    } else {
      int scale = prescale[timer.control & 0x3];
      timer.accum += static_cast<std::uint32_t>(cycles);
      while (timer.accum >= static_cast<std::uint32_t>(scale)) {
        timer.accum -= static_cast<std::uint32_t>(scale);
        ++timer.counter;
        if (timer.counter >= 0x10000) {
          timer.counter = timer.reload;
          overflowed[i] = true;
          if (timer.control & 0x0040) {
            request_interrupt(3 + i);
          }
          break;
        }
      }
    }
    std::uint32_t addr = 0x04000100u + static_cast<std::uint32_t>(i) * 4;
    bus_.write16(addr, static_cast<std::uint16_t>(timer.counter));
  }
}

void GbaCore::step_ppu(int cycles) {
  int remaining = cycles;
  while (remaining > 0) {
    int target = hblank_ ? kGbaCyclesPerLine : kGbaVisibleCycles;
    int to_boundary = target - line_cycles_;
    if (to_boundary <= 0) {
      to_boundary = 1;
    }
    int step = (remaining < to_boundary) ? remaining : to_boundary;
    line_cycles_ += step;
    remaining -= step;

    if (!hblank_ && line_cycles_ >= kGbaVisibleCycles) {
      hblank_ = true;
      if (vcount_ < kGbaVblankStart) {
        render_line(vcount_);
        trigger_dma(2);
      }
      update_dispstat();
    }

    if (line_cycles_ >= kGbaCyclesPerLine) {
      line_cycles_ -= kGbaCyclesPerLine;
      vcount_ = (vcount_ + 1) % kGbaLinesPerFrame;
      bus_.write_io16_raw(kRegVcount, static_cast<std::uint16_t>(vcount_));
      hblank_ = false;
      update_dispstat();
    }
  }
}

void GbaCore::update_dispstat() {
  std::uint16_t dispstat = bus_.read_io16(kRegDispstat);
  bool was_vblank = (dispstat & 0x0001) != 0;
  bool was_hblank = (dispstat & 0x0002) != 0;
  bool now_vblank = vcount_ >= kGbaVblankStart;
  if (now_vblank) {
    dispstat |= 0x0001;
  } else {
    dispstat = static_cast<std::uint16_t>(dispstat & ~0x0001);
  }
  if (hblank_) {
    dispstat |= 0x0002;
  } else {
    dispstat = static_cast<std::uint16_t>(dispstat & ~0x0002);
  }

  std::uint16_t vcount_target = static_cast<std::uint16_t>(dispstat >> 8);
  bool match = (vcount_ == vcount_target);
  if (match) {
    dispstat |= 0x0004;
  } else {
    dispstat = static_cast<std::uint16_t>(dispstat & ~0x0004);
  }
  bus_.write_io16_raw(kRegDispstat, dispstat);

  if (!was_vblank && now_vblank) {
    trigger_dma(1);
    if (dispstat & 0x0008) {
      request_interrupt(0);
    }
  }
  if (!was_hblank && hblank_) {
    if (dispstat & 0x0010) {
      request_interrupt(1);
    }
  }
  if (match && (dispstat & 0x0020)) {
    request_interrupt(2);
  }
  vblank_ = now_vblank;
}

void GbaCore::request_interrupt(int bit) {
  if (bit < 0 || bit > 15) {
    return;
  }
  bus_.set_if_bits(static_cast<std::uint16_t>(1u << bit));
}

void GbaCore::service_interrupts() {
  std::uint16_t ime = bus_.read_io16(kRegIme);
  if ((ime & 0x0001) == 0) {
    return;
  }
  std::uint16_t ie = bus_.read_io16(kRegIe);
  std::uint16_t iflag = bus_.read_io16(kRegIf);
  if ((ie & iflag) == 0) {
    return;
  }
  std::uint32_t pc = cpu_.pc();
  std::uint32_t lr = pc + (cpu_.thumb() ? 2u : 4u);
  cpu_.set_reg(14, lr);
  cpu_.set_thumb(false);
  cpu_.set_pc(0x00000018u);
  cpu_.set_mode(0x12);
  cpu_.set_irq_disable(true);
  bus_.write_io16_raw(kRegIme, 0);
}

void GbaCore::fast_boot_to_rom() {
  bios_handoff_done_ = true;
  cpu_.set_thumb(false);
  cpu_.set_mode(0x1F);
  cpu_.set_irq_disable(false);
  cpu_.set_pc(0x08000000u);
  cpu_.clear_unimplemented_count();
}

void GbaCore::step_dma() {
  run_dma(0);
}

void GbaCore::trigger_dma(int timing) {
  run_dma(timing);
}

void GbaCore::run_dma(int timing) {
  static constexpr std::uint32_t kDmaBase = 0x040000B0;
  for (int ch = 0; ch < 4; ++ch) {
    std::uint32_t base = kDmaBase + static_cast<std::uint32_t>(ch) * 12;
    std::uint32_t src = bus_.read32(base);
    std::uint32_t dst = bus_.read32(base + 4);
    std::uint16_t count = bus_.read16(base + 8);
    std::uint16_t ctrl = bus_.read16(base + 10);
    bool enable = (ctrl & 0x8000) != 0;
    std::uint32_t start_timing = (ctrl >> 12) & 0x3;
    bool repeat = (ctrl & 0x0200) != 0;
    if (!enable || static_cast<int>(start_timing) != timing) {
      if (timing == 0 && (!enable || start_timing != 0)) {
        dma_[ch].active = false;
      }
      continue;
    }
    if (dma_[ch].active) {
      continue;
    }
    dma_[ch].active = true;
    bool word = (ctrl & 0x0400) != 0;
    std::uint32_t length = count;
    if (length == 0) {
      length = (ch == 3) ? 0x10000u : 0x4000u;
    }
    std::uint32_t src_ctrl = (ctrl >> 7) & 0x3;
    std::uint32_t dst_ctrl = (ctrl >> 5) & 0x3;
    std::uint32_t step = word ? 4u : 2u;
    std::uint32_t cur_src = src;
    std::uint32_t cur_dst = dst;
    for (std::uint32_t i = 0; i < length; ++i) {
      if (word) {
        std::uint32_t value = bus_.read32(cur_src);
        bus_.write32(cur_dst, value);
      } else {
        std::uint16_t value = bus_.read16(cur_src);
        bus_.write16(cur_dst, value);
      }
      if (src_ctrl == 0) {
        cur_src += step;
      } else if (src_ctrl == 1) {
        cur_src -= step;
      }
      if (dst_ctrl == 0 || dst_ctrl == 3) {
        cur_dst += step;
      } else if (dst_ctrl == 1) {
        cur_dst -= step;
      }
    }

    std::uint32_t write_src = cur_src;
    std::uint32_t write_dst = cur_dst;
    if (dst_ctrl == 3 && repeat && timing != 0) {
      write_dst = dst;
    }
    bus_.write32(base, write_src);
    bus_.write32(base + 4, write_dst);
    if (ctrl & 0x4000) {
      request_interrupt(8 + ch);
    }
    if (timing == 0 || !repeat) {
      ctrl = static_cast<std::uint16_t>(ctrl & ~0x8000);
      bus_.write16(base + 10, ctrl);
    }
    dma_[ch].active = false;
  }
}

} // namespace gbemu::core
