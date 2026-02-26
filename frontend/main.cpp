#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <SDL2/SDL.h>
#ifdef GBEMU_HAS_SDL_IMAGE
#include <SDL2/SDL_image.h>
#endif

#include "core.h"
#include "config.h"
#include "input.h"
#include "rom.h"
#include "vk_frontend.h"

namespace {

struct Glyph {
  std::array<std::uint8_t, 7> rows{};
};

std::string to_lower(std::string_view value);
std::string trim_ascii(std::string_view value);
std::optional<bool> parse_bool(std::string_view value);
gbemu::core::System detect_system(const std::vector<std::uint8_t>& data);

struct ArmRomBuilder {
  static std::uint32_t dp_imm(std::uint8_t opcode,
                              bool set_flags,
                              std::uint8_t rn,
                              std::uint8_t rd,
                              std::uint8_t rot,
                              std::uint8_t imm8) {
    return 0xE0000000u |
           (1u << 25) |
           (static_cast<std::uint32_t>(opcode) << 21) |
           (set_flags ? (1u << 20) : 0) |
           (static_cast<std::uint32_t>(rn) << 16) |
           (static_cast<std::uint32_t>(rd) << 12) |
           (static_cast<std::uint32_t>(rot & 0x0F) << 8) |
           imm8;
  }

  static std::uint32_t dp_reg(std::uint8_t opcode,
                              bool set_flags,
                              std::uint8_t rn,
                              std::uint8_t rd,
                              std::uint8_t rm,
                              std::uint8_t shift_type,
                              std::uint8_t shift_imm) {
    return 0xE0000000u |
           (static_cast<std::uint32_t>(opcode) << 21) |
           (set_flags ? (1u << 20) : 0) |
           (static_cast<std::uint32_t>(rn) << 16) |
           (static_cast<std::uint32_t>(rd) << 12) |
           (static_cast<std::uint32_t>(shift_imm & 0x1F) << 7) |
           (static_cast<std::uint32_t>(shift_type & 0x03) << 5) |
           (rm & 0x0F);
  }

  static std::uint32_t str_imm(std::uint8_t rd,
                               std::uint8_t rn,
                               std::uint16_t offset,
                               bool pre_index,
                               bool up,
                               bool byte,
                               bool writeback) {
    return 0xE0000000u |
           (1u << 26) |
           (pre_index ? (1u << 24) : 0) |
           (up ? (1u << 23) : 0) |
           (byte ? (1u << 22) : 0) |
           (writeback ? (1u << 21) : 0) |
           (static_cast<std::uint32_t>(rn) << 16) |
           (static_cast<std::uint32_t>(rd) << 12) |
           (offset & 0x0FFF);
  }

