#include "mmu.h"

#include <algorithm>
#include <ctime>

#include "state_io.h"

namespace gbemu::core {

namespace {

bool is_valid_boot_rom_size(System system, std::size_t size) {
  switch (system) {
    case System::GBA:
      return size == 0x4000;
    case System::GBC:
      return size == 0x900;
    case System::GB:
    default:
      return size == 0x100 || size == 0x900;
  }
}

std::size_t boot_rom_range(System system) {
  switch (system) {
    case System::GBA:
      return 0x4000;
    case System::GBC:
      return 0x900;
    case System::GB:
    default:
      return 0x100;
  }
}

std::array<std::uint8_t, 5> rtc_regs_from_seconds(std::int64_t total_seconds) {
  std::array<std::uint8_t, 5> regs{};
  if (total_seconds < 0) {
    total_seconds = 0;
  }
  std::int64_t seconds = total_seconds % 60;
  std::int64_t minutes = (total_seconds / 60) % 60;
  std::int64_t hours = (total_seconds / 3600) % 24;
  std::int64_t days = total_seconds / 86400;
  bool carry = days >= 512;
  days %= 512;

  regs[0] = static_cast<std::uint8_t>(seconds & 0x3F);
  regs[1] = static_cast<std::uint8_t>(minutes & 0x3F);
  regs[2] = static_cast<std::uint8_t>(hours & 0x1F);
  regs[3] = static_cast<std::uint8_t>(days & 0xFF);
  regs[4] = static_cast<std::uint8_t>((days >> 8) & 0x01);
  if (carry) {
    regs[4] = static_cast<std::uint8_t>(regs[4] | 0x80);
  }
  return regs;
}

std::int64_t seconds_from_rtc_regs(const std::array<std::uint8_t, 5>& regs) {
  std::int64_t seconds = regs[0] & 0x3F;
  std::int64_t minutes = regs[1] & 0x3F;
  std::int64_t hours = regs[2] & 0x1F;
  std::int64_t days = (static_cast<std::int64_t>(regs[3]) | (static_cast<std::int64_t>(regs[4] & 0x01) << 8));
  return days * 86400 + hours * 3600 + minutes * 60 + seconds;
}

} // namespace

bool Mmu::load(System system,
               const std::vector<std::uint8_t>& rom,
               const std::vector<std::uint8_t>& boot_rom,
               std::string* error) {
  system_ = system;

  if (rom.empty()) {
    if (error) {
      *error = "ROM is empty";
    }
    return false;
  }

  if (boot_rom.empty()) {
    if (error) {
      *error = "Boot ROM is required";
    }
    return false;
  }

  if (!is_valid_boot_rom_size(system, boot_rom.size())) {
    if (error) {
      *error = "Boot ROM has unexpected size";
    }
    return false;
  }

  rom_ = rom;
  boot_rom_ = boot_rom;
  boot_rom_enabled_ = true;

  cart_type_ = 0;
  rom_size_code_ = 0;
  ram_size_code_ = 0;
  mbc_type_ = MbcType::None;
  rom_banks_ = 0;
  ram_banks_ = 0;
  ram_enabled_ = false;
  rom_bank_low_ = 1;
  rom_bank_high_ = 0;
  bank_mode_ = 0;
  ram_bank_ = 0;
  vram_bank_ = 0;
  key1_ = 0;
  bg_palette_.fill(0);
  obj_palette_.fill(0);
  bgpi_ = 0;
  bgpi_inc_ = false;
  obpi_ = 0;
  obpi_inc_ = false;
  joypad_state_ = 0xFF;
  rtc_offset_seconds_ = 0;
  rtc_base_time_ = static_cast<std::int64_t>(std::time(nullptr));
  rtc_halt_ = false;
  rtc_latched_ = false;
  rtc_latch_.fill(0);
  rtc_latch_state_ = 0;

  if (rom_.size() >= 0x150) {
    cart_type_ = rom_[0x147];
    rom_size_code_ = rom_[0x148];
    ram_size_code_ = rom_[0x149];

    switch (cart_type_) {
      case 0x00:
        mbc_type_ = MbcType::None;
        break;
      case 0x01:
      case 0x02:
      case 0x03:
        mbc_type_ = MbcType::MBC1;
        break;
      case 0x0F:
      case 0x10:
      case 0x11:
      case 0x12:
      case 0x13:
        mbc_type_ = MbcType::MBC3;
        break;
      default:
        mbc_type_ = MbcType::None;
        break;
    }
  }

  rom_banks_ = rom_banks_from_code(rom_size_code_);
  if (rom_banks_ <= 0) {
    rom_banks_ = static_cast<int>((rom_.size() + 0x3FFF) / 0x4000);
  }
  ram_banks_ = ram_banks_from_code(ram_size_code_);

  vram_.assign(system_ == System::GBC ? 0x4000 : 0x2000, 0);
  wram_.assign(0x2000, 0);
  eram_.assign(static_cast<std::size_t>(ram_banks_ * 0x2000), 0);
  oam_.assign(0xA0, 0);
  io_.assign(0x80, 0);
  hram_.assign(0x7F, 0);
  interrupt_enable_ = 0;
  div_ = 0;
  tima_ = 0;
  tma_ = 0;
  tac_ = 0;
  div_counter_ = 0;
  timer_counter_ = 0;

  return true;
}

void Mmu::step(int cycles) {
  if (cycles <= 0) {
    return;
  }

  div_counter_ += cycles;
  while (div_counter_ >= 256) {
    div_counter_ -= 256;
    div_ = static_cast<std::uint8_t>(div_ + 1);
  }

  if (tac_ & 0x04) {
    int period = 1024;
    switch (tac_ & 0x03) {
      case 0x00: period = 1024; break;
      case 0x01: period = 16; break;
      case 0x02: period = 64; break;
      case 0x03: period = 256; break;
      default: break;
    }

    timer_counter_ += cycles;
    while (timer_counter_ >= period) {
      timer_counter_ -= period;
      if (tima_ == 0xFF) {
        tima_ = tma_;
        request_interrupt(2);
      } else {
        tima_ = static_cast<std::uint8_t>(tima_ + 1);
      }
    }
  }
}

std::uint8_t Mmu::read_u8(std::uint16_t address) const {
  if (boot_rom_enabled_) {
    std::size_t limit = boot_rom_range(system_);
    if (address < limit && address < boot_rom_.size()) {
      return boot_rom_[address];
    }
  }

  if (address < 0x8000) {
    return read_rom(address);
  }
  if (address < 0xA000) {
    return vram_[static_cast<std::size_t>(vram_bank_) * 0x2000 + (address - 0x8000)];
  }
  if (address < 0xC000) {
    if (mbc_type_ == MbcType::MBC3 && ram_bank_ >= 0x08 && ram_bank_ <= 0x0C) {
      std::array<std::uint8_t, 5> regs = rtc_latched_ ? rtc_latch_ : rtc_regs_from_seconds(
          rtc_halt_ ? rtc_offset_seconds_
                    : rtc_offset_seconds_ + static_cast<std::int64_t>(std::time(nullptr)) - rtc_base_time_);
      return regs[ram_bank_ - 0x08];
    }
    if (!ram_enabled_ || eram_.empty()) {
      return 0xFF;
    }
    std::size_t bank = static_cast<std::size_t>(effective_ram_bank());
    std::size_t offset = bank * 0x2000 + (address - 0xA000);
    if (offset < eram_.size()) {
      return eram_[offset];
    }
    return 0xFF;
  }
  if (address < 0xE000) {
    return wram_[address - 0xC000];
  }
  if (address < 0xFE00) {
    return wram_[address - 0xE000];
  }
  if (address < 0xFEA0) {
    return oam_[address - 0xFE00];
  }
  if (address < 0xFF00) {
    return 0xFF;
  }
  if (address < 0xFF80) {
    switch (address) {
      case 0xFF04: return div_;
      case 0xFF05: return tima_;
      case 0xFF06: return tma_;
      case 0xFF07: return tac_ | 0xF8;
      case 0xFF4D:
        if (system_ == System::GBC) {
          return static_cast<std::uint8_t>(0x7E | (key1_ & 0x81));
        }
        return 0xFF;
      case 0xFF4F:
        return static_cast<std::uint8_t>(0xFE | (vram_bank_ & 0x01));
      case 0xFF00: {
        std::uint8_t select = io_[0x00] & 0x30;
        std::uint8_t upper = static_cast<std::uint8_t>(0xC0 | select);
        std::uint8_t lower = 0x0F;
        if ((select & 0x10) == 0) {
          lower &= static_cast<std::uint8_t>(joypad_state_ & 0x0F);
        }
        if ((select & 0x20) == 0) {
          lower &= static_cast<std::uint8_t>((joypad_state_ >> 4) & 0x0F);
        }
        return static_cast<std::uint8_t>(upper | lower);
      }
      case 0xFF68:
        return static_cast<std::uint8_t>((bgpi_ & 0x3F) | (bgpi_inc_ ? 0x80 : 0x00));
      case 0xFF69:
        return bg_palette_[bgpi_ & 0x3F];
      case 0xFF6A:
        return static_cast<std::uint8_t>((obpi_ & 0x3F) | (obpi_inc_ ? 0x80 : 0x00));
      case 0xFF6B:
        return obj_palette_[obpi_ & 0x3F];
      case 0xFF0F: return io_[0x0F];
      default: return io_[address - 0xFF00];
    }
  }
  if (address < 0xFFFF) {
    return hram_[address - 0xFF80];
  }
  return interrupt_enable_;
}

void Mmu::write_u8(std::uint16_t address, std::uint8_t value) {
  if (address < 0x8000) {
    if (mbc_type_ == MbcType::MBC1) {
      if (address < 0x2000) {
        ram_enabled_ = ((value & 0x0F) == 0x0A);
      } else if (address < 0x4000) {
        rom_bank_low_ = static_cast<std::uint8_t>(value & 0x1F);
        if (rom_bank_low_ == 0) {
          rom_bank_low_ = 1;
        }
      } else if (address < 0x6000) {
        rom_bank_high_ = static_cast<std::uint8_t>(value & 0x03);
        ram_bank_ = static_cast<std::uint8_t>(value & 0x03);
      } else {
        bank_mode_ = static_cast<std::uint8_t>(value & 0x01);
      }
      return;
    }

    if (mbc_type_ == MbcType::MBC3) {
      if (address < 0x2000) {
        ram_enabled_ = ((value & 0x0F) == 0x0A);
      } else if (address < 0x4000) {
        std::uint8_t bank = static_cast<std::uint8_t>(value & 0x7F);
        if (bank == 0) {
          bank = 1;
        }
        rom_bank_low_ = bank;
      } else if (address < 0x6000) {
        ram_bank_ = static_cast<std::uint8_t>(value);
      } else {
        if (rtc_latch_state_ == 0 && value == 1) {
          rtc_latch_ = rtc_regs_from_seconds(
              rtc_halt_ ? rtc_offset_seconds_
                        : rtc_offset_seconds_ + static_cast<std::int64_t>(std::time(nullptr)) - rtc_base_time_);
          rtc_latched_ = true;
        }
        if (value == 0) {
          rtc_latched_ = false;
        }
        rtc_latch_state_ = value;
      }
      return;
    }

    return;
  }
  if (address < 0xA000) {
    vram_[static_cast<std::size_t>(vram_bank_) * 0x2000 + (address - 0x8000)] = value;
    return;
  }
  if (address < 0xC000) {
    if (mbc_type_ == MbcType::MBC3 && ram_bank_ >= 0x08 && ram_bank_ <= 0x0C) {
      std::array<std::uint8_t, 5> regs = rtc_latched_ ? rtc_latch_ : rtc_regs_from_seconds(
          rtc_halt_ ? rtc_offset_seconds_
                    : rtc_offset_seconds_ + static_cast<std::int64_t>(std::time(nullptr)) - rtc_base_time_);
      regs[ram_bank_ - 0x08] = value;
      rtc_halt_ = (regs[4] & 0x40) != 0;
      rtc_offset_seconds_ = seconds_from_rtc_regs(regs);
      rtc_base_time_ = static_cast<std::int64_t>(std::time(nullptr));
      rtc_latch_ = regs;
      return;
    }
    if (!ram_enabled_ || eram_.empty()) {
      return;
    }
    std::size_t bank = static_cast<std::size_t>(effective_ram_bank());
    std::size_t offset = bank * 0x2000 + (address - 0xA000);
    if (offset < eram_.size()) {
      eram_[offset] = value;
    }
    return;
  }
  if (address < 0xE000) {
    wram_[address - 0xC000] = value;
    return;
  }
  if (address < 0xFE00) {
    wram_[address - 0xE000] = value;
    return;
  }
  if (address < 0xFEA0) {
    oam_[address - 0xFE00] = value;
    return;
  }
  if (address < 0xFF00) {
    return;
  }
  if (address < 0xFF80) {
    std::uint16_t index = address - 0xFF00;
    switch (address) {
      case 0xFF04:
        div_ = 0;
        div_counter_ = 0;
        break;
      case 0xFF05:
        tima_ = value;
        break;
      case 0xFF06:
        tma_ = value;
        break;
      case 0xFF07:
        tac_ = static_cast<std::uint8_t>(value & 0x07);
        break;
      case 0xFF4D:
        if (system_ == System::GBC) {
          key1_ = static_cast<std::uint8_t>((key1_ & 0x80) | (value & 0x01));
        }
        break;
      case 0xFF4F:
        if (system_ == System::GBC) {
          vram_bank_ = static_cast<std::uint8_t>(value & 0x01);
        }
        break;
      case 0xFF00:
        io_[0x00] = static_cast<std::uint8_t>(value & 0x30);
        break;
      case 0xFF68:
        bgpi_ = static_cast<std::uint8_t>(value & 0x3F);
        bgpi_inc_ = (value & 0x80) != 0;
        break;
      case 0xFF69:
        bg_palette_[bgpi_ & 0x3F] = value;
        if (bgpi_inc_) {
          bgpi_ = static_cast<std::uint8_t>((bgpi_ + 1) & 0x3F);
        }
        break;
      case 0xFF6A:
        obpi_ = static_cast<std::uint8_t>(value & 0x3F);
        obpi_inc_ = (value & 0x80) != 0;
        break;
      case 0xFF6B:
        obj_palette_[obpi_ & 0x3F] = value;
        if (obpi_inc_) {
          obpi_ = static_cast<std::uint8_t>((obpi_ + 1) & 0x3F);
        }
        break;
      case 0xFF41:
        io_[0x41] = static_cast<std::uint8_t>((io_[0x41] & 0x07) | (value & 0xF8));
        break;
      case 0xFF44:
        io_[0x44] = 0;
        break;
      case 0xFF0F:
        io_[0x0F] = value;
        break;
      default:
        io_[index] = value;
        break;
    }
    if (address == 0xFF50 && value != 0) {
      boot_rom_enabled_ = false;
    }
    return;
  }
  if (address < 0xFFFF) {
    hram_[address - 0xFF80] = value;
    return;
  }
  interrupt_enable_ = value;
}

bool Mmu::boot_rom_enabled() const {
  return boot_rom_enabled_;
}

std::uint8_t Mmu::interrupt_enable() const {
  return interrupt_enable_;
}

std::uint8_t Mmu::interrupt_flags() const {
  return io_[0x0F];
}

void Mmu::set_interrupt_flags(std::uint8_t value) {
  io_[0x0F] = value;
}

void Mmu::request_interrupt(std::uint8_t bit) {
  io_[0x0F] = static_cast<std::uint8_t>(io_[0x0F] | (1u << bit));
}

void Mmu::set_ly(std::uint8_t value) {
  io_[0x44] = value;
}

void Mmu::set_stat(std::uint8_t value) {
  io_[0x41] = value;
}

std::uint8_t Mmu::read_vram(std::uint16_t address, int bank) const {
  if (address < 0x8000 || address >= 0xA000) {
    return 0xFF;
  }
  int selected = bank & 0x01;
  if (system_ != System::GBC) {
    selected = 0;
  }
  std::size_t offset = static_cast<std::size_t>(selected) * 0x2000 + (address - 0x8000);
  if (offset >= vram_.size()) {
    return 0xFF;
  }
  return vram_[offset];
}

std::uint8_t Mmu::bg_palette_byte(int index) const {
  return bg_palette_[static_cast<std::size_t>(index) & 0x3F];
}

std::uint8_t Mmu::obj_palette_byte(int index) const {
  return obj_palette_[static_cast<std::size_t>(index) & 0x3F];
}

void Mmu::set_joypad_state(std::uint8_t state) {
  joypad_state_ = state;
}

bool Mmu::handle_stop() {
  if (system_ == System::GBC && (key1_ & 0x01)) {
    key1_ = static_cast<std::uint8_t>((key1_ ^ 0x80) & 0x80);
    return true;
  }
  return false;
}

void Mmu::serialize(std::vector<std::uint8_t>* out) const {
  if (!out) {
    return;
  }
  state_io::write_u8(*out, static_cast<std::uint8_t>(system_));
  state_io::write_bool(*out, boot_rom_enabled_);
  state_io::write_u8(*out, cart_type_);
  state_io::write_u8(*out, rom_size_code_);
  state_io::write_u8(*out, ram_size_code_);
  state_io::write_u32(*out, static_cast<std::uint32_t>(rom_banks_));
  state_io::write_u32(*out, static_cast<std::uint32_t>(ram_banks_));
  state_io::write_bool(*out, ram_enabled_);
  state_io::write_u8(*out, rom_bank_low_);
  state_io::write_u8(*out, rom_bank_high_);
  state_io::write_u8(*out, bank_mode_);
  state_io::write_u8(*out, ram_bank_);
  state_io::write_u8(*out, vram_bank_);
  state_io::write_u8(*out, key1_);
  state_io::write_u8(*out, interrupt_enable_);

  state_io::write_u8(*out, div_);
  state_io::write_u8(*out, tima_);
  state_io::write_u8(*out, tma_);
  state_io::write_u8(*out, tac_);
  state_io::write_u32(*out, static_cast<std::uint32_t>(div_counter_));
  state_io::write_u32(*out, static_cast<std::uint32_t>(timer_counter_));

  state_io::write_u8(*out, joypad_state_);

  state_io::write_i64(*out, rtc_offset_seconds_);
  state_io::write_i64(*out, rtc_base_time_);
  state_io::write_bool(*out, rtc_halt_);
  state_io::write_bool(*out, rtc_latched_);
  state_io::write_u8(*out, rtc_latch_state_);
  state_io::write_bytes_fixed(*out, rtc_latch_.data(), rtc_latch_.size());

  state_io::write_u8(*out, bgpi_);
  state_io::write_bool(*out, bgpi_inc_);
  state_io::write_u8(*out, obpi_);
  state_io::write_bool(*out, obpi_inc_);
  state_io::write_bytes_fixed(*out, bg_palette_.data(), bg_palette_.size());
  state_io::write_bytes_fixed(*out, obj_palette_.data(), obj_palette_.size());

  state_io::write_bytes(*out, vram_);
  state_io::write_bytes(*out, wram_);
  state_io::write_bytes(*out, eram_);
  state_io::write_bytes(*out, oam_);
  state_io::write_bytes(*out, io_);
  state_io::write_bytes(*out, hram_);
}

bool Mmu::deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error) {
  std::uint8_t sys = 0;
  if (!state_io::read_u8(data, offset, sys)) return false;
  if (sys != static_cast<std::uint8_t>(system_)) {
    if (error) *error = "State system mismatch";
    return false;
  }
  if (!state_io::read_bool(data, offset, boot_rom_enabled_)) return false;
  if (!state_io::read_u8(data, offset, cart_type_)) return false;
  if (!state_io::read_u8(data, offset, rom_size_code_)) return false;
  if (!state_io::read_u8(data, offset, ram_size_code_)) return false;
  std::uint32_t v32 = 0;
  if (!state_io::read_u32(data, offset, v32)) return false;
  rom_banks_ = static_cast<int>(v32);
  if (!state_io::read_u32(data, offset, v32)) return false;
  ram_banks_ = static_cast<int>(v32);
  if (!state_io::read_bool(data, offset, ram_enabled_)) return false;
  if (!state_io::read_u8(data, offset, rom_bank_low_)) return false;
  if (!state_io::read_u8(data, offset, rom_bank_high_)) return false;
  if (!state_io::read_u8(data, offset, bank_mode_)) return false;
  if (!state_io::read_u8(data, offset, ram_bank_)) return false;
  if (!state_io::read_u8(data, offset, vram_bank_)) return false;
  if (!state_io::read_u8(data, offset, key1_)) return false;
  if (!state_io::read_u8(data, offset, interrupt_enable_)) return false;

  if (!state_io::read_u8(data, offset, div_)) return false;
  if (!state_io::read_u8(data, offset, tima_)) return false;
  if (!state_io::read_u8(data, offset, tma_)) return false;
  if (!state_io::read_u8(data, offset, tac_)) return false;
  if (!state_io::read_u32(data, offset, v32)) return false;
  div_counter_ = static_cast<int>(v32);
  if (!state_io::read_u32(data, offset, v32)) return false;
  timer_counter_ = static_cast<int>(v32);

  if (!state_io::read_u8(data, offset, joypad_state_)) return false;

  if (!state_io::read_i64(data, offset, rtc_offset_seconds_)) return false;
  if (!state_io::read_i64(data, offset, rtc_base_time_)) return false;
  if (!state_io::read_bool(data, offset, rtc_halt_)) return false;
  if (!state_io::read_bool(data, offset, rtc_latched_)) return false;
  if (!state_io::read_u8(data, offset, rtc_latch_state_)) return false;
  if (!state_io::read_bytes_fixed(data, offset, rtc_latch_.data(), rtc_latch_.size())) return false;

  if (!state_io::read_u8(data, offset, bgpi_)) return false;
  if (!state_io::read_bool(data, offset, bgpi_inc_)) return false;
  if (!state_io::read_u8(data, offset, obpi_)) return false;
  if (!state_io::read_bool(data, offset, obpi_inc_)) return false;
  if (!state_io::read_bytes_fixed(data, offset, bg_palette_.data(), bg_palette_.size())) return false;
  if (!state_io::read_bytes_fixed(data, offset, obj_palette_.data(), obj_palette_.size())) return false;

  if (!state_io::read_bytes(data, offset, vram_)) return false;
  if (!state_io::read_bytes(data, offset, wram_)) return false;
  if (!state_io::read_bytes(data, offset, eram_)) return false;
  if (!state_io::read_bytes(data, offset, oam_)) return false;
  if (!state_io::read_bytes(data, offset, io_)) return false;
  if (!state_io::read_bytes(data, offset, hram_)) return false;
  return true;
}

