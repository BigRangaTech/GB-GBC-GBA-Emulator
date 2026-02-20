#include "mmu.h"

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

  vram_.assign(0x2000, 0);
  wram_.assign(0x2000, 0);
  eram_.assign(0x2000, 0);
  oam_.assign(0xA0, 0);
  io_.assign(0x80, 0);
  hram_.assign(0x7F, 0);
  interrupt_enable_ = 0;

  return true;
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
    return vram_[address - 0x8000];
  }
  if (address < 0xC000) {
    return eram_[address - 0xA000];
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
    return io_[address - 0xFF00];
  }
  if (address < 0xFFFF) {
    return hram_[address - 0xFF80];
  }
  return interrupt_enable_;
}

void Mmu::write_u8(std::uint16_t address, std::uint8_t value) {
  if (address < 0x8000) {
    return;
  }
  if (address < 0xA000) {
    vram_[address - 0x8000] = value;
    return;
  }
  if (address < 0xC000) {
    eram_[address - 0xA000] = value;
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
    io_[index] = value;
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

std::uint8_t Mmu::read_rom(std::uint16_t address) const {
  if (rom_.empty()) {
    return 0xFF;
  }
  if (address < rom_.size()) {
    return rom_[address];
  }
  return 0xFF;
}

} // namespace gbemu::core
