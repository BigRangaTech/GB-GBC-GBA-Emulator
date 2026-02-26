#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gba_bus.h"
#include "gba_cpu.h"

namespace gbemu::core {

class GbaCore {
 public:
  bool load(const std::vector<std::uint8_t>& rom,
            const std::vector<std::uint8_t>& bios,
            std::string* error);
  void reset();
  void step_frame();

  int framebuffer_width() const { return width_; }
  int framebuffer_height() const { return height_; }
  int framebuffer_stride_bytes() const { return width_ * 4; }
  const std::uint32_t* framebuffer() const { return framebuffer_.data(); }

  bool cpu_faulted() const { return cpu_.faulted(); }
  const std::string& cpu_fault_reason() const { return cpu_.fault_reason(); }
  std::uint32_t cpu_pc() const { return cpu_.pc(); }
  void set_trace(int steps, bool trace_io);
  void set_trace_after_rom(int steps, bool trace_io);
  void set_log_unimplemented(int limit) { cpu_.set_log_unimplemented(limit); }
  void set_watch_video_io(int limit) { bus_.set_watch_video_io_limit(limit); }
  void set_watch_io_reads(int limit) { bus_.set_watch_io_read_limit(limit); }
  void set_mem_watch(std::uint32_t start,
                     std::uint32_t end,
                     int count,
                     bool read,
                     bool write);
  void set_log_swi(int limit);
  void set_watchdog_steps(int steps);
  void set_pc_watch(std::uint32_t start, std::uint32_t end, int count);
  void set_keyinput(std::uint16_t value);
  void set_gba_color_correction(bool enabled) { gba_color_correct_ = enabled; }
  void set_auto_handoff(bool enabled) { auto_handoff_enabled_ = enabled; }
  void set_fastboot(bool enabled);
  void handoff_to_rom();
  void set_auto_patch_hang(bool enabled) { auto_patch_hang_ = enabled; }
  void set_auto_patch_threshold(int count) { auto_patch_threshold_ = count; }
  void set_auto_patch_span(std::uint32_t span) { auto_patch_span_ = span; }
  void set_auto_patch_range(std::uint32_t start, std::uint32_t end) {
    auto_patch_start_ = start;
    auto_patch_end_ = end;
  }
  void set_hle_swi(bool enabled) { hle_swi_enabled_ = enabled; }
  void set_trace_assert(bool enabled) { trace_assert_ = enabled; }
  void set_bypass_assert(bool enabled) { bypass_assert_ = enabled; }

 private:
  void render_placeholder();
  void render_line(int y);
  void render_line_mode0(int y,
                         std::vector<std::uint32_t>& line_color,
                         std::vector<int>& line_prio,
                         std::vector<int>& line_layer,
                         int bg_mask);
  void render_line_mode3(int y,
                         std::vector<std::uint32_t>& line_color,
                         std::vector<int>& line_prio,
                         std::vector<int>& line_layer);
  void render_line_mode4(int y,
                         std::vector<std::uint32_t>& line_color,
                         std::vector<int>& line_prio,
                         std::vector<int>& line_layer);
  void render_line_affine_bg(int y,
                             int bg_index,
                             std::vector<std::uint32_t>& line_color,
                             std::vector<int>& line_prio,
                             std::vector<int>& line_layer);
  void render_line_sprites(int y,
                           std::vector<std::uint32_t>& obj_color,
                           std::vector<int>& obj_prio,
                           std::vector<bool>& obj_semi,
                           std::vector<bool>& obj_present,
                           std::vector<bool>& obj_window);
  void ensure_render_buffers();