bool Mmu::has_battery() const {
  switch (cart_type_) {
    case 0x03:
    case 0x06:
    case 0x09:
    case 0x0F:
    case 0x10:
    case 0x13:
    case 0x1B:
    case 0x1E:
      return true;
    default:
      return false;
  }
}

bool Mmu::has_ram() const {
  return ram_banks_ > 0;
}

bool Mmu::has_rtc() const {
  return cart_type_ == 0x0F || cart_type_ == 0x10;
}

std::vector<std::uint8_t> Mmu::ram_data() const {
  return eram_;
}

void Mmu::load_ram_data(const std::vector<std::uint8_t>& data) {
  if (data.empty() || eram_.empty()) {
    return;
  }
  std::size_t size = std::min(data.size(), eram_.size());
  std::copy_n(data.begin(), size, eram_.begin());
}

std::vector<std::uint8_t> Mmu::rtc_data() const {
  std::vector<std::uint8_t> out(21, 0);
  std::int64_t offset = rtc_halt_ ? rtc_offset_seconds_
                                  : rtc_offset_seconds_ + static_cast<std::int64_t>(std::time(nullptr)) - rtc_base_time_;
  std::int64_t base = rtc_base_time_;
  std::uint8_t flags = 0;
  if (rtc_halt_) flags |= 0x01;
  if (rtc_latched_) flags |= 0x02;
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((offset >> (i * 8)) & 0xFF);
  }
  for (int i = 0; i < 8; ++i) {
    out[8 + i] = static_cast<std::uint8_t>((base >> (i * 8)) & 0xFF);
  }
  out[16] = flags;
  out[17] = rtc_latch_[0];
  out[18] = rtc_latch_[1];
  out[19] = rtc_latch_[2];
  out[20] = rtc_latch_[3];
  return out;
}

