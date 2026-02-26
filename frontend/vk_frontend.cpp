#include "vk_frontend.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "input.h"
#include "core.h"
#include "rom.h"
#include "vulkan_renderer.h"

namespace gbemu::frontend {
namespace {

struct Options {
  enum class FilterMode {
    None,
    Crt,
  };

  std::string rom_path;
  std::string config_path = "gbemu.conf";
  std::optional<gbemu::core::System> system_override;
  std::optional<double> fps_override;
  std::optional<std::string> video_driver;
  std::optional<int> filter_workers;
  int scale = 4;
  bool cpu_trace = false;
  bool gba_trace = false;
  int gba_trace_steps = 2000;
  bool gba_trace_io = true;
  bool gba_trace_after_rom = false;
  bool gba_hle_swi = false;
  bool gba_auto_handoff = true;
  bool gba_fastboot = false;
  bool gba_color_correct = false;
  bool gba_trace_assert = false;
  bool gba_bypass_assert = false;
  int gba_unimp_limit = 0;
  int gba_watch_video_io = 0;
  int gba_io_read_watch = 0;
  int gba_swi_limit = 0;
  int gba_watchdog_steps = 0;
  std::optional<std::uint32_t> gba_pc_watch_start;
  std::optional<std::uint32_t> gba_pc_watch_end;
  int gba_pc_watch_count = 0;
  std::optional<std::uint32_t> gba_mem_watch_start;
  std::optional<std::uint32_t> gba_mem_watch_end;
  int gba_mem_watch_count = 0;
  bool gba_mem_watch_reads = true;
  bool gba_mem_watch_writes = true;
  bool gba_auto_patch_hang = false;
  int gba_auto_patch_threshold = 50000;
  std::uint32_t gba_auto_patch_span = 0x40;
  std::optional<std::uint32_t> gba_auto_patch_start;
  std::optional<std::uint32_t> gba_auto_patch_end;
  FilterMode filter = FilterMode::None;
};

std::optional<Options::FilterMode> parse_filter(const std::string& value) {
  if (value == "none") {
    return Options::FilterMode::None;
  }
  if (value == "scanlines" || value == "lcd") {
    return Options::FilterMode::None;
  }
  if (value == "crt") {
    return Options::FilterMode::Crt;
  }
  return std::nullopt;
}

const char* filter_name(Options::FilterMode mode) {
  switch (mode) {
    case Options::FilterMode::None: return "NONE";
    case Options::FilterMode::Crt: return "CRT";
  }
  return "NONE";
}

std::optional<gbemu::core::System> parse_system(const std::string& value) {
  if (value == "gb" || value == "dmg") {
    return gbemu::core::System::GB;
  }
  if (value == "gbc" || value == "cgb") {
    return gbemu::core::System::GBC;
  }
  if (value == "gba") {
    return gbemu::core::System::GBA;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> parse_u32_arg(const char* text) {
  if (!text) {
    return std::nullopt;
  }
  try {
    std::size_t idx = 0;
    unsigned long value = std::stoul(text, &idx, 0);
    if (idx == 0 || text[idx] != '\0' || value > 0xFFFFFFFFul) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
  } catch (...) {
    return std::nullopt;
  }
}

bool init_sdl_with_wayland_fallback(const std::optional<std::string>& driver_override) {
  constexpr Uint32 kInitFlags = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO |
                                SDL_INIT_GAMECONTROLLER;

  auto try_init = [&](const char* driver) {
    SDL_setenv("SDL_VIDEODRIVER", driver, 1);
    if (SDL_Init(kInitFlags) == 0) {
      return true;
    }
    SDL_Quit();
    return false;
  };

  if (driver_override.has_value()) {
    if (*driver_override != "wayland" && *driver_override != "x11") {
      std::cout << "Unsupported video driver override: " << *driver_override << "\n";
      return false;
    }
    if (!try_init(driver_override->c_str())) {
      std::cout << "SDL init failed with SDL_VIDEODRIVER=" << *driver_override << ": "
                << SDL_GetError() << "\n";
      return false;
    }
    return true;
  }

  const char* existing = SDL_getenv("SDL_VIDEODRIVER");
  if (existing && *existing) {
    if (SDL_Init(kInitFlags) == 0) {
      return true;
    }
    std::cout << "SDL init failed with SDL_VIDEODRIVER=" << existing << ": "
              << SDL_GetError() << "\n";
    return false;
  }

  if (try_init("wayland")) {
    return true;
  }
  std::string wayland_error = SDL_GetError();
  if (try_init("x11")) {
    std::cout << "Wayland init failed: " << wayland_error << ". Using X11 fallback.\n";
    return true;
  }
  std::cout << "SDL init failed for Wayland (" << wayland_error << ") and X11 ("
            << SDL_GetError() << ").\n";
  return false;
}

gbemu::core::System detect_system(const std::vector<std::uint8_t>& data) {
  if (data.size() >= 0xC0 && data[0xB2] == 0x96) {
    return gbemu::core::System::GBA;
  }
  if (data.size() >= 0x150) {
    std::uint8_t cgb_flag = data[0x0143];
    if (cgb_flag == 0x80 || cgb_flag == 0xC0) {
      return gbemu::core::System::GBC;
    }
  }
  return gbemu::core::System::GB;
}

bool load_boot_rom(gbemu::core::System system, std::vector<std::uint8_t>* out, std::string* error) {
  std::vector<std::string> candidates;
  if (system == gbemu::core::System::GBA) {
    candidates = {
        "firmware/GBA/Game-Boy-Advance-Boot-ROM.bin",
        "firmware/gba_bios.bin",
        "firmware/gba_boot.bin",
        "firmware/bios.bin",
    };
  } else if (system == gbemu::core::System::GBC) {
    candidates = {
        "firmware/GBC/Game-Boy-Color-Boot-ROM.gbc",
        "firmware/cgb_boot.bin",
        "firmware/gbc_boot.bin",
        "firmware/boot.gbc",
        "firmware/boot.bin",
    };
  } else {
    candidates = {
        "firmware/GB/Game-Boy-Boot-ROM.gb",
        "firmware/dmg_boot.bin",
        "firmware/gb_boot.bin",
        "firmware/boot.gb",
        "firmware/boot.bin",
    };
  }

  for (const auto& path : candidates) {
    if (!std::filesystem::exists(path)) {
      continue;
    }
    std::string read_error;
    if (gbemu::common::read_file(path, out, &read_error)) {
      return true;
    }
  }

  if (error) {
    *error = "Boot ROM not found in firmware/";
  }
  return false;
}

void print_usage(const char* exe) {
  std::cout << "Usage: " << exe << " [options] <rom_file>\n";
  std::cout << "Options:\n";
  std::cout << "  --renderer <sdl|vulkan>\n";
  std::cout << "  --config <path>\n";
  std::cout << "  --system <gb|gbc|gba>\n";
  std::cout << "  --video-driver <wayland|x11>\n";
  std::cout << "  --fps <value>\n";
  std::cout << "  --scale <int>\n";
  std::cout << "  --filter <none|crt>\n";
  std::cout << "  --filter-workers <0..16>\n";
  std::cout << "  --gba-hle-swi\n";
  std::cout << "  --gba-fastboot\n";
  std::cout << "  --gba-no-auto-handoff\n";
  std::cout << "  --gba-color-correct\n";
  std::cout << "  --cpu-trace\n";
  std::cout << "  --gba-trace\n";
  std::cout << "  --gba-trace-after-rom\n";
  std::cout << "  --gba-trace-steps <n>\n";
  std::cout << "  --gba-trace-no-io\n";
  std::cout << "  --gba-trace-assert\n";
  std::cout << "  --gba-bypass-assert\n";
  std::cout << "  --gba-unimp <n>\n";
  std::cout << "  --gba-video-io <n>\n";
  std::cout << "  --gba-io-read <n>\n";
  std::cout << "  --gba-swi <n>\n";
  std::cout << "  --gba-watchdog <n>\n";
  std::cout << "  --gba-pc-watch <start> <end> <count>\n";
  std::cout << "  --gba-mem-watch <start> <end> <count>\n";
  std::cout << "  --gba-mem-watch-read\n";
  std::cout << "  --gba-mem-watch-write\n";
  std::cout << "  --gba-auto-patch-hang\n";
  std::cout << "  --gba-auto-patch-threshold <n>\n";
  std::cout << "  --gba-auto-patch-span <n>\n";
  std::cout << "  --gba-auto-patch-range <start> <end>\n";
  std::cout << "  -h, --help\n";
}

class FramePacer {
 public:
  explicit FramePacer(double fps)
      : freq_(SDL_GetPerformanceFrequency()),
        ticks_(fps > 0.0 ? static_cast<std::uint64_t>(freq_ / fps) : 0),
        next_(SDL_GetPerformanceCounter() + ticks_) {}

  void sleep() {
    if (ticks_ == 0) {
      return;
    }
    std::uint64_t now = SDL_GetPerformanceCounter();
    if (now < next_) {
      std::uint64_t remaining = next_ - now;
      std::uint32_t ms = static_cast<std::uint32_t>((remaining * 1000) / freq_);
      if (ms > 1) {
        SDL_Delay(ms - 1);
      }
      while (SDL_GetPerformanceCounter() < next_) {
      }
    }
    next_ += ticks_;
  }

 private:
  std::uint64_t freq_ = 0;
  std::uint64_t ticks_ = 0;
  std::uint64_t next_ = 0;
};

struct Glyph {
  std::array<std::uint8_t, 7> rows{};
};

const Glyph& glyph_for(char c) {
  static const Glyph empty{{0, 0, 0, 0, 0, 0, 0}};
  static const std::unordered_map<char, Glyph> font = {
      {'A', {{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}},
      {'B', {{0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}}},
      {'C', {{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}},
      {'D', {{0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}}},
      {'E', {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}},
      {'F', {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}},
      {'G', {{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}}},
      {'H', {{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}},
      {'I', {{0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}},
      {'J', {{0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}}},
      {'K', {{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}},
      {'L', {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}},
      {'M', {{0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}},
      {'N', {{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}},
      {'O', {{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}},
      {'P', {{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}},
      {'Q', {{0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}},
      {'R', {{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}},
      {'S', {{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}},
      {'T', {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}},
      {'U', {{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}},
      {'V', {{0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}}},
      {'W', {{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}},
      {'X', {{0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}}},
      {'Y', {{0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}},
      {'Z', {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}}},
      {'0', {{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}},
      {'1', {{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}},
      {'2', {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}},
      {'3', {{0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}}},
      {'4', {{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}},
      {'5', {{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}}},
      {'6', {{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}},
      {'7', {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}},
      {'8', {{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}},
      {'9', {{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}}},
      {'-', {{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}},
      {'+', {{0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}},
      {'/', {{0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}}},
      {':', {{0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}}},
      {'.', {{0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00}}},
      {'(', {{0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}},
      {')', {{0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}},
      {' ', {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}},
  };
  auto it = font.find(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  if (it == font.end()) {
    return empty;
  }
  return it->second;
}

std::uint32_t blend_argb(std::uint32_t dst, std::uint32_t src) {
  std::uint32_t sa = (src >> 24) & 0xFFu;
  if (sa == 0) {
    return dst;
  }
  if (sa == 255) {
    return src;
  }
  std::uint32_t da = 255u - sa;
  std::uint32_t sr = (src >> 16) & 0xFFu;
  std::uint32_t sg = (src >> 8) & 0xFFu;
  std::uint32_t sb = src & 0xFFu;
  std::uint32_t dr = (dst >> 16) & 0xFFu;
  std::uint32_t dg = (dst >> 8) & 0xFFu;
  std::uint32_t db = dst & 0xFFu;
  std::uint32_t r = (sr * sa + dr * da) / 255u;
  std::uint32_t g = (sg * sa + dg * da) / 255u;
  std::uint32_t b = (sb * sa + db * da) / 255u;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void fill_rect(std::uint32_t* pixels, int width, int height, int x, int y, int w, int h,
               std::uint32_t color) {
  if (!pixels || width <= 0 || height <= 0 || w <= 0 || h <= 0) {
    return;
  }
  int x0 = std::max(0, x);
  int y0 = std::max(0, y);
  int x1 = std::min(width, x + w);
  int y1 = std::min(height, y + h);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  for (int py = y0; py < y1; ++py) {
    std::size_t row = static_cast<std::size_t>(py) * width;
    for (int px = x0; px < x1; ++px) {
      std::size_t idx = row + static_cast<std::size_t>(px);
      pixels[idx] = blend_argb(pixels[idx], color);
    }
  }
}

void draw_text(std::uint32_t* pixels, int width, int height, int x, int y, int scale,
               const std::string& text, std::uint32_t color) {
  if (!pixels || scale <= 0) {
    return;
  }
  int cursor_x = x;
  int cursor_y = y;
  for (char ch : text) {
    if (ch == '\n') {
      cursor_x = x;
      cursor_y += 8 * scale;
      continue;
    }
    const Glyph& glyph = glyph_for(ch);
    for (int row = 0; row < 7; ++row) {
      std::uint8_t bits = glyph.rows[static_cast<std::size_t>(row)];
      for (int col = 0; col < 5; ++col) {
        if (bits & (1u << (4 - col))) {
          fill_rect(pixels, width, height, cursor_x + col * scale, cursor_y + row * scale, scale,
                    scale, color);
        }
      }
    }
    cursor_x += 6 * scale;
  }
}

void draw_menu_overlay(std::uint32_t* pixels, int width, int height, int menu_index,
                       int scroll_offset, const std::vector<std::string>& labels) {
  if (!pixels || width <= 0 || height <= 0) {
    return;
  }

  fill_rect(pixels, width, height, 0, 0, width, height, 0x70000000u);

  constexpr int kPanelBase = 44;
  constexpr int kItemPitch = 20;

  int max_panel_h = std::max(80, height - 20);
  int visible_items = std::max(1, (max_panel_h - kPanelBase) / kItemPitch);
  visible_items = std::min(visible_items, static_cast<int>(labels.size()));
  int max_start = std::max(0, static_cast<int>(labels.size()) - visible_items);
  int start = std::clamp(scroll_offset, 0, max_start);

  int panel_w = std::min(width - 24, 250);
  int panel_h = kPanelBase + visible_items * kItemPitch;
  int panel_x = (width - panel_w) / 2;
  int panel_y = (height - panel_h) / 2;

  fill_rect(pixels, width, height, panel_x, panel_y, panel_w, panel_h, 0xE0202020u);
  fill_rect(pixels, width, height, panel_x, panel_y, panel_w, 2, 0xFF5AE3FFu);
  draw_text(pixels, width, height, panel_x + 10, panel_y + 8, 2, "PAUSED", 0xFFFFFFFFu);

  const int item_h = 16;
  const int item_x = panel_x + 10;
  const int item_y = panel_y + 28;
  const int item_w = panel_w - 20;
  for (int row = 0; row < visible_items; ++row) {
    int i = start + row;
    int y = item_y + row * (item_h + 4);
    bool selected = (i == menu_index);
    if (selected) {
      fill_rect(pixels, width, height, item_x, y - 1, item_w, item_h, 0xA03E5865u);
    }
    std::uint32_t text_color = selected ? 0xFFBFEFFFu : 0xFFDCDCDC;
    draw_text(pixels, width, height, item_x + 4, y + 3, 1, labels[static_cast<std::size_t>(i)],
              text_color);
  }

  if (start > 0) {
    draw_text(pixels, width, height, panel_x + panel_w - 18, panel_y + 8, 1, "^", 0xFFB0B0B0u);
  }
  if (start + visible_items < static_cast<int>(labels.size())) {
    draw_text(pixels, width, height, panel_x + panel_w - 18, panel_y + panel_h - 20, 1, "V",
              0xFFB0B0B0u);
  }

  draw_text(pixels, width, height, panel_x + 10, panel_y + panel_h - 11, 1,
            "F10/ESC CLOSE  ENTER SELECT", 0xFFB0B0B0u);
}

class FilterWorkerPool {
 public:
  explicit FilterWorkerPool(unsigned workers) : workers_(workers) {
    if (workers_ == 0) {
      return;
    }
    ranges_.assign(workers_ + 1, std::pair<int, int>{0, 0});
    threads_.reserve(workers_);
    for (unsigned i = 0; i < workers_; ++i) {
      threads_.emplace_back([this, i]() { worker_loop(i + 1); });
    }
  }

  ~FilterWorkerPool() {
    {
      std::scoped_lock lock(mutex_);
      stop_ = true;
      ++epoch_;
    }
    cv_.notify_all();
    for (auto& t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  unsigned worker_count() const { return workers_; }

  template <typename Fn>
  void run(int rows, Fn&& fn) {
    if (rows <= 0) {
      return;
    }
    if (workers_ == 0 || rows < 8) {
      fn(0, rows);
      return;
    }

    int slots = static_cast<int>(workers_) + 1;
    int chunk = (rows + slots - 1) / slots;
    if (chunk <= 0) {
      chunk = 1;
    }
    {
      std::scoped_lock lock(mutex_);
      for (int i = 0; i < slots; ++i) {
        int start = i * chunk;
        int end = std::min(rows, start + chunk);
        ranges_[static_cast<std::size_t>(i)] = {start, end};
      }
      job_ = std::forward<Fn>(fn);
      pending_ = workers_;
      ++epoch_;
    }
    cv_.notify_all();

    const auto main_range = ranges_[0];
    if (main_range.first < main_range.second) {
      fn(main_range.first, main_range.second);
    }

    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this]() { return pending_ == 0; });
  }

 private:
  void worker_loop(unsigned index) {
    std::size_t seen_epoch = 0;
    while (true) {
      std::function<void(int, int)> job;
      std::pair<int, int> range{0, 0};
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this, &seen_epoch]() { return stop_ || epoch_ != seen_epoch; });
        if (stop_) {
          return;
        }
        seen_epoch = epoch_;
        job = job_;
        range = ranges_[index];
      }

      if (job && range.first < range.second) {
        job(range.first, range.second);
      }

      {
        std::scoped_lock lock(mutex_);
        if (--pending_ == 0) {
          done_cv_.notify_one();
        }
      }
    }
  }

  unsigned workers_ = 0;
  std::vector<std::thread> threads_;
  std::vector<std::pair<int, int>> ranges_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable done_cv_;
  std::function<void(int, int)> job_;
  std::size_t epoch_ = 0;
  int pending_ = 0;
  bool stop_ = false;
};

unsigned default_filter_workers() {
  unsigned hw_threads = std::thread::hardware_concurrency();
  if (hw_threads == 0) {
    hw_threads = 8;
  }
  unsigned workers = std::max(4u, hw_threads / 2);
  return std::clamp(workers, 1u, 8u);
}

unsigned sanitize_filter_workers(int requested) {
  return static_cast<unsigned>(std::clamp(requested, 0, 16));
}

void apply_passthrough_filter(const std::uint32_t* src, int src_stride_words, std::uint32_t* dst,
                              int width, int height, FilterWorkerPool* pool) {
  if (!src || !dst || width <= 0 || height <= 0) {
    return;
  }
  auto run_rows = [&](auto&& fn) {
    if (pool && pool->worker_count() > 0) {
      pool->run(height, fn);
    } else {
      fn(0, height);
    }
  };
  run_rows([&](int y_start, int y_end) {
    for (int y = y_start; y < y_end; ++y) {
      int src_row = y * src_stride_words;
      int dst_row = y * width;
      for (int x = 0; x < width; ++x) {
        dst[dst_row + x] = src[src_row + x];
      }
    }
  });
}

void apply_crt_filter(const std::uint32_t* src, int src_stride_words, std::uint32_t* dst,
                      std::uint32_t* prev, int width, int height, bool reset,
                      FilterWorkerPool* pool) {
  if (!src || !dst || !prev || width <= 0 || height <= 0) {
    return;
  }
  auto run_rows = [&](auto&& fn) {
    if (pool && pool->worker_count() > 0) {
      pool->run(height, fn);
    } else {
      fn(0, height);
    }
  };
  if (reset) {
    run_rows([&](int y_start, int y_end) {
      for (int y = y_start; y < y_end; ++y) {
        int src_row = y * src_stride_words;
        int dst_row = y * width;
        for (int x = 0; x < width; ++x) {
          std::uint32_t pixel = src[src_row + x];
          dst[dst_row + x] = pixel;
          prev[dst_row + x] = pixel;
        }
      }
    });
    return;
  }

  float cx = (width - 1) * 0.5f;
  float cy = (height - 1) * 0.5f;
  float inv_cx = (cx > 0.0f) ? (1.0f / cx) : 0.0f;
  float inv_cy = (cy > 0.0f) ? (1.0f / cy) : 0.0f;
  float blend = 0.25f;
  float scanline = 0.86f;

  run_rows([&](int y_start, int y_end) {
    for (int y = y_start; y < y_end; ++y) {
      float fy = (static_cast<float>(y) - cy) * inv_cy;
      float fy2 = fy * fy;
      float scan = (y & 1) ? scanline : 1.0f;
      int src_row = y * src_stride_words;
      int dst_row = y * width;
      for (int x = 0; x < width; ++x) {
        std::uint32_t src_pixel = src[src_row + x];
        std::uint32_t prev_pixel = prev[dst_row + x];
        int sr = (src_pixel >> 16) & 0xFF;
        int sg = (src_pixel >> 8) & 0xFF;
        int sb = src_pixel & 0xFF;
        int pr = (prev_pixel >> 16) & 0xFF;
        int pg = (prev_pixel >> 8) & 0xFF;
        int pb = prev_pixel & 0xFF;
        float rf = sr * (1.0f - blend) + pr * blend;
        float gf = sg * (1.0f - blend) + pg * blend;
        float bf = sb * (1.0f - blend) + pb * blend;
        float fx = (static_cast<float>(x) - cx) * inv_cx;
        float vignette = 1.0f - 0.18f * (fx * fx + fy2);
        vignette = std::clamp(vignette, 0.72f, 1.0f);
        float mul = scan * vignette;
        int out_r = static_cast<int>(rf * mul);
        int out_g = static_cast<int>(gf * mul);
        int out_b = static_cast<int>(bf * mul);
        out_r = std::clamp(out_r, 0, 255);
        out_g = std::clamp(out_g, 0, 255);
        out_b = std::clamp(out_b, 0, 255);
        dst[dst_row + x] = 0xFF000000u | (static_cast<std::uint32_t>(out_r) << 16) |
                           (static_cast<std::uint32_t>(out_g) << 8) |
                           static_cast<std::uint32_t>(out_b);
        prev[dst_row + x] = src_pixel;
      }
    }
  });
}

}  // namespace

int run_vulkan_frontend(int argc, char** argv) {
  Options options;
  bool show_help = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      show_help = true;
    } else if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --config\n";
        return 1;
      }
      options.config_path = argv[++i];
    } else if (arg == "--renderer") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --renderer\n";
        return 1;
      }
      std::string value = argv[++i];
      if (value != "vulkan" && value != "sdl") {
        std::cout << "Invalid renderer value: " << value << "\n";
        return 1;
      }
    } else if (arg == "--system") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --system\n";
        return 1;
      }
      auto parsed = parse_system(argv[++i]);
      if (!parsed.has_value()) {
        std::cout << "Invalid system\n";
        return 1;
      }
      options.system_override = parsed;
    } else if (arg == "--video-driver") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --video-driver\n";
        return 1;
      }
      options.video_driver = std::string(argv[++i]);
    } else if (arg == "--fps") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --fps\n";
        return 1;
      }
      options.fps_override = std::stod(argv[++i]);
    } else if (arg == "--scale") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --scale\n";
        return 1;
      }
      options.scale = std::max(1, std::stoi(argv[++i]));
    } else if (arg == "--filter") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --filter\n";
        return 1;
      }
      auto parsed = parse_filter(argv[++i]);
      if (!parsed.has_value()) {
        std::cout << "Invalid filter value\n";
        return 1;
      }
      options.filter = *parsed;
    } else if (arg == "--filter-workers") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --filter-workers\n";
        return 1;
      }
      int value = std::stoi(argv[++i]);
      if (value < 0 || value > 16) {
        std::cout << "filter-workers must be in range 0..16\n";
        return 1;
      }
      options.filter_workers = value;
    } else if (arg == "--cpu-trace") {
      options.cpu_trace = true;
    } else if (arg == "--gba-trace") {
      options.gba_trace = true;
    } else if (arg == "--gba-trace-after-rom") {
      options.gba_trace_after_rom = true;
    } else if (arg == "--gba-trace-steps") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-trace-steps\n";
        return 1;
      }
      options.gba_trace_steps = std::max(1, std::stoi(argv[++i]));
    } else if (arg == "--gba-trace-no-io") {
      options.gba_trace_io = false;
    } else if (arg == "--gba-hle-swi") {
      options.gba_hle_swi = true;
    } else if (arg == "--gba-fastboot") {
      options.gba_fastboot = true;
    } else if (arg == "--gba-no-auto-handoff") {
      options.gba_auto_handoff = false;
    } else if (arg == "--gba-color-correct") {
      options.gba_color_correct = true;
    } else if (arg == "--gba-trace-assert") {
      options.gba_trace_assert = true;
    } else if (arg == "--gba-bypass-assert") {
      options.gba_bypass_assert = true;
    } else if (arg == "--gba-unimp") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-unimp\n";
        return 1;
      }
      options.gba_unimp_limit = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-video-io") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-video-io\n";
        return 1;
      }
      options.gba_watch_video_io = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-io-read") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-io-read\n";
        return 1;
      }
      options.gba_io_read_watch = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-swi") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-swi\n";
        return 1;
      }
      options.gba_swi_limit = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-watchdog") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-watchdog\n";
        return 1;
      }
      options.gba_watchdog_steps = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-pc-watch") {
      if (i + 3 >= argc) {
        std::cout << "Usage: --gba-pc-watch <start> <end> <count>\n";
        return 1;
      }
      auto start = parse_u32_arg(argv[++i]);
      auto end = parse_u32_arg(argv[++i]);
      if (!start.has_value() || !end.has_value()) {
        std::cout << "Invalid --gba-pc-watch range\n";
        return 1;
      }
      options.gba_pc_watch_start = *start;
      options.gba_pc_watch_end = *end;
      options.gba_pc_watch_count = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-mem-watch") {
      if (i + 3 >= argc) {
        std::cout << "Usage: --gba-mem-watch <start> <end> <count>\n";
        return 1;
      }
      auto start = parse_u32_arg(argv[++i]);
      auto end = parse_u32_arg(argv[++i]);
      if (!start.has_value() || !end.has_value()) {
        std::cout << "Invalid --gba-mem-watch range\n";
        return 1;
      }
      options.gba_mem_watch_start = *start;
      options.gba_mem_watch_end = *end;
      options.gba_mem_watch_count = std::max(0, std::stoi(argv[++i]));
      options.gba_mem_watch_reads = true;
      options.gba_mem_watch_writes = true;
    } else if (arg == "--gba-mem-watch-read") {
      options.gba_mem_watch_reads = true;
      options.gba_mem_watch_writes = false;
    } else if (arg == "--gba-mem-watch-write") {
      options.gba_mem_watch_reads = false;
      options.gba_mem_watch_writes = true;
    } else if (arg == "--gba-auto-patch-hang") {
      options.gba_auto_patch_hang = true;
    } else if (arg == "--gba-auto-patch-threshold") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-auto-patch-threshold\n";
        return 1;
      }
      options.gba_auto_patch_threshold = std::max(1, std::stoi(argv[++i]));
    } else if (arg == "--gba-auto-patch-span") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-auto-patch-span\n";
        return 1;
      }
      auto span = parse_u32_arg(argv[++i]);
      if (!span.has_value()) {
        std::cout << "Invalid --gba-auto-patch-span\n";
        return 1;
      }
      options.gba_auto_patch_span = *span;
    } else if (arg == "--gba-auto-patch-range") {
      if (i + 2 >= argc) {
        std::cout << "Usage: --gba-auto-patch-range <start> <end>\n";
        return 1;
      }
      auto start = parse_u32_arg(argv[++i]);
      auto end = parse_u32_arg(argv[++i]);
      if (!start.has_value() || !end.has_value()) {
        std::cout << "Invalid --gba-auto-patch-range\n";
        return 1;
      }
      options.gba_auto_patch_start = *start;
      options.gba_auto_patch_end = *end;
    } else if (!arg.empty() && arg[0] == '-') {
      std::cout << "Unknown option for Vulkan frontend: " << arg << "\n";
      return 1;
    } else if (options.rom_path.empty()) {
      options.rom_path = arg;
    } else {
      std::cout << "Unexpected argument: " << arg << "\n";
      return 1;
    }
  }

  if (show_help || options.rom_path.empty()) {
    print_usage(argv[0]);
    return show_help ? 0 : 1;
  }

  gbemu::core::EmulatorCore core;
  std::cout << "GBEmu Vulkan frontend v" << core.version() << "\n";

  std::vector<std::uint8_t> rom;
  std::string error;
  if (!gbemu::common::read_file(options.rom_path, &rom, &error)) {
    std::cout << "Failed to read ROM: " << error << "\n";
    return 1;
  }

  gbemu::core::System system = options.system_override.value_or(detect_system(rom));
  core.set_system(system);

  std::vector<std::uint8_t> boot;
  if (!load_boot_rom(system, &boot, &error)) {
    std::cout << error << "\n";
    return 1;
  }

  if (!core.load_rom(rom, boot, &error)) {
    std::cout << "Failed to load ROM: " << error << "\n";
    return 1;
  }

  core.set_gba_hle_swi(options.gba_hle_swi);
  core.set_gba_fastboot(options.gba_fastboot);
  core.set_gba_auto_handoff(options.gba_auto_handoff);
  core.set_gba_color_correction(options.gba_color_correct);
  core.set_gba_trace_assert(options.gba_trace_assert);
  core.set_gba_bypass_assert(options.gba_bypass_assert);
  core.set_cpu_trace_enabled(options.cpu_trace);
  if (options.gba_trace_after_rom) {
    core.set_gba_trace_after_rom(options.gba_trace_steps, options.gba_trace_io);
  } else if (options.gba_trace) {
    core.set_gba_trace(options.gba_trace_steps, options.gba_trace_io);
  }
  if (options.gba_unimp_limit > 0) {
    core.set_gba_log_unimplemented(options.gba_unimp_limit);
  }
  if (options.gba_watch_video_io > 0) {
    core.set_gba_watch_video_io(options.gba_watch_video_io);
  }
  if (options.gba_io_read_watch > 0) {
    core.set_gba_watch_io_reads(options.gba_io_read_watch);
  }
  if (options.gba_swi_limit > 0) {
    core.set_gba_log_swi(options.gba_swi_limit);
  }
  if (options.gba_watchdog_steps > 0) {
    core.set_gba_watchdog(options.gba_watchdog_steps);
  }
  if (options.gba_pc_watch_count > 0 &&
      options.gba_pc_watch_start.has_value() &&
      options.gba_pc_watch_end.has_value()) {
    core.set_gba_pc_watch(*options.gba_pc_watch_start,
                          *options.gba_pc_watch_end,
                          options.gba_pc_watch_count);
  }
  if (options.gba_mem_watch_count > 0 &&
      options.gba_mem_watch_start.has_value() &&
      options.gba_mem_watch_end.has_value()) {
    core.set_gba_mem_watch(*options.gba_mem_watch_start,
                           *options.gba_mem_watch_end,
                           options.gba_mem_watch_count,
                           options.gba_mem_watch_reads,
                           options.gba_mem_watch_writes);
  }
  if (options.gba_auto_patch_hang) {
    core.set_gba_auto_patch_hang(true);
    core.set_gba_auto_patch_threshold(options.gba_auto_patch_threshold);
    core.set_gba_auto_patch_span(options.gba_auto_patch_span);
    if (options.gba_auto_patch_start.has_value() &&
        options.gba_auto_patch_end.has_value()) {
      core.set_gba_auto_patch_range(*options.gba_auto_patch_start,
                                    *options.gba_auto_patch_end);
    }
  }

  bool debug_window_overlay = false;
  bool color_correction = (system == gbemu::core::System::GBA) ? options.gba_color_correct : false;
  if (system == gbemu::core::System::GBC) {
    core.set_cgb_color_correction(color_correction);
  }
  core.set_debug_window_overlay(debug_window_overlay);

  std::filesystem::path save_dir = "saves";
  std::filesystem::create_directories(save_dir);
  std::filesystem::path rom_name = std::filesystem::path(options.rom_path).filename();
  std::filesystem::path state_path = save_dir / rom_name.stem();
  state_path += ".state";

  gbemu::common::InputConfig input_config;
  input_config.set_default();
  {
    gbemu::common::Config config_for_inputs;
    std::string cfg_err;
    if (!options.config_path.empty() &&
        config_for_inputs.load_file(options.config_path, &cfg_err)) {
      input_config.load_from_config(config_for_inputs.values());
    }
  }
  {
    std::filesystem::path input_map_path = save_dir / "input_map.conf";
    if (std::filesystem::exists(input_map_path)) {
      gbemu::common::Config config_override;
      std::string cfg_err;
      if (config_override.load_file(input_map_path.string(), &cfg_err)) {
        input_config.load_from_config(config_override.values());
      }
    }
  }
  {
    std::filesystem::path rom_map = save_dir / "input_map" / (rom_name.stem().string() + ".conf");
    if (std::filesystem::exists(rom_map)) {
      gbemu::common::Config config_override;
      std::string cfg_err;
      if (config_override.load_file(rom_map.string(), &cfg_err)) {
        input_config.load_from_config(config_override.values());
      }
    }
  }

  if (!init_sdl_with_wayland_fallback(options.video_driver)) {
    return 1;
  }

  const char* driver = SDL_GetCurrentVideoDriver();
  std::cout << "SDL video driver: " << (driver ? driver : "unknown") << "\n";

  int fb_width = core.framebuffer_width();
  int fb_height = core.framebuffer_height();

  SDL_Window* window = SDL_CreateWindow("GBEmu Vulkan", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, fb_width * options.scale,
                                        fb_height * options.scale,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                            SDL_WINDOW_SHOWN);
  if (!window) {
    std::cout << "Failed to create Vulkan window: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 1;
  }

  VulkanRenderer renderer;
  if (!renderer.init(window, fb_width, fb_height, &error)) {
    std::cout << "Failed to initialize Vulkan renderer: " << error << "\n";
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_AudioSpec want{};
  SDL_AudioSpec have{};
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (audio_device != 0) {
    SDL_PauseAudioDevice(audio_device, 0);
    std::cout << "Audio device opened at " << have.freq << " Hz\n";
  } else {
    std::cout << "Audio unavailable: " << SDL_GetError() << "\n";
  }
  std::atomic<bool> audio_enabled{audio_device != 0};
  std::unordered_map<int, SDL_GameController*> controllers;

  int joy_count = SDL_NumJoysticks();
  for (int i = 0; i < joy_count; ++i) {
    if (!SDL_IsGameController(i)) {
      continue;
    }
    SDL_GameController* controller = SDL_GameControllerOpen(i);
    if (!controller) {
      continue;
    }
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    if (!joystick) {
      SDL_GameControllerClose(controller);
      continue;
    }
    int instance = SDL_JoystickInstanceID(joystick);
    controllers[instance] = controller;
  }
  if (!controllers.empty()) {
    std::cout << "Controllers connected: " << controllers.size() << "\n";
  }

  std::atomic<bool> running{true};
  std::atomic<bool> paused{false};
  std::atomic<std::uint8_t> joypad_state{0xFF};
  std::atomic<bool> joypad_irq{false};
  std::atomic<int> active_filter_mode{static_cast<int>(options.filter)};
  std::uint8_t key_state = 0xFF;
  std::uint8_t pad_state = 0xFF;

  std::mutex frame_mutex;
  std::vector<std::uint32_t> front_buffer(static_cast<std::size_t>(fb_width) * fb_height);
  std::uint64_t front_serial = 0;
  std::atomic<std::uint64_t> shared_serial{0};

  std::mutex core_mutex;
  double target_fps = options.fps_override.value_or(core.target_fps());
  FramePacer emu_pacer(target_fps);
  unsigned filter_workers =
      options.filter_workers.has_value()
          ? sanitize_filter_workers(*options.filter_workers)
          : (options.filter == Options::FilterMode::Crt ? default_filter_workers() : 0);
  FilterWorkerPool filter_pool(filter_workers);
  std::vector<std::uint32_t> filtered_frame;
  std::vector<std::uint32_t> prev_filtered_frame;
  bool filter_reset = true;
  std::cout << "Vulkan filter: "
            << (options.filter == Options::FilterMode::Crt ? "crt" : "none")
            << ", workers=" << filter_workers << "\n";

  std::thread emu_thread([&]() {
    double audio_accum = 0.0;
    std::uint8_t applied_joypad = 0xFF;
    Options::FilterMode last_filter_mode =
        static_cast<Options::FilterMode>(active_filter_mode.load(std::memory_order_relaxed));
    while (running.load(std::memory_order_relaxed)) {
      if (paused.load(std::memory_order_relaxed)) {
        SDL_Delay(1);
        continue;
      }

      std::vector<std::uint32_t> snapshot;
      int width = 0;
      int height = 0;
      bool faulted = false;
      {
        std::scoped_lock lock(core_mutex);
        std::uint8_t desired = joypad_state.load(std::memory_order_relaxed);
        if (desired != applied_joypad) {
          core.set_joypad_state(desired);
          applied_joypad = desired;
        }
        if (joypad_irq.exchange(false, std::memory_order_relaxed)) {
          core.request_interrupt(4);
        }

        core.step_frame();
        faulted = core.cpu_faulted();

        width = core.framebuffer_width();
        height = core.framebuffer_height();
        const std::uint32_t* src = core.framebuffer();
        if (src && width > 0 && height > 0) {
          snapshot.assign(src, src + static_cast<std::size_t>(width) * height);
        }

        if (audio_device != 0 && audio_enabled.load(std::memory_order_relaxed)) {
          double fps = target_fps > 0.0 ? target_fps : 60.0;
          audio_accum += static_cast<double>(have.freq) / fps;
          int samples = static_cast<int>(audio_accum);
          if (samples > 0) {
            audio_accum -= samples;
            if (SDL_GetQueuedAudioSize(audio_device) < static_cast<Uint32>(have.freq * 8)) {
              std::vector<std::int16_t> audio;
              core.generate_audio(have.freq, samples, &audio);
              if (!audio.empty()) {
                SDL_QueueAudio(audio_device, audio.data(),
                               static_cast<Uint32>(audio.size() * sizeof(std::int16_t)));
              }
            }
          }
        }
      }

      if (faulted) {
        std::cout << "CPU fault: " << core.cpu_fault_reason() << "\n";
        running.store(false, std::memory_order_relaxed);
        break;
      }

      if (!snapshot.empty()) {
        const std::uint32_t* output_data = snapshot.data();
        std::size_t pixel_count = static_cast<std::size_t>(width) * height;
        Options::FilterMode filter_mode =
            static_cast<Options::FilterMode>(active_filter_mode.load(std::memory_order_relaxed));
        if (filter_mode != last_filter_mode) {
          filter_reset = true;
          last_filter_mode = filter_mode;
        }

        if (filter_mode == Options::FilterMode::Crt) {
          if (filtered_frame.size() != pixel_count) {
            filtered_frame.resize(pixel_count);
            prev_filtered_frame.resize(pixel_count);
            filter_reset = true;
          }
          apply_crt_filter(snapshot.data(), width, filtered_frame.data(), prev_filtered_frame.data(),
                           width, height, filter_reset, &filter_pool);
          filter_reset = false;
          output_data = filtered_frame.data();
        } else if (filter_workers > 0) {
          if (filtered_frame.size() != pixel_count) {
            filtered_frame.resize(pixel_count);
          }
          apply_passthrough_filter(snapshot.data(), width, filtered_frame.data(), width, height,
                                   &filter_pool);
          output_data = filtered_frame.data();
        }

        std::scoped_lock lock(frame_mutex);
        front_buffer.assign(output_data, output_data + pixel_count);
        shared_serial.fetch_add(1, std::memory_order_relaxed);
      }

      emu_pacer.sleep();
    }
  });

  auto action_to_bit = [](gbemu::common::InputAction action) -> int {
    switch (action) {
      case gbemu::common::InputAction::Right: return 0;
      case gbemu::common::InputAction::Left: return 1;
      case gbemu::common::InputAction::Up: return 2;
      case gbemu::common::InputAction::Down: return 3;
      case gbemu::common::InputAction::A: return 4;
      case gbemu::common::InputAction::B: return 5;
      case gbemu::common::InputAction::Select: return 6;
      case gbemu::common::InputAction::Start: return 7;
      default: return -1;
    }
  };

  auto set_action_bit = [](std::uint8_t* state, int bit, bool down) -> bool {
    if (!state || bit < 0) {
      return false;
    }
    std::uint8_t before = *state;
    if (down) {
      *state = static_cast<std::uint8_t>(*state & ~(1u << bit));
    } else {
      *state = static_cast<std::uint8_t>(*state | (1u << bit));
    }
    return *state != before;
  };

  auto publish_joypad_state = [&]() {
    joypad_state.store(static_cast<std::uint8_t>(key_state & pad_state), std::memory_order_relaxed);
  };

  auto set_key = [&](SDL_Keycode key, bool down) {
    bool changed = false;
    const gbemu::common::InputAction actions[] = {
        gbemu::common::InputAction::A,      gbemu::common::InputAction::B,
        gbemu::common::InputAction::Select, gbemu::common::InputAction::Start,
        gbemu::common::InputAction::Right,  gbemu::common::InputAction::Left,
        gbemu::common::InputAction::Up,     gbemu::common::InputAction::Down,
    };
    for (auto action : actions) {
      if (!input_config.resolve(action, static_cast<int>(key))) {
        continue;
      }
      changed |= set_action_bit(&key_state, action_to_bit(action), down);
    }
    if (!changed) {
      return;
    }
    publish_joypad_state();
    if (down) {
      joypad_irq.store(true, std::memory_order_relaxed);
    }
  };

  auto set_controller_button = [&](int button, bool down) {
    auto action = input_config.action_for_controller_button(button);
    if (!action.has_value()) {
      return;
    }
    bool changed = set_action_bit(&pad_state, action_to_bit(*action), down);
    if (!changed) {
      return;
    }
    publish_joypad_state();
    if (down) {
      joypad_irq.store(true, std::memory_order_relaxed);
    }
  };

  auto set_controller_axis = [&](int axis, int value) {
    int deadzone = input_config.axis_deadzone();
    bool pos = value > deadzone;
    bool neg = value < -deadzone;
    bool changed = false;
    if (auto action = input_config.action_for_controller_axis_pos(axis)) {
      changed |= set_action_bit(&pad_state, action_to_bit(*action), pos);
    }
    if (auto action = input_config.action_for_controller_axis_neg(axis)) {
      changed |= set_action_bit(&pad_state, action_to_bit(*action), neg);
    }
    if (changed) {
      publish_joypad_state();
      if (pos || neg) {
        joypad_irq.store(true, std::memory_order_relaxed);
      }
    }
  };

  std::cout << "Window created (Vulkan). Press ESC or close to exit.\n";
  std::uint64_t fps_last_tick = SDL_GetPerformanceCounter();
  const std::uint64_t perf_freq = SDL_GetPerformanceFrequency();
  int fps_frames = 0;
  enum MenuItem {
    kMenuResume = 0,
    kMenuSaveState = 1,
    kMenuLoadState = 2,
    kMenuFilter = 3,
    kMenuWindowOverlay = 4,
    kMenuColorCorrection = 5,
    kMenuAudio = 6,
    kMenuQuit = 7,
  };
  bool menu_open = false;
  int menu_index = kMenuResume;
  int menu_scroll = 0;
  bool ui_dirty = true;
  std::vector<std::uint32_t> last_frame;
  std::vector<std::uint32_t> overlay_frame;
  char title_buf[96];

  auto set_menu_open = [&](bool open) {
    if (menu_open == open) {
      return;
    }
    menu_open = open;
    paused.store(open, std::memory_order_relaxed);
    if (open) {
      key_state = 0xFF;
      pad_state = 0xFF;
      joypad_state.store(0xFF, std::memory_order_relaxed);
      joypad_irq.store(false, std::memory_order_relaxed);
      menu_scroll = 0;
    }
    ui_dirty = true;
  };

  auto cycle_filter = [&](int dir) {
    int mode = active_filter_mode.load(std::memory_order_relaxed);
    if (dir > 0) {
      mode = (mode + 1) % 2;
    } else {
      mode = (mode + 1) % 2;
    }
    active_filter_mode.store(mode, std::memory_order_relaxed);
    std::cout << "Vulkan filter: " << filter_name(static_cast<Options::FilterMode>(mode)) << "\n";
    ui_dirty = true;
  };

  auto save_state = [&]() {
    std::vector<std::uint8_t> state;
    std::string save_error;
    {
      std::scoped_lock lock(core_mutex);
      if (!core.save_state(&state)) {
        std::cout << "Failed to build save state\n";
        return;
      }
    }
    if (gbemu::common::write_file(state_path.string(), state, &save_error)) {
      std::cout << "Saved state to " << state_path.string() << "\n";
    } else {
      std::cout << "Failed to save state: " << save_error << "\n";
    }
  };

  auto load_state = [&]() {
    std::vector<std::uint8_t> state;
    std::string load_error;
    if (!gbemu::common::read_file(state_path.string(), &state, &load_error)) {
      std::cout << "Failed to read state: " << load_error << "\n";
      return;
    }
    std::string err;
    {
      std::scoped_lock lock(core_mutex);
      if (!core.load_state(state, &err)) {
        std::cout << "Failed to load state: " << err << "\n";
        return;
      }
    }
    std::cout << "Loaded state from " << state_path.string() << "\n";
    ui_dirty = true;
  };

  auto toggle_window_overlay = [&]() {
    debug_window_overlay = !debug_window_overlay;
    {
      std::scoped_lock lock(core_mutex);
      core.set_debug_window_overlay(debug_window_overlay);
    }
    std::cout << "Window overlay: " << (debug_window_overlay ? "ON" : "OFF") << "\n";
    ui_dirty = true;
  };

  auto toggle_color_correction = [&]() {
    color_correction = !color_correction;
    {
      std::scoped_lock lock(core_mutex);
      if (system == gbemu::core::System::GBA) {
        core.set_gba_color_correction(color_correction);
      } else if (system == gbemu::core::System::GBC) {
        core.set_cgb_color_correction(color_correction);
      }
    }
    std::cout << "Color correction: " << (color_correction ? "ON" : "OFF") << "\n";
    ui_dirty = true;
  };

  auto toggle_audio = [&]() {
    if (audio_device == 0) {
      return;
    }
    bool enabled = !audio_enabled.load(std::memory_order_relaxed);
    audio_enabled.store(enabled, std::memory_order_relaxed);
    SDL_PauseAudioDevice(audio_device, enabled ? 0 : 1);
    if (!enabled) {
      SDL_ClearQueuedAudio(audio_device);
    }
    std::cout << "Audio: " << (enabled ? "ON" : "OFF") << "\n";
    ui_dirty = true;
  };

  auto menu_labels = [&]() {
    std::vector<std::string> labels;
    labels.emplace_back("RESUME");
    labels.emplace_back("SAVE STATE");
    labels.emplace_back("LOAD STATE");
    labels.emplace_back(std::string("FILTER: ") +
                        filter_name(static_cast<Options::FilterMode>(
                            active_filter_mode.load(std::memory_order_relaxed))));
    labels.emplace_back(std::string("WINDOW OVERLAY: ") + (debug_window_overlay ? "ON" : "OFF"));
    if (system == gbemu::core::System::GBA) {
      labels.emplace_back(std::string("GBA COLOR: ") + (color_correction ? "ON" : "OFF"));
    } else if (system == gbemu::core::System::GBC) {
      labels.emplace_back(std::string("CGB COLOR: ") + (color_correction ? "ON" : "OFF"));
    } else {
      labels.emplace_back("COLOR: N/A");
    }
    if (audio_device != 0) {
      labels.emplace_back(std::string("AUDIO: ") +
                          (audio_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF"));
    } else {
      labels.emplace_back("AUDIO: UNAVAILABLE");
    }
    labels.emplace_back("QUIT");
    return labels;
  };

  auto move_menu = [&](int delta) {
    int count = static_cast<int>(menu_labels().size());
    if (count <= 0) {
      return;
    }
    menu_index = (menu_index + delta + count) % count;
    ui_dirty = true;
  };

  auto menu_visible_items = [&]() {
    constexpr int kPanelBase = 44;
    constexpr int kItemPitch = 20;
    int max_panel_h = std::max(80, fb_height - 20);
    int visible = std::max(1, (max_panel_h - kPanelBase) / kItemPitch);
    return std::max(1, visible);
  };

  auto keep_menu_selection_visible = [&]() {
    int count = static_cast<int>(menu_labels().size());
    if (count <= 0) {
      menu_scroll = 0;
      return;
    }
    int visible = std::min(menu_visible_items(), count);
    int max_start = std::max(0, count - visible);
    if (menu_index < menu_scroll) {
      menu_scroll = menu_index;
    } else if (menu_index >= menu_scroll + visible) {
      menu_scroll = menu_index - visible + 1;
    }
    menu_scroll = std::clamp(menu_scroll, 0, max_start);
  };

  auto apply_menu_action = [&]() {
    switch (menu_index) {
      case kMenuResume:
        set_menu_open(false);
        break;
      case kMenuSaveState:
        save_state();
        break;
      case kMenuLoadState:
        load_state();
        break;
      case kMenuFilter:
        cycle_filter(1);
        break;
      case kMenuWindowOverlay:
        toggle_window_overlay();
        break;
      case kMenuColorCorrection:
        if (system != gbemu::core::System::GB) {
          toggle_color_correction();
        }
        break;
      case kMenuAudio:
        toggle_audio();
        break;
      case kMenuQuit:
        running.store(false, std::memory_order_relaxed);
        break;
      default:
        break;
    }
  };

  while (running.load(std::memory_order_relaxed)) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) {
        running.store(false, std::memory_order_relaxed);
      } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
        int index = ev.cdevice.which;
        if (index >= 0 && SDL_IsGameController(index)) {
          SDL_GameController* controller = SDL_GameControllerOpen(index);
          if (controller) {
            SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
            if (joystick) {
              int instance = SDL_JoystickInstanceID(joystick);
              controllers[instance] = controller;
            } else {
              SDL_GameControllerClose(controller);
            }
          }
        }
      } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        int instance = ev.cdevice.which;
        auto it = controllers.find(instance);
        if (it != controllers.end()) {
          SDL_GameControllerClose(it->second);
          controllers.erase(it);
          pad_state = 0xFF;
          publish_joypad_state();
        }
      } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        renderer.notify_resize();
        keep_menu_selection_visible();
        ui_dirty = true;
      } else if (ev.type == SDL_KEYDOWN) {
        SDL_Keycode key = ev.key.keysym.sym;
        if (key == SDLK_F10) {
          set_menu_open(!menu_open);
          continue;
        }

        if (menu_open) {
          if (key == SDLK_ESCAPE) {
            set_menu_open(false);
          } else if (key == SDLK_UP) {
            move_menu(-1);
            keep_menu_selection_visible();
          } else if (key == SDLK_DOWN) {
            move_menu(1);
            keep_menu_selection_visible();
          } else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
            if (menu_index == kMenuFilter) {
              cycle_filter(key == SDLK_RIGHT ? 1 : -1);
            } else if (menu_index == kMenuColorCorrection &&
                       system != gbemu::core::System::GB) {
              toggle_color_correction();
            } else if (menu_index == kMenuAudio) {
              toggle_audio();
            }
          } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            apply_menu_action();
          }
          continue;
        }

        if (key == SDLK_F5) {
          save_state();
          continue;
        }
        if (key == SDLK_F9) {
          load_state();
          continue;
        }

        if (key == SDLK_ESCAPE) {
          running.store(false, std::memory_order_relaxed);
          continue;
        }
        set_key(key, true);
      } else if (ev.type == SDL_KEYUP) {
        if (menu_open) {
          continue;
        }
        set_key(ev.key.keysym.sym, false);
      } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
        if (!menu_open) {
          set_controller_button(ev.cbutton.button, true);
        }
      } else if (ev.type == SDL_CONTROLLERBUTTONUP) {
        if (!menu_open) {
          set_controller_button(ev.cbutton.button, false);
        }
      } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
        if (!menu_open) {
          set_controller_axis(ev.caxis.axis, ev.caxis.value);
        }
      } else if (ev.type == SDL_MOUSEWHEEL) {
        if (menu_open && ev.wheel.y != 0) {
          move_menu(-ev.wheel.y);
          keep_menu_selection_visible();
        }
      }
    }

    bool new_frame = false;
    std::uint64_t serial = shared_serial.load(std::memory_order_relaxed);
    if (serial != front_serial) {
      {
        std::scoped_lock lock(frame_mutex);
        last_frame = front_buffer;
        front_serial = serial;
      }
      new_frame = !last_frame.empty();
    }

    if (new_frame || ui_dirty) {
      if (!last_frame.empty()) {
        const std::uint32_t* draw_data = last_frame.data();
        if (menu_open) {
          overlay_frame = last_frame;
          keep_menu_selection_visible();
          draw_menu_overlay(overlay_frame.data(), fb_width, fb_height, menu_index, menu_scroll,
                            menu_labels());
          draw_data = overlay_frame.data();
        }

        if (!renderer.draw_frame(draw_data, fb_width, fb_height, &error)) {
          std::cout << "Vulkan render error: " << error << "\n";
          running.store(false, std::memory_order_relaxed);
          break;
        }

        if (new_frame) {
          ++fps_frames;
        }
        ui_dirty = false;
      }
    } else {
      SDL_Delay(1);
    }

    std::uint64_t now = SDL_GetPerformanceCounter();
    if (now - fps_last_tick >= perf_freq) {
      double seconds = static_cast<double>(now - fps_last_tick) / static_cast<double>(perf_freq);
      double fps = seconds > 0.0 ? static_cast<double>(fps_frames) / seconds : 0.0;
      std::snprintf(title_buf, sizeof(title_buf), "GBEmu Vulkan - %.1f FPS", fps);
      SDL_SetWindowTitle(window, title_buf);
      fps_frames = 0;
      fps_last_tick = now;
    }
  }

  running.store(false, std::memory_order_relaxed);
  if (emu_thread.joinable()) {
    emu_thread.join();
  }

  if (audio_device != 0) {
    SDL_CloseAudioDevice(audio_device);
  }
  for (auto& entry : controllers) {
    if (entry.second) {
      SDL_GameControllerClose(entry.second);
    }
  }
  controllers.clear();

  renderer.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

}  // namespace gbemu::frontend