  GbaBus bus_;
  GbaCpu cpu_;
  std::vector<std::uint32_t> framebuffer_;
  std::vector<std::uint32_t> line_color_buf_;
  std::vector<int> line_prio_buf_;
  std::vector<int> line_layer_buf_;
  std::vector<std::uint32_t> obj_color_buf_;
  std::vector<int> obj_prio_buf_;
  std::vector<bool> obj_semi_buf_;
  std::vector<bool> obj_present_buf_;
  std::vector<bool> obj_window_buf_;
  std::vector<std::uint8_t> win_mask_buf_;
  std::vector<int> obj_index_buf_;
  std::uint64_t frame_counter_ = 0;
  int width_ = 240;
  int height_ = 160;
  bool bios_handoff_done_ = false;
  int bios_watchdog_cycles_ = 0;
  int line_cycles_ = 0;
  int vcount_ = 0;
  bool vblank_ = false;
  bool hblank_ = false;
  bool halted_ = false;
  bool swi_wait_active_ = false;
  std::uint16_t swi_wait_mask_ = 0;
  int trace_steps_remaining_ = 0;
  bool trace_stop_on_rom_ = true;
  bool trace_stop_notified_ = false;
  bool trace_start_on_rom_ = false;
  bool trace_start_notified_ = false;
  int post_trace_remaining_ = 0;
  bool pc_watch_enabled_ = false;
  std::uint32_t pc_watch_start_ = 0;
  std::uint32_t pc_watch_end_ = 0;
  int pc_watch_remaining_ = 0;
  std::uint16_t keyinput_ = 0x03FF;
  bool gba_color_correct_ = false;
  bool auto_handoff_enabled_ = true;
  bool fastboot_enabled_ = false;
  bool auto_patch_hang_ = false;
  bool hle_swi_enabled_ = false;
  int auto_patch_threshold_ = 0;
  std::uint32_t auto_patch_span_ = 0x40;
  std::uint32_t auto_patch_start_ = 0;
  std::uint32_t auto_patch_end_ = 0;
  std::uint32_t loop_pc_ = 0;
  std::uint32_t loop_target_ = 0;
  int loop_count_ = 0;
  bool loop_thumb_ = false;
  std::unordered_set<std::uint32_t> auto_patched_pcs_;
  int watchdog_steps_ = 0;
  int watchdog_counter_ = 0;
  struct WatchdogSample {
    int count = 0;
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
    std::uint32_t r2 = 0;
    std::uint32_t r3 = 0;
    std::uint32_t sp = 0;
    std::uint32_t lr = 0;
    std::uint32_t cpsr = 0;
    bool thumb = false;
  };
  std::unordered_map<std::uint32_t, WatchdogSample> watchdog_;
  struct Timer {
    std::uint32_t reload = 0;
    std::uint32_t counter = 0;
    std::uint16_t control = 0;
    std::uint32_t accum = 0;
  };
  Timer timers_[4]{};
  struct DmaChannel {
    bool active = false;
  };
  DmaChannel dma_[4]{};

  void sync_timers_from_io();
  void step_timers(int cycles);
  void step_dma();
  void trigger_dma(int timing);
  void run_dma(int timing);
  void step_ppu(int cycles);
  void update_dispstat();
  void request_interrupt(int bit);
  void service_interrupts();
  bool interrupt_pending() const;
  void watchdog_tick(std::uint32_t pc);
  void report_watchdog();
  void fast_boot_to_rom();
  bool handle_swi_hle(std::uint32_t pc_before, bool thumb_before, std::uint32_t op_before, int* cycles_out);
  void apply_assert_bypass_patches();
  void auto_patch_tick(std::uint32_t pc_before,
                       std::uint32_t pc_after,
                       std::uint32_t op_before,
                       bool thumb_before);
  std::string read_rom_string(std::uint32_t address, std::size_t max_len) const;
  bool handle_butano_assert(std::uint32_t pc_before, bool thumb_before, int* cycles_out);

  bool trace_assert_ = false;
  bool bypass_assert_ = false;
  bool assert_bypass_patched_ = false;
  int hle_swi_log_limit_ = 0;
  int hle_swi_log_count_ = 0;
  std::string rom_game_code_;
};

} // namespace gbemu::core