void Mmu::load_rtc_data(const std::vector<std::uint8_t>& data) {
  if (data.size() < 21) {
    return;
  }
  std::int64_t offset = 0;
  std::int64_t base = 0;
  for (int i = 0; i < 8; ++i) {
    offset |= (static_cast<std::int64_t>(data[i]) << (i * 8));
  }
  for (int i = 0; i < 8; ++i) {
    base |= (static_cast<std::int64_t>(data[8 + i]) << (i * 8));
  }
  rtc_offset_seconds_ = offset;
  rtc_base_time_ = base;
  rtc_halt_ = (data[16] & 0x01) != 0;
  rtc_latch_[0] = data[17];
  rtc_latch_[1] = data[18];
  rtc_latch_[2] = data[19];
  rtc_latch_[3] = data[20];
  rtc_latch_[4] = static_cast<std::uint8_t>((data[16] & 0xFE));
  rtc_latched_ = (data[16] & 0x02) != 0;
}

std::uint8_t Mmu::read_rom(std::uint16_t address) const {
  if (rom_.empty()) {
    return 0xFF;
  }
  if (mbc_type_ == MbcType::None) {
    if (address < rom_.size()) {
      return rom_[address];
    }
    return 0xFF;
  }

  if (address < 0x4000) {
    int bank = 0;
    if (mbc_type_ == MbcType::MBC1 && bank_mode_ == 1) {
      bank = (rom_bank_high_ << 5);
    }
    if (rom_banks_ > 0) {
      bank %= rom_banks_;
    }
    std::size_t offset = static_cast<std::size_t>(bank) * 0x4000 + address;
    if (offset < rom_.size()) {
      return rom_[offset];
    }
    return 0xFF;
  }

  int bank = effective_rom_bank();
  std::size_t offset = static_cast<std::size_t>(bank) * 0x4000 + (address - 0x4000);
  if (offset < rom_.size()) {
    return rom_[offset];
  }
  return 0xFF;
}

