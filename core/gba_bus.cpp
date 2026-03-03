#include "gba_bus.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>

#include "state_io.h"

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
constexpr std::uint8_t kBusExtMagic[] = {'G', 'B', 'A', 'B', 'U', 'S', 'E', '1'};
constexpr std::uint32_t kIfAddr = 0x04000202;
constexpr std::uint32_t kRegDispcnt = 0x04000000;
constexpr std::uint32_t kRegDispstat = 0x04000004;
constexpr std::uint32_t kRegVcount = 0x04000006;
constexpr std::uint32_t kRegKeyinput = 0x04000130;
constexpr std::uint32_t kRegIe = 0x04000200;
constexpr std::uint32_t kRegIme = 0x04000208;
constexpr std::uint32_t kRegWaitcnt = 0x04000204;
constexpr std::uint32_t kRegPostflg = 0x04000300;
constexpr std::uint32_t kRegHaltcnt = 0x04000301;
constexpr std::uint16_t kIrqWritableMask = 0x3FFF;
constexpr std::uint32_t kMgbaDebugBase = 0x04FFF600;
constexpr std::uint32_t kMgbaDebugSize = 0x200;
constexpr std::uint32_t kMgbaDebugFlags = 0x04FFF700;
constexpr std::uint32_t kMgbaDebugEnable = 0x04FFF780;

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

bool rom_contains_tag(const std::vector<std::uint8_t>& rom, const char* tag) {
  if (!tag || *tag == '\0' || rom.empty()) {
    return false;
  }
  std::size_t len = 0;
  while (tag[len] != '\0') {
    ++len;
  }
  if (len == 0 || len > rom.size()) {
    return false;
  }
  for (std::size_t i = 0; i + len <= rom.size(); ++i) {
    std::size_t j = 0;
    while (j < len && rom[i + j] == static_cast<std::uint8_t>(tag[j])) {
      ++j;
    }
    if (j == len) {
      return true;
    }
  }
  return false;
}

GbaBus::SaveType detect_save_type(const std::vector<std::uint8_t>& rom) {
  if (rom_contains_tag(rom, "FLASH1M_V")) {
    return GbaBus::SaveType::Flash128K;
  }
  if (rom_contains_tag(rom, "FLASH512_V") || rom_contains_tag(rom, "FLASH_V")) {
    return GbaBus::SaveType::Flash64K;
  }
  if (rom_contains_tag(rom, "EEPROM_V")) {
    return GbaBus::SaveType::Eeprom8K;
  }
  if (rom_contains_tag(rom, "SRAM_V")) {
    return GbaBus::SaveType::Sram;
  }
  return GbaBus::SaveType::None;
}

std::size_t save_size_for_type(GbaBus::SaveType type) {
  switch (type) {
    case GbaBus::SaveType::Sram:
    case GbaBus::SaveType::Flash64K:
      return 64 * 1024;
    case GbaBus::SaveType::Flash128K:
      return 128 * 1024;
    case GbaBus::SaveType::Eeprom512B:
      return 512;
    case GbaBus::SaveType::Eeprom8K:
      return 8 * 1024;
    case GbaBus::SaveType::None:
    default:
      return 0;
  }
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
  save_type_ = detect_save_type(rom_);
  std::uint8_t fill = (save_type_ == SaveType::Sram || save_type_ == SaveType::None) ? 0x00 : 0xFF;
  save_data_.assign(save_size_for_type(save_type_), fill);
  flash_id_mode_ = false;
  flash_reset_command_state();
  flash_bank_ = 0;
  eeprom_mode_ = EepromMode::Idle;
  eeprom_addr_bits_ = (save_type_ == SaveType::Eeprom512B) ? 6
                    : (save_type_ == SaveType::Eeprom8K) ? 14
                                                         : 0;
  eeprom_cmd_bits_.clear();
  eeprom_read_bits_.clear();
  eeprom_read_index_ = 0;
  mgba_debug_io_.assign(kMgbaDebugSize, 0);
  mgba_debug_enabled_ = false;
  mgba_debug_output_.clear();
  timing_active_ = false;
  pending_timing_cycles_ = 0;
  fetch_stream_active_ = false;
  fetch_expected_addr_ = 0;
  prefetch_halfwords_ = 0;
  return true;
}

void GbaBus::begin_cpu_step() {
  timing_active_ = true;
  pending_timing_cycles_ = 0;
  if (!prefetch_enabled()) {
    prefetch_halfwords_ = 0;
  }
}

int GbaBus::finish_cpu_step(int base_cycles) {
  if (base_cycles > 0) {
    refill_prefetch(base_cycles);
  }
  int extra = pending_timing_cycles_;
  pending_timing_cycles_ = 0;
  timing_active_ = false;
  return extra;
}

bool GbaBus::is_gamepak_address(std::uint32_t address) const {
  return address >= kRomBase && address <= kRomEnd;
}

int GbaBus::sram_wait_cycles() const {
  static constexpr std::array<int, 4> kSramWait = {4, 3, 2, 8};
  std::uint16_t waitcnt = read_io16(kRegWaitcnt);
  return kSramWait[waitcnt & 0x3u];
}

