#include "apu.h"

#include <algorithm>
#include <cmath>

#include "state_io.h"

namespace gbemu::core {

namespace {

constexpr int kFrameSequencerHz = 512;

int length_from_nr1(std::uint8_t nr11) {
  int len = 64 - (nr11 & 0x3F);
  return len < 0 ? 0 : len;
}

int length_from_nr2(std::uint8_t nr21) {
  int len = 64 - (nr21 & 0x3F);
  return len < 0 ? 0 : len;
}

int length_from_nr3(std::uint8_t nr31) {
  int len = 256 - nr31;
  return len < 0 ? 0 : len;
}

int length_from_nr4(std::uint8_t nr41) {
  int len = 64 - (nr41 & 0x3F);
  return len < 0 ? 0 : len;
}

} // namespace

void Apu::reset() {
  phase1_ = 0.0;
  phase2_ = 0.0;
  phase3_ = 0.0;
  phase4_ = 0.0;
  lfsr_ = 0x7FFF;
  frame_step_ = 0;
  frame_counter_ = 0;
  ch1_ = {};
  ch2_ = {};
  ch3_ = {};
  ch4_ = {};
}

void Apu::serialize(std::vector<std::uint8_t>* out) const {
  if (!out) {
    return;
  }
  using namespace gbemu::core::state_io;
  write_f64(*out, phase1_);
  write_f64(*out, phase2_);
  write_f64(*out, phase3_);
  write_f64(*out, phase4_);
  write_u16(*out, lfsr_);
  write_u32(*out, static_cast<std::uint32_t>(frame_step_));
  write_u32(*out, static_cast<std::uint32_t>(frame_counter_));

  auto write_channel = [&](const Channel& ch) {
    write_bool(*out, ch.enabled);
    write_u32(*out, static_cast<std::uint32_t>(ch.length));
    write_bool(*out, ch.length_enabled);
    write_u32(*out, static_cast<std::uint32_t>(ch.volume));
    write_u32(*out, static_cast<std::uint32_t>(ch.envelope_period));
    write_u32(*out, static_cast<std::uint32_t>(ch.envelope_timer));
    write_u32(*out, static_cast<std::uint32_t>(ch.envelope_dir));
  };

  write_channel(ch1_.base);
  write_u32(*out, static_cast<std::uint32_t>(ch1_.sweep_period));
  write_u32(*out, static_cast<std::uint32_t>(ch1_.sweep_timer));
  write_bool(*out, ch1_.sweep_decrease);
  write_u32(*out, static_cast<std::uint32_t>(ch1_.sweep_shift));
  write_u32(*out, static_cast<std::uint32_t>(ch1_.shadow_freq));

  write_channel(ch2_);

  write_bool(*out, ch3_.enabled);
  write_u32(*out, static_cast<std::uint32_t>(ch3_.length));
  write_bool(*out, ch3_.length_enabled);

  write_channel(ch4_.base);
}

bool Apu::deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error) {
  using namespace gbemu::core::state_io;
  if (!read_f64(data, offset, phase1_)) return false;
  if (!read_f64(data, offset, phase2_)) return false;
  if (!read_f64(data, offset, phase3_)) return false;
  if (!read_f64(data, offset, phase4_)) return false;
  if (!read_u16(data, offset, lfsr_)) return false;
  std::uint32_t v32 = 0;
  if (!read_u32(data, offset, v32)) return false;
  frame_step_ = static_cast<int>(v32);
  if (!read_u32(data, offset, v32)) return false;
  frame_counter_ = static_cast<int>(v32);

  auto read_channel = [&](Channel& ch) -> bool {
    if (!read_bool(data, offset, ch.enabled)) return false;
    if (!read_u32(data, offset, v32)) return false;
    ch.length = static_cast<int>(v32);
    if (!read_bool(data, offset, ch.length_enabled)) return false;
    if (!read_u32(data, offset, v32)) return false;
    ch.volume = static_cast<int>(v32);
    if (!read_u32(data, offset, v32)) return false;
    ch.envelope_period = static_cast<int>(v32);
    if (!read_u32(data, offset, v32)) return false;
    ch.envelope_timer = static_cast<int>(v32);
    if (!read_u32(data, offset, v32)) return false;
    ch.envelope_dir = static_cast<int>(v32);
    return true;
  };

  if (!read_channel(ch1_.base)) return false;
  if (!read_u32(data, offset, v32)) return false;
  ch1_.sweep_period = static_cast<int>(v32);
  if (!read_u32(data, offset, v32)) return false;
  ch1_.sweep_timer = static_cast<int>(v32);
  if (!read_bool(data, offset, ch1_.sweep_decrease)) return false;
  if (!read_u32(data, offset, v32)) return false;
  ch1_.sweep_shift = static_cast<int>(v32);
  if (!read_u32(data, offset, v32)) return false;
  ch1_.shadow_freq = static_cast<int>(v32);

  if (!read_channel(ch2_)) return false;

  if (!read_bool(data, offset, ch3_.enabled)) return false;
  if (!read_u32(data, offset, v32)) return false;
  ch3_.length = static_cast<int>(v32);
  if (!read_bool(data, offset, ch3_.length_enabled)) return false;

  if (!read_channel(ch4_.base)) return false;

  (void)error;
  return true;
}

