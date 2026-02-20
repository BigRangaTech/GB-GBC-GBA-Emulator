#include <algorithm>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SDL2/SDL.h>

#include "core.h"
#include "config.h"
#include "input.h"
#include "rom.h"

namespace {

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
      {':', {{0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}}},
      {'-', {{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}},
      {'/', {{0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}}},
      {'+', {{0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}},
      {'?', {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}}},
      {' ', {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}},
  };

  auto it = font.find(c);
  if (it != font.end()) {
    return it->second;
  }
  return empty;
}

std::string upper_ascii(const std::string& input) {
  std::string out = input;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return out;
}

std::string key_label(int keycode) {
  const char* name = SDL_GetKeyName(static_cast<SDL_Keycode>(keycode));
  if (!name || !*name) {
    return "?";
  }
  return upper_ascii(name);
}

void draw_text(SDL_Renderer* renderer, int x, int y, int scale, const std::string& text,
               SDL_Color color) {
  if (!renderer) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  int cursor_x = x;
  int cursor_y = y;
  for (char ch : text) {
    if (ch == '\n') {
      cursor_x = x;
      cursor_y += 8 * scale;
      continue;
    }
    Glyph glyph = glyph_for(ch);
    for (int row = 0; row < 7; ++row) {
      std::uint8_t bits = glyph.rows[row];
      for (int col = 0; col < 5; ++col) {
        if (bits & (1u << (4 - col))) {
          SDL_Rect r{cursor_x + col * scale, cursor_y + row * scale, scale, scale};
          SDL_RenderFillRect(renderer, &r);
        }
      }
    }
    cursor_x += 6 * scale;
  }
}

std::string to_ascii_title(const std::vector<std::uint8_t>& data, std::size_t offset, std::size_t len) {
  if (data.size() < offset + len) {
    return "";
  }
  std::string s;
  s.reserve(len);
  for (std::size_t i = 0; i < len; ++i) {
    char c = static_cast<char>(data[offset + i]);
    s.push_back(c);
  }
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

std::string gb_rom_size(std::uint8_t code) {
  switch (code) {
    case 0x00: return "32 KB";
    case 0x01: return "64 KB";
    case 0x02: return "128 KB";
    case 0x03: return "256 KB";
    case 0x04: return "512 KB";
    case 0x05: return "1 MB";
    case 0x06: return "2 MB";
    case 0x07: return "4 MB";
    case 0x08: return "8 MB";
    case 0x52: return "1.1 MB";
    case 0x53: return "1.2 MB";
    case 0x54: return "1.5 MB";
    default: return "Unknown";
  }
}

std::string gb_ram_size(std::uint8_t code) {
  switch (code) {
    case 0x00: return "None";
    case 0x01: return "2 KB";
    case 0x02: return "8 KB";
    case 0x03: return "32 KB";
    case 0x04: return "128 KB";
    case 0x05: return "64 KB";
    default: return "Unknown";
  }
}

std::string to_lower(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::optional<bool> parse_bool(std::string_view value) {
  std::string lower = to_lower(value);
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
    return true;
  }
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
    return false;
  }
  return std::nullopt;
}

std::optional<gbemu::core::System> parse_system(std::string_view value) {
  std::string lower = to_lower(value);
  if (lower == "gb" || lower == "dmg") {
    return gbemu::core::System::GB;
  }
  if (lower == "gbc" || lower == "cgb") {
    return gbemu::core::System::GBC;
  }
  if (lower == "gba") {
    return gbemu::core::System::GBA;
  }
  return std::nullopt;
}

bool is_auto_system_value(std::string_view value) {
  std::string lower = to_lower(value);
  return lower.empty() || lower == "auto";
}

struct Options {
  std::string rom_path;
  std::string config_path;
  std::string boot_rom_path;
  std::string boot_rom_gb;
  std::string boot_rom_gbc;
  std::string boot_rom_gba;
  std::optional<gbemu::core::System> system_override;
  std::optional<double> fps_override;
  std::optional<int> scale_override;
  std::optional<std::string> video_driver;
  bool cpu_trace = false;
  bool boot_trace = false;
  bool headless = false;
  int headless_frames = 120;
  bool debug_window_overlay = false;
  bool cgb_color_correction = false;
  bool show_help_overlay = false;
  bool show_help = false;
};