bool GbaBus::prefetch_enabled() const {
  return (read_io16(kRegWaitcnt) & (1u << 14)) != 0;
}

int GbaBus::gamepak_wait_cycles_halfword(std::uint32_t address, bool sequential) const {
  if (!is_gamepak_address(address)) {
    return 0;
  }

  int ws = 0;
  if (address >= 0x0A000000u) {
    ws = (address >= 0x0C000000u) ? 2 : 1;
  }
  if (ws < 0) {
    ws = 0;
  } else if (ws > 2) {
    ws = 2;
  }

  static constexpr std::array<int, 4> kNseqWait = {4, 3, 2, 8};
  static constexpr std::array<std::array<int, 2>, 3> kSeqWait = {
      std::array<int, 2>{2, 1}, // WS0
      std::array<int, 2>{4, 1}, // WS1
      std::array<int, 2>{8, 1}, // WS2
  };

  std::uint16_t waitcnt = read_io16(kRegWaitcnt);
  int nseq_index = (waitcnt >> (4 + ws * 3)) & 0x3;
  int seq_index = (waitcnt >> (6 + ws * 3)) & 0x1;
  return sequential ? kSeqWait[static_cast<std::size_t>(ws)][static_cast<std::size_t>(seq_index)]
                    : kNseqWait[static_cast<std::size_t>(nseq_index)];
}

int GbaBus::gamepak_wait_cycles(std::uint32_t address, int bytes, bool sequential_first) const {
  if (bytes <= 0 || !is_gamepak_address(address)) {
    return 0;
  }
  int halfwords = (bytes + 1) / 2;
  int cycles = 0;
  for (int i = 0; i < halfwords; ++i) {
    bool sequential = sequential_first || i > 0;
    cycles += gamepak_wait_cycles_halfword(address + static_cast<std::uint32_t>(i * 2), sequential);
  }
  return cycles;
}

void GbaBus::refill_prefetch(int cycles) const {
  if (cycles <= 0) {
    return;
  }
  if (!prefetch_enabled() || !fetch_stream_active_) {
    prefetch_halfwords_ = 0;
    return;
  }

  constexpr int kPrefetchDepthHalfwords = 8;
  int budget = cycles;
  while (prefetch_halfwords_ < kPrefetchDepthHalfwords && budget > 0) {
    std::uint32_t next_addr =
        fetch_expected_addr_ + static_cast<std::uint32_t>(prefetch_halfwords_ * 2);
    if (!is_gamepak_address(next_addr)) {
      fetch_stream_active_ = false;
      prefetch_halfwords_ = 0;
      break;
    }
    int cost = gamepak_wait_cycles_halfword(next_addr, true);
    if (cost <= 0 || budget < cost) {
      break;
    }
    budget -= cost;
    ++prefetch_halfwords_;
  }
}

void GbaBus::account_data_access_timing(std::uint32_t address, int bytes) const {
  if (!timing_active_ || bytes <= 0) {
    return;
  }
  if (is_gamepak_address(address)) {
    // Any CPU data access on the Game Pak bus breaks the instruction-fetch stream
    // and invalidates queued prefetch data.
    fetch_stream_active_ = false;
    prefetch_halfwords_ = 0;
    pending_timing_cycles_ += gamepak_wait_cycles(address, bytes, false);
    return;
  }
  if (address >= kSramBase && address < kSramBase + kSramSize) {
    pending_timing_cycles_ += sram_wait_cycles() * std::max(1, bytes);
  }
}

void GbaBus::account_fetch_timing(std::uint32_t address, int bytes) const {
  if (!timing_active_ || bytes <= 0) {
    return;
  }
  if (!is_gamepak_address(address)) {
    fetch_stream_active_ = false;
    prefetch_halfwords_ = 0;
    return;
  }

  bool stream_sequential = fetch_stream_active_ && (address == fetch_expected_addr_);
  if (!stream_sequential) {
    fetch_stream_active_ = true;
    fetch_expected_addr_ = address;
    prefetch_halfwords_ = 0;
  }

  int needed_halfwords = (bytes + 1) / 2;
  int queued = 0;
  if (prefetch_enabled()) {
    queued = std::min(prefetch_halfwords_, needed_halfwords);
    prefetch_halfwords_ -= queued;
    needed_halfwords -= queued;
  } else {
    prefetch_halfwords_ = 0;
  }

  for (int i = 0; i < needed_halfwords; ++i) {
    bool sequential = stream_sequential || queued > 0 || i > 0;
    std::uint32_t fetch_addr =
        address + static_cast<std::uint32_t>((queued + i) * 2);
    pending_timing_cycles_ += gamepak_wait_cycles_halfword(fetch_addr, sequential);
    stream_sequential = true;
  }

  fetch_expected_addr_ = address + static_cast<std::uint32_t>(bytes);
}

bool GbaBus::is_eeprom_address(std::uint32_t address) const {
  if (save_type_ != SaveType::Eeprom512B && save_type_ != SaveType::Eeprom8K) {
    return false;
  }
  return address >= 0x0D000000u && address <= 0x0DFFFFFFu;
}

bool GbaBus::is_flash_address(std::uint32_t address) const {
  if (save_type_ != SaveType::Flash64K && save_type_ != SaveType::Flash128K) {
    return false;
  }
  return address >= kSramBase && address < kSramBase + kSramSize;
}

