#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbemu::core {

class GbaBus {
 public:
  bool load(const std::vector<std::uint8_t>& rom,
            const std::vector<std::uint8_t>& bios,
            std::string* error);

  std::uint8_t read8(std::uint32_t address) const;
  std::uint16_t read16(std::uint32_t address) const;
  std::uint32_t read32(std::uint32_t address) const;

  void write8(std::uint32_t address, std::uint8_t value);
  void write16(std::uint32_t address, std::uint16_t value);
  void write32(std::uint32_t address, std::uint32_t value);
  std::uint16_t read_io16(std::uint32_t address) const;
  void write_io16_raw(std::uint32_t address, std::uint16_t value);
  void set_if_bits(std::uint16_t mask);

  const std::vector<std::uint8_t>& rom() const { return rom_; }
  const std::vector<std::uint8_t>& bios() const { return bios_; }

 private:
  std::uint8_t read_mem(const std::vector<std::uint8_t>& mem,
                        std::uint32_t address,
                        std::uint32_t base) const;
  void write_mem(std::vector<std::uint8_t>& mem,
                 std::uint32_t address,
                 std::uint32_t base,
                 std::uint8_t value);

  std::vector<std::uint8_t> bios_;
  std::vector<std::uint8_t> ewram_;
  std::vector<std::uint8_t> iwram_;
  std::vector<std::uint8_t> io_;
  std::vector<std::uint8_t> palette_;
  std::vector<std::uint8_t> vram_;
  std::vector<std::uint8_t> oam_;
  std::vector<std::uint8_t> rom_;
  std::vector<std::uint8_t> sram_;
};

} // namespace gbemu::core
