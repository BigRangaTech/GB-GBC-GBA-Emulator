#include "gba_core.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

#include "state_io.h"
#include "timing.h"

namespace gbemu::core {

namespace {

constexpr std::uint32_t kRegDispstat = 0x04000004;
constexpr std::uint32_t kRegVcount = 0x04000006;
constexpr std::uint32_t kRegIe = 0x04000200;
constexpr std::uint32_t kRegIf = 0x04000202;
constexpr std::uint32_t kRegIme = 0x04000208;
constexpr std::uint16_t kIrqValidMask = 0x3FFF;
constexpr int kGbaLinesPerFrame = 228;
constexpr int kGbaVblankStart = 160;
constexpr int kGbaVisibleCycles = 960;
constexpr int kGbaCyclesPerLine = 1232;
constexpr std::uint32_t kSwiDecompMaxOutput = 8u * 1024u * 1024u;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

bool in_window_range(int value, int start, int end) {
  if (start == end) {
    return false;
  }
  if (start < end) {
    return value >= start && value < end;
  }
  return value >= start || value < end;
}

std::uint32_t bgr555_to_argb(std::uint16_t pixel) {
  std::uint8_t r = static_cast<std::uint8_t>(((pixel >> 0) & 0x1F) * 255 / 31);
  std::uint8_t g = static_cast<std::uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
  std::uint8_t b = static_cast<std::uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
  return (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
         (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

std::uint32_t isqrt32(std::uint32_t value) {
  std::uint32_t root = 0;
  std::uint32_t bit = 1u << 30;
  while (bit > value) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (value >= root + bit) {
      value -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

bool gba_signed_div(std::int32_t num,
                    std::int32_t den,
                    std::int32_t* quotient_out,
                    std::int32_t* remainder_out,
                    std::uint32_t* abs_quotient_out) {
  if (den == 0) {
    return false;
  }

  std::int32_t quotient = 0;
  std::int32_t remainder = 0;
  if (num == std::numeric_limits<std::int32_t>::min() && den == -1) {
    quotient = std::numeric_limits<std::int32_t>::min();
    remainder = 0;
  } else {
    quotient = num / den;
    remainder = num % den;
  }

  std::uint32_t abs_quotient = 0;
  if (quotient < 0) {
    if (quotient == std::numeric_limits<std::int32_t>::min()) {
      abs_quotient = 0x80000000u;
    } else {
      abs_quotient = static_cast<std::uint32_t>(-quotient);
    }
  } else {
    abs_quotient = static_cast<std::uint32_t>(quotient);
  }

  *quotient_out = quotient;
  *remainder_out = remainder;
  *abs_quotient_out = abs_quotient;
  return true;
}

std::int32_t asr_div_pow2(std::int64_t value, int shift) {
  if (shift <= 0) {
    return static_cast<std::int32_t>(value);
  }
  std::int64_t denom = 1LL << shift;
  if (value >= 0) {
    return static_cast<std::int32_t>(value / denom);
  }
  return static_cast<std::int32_t>(-(((-value) + denom - 1) / denom));
}

void calc_affine_matrix(std::int16_t sx,
                        std::int16_t sy,
                        std::uint16_t angle,
                        std::int16_t* pa_out,
                        std::int16_t* pb_out,
                        std::int16_t* pc_out,
                        std::int16_t* pd_out) {
  std::uint8_t phase = static_cast<std::uint8_t>(angle >> 8);
  double theta = static_cast<double>(phase) * (kTwoPi / 256.0);
  std::int32_t cos14 = static_cast<std::int32_t>(std::lround(std::cos(theta) * 16384.0));
  std::int32_t sin14 = static_cast<std::int32_t>(std::lround(std::sin(theta) * 16384.0));
  std::int32_t pa = asr_div_pow2(static_cast<std::int64_t>(sx) * cos14, 14);
  std::int32_t pb = asr_div_pow2(-static_cast<std::int64_t>(sx) * sin14, 14);
  std::int32_t pc = asr_div_pow2(static_cast<std::int64_t>(sy) * sin14, 14);
  std::int32_t pd = asr_div_pow2(static_cast<std::int64_t>(sy) * cos14, 14);
  *pa_out = static_cast<std::int16_t>(pa);
  *pb_out = static_cast<std::int16_t>(pb);
  *pc_out = static_cast<std::int16_t>(pc);
  *pd_out = static_cast<std::int16_t>(pd);
}

bool bios_has_real_code(const std::vector<std::uint8_t>& bios) {
  for (std::uint8_t value : bios) {
    if (value != 0) {
      return true;
    }
  }
  return false;
}

std::uint16_t rad_to_bios_angle(double radians) {
  double scaled = std::lround(radians * (32768.0 / kPi));
  std::int64_t value = static_cast<std::int64_t>(scaled);
  value %= 65536;
  if (value < 0) {
    value += 65536;
  }
  return static_cast<std::uint16_t>(value);
}

bool decode_lz77(const GbaBus& bus,
                 std::uint32_t src,
                 std::vector<std::uint8_t>* out) {
  if (!out) {
    return false;
  }
  std::uint8_t header = bus.read8(src);
  if ((header & 0xF0u) != 0x10u) {
    return false;
  }
  std::uint32_t out_size = static_cast<std::uint32_t>(bus.read8(src + 1u)) |
                           (static_cast<std::uint32_t>(bus.read8(src + 2u)) << 8u) |
                           (static_cast<std::uint32_t>(bus.read8(src + 3u)) << 16u);
  if (out_size == 0 || out_size > kSwiDecompMaxOutput) {
    return false;
  }

  out->assign(out_size, 0);
  std::size_t out_pos = 0;
  std::uint32_t in_pos = src + 4u;
  while (out_pos < out->size()) {
    std::uint8_t flags = bus.read8(in_pos++);
    for (int bit = 7; bit >= 0 && out_pos < out->size(); --bit) {
      if ((flags & (1u << bit)) == 0) {
        (*out)[out_pos++] = bus.read8(in_pos++);
      } else {
        std::uint8_t b1 = bus.read8(in_pos++);
        std::uint8_t b2 = bus.read8(in_pos++);
        int length = static_cast<int>((b1 >> 4) & 0xFu) + 3;
        int distance = static_cast<int>(((b1 & 0xFu) << 8) | b2) + 1;
        if (distance <= 0 || static_cast<std::size_t>(distance) > out_pos) {
          return false;
        }
        for (int i = 0; i < length && out_pos < out->size(); ++i) {
          (*out)[out_pos] = (*out)[out_pos - static_cast<std::size_t>(distance)];
          ++out_pos;
        }
      }
    }
  }
  return true;
}

bool decode_rl(const GbaBus& bus,
               std::uint32_t src,
               std::vector<std::uint8_t>* out) {
  if (!out) {
    return false;
  }
  std::uint8_t header = bus.read8(src);
  if ((header & 0xF0u) != 0x30u) {
    return false;
  }
  std::uint32_t out_size = static_cast<std::uint32_t>(bus.read8(src + 1u)) |
                           (static_cast<std::uint32_t>(bus.read8(src + 2u)) << 8u) |
                           (static_cast<std::uint32_t>(bus.read8(src + 3u)) << 16u);
  if (out_size == 0 || out_size > kSwiDecompMaxOutput) {
    return false;
  }

  out->assign(out_size, 0);
  std::size_t out_pos = 0;
  std::uint32_t in_pos = src + 4u;
  while (out_pos < out->size()) {
    std::uint8_t tag = bus.read8(in_pos++);
    if (tag & 0x80u) {
      int length = static_cast<int>(tag & 0x7Fu) + 3;
      std::uint8_t value = bus.read8(in_pos++);
      for (int i = 0; i < length && out_pos < out->size(); ++i) {
        (*out)[out_pos++] = value;
      }
    } else {
      int length = static_cast<int>(tag & 0x7Fu) + 1;
      for (int i = 0; i < length && out_pos < out->size(); ++i) {
        (*out)[out_pos++] = bus.read8(in_pos++);
      }
    }
  }
  return true;
}

void write_swi_output(GbaBus* bus,
                      std::uint32_t dst,
                      const std::vector<std::uint8_t>& data,
                      bool vram_mode) {
  if (!bus) {
    return;
  }
  if (!vram_mode) {
    for (std::size_t i = 0; i < data.size(); ++i) {
      bus->write8(dst + static_cast<std::uint32_t>(i), data[i]);
    }
    return;
  }
  dst &= ~1u;
  for (std::size_t i = 0; i < data.size(); i += 2) {
    std::uint16_t lo = data[i];
    std::uint16_t hi = (i + 1 < data.size()) ? static_cast<std::uint16_t>(data[i + 1]) : 0u;
    bus->write16(dst + static_cast<std::uint32_t>(i),
                 static_cast<std::uint16_t>(lo | (hi << 8u)));
  }
}

} // namespace

bool GbaCore::load(const std::vector<std::uint8_t>& rom,
                   const std::vector<std::uint8_t>& bios,
                   std::string* error) {
  std::vector<std::uint8_t> rom_data = rom;
  if (rom_data.size() >= 0xA0 && bios.size() >= 0xA0) {
    auto rom_logo_begin = rom_data.begin() + 0x04;
    auto rom_logo_end = rom_data.begin() + 0xA0;
    auto bios_logo_begin = bios.begin() + 0x04;
    if (!std::equal(rom_logo_begin, rom_logo_end, bios_logo_begin)) {
      std::copy(bios_logo_begin, bios_logo_begin + (rom_logo_end - rom_logo_begin), rom_logo_begin);
      std::cout << "GBA ROM logo patched to match BIOS logo\n";
    }
  }
  if (!bus_.load(rom_data, bios, error)) {
    return false;
  }
  rom_game_code_.clear();
  if (rom_data.size() >= 0xB0) {
    rom_game_code_.assign(reinterpret_cast<const char*>(&rom_data[0xAC]), 4);
  }
  reset();
  return true;
}

void GbaCore::set_trace(int steps, bool trace_io) {
  if (steps < 0) {
    steps = 0;
  }
  trace_steps_remaining_ = steps;
  trace_stop_on_rom_ = true;
  trace_stop_notified_ = false;
  trace_start_on_rom_ = false;
  trace_start_notified_ = false;
  if (trace_io) {
    bus_.set_trace_io_limit(steps > 0 ? steps * 4 : 0);
  } else {
    bus_.set_trace_io_limit(0);
  }
}

void GbaCore::set_trace_after_rom(int steps, bool trace_io) {
  if (steps < 0) {
    steps = 0;
  }
  trace_steps_remaining_ = steps;
  trace_start_on_rom_ = true;
  trace_start_notified_ = false;
  trace_stop_on_rom_ = false;
  trace_stop_notified_ = false;
  if (trace_io) {
    bus_.set_trace_io_limit(steps > 0 ? steps * 4 : 0);
  } else {
    bus_.set_trace_io_limit(0);
  }
}

void GbaCore::set_watchdog_steps(int steps) {
  if (steps < 0) {
    steps = 0;
  }
  watchdog_steps_ = steps;
  watchdog_counter_ = 0;
  watchdog_.clear();
}

void GbaCore::set_mem_watch(std::uint32_t start,
                            std::uint32_t end,
                            int count,
                            bool read,
                            bool write) {
  if (count < 0) {
    count = 0;
  }
  bus_.clear_watchpoints();
  bus_.set_watchpoint_limit(count);
  if (count > 0 && (read || write)) {
    bus_.add_watchpoint(start, end, read, write);
  }
}

void GbaCore::set_log_swi(int limit) {
  if (limit < 0) {
    limit = 0;
  }
  cpu_.set_log_swi(limit);
  hle_swi_log_limit_ = limit;
  hle_swi_log_count_ = 0;
}

void GbaCore::set_pc_watch(std::uint32_t start, std::uint32_t end, int count) {
  if (count < 0) {
    count = 0;
  }
  if (start > end) {
    std::swap(start, end);
  }
  pc_watch_enabled_ = count > 0;
  pc_watch_start_ = start;
  pc_watch_end_ = end;
  pc_watch_remaining_ = count;
}

void GbaCore::reset() {
  cpu_.reset();
  framebuffer_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
  ensure_render_buffers();
  frame_counter_ = 0;
  bios_handoff_done_ = false;
  bios_watchdog_cycles_ = 0;
  loop_pc_ = 0;
  loop_target_ = 0;
  loop_count_ = 0;
  loop_thumb_ = false;
  auto_patched_pcs_.clear();
  hle_swi_log_count_ = 0;
  line_cycles_ = 0;
  vcount_ = 0;
  vblank_ = false;
  hblank_ = false;
  halted_ = false;
  swi_wait_active_ = false;
  swi_wait_mask_ = 0;
  trace_steps_remaining_ = 0;
  trace_stop_notified_ = false;
  trace_start_on_rom_ = false;
  trace_start_notified_ = false;
  post_trace_remaining_ = 0;
  watchdog_counter_ = 0;
  watchdog_.clear();
  assert_bypass_patched_ = false;
  bus_.set_bios_enabled(true);
  keyinput_ = 0x03FF;
  auto reset_io = [this]() {
    bus_.write_io16_raw(0x04000000u, 0x0000);
    bus_.write_io16_raw(kRegDispstat, 0x0000);
    bus_.write_io16_raw(kRegVcount, 0x0000);
    bus_.write_io16_raw(0x04000008u, 0x0000);
    bus_.write_io16_raw(0x0400000Au, 0x0000);
    bus_.write_io16_raw(0x0400000Cu, 0x0000);
    bus_.write_io16_raw(0x0400000Eu, 0x0000);
    bus_.write_io16_raw(0x04000010u, 0x0000);
    bus_.write_io16_raw(0x04000012u, 0x0000);
    bus_.write_io16_raw(0x04000014u, 0x0000);
    bus_.write_io16_raw(0x04000016u, 0x0000);
    bus_.write_io16_raw(0x04000018u, 0x0000);
    bus_.write_io16_raw(0x0400001Au, 0x0000);
    bus_.write_io16_raw(0x0400001Cu, 0x0000);
    bus_.write_io16_raw(0x0400001Eu, 0x0000);
    bus_.write_io16_raw(0x04000040u, 0x0000);
    bus_.write_io16_raw(0x04000042u, 0x0000);
    bus_.write_io16_raw(0x04000044u, 0x0000);
    bus_.write_io16_raw(0x04000046u, 0x0000);
    bus_.write_io16_raw(0x04000048u, 0x0000);
    bus_.write_io16_raw(0x0400004Au, 0x0000);
    bus_.write_io16_raw(0x0400004Cu, 0x0000);
    bus_.write_io16_raw(0x04000050u, 0x0000);
    bus_.write_io16_raw(0x04000052u, 0x0000);
    bus_.write_io16_raw(0x04000054u, 0x0000);
    bus_.write_io16_raw(0x04000088u, 0x0200);
    bus_.write_io16_raw(0x04000130u, keyinput_);
    bus_.write_io16_raw(0x04000132u, 0x0000);
    bus_.write_io16_raw(kRegIe, 0x0000);
    bus_.write_io16_raw(kRegIf, 0x0000);
    bus_.write_io16_raw(kRegIme, 0x0000);
    bus_.write_io16_raw(0x04000204u, 0x0000);
    bus_.write_io16_raw(0x04000300u, 0x0000);
  };
  reset_io();
  for (auto& timer : timers_) {
    timer = Timer{};
  }
  for (auto& chan : dma_) {
    chan.active = false;
  }
  if (fastboot_enabled_) {
    fast_boot_to_rom();
  }
}

void GbaCore::ensure_render_buffers() {
  std::size_t pixels = static_cast<std::size_t>(width_);
  line_color_buf_.resize(pixels);
  line_prio_buf_.resize(pixels);
  line_layer_buf_.resize(pixels);
  obj_color_buf_.resize(pixels);
  obj_prio_buf_.resize(pixels);
  obj_semi_buf_.resize(pixels);
  obj_present_buf_.resize(pixels);
  obj_window_buf_.resize(pixels);
  win_mask_buf_.resize(pixels);
  obj_index_buf_.resize(pixels);
}

void GbaCore::set_keyinput(std::uint16_t value) {
  keyinput_ = static_cast<std::uint16_t>(value & 0x03FFu);
  keyinput_ |= 0xFC00u;
  bus_.write_io16_raw(0x04000130u, keyinput_);
}

void GbaCore::set_fastboot(bool enabled) {
  fastboot_enabled_ = enabled;
  if (fastboot_enabled_) {
    fast_boot_to_rom();
  }
}

int GbaCore::debug_step_cpu_instruction() {
  std::uint32_t pc_before = cpu_.pc();
  bus_.set_bios_enabled(pc_before < 0x00004000u);
  bus_.set_last_pc(pc_before);
  bus_.begin_cpu_step();
  int used = cpu_.step(&bus_);
  if (used <= 0) {
    bus_.finish_cpu_step(0);
    return used;
  }
  used += bus_.finish_cpu_step(used);
  step_timers(used);
  step_dma();
  step_ppu(used);
  service_interrupts();
  return used;
}

void GbaCore::serialize(std::vector<std::uint8_t>* out) const {
  if (!out) {
    return;
  }
  using namespace gbemu::core::state_io;

  write_u32(*out, static_cast<std::uint32_t>(width_));
  write_u32(*out, static_cast<std::uint32_t>(height_));
  write_u64(*out, frame_counter_);
  write_bool(*out, bios_handoff_done_);
  write_u32(*out, static_cast<std::uint32_t>(bios_watchdog_cycles_));
  write_u32(*out, static_cast<std::uint32_t>(line_cycles_));
  write_u32(*out, static_cast<std::uint32_t>(vcount_));
  write_bool(*out, vblank_);
  write_bool(*out, hblank_);
  write_bool(*out, halted_);
  write_bool(*out, swi_wait_active_);
  write_u16(*out, swi_wait_mask_);
  write_u32(*out, static_cast<std::uint32_t>(trace_steps_remaining_));
  write_bool(*out, trace_stop_on_rom_);
  write_bool(*out, trace_stop_notified_);
  write_bool(*out, trace_start_on_rom_);
  write_bool(*out, trace_start_notified_);
  write_u32(*out, static_cast<std::uint32_t>(post_trace_remaining_));
  write_bool(*out, pc_watch_enabled_);
  write_u32(*out, pc_watch_start_);
  write_u32(*out, pc_watch_end_);
  write_u32(*out, static_cast<std::uint32_t>(pc_watch_remaining_));
  write_u16(*out, keyinput_);
  write_bool(*out, gba_color_correct_);
  write_bool(*out, auto_handoff_enabled_);
  write_bool(*out, fastboot_enabled_);
  write_bool(*out, auto_patch_hang_);
  write_bool(*out, hle_swi_enabled_);
  write_bool(*out, trace_assert_);
  write_bool(*out, bypass_assert_);
  write_bool(*out, assert_bypass_patched_);
  write_u32(*out, static_cast<std::uint32_t>(hle_swi_log_limit_));
  write_u32(*out, static_cast<std::uint32_t>(hle_swi_log_count_));
  write_u32(*out, static_cast<std::uint32_t>(auto_patch_threshold_));
  write_u32(*out, auto_patch_span_);
  write_u32(*out, auto_patch_start_);
  write_u32(*out, auto_patch_end_);
  write_u32(*out, loop_pc_);
  write_u32(*out, loop_target_);
  write_u32(*out, static_cast<std::uint32_t>(loop_count_));
  write_bool(*out, loop_thumb_);
  write_u32(*out, static_cast<std::uint32_t>(watchdog_steps_));
  write_u32(*out, static_cast<std::uint32_t>(watchdog_counter_));
  write_string(*out, rom_game_code_);

  cpu_.serialize(out);
  bus_.serialize(out);

  write_u32(*out, static_cast<std::uint32_t>(timers_->reload));
  write_u32(*out, static_cast<std::uint32_t>(timers_->counter));
  write_u16(*out, timers_->control);
  write_u32(*out, timers_->accum);
  for (int i = 1; i < 4; ++i) {
    write_u32(*out, static_cast<std::uint32_t>(timers_[i].reload));
    write_u32(*out, static_cast<std::uint32_t>(timers_[i].counter));
    write_u16(*out, timers_[i].control);
    write_u32(*out, timers_[i].accum);
  }
  for (const DmaChannel& channel : dma_) {
    write_bool(*out, channel.active);
  }

  std::vector<std::uint32_t> patched(auto_patched_pcs_.begin(), auto_patched_pcs_.end());
  std::sort(patched.begin(), patched.end());
  write_u32(*out, static_cast<std::uint32_t>(patched.size()));
  for (std::uint32_t pc : patched) {
    write_u32(*out, pc);
  }

  std::vector<std::pair<std::uint32_t, WatchdogSample>> samples;
  samples.reserve(watchdog_.size());
  for (const auto& entry : watchdog_) {
    samples.push_back(entry);
  }
  std::sort(samples.begin(), samples.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  write_u32(*out, static_cast<std::uint32_t>(samples.size()));
  for (const auto& entry : samples) {
    write_u32(*out, entry.first);
    write_u32(*out, static_cast<std::uint32_t>(entry.second.count));
    write_u32(*out, entry.second.r0);
    write_u32(*out, entry.second.r1);
    write_u32(*out, entry.second.r2);
    write_u32(*out, entry.second.r3);
    write_u32(*out, entry.second.sp);
    write_u32(*out, entry.second.lr);
    write_u32(*out, entry.second.cpsr);
    write_bool(*out, entry.second.thumb);
  }

  write_u32(*out, static_cast<std::uint32_t>(framebuffer_.size()));
  for (std::uint32_t pixel : framebuffer_) {
    write_u32(*out, pixel);
  }
}

bool GbaCore::deserialize(const std::vector<std::uint8_t>& data,
                          std::size_t& offset,
                          std::string* error) {
  using namespace gbemu::core::state_io;
  auto fail = [error](const char* message) {
    if (error) {
      *error = message;
    }
    return false;
  };
  auto read_int = [&](int* out_value) -> bool {
    std::uint32_t value = 0;
    if (!read_u32(data, offset, value)) {
      return false;
    }
    *out_value = static_cast<int>(value);
    return true;
  };

  if (bus_.rom().empty() || bus_.bios().empty()) {
    return fail("GBA ROM/BIOS must be loaded before state load");
  }

  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (!read_u32(data, offset, width) || !read_u32(data, offset, height)) {
    return fail("Truncated GBA state");
  }
  if (width != static_cast<std::uint32_t>(width_) ||
      height != static_cast<std::uint32_t>(height_)) {
    return fail("GBA framebuffer size mismatch");
  }
  if (!read_u64(data, offset, frame_counter_) ||
      !read_bool(data, offset, bios_handoff_done_) ||
      !read_int(&bios_watchdog_cycles_) ||
      !read_int(&line_cycles_) ||
      !read_int(&vcount_) ||
      !read_bool(data, offset, vblank_) ||
      !read_bool(data, offset, hblank_) ||
      !read_bool(data, offset, halted_) ||
      !read_bool(data, offset, swi_wait_active_) ||
      !read_u16(data, offset, swi_wait_mask_) ||
      !read_int(&trace_steps_remaining_) ||
      !read_bool(data, offset, trace_stop_on_rom_) ||
      !read_bool(data, offset, trace_stop_notified_) ||
      !read_bool(data, offset, trace_start_on_rom_) ||
      !read_bool(data, offset, trace_start_notified_) ||
      !read_int(&post_trace_remaining_) ||
      !read_bool(data, offset, pc_watch_enabled_) ||
      !read_u32(data, offset, pc_watch_start_) ||
      !read_u32(data, offset, pc_watch_end_) ||
      !read_int(&pc_watch_remaining_) ||
      !read_u16(data, offset, keyinput_) ||
      !read_bool(data, offset, gba_color_correct_) ||
      !read_bool(data, offset, auto_handoff_enabled_) ||
      !read_bool(data, offset, fastboot_enabled_) ||
      !read_bool(data, offset, auto_patch_hang_) ||
      !read_bool(data, offset, hle_swi_enabled_) ||
      !read_bool(data, offset, trace_assert_) ||
      !read_bool(data, offset, bypass_assert_) ||
      !read_bool(data, offset, assert_bypass_patched_) ||
      !read_int(&hle_swi_log_limit_) ||
      !read_int(&hle_swi_log_count_) ||
      !read_int(&auto_patch_threshold_) ||
      !read_u32(data, offset, auto_patch_span_) ||
      !read_u32(data, offset, auto_patch_start_) ||
      !read_u32(data, offset, auto_patch_end_) ||
      !read_u32(data, offset, loop_pc_) ||
      !read_u32(data, offset, loop_target_) ||
      !read_int(&loop_count_) ||
      !read_bool(data, offset, loop_thumb_) ||
      !read_int(&watchdog_steps_) ||
      !read_int(&watchdog_counter_) ||
      !read_string(data, offset, rom_game_code_)) {
    return fail("Truncated GBA state");
  }

  if (!cpu_.deserialize(data, offset, error)) {
    return false;
  }
  if (!bus_.deserialize(data, offset, error)) {
    return false;
  }

  for (Timer& timer : timers_) {
    if (!read_u32(data, offset, timer.reload) ||
        !read_u32(data, offset, timer.counter) ||
        !read_u16(data, offset, timer.control) ||
        !read_u32(data, offset, timer.accum)) {
      return fail("Truncated GBA state");
    }
  }
  for (DmaChannel& channel : dma_) {
    if (!read_bool(data, offset, channel.active)) {
      return fail("Truncated GBA state");
    }
  }
  for (int i = 0; i < 4; ++i) {
    std::uint32_t addr = 0x04000100u + static_cast<std::uint32_t>(i) * 4u;
    timers_[i].io_data_shadow = bus_.read16(addr);
    timers_[i].io_control_shadow = bus_.read16(addr + 2u);
  }

  std::uint32_t patched_count = 0;
  if (!read_u32(data, offset, patched_count)) {
    return fail("Truncated GBA state");
  }
  auto_patched_pcs_.clear();
  for (std::uint32_t i = 0; i < patched_count; ++i) {
    std::uint32_t value = 0;
    if (!read_u32(data, offset, value)) {
      return fail("Truncated GBA state");
    }
    auto_patched_pcs_.insert(value);
  }

  std::uint32_t watchdog_count = 0;
  if (!read_u32(data, offset, watchdog_count)) {
    return fail("Truncated GBA state");
  }
  watchdog_.clear();
  for (std::uint32_t i = 0; i < watchdog_count; ++i) {
    std::uint32_t pc = 0;
    WatchdogSample sample{};
    std::uint32_t count = 0;
    if (!read_u32(data, offset, pc) ||
        !read_u32(data, offset, count) ||
        !read_u32(data, offset, sample.r0) ||
        !read_u32(data, offset, sample.r1) ||
        !read_u32(data, offset, sample.r2) ||
        !read_u32(data, offset, sample.r3) ||
        !read_u32(data, offset, sample.sp) ||
        !read_u32(data, offset, sample.lr) ||
        !read_u32(data, offset, sample.cpsr) ||
        !read_bool(data, offset, sample.thumb)) {
      return fail("Truncated GBA state");
    }
    sample.count = static_cast<int>(count);
    watchdog_[pc] = sample;
  }

  std::uint32_t fb_size = 0;
  if (!read_u32(data, offset, fb_size)) {
    return fail("Truncated GBA state");
  }
  std::size_t expected_fb = static_cast<std::size_t>(width_) * height_;
  if (fb_size != expected_fb) {
    return fail("GBA framebuffer size mismatch");
  }
  framebuffer_.assign(expected_fb, 0xFF000000u);
  for (std::size_t i = 0; i < expected_fb; ++i) {
    if (!read_u32(data, offset, framebuffer_[i])) {
      return fail("Truncated GBA state");
    }
  }
  ensure_render_buffers();
  return true;
}

void GbaCore::step_frame() {
  apply_assert_bypass_patches();

  int cycles = 0;
  auto step_once = [this, &cycles]() -> bool {
    std::uint32_t pc_before = cpu_.pc();
    bool thumb_before = cpu_.thumb();
    bus_.set_bios_enabled(pc_before < 0x00004000u);
    bus_.set_last_pc(pc_before);
    std::uint32_t op_before = 0;
    if (auto_patch_hang_ || hle_swi_enabled_) {
      op_before = thumb_before ? bus_.read16(pc_before) : bus_.read32(pc_before);
    }
    if (pc_watch_enabled_ && pc_watch_remaining_ > 0 &&
        pc_before >= pc_watch_start_ && pc_before <= pc_watch_end_) {
      bool thumb = thumb_before;
      std::uint32_t op = (op_before != 0) ? op_before
                                          : (thumb ? bus_.read16(pc_before) : bus_.read32(pc_before));
      std::ostringstream line;
      line << "GBA PCWATCH " << (thumb ? "T" : "A") << " PC=0x"
           << std::hex << std::setw(8) << std::setfill('0') << pc_before
           << " OP=0x" << std::setw(thumb ? 4 : 8) << op
           << " R0=0x" << std::setw(8) << cpu_.reg(0)
           << " R1=0x" << std::setw(8) << cpu_.reg(1)
           << " R2=0x" << std::setw(8) << cpu_.reg(2)
           << " R3=0x" << std::setw(8) << cpu_.reg(3)
           << " R4=0x" << std::setw(8) << cpu_.reg(4)
           << " R5=0x" << std::setw(8) << cpu_.reg(5)
           << " R10=0x" << std::setw(8) << cpu_.reg(10)
           << " SP=0x" << std::setw(8) << cpu_.reg(13)
           << " LR=0x" << std::setw(8) << cpu_.reg(14)
           << " CPSR=0x" << std::setw(8) << cpu_.cpsr()
           << std::dec;
      std::cout << line.str() << "\n";
      --pc_watch_remaining_;
    }
    if (hle_swi_enabled_) {
      int hle_cycles = 0;
      if (handle_swi_hle(pc_before, thumb_before, op_before, &hle_cycles)) {
        if (hle_cycles <= 0) {
          hle_cycles = 2;
        }
        watchdog_tick(pc_before);
        step_timers(hle_cycles);
        step_dma();
        step_ppu(hle_cycles);
        service_interrupts();
        cycles += hle_cycles;
        return true;
      }
    }

    int assert_cycles = 0;
    if (handle_butano_assert(pc_before, thumb_before, &assert_cycles)) {
      if (assert_cycles <= 0) {
        assert_cycles = 2;
      }
      watchdog_tick(pc_before);
      step_timers(assert_cycles);
      step_dma();
      step_ppu(assert_cycles);
      service_interrupts();
      cycles += assert_cycles;
      return true;
    }

    bus_.begin_cpu_step();
    int used = cpu_.step(&bus_);
    if (used <= 0) {
      bus_.finish_cpu_step(0);
      return false;
    }
    used += bus_.finish_cpu_step(used);
    auto_patch_tick(pc_before, cpu_.pc(), op_before, thumb_before);
    watchdog_tick(pc_before);
    step_timers(used);
    step_dma();
    step_ppu(used);
    service_interrupts();
    cycles += used;
    bool stop = false;
    if (bus_.take_halt_request(&stop)) {
      halted_ = !interrupt_pending();
      swi_wait_active_ = false;
      swi_wait_mask_ = 0;
    }
    std::uint32_t post_pc = 0;
    std::uint8_t post_value = 0;
    if (bus_.take_postflg_write(&post_pc, &post_value)) {
      std::cout << "GBA POSTFLG W8 PC=0x" << std::hex << std::setw(8) << std::setfill('0')
                << post_pc << " VALUE=0x" << std::setw(2) << static_cast<int>(post_value)
                << " LR=0x" << std::setw(8) << cpu_.reg(14)
                << " CPSR=0x" << std::setw(8) << cpu_.cpsr() << std::dec << "\n";
      if (trace_start_on_rom_ && trace_steps_remaining_ > 0) {
        post_trace_remaining_ = trace_steps_remaining_;
      }
      if (auto_handoff_enabled_ && !bios_handoff_done_ && post_value == 0x01 &&
          cpu_.pc() < 0x00004000u) {
        std::cout << "GBA BIOS auto handoff after POSTFLG\n";
        handoff_to_rom();
      }
    }
    if (!bios_handoff_done_) {
      bios_watchdog_cycles_ += used;
      if (cpu_.pc() >= 0x08000000u) {
        bios_handoff_done_ = true;
        cpu_.clear_unimplemented_count();
        std::cout << "GBA BIOS handoff: entered ROM at PC=0x" << std::hex << std::setw(8)
                  << std::setfill('0') << cpu_.pc() << std::dec << "\n";
      }
    }
    return true;
  };
  while (cycles < kGbaCyclesPerFrame && !cpu_.faulted()) {
    sync_timers_from_io();
    if (halted_) {
      // Fast-forward while halted to avoid spending most host CPU time
      // in 4-cycle idle slices during BIOS/IRQ waits.
      int remaining = kGbaCyclesPerFrame - cycles;
      int used = 4;
      if (remaining > 4 && !interrupt_pending()) {
        used = std::min(remaining, 1024);
      }
      step_timers(used);
      step_dma();
      step_ppu(used);
      service_interrupts();
      cycles += used;
      if (swi_wait_active_) {
        std::uint16_t ie = bus_.read_io16(kRegIe);
        std::uint16_t iflag = bus_.read_io16(kRegIf);
        if ((ie & iflag & swi_wait_mask_) != 0) {
          halted_ = false;
          swi_wait_active_ = false;
          swi_wait_mask_ = 0;
        }
      } else if (interrupt_pending()) {
        halted_ = false;
      }
      continue;
    }
    if (trace_steps_remaining_ > 0 && trace_stop_on_rom_ && cpu_.pc() >= 0x08000000u) {
      if (!trace_stop_notified_) {
        std::cout << "GBA TRACE stop: entered ROM at PC=0x" << std::hex << std::setw(8)
                  << std::setfill('0') << cpu_.pc() << std::dec << "\n";
        trace_stop_notified_ = true;
      }
      trace_steps_remaining_ = 0;
      bus_.set_trace_io_limit(0);
    }
    if (post_trace_remaining_ > 0) {
      std::uint32_t pc = cpu_.pc();
      bool thumb = cpu_.thumb();
      std::uint32_t op = thumb ? bus_.read16(pc) : bus_.read32(pc);
      std::ostringstream line;
      line << "GBA TRACE POST " << (thumb ? "T" : "A") << " PC=0x"
           << std::hex << std::setw(8) << std::setfill('0') << pc
           << " OP=0x" << std::setw(thumb ? 4 : 8) << op
           << " R0=0x" << std::setw(8) << cpu_.reg(0)
           << " R1=0x" << std::setw(8) << cpu_.reg(1)
           << " R2=0x" << std::setw(8) << cpu_.reg(2)
           << " R3=0x" << std::setw(8) << cpu_.reg(3)
           << " SP=0x" << std::setw(8) << cpu_.reg(13)
           << " LR=0x" << std::setw(8) << cpu_.reg(14)
           << " CPSR=0x" << std::setw(8) << cpu_.cpsr()
           << std::dec;
      std::cout << line.str() << "\n";
      --post_trace_remaining_;
    }
    if (trace_steps_remaining_ > 0 && trace_start_on_rom_ && post_trace_remaining_ == 0) {
      if (cpu_.pc() >= 0x08000000u) {
        if (!trace_start_notified_) {
          std::cout << "GBA TRACE start: entered ROM at PC=0x" << std::hex << std::setw(8)
                    << std::setfill('0') << cpu_.pc() << std::dec << "\n";
          trace_start_notified_ = true;
        }
      } else {
        if (!step_once()) {
          break;
        }
        continue;
      }
    }
    if (trace_steps_remaining_ > 0) {
      std::uint32_t pc = cpu_.pc();
      bool thumb = cpu_.thumb();
      std::uint32_t op = thumb ? bus_.read16(pc) : bus_.read32(pc);
      std::ostringstream line;
      line << "GBA TRACE " << (thumb ? "T" : "A") << " PC=0x"
           << std::hex << std::setw(8) << std::setfill('0') << pc
           << " OP=0x" << std::setw(thumb ? 4 : 8) << op
           << " R0=0x" << std::setw(8) << cpu_.reg(0)
           << " R1=0x" << std::setw(8) << cpu_.reg(1)
           << " R2=0x" << std::setw(8) << cpu_.reg(2)
           << " R3=0x" << std::setw(8) << cpu_.reg(3)
           << " SP=0x" << std::setw(8) << cpu_.reg(13)
           << " LR=0x" << std::setw(8) << cpu_.reg(14)
           << " CPSR=0x" << std::setw(8) << cpu_.cpsr()
           << std::dec;
      std::cout << line.str() << "\n";
      --trace_steps_remaining_;
      if (trace_steps_remaining_ == 0) {
        bus_.set_trace_io_limit(0);
      }
    }
    if (!step_once()) {
      break;
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
  if (mode <= 5) {
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
  ensure_render_buffers();
  constexpr int kLayerBg0 = 0;
  constexpr int kLayerBg1 = 1;
  constexpr int kLayerBg2 = 2;
  constexpr int kLayerBg3 = 3;
  constexpr int kLayerObj = 4;
  constexpr int kLayerBd = 5;

  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  std::uint16_t mos = bus_.read_io16(0x0400004Cu);
  int mosaic_bg_w = (mos & 0xF) + 1;
  int mosaic_bg_h = ((mos >> 4) & 0xF) + 1;
  int mosaic_obj_w = ((mos >> 8) & 0xF) + 1;
  int mosaic_obj_h = ((mos >> 12) & 0xF) + 1;
  std::fill(line_prio_buf_.begin(), line_prio_buf_.end(), 4);
  std::fill(line_layer_buf_.begin(), line_layer_buf_.end(), kLayerBd);
  std::fill(obj_color_buf_.begin(), obj_color_buf_.end(), 0u);
  std::fill(obj_prio_buf_.begin(), obj_prio_buf_.end(), 4);
  std::fill(obj_semi_buf_.begin(), obj_semi_buf_.end(), false);
  std::fill(obj_present_buf_.begin(), obj_present_buf_.end(), false);
  std::fill(obj_window_buf_.begin(), obj_window_buf_.end(), false);

  std::uint16_t backdrop = bus_.read_palette16_fast(0x05000000u);
  std::uint32_t backdrop_color = bgr555_to_argb(backdrop);

  bool gba_color_correct = gba_color_correct_;
  if (dispcnt & 0x0080) {
    std::fill(line_color_buf_.begin(), line_color_buf_.end(), 0xFF000000u);
    std::fill(line_prio_buf_.begin(), line_prio_buf_.end(), 4);
    std::fill(line_layer_buf_.begin(), line_layer_buf_.end(), kLayerBd);
  } else {
    std::uint16_t mode = dispcnt & 0x0007;
    int mosaiced_y = y;
    if (mosaic_bg_h > 1) {
      mosaiced_y = y - (y % mosaic_bg_h);
    }
    switch (mode) {
      case 0:
        render_line_mode0(mosaiced_y, line_color_buf_, line_prio_buf_, line_layer_buf_, 0xF);
        break;
      case 1:
        render_line_mode0(mosaiced_y, line_color_buf_, line_prio_buf_, line_layer_buf_, 0x3);
        render_line_affine_bg(mosaiced_y, 2, line_color_buf_, line_prio_buf_, line_layer_buf_);
        break;
      case 2:
        render_line_mode0(mosaiced_y, line_color_buf_, line_prio_buf_, line_layer_buf_, 0x0);
        render_line_affine_bg(mosaiced_y, 2, line_color_buf_, line_prio_buf_, line_layer_buf_);
        render_line_affine_bg(mosaiced_y, 3, line_color_buf_, line_prio_buf_, line_layer_buf_);
        break;
      case 3:
        render_line_mode3(mosaiced_y, line_color_buf_, line_prio_buf_, line_layer_buf_);
        break;
      case 4:
        render_line_mode4(mosaiced_y, line_color_buf_, line_prio_buf_, line_layer_buf_);
        break;
      case 5:
        render_line_mode5(mosaiced_y, line_color_buf_, line_prio_buf_, line_layer_buf_);
        break;
      default:
        for (int x = 0; x < width_; ++x) {
          std::uint8_t r = static_cast<std::uint8_t>((x + frame_counter_ * 2) % 256);
          std::uint8_t g = static_cast<std::uint8_t>((y + frame_counter_) % 256);
          std::uint8_t b = static_cast<std::uint8_t>((x ^ y ^ static_cast<int>(frame_counter_)) & 0xFF);
          line_color_buf_[static_cast<std::size_t>(x)] =
              (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
              (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
          line_prio_buf_[static_cast<std::size_t>(x)] = 4;
          line_layer_buf_[static_cast<std::size_t>(x)] = kLayerBd;
        }
        break;
    }
  }

  if ((dispcnt & 0x1000) != 0) {
    int obj_y = y;
    if (mosaic_obj_h > 1) {
      obj_y = y - (y % mosaic_obj_h);
    }
    render_line_sprites(obj_y, obj_color_buf_, obj_prio_buf_, obj_semi_buf_, obj_present_buf_,
                        obj_window_buf_);
  }

  std::uint16_t winout = bus_.read_io16(0x0400004Au);
  std::uint16_t winin = bus_.read_io16(0x04000048u);
  std::uint8_t outside_mask = static_cast<std::uint8_t>(winout & 0x3F);
  std::uint8_t obj_mask = static_cast<std::uint8_t>((winout >> 8) & 0x3F);
  std::uint8_t win0_mask = static_cast<std::uint8_t>(winin & 0x3F);
  std::uint8_t win1_mask = static_cast<std::uint8_t>((winin >> 8) & 0x3F);

  bool win0_enabled = (dispcnt & 0x2000) != 0;
  bool win1_enabled = (dispcnt & 0x4000) != 0;
  bool winobj_enabled = (dispcnt & 0x8000) != 0;
  if (!win0_enabled && !win1_enabled && !winobj_enabled) {
    outside_mask = 0x3F;
    obj_mask = 0x3F;
  }
  int win0_left = 0;
  int win0_right = 0;
  int win0_top = 0;
  int win0_bottom = 0;
  int win1_left = 0;
  int win1_right = 0;
  int win1_top = 0;
  int win1_bottom = 0;
  if (win0_enabled) {
    std::uint16_t win0h = bus_.read_io16(0x04000040u);
    std::uint16_t win0v = bus_.read_io16(0x04000044u);
    win0_right = static_cast<int>(win0h & 0xFF);
    win0_left = static_cast<int>((win0h >> 8) & 0xFF);
    win0_bottom = static_cast<int>(win0v & 0xFF);
    win0_top = static_cast<int>((win0v >> 8) & 0xFF);
  }
  if (win1_enabled) {
    std::uint16_t win1h = bus_.read_io16(0x04000042u);
    std::uint16_t win1v = bus_.read_io16(0x04000046u);
    win1_right = static_cast<int>(win1h & 0xFF);
    win1_left = static_cast<int>((win1h >> 8) & 0xFF);
    win1_bottom = static_cast<int>(win1v & 0xFF);
    win1_top = static_cast<int>((win1v >> 8) & 0xFF);
  }

  std::fill(win_mask_buf_.begin(), win_mask_buf_.end(), outside_mask);
  if (win0_enabled || win1_enabled || winobj_enabled) {
    bool in_win0_y = win0_enabled && in_window_range(y, win0_top, win0_bottom);
    bool in_win1_y = win1_enabled && in_window_range(y, win1_top, win1_bottom);
    for (int x = 0; x < width_; ++x) {
      bool in0 = in_win0_y && in_window_range(x, win0_left, win0_right);
      bool in1 = in_win1_y && in_window_range(x, win1_left, win1_right);
      if (in1) {
        win_mask_buf_[static_cast<std::size_t>(x)] = win1_mask;
      }
      if (in0) {
        win_mask_buf_[static_cast<std::size_t>(x)] = win0_mask;
      }
    }
  }

  std::uint16_t bldcnt = bus_.read_io16(0x04000050u);
  std::uint16_t bldalpha = bus_.read_io16(0x04000052u);
  std::uint16_t bldy = bus_.read_io16(0x04000054u);
  std::uint8_t target1 = static_cast<std::uint8_t>(bldcnt & 0x3F);
  std::uint8_t target2 = static_cast<std::uint8_t>((bldcnt >> 8) & 0x3F);
  std::uint8_t effect = static_cast<std::uint8_t>((bldcnt >> 6) & 0x3);
  int eva = bldalpha & 0x1F;
  int evb = (bldalpha >> 8) & 0x1F;
  int evy = bldy & 0x1F;
  if (eva > 16) eva = 16;
  if (evb > 16) evb = 16;
  if (evy > 16) evy = 16;

  auto layer_bit = [&](int layer) -> std::uint8_t {
    if (layer >= 0 && layer <= 5) {
      return static_cast<std::uint8_t>(1u << layer);
    }
    return 0;
  };

  for (int x = 0; x < width_; ++x) {
    if (mosaic_bg_w > 1) {
      int mx = x - (x % mosaic_bg_w);
      if (mx != x) {
        line_color_buf_[static_cast<std::size_t>(x)] = line_color_buf_[static_cast<std::size_t>(mx)];
        line_prio_buf_[static_cast<std::size_t>(x)] = line_prio_buf_[static_cast<std::size_t>(mx)];
        line_layer_buf_[static_cast<std::size_t>(x)] = line_layer_buf_[static_cast<std::size_t>(mx)];
      }
    }
    if (mosaic_obj_w > 1) {
      int mx = x - (x % mosaic_obj_w);
      if (mx != x) {
        obj_color_buf_[static_cast<std::size_t>(x)] = obj_color_buf_[static_cast<std::size_t>(mx)];
        obj_prio_buf_[static_cast<std::size_t>(x)] = obj_prio_buf_[static_cast<std::size_t>(mx)];
        obj_semi_buf_[static_cast<std::size_t>(x)] = obj_semi_buf_[static_cast<std::size_t>(mx)];
        obj_present_buf_[static_cast<std::size_t>(x)] =
            obj_present_buf_[static_cast<std::size_t>(mx)];
        obj_window_buf_[static_cast<std::size_t>(x)] = obj_window_buf_[static_cast<std::size_t>(mx)];
      }
    }
    std::uint8_t mask = win_mask_buf_[static_cast<std::size_t>(x)];
    if (winobj_enabled && obj_window_buf_[static_cast<std::size_t>(x)] &&
        mask == outside_mask) {
      mask = obj_mask;
    }
    bool effects_enabled = (mask & (1u << 5)) != 0;
    bool obj_allowed = (mask & (1u << 4)) != 0;

    int base_layer = line_layer_buf_[static_cast<std::size_t>(x)];
    std::uint32_t base_color = line_color_buf_[static_cast<std::size_t>(x)];
    int base_prio = line_prio_buf_[static_cast<std::size_t>(x)];
    if (base_layer >= kLayerBg0 && base_layer <= kLayerBg3) {
      if ((mask & (1u << base_layer)) == 0) {
        base_layer = kLayerBd;
        base_prio = 4;
        base_color = backdrop_color;
      }
    }

    bool have_obj = obj_present_buf_[static_cast<std::size_t>(x)] && obj_allowed;
    std::uint32_t obj_col = obj_color_buf_[static_cast<std::size_t>(x)];
    int obj_pr = obj_prio_buf_[static_cast<std::size_t>(x)];
    bool obj_is_semi = obj_semi_buf_[static_cast<std::size_t>(x)];

    std::uint32_t top_color = base_color;
    int top_layer = base_layer;
    std::uint32_t second_color = backdrop_color;
    int second_layer = kLayerBd;
    bool top_semi = false;

    if (have_obj) {
      if (obj_pr < base_prio || (obj_pr == base_prio)) {
        top_color = obj_col;
        top_layer = kLayerObj;
        second_color = base_color;
        second_layer = base_layer;
        top_semi = obj_is_semi;
      } else {
        top_color = base_color;
        top_layer = base_layer;
        second_color = obj_col;
        second_layer = kLayerObj;
        top_semi = false;
      }
    } else {
      top_color = base_color;
      top_layer = base_layer;
      second_color = backdrop_color;
      second_layer = kLayerBd;
      top_semi = false;
    }

    std::uint32_t final_color = top_color;
    std::uint8_t eff = effects_enabled ? effect : 0;
    bool do_blend = false;
    if (eff == 1) {
      if ((target1 & layer_bit(top_layer)) && (target2 & layer_bit(second_layer))) {
        do_blend = true;
      }
    }
    if (top_semi && effects_enabled &&
        (target1 & layer_bit(kLayerObj)) && (target2 & layer_bit(second_layer))) {
      do_blend = true;
    }

    if (do_blend) {
      std::uint8_t r1 = static_cast<std::uint8_t>((top_color >> 16) & 0xFF);
      std::uint8_t g1 = static_cast<std::uint8_t>((top_color >> 8) & 0xFF);
      std::uint8_t b1 = static_cast<std::uint8_t>(top_color & 0xFF);
      std::uint8_t r2 = static_cast<std::uint8_t>((second_color >> 16) & 0xFF);
      std::uint8_t g2 = static_cast<std::uint8_t>((second_color >> 8) & 0xFF);
      std::uint8_t b2 = static_cast<std::uint8_t>(second_color & 0xFF);
      int r = (r1 * eva + r2 * evb) / 16;
      int g = (g1 * eva + g2 * evb) / 16;
      int b = (b1 * eva + b2 * evb) / 16;
      if (r > 255) r = 255;
      if (g > 255) g = 255;
      if (b > 255) b = 255;
      final_color = (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
                    (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
    } else if (eff == 2 && (target1 & layer_bit(top_layer))) {
      std::uint8_t r1 = static_cast<std::uint8_t>((top_color >> 16) & 0xFF);
      std::uint8_t g1 = static_cast<std::uint8_t>((top_color >> 8) & 0xFF);
      std::uint8_t b1 = static_cast<std::uint8_t>(top_color & 0xFF);
      std::uint8_t r = static_cast<std::uint8_t>(r1 + ((255 - r1) * evy) / 16);
      std::uint8_t g = static_cast<std::uint8_t>(g1 + ((255 - g1) * evy) / 16);
      std::uint8_t b = static_cast<std::uint8_t>(b1 + ((255 - b1) * evy) / 16);
      final_color = (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
                    (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
    } else if (eff == 3 && (target1 & layer_bit(top_layer))) {
      std::uint8_t r1 = static_cast<std::uint8_t>((top_color >> 16) & 0xFF);
      std::uint8_t g1 = static_cast<std::uint8_t>((top_color >> 8) & 0xFF);
      std::uint8_t b1 = static_cast<std::uint8_t>(top_color & 0xFF);
      std::uint8_t r = static_cast<std::uint8_t>(r1 - (r1 * evy) / 16);
      std::uint8_t g = static_cast<std::uint8_t>(g1 - (g1 * evy) / 16);
      std::uint8_t b = static_cast<std::uint8_t>(b1 - (b1 * evy) / 16);
      final_color = (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
                    (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
    }

    if (gba_color_correct) {
      std::uint8_t r = static_cast<std::uint8_t>((final_color >> 16) & 0xFF);
      std::uint8_t g = static_cast<std::uint8_t>((final_color >> 8) & 0xFF);
      std::uint8_t b = static_cast<std::uint8_t>(final_color & 0xFF);
      r = static_cast<std::uint8_t>(r * 0.95 + 8);
      g = static_cast<std::uint8_t>(g * 0.95 + 8);
      b = static_cast<std::uint8_t>(b * 0.90 + 10);
      final_color = (0xFFu << 24) | (static_cast<std::uint32_t>(r) << 16) |
                    (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
    }
    framebuffer_[static_cast<std::size_t>(y) * width_ + x] = final_color;
  }
}

void GbaCore::render_line_mode3(int y,
                                std::vector<std::uint32_t>& line_color,
                                std::vector<int>& line_prio,
                                std::vector<int>& line_layer) {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  bool bg2 = (dispcnt & 0x0400) != 0;
  int bg2_prio = bus_.read_io16(0x0400000Cu) & 0x3;
  std::uint16_t backdrop = bus_.read16(0x05000000u);
  std::uint32_t backdrop_color = bgr555_to_argb(backdrop);
  std::uint32_t base = 0x06000000u + static_cast<std::uint32_t>(y) * width_ * 2u;
  for (int x = 0; x < width_; ++x) {
    if (bg2) {
      std::uint16_t pixel = bus_.read_vram16_fast(base + static_cast<std::uint32_t>(x) * 2u);
      line_color[static_cast<std::size_t>(x)] = bgr555_to_argb(pixel);
      line_prio[static_cast<std::size_t>(x)] = bg2_prio;
      line_layer[static_cast<std::size_t>(x)] = 2;
    } else {
      line_color[static_cast<std::size_t>(x)] = backdrop_color;
      line_prio[static_cast<std::size_t>(x)] = 4;
      line_layer[static_cast<std::size_t>(x)] = 5;
    }
  }
}

void GbaCore::render_line_mode4(int y,
                                std::vector<std::uint32_t>& line_color,
                                std::vector<int>& line_prio,
                                std::vector<int>& line_layer) {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  bool bg2 = (dispcnt & 0x0400) != 0;
  int bg2_prio = bus_.read_io16(0x0400000Cu) & 0x3;
  std::uint16_t backdrop = bus_.read_palette16_fast(0x05000000u);
  std::uint32_t backdrop_color = bgr555_to_argb(backdrop);
  std::uint32_t page = (dispcnt & 0x0010) ? 0x0000A000u : 0x00000000u;
  std::uint32_t base = 0x06000000u + page + static_cast<std::uint32_t>(y) * width_;
  for (int x = 0; x < width_; ++x) {
    if (bg2) {
      std::uint8_t index = bus_.read_vram8_fast(base + static_cast<std::uint32_t>(x));
      std::uint16_t pal =
          bus_.read_palette16_fast(0x05000000u + static_cast<std::uint32_t>(index) * 2u);
      line_color[static_cast<std::size_t>(x)] = bgr555_to_argb(pal);
      line_prio[static_cast<std::size_t>(x)] = bg2_prio;
      line_layer[static_cast<std::size_t>(x)] = 2;
    } else {
      line_color[static_cast<std::size_t>(x)] = backdrop_color;
      line_prio[static_cast<std::size_t>(x)] = 4;
      line_layer[static_cast<std::size_t>(x)] = 5;
    }
  }
}

void GbaCore::render_line_mode5(int y,
                                std::vector<std::uint32_t>& line_color,
                                std::vector<int>& line_prio,
                                std::vector<int>& line_layer) {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  bool bg2 = (dispcnt & 0x0400) != 0;
  int bg2_prio = bus_.read_io16(0x0400000Cu) & 0x3;
  std::uint16_t backdrop = bus_.read_palette16_fast(0x05000000u);
  std::uint32_t backdrop_color = bgr555_to_argb(backdrop);
  std::uint32_t page = (dispcnt & 0x0010) ? 0x0000A000u : 0x00000000u;
  constexpr int kMode5Width = 160;
  constexpr int kMode5Height = 128;
  std::uint32_t base = 0x06000000u + page + static_cast<std::uint32_t>(y) * kMode5Width * 2u;

  for (int x = 0; x < width_; ++x) {
    if (bg2 && y < kMode5Height && x < kMode5Width) {
      std::uint16_t pixel = bus_.read_vram16_fast(base + static_cast<std::uint32_t>(x) * 2u);
      line_color[static_cast<std::size_t>(x)] = bgr555_to_argb(pixel);
      line_prio[static_cast<std::size_t>(x)] = bg2_prio;
      line_layer[static_cast<std::size_t>(x)] = 2;
    } else {
      line_color[static_cast<std::size_t>(x)] = backdrop_color;
      line_prio[static_cast<std::size_t>(x)] = 4;
      line_layer[static_cast<std::size_t>(x)] = 5;
    }
  }
}

void GbaCore::render_line_affine_bg(int y,
                                    int bg_index,
                                    std::vector<std::uint32_t>& line_color,
                                    std::vector<int>& line_prio,
                                    std::vector<int>& line_layer) {
  if (bg_index != 2 && bg_index != 3) {
    return;
  }
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  if (bg_index == 2 && (dispcnt & 0x0400) == 0) {
    return;
  }
  if (bg_index == 3 && (dispcnt & 0x0800) == 0) {
    return;
  }

  std::uint32_t cnt_addr = (bg_index == 2) ? 0x0400000Cu : 0x0400000Eu;
  std::uint16_t cnt = bus_.read_io16(cnt_addr);
  int prio = cnt & 0x3;
  int char_base = (cnt >> 2) & 0x3;
  int screen_base = (cnt >> 8) & 0x1F;
  bool wrap = (cnt & 0x2000) != 0;
  int size_code = (cnt >> 14) & 0x3;
  int size = 128 << size_code;
  int map_width = size / 8;

  std::uint32_t pa_addr = (bg_index == 2) ? 0x04000020u : 0x04000030u;
  std::uint32_t pb_addr = pa_addr + 2;
  std::uint32_t pc_addr = pa_addr + 4;
  std::uint32_t pd_addr = pa_addr + 6;
  std::uint32_t refx_addr = (bg_index == 2) ? 0x04000028u : 0x04000038u;
  std::uint32_t refy_addr = (bg_index == 2) ? 0x0400002Cu : 0x0400003Cu;

  std::int32_t pa = static_cast<std::int16_t>(bus_.read16(pa_addr));
  std::int32_t pb = static_cast<std::int16_t>(bus_.read16(pb_addr));
  std::int32_t pc = static_cast<std::int16_t>(bus_.read16(pc_addr));
  std::int32_t pd = static_cast<std::int16_t>(bus_.read16(pd_addr));
  std::int32_t ref_x = static_cast<std::int32_t>(bus_.read32(refx_addr));
  std::int32_t ref_y = static_cast<std::int32_t>(bus_.read32(refy_addr));
  if (ref_x & 0x08000000) {
    ref_x |= ~0x0FFFFFFF;
  }
  if (ref_y & 0x08000000) {
    ref_y |= ~0x0FFFFFFF;
  }

  std::int32_t start_x = ref_x + pb * y;
  std::int32_t start_y = ref_y + pd * y;

  std::uint32_t char_base_addr = 0x06000000u + static_cast<std::uint32_t>(char_base) * 0x4000u;
  std::uint32_t screen_base_addr = 0x06000000u + static_cast<std::uint32_t>(screen_base) * 2048u;

  for (int x = 0; x < width_; ++x) {
    std::int32_t sx = start_x + pa * x;
    std::int32_t sy = start_y + pc * x;
    int src_x = static_cast<int>(sx >> 8);
    int src_y = static_cast<int>(sy >> 8);
    if (wrap) {
      if (size > 0) {
        src_x %= size;
        src_y %= size;
        if (src_x < 0) src_x += size;
        if (src_y < 0) src_y += size;
      }
    } else {
      if (src_x < 0 || src_x >= size || src_y < 0 || src_y >= size) {
        continue;
      }
    }
    int tile_x = src_x / 8;
    int tile_y = src_y / 8;
    int in_tile_x = src_x & 7;
    int in_tile_y = src_y & 7;
    std::uint32_t map_addr = screen_base_addr +
                             static_cast<std::uint32_t>(tile_y) * map_width +
                             static_cast<std::uint32_t>(tile_x);
    std::uint8_t tile_index = bus_.read_vram8_fast(map_addr);
    if (tile_index == 0) {
      continue;
    }
    std::uint32_t tile_addr = char_base_addr +
                              static_cast<std::uint32_t>(tile_index) * 64u +
                              static_cast<std::uint32_t>(in_tile_y) * 8u +
                              static_cast<std::uint32_t>(in_tile_x);
    std::uint8_t color_index = bus_.read_vram8_fast(tile_addr);
    if (color_index == 0) {
      continue;
    }
    std::uint16_t pal =
        bus_.read_palette16_fast(0x05000000u + static_cast<std::uint32_t>(color_index) * 2u);
    if (prio < line_prio[static_cast<std::size_t>(x)]) {
      line_color[static_cast<std::size_t>(x)] = bgr555_to_argb(pal);
      line_prio[static_cast<std::size_t>(x)] = prio;
      line_layer[static_cast<std::size_t>(x)] = bg_index;
    }
  }
}

void GbaCore::render_line_mode0(int y,
                                std::vector<std::uint32_t>& line_color,
                                std::vector<int>& line_prio,
                                std::vector<int>& line_layer,
                                int bg_mask) {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  bool bg_enabled[4] = {
      ((dispcnt & 0x0100) != 0) && ((bg_mask & 0x1) != 0),
      ((dispcnt & 0x0200) != 0) && ((bg_mask & 0x2) != 0),
      ((dispcnt & 0x0400) != 0) && ((bg_mask & 0x4) != 0),
      ((dispcnt & 0x0800) != 0) && ((bg_mask & 0x8) != 0),
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

  std::uint16_t backdrop = bus_.read_palette16_fast(0x05000000u);
  std::uint32_t backdrop_color = bgr555_to_argb(backdrop);
  for (int x = 0; x < width_; ++x) {
    std::uint32_t best_color = backdrop_color;
    int best_priority = 4;
    int best_layer = 5;
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
      std::uint16_t entry = bus_.read_vram16_fast(map_addr);
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
        color_index = bus_.read_vram8_fast(tile_addr);
      } else {
        std::uint32_t tile_addr = tile_base + static_cast<std::uint32_t>(tile_index) * 32u +
                                  static_cast<std::uint32_t>(py) * 4u +
                                  static_cast<std::uint32_t>(px / 2);
        std::uint8_t byte = bus_.read_vram8_fast(tile_addr);
        int color = (px & 1) ? (byte >> 4) : (byte & 0x0F);
        if (color == 0) {
          continue;
        }
        color_index = (palette_bank << 4) | color;
      }

      if (color_index == 0) {
        continue;
      }
      std::uint16_t pal =
          bus_.read_palette16_fast(0x05000000u + static_cast<std::uint32_t>(color_index) * 2u);
      std::uint32_t color = bgr555_to_argb(pal);
      if (bg[i].priority < best_priority) {
        best_priority = bg[i].priority;
        best_color = color;
        best_layer = i;
      }
    }
    line_color[static_cast<std::size_t>(x)] = best_color;
    line_prio[static_cast<std::size_t>(x)] = best_priority;
    line_layer[static_cast<std::size_t>(x)] = best_layer;
  }
}

void GbaCore::render_line_sprites(int y,
                                  std::vector<std::uint32_t>& obj_color,
                                  std::vector<int>& obj_prio,
                                  std::vector<bool>& obj_semi,
                                  std::vector<bool>& obj_present,
                                  std::vector<bool>& obj_window) {
  std::uint16_t dispcnt = bus_.read_io16(0x04000000);
  bool obj_1d = (dispcnt & 0x0040) != 0;
  constexpr std::uint32_t kOamBase = 0x07000000u;
  constexpr std::uint32_t kObjPaletteBase = 0x05000200u;

  static const int size_table[3][4][2] = {
      {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
      {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
      {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
  };

  std::fill(obj_index_buf_.begin(), obj_index_buf_.end(), 128);

  for (int i = 0; i < 128; ++i) {
    std::uint32_t base = kOamBase + static_cast<std::uint32_t>(i) * 8u;
    std::uint16_t attr0 = bus_.read_oam16_fast(base);
    std::uint16_t attr1 = bus_.read_oam16_fast(base + 2);
    std::uint16_t attr2 = bus_.read_oam16_fast(base + 4);

    bool affine = (attr0 & 0x0100) != 0;
    bool double_size = (attr0 & 0x0200) != 0;
    if (!affine && double_size) {
      continue;
    }
    int obj_mode = (attr0 >> 10) & 0x3;
    if (obj_mode == 3) {
      continue;
    }
    int shape = (attr0 >> 14) & 0x3;
    int size = (attr1 >> 14) & 0x3;
    if (shape >= 3) {
      continue;
    }
    int sprite_w = size_table[shape][size][0];
    int sprite_h = size_table[shape][size][1];
    int disp_w = sprite_w;
    int disp_h = sprite_h;
    if (affine && double_size) {
      disp_w *= 2;
      disp_h *= 2;
    }
    int sprite_y = attr0 & 0xFF;
    if (sprite_y >= 160) {
      sprite_y -= 256;
    }
    if (y < sprite_y || y >= sprite_y + disp_h) {
      continue;
    }
    int sprite_x = attr1 & 0x1FF;
    if (sprite_x >= 256) {
      sprite_x -= 512;
    }
    bool hflip = (!affine && (attr1 & 0x1000) != 0);
    bool vflip = (!affine && (attr1 & 0x2000) != 0);
    bool color_8bpp = (attr0 & 0x2000) != 0;
    int prio = (attr2 >> 10) & 0x3;
    int palette_bank = (attr2 >> 12) & 0xF;
    int tile_index = attr2 & 0x03FF;

    // OBJ attr2 tile index is addressed in 32-byte units.
    // 8bpp OBJ tiles consume 2 units (64 bytes), so both the base tile and
    // per-tile offsets must be computed in 32-byte units.
    int tile_units_per_tile = color_8bpp ? 2 : 1;
    int tiles_per_row_units = obj_1d ? (sprite_w / 8) * tile_units_per_tile : 32;

    int affine_index = (attr1 >> 9) & 0x1F;
    std::int32_t pa = 0;
    std::int32_t pb = 0;
    std::int32_t pc = 0;
    std::int32_t pd = 0;
    if (affine) {
      std::uint32_t aff_base = kOamBase + static_cast<std::uint32_t>(affine_index) * 32u;
      // Affine params are stored in the 4th halfword of each of the 4 OAM
      // entries in a matrix block: +6, +E, +16, +1E.
      pa = static_cast<std::int16_t>(bus_.read_oam16_fast(aff_base + 0x06));
      pb = static_cast<std::int16_t>(bus_.read_oam16_fast(aff_base + 0x0E));
      pc = static_cast<std::int16_t>(bus_.read_oam16_fast(aff_base + 0x16));
      pd = static_cast<std::int16_t>(bus_.read_oam16_fast(aff_base + 0x1E));
    }

    for (int x = 0; x < disp_w; ++x) {
      int screen_x = sprite_x + x;
      if (screen_x < 0 || screen_x >= width_) {
        continue;
      }
      int src_x = 0;
      int src_y = 0;
      if (affine) {
        int cx = (disp_w - 1) / 2;
        int cy = (disp_h - 1) / 2;
        int rx = x - cx;
        int ry = (y - sprite_y) - cy;
        src_x = (pa * rx + pb * ry + 128) / 256 + (sprite_w / 2);
        src_y = (pc * rx + pd * ry + 128) / 256 + (sprite_h / 2);
      } else {
        int col = hflip ? (disp_w - 1 - x) : x;
        int row = y - sprite_y;
        if (vflip) {
          row = disp_h - 1 - row;
        }
        src_x = col;
        src_y = row;
      }
      if (src_x < 0 || src_x >= sprite_w || src_y < 0 || src_y >= sprite_h) {
        continue;
      }
      int tile_x = src_x / 8;
      int tile_y = src_y / 8;
      int in_tile_x = src_x & 7;
      int in_tile_y = src_y & 7;
      int tile_offset_units = tile_y * tiles_per_row_units + tile_x * tile_units_per_tile;
      int tile_base_units = color_8bpp ? (tile_index & ~1) : tile_index;
      int tile_units = tile_base_units + tile_offset_units;
      std::uint32_t tile_addr =
          0x06010000u + static_cast<std::uint32_t>(tile_units) * 32u +
          static_cast<std::uint32_t>(in_tile_y) * (color_8bpp ? 8u : 4u);
      int color_index = 0;
      if (color_8bpp) {
        color_index = bus_.read_vram8_fast(tile_addr + static_cast<std::uint32_t>(in_tile_x));
        if (color_index == 0) {
          continue;
        }
      } else {
        std::uint8_t byte =
            bus_.read_vram8_fast(tile_addr + static_cast<std::uint32_t>(in_tile_x / 2));
        int nibble = (in_tile_x & 1) ? (byte >> 4) : (byte & 0x0F);
        if (nibble == 0) {
          continue;
        }
        color_index = (palette_bank << 4) | nibble;
      }

      if (obj_mode == 2) {
        obj_window[static_cast<std::size_t>(screen_x)] = true;
        continue;
      }

      if (!obj_present[static_cast<std::size_t>(screen_x)] ||
          prio < obj_prio[static_cast<std::size_t>(screen_x)] ||
          (prio == obj_prio[static_cast<std::size_t>(screen_x)] &&
           i < obj_index_buf_[static_cast<std::size_t>(screen_x)])) {
        std::uint16_t pal = bus_.read_palette16_fast(
            kObjPaletteBase + static_cast<std::uint32_t>(color_index) * 2u);
        obj_color[static_cast<std::size_t>(screen_x)] = bgr555_to_argb(pal);
        obj_prio[static_cast<std::size_t>(screen_x)] = prio;
        obj_semi[static_cast<std::size_t>(screen_x)] = (obj_mode == 1);
        obj_present[static_cast<std::size_t>(screen_x)] = true;
        obj_index_buf_[static_cast<std::size_t>(screen_x)] = i;
      }
    }
  }
}
void GbaCore::sync_timers_from_io() {
  static constexpr std::uint32_t kBase = 0x04000100;
  for (int i = 0; i < 4; ++i) {
    Timer& timer = timers_[i];
    std::uint32_t addr = kBase + static_cast<std::uint32_t>(i) * 4;
    std::uint16_t data = bus_.read16(addr);
    std::uint16_t control = bus_.read16(addr + 2);
    bool was_enabled = (timer.control & 0x0080) != 0;

    if (data != timer.io_data_shadow) {
      // TMxCNT_L is writable reload and readable current counter. The core
      // updates readable counter each tick and tracks that mirror so external
      // writes can be treated as reload updates.
      timer.reload = data;
      if (!was_enabled) {
        timer.counter = data;
      }
      timer.io_data_shadow = data;
    }

    if (control != timer.io_control_shadow) {
      bool now_enabled = (control & 0x0080) != 0;
      timer.control = control;
      timer.io_control_shadow = control;
      if (!was_enabled && now_enabled) {
        timer.counter = timer.reload;
        timer.accum = 0;
        std::uint16_t counter16 = static_cast<std::uint16_t>(timer.counter & 0xFFFFu);
        bus_.write16(addr, counter16);
        timer.io_data_shadow = counter16;
      } else if (was_enabled && !now_enabled) {
        timer.accum = 0;
      }
    }
  }
}

void GbaCore::step_timers(int cycles) {
  if (cycles <= 0) {
    return;
  }
  static constexpr int prescale[4] = {1, 64, 256, 1024};
  std::uint32_t overflow_count[4] = {0, 0, 0, 0};
  auto step_counter = [](Timer* timer, std::uint32_t ticks) -> std::uint32_t {
    if (!timer || ticks == 0) {
      return 0;
    }
    std::uint32_t counter = timer->counter & 0xFFFFu;
    std::uint32_t reload = timer->reload & 0xFFFFu;
    std::uint32_t until_overflow = 0x10000u - counter;
    if (ticks < until_overflow) {
      timer->counter = counter + ticks;
      return 0;
    }

    ticks -= until_overflow;
    std::uint32_t overflows = 1;
    counter = reload;
    std::uint32_t period = 0x10000u - reload;
    if (period == 0) {
      period = 0x10000u;
    }
    if (ticks > 0) {
      overflows += ticks / period;
      ticks %= period;
      counter = reload + ticks;
    }
    timer->counter = counter & 0xFFFFu;
    return overflows;
  };

  for (int i = 0; i < 4; ++i) {
    auto& timer = timers_[i];
    bool enabled = (timer.control & 0x0080) != 0;
    bool count_up = (timer.control & 0x0004) != 0;
    if (!enabled) {
      continue;
    }

    std::uint32_t ticks = 0;
    if (count_up) {
      if (i == 0) {
        continue;
      }
      ticks = overflow_count[i - 1];
    } else {
      int scale = prescale[timer.control & 0x3];
      std::uint64_t total = static_cast<std::uint64_t>(timer.accum) +
                            static_cast<std::uint64_t>(cycles);
      ticks = static_cast<std::uint32_t>(total / static_cast<std::uint64_t>(scale));
      timer.accum = static_cast<std::uint32_t>(total % static_cast<std::uint64_t>(scale));
    }

    overflow_count[i] = step_counter(&timer, ticks);
    if (overflow_count[i] > 0 && (timer.control & 0x0040)) {
      request_interrupt(3 + i);
    }

    std::uint32_t addr = 0x04000100u + static_cast<std::uint32_t>(i) * 4;
    std::uint16_t counter16 = static_cast<std::uint16_t>(timer.counter & 0xFFFFu);
    bus_.write16(addr, counter16);
    timer.io_data_shadow = counter16;
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
  bool was_vcount_match = (dispstat & 0x0004) != 0;
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
  if (!was_vcount_match && match && (dispstat & 0x0020)) {
    request_interrupt(2);
  }
  vblank_ = now_vblank;
}

void GbaCore::request_interrupt(int bit) {
  if (bit < 0 || bit > 13) {
    return;
  }
  bus_.set_if_bits(static_cast<std::uint16_t>(1u << bit));
}

bool GbaCore::interrupt_pending() const {
  std::uint16_t ie = bus_.read_io16(kRegIe);
  std::uint16_t iflag = bus_.read_io16(kRegIf);
  return (static_cast<std::uint16_t>(ie & iflag & kIrqValidMask)) != 0;
}

void GbaCore::service_interrupts() {
  std::uint16_t ime = bus_.read_io16(kRegIme);
  if ((ime & 0x0001) == 0) {
    return;
  }
  if (cpu_.cpsr() & (1u << 7)) {
    return;
  }
  std::uint16_t ie = bus_.read_io16(kRegIe);
  std::uint16_t iflag = bus_.read_io16(kRegIf);
  std::uint16_t pending = static_cast<std::uint16_t>(ie & iflag & kIrqValidMask);
  if (pending == 0) {
    return;
  }
  int irq_bit = 0;
  for (; irq_bit < 14; ++irq_bit) {
    if (pending & (1u << irq_bit)) {
      break;
    }
  }
  if (irq_bit >= 14) {
    return;
  }

  std::uint16_t next_if =
      static_cast<std::uint16_t>(iflag & ~(static_cast<std::uint16_t>(1u << irq_bit)));
  bus_.write_io16_raw(kRegIf, next_if);

  std::uint32_t pc = cpu_.pc();
  // service_interrupts() runs between instructions in this core, so PC already
  // points at the next instruction to execute. BIOS IRQ return uses
  // `subs pc, lr, #4`, so set LR to PC+4 to resume at current PC.
  std::uint32_t lr = pc + 4u;
  cpu_.set_spsr_for_mode(0x12, cpu_.cpsr());
  cpu_.set_mode(0x12);
  cpu_.set_reg(14, lr);
  cpu_.set_thumb(false);
  cpu_.set_pc(0x00000018u);
  cpu_.set_irq_disable(true);
  halted_ = false;
  swi_wait_active_ = false;
  swi_wait_mask_ = 0;
}

void GbaCore::watchdog_tick(std::uint32_t pc) {
  if (watchdog_steps_ <= 0) {
    return;
  }
  auto& sample = watchdog_[pc];
  sample.count += 1;
  sample.r0 = cpu_.reg(0);
  sample.r1 = cpu_.reg(1);
  sample.r2 = cpu_.reg(2);
  sample.r3 = cpu_.reg(3);
  sample.sp = cpu_.reg(13);
  sample.lr = cpu_.reg(14);
  sample.cpsr = cpu_.cpsr();
  sample.thumb = cpu_.thumb();
  watchdog_counter_ += 1;
  if (watchdog_counter_ >= watchdog_steps_) {
    report_watchdog();
    watchdog_counter_ = 0;
    watchdog_.clear();
  }
}

void GbaCore::report_watchdog() {
  if (watchdog_.empty()) {
    return;
  }
  std::vector<std::pair<std::uint32_t, WatchdogSample>> entries;
  entries.reserve(watchdog_.size());
  for (const auto& it : watchdog_) {
    entries.emplace_back(it.first, it.second);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
  std::cout << "GBA WATCHDOG: top PCs over " << watchdog_steps_ << " steps\n";
  int shown = 0;
  for (const auto& entry : entries) {
    if (shown >= 3) {
      break;
    }
    const auto& sample = entry.second;
    std::ostringstream line;
    line << "GBA WATCHDOG PC=0x" << std::hex << std::setw(8) << std::setfill('0')
         << entry.first << " count=" << std::dec << sample.count
         << " mode=" << (sample.thumb ? "T" : "A")
         << " R0=0x" << std::hex << std::setw(8) << sample.r0
         << " R1=0x" << std::setw(8) << sample.r1
         << " R2=0x" << std::setw(8) << sample.r2
         << " R3=0x" << std::setw(8) << sample.r3
         << " SP=0x" << std::setw(8) << sample.sp
         << " LR=0x" << std::setw(8) << sample.lr
         << " CPSR=0x" << std::setw(8) << sample.cpsr
         << std::dec;
    std::cout << line.str() << "\n";
    ++shown;
  }
}

void GbaCore::fast_boot_to_rom() {
  bios_handoff_done_ = true;
  cpu_.set_banked_sp(0x1F, 0x03007F00u);
  cpu_.set_banked_sp(0x10, 0x03007F00u);
  cpu_.set_banked_sp(0x13, 0x03007FE0u);
  cpu_.set_banked_sp(0x12, 0x03007FA0u);
  cpu_.set_banked_sp(0x11, 0x03007FA0u);
  cpu_.set_banked_sp(0x17, 0x03007F00u);
  cpu_.set_banked_sp(0x1B, 0x03007F00u);
  cpu_.set_thumb(false);
  cpu_.set_mode(0x1F);
  cpu_.set_irq_disable(false);
  // Auto-handoff can happen mid-BIOS. Reset GPRs so ROM startup sees a stable
  // post-BIOS style entry state instead of transient BIOS working values.
  for (int i = 0; i <= 12; ++i) {
    cpu_.set_reg(i, 0);
  }
  cpu_.set_reg(0, 0x08000000u);
  cpu_.set_reg(13, 0x03007F00u);
  cpu_.set_reg(14, 0);
  cpu_.set_pc(0x08000000u);
  cpu_.clear_unimplemented_count();
}

void GbaCore::apply_assert_bypass_patches() {
  if (!bypass_assert_ || assert_bypass_patched_) {
    return;
  }
  if (rom_game_code_ != "SBTP") {
    assert_bypass_patched_ = true;
    return;
  }

  const std::uint32_t patch_addrs[] = {
      0x08010D40u, // original guard
      0x08005A28u, // bn_sprite_builder.cpp.h:39 assert BL (halfword 1)
      0x08005A2Au, // bn_sprite_builder.cpp.h:39 assert BL (halfword 2)
      0x08004B8Cu, // bn_vector.h:445 assert BL (halfword 1)
      0x08004B8Eu, // bn_vector.h:445 assert BL (halfword 2)
      0x080030F6u, // bn_vector.h:445 assert BL (halfword 1)
      0x080030F8u, // bn_vector.h:445 assert BL (halfword 2)
  };
  for (std::uint32_t addr : patch_addrs) {
    bool patched = bus_.patch_rom16(addr, 0x46C0u);
    if (patched) {
      std::cout << "GBA ASSERT BYPASS PATCH: NOP at PC=0x" << std::hex << std::setw(8)
                << std::setfill('0') << addr << std::dec << "\n";
    } else {
      std::cout << "GBA ASSERT BYPASS PATCH: failed at PC=0x" << std::hex << std::setw(8)
                << std::setfill('0') << addr << std::dec << "\n";
    }
  }
  assert_bypass_patched_ = true;
}

void GbaCore::handoff_to_rom() {
  // Complete handoff using the same CPU mode/stack baseline as fastboot.
  // Auto-handoff can trigger before BIOS finishes all register setup.
  fast_boot_to_rom();
}

bool GbaCore::handle_swi_hle(std::uint32_t pc_before,
                             bool thumb_before,
                             std::uint32_t op_before,
                             int* cycles_out) {
  if (!hle_swi_enabled_) {
    return false;
  }
  std::uint32_t imm = 0;
  if (thumb_before) {
    if ((op_before & 0xFF00u) != 0xDF00u) {
      return false;
    }
    imm = op_before & 0xFFu;
  } else {
    if ((op_before & 0x0F000000u) != 0x0F000000u) {
      return false;
    }
    imm = op_before & 0x00FFFFFFu;
  }

  switch (imm) {
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0E:
    case 0x0F:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
      break;
    default:
      return false;
  }

  bool real_bios_available = bios_has_real_code(bus_.bios());

  int hle_cycles = thumb_before ? 3 : 4;
  if (hle_swi_log_limit_ > 0 && hle_swi_log_count_ < hle_swi_log_limit_) {
    ++hle_swi_log_count_;
    std::cout << "GBA HLE SWI " << (thumb_before ? "T" : "A") << " PC=0x" << std::hex
              << std::setw(8) << std::setfill('0') << pc_before << " IMM=0x" << std::setw(2)
              << imm << std::dec << "\n";
  }

  std::uint16_t wait_mask = 0;
  bool clear_if = false;
  if (imm == 0x02 || imm == 0x03) {
    // Halt/Stop: keep compatibility by suspending CPU until an IRQ wakes it.
    halted_ = true;
    swi_wait_active_ = false;
    swi_wait_mask_ = 0;
    hle_cycles = 4;
  } else if (imm == 0x05) {
    // VBlankIntrWait: wait for VBlank IRQ (bit 0).
    wait_mask = 0x0001u;
    clear_if = true;
  } else if (imm == 0x04) {
    // IntrWait: R0 controls IF clear behavior, R1 is IRQ bit mask.
    clear_if = (cpu_.reg(0) != 0);
    wait_mask = static_cast<std::uint16_t>(cpu_.reg(1) & 0xFFFFu);
  } else if (imm == 0x06 || imm == 0x07) {
    std::int32_t num = static_cast<std::int32_t>(cpu_.reg(imm == 0x06 ? 0 : 1));
    std::int32_t den = static_cast<std::int32_t>(cpu_.reg(imm == 0x06 ? 1 : 0));
    std::int32_t quotient = 0;
    std::int32_t remainder = 0;
    std::uint32_t abs_quotient = 0;
    if (!gba_signed_div(num, den, &quotient, &remainder, &abs_quotient)) {
      // BIOS loops forever on divide-by-zero; defer to real BIOS path for fidelity.
      return false;
    }
    cpu_.set_reg(0, static_cast<std::uint32_t>(quotient));
    cpu_.set_reg(1, static_cast<std::uint32_t>(remainder));
    cpu_.set_reg(3, abs_quotient);
    hle_cycles = 52;
  } else if (imm == 0x08) {
    cpu_.set_reg(0, isqrt32(cpu_.reg(0)));
    hle_cycles = 34;
  } else if (imm == 0x09) {
    std::int32_t tangent = static_cast<std::int32_t>(cpu_.reg(0));
    double value = static_cast<double>(tangent) / 65536.0;
    cpu_.set_reg(0, rad_to_bios_angle(std::atan(value)));
    hle_cycles = 32;
  } else if (imm == 0x0A) {
    std::int32_t y = static_cast<std::int32_t>(cpu_.reg(0));
    std::int32_t x = static_cast<std::int32_t>(cpu_.reg(1));
    cpu_.set_reg(0, rad_to_bios_angle(std::atan2(static_cast<double>(y), static_cast<double>(x))));
    hle_cycles = 48;
  } else if (imm == 0x0B) {
    std::uint32_t src = cpu_.reg(0);
    std::uint32_t dst = cpu_.reg(1);
    std::uint32_t ctrl = cpu_.reg(2);
    std::uint32_t count = ctrl & 0x1FFFFFu;
    if (count != 0) {
      // SWI 0x0B (CpuSet):
      // bit24 = fixed source (fill), bit26 = 32-bit transfer.
      bool fill = (ctrl & (1u << 24)) != 0;
      bool transfer32 = (ctrl & (1u << 26)) != 0;
      if (transfer32) {
        src &= ~3u;
        dst &= ~3u;
      } else {
        src &= ~1u;
        dst &= ~1u;
      }
      if (transfer32) {
        std::uint32_t value = fill ? bus_.read32(src) : 0;
        for (std::uint32_t i = 0; i < count; ++i) {
          if (!fill) {
            value = bus_.read32(src + i * 4u);
          }
          bus_.write32(dst + i * 4u, value);
        }
      } else {
        std::uint16_t value = fill ? bus_.read16(src) : 0;
        for (std::uint32_t i = 0; i < count; ++i) {
          if (!fill) {
            value = bus_.read16(src + i * 2u);
          }
          bus_.write16(dst + i * 2u, value);
        }
      }
      // Keep rough timing proportional to transfer size so frame pacing and
      // IRQ cadence stay closer to BIOS behavior.
      std::uint64_t cost = 6u + static_cast<std::uint64_t>(count) * (transfer32 ? 4u : 2u);
      if (cost > 200000u) {
        cost = 200000u;
      }
      hle_cycles = static_cast<int>(cost);
    }
  } else if (imm == 0x0C) {
    std::uint32_t src = cpu_.reg(0);
    std::uint32_t dst = cpu_.reg(1);
    std::uint32_t ctrl = cpu_.reg(2);
    std::uint32_t count = ctrl & 0x1FFFFFu;
    if (count != 0) {
      bool fill = (ctrl & (1u << 24)) != 0;
      src &= ~3u;
      dst &= ~3u;
      // CpuFastSet count is in 32-bit words and is processed in blocks of 8.
      std::uint32_t total_words = count & ~7u;
      std::uint32_t value = fill ? bus_.read32(src) : 0;
      for (std::uint32_t i = 0; i < total_words; ++i) {
        if (!fill) {
          value = bus_.read32(src + i * 4u);
        }
        bus_.write32(dst + i * 4u, value);
      }
      std::uint64_t cost = 10u + static_cast<std::uint64_t>(total_words) * 2u;
      if (cost > 200000u) {
        cost = 200000u;
      }
      hle_cycles = static_cast<int>(cost);
    }
  } else if (imm == 0x0E) {
    std::uint32_t src = cpu_.reg(0);
    std::uint32_t dst = cpu_.reg(1);
    std::uint32_t count = cpu_.reg(2);
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint32_t src_entry = src + i * 20u;
      std::uint32_t dst_entry = dst + i * 16u;
      std::int32_t tex_x = static_cast<std::int32_t>(bus_.read32(src_entry + 0u));
      std::int32_t tex_y = static_cast<std::int32_t>(bus_.read32(src_entry + 4u));
      std::int16_t scr_x = static_cast<std::int16_t>(bus_.read16(src_entry + 8u));
      std::int16_t scr_y = static_cast<std::int16_t>(bus_.read16(src_entry + 10u));
      std::int16_t sx = static_cast<std::int16_t>(bus_.read16(src_entry + 12u));
      std::int16_t sy = static_cast<std::int16_t>(bus_.read16(src_entry + 14u));
      std::uint16_t angle = bus_.read16(src_entry + 16u);

      std::int16_t pa = 0;
      std::int16_t pb = 0;
      std::int16_t pc = 0;
      std::int16_t pd = 0;
      calc_affine_matrix(sx, sy, angle, &pa, &pb, &pc, &pd);
      bus_.write16(dst_entry + 0u, static_cast<std::uint16_t>(pa));
      bus_.write16(dst_entry + 2u, static_cast<std::uint16_t>(pb));
      bus_.write16(dst_entry + 4u, static_cast<std::uint16_t>(pc));
      bus_.write16(dst_entry + 6u, static_cast<std::uint16_t>(pd));

      std::int64_t dx64 =
          static_cast<std::int64_t>(tex_x) - static_cast<std::int64_t>(pa) * scr_x -
          static_cast<std::int64_t>(pb) * scr_y;
      std::int64_t dy64 =
          static_cast<std::int64_t>(tex_y) - static_cast<std::int64_t>(pc) * scr_x -
          static_cast<std::int64_t>(pd) * scr_y;
      bus_.write32(dst_entry + 8u, static_cast<std::uint32_t>(static_cast<std::int32_t>(dx64)));
      bus_.write32(dst_entry + 12u, static_cast<std::uint32_t>(static_cast<std::int32_t>(dy64)));
    }
    std::uint64_t cost = 24u + static_cast<std::uint64_t>(count) * 52u;
    if (cost > 200000u) {
      cost = 200000u;
    }
    hle_cycles = static_cast<int>(cost);
  } else if (imm == 0x0F) {
    std::uint32_t src = cpu_.reg(0);
    std::uint32_t dst = cpu_.reg(1);
    std::uint32_t count = cpu_.reg(2);
    std::uint32_t offset = cpu_.reg(3);
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint32_t src_entry = src + i * 8u;
      std::uint32_t dst_entry = dst + i * (offset * 4u);
      std::int16_t sx = static_cast<std::int16_t>(bus_.read16(src_entry + 0u));
      std::int16_t sy = static_cast<std::int16_t>(bus_.read16(src_entry + 2u));
      std::uint16_t angle = bus_.read16(src_entry + 4u);

      std::int16_t pa = 0;
      std::int16_t pb = 0;
      std::int16_t pc = 0;
      std::int16_t pd = 0;
      calc_affine_matrix(sx, sy, angle, &pa, &pb, &pc, &pd);
      bus_.write16(dst_entry + offset * 0u, static_cast<std::uint16_t>(pa));
      bus_.write16(dst_entry + offset * 1u, static_cast<std::uint16_t>(pb));
      bus_.write16(dst_entry + offset * 2u, static_cast<std::uint16_t>(pc));
      bus_.write16(dst_entry + offset * 3u, static_cast<std::uint16_t>(pd));
    }
    std::uint64_t cost = 20u + static_cast<std::uint64_t>(count) * 40u;
    if (cost > 200000u) {
      cost = 200000u;
    }
    hle_cycles = static_cast<int>(cost);
  } else if (imm == 0x11 || imm == 0x12 || imm == 0x15 || imm == 0x16) {
    std::uint32_t src = cpu_.reg(0);
    std::uint32_t dst = cpu_.reg(1);
    bool vram_mode = (imm == 0x12 || imm == 0x16);
    bool lz_mode = (imm == 0x11 || imm == 0x12);
    std::vector<std::uint8_t> decoded;
    bool ok = lz_mode ? decode_lz77(bus_, src, &decoded) : decode_rl(bus_, src, &decoded);
    if (!ok) {
      if (real_bios_available) {
        // Fall back to BIOS execution when available for malformed/unsupported streams.
        return false;
      }
      decoded.clear();
    } else {
      write_swi_output(&bus_, dst, decoded, vram_mode);
    }
    std::uint64_t cost = 18u + static_cast<std::uint64_t>(decoded.size()) * (vram_mode ? 3u : 2u);
    if (cost > 200000u) {
      cost = 200000u;
    }
    hle_cycles = static_cast<int>(cost);
  } else if (imm == 0x13 || imm == 0x14) {
    // Huffman and differential filters are left to BIOS for now.
    if (real_bios_available) {
      return false;
    }
    hle_cycles = 8;
  }

  if (clear_if && wait_mask != 0) {
    std::uint16_t iflag = bus_.read_io16(kRegIf);
    bus_.write_io16_raw(kRegIf, static_cast<std::uint16_t>(iflag & ~wait_mask));
  }

  if (wait_mask != 0) {
    std::uint16_t ie = bus_.read_io16(kRegIe);
    std::uint16_t iflag = bus_.read_io16(kRegIf);
    if ((ie & iflag & wait_mask) == 0) {
      halted_ = true;
      swi_wait_active_ = true;
      swi_wait_mask_ = wait_mask;
    } else {
      swi_wait_active_ = false;
      swi_wait_mask_ = 0;
    }
  }

  if (cycles_out) {
    *cycles_out = hle_cycles;
  }
  cpu_.set_pc(pc_before + (thumb_before ? 2u : 4u));
  return true;
}

void GbaCore::auto_patch_tick(std::uint32_t pc_before,
                              std::uint32_t pc_after,
                              std::uint32_t op_before,
                              bool thumb_before) {
  if (!auto_patch_hang_ || auto_patch_threshold_ <= 0) {
    return;
  }
  auto is_valid_ptr = [](std::uint32_t addr) -> bool {
    if (addr >= 0x02000000u && addr <= 0x02FFFFFFu) {
      return true;
    }
    if (addr >= 0x03000000u && addr <= 0x03FFFFFFu) {
      return true;
    }
    if (addr >= 0x04000000u && addr <= 0x040003FFu) {
      return true;
    }
    if (addr >= 0x05000000u && addr <= 0x050003FFu) {
      return true;
    }
    if (addr >= 0x06000000u && addr <= 0x06017FFFu) {
      return true;
    }
    if (addr >= 0x07000000u && addr <= 0x070003FFu) {
      return true;
    }
    if (addr >= 0x08000000u && addr <= 0x0DFFFFFFu) {
      return true;
    }
    if (addr >= 0x0E000000u && addr <= 0x0E00FFFFu) {
      return true;
    }
    return false;
  };

  if (pc_before < 0x08000000u || pc_before > 0x0DFFFFFFu) {
    loop_count_ = 0;
    return;
  }
  if (auto_patch_start_ != 0 || auto_patch_end_ != 0) {
    std::uint32_t start = auto_patch_start_;
    std::uint32_t end = auto_patch_end_;
    if (start > end) {
      std::swap(start, end);
    }
    if (auto_patch_span_ > 0) {
      std::uint32_t span = auto_patch_span_;
      if (start > span) {
        start -= span;
      } else {
        start = 0;
      }
      if (end > 0xFFFFFFFFu - span) {
        end = 0xFFFFFFFFu;
      } else {
        end += span;
      }
    }
    if (pc_before < start || pc_before > end) {
      return;
    }
  }
  if (pc_after > pc_before) {
    return;
  }
  std::uint32_t span = pc_before - pc_after;
  if (span > auto_patch_span_) {
    return;
  }
  if (loop_pc_ == pc_before && loop_target_ == pc_after && loop_thumb_ == thumb_before) {
    ++loop_count_;
  } else {
    loop_pc_ = pc_before;
    loop_target_ = pc_after;
    loop_thumb_ = thumb_before;
    loop_count_ = 1;
  }
  int threshold = auto_patch_threshold_;
  if (!is_valid_ptr(cpu_.reg(0))) {
    threshold = std::min(threshold, 256);
  }
  if (loop_count_ < threshold) {
    return;
  }
  loop_count_ = 0;
  if (auto_patched_pcs_.find(pc_before) != auto_patched_pcs_.end()) {
    return;
  }

  bool branch_matches = false;
  if (thumb_before) {
    std::uint16_t op = static_cast<std::uint16_t>(op_before & 0xFFFFu);
    std::uint32_t target = 0;
    if ((op & 0xF800u) == 0xE000u) {
      std::int32_t imm11 = static_cast<std::int32_t>(op & 0x7FFu);
      if (imm11 & 0x400) {
        imm11 |= ~0x7FF;
      }
      target = pc_before + 4u + (static_cast<std::uint32_t>(imm11) << 1);
      branch_matches = (target == pc_after);
    } else if ((op & 0xF000u) == 0xD000u && (op & 0x0F00u) != 0x0F00u) {
      std::int8_t imm8 = static_cast<std::int8_t>(op & 0xFFu);
      target = pc_before + 4u + (static_cast<std::int32_t>(imm8) << 1);
      branch_matches = (target == pc_after);
    }
  } else {
    std::uint32_t op = op_before;
    if ((op & 0x0E000000u) == 0x0A000000u) {
      std::int32_t imm24 = static_cast<std::int32_t>(op & 0x00FFFFFFu);
      if (imm24 & 0x00800000) {
        imm24 |= ~0x00FFFFFF;
      }
      std::uint32_t target = pc_before + 8u + (static_cast<std::uint32_t>(imm24) << 2);
      branch_matches = (target == pc_after);
    }
  }

  if (!branch_matches) {
    return;
  }

  bool patched = false;
  if (thumb_before) {
    patched = bus_.patch_rom16(pc_before, 0x46C0u);
  } else {
    patched = bus_.patch_rom32(pc_before, 0xE1A00000u);
  }
  if (patched) {
    auto_patched_pcs_.insert(pc_before);
    std::cout << "GBA AUTO PATCH: loop break at PC=0x" << std::hex
              << std::setw(8) << std::setfill('0') << pc_before
              << " -> NOP\n" << std::dec;
  }
}

std::string GbaCore::read_rom_string(std::uint32_t address, std::size_t max_len) const {
  if (address < 0x08000000u || max_len == 0) {
    return {};
  }
  std::string result;
  result.reserve(max_len);
  for (std::size_t i = 0; i < max_len; ++i) {
    std::uint8_t c = bus_.read8(address + static_cast<std::uint32_t>(i));
    if (c == 0) {
      break;
    }
    if (c < 0x20 || c > 0x7E) {
      break;
    }
    result.push_back(static_cast<char>(c));
  }
  return result;
}

bool GbaCore::handle_butano_assert(std::uint32_t pc_before, bool thumb_before, int* cycles_out) {
  constexpr std::uint32_t kButanoAssertPc = 0x0800A444u;
  if (!thumb_before || pc_before != kButanoAssertPc) {
    return false;
  }
  if (!trace_assert_ && !bypass_assert_) {
    return false;
  }

  const std::uint32_t expr_ptr = cpu_.reg(0);
  const std::uint32_t file_ptr = cpu_.reg(1);
  const std::uint32_t func_ptr = cpu_.reg(2);
  const std::uint32_t line = cpu_.reg(4);
  const std::uint32_t lr = cpu_.reg(14);
  const std::uint32_t sp = cpu_.reg(13);
  const std::string expr = read_rom_string(expr_ptr, 160);
  const std::string file = read_rom_string(file_ptr, 160);
  const std::string func = read_rom_string(func_ptr, 80);

  if (trace_assert_) {
    std::cout << "GBA ASSERT: file=" << (file.empty() ? "<unknown>" : file)
              << " line=" << std::dec << line
              << " func=" << (func.empty() ? "<unknown>" : func)
              << " expr=" << (expr.empty() ? "<unknown>" : expr)
              << " LR=0x" << std::hex << std::setw(8) << std::setfill('0') << lr
              << " SP=0x" << std::setw(8) << sp << std::dec << "\n";
  }

  (void)cycles_out;
  return false;
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
    if (ch == 0 && start_timing == 3) {
      // DMA0 "special" timing is prohibited; map to immediate for compatibility.
      start_timing = 0;
    }
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
    if (start_timing == 3 && (ch == 1 || ch == 2)) {
      // FIFO sound DMA: fixed 32-bit units.
      word = true;
    }
    std::uint32_t count_mask = (ch == 3) ? 0xFFFFu : 0x3FFFu;
    std::uint32_t length = static_cast<std::uint32_t>(count) & count_mask;
    if (start_timing == 3 && (ch == 1 || ch == 2)) {
      length = 4;
    }
    if (length == 0) {
      length = (ch == 3) ? 0x10000u : 0x4000u;
    }
    std::uint32_t src_ctrl = (ctrl >> 7) & 0x3;
    std::uint32_t dst_ctrl = (ctrl >> 5) & 0x3;
    if (start_timing == 3 && (ch == 1 || ch == 2)) {
      dst_ctrl = 2;
      src_ctrl = 0;
    }
    std::uint32_t step = word ? 4u : 2u;
    std::uint32_t align_mask = ~(step - 1u);
    std::uint32_t cur_src = src & align_mask;
    std::uint32_t cur_dst = dst & align_mask;
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
      write_dst = dst & align_mask;
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