std::size_t GbaBus::flash_data_offset(std::uint32_t address) const {
  if (save_data_.empty() || address < kSramBase) {
    return 0;
  }
  std::size_t offset = static_cast<std::size_t>((address - kSramBase) & 0xFFFFu);
  if (save_type_ == SaveType::Flash128K) {
    offset += static_cast<std::size_t>(flash_bank_ & 0x1u) * 0x10000u;
  }
  if (offset >= save_data_.size()) {
    offset %= save_data_.size();
  }
  return offset;
}

void GbaBus::flash_reset_command_state() {
  flash_unlock_stage_ = 0;
  flash_expect_program_ = false;
  flash_expect_bank_ = false;
  flash_erase_armed_ = false;
}

void GbaBus::flash_erase_sector(std::uint32_t address) {
  if (save_data_.empty() || address < kSramBase) {
    return;
  }
  constexpr std::size_t kSectorSize = 0x1000;
  std::size_t base = flash_data_offset(address);
  base = (base / kSectorSize) * kSectorSize;
  std::size_t end = std::min(base + kSectorSize, save_data_.size());
  std::fill(save_data_.begin() + static_cast<std::ptrdiff_t>(base),
            save_data_.begin() + static_cast<std::ptrdiff_t>(end),
            0xFF);
}

std::uint8_t GbaBus::read_flash8(std::uint32_t address) const {
  if (save_data_.empty()) {
    return 0xFF;
  }
  std::uint32_t offset = (address - kSramBase) & 0xFFFFu;
  if (flash_id_mode_) {
    if (offset == 0x0000u) {
      return (save_type_ == SaveType::Flash128K) ? 0xC2u : 0xBFu;
    }
    if (offset == 0x0001u) {
      return (save_type_ == SaveType::Flash128K) ? 0x09u : 0xD4u;
    }
    return 0xFF;
  }
  return save_data_[flash_data_offset(address)];
}

void GbaBus::write_flash8(std::uint32_t address, std::uint8_t value) {
  if (save_data_.empty()) {
    return;
  }
  std::uint32_t offset = (address - kSramBase) & 0xFFFFu;

  if (value == 0xF0) {
    flash_id_mode_ = false;
    flash_reset_command_state();
    return;
  }
  if (flash_expect_program_) {
    save_data_[flash_data_offset(address)] = value;
    flash_expect_program_ = false;
    flash_unlock_stage_ = 0;
    return;
  }
  if (flash_expect_bank_) {
    if (save_type_ == SaveType::Flash128K && offset == 0x0000u) {
      flash_bank_ = static_cast<std::uint8_t>(value & 0x1u);
    }
    flash_expect_bank_ = false;
    flash_unlock_stage_ = 0;
    return;
  }

  if (flash_unlock_stage_ == 0) {
    if (offset == 0x5555u && value == 0xAAu) {
      flash_unlock_stage_ = 1;
    }
    return;
  }
  if (flash_unlock_stage_ == 1) {
    if (offset == 0x2AAAu && value == 0x55u) {
      flash_unlock_stage_ = 2;
      return;
    }
    flash_unlock_stage_ = 0;
    return;
  }

  flash_unlock_stage_ = 0;
  if (flash_erase_armed_) {
    if (offset == 0x5555u && value == 0x10u) {
      std::fill(save_data_.begin(), save_data_.end(), 0xFF);
    } else if (value == 0x30u) {
      flash_erase_sector(address);
    }
    flash_erase_armed_ = false;
    return;
  }
  if (offset != 0x5555u) {
    return;
  }
  switch (value) {
    case 0x90:
      flash_id_mode_ = true;
      flash_reset_command_state();
      break;
    case 0xA0:
      flash_expect_program_ = true;
      break;
    case 0xB0:
      if (save_type_ == SaveType::Flash128K) {
        flash_expect_bank_ = true;
      }
      break;
    case 0x80:
      flash_erase_armed_ = true;
      break;
    default:
      break;
  }
}

void GbaBus::eeprom_set_addr_bits(int bits) {
  if (bits != 6 && bits != 14) {
    return;
  }
  eeprom_addr_bits_ = bits;
  std::size_t target_size = (bits == 6) ? 512u : 8192u;
  if (save_data_.size() == target_size) {
    save_type_ = (bits == 6) ? SaveType::Eeprom512B : SaveType::Eeprom8K;
    return;
  }
  std::vector<std::uint8_t> resized(target_size, 0xFF);
  std::size_t copy = std::min(target_size, save_data_.size());
  std::copy_n(save_data_.begin(), copy, resized.begin());
  save_data_.swap(resized);
  save_type_ = (bits == 6) ? SaveType::Eeprom512B : SaveType::Eeprom8K;
}

std::uint32_t GbaBus::eeprom_block_from_command(std::size_t start_bit, int bits) const {
  std::uint32_t value = 0;
  for (int i = 0; i < bits; ++i) {
    value <<= 1;
    std::size_t index = start_bit + static_cast<std::size_t>(i);
    if (index < eeprom_cmd_bits_.size() && eeprom_cmd_bits_[index] != 0) {
      value |= 1u;
    }
  }
  if (bits == 14) {
    return value & 0x03FFu;
  }
  return value & 0x003Fu;
}

