#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL2/SDL.h>

#include "core.h"
#include "config.h"
#include "rom.h"

namespace {

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

  std::vector<std::string> candidates;
  if (system == gbemu::core::System::GB) {
    candidates = {
        "firmware/dmg_boot.bin",
        "firmware/gb_boot.bin",
        "firmware/boot.gb",
        "firmware/boot.bin",
        "firmware/bootrom.bin",
    };
  } else if (system == gbemu::core::System::GBC) {
    candidates = {
        "firmware/cgb_boot.bin",
        "firmware/gbc_boot.bin",
        "firmware/boot.gbc",
        "firmware/boot.bin",
        "firmware/bootrom.bin",
    };
  } else {
    candidates = {
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

  if (system == gbemu::core::System::GBA) {
    print_gba_header(rom);
  } else if (rom.size() >= 0x150) {
    print_gb_header(rom);
  } else {
    std::cout << "Unknown or too-small ROM.\n";
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

  std::cout << "Window created. Press ESC or close the window to exit.\n";

  double target_fps = options.fps_override.value_or(core.target_fps());
  FramePacer pacer(target_fps);

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
        running = false;
      }
    }
    core.step_frame();
    SDL_UpdateTexture(texture, nullptr, core.framebuffer(), core.framebuffer_stride_bytes());
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    pacer.sleep();
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
