#include "gba_bus.h"

#include <algorithm>

namespace gbemu::core {

namespace {

constexpr std::uint32_t kBiosBase = 0x00000000;
constexpr std::uint32_t kBiosSize = 0x00004000;
constexpr std::uint32_t kEwramBase = 0x02000000;
constexpr std::uint32_t kEwramSize = 0x00040000;
constexpr std::uint32_t kIwramBase = 0x03000000;
constexpr std::uint32_t kIwramSize = 0x00008000;
constexpr std::uint32_t kIoBase = 0x04000000;
constexpr std::uint32_t kIoSize = 0x00000400;
constexpr std::uint32_t kPaletteBase = 0x05000000;
constexpr std::uint32_t kPaletteSize = 0x00000400;
constexpr std::uint32_t kVramBase = 0x06000000;
constexpr std::uint32_t kVramSize = 0x00018000;
constexpr std::uint32_t kOamBase = 0x07000000;
constexpr std::uint32_t kOamSize = 0x00000400;
constexpr std::uint32_t kRomBase = 0x08000000;
constexpr std::uint32_t kRomEnd = 0x0DFFFFFF;
constexpr std::uint32_t kSramBase = 0x0E000000;
constexpr std::uint32_t kSramSize = 0x00010000;
constexpr std::uint32_t kIfAddr = 0x04000202;

} // namespace

bool GbaBus::load(const std::vector<std::uint8_t>& rom,
                  const std::vector<std::uint8_t>& bios,
                  std::string* error) {
  if (bios.size() != kBiosSize) {
    if (error) {
      *error = "GBA BIOS must be 16 KB";
    }
    return false;
  }
  if (rom.empty()) {
    if (error) {
      *error = "ROM is empty";
    }
    return false;
  }

  bios_ = bios;
  rom_ = rom;
  ewram_.assign(kEwramSize, 0);
  iwram_.assign(kIwramSize, 0);
  io_.assign(kIoSize, 0);
  palette_.assign(kPaletteSize, 0);
  vram_.assign(kVramSize, 0);
  oam_.assign(kOamSize, 0);
  sram_.assign(kSramSize, 0);
  return true;
}

std::uint8_t GbaBus::read_mem(const std::vector<std::uint8_t>& mem,
                              std::uint32_t address,
                              std::uint32_t base) const {
  if (address < base) {
    return 0xFF;
  }
  std::uint32_t offset = address - base;
  if (offset >= mem.size()) {
    return 0xFF;
  }
  return mem[offset];
}

void GbaBus::write_mem(std::vector<std::uint8_t>& mem,
                       std::uint32_t address,
                       std::uint32_t base,
                       std::uint8_t value) {
  if (address < base) {
    return;
  }
  std::uint32_t offset = address - base;
  if (offset >= mem.size()) {
    return;
  }
  mem[offset] = value;
}

std::uint8_t GbaBus::read8(std::uint32_t address) const {
  if (address < kBiosBase + kBiosSize) {
    return read_mem(bios_, address, kBiosBase);
  }
  if (address >= kEwramBase && address < kEwramBase + kEwramSize) {
    return read_mem(ewram_, address, kEwramBase);
  }
  if (address >= kIwramBase && address < kIwramBase + kIwramSize) {
    return read_mem(iwram_, address, kIwramBase);
  }
  if (address >= kIoBase && address < kIoBase + kIoSize) {
    return read_mem(io_, address, kIoBase);
  }
  if (address >= kPaletteBase && address < kPaletteBase + kPaletteSize) {
    return read_mem(palette_, address, kPaletteBase);
  }
  if (address >= kVramBase && address < kVramBase + kVramSize) {
    return read_mem(vram_, address, kVramBase);
  }
  if (address >= kOamBase && address < kOamBase + kOamSize) {
    return read_mem(oam_, address, kOamBase);
  }
  if (address >= kSramBase && address < kSramBase + kSramSize) {
    return read_mem(sram_, address, kSramBase);
  }
  if (address >= kRomBase && address <= kRomEnd) {
    if (rom_.empty()) {
      return 0xFF;
    }
    std::uint32_t offset = address - kRomBase;
    offset %= static_cast<std::uint32_t>(rom_.size());
    return rom_[offset];
  }
  return 0xFF;
}

std::uint16_t GbaBus::read16(std::uint32_t address) const {
  std::uint16_t lo = read8(address);
  std::uint16_t hi = read8(address + 1);
  return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint32_t GbaBus::read32(std::uint32_t address) const {
  std::uint32_t b0 = read8(address);
  std::uint32_t b1 = read8(address + 1);
  std::uint32_t b2 = read8(address + 2);
  std::uint32_t b3 = read8(address + 3);
  return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void GbaBus::write8(std::uint32_t address, std::uint8_t value) {
  if (address >= kEwramBase && address < kEwramBase + kEwramSize) {
    write_mem(ewram_, address, kEwramBase, value);
    return;
  }
  if (address >= kIwramBase && address < kIwramBase + kIwramSize) {
    write_mem(iwram_, address, kIwramBase, value);
    return;
  }
  if (address >= kIoBase && address < kIoBase + kIoSize) {
    if (address == kIfAddr || address == (kIfAddr + 1)) {
      std::uint16_t cur = read_io16(kIfAddr);
      std::uint16_t mask = (address == kIfAddr) ? value
                                                : static_cast<std::uint16_t>(value << 8);
      std::uint16_t next = static_cast<std::uint16_t>(cur & ~mask);
      write_io16_raw(kIfAddr, next);
      return;
    }
    write_mem(io_, address, kIoBase, value);
    return;
  }
  if (address >= kPaletteBase && address < kPaletteBase + kPaletteSize) {
    write_mem(palette_, address, kPaletteBase, value);
    return;
  }
  if (address >= kVramBase && address < kVramBase + kVramSize) {
    write_mem(vram_, address, kVramBase, value);
    return;
  }
  if (address >= kOamBase && address < kOamBase + kOamSize) {
    write_mem(oam_, address, kOamBase, value);
    return;
  }
  if (address >= kSramBase && address < kSramBase + kSramSize) {
    write_mem(sram_, address, kSramBase, value);
    return;
  }
}

void GbaBus::write16(std::uint32_t address, std::uint16_t value) {
  if (address == kIfAddr) {
    std::uint16_t cur = read_io16(kIfAddr);
    std::uint16_t next = static_cast<std::uint16_t>(cur & ~value);
    write_io16_raw(kIfAddr, next);
    return;
  }
  write8(address, static_cast<std::uint8_t>(value & 0xFF));
  write8(address + 1, static_cast<std::uint8_t>(value >> 8));
}

void GbaBus::write32(std::uint32_t address, std::uint32_t value) {
  write8(address, static_cast<std::uint8_t>(value & 0xFF));
  write8(address + 1, static_cast<std::uint8_t>((value >> 8) & 0xFF));
  write8(address + 2, static_cast<std::uint8_t>((value >> 16) & 0xFF));
  write8(address + 3, static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

std::uint16_t GbaBus::read_io16(std::uint32_t address) const {
  if (address < kIoBase || address + 1 >= kIoBase + kIoSize) {
    return 0xFFFF;
  }
  std::uint32_t offset = address - kIoBase;
  std::uint16_t lo = io_[offset];
  std::uint16_t hi = io_[offset + 1];
  return static_cast<std::uint16_t>(lo | (hi << 8));
}

void GbaBus::write_io16_raw(std::uint32_t address, std::uint16_t value) {
  if (address < kIoBase || address + 1 >= kIoBase + kIoSize) {
    return;
  }
  std::uint32_t offset = address - kIoBase;
  io_[offset] = static_cast<std::uint8_t>(value & 0xFF);
  io_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void GbaBus::set_if_bits(std::uint16_t mask) {
  std::uint16_t cur = read_io16(kIfAddr);
  std::uint16_t next = static_cast<std::uint16_t>(cur | mask);
  write_io16_raw(kIfAddr, next);
}

} // namespace gbemu::core