void GbaBus::eeprom_finalize_write_command(int addr_bits) {
  if (save_data_.empty()) {
    eeprom_cmd_bits_.clear();
    eeprom_mode_ = EepromMode::Idle;
    return;
  }
  std::uint32_t block = eeprom_block_from_command(2, addr_bits);
  std::size_t base = static_cast<std::size_t>(block) * 8u;
  std::size_t data_start = 2u + static_cast<std::size_t>(addr_bits);
  if (base + 8u <= save_data_.size()) {
    for (int i = 0; i < 64; ++i) {
      std::uint8_t bit = eeprom_cmd_bits_[data_start + static_cast<std::size_t>(i)] & 1u;
      std::size_t byte_index = base + static_cast<std::size_t>(i / 8);
      int shift = 7 - (i % 8);
      if (bit) {
        save_data_[byte_index] = static_cast<std::uint8_t>(save_data_[byte_index] | (1u << shift));
      } else {
        save_data_[byte_index] = static_cast<std::uint8_t>(save_data_[byte_index] & ~(1u << shift));
      }
    }
  }
  eeprom_cmd_bits_.clear();
  eeprom_mode_ = EepromMode::Idle;
}

void GbaBus::eeprom_finalize_read_command(int addr_bits) const {
  eeprom_read_bits_.clear();
  eeprom_read_index_ = 0;
  if (save_data_.empty()) {
    eeprom_mode_ = EepromMode::Idle;
    eeprom_cmd_bits_.clear();
    return;
  }
  std::uint32_t block = eeprom_block_from_command(2, addr_bits);
  std::size_t base = static_cast<std::size_t>(block) * 8u;
  eeprom_read_bits_.assign(4, 0);
  for (int i = 0; i < 64; ++i) {
    std::uint8_t bit = 1;
    std::size_t byte_index = base + static_cast<std::size_t>(i / 8);
    if (byte_index < save_data_.size()) {
      int shift = 7 - (i % 8);
      bit = static_cast<std::uint8_t>((save_data_[byte_index] >> shift) & 1u);
    }
    eeprom_read_bits_.push_back(bit);
  }
  eeprom_cmd_bits_.clear();
  eeprom_mode_ = EepromMode::Reading;
}

void GbaBus::write_eeprom16(std::uint32_t address, std::uint16_t value) {
  if (!is_eeprom_address(address)) {
    return;
  }
  if (eeprom_mode_ == EepromMode::Reading) {
    eeprom_mode_ = EepromMode::Idle;
    eeprom_read_bits_.clear();
    eeprom_read_index_ = 0;
  }
  std::uint8_t bit = static_cast<std::uint8_t>(value & 1u);
  eeprom_cmd_bits_.push_back(bit);
  if (!eeprom_cmd_bits_.empty() && eeprom_cmd_bits_[0] == 0) {
    eeprom_cmd_bits_.clear();
    return;
  }
  if (eeprom_cmd_bits_.size() < 2) {
    return;
  }
  std::uint8_t b0 = eeprom_cmd_bits_[0];
  std::uint8_t b1 = eeprom_cmd_bits_[1];
  std::size_t size = eeprom_cmd_bits_.size();
  int addr_bits = eeprom_addr_bits_;
  if (addr_bits != 6 && addr_bits != 14) {
    if (save_type_ == SaveType::Eeprom512B) {
      addr_bits = 6;
    } else if (save_type_ == SaveType::Eeprom8K) {
      addr_bits = 14;
    }
  }

  if (b0 == 1 && b1 == 0) {
    if (addr_bits == 6) {
      if (size == 73) {
        eeprom_set_addr_bits(6);
        eeprom_finalize_write_command(6);
      } else if (size > 73) {
        eeprom_cmd_bits_.clear();
      }
      return;
    }
    if (addr_bits == 14) {
      if (size == 81) {
        eeprom_set_addr_bits(14);
        eeprom_finalize_write_command(14);
      } else if (size > 81) {
        eeprom_cmd_bits_.clear();
      }
      return;
    }
    if (size == 73) {
      eeprom_set_addr_bits(6);
      eeprom_finalize_write_command(6);
    } else if (size == 81) {
      eeprom_set_addr_bits(14);
      eeprom_finalize_write_command(14);
    } else if (size > 81) {
      eeprom_cmd_bits_.clear();
    }
    return;
  }
  if (b0 == 1 && b1 == 1) {
    if (addr_bits == 6) {
      if (size == 9) {
        eeprom_set_addr_bits(6);
        eeprom_finalize_read_command(6);
      } else if (size > 9) {
        eeprom_cmd_bits_.clear();
      }
      return;
    }
    if (addr_bits == 14) {
      if (size == 17) {
        eeprom_set_addr_bits(14);
        eeprom_finalize_read_command(14);
      } else if (size > 17) {
        eeprom_cmd_bits_.clear();
      }
      return;
    }
    if (size == 9) {
      eeprom_set_addr_bits(6);
      eeprom_finalize_read_command(6);
    } else if (size == 17) {
      eeprom_set_addr_bits(14);
      eeprom_finalize_read_command(14);
    } else if (size > 17) {
      eeprom_cmd_bits_.clear();
    }
    return;
  }
  if (size > 2) {
    eeprom_cmd_bits_.clear();
  }
}

