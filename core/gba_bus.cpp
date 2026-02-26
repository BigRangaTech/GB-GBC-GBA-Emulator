#include "gba_bus.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

namespace gbemu::core {

namespace {

constexpr std::uint32_t kBiosBase = 0x00000000;
constexpr std::uint32_t kBiosSize = 0x00004000;
constexpr std::uint32_t kEwramBase = 0x02000000;
constexpr std::uint32_t kEwramSize = 0x00040000;
constexpr std::uint32_t kEwramMirrorSize = 0x01000000;
constexpr std::uint32_t kIwramBase = 0x03000000;
constexpr std::uint32_t kIwramSize = 0x00008000;
constexpr std::uint32_t kIwramMirrorSize = 0x01000000;
constexpr std::uint32_t kIoBase = 0x04000000;
constexpr std::uint32_t kIoSize = 0x00000400;
constexpr std::uint32_t kPaletteBase = 0x05000000;
constexpr std::uint32_t kPaletteSize = 0x00000400;
constexpr std::uint32_t kPaletteMirrorSize = 0x00000400;
constexpr std::uint32_t kVramBase = 0x06000000;
constexpr std::uint32_t kVramSize = 0x00018000;
constexpr std::uint32_t kVramMirrorSize = 0x00020000;
constexpr std::uint32_t kOamBase = 0x07000000;
constexpr std::uint32_t kOamSize = 0x00000400;
constexpr std::uint32_t kRomBase = 0x08000000;
constexpr std::uint32_t kRomEnd = 0x0DFFFFFF;
constexpr std::uint32_t kSramBase = 0x0E000000;
constexpr std::uint32_t kSramSize = 0x00010000;
constexpr std::uint32_t kIfAddr = 0x04000202;
constexpr std::uint32_t kRegDispcnt = 0x04000000;
constexpr std::uint32_t kRegDispstat = 0x04000004;
constexpr std::uint32_t kRegVcount = 0x04000006;
constexpr std::uint32_t kRegKeyinput = 0x04000130;
constexpr std::uint32_t kRegIe = 0x04000200;
constexpr std::uint32_t kRegIme = 0x04000208;
constexpr std::uint32_t kRegPostflg = 0x04000300;
constexpr std::uint32_t kRegHaltcnt = 0x04000301;

bool palette_offset_for(std::uint32_t address, std::uint32_t* offset) {
  if (!offset || address < kPaletteBase || address >= kPaletteBase + kPaletteMirrorSize) {
    return false;
  }
  *offset = (address - kPaletteBase) & (kPaletteSize - 1u);
  return true;
}

bool vram_offset_for(std::uint32_t address, std::uint32_t* offset) {
  if (!offset || address < kVramBase || address >= kVramBase + kVramMirrorSize) {
    return false;
  }
  std::uint32_t off = address - kVramBase;
  if (off >= kVramSize) {
    // 0x06018000-0x0601FFFF mirrors 0x06010000-0x06017FFF.
    off = 0x00010000u + (off & 0x00007FFFu);
  }
  *offset = off;
  return true;
}

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
  rom_mask_ = 0;
  rom_size_pow2_ = false;
  if (!rom_.empty()) {
    std::uint32_t pow2 = 1;
    std::uint32_t size = static_cast<std::uint32_t>(rom_.size());
    rom_size_pow2_ = (size & (size - 1u)) == 0;
    while (pow2 < size && pow2 < 0x02000000u) {
      pow2 <<= 1;
    }
    if (pow2 > 0x02000000u) {
      pow2 = 0x02000000u;
    }
    rom_mask_ = pow2 - 1u;
  }
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

void GbaBus::set_trace_io_limit(int limit) {
  trace_io_limit_ = limit;
  trace_io_ = trace_io_limit_ > 0;
}

void GbaBus::set_watch_video_io_limit(int limit) {
  if (limit < 0) {
    limit = 0;
  }
  watch_video_io_limit_ = limit;
  watch_video_io_count_ = 0;
}

void GbaBus::set_watch_io_read_limit(int limit) {
  if (limit < 0) {
    limit = 0;
  }
  watch_io_read_limit_ = limit;
  watch_io_read_count_ = 0;
  watch_io_read_enabled_ = watch_io_read_limit_ > 0;
}

void GbaBus::clear_watchpoints() {
  watchpoints_.clear();
  watchpoint_count_ = 0;
}

void GbaBus::add_watchpoint(std::uint32_t start,
                            std::uint32_t end,
                            bool read,
                            bool write) {
  if (start > end) {
    std::swap(start, end);
  }
  Watchpoint watch;
  watch.start = start;
  watch.end = end;
  watch.read = read;
  watch.write = write;
  watchpoints_.push_back(watch);
}

void GbaBus::set_watchpoint_limit(int limit) {
  if (limit < 0) {
    limit = 0;
  }
  watchpoint_limit_ = limit;
  watchpoint_count_ = 0;
}

void GbaBus::set_last_pc(std::uint32_t pc) {
  last_pc_ = pc;
}

bool GbaBus::take_postflg_write(std::uint32_t* pc, std::uint8_t* value) {
  if (!postflg_pending_) {
    return false;
  }
  postflg_pending_ = false;
  if (pc) {
    *pc = postflg_pc_;
  }
  if (value) {
    *value = postflg_value_;
  }
  return true;
}

bool GbaBus::take_halt_request(bool* stop) {
  if (!halt_requested_) {
    if (stop) {
      *stop = false;
    }
    return false;
  }
  halt_requested_ = false;
  if (stop) {
    *stop = stop_requested_;
  }
  stop_requested_ = false;
  return true;
}

void GbaBus::log_io_write(std::uint32_t address, std::uint32_t value, int bits) {
  if (!trace_io_ || trace_io_limit_ <= 0) {
    return;
  }
  if (address < kIoBase || address >= kIoBase + kIoSize) {
    return;
  }
  int width = bits / 4;
  std::uint32_t mask = 0xFFFFFFFFu;
  if (bits < 32) {
    mask = (1u << bits) - 1u;
  }
  std::cout << "GBA IO W" << bits << " 0x" << std::hex << std::setw(8) << std::setfill('0')
            << address << " = 0x" << std::setw(width) << (value & mask)
            << std::dec << "\n";
  --trace_io_limit_;
  if (trace_io_limit_ <= 0) {
    trace_io_ = false;
  }
}

void GbaBus::log_video_io_write(std::uint32_t address, std::uint32_t value, int bits) {
  if (watch_video_io_limit_ <= 0 || watch_video_io_count_ >= watch_video_io_limit_) {
    return;
  }
  if (address < kIoBase || address >= kIoBase + kIoSize) {
    return;
  }
  if (address >= 0x04000000u && address <= 0x04000060u) {
    int width = bits / 4;
    std::uint32_t mask = 0xFFFFFFFFu;
    if (bits < 32) {
      mask = (1u << bits) - 1u;
    }
    std::cout << "GBA VID IO W" << bits << " 0x" << std::hex << std::setw(8)
              << std::setfill('0') << address << " = 0x" << std::setw(width)
              << (value & mask) << " PC=0x" << std::setw(8) << last_pc_ << std::dec
              << "\n";
    ++watch_video_io_count_;
  }
}

void GbaBus::log_io_read(std::uint32_t address, std::uint32_t value, int bits) const {
  if (!watch_io_read_enabled_ || watch_io_read_limit_ <= 0 ||
      watch_io_read_count_ >= watch_io_read_limit_) {
    return;
  }
  if (address < kIoBase || address >= kIoBase + kIoSize) {
    return;
  }
  std::uint32_t base = address;
  if (bits == 8 || bits == 16) {
    base &= ~1u;
  } else if (bits == 32) {
    base &= ~3u;
  }
  switch (base) {
    case kRegDispcnt:
    case kRegDispstat:
    case kRegVcount:
    case kRegKeyinput:
    case kRegIe:
    case kIfAddr:
    case kRegIme:
    case kRegPostflg:
    case kRegHaltcnt:
      break;
    default:
      return;
  }
  int width = bits / 4;
  std::uint32_t mask = 0xFFFFFFFFu;
  if (bits < 32) {
    mask = (1u << bits) - 1u;
  }
  std::cout << "GBA IO R" << bits << " 0x" << std::hex << std::setw(8)
            << std::setfill('0') << base << " = 0x" << std::setw(width)
            << (value & mask) << " PC=0x" << std::setw(8) << last_pc_
            << std::dec << "\n";
  ++watch_io_read_count_;
}

void GbaBus::log_watchpoint(std::uint32_t address,
                            std::uint32_t value,
                            int bits,
                            bool write) const {
  if (watchpoints_.empty() || watchpoint_limit_ == 0) {
    return;
  }
  if (watchpoint_limit_ > 0 && watchpoint_count_ >= watchpoint_limit_) {
    return;
  }
  for (const auto& watch : watchpoints_) {
    if (address < watch.start || address > watch.end) {
      continue;
    }
    if (write && !watch.write) {
      continue;
    }
    if (!write && !watch.read) {
      continue;
    }
    int width = bits / 4;
    std::uint32_t mask = 0xFFFFFFFFu;
    if (bits < 32) {
      mask = (1u << bits) - 1u;
    }
    std::cout << "GBA WATCH " << (write ? "W" : "R") << bits << " 0x"
              << std::hex << std::setw(8) << std::setfill('0') << address
              << " = 0x" << std::setw(width) << (value & mask)
              << " PC=0x" << std::setw(8) << last_pc_ << std::dec << "\n";
    ++watchpoint_count_;
    break;
  }
}

void GbaBus::write8_internal(std::uint32_t address,
                             std::uint8_t value,
                             bool allow_trace,
                             bool byte_access) {
  if (allow_trace) {
    log_io_write(address, value, 8);
  }
  if (address == 0x04000300u) {
    postflg_pending_ = true;
    postflg_pc_ = last_pc_;
    postflg_value_ = value;
  }
  if (address == 0x04000301u) {
    halt_requested_ = true;
    stop_requested_ = (value & 0x80u) != 0;
  }
  if (address >= kEwramBase && address < kEwramBase + kEwramMirrorSize) {
    std::uint32_t offset = (address - kEwramBase) & (kEwramSize - 1u);
    ewram_[offset] = value;
    return;
  }
  if (address >= kIwramBase && address < kIwramBase + kIwramMirrorSize) {
    std::uint32_t offset = (address - kIwramBase) & (kIwramSize - 1u);
    iwram_[offset] = value;
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
  if (address >= kPaletteBase && address < kPaletteBase + kPaletteMirrorSize) {
    std::uint32_t offset = 0;
    if (!palette_offset_for(address, &offset)) {
      return;
    }
    if (byte_access) {
      // Palette RAM is 16-bit bus only on GBA; byte writes mirror to the full halfword.
      std::uint32_t aligned = offset & ~1u;
      if (aligned + 1 < palette_.size()) {
        palette_[aligned] = value;
        palette_[aligned + 1] = value;
      }
    } else {
      if (offset < palette_.size()) {
        palette_[offset] = value;
      }
    }
    return;
  }
  if (address >= kVramBase && address < kVramBase + kVramMirrorSize) {
    std::uint32_t offset = 0;
    if (!vram_offset_for(address, &offset)) {
      return;
    }
    if (byte_access) {
      // VRAM is also effectively halfword-addressable for byte writes.
      std::uint32_t aligned = offset & ~1u;
      if (aligned + 1 < vram_.size()) {
        vram_[aligned] = value;
        vram_[aligned + 1] = value;
      }
    } else {
      if (offset < vram_.size()) {
        vram_[offset] = value;
      }
    }
    return;
  }
  if (address >= kOamBase && address < kOamBase + kOamSize) {
    if (!byte_access) {
      write_mem(oam_, address, kOamBase, value);
    }
    // Byte writes to OAM are ignored on real hardware.
    return;
  }
  if (address >= kSramBase && address < kSramBase + kSramSize) {
    write_mem(sram_, address, kSramBase, value);
    return;
  }
}

std::uint8_t GbaBus::read8_internal(std::uint32_t address, bool allow_log) const {
  std::uint8_t value = 0xFF;
  if (bios_enabled_ && address < kBiosBase + kBiosSize) {
    value = read_mem(bios_, address, kBiosBase);
  } else if (address >= kEwramBase && address < kEwramBase + kEwramMirrorSize) {
    std::uint32_t offset = (address - kEwramBase) & (kEwramSize - 1u);
    value = ewram_[offset];
  } else if (address >= kIwramBase && address < kIwramBase + kIwramMirrorSize) {
    std::uint32_t offset = (address - kIwramBase) & (kIwramSize - 1u);
    value = iwram_[offset];
  } else if (address >= kIoBase && address < kIoBase + kIoSize) {
    value = read_mem(io_, address, kIoBase);
  } else if (address >= kPaletteBase && address < kPaletteBase + kPaletteMirrorSize) {
    std::uint32_t offset = 0;
    if (palette_offset_for(address, &offset) && offset < palette_.size()) {
      value = palette_[offset];
    }
  } else if (address >= kVramBase && address < kVramBase + kVramMirrorSize) {
    std::uint32_t offset = 0;
    if (vram_offset_for(address, &offset) && offset < vram_.size()) {
      value = vram_[offset];
    }
  } else if (address >= kOamBase && address < kOamBase + kOamSize) {
    value = read_mem(oam_, address, kOamBase);
  } else if (address >= kSramBase && address < kSramBase + kSramSize) {
    value = read_mem(sram_, address, kSramBase);
  } else if (address >= kRomBase && address <= kRomEnd) {
    if (rom_.empty()) {
      value = 0xFF;
    } else {
      std::uint32_t offset = (address - kRomBase) & 0x01FFFFFFu;
      if (rom_mask_ != 0) {
        offset &= rom_mask_;
      }
      if (offset >= rom_.size()) {
        if (!rom_size_pow2_ && !rom_.empty()) {
          offset %= static_cast<std::uint32_t>(rom_.size());
        } else {
          value = 0xFF;
          if (allow_log) {
            log_io_read(address, value, 8);
          }
          return value;
        }
      }
      value = rom_[offset];
    }
  }
  if (allow_log) {
    log_io_read(address, value, 8);
  }
  return value;
}

std::uint8_t GbaBus::read8(std::uint32_t address) const {
  std::uint8_t value = read8_internal(address, true);
  log_watchpoint(address, value, 8, false);
  return value;
}

std::uint16_t GbaBus::read16(std::uint32_t address) const {
  std::uint32_t aligned = address & ~1u;
  std::uint16_t lo = read8_internal(aligned, false);
  std::uint16_t hi = read8_internal(aligned + 1, false);
  std::uint16_t value = static_cast<std::uint16_t>(lo | (hi << 8));
  if (address & 0x1u) {
    value = static_cast<std::uint16_t>((value >> 8) | (value << 8));
  }
  log_io_read(address, value, 16);
  log_watchpoint(address, value, 16, false);
  return value;
}

std::uint32_t GbaBus::read32(std::uint32_t address) const {
  std::uint32_t aligned = address & ~3u;
  std::uint32_t b0 = read8_internal(aligned, false);
  std::uint32_t b1 = read8_internal(aligned + 1, false);
  std::uint32_t b2 = read8_internal(aligned + 2, false);
  std::uint32_t b3 = read8_internal(aligned + 3, false);
  std::uint32_t value = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
  std::uint32_t rotate = (address & 0x3u) * 8u;
  if (rotate != 0) {
    value = (value >> rotate) | (value << (32 - rotate));
  }
  log_io_read(address, value, 32);
  log_watchpoint(address, value, 32, false);
  return value;
}

std::uint8_t GbaBus::read_palette8_fast(std::uint32_t address) const {
  std::uint32_t offset = 0;
  if (!palette_offset_for(address, &offset)) {
    return 0xFF;
  }
  if (offset >= palette_.size()) {
    return 0xFF;
  }
  return palette_[offset];
}

std::uint16_t GbaBus::read_palette16_fast(std::uint32_t address) const {
  std::uint32_t aligned = address & ~1u;
  std::uint32_t offset = 0;
  if (!palette_offset_for(aligned, &offset)) {
    return 0xFFFF;
  }
  if (offset + 1 >= palette_.size()) {
    return 0xFFFF;
  }
  return static_cast<std::uint16_t>(palette_[offset] |
                                    (static_cast<std::uint16_t>(palette_[offset + 1]) << 8));
}

std::uint8_t GbaBus::read_vram8_fast(std::uint32_t address) const {
  std::uint32_t offset = 0;
  if (!vram_offset_for(address, &offset)) {
    return 0xFF;
  }
  if (offset >= vram_.size()) {
    return 0xFF;
  }
  return vram_[offset];
}

std::uint16_t GbaBus::read_vram16_fast(std::uint32_t address) const {
  std::uint32_t aligned = address & ~1u;
  std::uint32_t offset = 0;
  if (!vram_offset_for(aligned, &offset)) {
    return 0xFFFF;
  }
  if (offset + 1 >= vram_.size()) {
    return 0xFFFF;
  }
  return static_cast<std::uint16_t>(vram_[offset] |
                                    (static_cast<std::uint16_t>(vram_[offset + 1]) << 8));
}

std::uint8_t GbaBus::read_oam8_fast(std::uint32_t address) const {
  if (address < kOamBase) {
    return 0xFF;
  }
  std::uint32_t offset = address - kOamBase;
  if (offset >= kOamSize) {
    return 0xFF;
  }
  return oam_[offset];
}

std::uint16_t GbaBus::read_oam16_fast(std::uint32_t address) const {
  std::uint32_t aligned = address & ~1u;
  if (aligned < kOamBase) {
    return 0xFFFF;
  }
  std::uint32_t offset = aligned - kOamBase;
  if (offset + 1 >= kOamSize) {
    return 0xFFFF;
  }
  return static_cast<std::uint16_t>(oam_[offset] |
                                    (static_cast<std::uint16_t>(oam_[offset + 1]) << 8));
}

void GbaBus::write8(std::uint32_t address, std::uint8_t value) {
  log_video_io_write(address, value, 8);
  log_watchpoint(address, value, 8, true);
  write8_internal(address, value, true, true);
}

void GbaBus::write16(std::uint32_t address, std::uint16_t value) {
  log_io_write(address, value, 16);
  log_video_io_write(address, value, 16);
  log_watchpoint(address, value, 16, true);
  std::uint32_t aligned = address & ~1u;
  if (aligned == kIfAddr) {
    std::uint16_t cur = read_io16(kIfAddr);
    std::uint16_t next = static_cast<std::uint16_t>(cur & ~value);
    write_io16_raw(kIfAddr, next);
    return;
  }
  write8_internal(aligned, static_cast<std::uint8_t>(value & 0xFF), false, false);
  write8_internal(aligned + 1, static_cast<std::uint8_t>(value >> 8), false, false);
}

void GbaBus::write32(std::uint32_t address, std::uint32_t value) {
  log_io_write(address, value, 32);
  log_video_io_write(address, value, 32);
  log_watchpoint(address, value, 32, true);
  std::uint32_t aligned = address & ~3u;
  write8_internal(aligned, static_cast<std::uint8_t>(value & 0xFF), false, false);
  write8_internal(aligned + 1, static_cast<std::uint8_t>((value >> 8) & 0xFF), false, false);
  write8_internal(aligned + 2, static_cast<std::uint8_t>((value >> 16) & 0xFF), false, false);
  write8_internal(aligned + 3, static_cast<std::uint8_t>((value >> 24) & 0xFF), false, false);
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

bool GbaBus::rom_offset_for(std::uint32_t address, std::uint32_t* offset) const {
  if (!offset || address < kRomBase || address > kRomEnd || rom_.empty()) {
    return false;
  }
  std::uint32_t calc = (address - kRomBase) & 0x01FFFFFFu;
  if (rom_mask_ != 0) {
    calc &= rom_mask_;
  }
  if (calc >= rom_.size()) {
    if (!rom_size_pow2_) {
      calc %= static_cast<std::uint32_t>(rom_.size());
    } else {
      return false;
    }
  }
  *offset = calc;
  return true;
}

bool GbaBus::patch_rom16(std::uint32_t address, std::uint16_t value) {
  std::uint32_t offset = 0;
  if (!rom_offset_for(address, &offset)) {
    return false;
  }
  if (offset + 1 >= rom_.size()) {
    return false;
  }
  rom_[offset] = static_cast<std::uint8_t>(value & 0xFF);
  rom_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  return true;
}

bool GbaBus::patch_rom32(std::uint32_t address, std::uint32_t value) {
  std::uint32_t offset = 0;
  if (!rom_offset_for(address, &offset)) {
    return false;
  }
  if (offset + 3 >= rom_.size()) {
    return false;
  }
  rom_[offset] = static_cast<std::uint8_t>(value & 0xFF);
  rom_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  rom_[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  rom_[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
  return true;
}

} // namespace gbemu::core
