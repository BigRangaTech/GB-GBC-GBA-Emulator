#pragma once

#include <array>
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

  void step(int cycles);

  std::uint8_t read_u8(std::uint16_t address) const;
  void write_u8(std::uint16_t address, std::uint8_t value);

  bool boot_rom_enabled() const;

  std::uint8_t interrupt_enable() const;
  std::uint8_t interrupt_flags() const;
  void set_interrupt_flags(std::uint8_t value);
  void request_interrupt(std::uint8_t bit);
  void set_ly(std::uint8_t value);
  void set_stat(std::uint8_t value);
  std::uint8_t read_vram(std::uint16_t address, int bank) const;
  std::uint8_t bg_palette_byte(int index) const;
  std::uint8_t obj_palette_byte(int index) const;
  void set_joypad_state(std::uint8_t state);
  bool handle_stop();
  void on_hblank();
  void serialize(std::vector<std::uint8_t>* out) const;
  bool deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error);
  bool has_battery() const;
  bool has_ram() const;
  bool has_rtc() const;
  std::vector<std::uint8_t> ram_data() const;
  void load_ram_data(const std::vector<std::uint8_t>& data);
  std::vector<std::uint8_t> rtc_data() const;
  void load_rtc_data(const std::vector<std::uint8_t>& data);

 private:
  enum class MbcType {
    None,
    MBC2,
    MBC1,
    MBC3,
    MBC5,
  };

  std::uint8_t read_rom(std::uint16_t address) const;
  int rom_banks_from_code(std::uint8_t code) const;
  int ram_banks_from_code(std::uint8_t code) const;
  int effective_rom_bank() const;
  int effective_ram_bank() const;
  int effective_wram_bank() const;
  void timer_tick();
  void hdma_start(std::uint8_t value);
  void hdma_transfer_block();

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

  std::uint8_t div_ = 0;
  std::uint8_t tima_ = 0;
  std::uint8_t tma_ = 0;
  std::uint8_t tac_ = 0;
  int div_counter_ = 0;
  int timer_counter_ = 0;
  bool tima_reload_pending_ = false;
  int tima_reload_delay_ = 0;

  MbcType mbc_type_ = MbcType::None;
  std::uint8_t cart_type_ = 0;
  std::uint8_t rom_size_code_ = 0;
  std::uint8_t ram_size_code_ = 0;
  int rom_banks_ = 0;
  int ram_banks_ = 0;
  bool ram_enabled_ = false;
  std::uint8_t rom_bank_low_ = 1;
  std::uint8_t rom_bank_high_ = 0;
  std::uint8_t bank_mode_ = 0;
  std::uint8_t ram_bank_ = 0;

  std::uint8_t vram_bank_ = 0;
  std::uint8_t key1_ = 0;
  std::uint8_t wram_bank_ = 1;
  std::array<std::uint8_t, 64> bg_palette_{};
  std::array<std::uint8_t, 64> obj_palette_{};
  std::uint8_t bgpi_ = 0;
  bool bgpi_inc_ = false;
  std::uint8_t obpi_ = 0;
  bool obpi_inc_ = false;
  std::uint8_t joypad_state_ = 0xFF;

  std::int64_t rtc_offset_seconds_ = 0;
  std::int64_t rtc_base_time_ = 0;
  bool rtc_halt_ = false;
  bool rtc_latched_ = false;
  std::array<std::uint8_t, 5> rtc_latch_{};
  std::uint8_t rtc_latch_state_ = 0;

  std::uint16_t hdma_source_ = 0;
  std::uint16_t hdma_dest_ = 0;
  std::uint8_t hdma_length_ = 0;
  bool hdma_active_ = false;
};

} // namespace gbemu::core