std::uint16_t GbaBus::read_eeprom16(std::uint32_t address) const {
  if (!is_eeprom_address(address)) {
    return 1;
  }
  std::uint16_t bit = 1;
  if (eeprom_mode_ == EepromMode::Reading && eeprom_read_index_ < eeprom_read_bits_.size()) {
    bit = static_cast<std::uint16_t>(eeprom_read_bits_[eeprom_read_index_] & 1u);
    ++eeprom_read_index_;
    if (eeprom_read_index_ >= eeprom_read_bits_.size()) {
      eeprom_mode_ = EepromMode::Idle;
      eeprom_read_bits_.clear();
      eeprom_read_index_ = 0;
    }
  }
  return bit;
}

std::uint8_t GbaBus::read_save8(std::uint32_t address) const {
  if (save_data_.empty()) {
    return 0xFF;
  }
  if (save_type_ == SaveType::Sram) {
    return read_mem(save_data_, address, kSramBase);
  }
  if (is_flash_address(address)) {
    return read_flash8(address);
  }
  return 0xFF;
}

void GbaBus::write_save8(std::uint32_t address, std::uint8_t value) {
  if (save_data_.empty()) {
    return;
  }
  if (save_type_ == SaveType::Sram) {
    write_mem(save_data_, address, kSramBase, value);
    return;
  }
  if (is_flash_address(address)) {
    write_flash8(address, value);
  }
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

std::string GbaBus::take_debug_output() {
  std::string out;
  out.swap(mgba_debug_output_);
  return out;
}

bool GbaBus::is_mgba_debug_address(std::uint32_t address) const {
  return address >= kMgbaDebugBase && address < (kMgbaDebugBase + kMgbaDebugSize);
}

void GbaBus::flush_mgba_debug_string() {
  if (!mgba_debug_enabled_ || mgba_debug_io_.empty()) {
    return;
  }
  constexpr std::size_t kStringOffset = 0x000;
  constexpr std::size_t kStringLimit = 0x100;
  std::string text;
  for (std::size_t i = 0; i < kStringLimit && (kStringOffset + i) < mgba_debug_io_.size(); ++i) {
    std::uint8_t c = mgba_debug_io_[kStringOffset + i];
    if (c == 0) {
      break;
    }
    text.push_back(static_cast<char>(c));
  }
  if (text.empty()) {
    return;
  }
  if (mgba_debug_output_.size() + text.size() + 1 > 64 * 1024) {
    mgba_debug_output_.erase(0, mgba_debug_output_.size() - 32 * 1024);
  }
  mgba_debug_output_ += text;
  mgba_debug_output_.push_back('\n');
}

void GbaBus::handle_mgba_debug_write(std::uint32_t address, std::uint8_t value) {
  if (!is_mgba_debug_address(address) || mgba_debug_io_.empty()) {
    return;
  }
  std::uint32_t offset = address - kMgbaDebugBase;
  mgba_debug_io_[offset] = value;

  if (address == kMgbaDebugEnable + 1u) {
    std::uint16_t enable =
        static_cast<std::uint16_t>(mgba_debug_io_[kMgbaDebugEnable - kMgbaDebugBase] |
                                   (static_cast<std::uint16_t>(
                                       mgba_debug_io_[kMgbaDebugEnable - kMgbaDebugBase + 1u])
                                    << 8));
    mgba_debug_enabled_ = (enable == 0xC0DEu);
    return;
  }
  if (address == kMgbaDebugFlags) {
    flush_mgba_debug_string();
  }
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
  if (is_mgba_debug_address(address)) {
    handle_mgba_debug_write(address, value);
    return;
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
      mask = static_cast<std::uint16_t>(mask & kIrqWritableMask);
      std::uint16_t next = static_cast<std::uint16_t>(cur & ~mask);
      write_io16_raw(kIfAddr, next);
      return;
    }
    write_mem(io_, address, kIoBase, value);
    std::uint32_t io_halfword = address & ~1u;
    if (io_halfword == kRegIe || io_halfword == kRegIme) {
      std::uint16_t cur = read_io16(io_halfword);
      write_io16_raw(io_halfword, cur);
    }
    if (address == kRegWaitcnt || address == (kRegWaitcnt + 1)) {
      fetch_stream_active_ = false;
      prefetch_halfwords_ = 0;
    }
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
    write_save8(address, value);
    return;
  }
}