void print_usage(const char* exe) {
  std::cout << "Usage: " << exe << " [options] <rom_file>\n";
  std::cout << "Options:\n";
  std::cout << "  --config <path>        Config file path (default: ./gbemu.conf if present)\n";
  std::cout << "  --system <gb|gbc|gba>  Override system detection\n";
  std::cout << "  --fps <value>          Override target frame rate (0 to disable pacing)\n";
  std::cout << "  --scale <int>          Override window scale factor\n";
  std::cout << "  --video-driver <name>  Force SDL video driver (wayland or x11)\n";
  std::cout << "  --boot-rom <path>      Boot ROM path (applies to the current ROM)\n";
  std::cout << "  --boot-rom-gb <path>   Boot ROM path for GB\n";
  std::cout << "  --boot-rom-gbc <path>  Boot ROM path for GBC\n";
  std::cout << "  --boot-rom-gba <path>  Boot ROM path for GBA\n";
  std::cout << "  --cpu-trace            Enable CPU trace buffer on faults\n";
  std::cout << "  --boot-trace           Log when boot ROM is disabled\n";
  std::cout << "  --headless             Run without SDL window\n";
  std::cout << "  --frames <count>       Frames to run in headless mode (default: 120)\n";
  std::cout << "  --debug-window         Draw window border overlay\n";
  std::cout << "  --cgb-color-correct    Apply simple CGB color correction\n";
  std::cout << "  --help-overlay         Toggle help overlay at start\n";
  std::cout << "  -h, --help             Show this help text\n";
}

