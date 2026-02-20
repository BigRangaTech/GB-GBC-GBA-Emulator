#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "system.h"

namespace gbemu::core {

class Mmu {
 public:
  bool load(System system,
            const std::vector<std::uint8_t>& rom,
            const std::vector<std::uint8_t>& boot_rom,
            std::string* error);

  std::uint8_t read_u8(std::uint16_t address) const;
  void write_u8(std::uint16_t address, std::uint8_t value);

  bool boot_rom_enabled() const;

 private:
  std::uint8_t read_rom(std::uint16_t address) const;

  System system_ = System::GB;
  bool boot_rom_enabled_ = false;

  std::vector<std::uint8_t> rom_;
  std::vector<std::uint8_t> boot_rom_;
  std::vector<std::uint8_t> vram_;
  std::vector<std::uint8_t> wram_;
  std::vector<std::uint8_t> eram_;
  std::vector<std::uint8_t> oam_;
  std::vector<std::uint8_t> io_;
  std::vector<std::uint8_t> hram_;
  std::uint8_t interrupt_enable_ = 0;
};

} // namespace gbemu::core