std::uint8_t GbaBus::read8_internal(std::uint32_t address, bool allow_log) const {
  std::uint8_t value = 0xFF;
  if (is_mgba_debug_address(address) && !mgba_debug_io_.empty()) {
    std::uint32_t offset = address - kMgbaDebugBase;
    value = mgba_debug_io_[offset];
  } else if (bios_enabled_ && address < kBiosBase + kBiosSize) {
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
    value = read_save8(address);
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
  account_data_access_timing(address, 1);
  return value;
}

std::uint16_t GbaBus::read16_no_timing(std::uint32_t address) const {
  std::uint32_t aligned = address & ~1u;
  if (is_eeprom_address(aligned)) {
    std::uint16_t value = read_eeprom16(aligned);
    if (address & 0x1u) {
      value = static_cast<std::uint16_t>((value >> 8) | (value << 8));
    }
    log_io_read(address, value, 16);
    log_watchpoint(address, value, 16, false);
    return value;
  }
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

std::uint16_t GbaBus::read16(std::uint32_t address) const {
  std::uint16_t value = read16_no_timing(address);
  account_data_access_timing(address, 2);
  return value;
}

std::uint16_t GbaBus::fetch16(std::uint32_t address) {
  std::uint16_t value = read16_no_timing(address);
  account_fetch_timing(address, 2);
  return value;
}

std::uint32_t GbaBus::read32_no_timing(std::uint32_t address) const {
  std::uint32_t aligned = address & ~3u;
  if (is_eeprom_address(aligned)) {
    std::uint32_t lo = read_eeprom16(aligned);
    std::uint32_t hi = read_eeprom16(aligned + 2);
    std::uint32_t value = lo | (hi << 16);
    std::uint32_t rotate = (address & 0x3u) * 8u;
    if (rotate != 0) {
      value = (value >> rotate) | (value << (32 - rotate));
    }
    log_io_read(address, value, 32);
    log_watchpoint(address, value, 32, false);
    return value;
  }
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

std::uint32_t GbaBus::read32(std::uint32_t address) const {
  std::uint32_t value = read32_no_timing(address);
  account_data_access_timing(address, 4);
  return value;
}

std::uint32_t GbaBus::fetch32(std::uint32_t address) {
  std::uint32_t value = read32_no_timing(address);
  account_fetch_timing(address, 4);
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
  account_data_access_timing(address, 1);
}

void GbaBus::write16(std::uint32_t address, std::uint16_t value) {
  log_io_write(address, value, 16);
  log_video_io_write(address, value, 16);
  log_watchpoint(address, value, 16, true);
  std::uint32_t aligned = address & ~1u;
  if (is_eeprom_address(aligned)) {
    write_eeprom16(aligned, value);
    account_data_access_timing(address, 2);
    return;
  }
  if (aligned == kIfAddr) {
    std::uint16_t cur = read_io16(kIfAddr);
    std::uint16_t next = static_cast<std::uint16_t>(cur & ~value);
    write_io16_raw(kIfAddr, next);
    account_data_access_timing(address, 2);
    return;
  }
  write8_internal(aligned, static_cast<std::uint8_t>(value & 0xFF), false, false);
  write8_internal(aligned + 1, static_cast<std::uint8_t>(value >> 8), false, false);
  account_data_access_timing(address, 2);
}

void GbaBus::write32(std::uint32_t address, std::uint32_t value) {
  log_io_write(address, value, 32);
  log_video_io_write(address, value, 32);
  log_watchpoint(address, value, 32, true);
  std::uint32_t aligned = address & ~3u;
  if (is_eeprom_address(aligned)) {
    write_eeprom16(aligned, static_cast<std::uint16_t>(value & 0xFFFFu));
    write_eeprom16(aligned + 2, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
    account_data_access_timing(address, 4);
    return;
  }
  write8_internal(aligned, static_cast<std::uint8_t>(value & 0xFF), false, false);
  write8_internal(aligned + 1, static_cast<std::uint8_t>((value >> 8) & 0xFF), false, false);
  write8_internal(aligned + 2, static_cast<std::uint8_t>((value >> 16) & 0xFF), false, false);
  write8_internal(aligned + 3, static_cast<std::uint8_t>((value >> 24) & 0xFF), false, false);
  account_data_access_timing(address, 4);
}

std::uint16_t GbaBus::read_io16(std::uint32_t address) const {
  if (address < kIoBase || address + 1 >= kIoBase + kIoSize) {
    return 0xFFFF;
  }
  std::uint32_t offset = address - kIoBase;
  std::uint16_t lo = io_[offset];
  std::uint16_t hi = io_[offset + 1];
  std::uint16_t value = static_cast<std::uint16_t>(lo | (hi << 8));
  if (address == kRegIe || address == kIfAddr) {
    return static_cast<std::uint16_t>(value & kIrqWritableMask);
  }
  if (address == kRegIme) {
    return static_cast<std::uint16_t>(value & 0x0001u);
  }
  return value;
}

void GbaBus::write_io16_raw(std::uint32_t address, std::uint16_t value) {
  if (address < kIoBase || address + 1 >= kIoBase + kIoSize) {
    return;
  }
  if (address == kRegIe || address == kIfAddr) {
    value = static_cast<std::uint16_t>(value & kIrqWritableMask);
  } else if (address == kRegIme) {
    value = static_cast<std::uint16_t>(value & 0x0001u);
  }
  std::uint32_t offset = address - kIoBase;
  io_[offset] = static_cast<std::uint8_t>(value & 0xFF);
  io_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void GbaBus::set_if_bits(std::uint16_t mask) {
  mask = static_cast<std::uint16_t>(mask & kIrqWritableMask);
  std::uint16_t cur = read_io16(kIfAddr);
  std::uint16_t next = static_cast<std::uint16_t>(cur | mask);
  write_io16_raw(kIfAddr, next);
}

void GbaBus::serialize(std::vector<std::uint8_t>* out) const {
  if (!out) {
    return;
  }
  using namespace gbemu::core::state_io;
  write_u32(*out, rom_mask_);
  write_bool(*out, rom_size_pow2_);
  write_bool(*out, bios_enabled_);
  write_bool(*out, trace_io_);
  write_u32(*out, static_cast<std::uint32_t>(trace_io_limit_));
  write_u32(*out, static_cast<std::uint32_t>(watch_video_io_limit_));
  write_u32(*out, static_cast<std::uint32_t>(watch_video_io_count_));
  write_bool(*out, watch_io_read_enabled_);
  write_u32(*out, static_cast<std::uint32_t>(watch_io_read_limit_));
  write_u32(*out, static_cast<std::uint32_t>(watch_io_read_count_));
  write_u32(*out, static_cast<std::uint32_t>(watchpoint_limit_));
  write_u32(*out, static_cast<std::uint32_t>(watchpoint_count_));
  write_u32(*out, last_pc_);
  write_bool(*out, postflg_pending_);
  write_u32(*out, postflg_pc_);
  write_u8(*out, postflg_value_);
  write_bool(*out, halt_requested_);
  write_bool(*out, stop_requested_);

  write_u32(*out, static_cast<std::uint32_t>(watchpoints_.size()));
  for (const Watchpoint& watch : watchpoints_) {
    write_u32(*out, watch.start);
    write_u32(*out, watch.end);
    write_bool(*out, watch.read);
    write_bool(*out, watch.write);
  }

  write_bytes(*out, ewram_);
  write_bytes(*out, iwram_);
  write_bytes(*out, io_);
  write_bytes(*out, palette_);
  write_bytes(*out, vram_);
  write_bytes(*out, oam_);
  write_bytes(*out, save_data_);
  write_bytes(*out, bios_);
  write_bytes(*out, rom_);

  write_bytes_fixed(*out, kBusExtMagic, sizeof(kBusExtMagic));
  write_u8(*out, static_cast<std::uint8_t>(save_type_));
  write_bool(*out, flash_id_mode_);
  write_u32(*out, static_cast<std::uint32_t>(flash_unlock_stage_));
  write_bool(*out, flash_expect_program_);
  write_bool(*out, flash_expect_bank_);
  write_bool(*out, flash_erase_armed_);
  write_u8(*out, flash_bank_);
  write_u8(*out, static_cast<std::uint8_t>(eeprom_mode_));
  write_u32(*out, static_cast<std::uint32_t>(eeprom_addr_bits_));
  write_bytes(*out, eeprom_cmd_bits_);
  write_bytes(*out, eeprom_read_bits_);
  write_u32(*out, static_cast<std::uint32_t>(eeprom_read_index_));
}

bool GbaBus::deserialize(const std::vector<std::uint8_t>& data,
                         std::size_t& offset,
                         std::string* error) {
  using namespace gbemu::core::state_io;
  auto fail = [error](const char* message) {
    if (error) {
      *error = message;
    }
    return false;
  };
  auto read_int = [&](int* out_value) -> bool {
    std::uint32_t value = 0;
    if (!read_u32(data, offset, value)) {
      return false;
    }
    *out_value = static_cast<int>(value);
    return true;
  };
  auto read_vec_fixed = [&](std::vector<std::uint8_t>* out_mem,
                            std::size_t expected,
                            const char* label) -> bool {
    std::vector<std::uint8_t> tmp;
    if (!read_bytes(data, offset, tmp)) {
      return fail("Truncated GBA bus state");
    }
    if (tmp.size() != expected) {
      if (error) {
        *error = std::string("GBA bus ") + label + " size mismatch";
      }
      return false;
    }
    out_mem->swap(tmp);
    return true;
  };

  if (!read_u32(data, offset, rom_mask_) ||
      !read_bool(data, offset, rom_size_pow2_) ||
      !read_bool(data, offset, bios_enabled_) ||
      !read_bool(data, offset, trace_io_) ||
      !read_int(&trace_io_limit_) ||
      !read_int(&watch_video_io_limit_) ||
      !read_int(&watch_video_io_count_) ||
      !read_bool(data, offset, watch_io_read_enabled_) ||
      !read_int(&watch_io_read_limit_) ||
      !read_int(&watch_io_read_count_) ||
      !read_int(&watchpoint_limit_) ||
      !read_int(&watchpoint_count_) ||
      !read_u32(data, offset, last_pc_) ||
      !read_bool(data, offset, postflg_pending_) ||
      !read_u32(data, offset, postflg_pc_) ||
      !read_u8(data, offset, postflg_value_) ||
      !read_bool(data, offset, halt_requested_) ||
      !read_bool(data, offset, stop_requested_)) {
    return fail("Truncated GBA bus state");
  }

  std::uint32_t watch_count = 0;
  if (!read_u32(data, offset, watch_count)) {
    return fail("Truncated GBA bus state");
  }
  watchpoints_.clear();
  watchpoints_.reserve(static_cast<std::size_t>(watch_count));
  for (std::uint32_t i = 0; i < watch_count; ++i) {
    Watchpoint watch{};
    if (!read_u32(data, offset, watch.start) ||
        !read_u32(data, offset, watch.end) ||
        !read_bool(data, offset, watch.read) ||
        !read_bool(data, offset, watch.write)) {
      return fail("Truncated GBA bus state");
    }
    watchpoints_.push_back(watch);
  }

  if (!read_vec_fixed(&ewram_, kEwramSize, "EWRAM") ||
      !read_vec_fixed(&iwram_, kIwramSize, "IWRAM") ||
      !read_vec_fixed(&io_, kIoSize, "IO") ||
      !read_vec_fixed(&palette_, kPaletteSize, "palette") ||
      !read_vec_fixed(&vram_, kVramSize, "VRAM") ||
      !read_vec_fixed(&oam_, kOamSize, "OAM")) {
    return false;
  }
  std::vector<std::uint8_t> save_data;
  if (!read_bytes(data, offset, save_data)) {
    return fail("Truncated GBA bus state");
  }
  if (!save_data_.empty() && !save_data.empty()) {
    std::size_t count = std::min(save_data_.size(), save_data.size());
    std::copy_n(save_data.begin(), count, save_data_.begin());
  }
  if (!read_vec_fixed(&bios_, kBiosSize, "BIOS") ||
      !read_vec_fixed(&rom_, rom_.size(), "ROM")) {
    return false;
  }

  flash_id_mode_ = false;
  flash_reset_command_state();
  flash_bank_ = 0;
  eeprom_mode_ = EepromMode::Idle;
  eeprom_addr_bits_ = 0;
  eeprom_cmd_bits_.clear();
  eeprom_read_bits_.clear();
  eeprom_read_index_ = 0;
  mgba_debug_io_.assign(kMgbaDebugSize, 0);
  mgba_debug_enabled_ = false;
  mgba_debug_output_.clear();
  timing_active_ = false;
  pending_timing_cycles_ = 0;
  fetch_stream_active_ = false;
  fetch_expected_addr_ = 0;
  prefetch_halfwords_ = 0;

  if (save_type_ == SaveType::Eeprom512B || save_type_ == SaveType::Eeprom8K) {
    if (save_data_.size() == 512u) {
      save_type_ = SaveType::Eeprom512B;
      eeprom_addr_bits_ = 6;
    } else if (save_data_.size() == 8192u) {
      save_type_ = SaveType::Eeprom8K;
      eeprom_addr_bits_ = 14;
    }
  }

  if (offset + sizeof(kBusExtMagic) <= data.size()) {
    bool has_ext = true;
    for (std::size_t i = 0; i < sizeof(kBusExtMagic); ++i) {
      if (data[offset + i] != kBusExtMagic[i]) {
        has_ext = false;
        break;
      }
    }
    if (has_ext) {
      offset += sizeof(kBusExtMagic);
      std::uint8_t save_type = 0;
      std::uint8_t eeprom_mode = 0;
      std::uint32_t flash_unlock_stage = 0;
      std::uint32_t eeprom_addr_bits = 0;
      std::uint32_t eeprom_read_index = 0;
      std::vector<std::uint8_t> eeprom_cmd_bits;
      std::vector<std::uint8_t> eeprom_read_bits;
      if (!read_u8(data, offset, save_type) ||
          !read_bool(data, offset, flash_id_mode_) ||
          !read_u32(data, offset, flash_unlock_stage) ||
          !read_bool(data, offset, flash_expect_program_) ||
          !read_bool(data, offset, flash_expect_bank_) ||
          !read_bool(data, offset, flash_erase_armed_) ||
          !read_u8(data, offset, flash_bank_) ||
          !read_u8(data, offset, eeprom_mode) ||
          !read_u32(data, offset, eeprom_addr_bits) ||
          !read_bytes(data, offset, eeprom_cmd_bits) ||
          !read_bytes(data, offset, eeprom_read_bits) ||
          !read_u32(data, offset, eeprom_read_index)) {
        return fail("Truncated GBA bus extension state");
      }

      if (save_type <= static_cast<std::uint8_t>(SaveType::Eeprom8K)) {
        save_type_ = static_cast<SaveType>(save_type);
      } else {
        return fail("Invalid GBA bus save type");
      }
      flash_unlock_stage_ = static_cast<int>(flash_unlock_stage);
      if (eeprom_mode <= static_cast<std::uint8_t>(EepromMode::Reading)) {
        eeprom_mode_ = static_cast<EepromMode>(eeprom_mode);
      } else {
        eeprom_mode_ = EepromMode::Idle;
      }
      eeprom_addr_bits_ = static_cast<int>(eeprom_addr_bits);
      eeprom_cmd_bits_.swap(eeprom_cmd_bits);
      eeprom_read_bits_.swap(eeprom_read_bits);
      eeprom_read_index_ = std::min<std::size_t>(eeprom_read_index, eeprom_read_bits_.size());
    }
  }
  return true;
}

void GbaBus::load_save_data(const std::vector<std::uint8_t>& data) {
  if (save_data_.empty() || data.empty()) {
    return;
  }
  std::size_t count = std::min(save_data_.size(), data.size());
  std::copy_n(data.begin(), count, save_data_.begin());
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