bool apply_config_file(const std::string& path, Options* options, bool required) {
  if (!options) {
    return false;
  }

  if (!required && !std::filesystem::exists(path)) {
    return true;
  }

  gbemu::common::Config config;
  std::string error;
  if (!config.load_file(path, &error)) {
    std::cout << "Failed to load config file: " << path << ". " << error << "\n";
    return !required;
  }

  std::string system_value = config.get_string("system", "");
  if (!system_value.empty()) {
    auto parsed = parse_system(system_value);
    if (parsed.has_value()) {
      options->system_override = parsed;
    } else if (is_auto_system_value(system_value)) {
      options->system_override.reset();
    } else {
      std::cout << "Config warning: invalid system value '" << system_value << "'\n";
    }
  }

  if (auto fps = config.get_double("fps")) {
    options->fps_override = *fps;
  } else if (auto fps_override = config.get_double("fps_override")) {
    options->fps_override = *fps_override;
  }

  if (auto scale = config.get_int("scale")) {
    options->scale_override = *scale;
  }

  std::string driver = config.get_string("video_driver", "");
  if (!driver.empty()) {
    options->video_driver = driver;
  }

  std::string trace_value = config.get_string("cpu_trace", "");
  if (!trace_value.empty()) {
    auto parsed = parse_bool(trace_value);
    if (parsed.has_value()) {
      options->cpu_trace = *parsed;
    } else {
      std::cout << "Config warning: invalid cpu_trace value '" << trace_value << "'\n";
    }
  }

  std::string boot_trace_value = config.get_string("boot_trace", "");
  if (!boot_trace_value.empty()) {
    auto parsed = parse_bool(boot_trace_value);
    if (parsed.has_value()) {
      options->boot_trace = *parsed;
    } else {
      std::cout << "Config warning: invalid boot_trace value '" << boot_trace_value << "'\n";
    }
  }

  std::string headless_value = config.get_string("headless", "");
  if (!headless_value.empty()) {
    auto parsed = parse_bool(headless_value);
    if (parsed.has_value()) {
      options->headless = *parsed;
    } else {
      std::cout << "Config warning: invalid headless value '" << headless_value << "'\n";
    }
  }

  if (auto frames = config.get_int("headless_frames")) {
    options->headless_frames = *frames;
  }

  std::string debug_window_value = config.get_string("debug_window_overlay", "");
  if (!debug_window_value.empty()) {
    auto parsed = parse_bool(debug_window_value);
    if (parsed.has_value()) {
      options->debug_window_overlay = *parsed;
    } else {
      std::cout << "Config warning: invalid debug_window_overlay value '" << debug_window_value << "'\n";
    }
  }

  std::string cgb_color_value = config.get_string("cgb_color_correction", "");
  if (!cgb_color_value.empty()) {
    auto parsed = parse_bool(cgb_color_value);
    if (parsed.has_value()) {
      options->cgb_color_correction = *parsed;
    } else {
      std::cout << "Config warning: invalid cgb_color_correction value '" << cgb_color_value << "'\n";
    }
  }

  std::string help_value = config.get_string("show_help_overlay", "");
  if (!help_value.empty()) {
    auto parsed = parse_bool(help_value);
    if (parsed.has_value()) {
      options->show_help_overlay = *parsed;
    } else {
      std::cout << "Config warning: invalid show_help_overlay value '" << help_value << "'\n";
    }
  }

  std::string boot_rom = config.get_string("boot_rom", "");
  if (!boot_rom.empty()) {
    options->boot_rom_path = boot_rom;
  }
  std::string boot_rom_gb = config.get_string("boot_rom_gb", "");
  if (!boot_rom_gb.empty()) {
    options->boot_rom_gb = boot_rom_gb;
  }
  std::string boot_rom_gbc = config.get_string("boot_rom_gbc", "");
  if (!boot_rom_gbc.empty()) {
    options->boot_rom_gbc = boot_rom_gbc;
  }
  std::string boot_rom_gba = config.get_string("boot_rom_gba", "");
  if (!boot_rom_gba.empty()) {
    options->boot_rom_gba = boot_rom_gba;
  }

  return true;
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

int default_scale(gbemu::core::System system) {
  if (system == gbemu::core::System::GBA) {
    return 3;
  }
  return 4;
}

std::string system_name(gbemu::core::System system) {
  switch (system) {
    case gbemu::core::System::GBA:
      return "GBA";
    case gbemu::core::System::GBC:
      return "GBC";
    case gbemu::core::System::GB:
    default:
      return "GB";
  }
}

std::vector<std::string> boot_rom_extensions(gbemu::core::System system) {
  if (system == gbemu::core::System::GBA) {
    return {".bin"};
  }
  if (system == gbemu::core::System::GBC) {
    return {".gbc", ".bin"};
  }
  return {".gb", ".bin"};
}

bool has_extension(const std::filesystem::path& path, const std::vector<std::string>& exts) {
  std::string ext = to_lower(path.extension().string());
  for (const auto& allowed : exts) {
    if (ext == allowed) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> find_boot_rom_in_dir(const std::filesystem::path& dir,
                                                gbemu::core::System system) {
  if (!std::filesystem::exists(dir)) {
    return std::nullopt;
  }
  std::vector<std::filesystem::path> candidates;
  auto exts = boot_rom_extensions(system);
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (!has_extension(entry.path(), exts)) {
      continue;
    }
    candidates.push_back(entry.path());
  }
  if (candidates.empty()) {
    return std::nullopt;
  }
  std::sort(candidates.begin(), candidates.end());
  return candidates.front().string();
}

bool read_boot_rom(const std::string& path,
                   std::vector<std::uint8_t>* out,
                   std::string* error) {
  if (path.empty()) {
    if (error) {
      *error = "Boot ROM path is empty";
    }
    return false;
  }
  if (!gbemu::common::read_file(path, out, error)) {
    if (error && error->empty()) {
      *error = "Failed to read boot ROM";
    }
    return false;
  }
  return true;
}

bool load_boot_rom_from_candidates(const std::vector<std::string>& candidates,
                                   std::vector<std::uint8_t>* out,
                                   std::string* error) {
  std::string last_error;
  for (const auto& path : candidates) {
    std::vector<std::uint8_t> data;
    std::string local_error;
    if (read_boot_rom(path, &data, &local_error)) {
      *out = std::move(data);
      return true;
    }
    last_error = local_error.empty() ? "Failed to read boot ROM" : local_error;
  }
  if (error) {
    *error = last_error.empty() ? "Boot ROM not found" : last_error;
  }
  return false;
}

bool load_boot_rom(gbemu::core::System system,
                   const Options& options,
                   std::vector<std::uint8_t>* out,
                   std::string* error) {
  if (!out) {
    return false;
  }

  if (!options.boot_rom_path.empty()) {
    if (!read_boot_rom(options.boot_rom_path, out, error)) {
      return false;
    }
    return true;
  }

  if (system == gbemu::core::System::GB && !options.boot_rom_gb.empty()) {
    return read_boot_rom(options.boot_rom_gb, out, error);
  }
  if (system == gbemu::core::System::GBC && !options.boot_rom_gbc.empty()) {
    return read_boot_rom(options.boot_rom_gbc, out, error);
  }
  if (system == gbemu::core::System::GBA && !options.boot_rom_gba.empty()) {
    return read_boot_rom(options.boot_rom_gba, out, error);
  }

  std::filesystem::path firmware_root("firmware");
  std::filesystem::path system_dir = firmware_root / system_name(system);
  if (auto found = find_boot_rom_in_dir(system_dir, system)) {
    return read_boot_rom(*found, out, error);
  }

  std::vector<std::string> candidates;
  if (system == gbemu::core::System::GB) {
    candidates = {
        "firmware/GB/Game-Boy-Boot-ROM.gb",
        "firmware/dmg_boot.bin",
        "firmware/gb_boot.bin",
        "firmware/boot.gb",
        "firmware/boot.bin",
        "firmware/bootrom.bin",
    };
  } else if (system == gbemu::core::System::GBC) {
    candidates = {
        "firmware/GBC/Game-Boy-Color-Boot-ROM.gbc",
        "firmware/cgb_boot.bin",
        "firmware/gbc_boot.bin",
        "firmware/boot.gbc",
        "firmware/boot.bin",
        "firmware/bootrom.bin",
    };
  } else {
    candidates = {
        "firmware/GBA/Game-Boy-Advance-Boot-ROM.bin",
        "firmware/gba_bios.bin",
        "firmware/gba_boot.bin",
        "firmware/bios.bin",
    };
  }

  return load_boot_rom_from_candidates(candidates, out, error);
}

class FramePacer {
 public:
  explicit FramePacer(double fps)
      : frequency_(SDL_GetPerformanceFrequency()),
        target_ticks_(fps > 0.0 ? static_cast<std::uint64_t>(frequency_ / fps) : 0),
        next_tick_(SDL_GetPerformanceCounter() + target_ticks_) {}

  void sleep() {
    if (target_ticks_ == 0) {
      return;
    }

    std::uint64_t now = SDL_GetPerformanceCounter();
    if (now < next_tick_) {
      std::uint64_t remaining = next_tick_ - now;
      std::uint32_t ms = static_cast<std::uint32_t>((remaining * 1000) / frequency_);
      if (ms > 1) {
        SDL_Delay(ms - 1);
      }
      while (SDL_GetPerformanceCounter() < next_tick_) {
      }
    } else if (now - next_tick_ > target_ticks_ * 5) {
      next_tick_ = now;
    }

    next_tick_ += target_ticks_;
  }

 private:
  std::uint64_t frequency_ = 0;
  std::uint64_t target_ticks_ = 0;
  std::uint64_t next_tick_ = 0;
};

void print_gb_header(const std::vector<std::uint8_t>& data) {
  constexpr std::size_t kHeaderSize = 0x150;
  if (data.size() < kHeaderSize) {
    std::cout << "ROM too small for GB header (need >= 0x150 bytes).\n";
    return;
  }

  std::string title = to_ascii_title(data, 0x0134, 16);
  std::uint8_t cgb_flag = data[0x0143];
  std::uint8_t sgb_flag = data[0x0146];
  std::uint8_t cart_type = data[0x0147];
  std::uint8_t rom_size = data[0x0148];
  std::uint8_t ram_size = data[0x0149];
  std::uint8_t header_checksum = data[0x014D];
  std::uint16_t global_checksum = static_cast<std::uint16_t>(data[0x014E] << 8) | data[0x014F];

  std::cout << "Detected: Game Boy/Game Boy Color ROM\n";
  std::cout << "Title: " << (title.empty() ? "(none)" : title) << "\n";
  std::cout << "CGB Flag: 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(cgb_flag) << std::dec << "\n";
  if (cgb_flag == 0x80) {
    std::cout << "CGB Support: Yes (dual)\n";
  } else if (cgb_flag == 0xC0) {
    std::cout << "CGB Support: Yes (CGB only)\n";
  } else {
    std::cout << "CGB Support: No\n";
  }
  std::cout << "SGB Flag: 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(sgb_flag) << std::dec << "\n";
  std::cout << "Cartridge Type: 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(cart_type) << std::dec << "\n";
  std::cout << "ROM Size: " << gb_rom_size(rom_size) << " (code 0x"
            << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(rom_size)
            << std::dec << ")\n";
  std::cout << "RAM Size: " << gb_ram_size(ram_size) << " (code 0x"
            << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ram_size)
            << std::dec << ")\n";
  std::cout << "Header Checksum: 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(header_checksum) << std::dec << "\n";
  std::cout << "Global Checksum: 0x" << std::hex << std::setw(4) << std::setfill('0')
            << global_checksum << std::dec << "\n";
}

void print_gba_header(const std::vector<std::uint8_t>& data) {
  constexpr std::size_t kHeaderSize = 0xC0;
  if (data.size() < kHeaderSize) {
    std::cout << "ROM too small for GBA header (need >= 0xC0 bytes).\n";
    return;
  }

  std::string title = to_ascii_title(data, 0xA0, 12);
  std::string game_code = to_ascii_title(data, 0xAC, 4);
  std::string maker_code = to_ascii_title(data, 0xB0, 2);
  std::uint8_t fixed_value = data[0xB2];
  std::uint8_t unit_code = data[0xB3];
  std::uint8_t header_checksum = data[0xBD];

  std::cout << "Detected: Game Boy Advance ROM\n";
  std::cout << "Title: " << (title.empty() ? "(none)" : title) << "\n";
  std::cout << "Game Code: " << (game_code.empty() ? "(none)" : game_code) << "\n";
  std::cout << "Maker Code: " << (maker_code.empty() ? "(none)" : maker_code) << "\n";
  std::cout << "Fixed Value: 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(fixed_value) << std::dec << "\n";
  std::cout << "Unit Code: 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(unit_code) << std::dec << "\n";
  std::cout << "Header Checksum: 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(header_checksum) << std::dec << "\n";
}

bool init_sdl_with_fallback(const std::optional<std::string>& driver_override) {
  if (driver_override.has_value()) {
    std::string preferred = to_lower(*driver_override);
    if (preferred == "wayland") {
      SDL_setenv("SDL_VIDEODRIVER", "wayland", 1);
      if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
        return true;
      }
      std::string wayland_error = SDL_GetError();
      SDL_Quit();
      SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
      if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
        std::cout << "Wayland init failed: " << wayland_error << ". Using X11 fallback.\n";
        return true;
      }
      std::cout << "SDL init failed for Wayland (" << wayland_error << ") and X11 ("
                << SDL_GetError() << ").\n";
      return false;
    }
    if (preferred == "x11") {
      SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
      if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
        return true;
      }
      std::cout << "SDL init failed with SDL_VIDEODRIVER=x11: " << SDL_GetError() << "\n";
      return false;
    }
    std::cout << "Unsupported video driver override: " << *driver_override << "\n";
    return false;
  }

  const char* existing = SDL_getenv("SDL_VIDEODRIVER");
  if (existing && *existing) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
      return true;
    }
    std::cout << "SDL init failed with SDL_VIDEODRIVER=" << existing << ": "
              << SDL_GetError() << "\n";
    return false;
  }

  SDL_setenv("SDL_VIDEODRIVER", "wayland", 1);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
    return true;
  }
  std::string wayland_error = SDL_GetError();
  SDL_Quit();

  SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
    std::cout << "Wayland init failed: " << wayland_error << ". Using X11 fallback.\n";
    return true;
  }

  std::cout << "SDL init failed for Wayland (" << wayland_error << ") and X11 ("
            << SDL_GetError() << ").\n";
  return false;
}

} // namespace