void Apu::step(int cycles, Mmu* mmu) {
  if (!mmu) {
    return;
  }

  std::uint8_t nr52 = mmu->read_u8(0xFF26);
  if ((nr52 & 0x80) == 0) {
    return;
  }

  frame_counter_ += cycles;
  int period = 4194304 / kFrameSequencerHz;
  while (frame_counter_ >= period) {
    frame_counter_ -= period;
    switch (frame_step_) {
      case 0:
      case 4:
        clock_length();
        break;
      case 2:
      case 6:
        clock_length();
        clock_sweep();
        break;
      case 7:
        clock_envelope();
        break;
      default:
        break;
    }
    frame_step_ = (frame_step_ + 1) & 7;
  }

  sync_from_registers(mmu);
}

void Apu::clock_length() {
  auto tick = [](int& len, bool enabled, bool& on) {
    if (!enabled || len <= 0) {
      return;
    }
    --len;
    if (len == 0) {
      on = false;
    }
  };

  tick(ch1_.base.length, ch1_.base.length_enabled, ch1_.base.enabled);
  tick(ch2_.length, ch2_.length_enabled, ch2_.enabled);
  if (ch3_.length_enabled && ch3_.length > 0) {
    --ch3_.length;
    if (ch3_.length == 0) {
      ch3_.enabled = false;
    }
  }
  tick(ch4_.base.length, ch4_.base.length_enabled, ch4_.base.enabled);
}

void Apu::clock_envelope() {
  auto tick_env = [](Channel& ch) {
    if (ch.envelope_period == 0) {
      return;
    }
    if (--ch.envelope_timer <= 0) {
      ch.envelope_timer = ch.envelope_period;
      int next = ch.volume + ch.envelope_dir;
      if (next >= 0 && next <= 15) {
        ch.volume = next;
      }
    }
  };

  tick_env(ch1_.base);
  tick_env(ch2_);
  tick_env(ch4_.base);
}

void Apu::clock_sweep() {
  if (!ch1_.base.enabled || ch1_.sweep_period == 0) {
    return;
  }
  if (--ch1_.sweep_timer <= 0) {
    ch1_.sweep_timer = ch1_.sweep_period;
    int delta = ch1_.shadow_freq >> ch1_.sweep_shift;
    int next = ch1_.sweep_decrease ? (ch1_.shadow_freq - delta) : (ch1_.shadow_freq + delta);
    if (next > 2047) {
      ch1_.base.enabled = false;
      return;
    }
    if (ch1_.sweep_shift > 0) {
      ch1_.shadow_freq = next;
    }
  }
}

