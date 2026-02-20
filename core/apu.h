#pragma once

#include <cstdint>
#include <vector>

#include "mmu.h"

namespace gbemu::core {

class Apu {
 public:
  void reset();
  void step(int cycles, Mmu* mmu);
  void generate_samples(int sample_rate, int count, Mmu* mmu, std::vector<std::int16_t>* out);
  void serialize(std::vector<std::uint8_t>* out) const;
  bool deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error);

 private:
  struct Channel {
    bool enabled = false;
    int length = 0;
    bool length_enabled = false;
    int volume = 0;
    int envelope_period = 0;
    int envelope_timer = 0;
    int envelope_dir = 1;
  };

  struct Channel1 {
    Channel base;
    int sweep_period = 0;
    int sweep_timer = 0;
    bool sweep_decrease = false;
    int sweep_shift = 0;
    int shadow_freq = 0;
  };

  struct Channel3 {
    bool enabled = false;
    int length = 0;
    bool length_enabled = false;
  };

  struct Channel4 {
    Channel base;
  };

  void clock_length();
  void clock_envelope();
  void clock_sweep();
  void trigger_ch1(Mmu* mmu);
  void trigger_ch2(Mmu* mmu);
  void trigger_ch3(Mmu* mmu);
  void trigger_ch4(Mmu* mmu);
  void update_nr52(Mmu* mmu);
  void sync_from_registers(Mmu* mmu);

  int frame_step_ = 0;
  int frame_counter_ = 0;

  Channel1 ch1_{};
  Channel ch2_{};
  Channel3 ch3_{};
  Channel4 ch4_{};

  double phase1_ = 0.0;
  double phase2_ = 0.0;
  double phase3_ = 0.0;
  double phase4_ = 0.0;
  std::uint16_t lfsr_ = 0x7FFF;
};

} // namespace gbemu::core