int Mmu::rom_banks_from_code(std::uint8_t code) const {
  switch (code) {
    case 0x00: return 2;
    case 0x01: return 4;
    case 0x02: return 8;
    case 0x03: return 16;
    case 0x04: return 32;
    case 0x05: return 64;
    case 0x06: return 128;
    case 0x07: return 256;
    case 0x08: return 512;
    case 0x52: return 72;
    case 0x53: return 80;
    case 0x54: return 96;
    default: return 0;
  }
}

int Mmu::ram_banks_from_code(std::uint8_t code) const {
  switch (code) {
    case 0x00: return 0;
    case 0x01: return 1;
    case 0x02: return 1;
    case 0x03: return 4;
    case 0x04: return 16;
    case 0x05: return 8;
    default: return 0;
  }
}

int Mmu::effective_rom_bank() const {
  int bank = 1;
  if (mbc_type_ == MbcType::MBC1) {
    int combined = (rom_bank_high_ << 5) | (rom_bank_low_ & 0x1F);
    if (combined == 0) {
      combined = 1;
    }
    bank = combined;
  } else if (mbc_type_ == MbcType::MBC3) {
    bank = rom_bank_low_;
    if (bank == 0) {
      bank = 1;
    }
  }
  if (rom_banks_ > 0) {
    bank %= rom_banks_;
    if (bank == 0) {
      bank = 1;
    }
  }
  return bank;
}

int Mmu::effective_ram_bank() const {
  if (ram_banks_ <= 1) {
    return 0;
  }
  if (mbc_type_ == MbcType::MBC1) {
    if (bank_mode_ == 0) {
      return 0;
    }
    return static_cast<int>(ram_bank_ & 0x03);
  }
  if (mbc_type_ == MbcType::MBC3) {
    if (ram_bank_ <= 0x03) {
      return static_cast<int>(ram_bank_ & 0x03);
    }
    return 0;
  }
  return 0;
}

} // namespace gbemu::core