int main(int argc, char** argv) {
  gbemu::core::EmulatorCore core;
  std::cout << "GBEmu skeleton v" << core.version() << "\n";

  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --config\n";
        return 1;
      }
      options.config_path = argv[++i];
    } else if (arg == "--boot-rom") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom\n";
        return 1;
      }
      options.boot_rom_path = argv[++i];
    } else if (arg == "--boot-rom-gb") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom-gb\n";
        return 1;
      }
      options.boot_rom_gb = argv[++i];
    } else if (arg == "--boot-rom-gbc") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom-gbc\n";
        return 1;
      }
      options.boot_rom_gbc = argv[++i];
    } else if (arg == "--boot-rom-gba") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom-gba\n";
        return 1;
      }
      options.boot_rom_gba = argv[++i];
    } else if (arg == "--cpu-trace") {
      options.cpu_trace = true;
    } else if (arg == "--boot-trace") {
      options.boot_trace = true;
    } else if (arg == "--headless") {
      options.headless = true;
    } else if (arg == "--frames") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --frames\n";
        return 1;
      }
      try {
        options.headless_frames = std::stoi(argv[++i]);
      } catch (...) {
        std::cout << "Invalid frames value\n";
        return 1;
      }
    } else if (arg == "--debug-window") {
      options.debug_window_overlay = true;
    } else if (arg == "--cgb-color-correct") {
      options.cgb_color_correction = true;
    } else if (arg == "--help-overlay") {
      options.show_help_overlay = true;
    }
  }

  if (options.show_help) {
    print_usage(argv[0]);
    return 0;
  }

  std::string config_path = options.config_path.empty() ? "gbemu.conf" : options.config_path;
  bool config_required = !options.config_path.empty();
  if (!apply_config_file(config_path, &options, config_required)) {
    return 1;
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --config\n";
        return 1;
      }
      ++i;
    } else if (arg == "--boot-rom") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom\n";
        return 1;
      }
      options.boot_rom_path = argv[++i];
    } else if (arg == "--boot-rom-gb") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom-gb\n";
        return 1;
      }
      options.boot_rom_gb = argv[++i];
    } else if (arg == "--boot-rom-gbc") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom-gbc\n";
        return 1;
      }
      options.boot_rom_gbc = argv[++i];
    } else if (arg == "--boot-rom-gba") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --boot-rom-gba\n";
        return 1;
      }
      options.boot_rom_gba = argv[++i];
    } else if (arg == "--cpu-trace") {
      options.cpu_trace = true;
    } else if (arg == "--boot-trace") {
      options.boot_trace = true;
    } else if (arg == "--headless") {
      options.headless = true;
    } else if (arg == "--frames") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --frames\n";
        return 1;
      }
      try {
        options.headless_frames = std::stoi(argv[++i]);
      } catch (...) {
        std::cout << "Invalid frames value\n";
        return 1;
      }
    } else if (arg == "--debug-window") {
      options.debug_window_overlay = true;
    } else if (arg == "--cgb-color-correct") {
      options.cgb_color_correction = true;
    } else if (arg == "--help-overlay") {
      options.show_help_overlay = true;
    } else if (arg == "--system") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --system\n";
        return 1;
      }
      std::string value = argv[++i];
      auto parsed = parse_system(value);
      if (!parsed.has_value() && !is_auto_system_value(value)) {
        std::cout << "Invalid system value: " << value << "\n";
        return 1;
      }
      options.system_override = parsed;
    } else if (arg == "--fps") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --fps\n";
        return 1;
      }
      std::string value = argv[++i];
      try {
        options.fps_override = std::stod(value);
      } catch (...) {
        std::cout << "Invalid fps value: " << value << "\n";
        return 1;
      }
    } else if (arg == "--scale") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --scale\n";
        return 1;
      }
      std::string value = argv[++i];
      try {
        options.scale_override = std::stoi(value);
      } catch (...) {
        std::cout << "Invalid scale value: " << value << "\n";
        return 1;
      }
    } else if (arg == "--video-driver") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --video-driver\n";
        return 1;
      }
      options.video_driver = std::string(argv[++i]);
    } else if (!arg.empty() && arg[0] == '-') {
      std::cout << "Unknown option: " << arg << "\n";
      return 1;
    } else if (options.rom_path.empty()) {
      options.rom_path = arg;
    } else {
      std::cout << "Unexpected argument: " << arg << "\n";
      return 1;
    }
  }

  if (options.show_help) {
    print_usage(argv[0]);
    return 0;
  }

  if (options.rom_path.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  if (options.scale_override && *options.scale_override <= 0) {
    std::cout << "Scale must be positive\n";
    return 1;
  }

  if (options.fps_override && *options.fps_override < 0.0) {
    std::cout << "FPS must be >= 0\n";
    return 1;
  }

  if (options.headless_frames <= 0) {
    std::cout << "Frames must be > 0\n";
    return 1;
  }

  std::string path = options.rom_path;
  std::vector<std::uint8_t> rom;
  std::string error;
  if (!gbemu::common::read_file(path, &rom, &error)) {
    std::cout << "Failed to read ROM: " << path << ". " << error << "\n";
    return 1;
  }

  std::cout << "ROM size: " << rom.size() << " bytes\n";

  gbemu::core::System detected_system = detect_system(rom);
  gbemu::core::System system = options.system_override.value_or(detected_system);
  core.set_system(system);

  std::vector<std::uint8_t> boot_rom;
  std::string boot_error;
  if (!load_boot_rom(system, options, &boot_rom, &boot_error)) {
    std::cout << "Boot ROM required for " << system_name(system) << " but not found. "
              << boot_error << "\n";
    return 1;
  }

  std::string load_error;
  if (!core.load_rom(rom, boot_rom, &load_error)) {
    std::cout << "Failed to load ROM into core: " << load_error << "\n";
    return 1;
  }
  core.set_cpu_trace_enabled(options.cpu_trace);
  core.set_debug_window_overlay(options.debug_window_overlay);
  core.set_cgb_color_correction(options.cgb_color_correction);
  core.set_joypad_state(0xFF);

  bool boot_rom_last = core.boot_rom_enabled();
  auto log_boot_state = [&](long long frame) {
    if (!options.boot_trace) {
      return;
    }
    bool now = core.boot_rom_enabled();
    if (boot_rom_last && !now) {
      std::cout << "Boot ROM disabled at frame " << frame
                << " PC=0x" << std::hex << std::setw(4) << std::setfill('0')
                << core.cpu_pc() << " opcode=0x" << std::setw(2)
                << static_cast<int>(core.cpu_opcode()) << std::dec << "\n";
    } else if (now && (frame % 120 == 0)) {
      std::cout << "Boot ROM still enabled at frame " << frame
                << " PC=0x" << std::hex << std::setw(4) << std::setfill('0')
                << core.cpu_pc() << " opcode=0x" << std::setw(2)
                << static_cast<int>(core.cpu_opcode()) << std::dec << "\n";
    }
    boot_rom_last = now;
  };

  if (options.boot_trace) {
    std::cout << "Boot ROM enabled: " << (boot_rom_last ? "yes" : "no") << "\n";
  }

  gbemu::common::InputConfig input_config;
  input_config.set_default();
  if (!config_path.empty()) {
    gbemu::common::Config config_for_inputs;
    std::string err;
    config_for_inputs.load_file(config_path, &err);
    input_config.load_from_config(config_for_inputs.values());
  }

  std::filesystem::path save_dir("saves");
  std::error_code save_ec;
  std::filesystem::create_directories(save_dir, save_ec);
  std::filesystem::path rom_path = std::filesystem::path(path).filename();
  std::filesystem::path save_base = save_dir / rom_path.stem();
  std::filesystem::path save_path = save_base;
  save_path += ".sav";
  std::filesystem::path rtc_path = save_base;
  rtc_path += ".rtc";
  std::filesystem::path state_path = save_base;
  state_path += ".state";

  if (core.has_battery() && core.has_ram() && std::filesystem::exists(save_path)) {
    std::vector<std::uint8_t> save_data;
    std::string save_error;
    if (gbemu::common::read_file(save_path.string(), &save_data, &save_error)) {
      core.load_ram_data(save_data);
      std::cout << "Loaded save RAM: " << save_path.string() << "\n";
    } else {
      std::cout << "Failed to read save RAM: " << save_error << "\n";
    }
  }

  if (core.has_rtc() && std::filesystem::exists(rtc_path)) {
    std::vector<std::uint8_t> rtc_data;
    std::string rtc_error;
    if (gbemu::common::read_file(rtc_path.string(), &rtc_data, &rtc_error)) {
      core.load_rtc_data(rtc_data);
      std::cout << "Loaded RTC data: " << rtc_path.string() << "\n";
    } else {
      std::cout << "Failed to read RTC data: " << rtc_error << "\n";
    }
  }

  auto dump_cpu_fault = [&core]() {
    std::cout << "CPU fault at PC=0x" << std::hex << std::setw(4) << std::setfill('0')
              << core.cpu_pc() << " opcode=0x" << std::setw(2)
              << static_cast<int>(core.cpu_opcode()) << std::dec << "\n";
    std::cout << "Reason: " << core.cpu_fault_reason() << "\n";
    auto trace = core.cpu_trace();
    if (!trace.empty()) {
      std::cout << "CPU trace (oldest -> newest):\n";
      for (const auto& entry : trace) {
        std::cout << "  PC=0x" << std::hex << std::setw(4) << std::setfill('0') << entry.pc
                  << " OP=0x" << std::setw(2) << static_cast<int>(entry.opcode);
        if (entry.opcode == 0xCB) {
          std::cout << " CB=0x" << std::setw(2) << static_cast<int>(entry.cb_opcode);
        }
        std::cout << std::dec << "\n";
      }
    }
  };

  if (system == gbemu::core::System::GBA) {
    print_gba_header(rom);
  } else if (rom.size() >= 0x150) {
    print_gb_header(rom);
  } else {
    std::cout << "Unknown or too-small ROM.\n";
  }

  auto save_state = [&]() {
    if (core.has_battery() && core.has_ram()) {
      std::vector<std::uint8_t> data = core.ram_data();
      std::string save_error;
      if (gbemu::common::write_file(save_path.string(), data, &save_error)) {
        std::cout << "Saved RAM to " << save_path.string() << "\n";
      } else {
        std::cout << "Failed to save RAM: " << save_error << "\n";
      }
    }
    if (core.has_rtc()) {
      std::vector<std::uint8_t> data = core.rtc_data();
      std::string save_error;
      if (gbemu::common::write_file(rtc_path.string(), data, &save_error)) {
        std::cout << "Saved RTC to " << rtc_path.string() << "\n";
      } else {
        std::cout << "Failed to save RTC: " << save_error << "\n";
      }
    }
  };

  auto save_full_state = [&]() {
    std::vector<std::uint8_t> state;
    if (!core.save_state(&state)) {
      std::cout << "Failed to build save state\n";
      return;
    }
    std::string save_error;
    if (gbemu::common::write_file(state_path.string(), state, &save_error)) {
      std::cout << "Saved state to " << state_path.string() << "\n";
    } else {
      std::cout << "Failed to save state: " << save_error << "\n";
    }
  };

  auto load_full_state = [&]() {
    std::vector<std::uint8_t> state;
    std::string load_error;
    if (!gbemu::common::read_file(state_path.string(), &state, &load_error)) {
      std::cout << "Failed to read state: " << load_error << "\n";
      return;
    }
    std::string err;
    if (!core.load_state(state, &err)) {
      std::cout << "Failed to load state: " << err << "\n";
      return;
    }
    std::cout << "Loaded state from " << state_path.string() << "\n";
  };

  if (options.headless) {
    for (int frame = 0; frame < options.headless_frames; ++frame) {
      if (core.cpu_faulted()) {
        dump_cpu_fault();
        save_state();
        return 1;
      }
      core.step_frame();
      log_boot_state(frame);
    }
    if (core.cpu_faulted()) {
      dump_cpu_fault();
      save_state();
      return 1;
    }
    std::cout << "Headless run completed " << options.headless_frames
              << " frame(s) without CPU fault.\n";
    save_state();
    return 0;
  }

  if (!init_sdl_with_fallback(options.video_driver)) {
    return 1;
  }

  const char* driver = SDL_GetCurrentVideoDriver();
  std::cout << "SDL video driver: " << (driver ? driver : "unknown") << "\n";

  int fb_width = core.framebuffer_width();
  int fb_height = core.framebuffer_height();
  int scale = options.scale_override.value_or(default_scale(system));

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

  SDL_Window* window = SDL_CreateWindow(
      "GBEmu",
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      fb_width * scale,
      fb_height * scale,
      SDL_WINDOW_SHOWN
  );

  if (!window) {
    std::cout << "Failed to create window: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) {
    std::cout << "Failed to create renderer: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_Texture* texture = SDL_CreateTexture(
      renderer,
      SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_STREAMING,
      fb_width,
      fb_height
  );
  if (!texture) {
    std::cout << "Failed to create texture: " << SDL_GetError() << "\n";
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_AudioSpec want = {};
  SDL_AudioSpec have = {};
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (audio_device != 0) {
    SDL_PauseAudioDevice(audio_device, 0);
    std::cout << "Audio device opened at " << have.freq << " Hz\n";
  } else {
    std::cout << "Failed to open audio device: " << SDL_GetError() << "\n";
  }

  std::cout << "Window created. Press ESC or close the window to exit.\n";

  double target_fps = options.fps_override.value_or(core.target_fps());
  FramePacer pacer(target_fps);
  double audio_accum = 0.0;
  const int sample_rate = (audio_device != 0) ? have.freq : 0;
  const std::size_t max_queue_bytes = static_cast<std::size_t>(sample_rate * 4 * 2);

  bool running = true;
  bool show_help = options.show_help_overlay;
  std::uint8_t joypad_state = 0xFF;
  long long frame_count = 0;
  auto update_joypad = [&](SDL_Keycode key, bool pressed) -> bool {
    auto set_bit = [&](int bit, bool down) {
      std::uint8_t before = joypad_state;
      if (down) {
        joypad_state = static_cast<std::uint8_t>(joypad_state & ~(1u << bit));
      } else {
        joypad_state = static_cast<std::uint8_t>(joypad_state | (1u << bit));
      }
      return joypad_state != before;
    };

    if (input_config.resolve(gbemu::common::InputAction::A, key)) return set_bit(0, pressed);
    if (input_config.resolve(gbemu::common::InputAction::B, key)) return set_bit(1, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Select, key)) return set_bit(2, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Start, key)) return set_bit(3, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Right, key)) return set_bit(4, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Left, key)) return set_bit(5, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Up, key)) return set_bit(6, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Down, key)) return set_bit(7, pressed);
    return false;
  };

  auto build_help_text = [&]() {
    std::string text;
    text += "GBEMU HELP\n";
    text += "F1 WINDOW OVERLAY\n";
    text += "F2 CGB COLOR\n";
    text += "F3 TOGGLE HELP\n";
    text += "F5 SAVE STATE\n";
    text += "F9 LOAD STATE\n";
    text += "A: " + key_label(input_config.key_for(gbemu::common::InputAction::A));
    text += "  B: " + key_label(input_config.key_for(gbemu::common::InputAction::B)) + "\n";
    text += "SELECT: " + key_label(input_config.key_for(gbemu::common::InputAction::Select));
    text += "  START: " + key_label(input_config.key_for(gbemu::common::InputAction::Start)) + "\n";
    text += "UP: " + key_label(input_config.key_for(gbemu::common::InputAction::Up));
    text += " DOWN: " + key_label(input_config.key_for(gbemu::common::InputAction::Down)) + "\n";
    text += "LEFT: " + key_label(input_config.key_for(gbemu::common::InputAction::Left));
    text += " RIGHT: " + key_label(input_config.key_for(gbemu::common::InputAction::Right)) + "\n";
    return text;
  };
  bool debug_window_overlay = options.debug_window_overlay;
  bool cgb_color_correction = options.cgb_color_correction;
  while (running) {
    if (core.cpu_faulted()) {
      dump_cpu_fault();
      save_state();
      break;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
        running = false;
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F1) {
        debug_window_overlay = !debug_window_overlay;
        core.set_debug_window_overlay(debug_window_overlay);
        std::cout << "Debug window overlay: " << (debug_window_overlay ? "ON" : "OFF") << "\n";
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F2) {
        cgb_color_correction = !cgb_color_correction;
        core.set_cgb_color_correction(cgb_color_correction);
        std::cout << "CGB color correction: " << (cgb_color_correction ? "ON" : "OFF") << "\n";
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F3) {
        show_help = !show_help;
        std::cout << "Help overlay: " << (show_help ? "ON" : "OFF") << "\n";
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F5) {
        save_full_state();
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F9) {
        load_full_state();
      } else if (event.type == SDL_KEYDOWN) {
        if (update_joypad(event.key.keysym.sym, true)) {
          core.set_joypad_state(joypad_state);
          core.request_interrupt(4);
        }
      } else if (event.type == SDL_KEYUP) {
        if (update_joypad(event.key.keysym.sym, false)) {
          core.set_joypad_state(joypad_state);
          core.request_interrupt(4);
        }
      }
    }
    core.step_frame();
    log_boot_state(frame_count);
    ++frame_count;
    SDL_UpdateTexture(texture, nullptr, core.framebuffer(), core.framebuffer_stride_bytes());
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    if (show_help) {
      int w = fb_width * scale;
      int h = fb_height * scale;
      int panel_width = w > 360 ? 360 : w - 20;
      int panel_height = 150;
      SDL_Rect panel{10, 10, panel_width, panel_height};
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
      SDL_RenderFillRect(renderer, &panel);
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
      SDL_RenderDrawRect(renderer, &panel);
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
      std::string help = build_help_text();
      draw_text(renderer, panel.x + 8, panel.y + 8, 2, help, SDL_Color{255, 255, 255, 255});
    }
    SDL_RenderPresent(renderer);
    if (audio_device != 0 && sample_rate > 0) {
      audio_accum += static_cast<double>(sample_rate) / target_fps;
      int samples = static_cast<int>(audio_accum);
      if (samples > 0) {
        audio_accum -= samples;
        if (SDL_GetQueuedAudioSize(audio_device) < max_queue_bytes) {
          std::vector<std::int16_t> audio;
          core.generate_audio(sample_rate, samples, &audio);
          if (!audio.empty()) {
            SDL_QueueAudio(audio_device, audio.data(),
                           static_cast<Uint32>(audio.size() * sizeof(std::int16_t)));
          }
        }
      }
    }
    pacer.sleep();
  }

  save_state();

  if (audio_device != 0) {
    SDL_CloseAudioDevice(audio_device);
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