void Apu::trigger_ch1(Mmu* mmu) {
  std::uint8_t nr10 = mmu->read_u8(0xFF10);
  std::uint8_t nr11 = mmu->read_u8(0xFF11);
  std::uint8_t nr12 = mmu->read_u8(0xFF12);
  std::uint8_t nr13 = mmu->read_u8(0xFF13);
  std::uint8_t nr14 = mmu->read_u8(0xFF14);

  ch1_.base.enabled = true;
  ch1_.base.length = length_from_nr1(nr11);
  ch1_.base.length_enabled = (nr14 & 0x40) != 0;
  ch1_.base.volume = (nr12 >> 4) & 0x0F;
  ch1_.base.envelope_period = nr12 & 0x07;
  ch1_.base.envelope_timer = ch1_.base.envelope_period == 0 ? 8 : ch1_.base.envelope_period;
  ch1_.base.envelope_dir = (nr12 & 0x08) ? 1 : -1;

  ch1_.sweep_period = (nr10 >> 4) & 0x07;
  ch1_.sweep_decrease = (nr10 & 0x08) != 0;
  ch1_.sweep_shift = nr10 & 0x07;
  ch1_.sweep_timer = ch1_.sweep_period == 0 ? 8 : ch1_.sweep_period;
  ch1_.shadow_freq = ((nr14 & 0x07) << 8) | nr13;
}

void Apu::trigger_ch2(Mmu* mmu) {
  std::uint8_t nr21 = mmu->read_u8(0xFF16);
  std::uint8_t nr22 = mmu->read_u8(0xFF17);
  std::uint8_t nr24 = mmu->read_u8(0xFF19);

  ch2_.enabled = true;
  ch2_.length = length_from_nr2(nr21);
  ch2_.length_enabled = (nr24 & 0x40) != 0;
  ch2_.volume = (nr22 >> 4) & 0x0F;
  ch2_.envelope_period = nr22 & 0x07;
  ch2_.envelope_timer = ch2_.envelope_period == 0 ? 8 : ch2_.envelope_period;
  ch2_.envelope_dir = (nr22 & 0x08) ? 1 : -1;
}

void Apu::trigger_ch3(Mmu* mmu) {
  std::uint8_t nr31 = mmu->read_u8(0xFF1B);
  std::uint8_t nr34 = mmu->read_u8(0xFF1E);

  ch3_.enabled = true;
  ch3_.length = length_from_nr3(nr31);
  ch3_.length_enabled = (nr34 & 0x40) != 0;
}

void Apu::trigger_ch4(Mmu* mmu) {
  std::uint8_t nr41 = mmu->read_u8(0xFF20);
  std::uint8_t nr42 = mmu->read_u8(0xFF21);
  std::uint8_t nr44 = mmu->read_u8(0xFF23);

  ch4_.base.enabled = true;
  ch4_.base.length = length_from_nr4(nr41);
  ch4_.base.length_enabled = (nr44 & 0x40) != 0;
  ch4_.base.volume = (nr42 >> 4) & 0x0F;
  ch4_.base.envelope_period = nr42 & 0x07;
  ch4_.base.envelope_timer = ch4_.base.envelope_period == 0 ? 8 : ch4_.base.envelope_period;
  ch4_.base.envelope_dir = (nr42 & 0x08) ? 1 : -1;
}

void Apu::update_nr52(Mmu* mmu) {
  std::uint8_t nr52 = 0x80;
  if (ch1_.base.enabled) nr52 |= 0x01;
  if (ch2_.enabled) nr52 |= 0x02;
  if (ch3_.enabled) nr52 |= 0x04;
  if (ch4_.base.enabled) nr52 |= 0x08;
  mmu->write_u8(0xFF26, nr52);
}

