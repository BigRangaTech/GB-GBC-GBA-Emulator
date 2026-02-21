#pragma once

#include <cstdint>
#include <string>
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

 private:
  void render_placeholder();
  void render_line(int y);
  void render_line_mode0(int y);
  void render_line_mode3(int y);
  void render_line_mode4(int y);

  GbaBus bus_;
  GbaCpu cpu_;
  std::vector<std::uint32_t> framebuffer_;
  std::uint64_t frame_counter_ = 0;
  int width_ = 240;
  int height_ = 160;
  bool bios_handoff_done_ = false;
  int bios_watchdog_cycles_ = 0;
  int line_cycles_ = 0;
  int vcount_ = 0;
  bool vblank_ = false;
  bool hblank_ = false;
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
  void fast_boot_to_rom();
};

} // namespace gbemu::core