  static std::uint32_t b_imm(std::uint8_t cond, std::int32_t imm24, bool link) {
    return (static_cast<std::uint32_t>(cond) << 28) |
           (link ? 0x0B000000u : 0x0A000000u) |
           (static_cast<std::uint32_t>(imm24) & 0x00FFFFFFu);
  }
};

void write_le32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  if (offset + 4 > out.size()) {
    return;
  }
  out[offset + 0] = static_cast<std::uint8_t>(value & 0xFF);
  out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

std::vector<std::uint8_t> build_gba_mode3_test_rom() {
  constexpr std::uint32_t base = 0x08000000u;
  std::vector<std::uint8_t> rom(0x2000, 0);
  const char* title = "MODE3TEST";
  for (int i = 0; title[i] != '\0' && (0xA0 + i) < static_cast<int>(rom.size()); ++i) {
    rom[0xA0 + i] = static_cast<std::uint8_t>(title[i]);
  }
  rom[0xAC] = 'M';
  rom[0xAD] = '3';
  rom[0xAE] = 'T';
  rom[0xAF] = 'S';
  rom[0xB2] = 0x96;
  rom[0xB3] = 0x00;

  std::vector<std::uint32_t> code;
  auto emit = [&](std::uint32_t op) { code.push_back(op); };

  emit(ArmRomBuilder::dp_imm(0xD, false, 0, 0, 4, 0x06));  // r0 = 0x06000000
  emit(ArmRomBuilder::dp_imm(0xD, false, 0, 1, 4, 0x04));  // r1 = 0x04000000
  emit(ArmRomBuilder::dp_imm(0xD, false, 0, 2, 0, 0x03));  // r2 = 3
  emit(ArmRomBuilder::dp_imm(0xD, false, 0, 4, 0, 0x04));  // r4 = 4
  emit(ArmRomBuilder::dp_reg(0xD, false, 0, 4, 4, 0, 8));  // r4 = r4 << 8
  emit(ArmRomBuilder::dp_reg(0xC, false, 2, 2, 4, 0, 0));  // r2 |= r4
  emit(ArmRomBuilder::str_imm(2, 1, 0, true, true, false, false)); // STR r2, [r1]
  emit(ArmRomBuilder::dp_imm(0xD, false, 0, 3, 0, 0x1F)); // r3 = 0x1F
  emit(ArmRomBuilder::dp_reg(0xD, false, 0, 4, 3, 0, 16)); // r4 = r3 << 16
  emit(ArmRomBuilder::dp_reg(0xC, false, 3, 3, 4, 0, 0)); // r3 |= r4
  emit(ArmRomBuilder::dp_imm(0xD, false, 0, 5, 0, 0x4B)); // r5 = 0x4B
  emit(ArmRomBuilder::dp_reg(0xD, false, 0, 5, 5, 0, 8)); // r5 = r5 << 8 (0x4B00)

  std::size_t loop_index = code.size();
  emit(ArmRomBuilder::str_imm(3, 0, 4, false, true, false, false)); // STR r3, [r0], #4
  emit(ArmRomBuilder::dp_imm(0x2, true, 5, 5, 0, 0x01)); // SUBS r5, r5, #1
  std::size_t bne_index = code.size();
  emit(0);
  std::size_t halt_index = code.size();
  emit(0);

  auto patch_branch = [&](std::size_t index, std::size_t target, std::uint8_t cond) {
    std::uint32_t pc = base + static_cast<std::uint32_t>(index * 4);
    std::uint32_t target_addr = base + static_cast<std::uint32_t>(target * 4);
    std::int32_t offset = static_cast<std::int32_t>(target_addr) -
                          static_cast<std::int32_t>(pc + 8);
    std::int32_t imm24 = offset >> 2;
    code[index] = ArmRomBuilder::b_imm(cond, imm24, false);
  };

  patch_branch(bne_index, loop_index, 0x1);  // BNE loop
  patch_branch(halt_index, halt_index, 0xE); // B .

  for (std::size_t i = 0; i < code.size(); ++i) {
    write_le32(rom, i * 4, code[i]);
  }
  return rom;
}

enum class UiTheme {
  Retro,
  Minimal,
  Deck,
};

struct UiThemeDef {
  const char* id = "";
  const char* display = "";
  SDL_Color bg_primary{};
  SDL_Color bg_secondary{};
  SDL_Color panel{};
  SDL_Color panel_border{};
  SDL_Color text{};
  SDL_Color accent{};
  int panel_padding = 8;
  int border_thickness = 1;
  int font_scale = 2;
};

struct LauncherLayout {
  SDL_Rect header{};
  SDL_Rect search_box{};
  SDL_Rect settings_button{};
  SDL_Rect filter_hit{};
  SDL_Rect list_panel{};
  SDL_Rect detail_panel{};
  SDL_Rect override_button{};
  SDL_Rect play_button{};
  SDL_Rect favorite_button{};
  int card_h = 38;
  int visible = 1;
  int start_y = 0;
};

const UiThemeDef& theme_def(UiTheme theme) {
  static const UiThemeDef kRetro{
      "retro",
      "RETRO HARDWARE",
      SDL_Color{10, 24, 12, 255},
      SDL_Color{18, 40, 20, 255},
      SDL_Color{10, 22, 12, 210},
      SDL_Color{120, 201, 90, 255},
      SDL_Color{186, 234, 165, 255},
      SDL_Color{255, 224, 120, 255},
      10,
      2,
      2,
  };
  static const UiThemeDef kMinimal{
      "minimal",
      "MODERN MINIMAL",
      SDL_Color{244, 246, 248, 255},
      SDL_Color{230, 234, 238, 255},
      SDL_Color{250, 250, 250, 230},
      SDL_Color{120, 128, 136, 255},
      SDL_Color{24, 28, 32, 255},
      SDL_Color{20, 110, 220, 255},
      12,
      1,
      2,
  };
  static const UiThemeDef kDeck{
      "deck",
      "DECK STYLE",
      SDL_Color{12, 14, 22, 255},
      SDL_Color{18, 24, 36, 255},
      SDL_Color{12, 16, 26, 220},
      SDL_Color{84, 182, 176, 255},
      SDL_Color{220, 230, 238, 255},
      SDL_Color{88, 210, 200, 255},
      12,
      2,
      2,
  };
  switch (theme) {
    case UiTheme::Retro: return kRetro;
    case UiTheme::Deck: return kDeck;
    case UiTheme::Minimal:
    default:
      return kMinimal;
  }
}

std::optional<UiTheme> parse_ui_theme(std::string_view value) {
  std::string lower = to_lower(std::string(value));
  if (lower == "retro") {
    return UiTheme::Retro;
  }
  if (lower == "minimal" || lower == "modern") {
    return UiTheme::Minimal;
  }
  if (lower == "deck") {
    return UiTheme::Deck;
  }
  return std::nullopt;
}

std::string ui_theme_name(UiTheme theme) {
  switch (theme) {
    case UiTheme::Retro: return "retro";
    case UiTheme::Minimal: return "minimal";
    case UiTheme::Deck: return "deck";
    default: return "retro";
  }
}

UiTheme next_theme(UiTheme theme, int direction) {
  int idx = 0;
  switch (theme) {
    case UiTheme::Retro: idx = 0; break;
    case UiTheme::Minimal: idx = 1; break;
    case UiTheme::Deck: idx = 2; break;
    default: idx = 0; break;
  }
  idx = (idx + direction + 3) % 3;
  if (idx == 1) return UiTheme::Minimal;
  if (idx == 2) return UiTheme::Deck;
  return UiTheme::Retro;
}

enum class UiMode {
  Hidden,
  Menu,
  Launcher,
  Settings,
  InputMap,
};

enum class LauncherFilter {
  All,
  Favorites,
  Recents,
};

enum class VideoFilter {
  None,
  Scanlines,
  Lcd,
  Crt,
};

std::string video_filter_name(VideoFilter filter) {
  switch (filter) {
    case VideoFilter::Scanlines: return "scanlines";
    case VideoFilter::Lcd: return "lcd";
    case VideoFilter::Crt: return "crt";
    case VideoFilter::None:
    default:
      return "none";
  }
}

std::optional<VideoFilter> parse_video_filter(std::string_view value) {
  std::string lower = to_lower(std::string(value));
  if (lower == "scanlines" || lower == "scanline") {
    return VideoFilter::Scanlines;
  }
  if (lower == "lcd" || lower == "grid") {
    return VideoFilter::Lcd;
  }
  if (lower == "crt") {
    return VideoFilter::Crt;
  }
  if (lower == "none" || lower == "off") {
    return VideoFilter::None;
  }
  return std::nullopt;
}

VideoFilter next_video_filter(VideoFilter filter, int direction) {
  int idx = 0;
  switch (filter) {
    case VideoFilter::None: idx = 0; break;
    case VideoFilter::Scanlines: idx = 1; break;
    case VideoFilter::Lcd: idx = 2; break;
    case VideoFilter::Crt: idx = 3; break;
    default: idx = 0; break;
  }
  idx = (idx + direction + 4) % 4;
  if (idx == 1) return VideoFilter::Scanlines;
  if (idx == 2) return VideoFilter::Lcd;
  if (idx == 3) return VideoFilter::Crt;
  return VideoFilter::None;
}

enum class HudCorner {
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
};

std::string hud_corner_name(HudCorner corner) {
  switch (corner) {
    case HudCorner::TopRight: return "top-right";
    case HudCorner::BottomLeft: return "bottom-left";
    case HudCorner::BottomRight: return "bottom-right";
    case HudCorner::TopLeft:
    default:
      return "top-left";
  }
}

std::optional<HudCorner> parse_hud_corner(std::string_view value) {
  std::string lower = to_lower(std::string(value));
  if (lower == "top-left" || lower == "topleft" || lower == "tl") {
    return HudCorner::TopLeft;
  }
  if (lower == "top-right" || lower == "topright" || lower == "tr") {
    return HudCorner::TopRight;
  }
  if (lower == "bottom-left" || lower == "bottomleft" || lower == "bl") {
    return HudCorner::BottomLeft;
  }
  if (lower == "bottom-right" || lower == "bottomright" || lower == "br") {
    return HudCorner::BottomRight;
  }
  return std::nullopt;
}

HudCorner next_hud_corner(HudCorner corner, int direction) {
  int idx = 0;
  switch (corner) {
    case HudCorner::TopLeft: idx = 0; break;
    case HudCorner::TopRight: idx = 1; break;
    case HudCorner::BottomLeft: idx = 2; break;
    case HudCorner::BottomRight: idx = 3; break;
    default: idx = 0; break;
  }
  idx = (idx + direction + 4) % 4;
  if (idx == 1) return HudCorner::TopRight;
  if (idx == 2) return HudCorner::BottomLeft;
  if (idx == 3) return HudCorner::BottomRight;
  return HudCorner::TopLeft;
}

std::string launcher_filter_name(LauncherFilter filter) {
  switch (filter) {
    case LauncherFilter::Favorites: return "favorites";
    case LauncherFilter::Recents: return "recents";
    case LauncherFilter::All:
    default:
      return "all";
  }
}

LauncherFilter next_launcher_filter(LauncherFilter filter, int direction) {
  int idx = 0;
  switch (filter) {
    case LauncherFilter::All: idx = 0; break;
    case LauncherFilter::Favorites: idx = 1; break;
    case LauncherFilter::Recents: idx = 2; break;
    default: idx = 0; break;
  }
  idx = (idx + direction + 3) % 3;
  if (idx == 1) return LauncherFilter::Favorites;
  if (idx == 2) return LauncherFilter::Recents;
  return LauncherFilter::All;
}

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
      {'|', {{0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}},
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
  if (keycode <= 0) {
    return "NONE";
  }
  const char* name = SDL_GetKeyName(static_cast<SDL_Keycode>(keycode));
  if (!name || !*name) {
    return "?";
  }
  return upper_ascii(name);
}

std::string controller_button_label(int button) {
  const char* name = SDL_GameControllerGetStringForButton(
      static_cast<SDL_GameControllerButton>(button));
  if (!name || !*name) {
    return "BUTTON";
  }
  return upper_ascii(name);
}

std::string controller_axis_label(int axis, bool positive) {
  const char* name = SDL_GameControllerGetStringForAxis(
      static_cast<SDL_GameControllerAxis>(axis));
  if (!name || !*name) {
    return "AXIS";
  }
  std::string label = upper_ascii(name);
  label.push_back(positive ? '+' : '-');
  return label;
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

int text_width(const std::string& text, int scale) {
  int width = 0;
  int line = 0;
  for (char ch : text) {
    if (ch == '\n') {
      width = std::max(width, line);
      line = 0;
      continue;
    }
    line += 6 * scale;
  }
  width = std::max(width, line);
  return width;
}

bool point_in_rect(int x, int y, const SDL_Rect& rect) {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

void fill_rect(SDL_Renderer* renderer, const SDL_Rect& rect, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

void draw_panel(SDL_Renderer* renderer, const SDL_Rect& rect, const UiThemeDef& theme) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  fill_rect(renderer, rect, theme.panel);
  SDL_SetRenderDrawColor(renderer, theme.panel_border.r, theme.panel_border.g,
                         theme.panel_border.b, theme.panel_border.a);
  for (int i = 0; i < theme.border_thickness; ++i) {
    SDL_Rect r{rect.x + i, rect.y + i, rect.w - i * 2, rect.h - i * 2};
    SDL_RenderDrawRect(renderer, &r);
  }
}

SDL_Color with_alpha(SDL_Color color, float alpha) {
  SDL_Color out = color;
  float scaled = static_cast<float>(color.a) * alpha;
  if (scaled < 0.0f) scaled = 0.0f;
  if (scaled > 255.0f) scaled = 255.0f;
  out.a = static_cast<Uint8>(scaled);
  return out;
}

void draw_panel_alpha(SDL_Renderer* renderer, const SDL_Rect& rect, const UiThemeDef& theme,
                      float alpha) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  fill_rect(renderer, rect, with_alpha(theme.panel, alpha));
  SDL_Color border = with_alpha(theme.panel_border, alpha);
  SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
  for (int i = 0; i < theme.border_thickness; ++i) {
    SDL_Rect r{rect.x + i, rect.y + i, rect.w - i * 2, rect.h - i * 2};
    SDL_RenderDrawRect(renderer, &r);
  }
}

void draw_menu_decor(SDL_Renderer* renderer, const SDL_Rect& rect, const UiThemeDef& theme) {
  if (!renderer) {
    return;
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  if (std::string(theme.id) == "retro") {
    SDL_SetRenderDrawColor(renderer, theme.accent.r, theme.accent.g, theme.accent.b, 30);
    for (int y = rect.y + 2; y < rect.y + rect.h; y += 3) {
      SDL_RenderDrawLine(renderer, rect.x + 2, y, rect.x + rect.w - 3, y);
    }
  } else if (std::string(theme.id) == "minimal") {
    for (int y = 0; y < rect.h; ++y) {
      std::uint8_t shade = static_cast<std::uint8_t>(theme.bg_secondary.r + (y * 8) / rect.h);
      SDL_SetRenderDrawColor(renderer, shade, shade, shade, 20);
      SDL_RenderDrawLine(renderer, rect.x + 1, rect.y + y, rect.x + rect.w - 2, rect.y + y);
    }
  } else {
    SDL_SetRenderDrawColor(renderer, theme.accent.r, theme.accent.g, theme.accent.b, 40);
    for (int i = 0; i < rect.w; i += 6) {
      SDL_RenderDrawLine(renderer, rect.x + i, rect.y + rect.h - 2,
                         rect.x + i + 10, rect.y + rect.h - 12);
    }
  }
}

void draw_scanlines(SDL_Renderer* renderer, const SDL_Rect& rect, Uint8 alpha) {
  if (!renderer || alpha == 0) {
    return;
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
  for (int y = rect.y; y < rect.y + rect.h; y += 2) {
    SDL_RenderDrawLine(renderer, rect.x, y, rect.x + rect.w, y);
  }
}

void draw_lcd(SDL_Renderer* renderer, const SDL_Rect& rect, Uint8 alpha) {
  if (!renderer || alpha == 0) {
    return;
  }
  draw_scanlines(renderer, rect, static_cast<Uint8>(alpha / 2));
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (int x = rect.x; x < rect.x + rect.w; x += 3) {
    SDL_SetRenderDrawColor(renderer, 180, 40, 40, alpha);
    SDL_RenderDrawLine(renderer, x, rect.y, x, rect.y + rect.h);
    if (x + 1 < rect.x + rect.w) {
      SDL_SetRenderDrawColor(renderer, 40, 180, 40, alpha);
      SDL_RenderDrawLine(renderer, x + 1, rect.y, x + 1, rect.y + rect.h);
    }
    if (x + 2 < rect.x + rect.w) {
      SDL_SetRenderDrawColor(renderer, 40, 40, 180, alpha);
      SDL_RenderDrawLine(renderer, x + 2, rect.y, x + 2, rect.y + rect.h);
    }
  }
}

class FilterWorkerPool;

void apply_crt_filter(const std::uint32_t* src,
                      int src_stride_words,
                      std::uint32_t* dst,
                      std::uint32_t* prev,
                      int width,
                      int height,
                      bool reset,
                      FilterWorkerPool* pool);

struct CoverTexture {
  SDL_Texture* texture = nullptr;
  int width = 0;
  int height = 0;
};

SDL_Surface* load_ppm_surface(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return nullptr;
  }
  std::string magic;
  file >> magic;
  if (magic != "P6") {
    return nullptr;
  }
  int width = 0;
  int height = 0;
  int maxval = 0;
  char ch;
  file.get(ch);
  while (file.peek() == '#') {
    std::string comment;
    std::getline(file, comment);
  }
  file >> width >> height >> maxval;
  file.get(ch);
  if (width <= 0 || height <= 0 || maxval <= 0) {
    return nullptr;
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
  file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  if (!file) {
    return nullptr;
  }

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
  for (int i = 0, j = 0; i < width * height; ++i, j += 3) {
    rgba[i * 4 + 0] = pixels[j + 0];
    rgba[i * 4 + 1] = pixels[j + 1];
    rgba[i * 4 + 2] = pixels[j + 2];
    rgba[i * 4 + 3] = 255;
  }
  SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
      rgba.data(), width, height, 32, width * 4,
      0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
  if (!surface) {
    return nullptr;
  }
  SDL_Surface* converted = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0);
  SDL_FreeSurface(surface);
  if (!converted) {
    return nullptr;
  }
  return converted;
}

SDL_Surface* load_cover_surface(const std::string& path) {
  std::string ext = to_lower(std::filesystem::path(path).extension().string());
#ifdef GBEMU_HAS_SDL_IMAGE
  SDL_Surface* loaded = IMG_Load(path.c_str());
  if (loaded) {
    SDL_Surface* converted = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(loaded);
    return converted;
  }
#endif
  if (ext == ".bmp") {
    SDL_Surface* loaded = SDL_LoadBMP(path.c_str());
    if (!loaded) {
      return nullptr;
    }
    SDL_Surface* converted = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(loaded);
    return converted;
  }
  if (ext == ".ppm") {
    return load_ppm_surface(path);
  }
  return nullptr;
}

SDL_Rect fit_rect(int dest_w, int dest_h, int src_w, int src_h, int x, int y) {
  if (src_w <= 0 || src_h <= 0) {
    return SDL_Rect{x, y, dest_w, dest_h};
  }
  float scale = std::min(static_cast<float>(dest_w) / src_w, static_cast<float>(dest_h) / src_h);
  int w = static_cast<int>(src_w * scale);
  int h = static_cast<int>(src_h * scale);
  int dx = x + (dest_w - w) / 2;
  int dy = y + (dest_h - h) / 2;
  return SDL_Rect{dx, dy, w, h};
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

struct RomEntry {
  std::string path;
  std::string title;
  gbemu::core::System system = gbemu::core::System::GB;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified{};
  std::string cover_path;
};

bool matches_search(const RomEntry& entry, const std::string& query) {
  std::string trimmed = trim_ascii(query);
  if (trimmed.empty()) {
    return true;
  }
  std::string needle = to_lower(trimmed);
  std::string title = to_lower(entry.title);
  if (title.find(needle) != std::string::npos) {
    return true;
  }
  std::string filename = to_lower(std::filesystem::path(entry.path).filename().string());
  if (filename.find(needle) != std::string::npos) {
    return true;
  }
  return false;
}

std::string system_short(gbemu::core::System system) {
  switch (system) {
    case gbemu::core::System::GBA: return "GBA";
    case gbemu::core::System::GBC: return "GBC";
    case gbemu::core::System::GB:
    default:
      return "GB";
  }
}

std::string format_bytes(std::uintmax_t bytes) {
  if (bytes >= (1ull << 20)) {
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream out;
    out << std::fixed << std::setprecision(mb < 10.0 ? 1 : 0) << mb << " MB";
    return out.str();
  }
  if (bytes >= (1ull << 10)) {
    double kb = static_cast<double>(bytes) / 1024.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(kb < 10.0 ? 1 : 0) << kb << " KB";
    return out.str();
  }
  return std::to_string(bytes) + " B";
}

std::vector<std::string> split_list(std::string_view value) {
  std::vector<std::string> out;
  std::string current;
  for (char c : value) {
    if (c == ';' || c == ',') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

std::string find_cover_path(const std::filesystem::path& rom_path) {
  std::vector<std::string> exts = {".png", ".jpg", ".jpeg", ".bmp", ".ppm"};
  std::filesystem::path base = rom_path;
  base.replace_extension();
  for (const auto& ext : exts) {
    std::filesystem::path candidate = base;
    candidate += ext;
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }

  std::filesystem::path filename = rom_path.stem();
  std::filesystem::path covers_dir = rom_path.parent_path() / "covers";
  for (const auto& ext : exts) {
    std::filesystem::path candidate = covers_dir / filename;
    candidate += ext;
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }

  std::filesystem::path global_covers = std::filesystem::path("covers") / filename;
  for (const auto& ext : exts) {
    std::filesystem::path candidate = global_covers;
    candidate += ext;
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }
  return "";
}

bool rom_extension_ok(const std::filesystem::path& path) {
  std::string ext = to_lower(path.extension().string());
  return ext == ".gb" || ext == ".gbc" || ext == ".gba";
}

std::string rom_title_from_data(const std::vector<std::uint8_t>& data, gbemu::core::System system) {
  if (system == gbemu::core::System::GBA) {
    std::string title = to_ascii_title(data, 0xA0, 12);
    return title;
  }
  return to_ascii_title(data, 0x0134, 16);
}

std::vector<RomEntry> scan_roms(const std::vector<std::filesystem::path>& roots) {
  std::vector<RomEntry> out;
  for (const auto& root : roots) {
    if (root.empty() || !std::filesystem::exists(root)) {
      continue;
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (ec) {
        break;
      }
      const auto& entry = *it;
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto& path = entry.path();
      if (!rom_extension_ok(path)) {
        continue;
      }
      RomEntry rom;
      rom.path = path.string();
      rom.size = entry.file_size(ec);
      rom.modified = entry.last_write_time(ec);
      std::vector<std::uint8_t> data;
      std::string error;
      if (gbemu::common::read_file(rom.path, &data, &error)) {
        rom.system = detect_system(data);
        rom.title = rom_title_from_data(data, rom.system);
      }
      if (rom.title.empty()) {
        rom.title = path.stem().string();
      }
      rom.cover_path = find_cover_path(path);
      out.push_back(std::move(rom));
    }
  }
  std::sort(out.begin(), out.end(), [](const RomEntry& a, const RomEntry& b) {
    return a.title < b.title;
  });
  return out;
}

std::string escape_field(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '|') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::string unescape_field(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  bool escape = false;
  for (char c : value) {
    if (escape) {
      out.push_back(c);
      escape = false;
    } else if (c == '\\') {
      escape = true;
    } else {
      out.push_back(c);
    }
  }
  if (escape) {
    out.push_back('\\');
  }
  return out;
}

void load_launcher_state(const std::string& path,
                         std::vector<std::string>* recents,
                         std::unordered_set<std::string>* favorites) {
  if (recents) recents->clear();
  if (favorites) favorites->clear();
  std::ifstream file(path);
  if (!file) {
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.size() < 3 || line[1] != '|') {
      continue;
    }
    char kind = line[0];
    std::string payload = unescape_field(line.substr(2));
    if (kind == 'R') {
      if (recents && !payload.empty()) {
        recents->push_back(payload);
      }
    } else if (kind == 'F') {
      if (favorites && !payload.empty()) {
        favorites->insert(payload);
      }
    }
  }
}

void save_launcher_state(const std::string& path,
                         const std::vector<std::string>& recents,
                         const std::unordered_set<std::string>& favorites) {
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    return;
  }
  for (const auto& entry : recents) {
    file << "R|" << escape_field(entry) << "\n";
  }
  std::vector<std::string> favs;
  favs.reserve(favorites.size());
  for (const auto& entry : favorites) {
    favs.push_back(entry);
  }
  std::sort(favs.begin(), favs.end());
  for (const auto& entry : favs) {
    file << "F|" << escape_field(entry) << "\n";
  }
}

struct UiState {
  std::optional<UiTheme> theme;
  std::optional<int> scale;
  std::optional<double> fps;
  std::optional<int> deadzone;
  std::optional<bool> show_help;
  std::optional<bool> audio;
  std::optional<bool> show_hud;
  std::optional<HudCorner> hud_corner;
  std::optional<bool> hud_compact;
  std::optional<int> hud_timeout;
  std::optional<VideoFilter> filter;
};

struct GlobalSettings {
  std::optional<int> scale_override;
  std::optional<double> fps_override;
  bool audio_enabled = true;
  bool cgb_color_correction = false;
  bool gba_color_correction = false;
  int deadzone = 16000;
  bool show_hud = true;
  HudCorner hud_corner = HudCorner::TopLeft;
  bool hud_compact = false;
  int hud_timeout_seconds = 0;
  VideoFilter filter = VideoFilter::None;
};

UiState load_ui_state(const std::string& path) {
  UiState state;
  std::ifstream file(path);
  if (!file) {
    return state;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    std::string key = to_lower(line.substr(0, pos));
    std::string value = line.substr(pos + 1);
    if (key == "ui_theme") {
      state.theme = parse_ui_theme(value);
    } else if (key == "scale") {
      try {
        state.scale = std::stoi(value);
      } catch (...) {
      }
    } else if (key == "fps") {
      try {
        state.fps = std::stod(value);
      } catch (...) {
      }
    } else if (key == "deadzone") {
      try {
        state.deadzone = std::stoi(value);
      } catch (...) {
      }
    } else if (key == "show_help") {
      auto parsed = parse_bool(value);
      if (parsed.has_value()) {
        state.show_help = parsed;
      }
    } else if (key == "audio") {
      auto parsed = parse_bool(value);
      if (parsed.has_value()) {
        state.audio = parsed;
      }
    } else if (key == "hud") {
      auto parsed = parse_bool(value);
      if (parsed.has_value()) {
        state.show_hud = parsed;
      }
    } else if (key == "hud_corner") {
      state.hud_corner = parse_hud_corner(value);
    } else if (key == "hud_compact") {
      auto parsed = parse_bool(value);
      if (parsed.has_value()) {
        state.hud_compact = parsed;
      }
    } else if (key == "hud_timeout") {
      try {
        state.hud_timeout = std::stoi(value);
      } catch (...) {
      }
    } else if (key == "filter") {
      state.filter = parse_video_filter(value);
    }
  }
  return state;
}

void save_ui_state(const std::string& path,
                   UiTheme theme,
                   int scale,
                   double fps,
                   int deadzone,
                   bool show_help,
                   bool audio_enabled,
                   bool show_hud,
                   HudCorner hud_corner,
                   bool hud_compact,
                   int hud_timeout_seconds,
                   VideoFilter filter) {
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    return;
  }
  file << "ui_theme=" << ui_theme_name(theme) << "\n";
  file << "scale=" << scale << "\n";
  file << "fps=" << fps << "\n";
  file << "deadzone=" << deadzone << "\n";
  file << "show_help=" << (show_help ? "true" : "false") << "\n";
  file << "audio=" << (audio_enabled ? "true" : "false") << "\n";
  file << "hud=" << (show_hud ? "true" : "false") << "\n";
  file << "hud_corner=" << hud_corner_name(hud_corner) << "\n";
  file << "hud_compact=" << (hud_compact ? "true" : "false") << "\n";
  file << "hud_timeout=" << hud_timeout_seconds << "\n";
  file << "filter=" << video_filter_name(filter) << "\n";
}

struct RomOverride {
  std::optional<int> scale;
  std::optional<double> fps;
  std::optional<int> deadzone;
  std::optional<bool> audio;
  std::optional<bool> cgb_color;
  std::optional<bool> show_hud;
  std::optional<HudCorner> hud_corner;
  std::optional<bool> hud_compact;
  std::optional<int> hud_timeout;
  std::optional<VideoFilter> filter;
};

RomOverride load_rom_override(const std::string& path) {
  RomOverride override;
  gbemu::common::Config config;
  std::string err;
  if (!config.load_file(path, &err)) {
    return override;
  }
  if (auto value = config.get_int("scale")) {
    override.scale = value;
  }
  if (auto value = config.get_double("fps")) {
    override.fps = value;
  }
  if (auto value = config.get_int("deadzone")) {
    override.deadzone = value;
  }
  std::string audio_value = config.get_string("audio", "");
  if (!audio_value.empty()) {
    override.audio = parse_bool(audio_value);
  }
  std::string cgb_value = config.get_string("cgb_color_correction", "");
  if (!cgb_value.empty()) {
    override.cgb_color = parse_bool(cgb_value);
  }
  std::string hud_value = config.get_string("hud", "");
  if (!hud_value.empty()) {
    override.show_hud = parse_bool(hud_value);
  }
  std::string hud_corner_value = config.get_string("hud_corner", "");
  if (!hud_corner_value.empty()) {
    override.hud_corner = parse_hud_corner(hud_corner_value);
  }
  std::string hud_compact_value = config.get_string("hud_compact", "");
  if (!hud_compact_value.empty()) {
    override.hud_compact = parse_bool(hud_compact_value);
  }
  if (auto timeout_value = config.get_int("hud_timeout")) {
    override.hud_timeout = timeout_value;
  }
  std::string filter_value = config.get_string("filter", "");
  if (!filter_value.empty()) {
    override.filter = parse_video_filter(filter_value);
  }
  return override;
}

void save_rom_override(const std::string& path,
                       int scale,
                       double fps,
                       int deadzone,
                       bool audio_enabled,
                       bool cgb_color,
                       bool show_hud,
                       HudCorner hud_corner,
                       bool hud_compact,
                       int hud_timeout_seconds,
                       VideoFilter filter) {
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    return;
  }
  file << "scale=" << scale << "\n";
  file << "fps=" << fps << "\n";
  file << "deadzone=" << deadzone << "\n";
  file << "audio=" << (audio_enabled ? "true" : "false") << "\n";
  file << "cgb_color_correction=" << (cgb_color ? "true" : "false") << "\n";
  file << "hud=" << (show_hud ? "true" : "false") << "\n";
  file << "hud_corner=" << hud_corner_name(hud_corner) << "\n";
  file << "hud_compact=" << (hud_compact ? "true" : "false") << "\n";
  file << "hud_timeout=" << hud_timeout_seconds << "\n";
  file << "filter=" << video_filter_name(filter) << "\n";
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

std::string trim_ascii(std::string_view value) {
  std::size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  if (start == value.size()) {
    return "";
  }
  std::size_t end = value.size() - 1;
  while (end > start && std::isspace(static_cast<unsigned char>(value[end]))) {
    --end;
  }
  return std::string(value.substr(start, end - start + 1));
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
  std::optional<UiTheme> ui_theme;
  std::optional<bool> hud;
  std::optional<VideoFilter> filter;
  std::optional<int> filter_workers;
  std::optional<HudCorner> hud_corner;
  std::optional<bool> hud_compact;
  std::optional<int> hud_timeout;
  bool gba_test = false;
  bool launcher = false;
  std::vector<std::string> rom_dirs;
  bool cpu_trace = false;
  bool boot_trace = false;
  bool gba_trace = false;
  int gba_trace_steps = 2000;
  bool gba_trace_io = true;
  bool gba_trace_after_rom = false;
  bool gba_fastboot = false;
  bool gba_auto_handoff = true;
  bool gba_hle_swi = false;
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
  bool headless = false;
  int headless_frames = 120;
  bool debug_window_overlay = false;
  bool cgb_color_correction = false;
  bool gba_color_correction = false;
  bool show_help_overlay = false;
  bool show_help = false;
};

void print_usage(const char* exe) {
  std::cout << "Usage: " << exe << " [options] <rom_file>\n";
  std::cout << "Options:\n";
  std::cout << "  --config <path>        Config file path (default: ./gbemu.conf if present)\n";
  std::cout << "  --renderer <sdl|vulkan>  Select rendering frontend\n";
  std::cout << "  --system <gb|gbc|gba>  Override system detection\n";
  std::cout << "  --fps <value>          Override target frame rate (0 to disable pacing)\n";
  std::cout << "  --filter <none|scanlines|lcd|crt>  Video filter\n";
  std::cout << "  --filter-workers <n>   CRT/filter worker threads (0-16, default auto 4-8)\n";
  std::cout << "  --scale <int>          Override window scale factor\n";
  std::cout << "  --video-driver <name>  Force SDL video driver (wayland or x11)\n";
  std::cout << "  --ui-theme <name>      UI theme: retro | minimal | deck\n";
  std::cout << "  --launcher             Start in launcher UI\n";
  std::cout << "  --rom-dir <path>       Add ROM scan directory (repeatable)\n";
  std::cout << "  --boot-rom <path>      Boot ROM path (applies to the current ROM)\n";
  std::cout << "  --boot-rom-gb <path>   Boot ROM path for GB\n";
  std::cout << "  --boot-rom-gbc <path>  Boot ROM path for GBC\n";
  std::cout << "  --boot-rom-gba <path>  Boot ROM path for GBA\n";
  std::cout << "  --gba-test             Use built-in GBA mode 3 test ROM\n";
  std::cout << "  --cpu-trace            Enable CPU trace buffer on faults\n";
  std::cout << "  --boot-trace           Log when boot ROM is disabled\n";
  std::cout << "  --gba-trace            Log GBA instructions + IO writes (short trace)\n";
  std::cout << "  --gba-trace-steps <n>  Instruction trace length (default: 2000)\n";
  std::cout << "  --gba-trace-after-rom  Start GBA trace after ROM entry\n";
  std::cout << "  --gba-fastboot         Skip GBA BIOS and jump straight to ROM\n";
  std::cout << "  --gba-no-auto-handoff  Disable auto handoff to ROM after POSTFLG\n";
  std::cout << "  --gba-hle-swi          HLE SWI 0x04/0x05/0x0B/0x0C (IntrWait/VBlankIntrWait/CPUSet/CPUFastSet)\n";
  std::cout << "  --gba-trace-assert     Log Butano assert file/line/function/expression\n";
  std::cout << "  --gba-bypass-assert    Bypass known SBTP bn_sprite_builder assert\n";
  std::cout << "  --gba-unimp <n>        Log first N unimplemented GBA opcodes\n";
  std::cout << "  --gba-video-io <n>     Log first N video IO writes\n";
  std::cout << "  --gba-io-read <n>      Log first N reads of key GBA IO regs\n";
  std::cout << "  --gba-swi <n>          Log first N GBA SWI calls\n";
  std::cout << "  --gba-watchdog <n>     Report hot PCs every N steps (0 disables)\n";
  std::cout << "  --gba-pc-watch <start> <end> <count>  Trace GBA PC range (hex or dec)\n";
  std::cout << "  --gba-mem-watch <start> <end> <count>  Watch GBA memory range (hex or dec)\n";
  std::cout << "  --gba-mem-watch-read   Watch reads only (use with --gba-mem-watch)\n";
  std::cout << "  --gba-mem-watch-write  Watch writes only (use with --gba-mem-watch)\n";
  std::cout << "  --gba-auto-patch-hang  Auto patch tight ROM loops (debug)\n";
  std::cout << "  --gba-auto-patch-threshold <n>  Loop iterations before patch (default: 50000)\n";
  std::cout << "  --gba-auto-patch-span <bytes>   Max backward branch span (default: 64)\n";
  std::cout << "  --gba-auto-patch-range <start> <end>  Limit auto patch to ROM range\n";
  std::cout << "  --headless             Run without SDL window\n";
  std::cout << "  --frames <count>       Frames to run in headless mode (default: 120)\n";
  std::cout << "  --debug-window         Draw window border overlay\n";
  std::cout << "  --cgb-color-correct    Apply simple CGB color correction\n";
  std::cout << "  --gba-color-correct    Apply simple GBA color correction\n";
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

  std::string ui_theme_value = config.get_string("ui_theme", "");
  if (!ui_theme_value.empty()) {
    auto parsed = parse_ui_theme(ui_theme_value);
    if (parsed.has_value()) {
      options->ui_theme = parsed;
    } else {
      std::cout << "Config warning: invalid ui_theme value '" << ui_theme_value << "'\n";
    }
  }

  std::string launcher_value = config.get_string("launcher", "");
  if (!launcher_value.empty()) {
    auto parsed = parse_bool(launcher_value);
    if (parsed.has_value()) {
      options->launcher = *parsed;
    } else {
      std::cout << "Config warning: invalid launcher value '" << launcher_value << "'\n";
    }
  }

  std::string rom_dirs_value = config.get_string("rom_dirs", "");
  if (!rom_dirs_value.empty()) {
    auto dirs = split_list(rom_dirs_value);
    for (const auto& dir : dirs) {
      if (!dir.empty()) {
        options->rom_dirs.push_back(dir);
      }
    }
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

  std::string gba_trace_value = config.get_string("gba_trace", "");
  if (!gba_trace_value.empty()) {
    auto parsed = parse_bool(gba_trace_value);
    if (parsed.has_value()) {
      options->gba_trace = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_trace value '" << gba_trace_value << "'\n";
    }
  }

  std::string gba_trace_after_rom_value = config.get_string("gba_trace_after_rom", "");
  if (!gba_trace_after_rom_value.empty()) {
    auto parsed = parse_bool(gba_trace_after_rom_value);
    if (parsed.has_value()) {
      options->gba_trace_after_rom = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_trace_after_rom value '"
                << gba_trace_after_rom_value << "'\n";
    }
  }

  std::string gba_fastboot_value = config.get_string("gba_fastboot", "");
  if (!gba_fastboot_value.empty()) {
    auto parsed = parse_bool(gba_fastboot_value);
    if (parsed.has_value()) {
      options->gba_fastboot = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_fastboot value '" << gba_fastboot_value << "'\n";
    }
  }

  std::string gba_auto_handoff_value = config.get_string("gba_auto_handoff", "");
  if (!gba_auto_handoff_value.empty()) {
    auto parsed = parse_bool(gba_auto_handoff_value);
    if (parsed.has_value()) {
      options->gba_auto_handoff = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_auto_handoff value '"
                << gba_auto_handoff_value << "'\n";
    }
  }

  std::string gba_hle_swi_value = config.get_string("gba_hle_swi", "");
  if (!gba_hle_swi_value.empty()) {
    auto parsed = parse_bool(gba_hle_swi_value);
    if (parsed.has_value()) {
      options->gba_hle_swi = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_hle_swi value '" << gba_hle_swi_value << "'\n";
    }
  }

  std::string gba_trace_assert_value = config.get_string("gba_trace_assert", "");
  if (!gba_trace_assert_value.empty()) {
    auto parsed = parse_bool(gba_trace_assert_value);
    if (parsed.has_value()) {
      options->gba_trace_assert = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_trace_assert value '"
                << gba_trace_assert_value << "'\n";
    }
  }

  std::string gba_bypass_assert_value = config.get_string("gba_bypass_assert", "");
  if (!gba_bypass_assert_value.empty()) {
    auto parsed = parse_bool(gba_bypass_assert_value);
    if (parsed.has_value()) {
      options->gba_bypass_assert = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_bypass_assert value '"
                << gba_bypass_assert_value << "'\n";
    }
  }

  if (auto steps = config.get_int("gba_trace_steps")) {
    if (*steps >= 0) {
      options->gba_trace_steps = *steps;
    }
  }

  if (auto unimp_limit = config.get_int("gba_unimp_limit")) {
    if (*unimp_limit >= 0) {
      options->gba_unimp_limit = *unimp_limit;
    }
  }

  if (auto watch_video = config.get_int("gba_video_io")) {
    if (*watch_video >= 0) {
      options->gba_watch_video_io = *watch_video;
    }
  }

  if (auto io_read_watch = config.get_int("gba_io_read")) {
    if (*io_read_watch >= 0) {
      options->gba_io_read_watch = *io_read_watch;
    }
  }

  if (auto swi_limit = config.get_int("gba_swi_limit")) {
    if (*swi_limit >= 0) {
      options->gba_swi_limit = *swi_limit;
    }
  }

  if (auto watchdog_steps = config.get_int("gba_watchdog_steps")) {
    if (*watchdog_steps >= 0) {
      options->gba_watchdog_steps = *watchdog_steps;
    }
  }

  if (auto mem_watch_start = config.get_int("gba_mem_watch_start")) {
    if (*mem_watch_start >= 0) {
      options->gba_mem_watch_start = static_cast<std::uint32_t>(*mem_watch_start);
    }
  }
  if (auto mem_watch_end = config.get_int("gba_mem_watch_end")) {
    if (*mem_watch_end >= 0) {
      options->gba_mem_watch_end = static_cast<std::uint32_t>(*mem_watch_end);
    }
  }
  if (auto mem_watch_count = config.get_int("gba_mem_watch_count")) {
    if (*mem_watch_count >= 0) {
      options->gba_mem_watch_count = *mem_watch_count;
    }
  }
  std::string mem_watch_read_value = config.get_string("gba_mem_watch_read", "");
  if (!mem_watch_read_value.empty()) {
    auto parsed = parse_bool(mem_watch_read_value);
    if (parsed.has_value()) {
      options->gba_mem_watch_reads = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_mem_watch_read value '"
                << mem_watch_read_value << "'\n";
    }
  }
  std::string mem_watch_write_value = config.get_string("gba_mem_watch_write", "");
  if (!mem_watch_write_value.empty()) {
    auto parsed = parse_bool(mem_watch_write_value);
    if (parsed.has_value()) {
      options->gba_mem_watch_writes = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_mem_watch_write value '"
                << mem_watch_write_value << "'\n";
    }
  }
  std::string auto_patch_value = config.get_string("gba_auto_patch_hang", "");
  if (!auto_patch_value.empty()) {
    auto parsed = parse_bool(auto_patch_value);
    if (parsed.has_value()) {
      options->gba_auto_patch_hang = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_auto_patch_hang value '"
                << auto_patch_value << "'\n";
    }
  }
  if (auto auto_patch_threshold = config.get_int("gba_auto_patch_threshold")) {
    if (*auto_patch_threshold >= 0) {
      options->gba_auto_patch_threshold = *auto_patch_threshold;
    }
  }
  if (auto auto_patch_span = config.get_int("gba_auto_patch_span")) {
    if (*auto_patch_span >= 0) {
      options->gba_auto_patch_span = static_cast<std::uint32_t>(*auto_patch_span);
    }
  }
  if (auto auto_patch_start = config.get_int("gba_auto_patch_start")) {
    if (*auto_patch_start >= 0) {
      options->gba_auto_patch_start = static_cast<std::uint32_t>(*auto_patch_start);
    }
  }
  if (auto auto_patch_end = config.get_int("gba_auto_patch_end")) {
    if (*auto_patch_end >= 0) {
      options->gba_auto_patch_end = static_cast<std::uint32_t>(*auto_patch_end);
    }
  }

  std::string gba_trace_io_value = config.get_string("gba_trace_io", "");
  if (!gba_trace_io_value.empty()) {
    auto parsed = parse_bool(gba_trace_io_value);
    if (parsed.has_value()) {
      options->gba_trace_io = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_trace_io value '" << gba_trace_io_value << "'\n";
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

  std::string gba_color_value = config.get_string("gba_color_correction", "");
  if (!gba_color_value.empty()) {
    auto parsed = parse_bool(gba_color_value);
    if (parsed.has_value()) {
      options->gba_color_correction = *parsed;
    } else {
      std::cout << "Config warning: invalid gba_color_correction value '" << gba_color_value << "'\n";
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

  std::string hud_value = config.get_string("hud", "");
  if (!hud_value.empty()) {
    auto parsed = parse_bool(hud_value);
    if (parsed.has_value()) {
      options->hud = *parsed;
    } else {
      std::cout << "Config warning: invalid hud value '" << hud_value << "'\n";
    }
  }

  std::string filter_value = config.get_string("filter", "");
  if (!filter_value.empty()) {
    auto parsed = parse_video_filter(filter_value);
    if (parsed.has_value()) {
      options->filter = *parsed;
    } else {
      std::cout << "Config warning: invalid filter value '" << filter_value << "'\n";
    }
  }

  if (auto workers = config.get_int("filter_workers")) {
    if (*workers >= 0) {
      options->filter_workers = *workers;
    }
  }

  std::string hud_corner_value = config.get_string("hud_corner", "");
  if (!hud_corner_value.empty()) {
    auto parsed = parse_hud_corner(hud_corner_value);
    if (parsed.has_value()) {
      options->hud_corner = *parsed;
    } else {
      std::cout << "Config warning: invalid hud_corner value '" << hud_corner_value << "'\n";
    }
  }

  std::string hud_compact_value = config.get_string("hud_compact", "");
  if (!hud_compact_value.empty()) {
    auto parsed = parse_bool(hud_compact_value);
    if (parsed.has_value()) {
      options->hud_compact = *parsed;
    } else {
      std::cout << "Config warning: invalid hud_compact value '" << hud_compact_value << "'\n";
    }
  }

  if (auto timeout_value = config.get_int("hud_timeout")) {
    options->hud_timeout = *timeout_value;
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

double default_fps(gbemu::core::System system) {
  if (system == gbemu::core::System::GBA) {
    return 60.0;
  }
  return 59.7275;
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
  // Default to 4-8 worker threads to leave CPU for emulation + main/render thread.
  unsigned workers = std::max(4u, hw_threads / 2);
  return std::clamp(workers, 1u, 8u);
}

unsigned sanitize_filter_workers(int requested) {
  return std::clamp(requested, 0, 16);
}

void apply_crt_filter(const std::uint32_t* src,
                      int src_stride_words,
                      std::uint32_t* dst,
                      std::uint32_t* prev,
                      int width,
                      int height,
                      bool reset,
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
        dst[dst_row + x] = 0xFF000000u |
                           (static_cast<std::uint32_t>(out_r) << 16) |
                           (static_cast<std::uint32_t>(out_g) << 8) |
                           static_cast<std::uint32_t>(out_b);
        prev[dst_row + x] = src_pixel;
      }
    }
  });
}

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
  constexpr Uint32 kInitFlags = SDL_INIT_VIDEO | SDL_INIT_EVENTS |
                                SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
  if (driver_override.has_value()) {
    std::string preferred = to_lower(*driver_override);
    if (preferred == "wayland") {
      SDL_setenv("SDL_VIDEODRIVER", "wayland", 1);
      if (SDL_Init(kInitFlags) == 0) {
        return true;
      }
      std::string wayland_error = SDL_GetError();
      SDL_Quit();
      SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
      if (SDL_Init(kInitFlags) == 0) {
        std::cout << "Wayland init failed: " << wayland_error << ". Using X11 fallback.\n";
        return true;
      }
      std::cout << "SDL init failed for Wayland (" << wayland_error << ") and X11 ("
                << SDL_GetError() << ").\n";
      return false;
    }
    if (preferred == "x11") {
      SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
      if (SDL_Init(kInitFlags) == 0) {
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
    if (SDL_Init(kInitFlags) == 0) {
      return true;
    }
    std::cout << "SDL init failed with SDL_VIDEODRIVER=" << existing << ": "
              << SDL_GetError() << "\n";
    return false;
  }

  SDL_setenv("SDL_VIDEODRIVER", "wayland", 1);
  if (SDL_Init(kInitFlags) == 0) {
    return true;
  }
  std::string wayland_error = SDL_GetError();
  SDL_Quit();

  SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
  if (SDL_Init(kInitFlags) == 0) {
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
  std::string renderer_backend = "sdl";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else if (arg == "--renderer") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --renderer\n";
        return 1;
      }
      renderer_backend = argv[++i];
      if (renderer_backend != "sdl" && renderer_backend != "vulkan") {
        std::cout << "Invalid renderer value: " << renderer_backend << "\n";
        return 1;
      }
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
    } else if (arg == "--gba-test") {
      options.gba_test = true;
    } else if (arg == "--cpu-trace") {
      options.cpu_trace = true;
    } else if (arg == "--boot-trace") {
      options.boot_trace = true;
    } else if (arg == "--gba-trace") {
      options.gba_trace = true;
    } else if (arg == "--gba-trace-after-rom") {
      options.gba_trace_after_rom = true;
    } else if (arg == "--gba-fastboot") {
      options.gba_fastboot = true;
    } else if (arg == "--gba-no-auto-handoff") {
      options.gba_auto_handoff = false;
    } else if (arg == "--gba-hle-swi") {
      options.gba_hle_swi = true;
    } else if (arg == "--gba-trace-assert") {
      options.gba_trace_assert = true;
    } else if (arg == "--gba-bypass-assert") {
      options.gba_bypass_assert = true;
    } else if (arg == "--gba-trace-steps") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-trace-steps\n";
        return 1;
      }
      options.gba_trace_steps = std::max(0, std::stoi(argv[++i]));
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
        std::cout << "Missing values for --gba-pc-watch\n";
        return 1;
      }
      try {
        options.gba_pc_watch_start = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_pc_watch_end = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_pc_watch_count = std::max(0, std::stoi(argv[++i]));
      } catch (...) {
        std::cout << "Invalid --gba-pc-watch values\n";
        return 1;
      }
    } else if (arg == "--gba-mem-watch") {
      if (i + 3 >= argc) {
        std::cout << "Missing values for --gba-mem-watch\n";
        return 1;
      }
      try {
        options.gba_mem_watch_start = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_mem_watch_end = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_mem_watch_count = std::max(0, std::stoi(argv[++i]));
      } catch (...) {
        std::cout << "Invalid --gba-mem-watch values\n";
        return 1;
      }
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
      options.gba_auto_patch_threshold = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-auto-patch-span") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-auto-patch-span\n";
        return 1;
      }
      options.gba_auto_patch_span = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0));
    } else if (arg == "--gba-auto-patch-range") {
      if (i + 2 >= argc) {
        std::cout << "Missing values for --gba-auto-patch-range\n";
        return 1;
      }
      try {
        options.gba_auto_patch_start = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_auto_patch_end = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
      } catch (...) {
        std::cout << "Invalid --gba-auto-patch-range values\n";
        return 1;
      }
    } else if (arg == "--gba-trace-no-io") {
      options.gba_trace_io = false;
    } else if (arg == "--ui-theme") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --ui-theme\n";
        return 1;
      }
      auto parsed = parse_ui_theme(argv[++i]);
      if (!parsed.has_value()) {
        std::cout << "Invalid ui-theme value: " << argv[i] << "\n";
        return 1;
      }
      options.ui_theme = parsed;
    } else if (arg == "--launcher") {
      options.launcher = true;
    } else if (arg == "--rom-dir") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --rom-dir\n";
        return 1;
      }
      options.rom_dirs.emplace_back(argv[++i]);
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
    } else if (arg == "--gba-color-correct") {
      options.gba_color_correction = true;
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
    } else if (arg == "--renderer") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --renderer\n";
        return 1;
      }
      renderer_backend = argv[++i];
      if (renderer_backend != "sdl" && renderer_backend != "vulkan") {
        std::cout << "Invalid renderer value: " << renderer_backend << "\n";
        return 1;
      }
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
    } else if (arg == "--gba-test") {
      options.gba_test = true;
    } else if (arg == "--cpu-trace") {
      options.cpu_trace = true;
    } else if (arg == "--boot-trace") {
      options.boot_trace = true;
    } else if (arg == "--gba-trace") {
      options.gba_trace = true;
    } else if (arg == "--gba-trace-after-rom") {
      options.gba_trace_after_rom = true;
    } else if (arg == "--gba-fastboot") {
      options.gba_fastboot = true;
    } else if (arg == "--gba-no-auto-handoff") {
      options.gba_auto_handoff = false;
    } else if (arg == "--gba-hle-swi") {
      options.gba_hle_swi = true;
    } else if (arg == "--gba-trace-assert") {
      options.gba_trace_assert = true;
    } else if (arg == "--gba-bypass-assert") {
      options.gba_bypass_assert = true;
    } else if (arg == "--gba-trace-steps") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-trace-steps\n";
        return 1;
      }
      options.gba_trace_steps = std::max(0, std::stoi(argv[++i]));
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
        std::cout << "Missing values for --gba-pc-watch\n";
        return 1;
      }
      try {
        options.gba_pc_watch_start = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_pc_watch_end = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_pc_watch_count = std::max(0, std::stoi(argv[++i]));
      } catch (...) {
        std::cout << "Invalid --gba-pc-watch values\n";
        return 1;
      }
    } else if (arg == "--gba-mem-watch") {
      if (i + 3 >= argc) {
        std::cout << "Missing values for --gba-mem-watch\n";
        return 1;
      }
      try {
        options.gba_mem_watch_start = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_mem_watch_end = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_mem_watch_count = std::max(0, std::stoi(argv[++i]));
      } catch (...) {
        std::cout << "Invalid --gba-mem-watch values\n";
        return 1;
      }
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
      options.gba_auto_patch_threshold = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--gba-auto-patch-span") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --gba-auto-patch-span\n";
        return 1;
      }
      options.gba_auto_patch_span = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0));
    } else if (arg == "--gba-auto-patch-range") {
      if (i + 2 >= argc) {
        std::cout << "Missing values for --gba-auto-patch-range\n";
        return 1;
      }
      try {
        options.gba_auto_patch_start = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
        options.gba_auto_patch_end = static_cast<std::uint32_t>(
            std::stoul(argv[++i], nullptr, 0));
      } catch (...) {
        std::cout << "Invalid --gba-auto-patch-range values\n";
        return 1;
      }
    } else if (arg == "--gba-trace-no-io") {
      options.gba_trace_io = false;
    } else if (arg == "--ui-theme") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --ui-theme\n";
        return 1;
      }
      auto parsed = parse_ui_theme(argv[++i]);
      if (!parsed.has_value()) {
        std::cout << "Invalid ui-theme value: " << argv[i] << "\n";
        return 1;
      }
      options.ui_theme = parsed;
    } else if (arg == "--launcher") {
      options.launcher = true;
    } else if (arg == "--rom-dir") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --rom-dir\n";
        return 1;
      }
      options.rom_dirs.emplace_back(argv[++i]);
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
    } else if (arg == "--gba-color-correct") {
      options.gba_color_correction = true;
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
    } else if (arg == "--filter") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --filter\n";
        return 1;
      }
      std::string value = argv[++i];
      auto parsed = parse_video_filter(value);
      if (!parsed.has_value()) {
        std::cout << "Invalid filter value: " << value << "\n";
        return 1;
      }
      options.filter = parsed;
    } else if (arg == "--filter-workers") {
      if (i + 1 >= argc) {
        std::cout << "Missing value for --filter-workers\n";
        return 1;
      }
      std::string value = argv[++i];
      try {
        options.filter_workers = std::stoi(value);
      } catch (...) {
        std::cout << "Invalid filter-workers value: " << value << "\n";
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

  if (renderer_backend == "vulkan") {
    return gbemu::frontend::run_vulkan_frontend(argc, argv);
  }

  bool launcher_enabled = options.launcher;
  if (launcher_enabled && options.headless) {
    std::cout << "Launcher requires a window; disabling headless mode.\n";
    options.headless = false;
  }
  if (options.rom_path.empty() && !launcher_enabled && !options.gba_test) {
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

  if (options.filter_workers && (*options.filter_workers < 0 || *options.filter_workers > 16)) {
    std::cout << "filter-workers must be in range 0..16\n";
    return 1;
  }

  if (options.headless_frames <= 0) {
    std::cout << "Frames must be > 0\n";
    return 1;
  }

  gbemu::common::InputConfig input_config;
  input_config.set_default();
  if (!config_path.empty()) {
    gbemu::common::Config config_for_inputs;
    std::string err;
    config_for_inputs.load_file(config_path, &err);
    input_config.load_from_config(config_for_inputs.values());
  }

  UiTheme ui_theme = options.ui_theme.value_or(UiTheme::Retro);
  int theme_toast_frames = 0;
  bool show_help_pref = options.show_help_overlay;
  bool show_hud_pref = options.hud.value_or(true);
  VideoFilter video_filter = options.filter.value_or(VideoFilter::None);
  HudCorner hud_corner = options.hud_corner.value_or(HudCorner::TopLeft);
  bool hud_compact = options.hud_compact.value_or(false);
  int hud_timeout_seconds = options.hud_timeout.value_or(0);
  if (hud_timeout_seconds < 0) {
    hud_timeout_seconds = 0;
  }
  bool show_hud = show_hud_pref;
  bool crt_reset = true;

  std::filesystem::path save_dir("saves");
  std::error_code save_ec;
  std::filesystem::create_directories(save_dir, save_ec);
  std::filesystem::path launcher_state_path = save_dir / "launcher_state.txt";
  std::filesystem::path ui_state_path = save_dir / "ui_state.conf";
  std::filesystem::path input_map_path = save_dir / "input_map.conf";
  std::filesystem::path input_map_dir = save_dir / "input_map";
  std::filesystem::path active_input_map_path = input_map_path;
  std::filesystem::path rom_override_path;
  std::filesystem::path rom_input_map_path;
  bool rom_override_active = false;
  GlobalSettings global_settings;
  SDL_AudioDeviceID audio_device = 0;
  bool audio_enabled = true;

  if (std::filesystem::exists(input_map_path)) {
    gbemu::common::Config config_override;
    std::string err;
    if (config_override.load_file(input_map_path.string(), &err)) {
      input_config.load_from_config(config_override.values());
    }
  }

  auto reload_input_config = [&](const std::filesystem::path& map_path) {
    input_config.set_default();
    if (!config_path.empty()) {
      gbemu::common::Config config_for_inputs;
      std::string err;
      config_for_inputs.load_file(config_path, &err);
      input_config.load_from_config(config_for_inputs.values());
    }
    if (!map_path.empty() && std::filesystem::exists(map_path)) {
      gbemu::common::Config config_override;
      std::string err;
      if (config_override.load_file(map_path.string(), &err)) {
        input_config.load_from_config(config_override.values());
      }
    }
  };

  auto rom_override_path_for = [&](const std::string& path) {
    std::filesystem::path rom_path = std::filesystem::path(path).filename();
    std::filesystem::path save_base = save_dir / rom_path.stem();
    std::filesystem::path override_path = save_base;
    override_path += ".override";
    return override_path;
  };

  auto rom_input_map_path_for = [&](const std::string& path) {
    std::filesystem::path rom_path = std::filesystem::path(path).filename();
    return input_map_dir / (rom_path.stem().string() + ".conf");
  };

  auto ensure_rom_input_map = [&](const std::string& path) {
    std::filesystem::path map_path = rom_input_map_path_for(path);
    if (map_path.empty() || std::filesystem::exists(map_path)) {
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(input_map_dir, ec);
    if (std::filesystem::exists(input_map_path)) {
      std::filesystem::copy_file(input_map_path, map_path,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (!ec) {
        return;
      }
    }
    gbemu::common::InputConfig base;
    base.set_default();
    if (!config_path.empty()) {
      gbemu::common::Config config_for_inputs;
      std::string err;
      config_for_inputs.load_file(config_path, &err);
      base.load_from_config(config_for_inputs.values());
    }
    std::ofstream file(map_path.string(), std::ios::trunc);
    if (file) {
      base.write_config(file);
    }
  };

  auto rom_override_exists = [&](const std::string& path) {
    return std::filesystem::exists(rom_override_path_for(path));
  };

  std::vector<std::string> recent_paths;
  std::unordered_set<std::string> favorite_paths;
  load_launcher_state(launcher_state_path.string(), &recent_paths, &favorite_paths);

  UiState ui_state = load_ui_state(ui_state_path.string());
  if (!options.ui_theme.has_value() && ui_state.theme.has_value()) {
    ui_theme = *ui_state.theme;
  }
  if (!options.scale_override.has_value() && ui_state.scale.has_value()) {
    options.scale_override = *ui_state.scale;
  }
  if (!options.fps_override.has_value() && ui_state.fps.has_value()) {
    options.fps_override = *ui_state.fps;
  }
  if (ui_state.deadzone.has_value()) {
    input_config.set_axis_deadzone(*ui_state.deadzone);
  }
  if (ui_state.show_help.has_value()) {
    show_help_pref = *ui_state.show_help;
  }
  if (ui_state.show_hud.has_value()) {
    show_hud_pref = *ui_state.show_hud;
  }
  if (ui_state.hud_corner.has_value()) {
    hud_corner = *ui_state.hud_corner;
  }
  if (ui_state.hud_compact.has_value()) {
    hud_compact = *ui_state.hud_compact;
  }
  if (ui_state.hud_timeout.has_value()) {
    hud_timeout_seconds = std::max(0, *ui_state.hud_timeout);
  }
  if (ui_state.filter.has_value()) {
    video_filter = *ui_state.filter;
  }
  show_hud = show_hud_pref;
  std::optional<bool> audio_pref = ui_state.audio;
  std::filesystem::path save_path;
  std::filesystem::path rtc_path;
  std::filesystem::path state_path;
  std::vector<std::uint8_t> rom;
  std::string current_rom_path;
  std::string current_rom_title;
  gbemu::core::System system = gbemu::core::System::GB;
  std::uint8_t key_state = 0xFF;
  std::uint8_t pad_state = 0xFF;
  std::uint8_t joypad_state = 0xFF;
  bool game_loaded = false;
  bool boot_rom_last = false;
  bool debug_window_overlay = options.debug_window_overlay;
  bool cgb_color_correction = options.cgb_color_correction;
  bool gba_color_correction = options.gba_color_correction;
  std::string launcher_error;
  std::mutex core_mutex;
  bool emu_thread_started = false;
  std::atomic<bool> emu_boot_enabled{false};
  std::atomic<std::uint16_t> emu_boot_pc{0};
  std::atomic<std::uint8_t> emu_boot_opcode{0};

  auto log_boot_state = [&](long long frame) {
    if (!options.boot_trace || !game_loaded) {
      return;
    }
    bool now = false;
    std::uint16_t pc = 0;
    std::uint8_t opcode = 0;
    if (emu_thread_started) {
      now = emu_boot_enabled.load(std::memory_order_relaxed);
      pc = emu_boot_pc.load(std::memory_order_relaxed);
      opcode = emu_boot_opcode.load(std::memory_order_relaxed);
    } else {
      std::scoped_lock lock(core_mutex);
      now = core.boot_rom_enabled();
      pc = core.cpu_pc();
      opcode = core.cpu_opcode();
    }
    if (boot_rom_last && !now) {
      std::cout << "Boot ROM disabled at frame " << frame
                << " PC=0x" << std::hex << std::setw(4) << std::setfill('0')
                << pc << " opcode=0x" << std::setw(2)
                << static_cast<int>(opcode) << std::dec << "\n";
    } else if (now && (frame % 120 == 0)) {
      std::cout << "Boot ROM still enabled at frame " << frame
                << " PC=0x" << std::hex << std::setw(4) << std::setfill('0')
                << pc << " opcode=0x" << std::setw(2)
                << static_cast<int>(opcode) << std::dec << "\n";
    }
    boot_rom_last = now;
  };

  auto load_game = [&](const std::string& path, std::string* out_error) -> bool {
    std::vector<std::uint8_t> data;
    std::string error;
    std::string resolved_path = path;
    if (options.gba_test) {
      data = build_gba_mode3_test_rom();
      resolved_path = "__gba_test__";
    } else {
      if (!gbemu::common::read_file(path, &data, &error)) {
        if (out_error) {
          *out_error = "Failed to read ROM: " + path + ". " + error;
        }
        return false;
      }
    }

    rom = std::move(data);
    current_rom_path = resolved_path;
    std::cout << "ROM size: " << rom.size() << " bytes\n";

    gbemu::core::System detected_system = detect_system(rom);
    if (options.gba_test) {
      system = gbemu::core::System::GBA;
    } else {
      system = options.system_override.value_or(detected_system);
    }
    core.set_system(system);

    std::vector<std::uint8_t> boot_rom;
    std::string boot_error;
    if (!load_boot_rom(system, options, &boot_rom, &boot_error)) {
      if (out_error) {
        *out_error = "Boot ROM required for " + system_name(system) +
                     " but not found. " + boot_error;
      }
      return false;
    }

    std::string load_error;
    if (!core.load_rom(rom, boot_rom, &load_error)) {
      if (out_error) {
        *out_error = "Failed to load ROM into core: " + load_error;
      }
      return false;
    }
    if (system == gbemu::core::System::GBA) {
      std::cout << "GBA core: early implementation (not all games boot yet).\n";
    }

    core.set_gba_auto_handoff(options.gba_auto_handoff);
    core.set_gba_fastboot(options.gba_fastboot);
    core.set_gba_hle_swi(options.gba_hle_swi);
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
    core.set_debug_window_overlay(debug_window_overlay);
    core.set_cgb_color_correction(cgb_color_correction);
    core.set_gba_color_correction(gba_color_correction);
    key_state = 0xFF;
    pad_state = 0xFF;
    joypad_state = 0xFF;
    core.set_joypad_state(joypad_state);

    std::filesystem::path rom_path = std::filesystem::path(path).filename();
    std::filesystem::path save_base = save_dir / rom_path.stem();
    save_path = save_base;
    save_path += ".sav";
    rtc_path = save_base;
    rtc_path += ".rtc";
    state_path = save_base;
    state_path += ".state";
    rom_override_path = save_base;
    rom_override_path += ".override";
    rom_input_map_path = input_map_dir / (rom_path.stem().string() + ".conf");

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

    if (system == gbemu::core::System::GBA) {
      print_gba_header(rom);
    } else if (rom.size() >= 0x150) {
      print_gb_header(rom);
    } else {
      std::cout << "Unknown or too-small ROM.\n";
    }
    current_rom_title = rom_title_from_data(rom, system);
    if (current_rom_title.empty()) {
      current_rom_title = rom_path.stem().string();
    }

    if (!rom_override_path.empty() && std::filesystem::exists(rom_override_path)) {
      rom_override_active = true;
      RomOverride override = load_rom_override(rom_override_path.string());
      if (override.scale.has_value()) {
        options.scale_override = *override.scale;
      } else {
        options.scale_override = global_settings.scale_override;
      }
      if (override.fps.has_value()) {
        options.fps_override = *override.fps;
      } else {
        options.fps_override = global_settings.fps_override;
      }
      if (override.deadzone.has_value()) {
        input_config.set_axis_deadzone(*override.deadzone);
      } else {
        input_config.set_axis_deadzone(global_settings.deadzone);
      }
      if (override.cgb_color.has_value()) {
        cgb_color_correction = *override.cgb_color;
      } else {
        cgb_color_correction = global_settings.cgb_color_correction;
      }
      core.set_cgb_color_correction(cgb_color_correction);
      if (override.audio.has_value()) {
        audio_enabled = *override.audio;
        if (audio_device != 0) {
          SDL_PauseAudioDevice(audio_device, audio_enabled ? 0 : 1);
          if (!audio_enabled) {
            SDL_ClearQueuedAudio(audio_device);
          }
        }
      } else {
        audio_enabled = global_settings.audio_enabled;
      }
      if (override.show_hud.has_value()) {
        show_hud = *override.show_hud;
      } else {
        show_hud = global_settings.show_hud;
      }
      if (override.hud_corner.has_value()) {
        hud_corner = *override.hud_corner;
      } else {
        hud_corner = global_settings.hud_corner;
      }
      if (override.hud_compact.has_value()) {
        hud_compact = *override.hud_compact;
      } else {
        hud_compact = global_settings.hud_compact;
      }
      if (override.hud_timeout.has_value()) {
        hud_timeout_seconds = std::max(0, *override.hud_timeout);
      } else {
        hud_timeout_seconds = global_settings.hud_timeout_seconds;
      }
      if (override.filter.has_value()) {
        video_filter = *override.filter;
      } else {
        video_filter = global_settings.filter;
      }
      if (!rom_input_map_path.empty() && std::filesystem::exists(rom_input_map_path)) {
        active_input_map_path = rom_input_map_path;
      } else {
        active_input_map_path = input_map_path;
      }
      reload_input_config(active_input_map_path);
    } else {
      rom_override_active = false;
      options.scale_override = global_settings.scale_override;
      options.fps_override = global_settings.fps_override;
      input_config.set_axis_deadzone(global_settings.deadzone);
      cgb_color_correction = global_settings.cgb_color_correction;
      core.set_cgb_color_correction(cgb_color_correction);
      audio_enabled = global_settings.audio_enabled;
      if (audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, audio_enabled ? 0 : 1);
        if (!audio_enabled) {
          SDL_ClearQueuedAudio(audio_device);
        }
      }
      show_hud = global_settings.show_hud;
      hud_corner = global_settings.hud_corner;
      hud_compact = global_settings.hud_compact;
      hud_timeout_seconds = global_settings.hud_timeout_seconds;
      video_filter = global_settings.filter;
      active_input_map_path = input_map_path;
      reload_input_config(active_input_map_path);
    }
    crt_reset = true;

    boot_rom_last = core.boot_rom_enabled();
    if (options.boot_trace) {
      std::cout << "Boot ROM enabled: " << (boot_rom_last ? "yes" : "no") << "\n";
    }

    if (!resolved_path.empty()) {
      recent_paths.erase(std::remove(recent_paths.begin(), recent_paths.end(), resolved_path),
                         recent_paths.end());
      recent_paths.insert(recent_paths.begin(), resolved_path);
      if (recent_paths.size() > 10) {
        recent_paths.resize(10);
      }
      save_launcher_state(launcher_state_path.string(), recent_paths, favorite_paths);
    }

    game_loaded = true;
    return true;
  };

  auto dump_cpu_fault = [&]() {
    std::scoped_lock lock(core_mutex);
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

  auto save_state = [&]() {
    if (!game_loaded) {
      return;
    }
    std::scoped_lock lock(core_mutex);
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

  auto save_input_map = [&]() {
    std::ofstream file(active_input_map_path.string(), std::ios::trunc);
    if (!file) {
      return;
    }
    input_config.write_config(file);
  };

  auto save_full_state = [&]() {
    if (!game_loaded) {
      return;
    }
    std::vector<std::uint8_t> state;
    {
      std::scoped_lock lock(core_mutex);
      if (!core.save_state(&state)) {
        std::cout << "Failed to build save state\n";
        return;
      }
    }
    std::string save_error;
    if (gbemu::common::write_file(state_path.string(), state, &save_error)) {
      std::cout << "Saved state to " << state_path.string() << "\n";
    } else {
      std::cout << "Failed to save state: " << save_error << "\n";
    }
  };

  auto load_full_state = [&]() {
    if (!game_loaded) {
      return;
    }
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
  };

  if (!launcher_enabled && (options.gba_test || !options.rom_path.empty())) {
    std::string err;
    std::string path = options.gba_test ? "__gba_test__" : options.rom_path;
    if (!load_game(path, &err)) {
      std::cout << err << "\n";
      return 1;
    }
  }

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

  int fb_width = game_loaded ? core.framebuffer_width() : 240;
  int fb_height = game_loaded ? core.framebuffer_height() : 160;
  int scale = options.scale_override.value_or(default_scale(system));
  if (!game_loaded && !options.scale_override.has_value()) {
    scale = 4;
  }

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
  std::vector<std::uint32_t> filtered_frame;
  std::vector<std::uint32_t> prev_frame;
  auto resize_filter_buffers = [&]() {
    std::size_t count = static_cast<std::size_t>(fb_width) * fb_height;
    filtered_frame.assign(count, 0);
    prev_frame.assign(count, 0);
    crt_reset = true;
  };
  resize_filter_buffers();

  SDL_AudioSpec want = {};
  SDL_AudioSpec have = {};
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    std::cout << "Failed to init audio subsystem: " << SDL_GetError() << "\n";
    audio_enabled = false;
  } else {
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  }
  if (audio_device != 0) {
    SDL_PauseAudioDevice(audio_device, 0);
    std::cout << "Audio device opened at " << have.freq << " Hz\n";
  } else {
    std::cout << "Failed to open audio device: " << SDL_GetError() << "\n";
    audio_enabled = false;
  }
  if (audio_pref.has_value()) {
    if (audio_device == 0) {
      audio_enabled = false;
    } else {
      audio_enabled = *audio_pref;
      SDL_PauseAudioDevice(audio_device, audio_enabled ? 0 : 1);
      if (!audio_enabled) {
        SDL_ClearQueuedAudio(audio_device);
      }
    }
  }

  global_settings.scale_override = options.scale_override;
  global_settings.fps_override = options.fps_override;
  global_settings.audio_enabled = audio_enabled;
  global_settings.cgb_color_correction = cgb_color_correction;
  global_settings.deadzone = input_config.axis_deadzone();
  global_settings.show_hud = show_hud_pref;
  global_settings.hud_corner = hud_corner;
  global_settings.hud_compact = hud_compact;
  global_settings.hud_timeout_seconds = hud_timeout_seconds;
  global_settings.filter = video_filter;

  if (rom_override_active && game_loaded && !rom_override_path.empty()) {
    RomOverride override = load_rom_override(rom_override_path.string());
    if (override.audio.has_value() && audio_device != 0) {
      audio_enabled = *override.audio;
      SDL_PauseAudioDevice(audio_device, audio_enabled ? 0 : 1);
      if (!audio_enabled) {
        SDL_ClearQueuedAudio(audio_device);
      }
    }
  }

#ifdef GBEMU_HAS_SDL_IMAGE
  int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
  int img_initted = IMG_Init(img_flags);
  if ((img_initted & img_flags) != img_flags) {
    std::cout << "SDL_image init failed: " << IMG_GetError() << "\n";
  }
#endif

  std::unordered_map<std::string, CoverTexture> cover_cache;
  auto get_cover_texture = [&](const std::string& path) -> CoverTexture* {
    if (path.empty()) {
      return nullptr;
    }
    auto it = cover_cache.find(path);
    if (it != cover_cache.end()) {
      return it->second.texture ? &it->second : nullptr;
    }
    CoverTexture entry;
    SDL_Surface* surface = load_cover_surface(path);
    if (!surface) {
      cover_cache.emplace(path, entry);
      return nullptr;
    }
    entry.texture = SDL_CreateTextureFromSurface(renderer, surface);
    entry.width = surface->w;
    entry.height = surface->h;
    SDL_FreeSurface(surface);
    cover_cache.emplace(path, entry);
    return entry.texture ? &cover_cache[path] : nullptr;
  };

  SDL_GameControllerEventState(SDL_ENABLE);
  std::unordered_map<SDL_JoystickID, SDL_GameController*> controllers;
  auto open_controller = [&](int device_index) {
    if (!SDL_IsGameController(device_index)) {
      const char* name = SDL_JoystickNameForIndex(device_index);
      if (name && *name) {
        std::cout << "Joystick detected (not game controller): " << name << "\n";
      }
      return;
    }
    SDL_GameController* controller = SDL_GameControllerOpen(device_index);
    if (!controller) {
      std::cout << "Failed to open controller: " << SDL_GetError() << "\n";
      return;
    }
    SDL_Joystick* joy = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickID instance_id = SDL_JoystickInstanceID(joy);
    if (controllers.find(instance_id) != controllers.end()) {
      SDL_GameControllerClose(controller);
      return;
    }
    controllers[instance_id] = controller;
    const char* name = SDL_GameControllerName(controller);
    std::cout << "Controller connected: " << (name ? name : "unknown")
              << " (id " << instance_id << ")\n";
  };
  auto close_controller = [&](SDL_JoystickID instance_id) {
    auto it = controllers.find(instance_id);
    if (it == controllers.end()) {
      return;
    }
    const char* name = SDL_GameControllerName(it->second);
    SDL_GameControllerClose(it->second);
    controllers.erase(it);
    std::cout << "Controller disconnected: " << (name ? name : "unknown")
              << " (id " << instance_id << ")\n";
  };
  int controller_count = SDL_NumJoysticks();
  for (int i = 0; i < controller_count; ++i) {
    open_controller(i);
  }
  if (controllers.empty()) {
    std::cout << "No controllers detected.\n";
  }

  std::cout << "Window created. Press ESC or close the window to exit.\n";

  double target_fps = 60.0;
  {
    std::scoped_lock lock(core_mutex);
    target_fps = options.fps_override.value_or(core.target_fps());
  }
  FramePacer pacer(target_fps);
  double fps_actual = 0.0;
  const double fps_freq = static_cast<double>(SDL_GetPerformanceFrequency());
  const int sample_rate = (audio_device != 0) ? have.freq : 0;
  const std::size_t max_queue_bytes = static_cast<std::size_t>(sample_rate * 4 * 2);
  unsigned filter_workers =
      options.filter_workers.has_value() ? sanitize_filter_workers(*options.filter_workers)
                                         : default_filter_workers();
  FilterWorkerPool filter_pool(filter_workers);
  std::cout << "Filter worker threads: " << filter_pool.worker_count() << "\n";
  std::atomic<double> emu_target_fps{target_fps};
  std::atomic<bool> emu_audio_enabled{audio_enabled};
  std::atomic<bool> emu_active{false};
  std::atomic<bool> emu_running{true};
  std::atomic<bool> emu_faulted{false};
  std::atomic<bool> emu_game_loaded{game_loaded};
  std::atomic<std::uint8_t> emu_pending_joypad{joypad_state};
  std::atomic<bool> emu_pending_joypad_irq{false};
  std::atomic<double> emu_fps_actual{0.0};
  std::mutex frame_mutex;
  std::vector<std::uint32_t> emu_framebuffer(
      static_cast<std::size_t>(fb_width) * fb_height, 0xFF000000u);
  int emu_fb_width = fb_width;
  int emu_fb_height = fb_height;
  int emu_fb_stride_bytes = fb_width * 4;
  std::atomic<std::uint64_t> emu_frame_serial{0};
  std::vector<std::uint32_t> render_framebuffer = emu_framebuffer;
  int render_fb_width = fb_width;
  int render_fb_height = fb_height;
  int render_fb_stride_bytes = fb_width * 4;
  std::uint64_t render_frame_serial = 0;

  auto reset_timing = [&]() {
    double default_fps_value = 60.0;
    {
      std::scoped_lock lock(core_mutex);
      default_fps_value = core.target_fps();
    }
    target_fps = options.fps_override.value_or(default_fps_value);
    pacer = FramePacer(target_fps);
    emu_target_fps.store(target_fps, std::memory_order_relaxed);
  };

  auto recalc_video = [&]() {
    int desired_scale = options.scale_override.value_or(default_scale(system));
    if (!game_loaded && !options.scale_override.has_value()) {
      desired_scale = 4;
    }
    scale = desired_scale;
    if (game_loaded) {
      std::scoped_lock lock(core_mutex);
      fb_width = core.framebuffer_width();
      fb_height = core.framebuffer_height();
    } else {
      fb_width = 240;
      fb_height = 160;
    }
    if (window) {
      SDL_SetWindowSize(window, fb_width * scale, fb_height * scale);
    }
    if (renderer) {
      if (texture) {
        SDL_DestroyTexture(texture);
      }
      texture = SDL_CreateTexture(renderer,
                                  SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  fb_width,
                                  fb_height);
    }
    resize_filter_buffers();
  };

  auto snapshot_frame_from_core = [&]() {
    if (!game_loaded) {
      std::scoped_lock frame_lock(frame_mutex);
      emu_fb_width = fb_width;
      emu_fb_height = fb_height;
      emu_fb_stride_bytes = fb_width * 4;
      emu_framebuffer.assign(static_cast<std::size_t>(fb_width) * fb_height, 0xFF000000u);
      emu_frame_serial.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
    std::vector<std::uint32_t> snapshot;
    bool boot_enabled = false;
    std::uint16_t boot_pc = 0;
    std::uint8_t boot_opcode = 0;
    {
      std::scoped_lock core_lock(core_mutex);
      width = core.framebuffer_width();
      height = core.framebuffer_height();
      stride_bytes = core.framebuffer_stride_bytes();
      const std::uint32_t* src = core.framebuffer();
      if (src && width > 0 && height > 0) {
        snapshot.assign(src, src + static_cast<std::size_t>(width) * height);
      }
      boot_enabled = core.boot_rom_enabled();
      boot_pc = core.cpu_pc();
      boot_opcode = core.cpu_opcode();
    }
    if (snapshot.empty() && width > 0 && height > 0) {
      snapshot.assign(static_cast<std::size_t>(width) * height, 0xFF000000u);
    }
    {
      std::scoped_lock frame_lock(frame_mutex);
      emu_fb_width = width;
      emu_fb_height = height;
      emu_fb_stride_bytes = stride_bytes;
      emu_framebuffer = std::move(snapshot);
    }
    emu_boot_enabled.store(boot_enabled, std::memory_order_relaxed);
    emu_boot_pc.store(boot_pc, std::memory_order_relaxed);
    emu_boot_opcode.store(boot_opcode, std::memory_order_relaxed);
    emu_frame_serial.fetch_add(1, std::memory_order_relaxed);
  };

  bool running = true;
  bool show_help = show_help_pref;
  std::uint64_t hud_last_activity = SDL_GetPerformanceCounter();
  UiMode ui_mode = launcher_enabled ? UiMode::Launcher : UiMode::Hidden;
  long long frame_count = 0;
  std::vector<RomEntry> roms;
  std::vector<int> launcher_list;
  int launcher_index = 0;
  int launcher_scroll = 0;
  int menu_index = 0;
  int settings_index = 0;
  LauncherFilter launcher_filter = LauncherFilter::All;
  std::string launcher_search;
  bool search_focus = false;
  bool text_input_active = false;
  UiMode last_mode = ui_mode;
  float ui_opacity = ui_mode == UiMode::Hidden ? 0.0f : 1.0f;
  int input_map_index = 0;
  bool rebind_active = false;
  gbemu::common::InputAction rebind_action = gbemu::common::InputAction::A;
  const std::vector<gbemu::common::InputAction> map_actions = {
      gbemu::common::InputAction::A,
      gbemu::common::InputAction::B,
      gbemu::common::InputAction::Select,
      gbemu::common::InputAction::Start,
      gbemu::common::InputAction::Up,
      gbemu::common::InputAction::Down,
      gbemu::common::InputAction::Left,
      gbemu::common::InputAction::Right,
  };
  int launcher_visible = 8;
  int mouse_x = 0;
  int mouse_y = 0;

  auto build_launcher_list = [&]() {
    launcher_list.clear();
    if (launcher_filter == LauncherFilter::Recents) {
      for (const auto& path : recent_paths) {
        for (std::size_t i = 0; i < roms.size(); ++i) {
          if (roms[i].path == path && matches_search(roms[i], launcher_search)) {
            launcher_list.push_back(static_cast<int>(i));
            break;
          }
        }
      }
    } else if (launcher_filter == LauncherFilter::Favorites) {
      for (std::size_t i = 0; i < roms.size(); ++i) {
        if (favorite_paths.count(roms[i].path) && matches_search(roms[i], launcher_search)) {
          launcher_list.push_back(static_cast<int>(i));
        }
      }
    } else {
      for (std::size_t i = 0; i < roms.size(); ++i) {
        if (matches_search(roms[i], launcher_search)) {
          launcher_list.push_back(static_cast<int>(i));
        }
      }
    }
  };

  auto normalize_launcher = [&]() {
    if (launcher_list.empty()) {
      launcher_index = 0;
      launcher_scroll = 0;
      return;
    }
    launcher_index = std::clamp(launcher_index, 0, static_cast<int>(launcher_list.size()) - 1);
    launcher_scroll = std::min(launcher_scroll, launcher_index);
  };

  auto selected_rom_index = [&]() -> int {
    if (launcher_list.empty()) {
      return -1;
    }
    if (launcher_index < 0 || launcher_index >= static_cast<int>(launcher_list.size())) {
      return -1;
    }
    return launcher_list[launcher_index];
  };

  auto select_launcher_rom = [&](int rom_index) {
    if (launcher_list.empty()) {
      launcher_index = 0;
      launcher_scroll = 0;
      return;
    }
    if (rom_index >= 0) {
      for (std::size_t i = 0; i < launcher_list.size(); ++i) {
        if (launcher_list[i] == rom_index) {
          launcher_index = static_cast<int>(i);
          break;
        }
      }
    }
    normalize_launcher();
  };

  auto refresh_roms = [&]() {
    launcher_error.clear();
    std::string selected_path;
    if (!launcher_list.empty() && launcher_index >= 0 &&
        launcher_index < static_cast<int>(launcher_list.size())) {
      selected_path = roms[launcher_list[launcher_index]].path;
    }
    std::vector<std::filesystem::path> roots;
    if (!options.rom_dirs.empty()) {
      for (const auto& dir : options.rom_dirs) {
        roots.emplace_back(dir);
      }
    } else {
      if (std::filesystem::exists("Test-Games")) {
        roots.emplace_back("Test-Games");
      }
      if (std::filesystem::exists("roms")) {
        roots.emplace_back("roms");
      }
    }
    if (!options.rom_path.empty()) {
      std::filesystem::path parent = std::filesystem::path(options.rom_path).parent_path();
      if (!parent.empty()) {
        roots.push_back(parent);
      }
    }
    roms = scan_roms(roots);

    if (!options.rom_path.empty()) {
      bool found = false;
      for (const auto& entry : roms) {
        if (entry.path == options.rom_path) {
          found = true;
          break;
        }
      }
      if (!found && std::filesystem::exists(options.rom_path)) {
        RomEntry entry;
        entry.path = options.rom_path;
        entry.size = std::filesystem::file_size(options.rom_path);
        entry.modified = std::filesystem::last_write_time(options.rom_path);
        std::vector<std::uint8_t> data;
        std::string error;
        if (gbemu::common::read_file(entry.path, &data, &error)) {
          entry.system = detect_system(data);
          entry.title = rom_title_from_data(data, entry.system);
        }
        if (entry.title.empty()) {
          entry.title = std::filesystem::path(entry.path).stem().string();
        }
        entry.cover_path = find_cover_path(entry.path);
        roms.insert(roms.begin(), entry);
      }
    }

    int selected_rom = -1;
    if (!selected_path.empty()) {
      for (std::size_t i = 0; i < roms.size(); ++i) {
        if (roms[i].path == selected_path) {
          selected_rom = static_cast<int>(i);
          break;
        }
      }
    } else if (!options.rom_path.empty()) {
      for (std::size_t i = 0; i < roms.size(); ++i) {
        if (roms[i].path == options.rom_path) {
          selected_rom = static_cast<int>(i);
          break;
        }
      }
    }
    launcher_index = 0;
    launcher_scroll = 0;
    build_launcher_list();
    select_launcher_rom(selected_rom);
  };

  if (launcher_enabled) {
    refresh_roms();
  }
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
  auto set_bit = [&](std::uint8_t& state, int bit, bool down) {
    if (bit < 0) {
      return false;
    }
    std::uint8_t before = state;
    if (down) {
      state = static_cast<std::uint8_t>(state & ~(1u << bit));
    } else {
      state = static_cast<std::uint8_t>(state | (1u << bit));
    }
    return state != before;
  };

  auto update_joypad_key = [&](SDL_Keycode key, bool pressed) -> bool {
    if (input_config.resolve(gbemu::common::InputAction::A, key)) return set_bit(key_state, 0, pressed);
    if (input_config.resolve(gbemu::common::InputAction::B, key)) return set_bit(key_state, 1, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Select, key)) return set_bit(key_state, 2, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Start, key)) return set_bit(key_state, 3, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Right, key)) return set_bit(key_state, 4, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Left, key)) return set_bit(key_state, 5, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Up, key)) return set_bit(key_state, 6, pressed);
    if (input_config.resolve(gbemu::common::InputAction::Down, key)) return set_bit(key_state, 7, pressed);
    return false;
  };

  auto update_joypad_button = [&](SDL_GameControllerButton button, bool pressed) -> bool {
    auto action = input_config.action_for_controller_button(static_cast<int>(button));
    if (!action.has_value()) {
      return false;
    }
    return set_bit(pad_state, action_to_bit(*action), pressed);
  };

  auto update_joypad_axis = [&](SDL_GameControllerAxis axis, int value) -> bool {
    bool changed = false;
    int deadzone = input_config.axis_deadzone();
    bool pos = value > deadzone;
    bool neg = value < -deadzone;
    if (auto action = input_config.action_for_controller_axis_pos(static_cast<int>(axis))) {
      changed |= set_bit(pad_state, action_to_bit(*action), pos);
    }
    if (auto action = input_config.action_for_controller_axis_neg(static_cast<int>(axis))) {
      changed |= set_bit(pad_state, action_to_bit(*action), neg);
    }
    if (!pos && !neg) {
      if (auto action = input_config.action_for_controller_axis_pos(static_cast<int>(axis))) {
        changed |= set_bit(pad_state, action_to_bit(*action), false);
      }
      if (auto action = input_config.action_for_controller_axis_neg(static_cast<int>(axis))) {
        changed |= set_bit(pad_state, action_to_bit(*action), false);
      }
    }
    return changed;
  };

  auto poll_controller_state = [&]() -> std::uint8_t {
    if (controllers.empty()) {
      return 0xFF;
    }
    SDL_GameControllerUpdate();
    std::uint8_t state = 0xFF;
    int deadzone = input_config.axis_deadzone();
    for (const auto& entry : controllers) {
      SDL_GameController* controller = entry.second;
      if (!controller) {
        continue;
      }
      for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
        if (SDL_GameControllerGetButton(
                controller, static_cast<SDL_GameControllerButton>(button)) == 0) {
          continue;
        }
        auto action = input_config.action_for_controller_button(button);
        if (action.has_value()) {
          int bit = action_to_bit(*action);
          if (bit >= 0) {
            state = static_cast<std::uint8_t>(state & ~(1u << bit));
          }
        }
      }
      for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis) {
        int value = SDL_GameControllerGetAxis(controller, static_cast<SDL_GameControllerAxis>(axis));
        if (value > deadzone) {
          auto action = input_config.action_for_controller_axis_pos(axis);
          if (action.has_value()) {
            int bit = action_to_bit(*action);
            if (bit >= 0) {
              state = static_cast<std::uint8_t>(state & ~(1u << bit));
            }
          }
        } else if (value < -deadzone) {
          auto action = input_config.action_for_controller_axis_neg(axis);
          if (action.has_value()) {
            int bit = action_to_bit(*action);
            if (bit >= 0) {
              state = static_cast<std::uint8_t>(state & ~(1u << bit));
            }
          }
        }
      }
    }
    return state;
  };

  auto poll_keyboard_state = [&]() -> std::uint8_t {
    SDL_PumpEvents();
    int count = 0;
    const Uint8* keys = SDL_GetKeyboardState(&count);
    if (!keys || count <= 0) {
      return 0xFF;
    }
    std::uint8_t state = 0xFF;
    const gbemu::common::InputAction actions[] = {
        gbemu::common::InputAction::A,
        gbemu::common::InputAction::B,
        gbemu::common::InputAction::Select,
        gbemu::common::InputAction::Start,
        gbemu::common::InputAction::Right,
        gbemu::common::InputAction::Left,
        gbemu::common::InputAction::Up,
        gbemu::common::InputAction::Down,
    };
    for (auto action : actions) {
      int keycode = input_config.key_for(action);
      if (keycode <= 0) {
        continue;
      }
      SDL_Scancode sc = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(keycode));
      if (sc == SDL_SCANCODE_UNKNOWN) {
        continue;
      }
      if (static_cast<int>(sc) < count && keys[sc]) {
        int bit = action_to_bit(action);
        if (bit >= 0) {
          state = static_cast<std::uint8_t>(state & ~(1u << bit));
        }
      }
    }
    return state;
  };

  auto build_help_text = [&]() {
    std::string text;
    text += "GBEMU HELP\n";
    text += "F1 WINDOW OVERLAY\n";
    text += "F2 CGB COLOR\n";
    text += "F3 TOGGLE HELP\n";
    text += "F4 HUD\n";
    text += "F6 CYCLE THEME\n";
    text += "F10 MENU\n";
    text += "F7 LAUNCHER\n";
    text += "/ SEARCH\n";
    text += "TAB FILTER\n";
    text += "F FAVORITE\n";
    text += "O OVERRIDE\n";
    text += "S SETTINGS\n";
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
  auto touch_hud = [&]() {
    hud_last_activity = SDL_GetPerformanceCounter();
  };
  auto set_theme = [&](UiTheme theme) {
    ui_theme = theme;
    theme_toast_frames = 180;
    std::cout << "UI theme: " << ui_theme_name(ui_theme) << "\n";
  };

  auto menu_labels = [&]() {
    std::vector<std::string> labels;
    labels.emplace_back("RESUME");
    labels.emplace_back("SAVE STATE");
    labels.emplace_back("LOAD STATE");
    labels.emplace_back(std::string("HELP: ") + (show_help ? "ON" : "OFF"));
    labels.emplace_back(std::string("WINDOW OVERLAY: ") + (debug_window_overlay ? "ON" : "OFF"));
    labels.emplace_back(std::string("CGB COLOR: ") + (cgb_color_correction ? "ON" : "OFF"));
    labels.emplace_back(std::string("THEME: ") + upper_ascii(ui_theme_name(ui_theme)));
    labels.emplace_back("SETTINGS");
    labels.emplace_back("LAUNCHER");
    labels.emplace_back("QUIT");
    return labels;
  };

  auto apply_menu_action = [&](int index) {
    switch (index) {
      case 0:
        ui_mode = UiMode::Hidden;
        break;
      case 1:
        save_full_state();
        break;
      case 2:
        load_full_state();
        break;
      case 3:
        show_help = !show_help;
        break;
      case 4:
        debug_window_overlay = !debug_window_overlay;
        {
          std::scoped_lock lock(core_mutex);
          core.set_debug_window_overlay(debug_window_overlay);
        }
        break;
      case 5:
        cgb_color_correction = !cgb_color_correction;
        {
          std::scoped_lock lock(core_mutex);
          core.set_cgb_color_correction(cgb_color_correction);
        }
        if (rom_override_active && game_loaded && !rom_override_path.empty()) {
          save_rom_override(rom_override_path.string(),
                            scale,
                            options.fps_override.value_or(core.target_fps()),
                            input_config.axis_deadzone(),
                            audio_enabled,
                            cgb_color_correction,
                            show_hud,
                            hud_corner,
                            hud_compact,
                            hud_timeout_seconds,
                            video_filter);
        } else {
          global_settings.cgb_color_correction = cgb_color_correction;
        }
        break;
      case 6:
        set_theme(next_theme(ui_theme, 1));
        break;
      case 7:
        ui_mode = UiMode::Settings;
        settings_index = 0;
        break;
      case 8:
        if (launcher_enabled) {
          ui_mode = UiMode::Launcher;
          refresh_roms();
        }
        break;
      case 9:
        running = false;
        break;
      default:
        break;
    }
  };

  auto settings_labels = [&]() {
    std::vector<std::string> labels;
    labels.emplace_back(std::string("THEME: ") + upper_ascii(ui_theme_name(ui_theme)));
    labels.emplace_back(std::string("SCALE: ") + std::to_string(scale));
    double fps_value = options.fps_override.value_or(core.target_fps());
    std::string fps_label = "FPS: ";
    if (fps_value <= 0.0) {
      fps_label += "UNLIMITED";
    } else {
      std::ostringstream ss;
      if (std::fabs(fps_value - std::round(fps_value)) < 0.05) {
        ss << static_cast<int>(std::round(fps_value));
      } else {
        ss << std::fixed << std::setprecision(1) << fps_value;
      }
      fps_label += ss.str();
    }
    labels.emplace_back(fps_label);
    labels.emplace_back(std::string("FILTER: ") + upper_ascii(video_filter_name(video_filter)));
    if (audio_device == 0) {
      labels.emplace_back("AUDIO: UNAVAILABLE");
    } else {
      labels.emplace_back(std::string("AUDIO: ") + (audio_enabled ? "ON" : "OFF"));
    }
    labels.emplace_back(std::string("DEADZONE: ") + std::to_string(input_config.axis_deadzone()));
    labels.emplace_back(std::string("HUD: ") + (show_hud ? "ON" : "OFF"));
    labels.emplace_back(std::string("HUD POS: ") + upper_ascii(hud_corner_name(hud_corner)));
    labels.emplace_back(std::string("HUD STYLE: ") + (hud_compact ? "COMPACT" : "FULL"));
    if (hud_timeout_seconds <= 0) {
      labels.emplace_back("HUD TIMEOUT: OFF");
    } else {
      labels.emplace_back(std::string("HUD TIMEOUT: ") +
                          std::to_string(hud_timeout_seconds) + "S");
    }
    if (game_loaded) {
      labels.emplace_back(std::string("PER-ROM: ") + (rom_override_active ? "ON" : "OFF"));
    } else {
      labels.emplace_back("PER-ROM: N/A");
    }
    labels.emplace_back("INPUT MAP");
    labels.emplace_back(std::string("HELP: ") + (show_help ? "ON" : "OFF"));
    labels.emplace_back("BACK");
    return labels;
  };

  auto apply_settings_action = [&](int index) {
    switch (index) {
      case 0:
        set_theme(next_theme(ui_theme, 1));
        break;
      case 4:
        if (audio_device != 0) {
          audio_enabled = !audio_enabled;
          SDL_PauseAudioDevice(audio_device, audio_enabled ? 0 : 1);
          if (!audio_enabled) {
            SDL_ClearQueuedAudio(audio_device);
          }
        }
        break;
      case 6:
        show_hud = !show_hud;
        break;
      case 7:
        hud_corner = next_hud_corner(hud_corner, 1);
        break;
      case 8:
        hud_compact = !hud_compact;
        break;
      case 9:
        if (hud_timeout_seconds <= 0) {
          hud_timeout_seconds = 5;
        } else if (hud_timeout_seconds < 30) {
          hud_timeout_seconds += 5;
        } else {
          hud_timeout_seconds = 0;
        }
        break;
      case 10:
        if (game_loaded) {
          if (rom_override_active) {
            rom_override_active = false;
            if (!rom_override_path.empty()) {
              std::error_code ec;
              std::filesystem::remove(rom_override_path, ec);
            }
            active_input_map_path = input_map_path;
            reload_input_config(active_input_map_path);
            options.scale_override = global_settings.scale_override;
            options.fps_override = global_settings.fps_override;
            cgb_color_correction = global_settings.cgb_color_correction;
            {
              std::scoped_lock lock(core_mutex);
              core.set_cgb_color_correction(cgb_color_correction);
            }
            audio_enabled = global_settings.audio_enabled;
            if (audio_device != 0) {
              SDL_PauseAudioDevice(audio_device, audio_enabled ? 0 : 1);
              if (!audio_enabled) {
                SDL_ClearQueuedAudio(audio_device);
              }
            }
            input_config.set_axis_deadzone(global_settings.deadzone);
            show_hud = global_settings.show_hud;
            hud_corner = global_settings.hud_corner;
            hud_compact = global_settings.hud_compact;
            hud_timeout_seconds = global_settings.hud_timeout_seconds;
            video_filter = global_settings.filter;
            reset_timing();
            recalc_video();
          } else {
            rom_override_active = true;
            std::error_code ec;
            std::filesystem::create_directories(input_map_dir, ec);
            active_input_map_path = rom_input_map_path;
            if (!active_input_map_path.empty() && !std::filesystem::exists(active_input_map_path)) {
              save_input_map();
            } else {
              reload_input_config(active_input_map_path);
            }
            if (!rom_override_path.empty()) {
              save_rom_override(rom_override_path.string(),
                                scale,
                                options.fps_override.value_or(core.target_fps()),
                                input_config.axis_deadzone(),
                                audio_enabled,
                                cgb_color_correction,
                                show_hud,
                                hud_corner,
                                hud_compact,
                                hud_timeout_seconds,
                                video_filter);
            }
          }
          crt_reset = true;
        }
        break;
      case 11:
        ui_mode = UiMode::InputMap;
        input_map_index = 0;
        rebind_active = false;
        break;
      case 12:
        show_help = !show_help;
        break;
      case 13:
        ui_mode = UiMode::Menu;
        break;
      default:
        break;
    }
    if ((index == 6 || index == 7 || index == 8 || index == 9) && show_hud) {
      touch_hud();
    }
    if (index == 4 || index == 6 || index == 7 || index == 8 ||
        index == 9 || index == 10) {
      if (rom_override_active && game_loaded && !rom_override_path.empty()) {
        save_rom_override(rom_override_path.string(),
                          scale,
                          options.fps_override.value_or(core.target_fps()),
                          input_config.axis_deadzone(),
                          audio_enabled,
                          cgb_color_correction,
                          show_hud,
                          hud_corner,
                          hud_compact,
                          hud_timeout_seconds,
                          video_filter);
      } else {
        global_settings.scale_override = options.scale_override;
        global_settings.fps_override = options.fps_override;
        global_settings.audio_enabled = audio_enabled;
        global_settings.cgb_color_correction = cgb_color_correction;
        global_settings.deadzone = input_config.axis_deadzone();
        global_settings.show_hud = show_hud;
        global_settings.hud_corner = hud_corner;
        global_settings.hud_compact = hud_compact;
        global_settings.hud_timeout_seconds = hud_timeout_seconds;
        global_settings.filter = video_filter;
      }
    }
  };

  auto move_settings = [&](int delta) {
    auto labels = settings_labels();
    if (labels.empty()) {
      return;
    }
    int count = static_cast<int>(labels.size());
    settings_index = (settings_index + delta + count) % count;
  };

  auto adjust_settings = [&](int index, int delta) {
    switch (index) {
      case 0:
        set_theme(next_theme(ui_theme, delta >= 0 ? 1 : -1));
        break;
      case 1: {
        int next_scale = std::clamp(scale + delta, 1, 8);
        if (next_scale != scale) {
          options.scale_override = next_scale;
          recalc_video();
        }
      } break;
      case 2: {
        double step = 5.0;
        double next_fps = options.fps_override.value_or(core.target_fps());
        next_fps = std::max(0.0, next_fps + step * delta);
        options.fps_override = next_fps;
        reset_timing();
      } break;
      case 3:
        video_filter = next_video_filter(video_filter, delta >= 0 ? 1 : -1);
        crt_reset = true;
        break;
      case 4:
        if (audio_device != 0) {
          audio_enabled = !audio_enabled;
          SDL_PauseAudioDevice(audio_device, audio_enabled ? 0 : 1);
          if (!audio_enabled) {
            SDL_ClearQueuedAudio(audio_device);
          }
        }
        break;
      case 5: {
        int next_deadzone = std::clamp(input_config.axis_deadzone() + delta * 1000, 0, 32000);
        input_config.set_axis_deadzone(next_deadzone);
        save_input_map();
      } break;
      case 6:
        show_hud = !show_hud;
        break;
      case 7:
        hud_corner = next_hud_corner(hud_corner, delta >= 0 ? 1 : -1);
        break;
      case 8:
        hud_compact = !hud_compact;
        break;
      case 9: {
        int step = 5;
        int next_timeout = hud_timeout_seconds + step * delta;
        next_timeout = std::clamp(next_timeout, 0, 30);
        hud_timeout_seconds = next_timeout;
      } break;
      case 12:
        show_help = !show_help;
        break;
      default:
        break;
    }
    if ((index == 6 || index == 7 || index == 8 || index == 9) && show_hud) {
      touch_hud();
    }
    if (index == 1 || index == 2 || index == 3 || index == 4 ||
        index == 5 || index == 6 || index == 7 || index == 8 ||
        index == 9) {
      if (rom_override_active && game_loaded && !rom_override_path.empty()) {
        save_rom_override(rom_override_path.string(),
                          scale,
                          options.fps_override.value_or(core.target_fps()),
                          input_config.axis_deadzone(),
                          audio_enabled,
                          cgb_color_correction,
                          show_hud,
                          hud_corner,
                          hud_compact,
                          hud_timeout_seconds,
                          video_filter);
      } else {
        global_settings.scale_override = options.scale_override;
        global_settings.fps_override = options.fps_override;
        global_settings.audio_enabled = audio_enabled;
        global_settings.cgb_color_correction = cgb_color_correction;
        global_settings.deadzone = input_config.axis_deadzone();
        global_settings.show_hud = show_hud;
        global_settings.hud_corner = hud_corner;
        global_settings.hud_compact = hud_compact;
        global_settings.hud_timeout_seconds = hud_timeout_seconds;
        global_settings.filter = video_filter;
      }
    }
  };

  auto toggle_rom_override_for = [&](const RomEntry& entry) {
    if (game_loaded && entry.path == current_rom_path) {
      apply_settings_action(10);
      return;
    }
    std::filesystem::path override_path = rom_override_path_for(entry.path);
    bool enable = !std::filesystem::exists(override_path);
    if (enable) {
      int base_scale = global_settings.scale_override
                           .value_or(default_scale(entry.system));
      double base_fps = global_settings.fps_override
                            .value_or(default_fps(entry.system));
      save_rom_override(override_path.string(),
                        base_scale,
                        base_fps,
                        global_settings.deadzone,
                        global_settings.audio_enabled,
                        global_settings.cgb_color_correction,
                        global_settings.show_hud,
                        global_settings.hud_corner,
                        global_settings.hud_compact,
                        global_settings.hud_timeout_seconds,
                        global_settings.filter);
      ensure_rom_input_map(entry.path);
    } else {
      std::error_code ec;
      std::filesystem::remove(override_path, ec);
    }
  };

  auto move_menu = [&](int delta) {
    auto labels = menu_labels();
    if (labels.empty()) {
      return;
    }
    int count = static_cast<int>(labels.size());
    menu_index = (menu_index + delta + count) % count;
  };

  auto move_launcher = [&](int delta) {
    if (launcher_list.empty()) {
      return;
    }
    int count = static_cast<int>(launcher_list.size());
    launcher_index = std::clamp(launcher_index + delta, 0, count - 1);
    int visible = std::max(1, launcher_visible);
    if (launcher_index < launcher_scroll) {
      launcher_scroll = launcher_index;
    } else if (launcher_index >= launcher_scroll + visible) {
      launcher_scroll = launcher_index - visible + 1;
    }
  };

  auto launch_selected = [&]() {
    if (launcher_list.empty()) {
      return;
    }
    emu_active.store(false, std::memory_order_relaxed);
    std::string err;
    int selected = launcher_list[launcher_index];
    bool loaded = false;
    {
      std::scoped_lock lock(core_mutex);
      loaded = load_game(roms[selected].path, &err);
    }
    if (!loaded) {
      launcher_error = err;
      return;
    }
    launcher_error.clear();
    reset_timing();
    recalc_video();
    if (audio_device != 0) {
      SDL_ClearQueuedAudio(audio_device);
    }
    emu_audio_enabled.store(audio_enabled, std::memory_order_relaxed);
    emu_game_loaded.store(game_loaded, std::memory_order_relaxed);
    emu_pending_joypad.store(joypad_state, std::memory_order_relaxed);
    emu_pending_joypad_irq.store(false, std::memory_order_relaxed);
    emu_faulted.store(false, std::memory_order_relaxed);
    snapshot_frame_from_core();
    ui_mode = UiMode::Hidden;
  };

  auto cycle_launcher_filter = [&](int delta) {
    int selected = selected_rom_index();
    launcher_filter = next_launcher_filter(launcher_filter, delta);
    build_launcher_list();
    select_launcher_rom(selected);
  };

  auto toggle_favorite = [&]() {
    int selected = selected_rom_index();
    if (selected < 0 || selected >= static_cast<int>(roms.size())) {
      return;
    }
    const std::string& path = roms[selected].path;
    if (favorite_paths.count(path)) {
      favorite_paths.erase(path);
    } else {
      favorite_paths.insert(path);
    }
    save_launcher_state(launcher_state_path.string(), recent_paths, favorite_paths);
    build_launcher_list();
    select_launcher_rom(selected);
  };

  auto update_text_input = [&]() {
    bool want = (ui_mode == UiMode::Launcher);
    if (want && !text_input_active) {
      SDL_StartTextInput();
      text_input_active = true;
    } else if (!want && text_input_active) {
      SDL_StopTextInput();
      text_input_active = false;
      search_focus = false;
    }
  };

  auto clear_search = [&]() {
    if (!launcher_search.empty()) {
      int selected = selected_rom_index();
      launcher_search.clear();
      build_launcher_list();
      select_launcher_rom(selected);
    }
  };

  update_text_input();
  snapshot_frame_from_core();
  emu_faulted.store(false, std::memory_order_relaxed);
  std::thread emu_thread([&]() {
    double local_target_fps = emu_target_fps.load(std::memory_order_relaxed);
    FramePacer emu_pacer(local_target_fps);
    double audio_accum = 0.0;
    int fps_frames_local = 0;
    std::uint64_t fps_last_local = SDL_GetPerformanceCounter();
    std::uint8_t applied_joypad = emu_pending_joypad.load(std::memory_order_relaxed);

    while (emu_running.load(std::memory_order_relaxed)) {
      if (!emu_active.load(std::memory_order_relaxed) ||
          !emu_game_loaded.load(std::memory_order_relaxed)) {
        SDL_Delay(1);
        audio_accum = 0.0;
        fps_frames_local = 0;
        fps_last_local = SDL_GetPerformanceCounter();
        continue;
      }

      double next_target_fps = emu_target_fps.load(std::memory_order_relaxed);
      if (std::fabs(next_target_fps - local_target_fps) > 0.001) {
        local_target_fps = next_target_fps;
        emu_pacer = FramePacer(local_target_fps);
        audio_accum = 0.0;
      }

      bool faulted = false;
      bool boot_enabled = false;
      std::uint16_t boot_pc = 0;
      std::uint8_t boot_opcode = 0;
      int width = 0;
      int height = 0;
      int stride_bytes = 0;
      std::vector<std::uint32_t> snapshot;

      {
        std::scoped_lock core_lock(core_mutex);
        std::uint8_t desired_joypad = emu_pending_joypad.load(std::memory_order_relaxed);
        if (desired_joypad != applied_joypad) {
          core.set_joypad_state(desired_joypad);
          applied_joypad = desired_joypad;
        }
        if (emu_pending_joypad_irq.exchange(false, std::memory_order_relaxed)) {
          core.request_interrupt(4);
        }

        core.step_frame();
        faulted = core.cpu_faulted();
        width = core.framebuffer_width();
        height = core.framebuffer_height();
        stride_bytes = core.framebuffer_stride_bytes();
        const std::uint32_t* src = core.framebuffer();
        if (src && width > 0 && height > 0) {
          snapshot.assign(src, src + static_cast<std::size_t>(width) * height);
        }
        boot_enabled = core.boot_rom_enabled();
        boot_pc = core.cpu_pc();
        boot_opcode = core.cpu_opcode();

        if (audio_device != 0 &&
            sample_rate > 0 &&
            emu_audio_enabled.load(std::memory_order_relaxed)) {
          double audio_fps = local_target_fps > 0.0 ? local_target_fps : 60.0;
          audio_accum += static_cast<double>(sample_rate) / audio_fps;
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
      }

      if (!snapshot.empty()) {
        std::scoped_lock frame_lock(frame_mutex);
        emu_fb_width = width;
        emu_fb_height = height;
        emu_fb_stride_bytes = stride_bytes;
        emu_framebuffer = std::move(snapshot);
        emu_frame_serial.fetch_add(1, std::memory_order_relaxed);
      }

      emu_boot_enabled.store(boot_enabled, std::memory_order_relaxed);
      emu_boot_pc.store(boot_pc, std::memory_order_relaxed);
      emu_boot_opcode.store(boot_opcode, std::memory_order_relaxed);
      emu_faulted.store(faulted, std::memory_order_relaxed);

      ++fps_frames_local;
      std::uint64_t now = SDL_GetPerformanceCounter();
      if (now - fps_last_local >= static_cast<std::uint64_t>(fps_freq)) {
        double fps = (fps_frames_local * fps_freq) /
                     static_cast<double>(now - fps_last_local);
        emu_fps_actual.store(fps, std::memory_order_relaxed);
        fps_frames_local = 0;
        fps_last_local = now;
      }

      if (faulted) {
        emu_active.store(false, std::memory_order_relaxed);
      }
      emu_pacer.sleep();
    }
  });
  emu_thread_started = true;

  while (running) {
    if (emu_faulted.load(std::memory_order_relaxed)) {
      emu_active.store(false, std::memory_order_relaxed);
      {
        std::scoped_lock lock(core_mutex);
      }
      dump_cpu_fault();
      save_state();
      break;
    }

    int window_w = fb_width * scale;
    int window_h = fb_height * scale;
    if (window) {
      SDL_GetWindowSize(window, &window_w, &window_h);
    }

    auto menu_panel_rect = [&]() {
      int panel_width = window_w > 520 ? 520 : window_w - 20;
      int panel_height = std::min(360, window_h - 40);
      return SDL_Rect{
          (window_w - panel_width) / 2,
          (window_h - panel_height) / 2,
          panel_width,
          panel_height
      };
    };

    auto settings_panel_rect = [&]() {
      int panel_width = window_w > 560 ? 560 : window_w - 20;
      int panel_height = std::min(380, window_h - 40);
      return SDL_Rect{
          (window_w - panel_width) / 2,
          (window_h - panel_height) / 2,
          panel_width,
          panel_height
      };
    };

    auto input_map_panel_rect = [&]() {
      int panel_width = window_w > 620 ? 620 : window_w - 20;
      int panel_height = std::min(420, window_h - 40);
      return SDL_Rect{
          (window_w - panel_width) / 2,
          (window_h - panel_height) / 2,
          panel_width,
          panel_height
      };
    };

    auto launcher_layout = [&]() {
      LauncherLayout layout;
      layout.header = SDL_Rect{0, 0, window_w, 44};
      int search_box_w = std::min(260, window_w / 3);
      int search_box_h = 22;
      int search_box_x = (window_w - search_box_w) / 2;
      int search_box_y = layout.header.y + 11;
      layout.search_box = SDL_Rect{search_box_x, search_box_y, search_box_w, search_box_h};

      std::string title = "GBEMU LAUNCHER";
      int title_w = text_width(title, 2);
      int settings_w = text_width("SETTINGS", 2) + 12;
      layout.settings_button = SDL_Rect{14 + title_w + 12, 9, settings_w, 24};

      std::string filter_label = "FILTER: " + upper_ascii(launcher_filter_name(launcher_filter));
      int filter_w = text_width(filter_label, 2);
      layout.filter_hit = SDL_Rect{window_w - 14 - filter_w, 20, filter_w, 14};

      int margin = 16;
      int top = layout.header.h + 8;
      int list_w = (window_w * 2) / 3;
      int detail_w = window_w - list_w - margin * 3;
      layout.list_panel = SDL_Rect{margin, top, list_w, window_h - top - margin};
      layout.detail_panel = SDL_Rect{layout.list_panel.x + layout.list_panel.w + margin,
                                     top, detail_w, layout.list_panel.h};
      layout.card_h = 38;
      layout.visible = std::max(1, (layout.list_panel.h - 16) / layout.card_h);
      layout.start_y = layout.list_panel.y + 8;

      int button_h = 24;
      int button_w = (layout.detail_panel.w - 30) / 2;
      int override_y = layout.detail_panel.y + layout.detail_panel.h - 92;
      int button_y = layout.detail_panel.y + layout.detail_panel.h - 60;
      layout.override_button = SDL_Rect{layout.detail_panel.x + 10, override_y,
                                        layout.detail_panel.w - 20, button_h};
      layout.play_button = SDL_Rect{layout.detail_panel.x + 10, button_y, button_w, button_h};
      layout.favorite_button = SDL_Rect{layout.play_button.x + button_w + 10, button_y,
                                        button_w, button_h};
      return layout;
    };

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
        continue;
      } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
        open_controller(event.cdevice.which);
        continue;
      } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        close_controller(event.cdevice.which);
        continue;
      } else if (event.type == SDL_KEYDOWN) {
        SDL_Keycode key = event.key.keysym.sym;
        if (key == SDLK_ESCAPE) {
          if (ui_mode == UiMode::Launcher && search_focus) {
            if (!launcher_search.empty()) {
              clear_search();
            } else {
              search_focus = false;
            }
            continue;
          }
          if (ui_mode == UiMode::Menu) {
            ui_mode = UiMode::Hidden;
          } else if (ui_mode == UiMode::Settings) {
            ui_mode = UiMode::Menu;
          } else if (ui_mode == UiMode::InputMap) {
            if (rebind_active) {
              rebind_active = false;
            } else {
              ui_mode = UiMode::Settings;
            }
          } else if (ui_mode == UiMode::Launcher) {
            if (game_loaded) {
              ui_mode = UiMode::Hidden;
            } else {
              running = false;
            }
          } else {
            running = false;
          }
          continue;
        }
        if (key == SDLK_F10) {
          if (ui_mode == UiMode::Menu) {
            ui_mode = UiMode::Hidden;
          } else if (ui_mode == UiMode::Settings) {
            ui_mode = UiMode::Menu;
          } else if (ui_mode == UiMode::InputMap) {
            ui_mode = UiMode::Settings;
          } else if (ui_mode == UiMode::Hidden && game_loaded) {
            ui_mode = UiMode::Menu;
            menu_index = 0;
          }
          continue;
        }
        if (key == SDLK_F6) {
          set_theme(next_theme(ui_theme, 1));
          continue;
        }
        if (key == SDLK_F7) {
          if (launcher_enabled) {
            ui_mode = UiMode::Launcher;
            refresh_roms();
          }
          continue;
        }
        if (key == SDLK_F1) {
          debug_window_overlay = !debug_window_overlay;
          {
            std::scoped_lock lock(core_mutex);
            core.set_debug_window_overlay(debug_window_overlay);
          }
          std::cout << "Debug window overlay: " << (debug_window_overlay ? "ON" : "OFF") << "\n";
          continue;
        }
        if (key == SDLK_F2) {
          cgb_color_correction = !cgb_color_correction;
          {
            std::scoped_lock lock(core_mutex);
            core.set_cgb_color_correction(cgb_color_correction);
          }
          std::cout << "CGB color correction: " << (cgb_color_correction ? "ON" : "OFF") << "\n";
          continue;
        }
        if (key == SDLK_F3) {
          show_help = !show_help;
          std::cout << "Help overlay: " << (show_help ? "ON" : "OFF") << "\n";
          continue;
        }
        if (key == SDLK_F4) {
          show_hud = !show_hud;
          if (show_hud) {
            touch_hud();
          }
          std::cout << "HUD: " << (show_hud ? "ON" : "OFF") << "\n";
          if (rom_override_active && game_loaded && !rom_override_path.empty()) {
            save_rom_override(rom_override_path.string(),
                              scale,
                              options.fps_override.value_or(core.target_fps()),
                              input_config.axis_deadzone(),
                              audio_enabled,
                              cgb_color_correction,
                              show_hud,
                              hud_corner,
                              hud_compact,
                              hud_timeout_seconds,
                              video_filter);
          } else {
            global_settings.show_hud = show_hud;
          }
          continue;
        }
        if (key == SDLK_F5) {
          save_full_state();
          continue;
        }
        if (key == SDLK_F9) {
          load_full_state();
          continue;
        }

        if (ui_mode == UiMode::Menu) {
          if (key == SDLK_UP) {
            move_menu(-1);
          } else if (key == SDLK_DOWN) {
            move_menu(1);
          } else if (key == SDLK_LEFT) {
            set_theme(next_theme(ui_theme, -1));
          } else if (key == SDLK_RIGHT) {
            set_theme(next_theme(ui_theme, 1));
          } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
            apply_menu_action(menu_index);
          }
          continue;
        }

        if (ui_mode == UiMode::Settings) {
          if (key == SDLK_UP) {
            move_settings(-1);
          } else if (key == SDLK_DOWN) {
            move_settings(1);
          } else if (key == SDLK_LEFT) {
            adjust_settings(settings_index, -1);
          } else if (key == SDLK_RIGHT) {
            adjust_settings(settings_index, 1);
          } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
            apply_settings_action(settings_index);
          }
          continue;
        }

        if (ui_mode == UiMode::InputMap) {
          if (rebind_active) {
            if (key == SDLK_ESCAPE) {
              rebind_active = false;
              continue;
            }
            input_config.set_key_binding(rebind_action, static_cast<int>(key));
            save_input_map();
            rebind_active = false;
          } else {
            if (key == SDLK_UP) {
              input_map_index = (input_map_index - 1 + static_cast<int>(map_actions.size()))
                                % static_cast<int>(map_actions.size());
            } else if (key == SDLK_DOWN) {
              input_map_index = (input_map_index + 1)
                                % static_cast<int>(map_actions.size());
            } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
              rebind_action = map_actions[static_cast<std::size_t>(input_map_index)];
              rebind_active = true;
            } else if (key == SDLK_DELETE) {
              input_config.clear_controller_binding(
                  map_actions[static_cast<std::size_t>(input_map_index)]);
              save_input_map();
            } else if (key == SDLK_BACKSPACE) {
              input_config.set_key_binding(
                  map_actions[static_cast<std::size_t>(input_map_index)], 0);
              save_input_map();
            }
          }
          continue;
        }

        if (ui_mode == UiMode::Launcher) {
          if (key == SDLK_UP) {
            move_launcher(-1);
          } else if (key == SDLK_DOWN) {
            move_launcher(1);
          } else if (key == SDLK_SLASH || (key == SDLK_f && (event.key.keysym.mod & KMOD_CTRL))) {
            search_focus = true;
          } else if (key == SDLK_LEFT) {
            set_theme(next_theme(ui_theme, -1));
          } else if (key == SDLK_RIGHT) {
            set_theme(next_theme(ui_theme, 1));
          } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
            launch_selected();
          } else if (key == SDLK_TAB) {
            cycle_launcher_filter(1);
          } else if (key == SDLK_BACKSPACE && search_focus) {
            if (!launcher_search.empty()) {
              int selected = selected_rom_index();
              launcher_search.pop_back();
              build_launcher_list();
              select_launcher_rom(selected);
            }
          } else if (key == SDLK_s && !search_focus) {
            ui_mode = UiMode::Settings;
            settings_index = 0;
          } else if (key == SDLK_f && !search_focus) {
            toggle_favorite();
          } else if (key == SDLK_o && !search_focus) {
            int selected = selected_rom_index();
            if (selected >= 0 && selected < static_cast<int>(roms.size())) {
              toggle_rom_override_for(roms[selected]);
            }
          } else if (key == SDLK_r && !search_focus) {
            refresh_roms();
          }
          continue;
        }

        if (ui_mode == UiMode::Hidden) {
          if (update_joypad_key(key, true)) {
            if (show_hud) {
              touch_hud();
            }
            std::uint8_t combined = static_cast<std::uint8_t>(key_state & pad_state);
            if (combined != joypad_state) {
              joypad_state = combined;
              emu_pending_joypad.store(joypad_state, std::memory_order_relaxed);
              emu_pending_joypad_irq.store(true, std::memory_order_relaxed);
            }
          }
        }
      } else if (event.type == SDL_KEYUP) {
        if (ui_mode == UiMode::Hidden) {
          if (update_joypad_key(event.key.keysym.sym, false)) {
            if (show_hud) {
              touch_hud();
            }
            std::uint8_t combined = static_cast<std::uint8_t>(key_state & pad_state);
            if (combined != joypad_state) {
              joypad_state = combined;
              emu_pending_joypad.store(joypad_state, std::memory_order_relaxed);
              emu_pending_joypad_irq.store(true, std::memory_order_relaxed);
            }
          }
        }
      } else if (event.type == SDL_TEXTINPUT) {
        if (ui_mode == UiMode::Launcher && search_focus) {
          int selected = selected_rom_index();
          constexpr std::size_t kMaxSearch = 32;
          const char* text = event.text.text;
          if (text) {
            for (const char* p = text; *p != '\0'; ++p) {
              unsigned char c = static_cast<unsigned char>(*p);
              if (c >= 32 && c < 127 && launcher_search.size() < kMaxSearch) {
                launcher_search.push_back(static_cast<char>(c));
              }
            }
          }
          build_launcher_list();
          select_launcher_rom(selected);
        }
      } else if (event.type == SDL_MOUSEMOTION) {
        int mx = event.motion.x;
        int my = event.motion.y;
        mouse_x = mx;
        mouse_y = my;
        const UiThemeDef& theme_local = theme_def(ui_theme);
        if (ui_mode == UiMode::Menu) {
          auto labels = menu_labels();
          SDL_Rect panel = menu_panel_rect();
          int item_y = panel.y + theme_local.panel_padding;
          int line_h = 18;
          for (std::size_t i = 0; i < labels.size(); ++i) {
            SDL_Rect row{panel.x + theme_local.panel_padding, item_y - 2,
                         panel.w - theme_local.panel_padding * 2, line_h};
            if (point_in_rect(mx, my, row)) {
              menu_index = static_cast<int>(i);
              break;
            }
            item_y += line_h + 6;
          }
        } else if (ui_mode == UiMode::Settings) {
          auto labels = settings_labels();
          SDL_Rect panel = settings_panel_rect();
          int item_y = panel.y + theme_local.panel_padding;
          int line_h = 18;
          for (std::size_t i = 0; i < labels.size(); ++i) {
            SDL_Rect row{panel.x + theme_local.panel_padding, item_y - 2,
                         panel.w - theme_local.panel_padding * 2, line_h};
            if (point_in_rect(mx, my, row)) {
              settings_index = static_cast<int>(i);
              break;
            }
            item_y += line_h + 6;
          }
        } else if (ui_mode == UiMode::InputMap) {
          SDL_Rect panel = input_map_panel_rect();
          int item_y = panel.y + theme_local.panel_padding + 18;
          int line_h = 18;
          for (std::size_t i = 0; i < map_actions.size(); ++i) {
            SDL_Rect row{panel.x + theme_local.panel_padding, item_y - 2,
                         panel.w - theme_local.panel_padding * 2, line_h};
            if (point_in_rect(mx, my, row)) {
              input_map_index = static_cast<int>(i);
              break;
            }
            item_y += line_h + 6;
          }
        } else if (ui_mode == UiMode::Launcher) {
          LauncherLayout layout = launcher_layout();
          launcher_visible = layout.visible;
          if (point_in_rect(mx, my, layout.list_panel)) {
            int rel_y = my - layout.start_y;
            if (rel_y >= 0) {
              int idx = rel_y / layout.card_h + launcher_scroll;
              if (idx >= 0 && idx < static_cast<int>(launcher_list.size())) {
                launcher_index = idx;
              }
            }
          }
        }
      } else if (event.type == SDL_MOUSEWHEEL) {
        if (ui_mode == UiMode::Launcher) {
          move_launcher(-event.wheel.y);
        } else if (ui_mode == UiMode::Menu) {
          move_menu(-event.wheel.y);
        } else if (ui_mode == UiMode::Settings) {
          move_settings(-event.wheel.y);
        } else if (ui_mode == UiMode::InputMap) {
          input_map_index = (input_map_index - event.wheel.y +
                             static_cast<int>(map_actions.size()))
                            % static_cast<int>(map_actions.size());
        }
      } else if (event.type == SDL_MOUSEBUTTONDOWN) {
        if (event.button.button == SDL_BUTTON_LEFT) {
          int mx = event.button.x;
          int my = event.button.y;
          const UiThemeDef& theme_local = theme_def(ui_theme);
          if (ui_mode == UiMode::Menu) {
            auto labels = menu_labels();
            SDL_Rect panel = menu_panel_rect();
            int item_y = panel.y + theme_local.panel_padding;
            int line_h = 18;
            for (std::size_t i = 0; i < labels.size(); ++i) {
              SDL_Rect row{panel.x + theme_local.panel_padding, item_y - 2,
                           panel.w - theme_local.panel_padding * 2, line_h};
              if (point_in_rect(mx, my, row)) {
                menu_index = static_cast<int>(i);
                apply_menu_action(menu_index);
                break;
              }
              item_y += line_h + 6;
            }
          } else if (ui_mode == UiMode::Settings) {
            auto labels = settings_labels();
            SDL_Rect panel = settings_panel_rect();
            int item_y = panel.y + theme_local.panel_padding;
            int line_h = 18;
            for (std::size_t i = 0; i < labels.size(); ++i) {
              SDL_Rect row{panel.x + theme_local.panel_padding, item_y - 2,
                           panel.w - theme_local.panel_padding * 2, line_h};
              if (point_in_rect(mx, my, row)) {
                settings_index = static_cast<int>(i);
                apply_settings_action(settings_index);
                break;
              }
              item_y += line_h + 6;
            }
          } else if (ui_mode == UiMode::InputMap) {
            SDL_Rect panel = input_map_panel_rect();
            int item_y = panel.y + theme_local.panel_padding + 18;
            int line_h = 18;
            for (std::size_t i = 0; i < map_actions.size(); ++i) {
              SDL_Rect row{panel.x + theme_local.panel_padding, item_y - 2,
                           panel.w - theme_local.panel_padding * 2, line_h};
              if (point_in_rect(mx, my, row)) {
                input_map_index = static_cast<int>(i);
                if (event.button.clicks >= 2) {
                  rebind_action = map_actions[i];
                  rebind_active = true;
                }
                break;
              }
              item_y += line_h + 6;
            }
          } else if (ui_mode == UiMode::Launcher) {
            LauncherLayout layout = launcher_layout();
            launcher_visible = layout.visible;
            if (point_in_rect(mx, my, layout.search_box)) {
              search_focus = true;
            } else {
              search_focus = false;
            }
            if (point_in_rect(mx, my, layout.settings_button)) {
              ui_mode = UiMode::Settings;
              settings_index = 0;
            } else if (point_in_rect(mx, my, layout.filter_hit)) {
              cycle_launcher_filter(1);
            } else if (point_in_rect(mx, my, layout.override_button)) {
              int selected = selected_rom_index();
              if (selected >= 0 && selected < static_cast<int>(roms.size())) {
                toggle_rom_override_for(roms[selected]);
              }
            } else if (point_in_rect(mx, my, layout.play_button)) {
              launch_selected();
            } else if (point_in_rect(mx, my, layout.favorite_button)) {
              toggle_favorite();
            } else if (point_in_rect(mx, my, layout.list_panel)) {
              int rel_y = my - layout.start_y;
              if (rel_y >= 0) {
                int idx = rel_y / layout.card_h + launcher_scroll;
                if (idx >= 0 && idx < static_cast<int>(launcher_list.size())) {
                  launcher_index = idx;
                  if (event.button.clicks >= 2) {
                    launch_selected();
                  }
                }
              }
            }
          }
        }
      } else if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
        bool pressed = (event.type == SDL_CONTROLLERBUTTONDOWN);
        SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(event.cbutton.button);
        if (button == SDL_CONTROLLER_BUTTON_GUIDE && pressed) {
          if (ui_mode == UiMode::Menu) {
            ui_mode = UiMode::Hidden;
          } else if (ui_mode == UiMode::Settings) {
            ui_mode = UiMode::Menu;
          } else if (ui_mode == UiMode::InputMap) {
            if (rebind_active) {
              rebind_active = false;
            } else {
              ui_mode = UiMode::Settings;
            }
          } else if (ui_mode == UiMode::Hidden && game_loaded) {
            ui_mode = UiMode::Menu;
            menu_index = 0;
          } else if (ui_mode == UiMode::Launcher) {
            if (game_loaded) {
              ui_mode = UiMode::Hidden;
            }
          }
          continue;
        }

        if (ui_mode == UiMode::InputMap && pressed) {
          if (rebind_active) {
            input_config.set_controller_button_binding(rebind_action, static_cast<int>(button));
            save_input_map();
            rebind_active = false;
          } else {
            if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
              input_map_index = (input_map_index - 1 + static_cast<int>(map_actions.size()))
                                % static_cast<int>(map_actions.size());
            } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
              input_map_index = (input_map_index + 1)
                                % static_cast<int>(map_actions.size());
            } else if (button == SDL_CONTROLLER_BUTTON_A || button == SDL_CONTROLLER_BUTTON_START) {
              rebind_action = map_actions[static_cast<std::size_t>(input_map_index)];
              rebind_active = true;
            } else if (button == SDL_CONTROLLER_BUTTON_B) {
              ui_mode = UiMode::Settings;
            } else if (button == SDL_CONTROLLER_BUTTON_Y) {
              input_config.clear_controller_binding(
                  map_actions[static_cast<std::size_t>(input_map_index)]);
              save_input_map();
            }
          }
          continue;
        }

        if (ui_mode == UiMode::Menu && pressed) {
          if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            move_menu(-1);
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            move_menu(1);
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
            set_theme(next_theme(ui_theme, -1));
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            set_theme(next_theme(ui_theme, 1));
          } else if (button == SDL_CONTROLLER_BUTTON_A || button == SDL_CONTROLLER_BUTTON_START) {
            apply_menu_action(menu_index);
          } else if (button == SDL_CONTROLLER_BUTTON_B) {
            ui_mode = UiMode::Hidden;
          }
          continue;
        }

        if (ui_mode == UiMode::Settings && pressed) {
          if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            move_settings(-1);
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            move_settings(1);
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
            adjust_settings(settings_index, -1);
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            adjust_settings(settings_index, 1);
          } else if (button == SDL_CONTROLLER_BUTTON_A || button == SDL_CONTROLLER_BUTTON_START) {
            apply_settings_action(settings_index);
          } else if (button == SDL_CONTROLLER_BUTTON_B) {
            ui_mode = UiMode::Menu;
          }
          continue;
        }

        if (ui_mode == UiMode::Launcher && pressed) {
          if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            move_launcher(-1);
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            move_launcher(1);
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
            set_theme(next_theme(ui_theme, -1));
          } else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            set_theme(next_theme(ui_theme, 1));
          } else if (button == SDL_CONTROLLER_BUTTON_A || button == SDL_CONTROLLER_BUTTON_START) {
            launch_selected();
          } else if (button == SDL_CONTROLLER_BUTTON_X) {
            cycle_launcher_filter(1);
          } else if (button == SDL_CONTROLLER_BUTTON_Y) {
            toggle_favorite();
          } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
            int selected = selected_rom_index();
            if (selected >= 0 && selected < static_cast<int>(roms.size())) {
              toggle_rom_override_for(roms[selected]);
            }
          } else if (button == SDL_CONTROLLER_BUTTON_B) {
            if (game_loaded) {
              ui_mode = UiMode::Hidden;
            } else {
              running = false;
            }
          }
          continue;
        }

        if (ui_mode == UiMode::Hidden) {
          if (update_joypad_button(button, pressed)) {
            if (show_hud) {
              touch_hud();
            }
            std::uint8_t combined = static_cast<std::uint8_t>(key_state & pad_state);
            if (combined != joypad_state) {
              joypad_state = combined;
              emu_pending_joypad.store(joypad_state, std::memory_order_relaxed);
              emu_pending_joypad_irq.store(true, std::memory_order_relaxed);
            }
          }
        }
      } else if (event.type == SDL_CONTROLLERAXISMOTION) {
        if (ui_mode == UiMode::InputMap && rebind_active) {
          int value = event.caxis.value;
          int deadzone = input_config.axis_deadzone();
          if (value > deadzone || value < -deadzone) {
            bool positive = value > 0;
            input_config.set_controller_axis_binding(
                rebind_action, static_cast<int>(event.caxis.axis), positive);
            save_input_map();
            rebind_active = false;
          }
          continue;
        }
        if (ui_mode == UiMode::Hidden) {
          if (update_joypad_axis(static_cast<SDL_GameControllerAxis>(event.caxis.axis), event.caxis.value)) {
            if (show_hud) {
              touch_hud();
            }
            std::uint8_t combined = static_cast<std::uint8_t>(key_state & pad_state);
            if (combined != joypad_state) {
              joypad_state = combined;
              emu_pending_joypad.store(joypad_state, std::memory_order_relaxed);
              emu_pending_joypad_irq.store(true, std::memory_order_relaxed);
            }
          }
        }
      }
    }
    if (ui_mode != last_mode) {
      update_text_input();
      if (last_mode != UiMode::Hidden && ui_mode == UiMode::Hidden) {
        key_state = 0xFF;
        pad_state = 0xFF;
        joypad_state = 0xFF;
        emu_pending_joypad.store(joypad_state, std::memory_order_relaxed);
        emu_pending_joypad_irq.store(false, std::memory_order_relaxed);
        if (show_hud) {
          touch_hud();
        }
      }
      last_mode = ui_mode;
    }
    float target_opacity = (ui_mode == UiMode::Hidden) ? 0.0f : 1.0f;
    ui_opacity += (target_opacity - ui_opacity) * 0.18f;
    ui_opacity = std::clamp(ui_opacity, 0.0f, 1.0f);
    bool run_emulation = (ui_mode == UiMode::Hidden && game_loaded);
    emu_active.store(run_emulation, std::memory_order_relaxed);
    emu_game_loaded.store(game_loaded, std::memory_order_relaxed);

    if (ui_mode == UiMode::Hidden && game_loaded) {
      std::uint8_t polled_keys = poll_keyboard_state();
      std::uint8_t polled_pad = poll_controller_state();
      bool input_changed = false;
      if (polled_keys != key_state) {
        key_state = polled_keys;
        input_changed = true;
      }
      if (polled_pad != pad_state) {
        pad_state = polled_pad;
        input_changed = true;
      }
      if (input_changed && show_hud) {
        touch_hud();
      }
      std::uint8_t combined = static_cast<std::uint8_t>(key_state & pad_state);
      if (combined != joypad_state) {
        joypad_state = combined;
        emu_pending_joypad.store(joypad_state, std::memory_order_relaxed);
        emu_pending_joypad_irq.store(true, std::memory_order_relaxed);
      }
      fps_actual = emu_fps_actual.load(std::memory_order_relaxed);
    }
    log_boot_state(frame_count);
    ++frame_count;

    bool frame_updated = false;
    if (game_loaded && texture) {
      std::uint64_t latest_serial = emu_frame_serial.load(std::memory_order_relaxed);
      if (latest_serial != render_frame_serial) {
        std::scoped_lock frame_lock(frame_mutex);
        latest_serial = emu_frame_serial.load(std::memory_order_relaxed);
        if (latest_serial != render_frame_serial) {
          render_fb_width = emu_fb_width;
          render_fb_height = emu_fb_height;
          render_fb_stride_bytes = emu_fb_stride_bytes;
          render_framebuffer = emu_framebuffer;
          render_frame_serial = latest_serial;
          frame_updated = true;
        }
      }
    }

    if (ui_mode == UiMode::Hidden && target_fps <= 0.0 && !frame_updated) {
      emu_audio_enabled.store(audio_enabled, std::memory_order_relaxed);
      SDL_Delay(1);
      continue;
    }

    const UiThemeDef& theme = theme_def(ui_theme);

    SDL_RenderClear(renderer);
    if (game_loaded && texture) {
      const std::uint32_t* frame_data =
          render_framebuffer.empty() ? nullptr : render_framebuffer.data();
      bool dims_match = (render_fb_width == fb_width && render_fb_height == fb_height &&
                         render_fb_stride_bytes == fb_width * 4);
      if (frame_data != nullptr && dims_match) {
        if (video_filter == VideoFilter::Crt) {
          int stride_words = render_fb_stride_bytes / 4;
          apply_crt_filter(frame_data, stride_words, filtered_frame.data(),
                           prev_frame.data(), fb_width, fb_height, crt_reset, &filter_pool);
          crt_reset = false;
          SDL_UpdateTexture(texture, nullptr, filtered_frame.data(), fb_width * 4);
        } else {
          SDL_UpdateTexture(texture, nullptr, frame_data, render_fb_stride_bytes);
        }
      } else if (!dims_match) {
        crt_reset = true;
      }
      SDL_RenderCopy(renderer, texture, nullptr, nullptr);
      SDL_Rect screen{0, 0, window_w, window_h};
      if (video_filter == VideoFilter::Scanlines) {
        draw_scanlines(renderer, screen, 45);
      } else if (video_filter == VideoFilter::Lcd) {
        draw_lcd(renderer, screen, 35);
      }
    } else {
      SDL_Rect bg{0, 0, window_w, window_h};
      fill_rect(renderer, bg, theme.bg_primary);
      draw_menu_decor(renderer, bg, theme);
    }

    if (ui_opacity > 0.01f) {
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
      int alpha = static_cast<int>(140 * ui_opacity);
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
      SDL_Rect dim{0, 0, window_w, window_h};
      SDL_RenderFillRect(renderer, &dim);
    }

    if (show_help && ui_mode == UiMode::Hidden) {
      int panel_width = window_w > 360 ? 360 : window_w - 20;
      int panel_height = 160;
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
    if (show_hud && ui_mode == UiMode::Hidden && game_loaded) {
      double hud_alpha = 1.0;
      if (hud_timeout_seconds > 0) {
        double elapsed = (SDL_GetPerformanceCounter() - hud_last_activity) / fps_freq;
        if (elapsed > hud_timeout_seconds) {
          double fade = 0.4;
          double t = (elapsed - hud_timeout_seconds) / fade;
          hud_alpha = std::clamp(1.0 - t, 0.0, 1.0);
        }
      }
      if (hud_alpha > 0.01) {
        int panel_w = hud_compact ? std::min(200, window_w - 20)
                                  : std::min(260, window_w - 20);
        int panel_h = hud_compact ? 46 : 92;
        int pad = 10;
        int panel_x = (hud_corner == HudCorner::TopRight ||
                       hud_corner == HudCorner::BottomRight)
                          ? window_w - panel_w - pad
                          : pad;
        int panel_y = (hud_corner == HudCorner::BottomLeft ||
                       hud_corner == HudCorner::BottomRight)
                          ? window_h - panel_h - pad
                          : pad;
        SDL_Rect panel{panel_x, panel_y, panel_w, panel_h};
        draw_panel_alpha(renderer, panel, theme, static_cast<float>(0.9f * hud_alpha));
        SDL_Color text = with_alpha(theme.text, static_cast<float>(hud_alpha));
        std::string title = upper_ascii(current_rom_title);
        int title_max_w = panel.w - 14;
        while (!title.empty() && text_width(title, 2) > title_max_w) {
          title.pop_back();
        }
        draw_text(renderer, panel.x + 8, panel.y + 8, 2, title, text);

        if (hud_compact) {
          std::string line = system_short(system);
          line += "  ";
          line += std::to_string(static_cast<int>(std::round(fps_actual)));
          line += " FPS";
          draw_text(renderer, panel.x + 8, panel.y + 26, 2, line, text);
        } else {
          std::string sys = "SYS " + system_short(system);
          std::string fps_line = "FPS " + std::to_string(static_cast<int>(std::round(fps_actual)));
          std::string audio_line = audio_enabled ? "AUDIO ON" : "AUDIO OFF";
          std::string filter_line = "FILTER " + upper_ascii(video_filter_name(video_filter));
          draw_text(renderer, panel.x + 8, panel.y + 26, 2, sys, text);
          draw_text(renderer, panel.x + 90, panel.y + 26, 2, fps_line, text);
          draw_text(renderer, panel.x + 8, panel.y + 44, 2, audio_line, text);
          draw_text(renderer, panel.x + 8, panel.y + 62, 2, filter_line, text);

          struct InputChip { const char* label; int bit; };
          const InputChip chips[] = {
              {"R", 0}, {"L", 1}, {"U", 2}, {"D", 3},
              {"A", 4}, {"B", 5}, {"SEL", 6}, {"STA", 7},
          };
          int chip_x = panel.x + 8;
          int chip_y = panel.y + panel.h - 16;
          for (const auto& chip : chips) {
            bool pressed = (joypad_state & (1u << chip.bit)) == 0;
            SDL_Rect box{chip_x, chip_y, 18, 10};
            if (pressed) {
              fill_rect(renderer, box, with_alpha(theme.accent, static_cast<float>(hud_alpha)));
            } else {
              SDL_Color border = with_alpha(theme.panel_border, static_cast<float>(hud_alpha));
              SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
              SDL_RenderDrawRect(renderer, &box);
            }
            draw_text(renderer, box.x + 3, box.y + 1, 1, chip.label, text);
            chip_x += (chip.bit == 3) ? 26 : 22;
          }
        }
      }
    }

    if (ui_mode == UiMode::Menu) {
      auto labels = menu_labels();
      SDL_Rect panel = menu_panel_rect();
      float anim = ui_opacity;
      SDL_Rect panel_anim = panel;
      panel_anim.y += static_cast<int>((1.0f - anim) * 10.0f);
      draw_panel_alpha(renderer, panel_anim, theme, anim);
      draw_menu_decor(renderer, panel, theme);
      SDL_Color text = with_alpha(theme.text, anim);
      SDL_Color accent = with_alpha(theme.accent, anim);
      int item_y = panel_anim.y + theme.panel_padding;
      int line_h = 18;
      for (std::size_t i = 0; i < labels.size(); ++i) {
        SDL_Rect row{panel_anim.x + theme.panel_padding, item_y - 2,
                     panel_anim.w - theme.panel_padding * 2, line_h};
        if (static_cast<int>(i) == menu_index) {
          fill_rect(renderer, row,
                    SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                              static_cast<Uint8>(80 * anim)});
        }
        draw_text(renderer, row.x + 4, row.y + 4, 2, upper_ascii(labels[i]), text);
        item_y += line_h + 6;
      }
      std::string footer = "ENTER/A SELECT  ESC/B CLOSE";
      draw_text(renderer, panel_anim.x + theme.panel_padding,
                panel_anim.y + panel_anim.h - 22, 2, footer, text);
    } else if (ui_mode == UiMode::Settings) {
      auto labels = settings_labels();
      SDL_Rect panel = settings_panel_rect();
      float anim = ui_opacity;
      SDL_Rect panel_anim = panel;
      panel_anim.y += static_cast<int>((1.0f - anim) * 10.0f);
      draw_panel_alpha(renderer, panel_anim, theme, anim);
      draw_menu_decor(renderer, panel, theme);
      SDL_Color text = with_alpha(theme.text, anim);
      int item_y = panel_anim.y + theme.panel_padding;
      int line_h = 18;
      for (std::size_t i = 0; i < labels.size(); ++i) {
        SDL_Rect row{panel_anim.x + theme.panel_padding, item_y - 2,
                     panel_anim.w - theme.panel_padding * 2, line_h};
        if (static_cast<int>(i) == settings_index) {
          fill_rect(renderer, row,
                    SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                              static_cast<Uint8>(80 * anim)});
        }
        draw_text(renderer, row.x + 4, row.y + 4, 2, upper_ascii(labels[i]), text);
        item_y += line_h + 6;
      }
      std::string footer = "LEFT/RIGHT ADJUST  ENTER/A TOGGLE  ESC/B BACK";
      draw_text(renderer, panel_anim.x + theme.panel_padding,
                panel_anim.y + panel_anim.h - 22, 2, footer, text);
    } else if (ui_mode == UiMode::InputMap) {
      SDL_Rect panel = input_map_panel_rect();
      float anim = ui_opacity;
      SDL_Rect panel_anim = panel;
      panel_anim.y += static_cast<int>((1.0f - anim) * 10.0f);
      draw_panel_alpha(renderer, panel_anim, theme, anim);
      draw_menu_decor(renderer, panel, theme);
      SDL_Color text = with_alpha(theme.text, anim);
      int header_y = panel_anim.y + theme.panel_padding;
      draw_text(renderer, panel_anim.x + theme.panel_padding, header_y, 2, "ACTION", text);
      draw_text(renderer, panel_anim.x + panel_anim.w / 2 - 20, header_y, 2, "KEY", text);
      draw_text(renderer, panel_anim.x + panel_anim.w - theme.panel_padding - 80, header_y, 2, "PAD", text);

      int item_y = header_y + 18;
      int line_h = 18;
      for (std::size_t i = 0; i < map_actions.size(); ++i) {
        SDL_Rect row{panel_anim.x + theme.panel_padding, item_y - 2,
                     panel_anim.w - theme.panel_padding * 2, line_h};
        if (static_cast<int>(i) == input_map_index) {
          fill_rect(renderer, row,
                    SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                              static_cast<Uint8>(80 * anim)});
        }
        auto action = map_actions[i];
        std::string action_label = upper_ascii(gbemu::common::action_name(action));
        draw_text(renderer, row.x + 4, row.y + 4, 2, action_label, text);

        std::string key = key_label(input_config.key_for(action));
        draw_text(renderer, panel_anim.x + panel_anim.w / 2 - 20, row.y + 4, 2, key, text);

        std::string pad = "NONE";
        if (auto button = input_config.controller_button_for_action(action)) {
          pad = controller_button_label(*button);
        } else if (auto axis = input_config.controller_axis_for_action(action)) {
          pad = controller_axis_label(axis->first, axis->second);
        }
        draw_text(renderer, panel_anim.x + panel_anim.w - theme.panel_padding - 80,
                  row.y + 4, 2, pad, text);
        item_y += line_h + 6;
      }

      if (rebind_active) {
        SDL_Rect overlay{panel_anim.x + 20, panel_anim.y + panel_anim.h / 2 - 20,
                         panel_anim.w - 40, 40};
        draw_panel_alpha(renderer, overlay, theme, anim);
        draw_text(renderer, overlay.x + 10, overlay.y + 12, 2,
                  "PRESS A KEY OR BUTTON...", text);
      }

      std::string footer = "ENTER/A REBIND  DEL CLEAR PAD  BACKSPACE CLEAR KEY  ESC/B BACK";
      draw_text(renderer, panel_anim.x + theme.panel_padding,
                panel_anim.y + panel_anim.h - 22, 2, footer, text);
    } else if (ui_mode == UiMode::Launcher) {
      LauncherLayout layout = launcher_layout();
      launcher_visible = layout.visible;
      SDL_Rect header = layout.header;
      float anim = ui_opacity;
      fill_rect(renderer, header, with_alpha(theme.bg_secondary, anim));
      SDL_Color text = with_alpha(theme.text, anim);
      SDL_Color accent = with_alpha(theme.accent, anim);
      draw_text(renderer, 14, 12, 2, "GBEMU LAUNCHER", text);
      SDL_Rect search_box = layout.search_box;
      fill_rect(renderer, search_box, with_alpha(theme.panel, anim));
      SDL_Color border = search_focus ? accent : with_alpha(theme.panel_border, anim);
      SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
      SDL_RenderDrawRect(renderer, &search_box);
      std::string search_text;
      if (launcher_search.empty()) {
        search_text = search_focus ? "TYPE TO SEARCH" : "/ TO SEARCH";
      } else {
        search_text = launcher_search;
      }
      if (search_focus && ((frame_count / 30) % 2 == 0)) {
        search_text.push_back('|');
      }
      std::string search_display = upper_ascii(search_text);
      int max_search_w = search_box.w - 10;
      while (text_width(search_display, 2) > max_search_w && !search_display.empty()) {
        search_display.erase(search_display.begin());
      }
      draw_text(renderer, search_box.x + 6, search_box.y + 6, 2, search_display, text);

      SDL_Rect settings_button = layout.settings_button;
      draw_panel_alpha(renderer, settings_button, theme, anim);
      if (point_in_rect(mouse_x, mouse_y, settings_button)) {
        fill_rect(renderer, settings_button,
                  SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                            static_cast<Uint8>(60 * anim)});
      }
      draw_text(renderer, settings_button.x + 6, settings_button.y + 6, 2, "SETTINGS", text);

      std::string theme_label = upper_ascii(ui_theme_name(ui_theme));
      int theme_w = text_width(theme_label, 2);
      draw_text(renderer, window_w - 14 - theme_w, 4, 2, theme_label, accent);
      std::string filter_label = "FILTER: " + upper_ascii(launcher_filter_name(launcher_filter));
      std::string count_label = std::to_string(launcher_list.size()) + "/" + std::to_string(roms.size());
      int filter_w = text_width(filter_label, 2);
      int count_w = text_width(count_label, 2);
      draw_text(renderer, window_w - 14 - filter_w, 20, 2, filter_label, text);
      draw_text(renderer, window_w - 14 - count_w, 30, 2, count_label, accent);

      SDL_Rect list_panel = layout.list_panel;
      SDL_Rect detail_panel = layout.detail_panel;
      SDL_Rect list_anim = list_panel;
      SDL_Rect detail_anim = detail_panel;
      list_anim.y += static_cast<int>((1.0f - anim) * 14.0f);
      detail_anim.y += static_cast<int>((1.0f - anim) * 14.0f);
      draw_panel_alpha(renderer, list_anim, theme, anim);
      draw_panel_alpha(renderer, detail_anim, theme, anim);

      int card_h = layout.card_h;
      int visible = layout.visible;
      if (launcher_index < launcher_scroll) {
        launcher_scroll = launcher_index;
      } else if (launcher_index >= launcher_scroll + visible) {
        launcher_scroll = launcher_index - visible + 1;
      }

      auto color_from_hash = [&](const std::string& name) {
        std::uint32_t h = 0;
        for (char c : name) {
          h = (h * 131) + static_cast<std::uint8_t>(c);
        }
        std::uint8_t r = static_cast<std::uint8_t>(theme.accent.r / 2 + (h & 0x3F));
        std::uint8_t g = static_cast<std::uint8_t>(theme.accent.g / 2 + ((h >> 6) & 0x3F));
        std::uint8_t b = static_cast<std::uint8_t>(theme.accent.b / 2 + ((h >> 12) & 0x3F));
        return SDL_Color{r, g, b, 255};
      };

      int start_y = layout.start_y;
      if (roms.empty()) {
        draw_text(renderer, list_anim.x + 12, list_anim.y + 12, 2, "NO ROMS FOUND", text);
      } else if (launcher_list.empty()) {
        draw_text(renderer, list_anim.x + 12, list_anim.y + 12, 2, "NO MATCHES", text);
      } else {
        for (int i = 0; i < visible; ++i) {
          int idx = launcher_scroll + i;
          if (idx >= static_cast<int>(launcher_list.size())) {
            break;
          }
          int rom_index = launcher_list[idx];
          const auto& entry = roms[rom_index];
          SDL_Rect card{list_anim.x + 8, (start_y + i * card_h),
                        list_anim.w - 16, card_h - 4};
          SDL_Color card_color = (i % 2 == 0) ? theme.bg_secondary : theme.panel;
          fill_rect(renderer, card, with_alpha(card_color, anim));
          if (idx == launcher_index) {
            fill_rect(renderer, card, SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                                                static_cast<Uint8>(70 * anim)});
            SDL_SetRenderDrawColor(renderer, theme.accent.r, theme.accent.g, theme.accent.b,
                                   static_cast<Uint8>(255 * anim));
            SDL_RenderDrawRect(renderer, &card);
          }
          SDL_Rect art{card.x + 6, card.y + 6, 24, 24};
          if (CoverTexture* cover = get_cover_texture(entry.cover_path)) {
            SDL_Rect dst = fit_rect(art.w, art.h, cover->width, cover->height, art.x, art.y);
            SDL_RenderCopy(renderer, cover->texture, nullptr, &dst);
          } else {
            SDL_Color chip = color_from_hash(entry.title);
            fill_rect(renderer, art, chip);
          }
          std::string line = upper_ascii(entry.title);
          draw_text(renderer, card.x + 36, card.y + 6, 2, line, text);
          std::string meta = system_short(entry.system) + "  " + format_bytes(entry.size);
          draw_text(renderer, card.x + 36, card.y + 18, 2, meta, text);
          int badge_right = card.x + card.w - 6;
          if (favorite_paths.count(entry.path)) {
            std::string fav = "FAV";
            int fav_w = text_width(fav, 2);
            draw_text(renderer, badge_right - fav_w, card.y + 6, 2, fav, accent);
            badge_right -= fav_w + 6;
          }
          if (rom_override_exists(entry.path)) {
            std::string ovr = "OVR";
            int ovr_w = text_width(ovr, 2);
            draw_text(renderer, badge_right - ovr_w, card.y + 6, 2, ovr, accent);
          }
        }
      }

      if (!launcher_list.empty()) {
        int rom_index = launcher_list[launcher_index];
        const auto& entry = roms[rom_index];
        std::string title = upper_ascii(entry.title);
        draw_text(renderer, detail_anim.x + 10, detail_anim.y + 10, 2, title, text);
        std::string info = "SYSTEM: " + system_short(entry.system);
        draw_text(renderer, detail_anim.x + 10, detail_anim.y + 28, 2, info, text);
        std::string size = "SIZE: " + format_bytes(entry.size);
        draw_text(renderer, detail_anim.x + 10, detail_anim.y + 44, 2, size, text);
        std::string path = upper_ascii(std::filesystem::path(entry.path).filename().string());
        draw_text(renderer, detail_anim.x + 10, detail_anim.y + 62, 2, path, text);

        int badge_y = detail_anim.y + 78;
        if (favorite_paths.count(entry.path)) {
          draw_text(renderer, detail_anim.x + 10, badge_y, 2, "FAVORITE", accent);
          badge_y += 14;
        }
        if (rom_override_exists(entry.path)) {
          draw_text(renderer, detail_anim.x + 10, badge_y, 2, "OVERRIDE", accent);
        }

        int art_y = detail_anim.y + 96;
        int art_bottom = detail_anim.y + (layout.override_button.y - detail_panel.y) - 12;
        int art_h = art_bottom - art_y;
        if (art_h > 30) {
          SDL_Rect art_box{detail_anim.x + 10, art_y, detail_anim.w - 20, art_h};
          draw_panel_alpha(renderer, art_box, theme, anim);
          if (CoverTexture* cover = get_cover_texture(entry.cover_path)) {
            SDL_Rect dst = fit_rect(art_box.w, art_box.h, cover->width, cover->height,
                                    art_box.x, art_box.y);
            SDL_RenderCopy(renderer, cover->texture, nullptr, &dst);
          } else {
            SDL_Color chip = color_from_hash(entry.title);
            SDL_Rect chip_rect{art_box.x + 8, art_box.y + 8, 28, 28};
            fill_rect(renderer, chip_rect, chip);
            draw_text(renderer, art_box.x + 44, art_box.y + 14, 2, "NO COVER", text);
          }
        }

        SDL_Rect override_button = layout.override_button;
        SDL_Rect play_button = layout.play_button;
        SDL_Rect favorite_button = layout.favorite_button;
        draw_panel_alpha(renderer, override_button, theme, anim);
        draw_panel_alpha(renderer, play_button, theme, anim);
        draw_panel_alpha(renderer, favorite_button, theme, anim);
        if (point_in_rect(mouse_x, mouse_y, override_button)) {
          fill_rect(renderer, override_button,
                    SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                              static_cast<Uint8>(60 * anim)});
        }
        if (point_in_rect(mouse_x, mouse_y, play_button)) {
          fill_rect(renderer, play_button,
                    SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                              static_cast<Uint8>(70 * anim)});
        }
        if (point_in_rect(mouse_x, mouse_y, favorite_button)) {
          fill_rect(renderer, favorite_button,
                    SDL_Color{theme.accent.r, theme.accent.g, theme.accent.b,
                              static_cast<Uint8>(70 * anim)});
        }
        std::string override_label = rom_override_exists(entry.path)
                                         ? "PER-ROM: ON"
                                         : "PER-ROM: OFF";
        draw_text(renderer, override_button.x + 8, override_button.y + 6, 2,
                  override_label, text);
        draw_text(renderer, play_button.x + 8, play_button.y + 6, 2, "PLAY", text);
        std::string fav_label = favorite_paths.count(entry.path) ? "UNFAV" : "FAVORITE";
        draw_text(renderer, favorite_button.x + 8, favorite_button.y + 6, 2, fav_label, text);
      }

      std::string actions = "ENTER/A START  / SEARCH  TAB/X FILTER  F/Y FAVORITE  O OVERRIDE  R RESCAN  ESC/B BACK/QUIT";
      draw_text(renderer, detail_anim.x + 10, detail_anim.y + detail_anim.h - 22, 2,
                actions, text);

      if (!launcher_error.empty()) {
        SDL_Rect err_panel{detail_panel.x + 10, detail_panel.y + detail_panel.h - 50,
                           detail_panel.w - 20, 20};
        fill_rect(renderer, err_panel, SDL_Color{120, 20, 20, 180});
        draw_text(renderer, err_panel.x + 4, err_panel.y + 4, 2, "FAILED TO LOAD", theme.text);
      }
    } else if (theme_toast_frames > 0) {
      SDL_Rect toast{10, window_h - 36, 220, 26};
      draw_panel(renderer, toast, theme);
      std::string toast_text = "THEME: " + upper_ascii(ui_theme_name(ui_theme));
      draw_text(renderer, toast.x + 8, toast.y + 6, 2, toast_text, theme.text);
      --theme_toast_frames;
    }
    SDL_RenderPresent(renderer);
    emu_audio_enabled.store(audio_enabled, std::memory_order_relaxed);
    pacer.sleep();
  }

  emu_active.store(false, std::memory_order_relaxed);
  emu_running.store(false, std::memory_order_relaxed);
  if (emu_thread.joinable()) {
    emu_thread.join();
  }
  emu_thread_started = false;

  save_ui_state(ui_state_path.string(), ui_theme, scale, target_fps,
                input_config.axis_deadzone(), show_help, audio_enabled,
                show_hud, hud_corner, hud_compact, hud_timeout_seconds,
                video_filter);
  save_state();

  for (auto& entry : controllers) {
    SDL_GameControllerClose(entry.second);
  }
  controllers.clear();

  if (audio_device != 0) {
    SDL_CloseAudioDevice(audio_device);
  }

  for (auto& entry : cover_cache) {
    if (entry.second.texture) {
      SDL_DestroyTexture(entry.second.texture);
    }
  }
  cover_cache.clear();

#ifdef GBEMU_HAS_SDL_IMAGE
  IMG_Quit();
#endif

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