void Apu::sync_from_registers(Mmu* mmu) {
  std::uint8_t nr14 = mmu->read_u8(0xFF14);
  std::uint8_t nr24 = mmu->read_u8(0xFF19);
  std::uint8_t nr34 = mmu->read_u8(0xFF1E);
  std::uint8_t nr44 = mmu->read_u8(0xFF23);

  if (nr14 & 0x80) {
    trigger_ch1(mmu);
    mmu->write_u8(0xFF14, static_cast<std::uint8_t>(nr14 & ~0x80));
  }
  if (nr24 & 0x80) {
    trigger_ch2(mmu);
    mmu->write_u8(0xFF19, static_cast<std::uint8_t>(nr24 & ~0x80));
  }
  if (nr34 & 0x80) {
    trigger_ch3(mmu);
    mmu->write_u8(0xFF1E, static_cast<std::uint8_t>(nr34 & ~0x80));
  }
  if (nr44 & 0x80) {
    trigger_ch4(mmu);
    mmu->write_u8(0xFF23, static_cast<std::uint8_t>(nr44 & ~0x80));
  }

  update_nr52(mmu);
}

static double duty_value(int duty, double phase) {
  double threshold = 0.125;
  switch (duty & 0x03) {
    case 0: threshold = 0.125; break;
    case 1: threshold = 0.25; break;
    case 2: threshold = 0.5; break;
    case 3: threshold = 0.75; break;
    default: break;
  }
  return phase < threshold ? 1.0 : -1.0;
}

static double clamp_sample(double v) {
  if (v > 1.0) return 1.0;
  if (v < -1.0) return -1.0;
  return v;
}

void Apu::generate_samples(int sample_rate, int count, Mmu* mmu, std::vector<std::int16_t>* out) {
  if (!out || count <= 0) {
    return;
  }
  out->assign(static_cast<std::size_t>(count) * 2, 0);
  if (!mmu || sample_rate <= 0) {
    return;
  }

  std::uint8_t nr52 = mmu->read_u8(0xFF26);
  if ((nr52 & 0x80) == 0) {
    return;
  }

  std::uint8_t nr50 = mmu->read_u8(0xFF24);
  std::uint8_t nr51 = mmu->read_u8(0xFF25);

  double left_vol = static_cast<double>((nr50 >> 4) & 0x07) / 7.0;
  double right_vol = static_cast<double>(nr50 & 0x07) / 7.0;

  std::uint8_t nr11 = mmu->read_u8(0xFF11);
  std::uint8_t nr12 = mmu->read_u8(0xFF12);
  std::uint8_t nr13 = mmu->read_u8(0xFF13);
  std::uint8_t nr14 = mmu->read_u8(0xFF14);
  std::uint8_t nr21 = mmu->read_u8(0xFF16);
  std::uint8_t nr22 = mmu->read_u8(0xFF17);
  std::uint8_t nr23 = mmu->read_u8(0xFF18);
  std::uint8_t nr24 = mmu->read_u8(0xFF19);
  std::uint8_t nr30 = mmu->read_u8(0xFF1A);
  std::uint8_t nr32 = mmu->read_u8(0xFF1C);
  std::uint8_t nr33 = mmu->read_u8(0xFF1D);
  std::uint8_t nr34 = mmu->read_u8(0xFF1E);
  std::uint8_t nr41 = mmu->read_u8(0xFF20);
  std::uint8_t nr42 = mmu->read_u8(0xFF21);
  std::uint8_t nr43 = mmu->read_u8(0xFF22);
  std::uint8_t nr44 = mmu->read_u8(0xFF23);

  int freq1 = ((nr14 & 0x07) << 8) | nr13;
  int freq2 = ((nr24 & 0x07) << 8) | nr23;
  int freq3 = ((nr34 & 0x07) << 8) | nr33;

  double hz1 = (freq1 >= 2048) ? 0.0 : 131072.0 / (2048.0 - freq1);
  double hz2 = (freq2 >= 2048) ? 0.0 : 131072.0 / (2048.0 - freq2);
  double hz3 = (freq3 >= 2048) ? 0.0 : 65536.0 / (2048.0 - freq3);

  double vol1 = static_cast<double>(ch1_.base.volume) / 15.0;
  double vol2 = static_cast<double>(ch2_.volume) / 15.0;
  double vol3 = 0.0;
  switch ((nr32 >> 5) & 0x03) {
    case 0: vol3 = 0.0; break;
    case 1: vol3 = 1.0; break;
    case 2: vol3 = 0.5; break;
    case 3: vol3 = 0.25; break;
    default: break;
  }
  double vol4 = static_cast<double>(ch4_.base.volume) / 15.0;

  bool ch1_on = ch1_.base.enabled && vol1 > 0.0;
  bool ch2_on = ch2_.enabled && vol2 > 0.0;
  bool ch3_on = ch3_.enabled && (nr30 & 0x80) != 0 && vol3 > 0.0;
  bool ch4_on = ch4_.base.enabled && vol4 > 0.0;

  double inc1 = hz1 / static_cast<double>(sample_rate);
  double inc2 = hz2 / static_cast<double>(sample_rate);
  double inc3 = hz3 / static_cast<double>(sample_rate);

  int divisor_code = nr43 & 0x07;
  int shift = (nr43 >> 4) & 0x0F;
  static const int divisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};
  double divisor = static_cast<double>(divisors[divisor_code & 0x07]);
  double noise_hz = 524288.0 / divisor / static_cast<double>(1 << (shift + 1));
  double inc4 = noise_hz / static_cast<double>(sample_rate);

  int duty1 = (nr11 >> 6) & 0x03;
  int duty2 = (nr21 >> 6) & 0x03;

  for (int i = 0; i < count; ++i) {
    double left = 0.0;
    double right = 0.0;

    if (ch1_on && hz1 > 0.0) {
      double s = duty_value(duty1, phase1_) * vol1;
      if (nr51 & 0x10) left += s;
      if (nr51 & 0x01) right += s;
      phase1_ += inc1;
      if (phase1_ >= 1.0) phase1_ -= 1.0;
    }

    if (ch2_on && hz2 > 0.0) {
      double s = duty_value(duty2, phase2_) * vol2;
      if (nr51 & 0x20) left += s;
      if (nr51 & 0x02) right += s;
      phase2_ += inc2;
      if (phase2_ >= 1.0) phase2_ -= 1.0;
    }

    if (ch3_on && hz3 > 0.0) {
      int idx = static_cast<int>(phase3_ * 32.0) & 31;
      std::uint16_t wave_addr = static_cast<std::uint16_t>(0xFF30 + idx / 2);
      std::uint8_t wave_byte = mmu->read_u8(wave_addr);
      std::uint8_t sample4 = (idx % 2 == 0) ? (wave_byte >> 4) : (wave_byte & 0x0F);
      double s = ((static_cast<double>(sample4) / 15.0) * 2.0 - 1.0) * vol3;
      if (nr51 & 0x40) left += s;
      if (nr51 & 0x04) right += s;
      phase3_ += inc3;
      if (phase3_ >= 1.0) phase3_ -= 1.0;
    }

    if (ch4_on && noise_hz > 0.0) {
      phase4_ += inc4;
      if (phase4_ >= 1.0) {
        phase4_ -= 1.0;
        std::uint16_t bit = static_cast<std::uint16_t>((lfsr_ ^ (lfsr_ >> 1)) & 0x01);
        lfsr_ = static_cast<std::uint16_t>((lfsr_ >> 1) | (bit << 14));
        if (nr43 & 0x08) {
          lfsr_ = static_cast<std::uint16_t>((lfsr_ & ~(1u << 6)) | (bit << 6));
        }
      }
      double s = ((lfsr_ & 0x01) ? -1.0 : 1.0) * vol4;
      if (nr51 & 0x80) left += s;
      if (nr51 & 0x08) right += s;
    }

    left = clamp_sample(left * left_vol);
    right = clamp_sample(right * right_vol);

    std::int16_t out_l = static_cast<std::int16_t>(left * 32767.0);
    std::int16_t out_r = static_cast<std::int16_t>(right * 32767.0);
    (*out)[static_cast<std::size_t>(i) * 2] = out_l;
    (*out)[static_cast<std::size_t>(i) * 2 + 1] = out_r;
  }
}

} // namespace gbemu::core
