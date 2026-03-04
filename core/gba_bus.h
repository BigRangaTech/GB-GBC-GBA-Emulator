#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbemu::core {

class GbaBus {
 public:
  enum class SaveType {
    None,
    Sram,
    Flash64K,
    Flash128K,
    Eeprom512B,
    Eeprom8K,
  };

  enum class EepromMode {
    Idle,
    Reading,
  };

  bool load(const std::vector<std::uint8_t>& rom,
            const std::vector<std::uint8_t>& bios,
            std::string* error);

  std::uint8_t read8(std::uint32_t address) const;
  std::uint16_t read16(std::uint32_t address) const;
  std::uint32_t read32(std::uint32_t address) const;
  std::uint16_t fetch16(std::uint32_t address);
  std::uint32_t fetch32(std::uint32_t address);
  std::uint8_t read_palette8_fast(std::uint32_t address) const;
  std::uint16_t read_palette16_fast(std::uint32_t address) const;
  std::uint8_t read_vram8_fast(std::uint32_t address) const;
  std::uint16_t read_vram16_fast(std::uint32_t address) const;
  std::uint8_t read_oam8_fast(std::uint32_t address) const;
  std::uint16_t read_oam16_fast(std::uint32_t address) const;

  void write8(std::uint32_t address, std::uint8_t value);
  void write16(std::uint32_t address, std::uint16_t value);
  void write32(std::uint32_t address, std::uint32_t value);
  std::uint16_t read_io16(std::uint32_t address) const;
  void write_io16_raw(std::uint32_t address, std::uint16_t value);
  void set_if_bits(std::uint16_t mask);
  void set_trace_io_limit(int limit);
  void set_watch_video_io_limit(int limit);
  void set_watch_io_read_limit(int limit);
  void clear_watchpoints();
  void add_watchpoint(std::uint32_t start, std::uint32_t end, bool read, bool write);
  void set_watchpoint_limit(int limit);
  void set_last_pc(std::uint32_t pc);
  bool take_postflg_write(std::uint32_t* pc, std::uint8_t* value);
  bool take_halt_request(bool* stop);
  std::string take_debug_output();
  bool patch_rom16(std::uint32_t address, std::uint16_t value);
  bool patch_rom32(std::uint32_t address, std::uint32_t value);
  void serialize(std::vector<std::uint8_t>* out) const;
  bool deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error);
  bool has_battery() const { return save_type_ != SaveType::None; }
  bool has_ram() const { return !save_data_.empty(); }
  std::vector<std::uint8_t> save_data() const { return save_data_; }
  void load_save_data(const std::vector<std::uint8_t>& data);
  SaveType save_type() const { return save_type_; }
  void begin_cpu_step();
  int finish_cpu_step(int base_cycles);
  void invalidate_prefetch_stream() const;

  const std::vector<std::uint8_t>& rom() const { return rom_; }
  const std::vector<std::uint8_t>& bios() const { return bios_; }
  void set_bios_enabled(bool enabled) { bios_enabled_ = enabled; }
  bool bios_enabled() const { return bios_enabled_; }

 private:
  std::uint8_t read_mem(const std::vector<std::uint8_t>& mem,
                        std::uint32_t address,
                        std::uint32_t base) const;
  void write_mem(std::vector<std::uint8_t>& mem,
                 std::uint32_t address,
                 std::uint32_t base,
                 std::uint8_t value);
  std::uint8_t read8_internal(std::uint32_t address, bool allow_log) const;
  void write8_internal(std::uint32_t address,
                       std::uint8_t value,
                       bool allow_trace,
                       bool byte_access);
  void log_io_write(std::uint32_t address, std::uint32_t value, int bits);
  void log_video_io_write(std::uint32_t address, std::uint32_t value, int bits);
  void log_io_read(std::uint32_t address, std::uint32_t value, int bits) const;
  void log_watchpoint(std::uint32_t address,
                      std::uint32_t value,
                      int bits,
                      bool write) const;
  bool rom_offset_for(std::uint32_t address, std::uint32_t* offset) const;
  bool is_gamepak_address(std::uint32_t address) const;
  int gamepak_wait_cycles_halfword(std::uint32_t address, bool sequential) const;
  int gamepak_wait_cycles(std::uint32_t address, int bytes, bool sequential_first) const;
  int sram_wait_cycles() const;
  bool prefetch_enabled() const;
  void refill_prefetch(int cycles) const;
  void account_data_access_timing(std::uint32_t address, int bytes) const;
  void account_fetch_timing(std::uint32_t address, int bytes) const;
  std::uint16_t read16_no_timing(std::uint32_t address) const;
  std::uint32_t read32_no_timing(std::uint32_t address) const;
  bool is_eeprom_address(std::uint32_t address) const;
  bool is_flash_address(std::uint32_t address) const;
  bool is_mgba_debug_address(std::uint32_t address) const;
  void handle_mgba_debug_write(std::uint32_t address, std::uint8_t value);
  void flush_mgba_debug_string();
  std::uint8_t read_save8(std::uint32_t address) const;
  void write_save8(std::uint32_t address, std::uint8_t value);
  std::uint8_t read_flash8(std::uint32_t address) const;
  void write_flash8(std::uint32_t address, std::uint8_t value);
  void flash_reset_command_state();
  std::size_t flash_data_offset(std::uint32_t address) const;
  void flash_erase_sector(std::uint32_t address);
  std::uint16_t read_eeprom16(std::uint32_t address) const;
  void write_eeprom16(std::uint32_t address, std::uint16_t value);
  void eeprom_set_addr_bits(int bits);
  std::uint32_t eeprom_block_from_command(std::size_t start_bit, int bits) const;
  void eeprom_finalize_write_command(int addr_bits);
  void eeprom_finalize_read_command(int addr_bits) const;

  struct Watchpoint {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    bool read = false;
    bool write = false;
  };

  std::vector<std::uint8_t> bios_;
  std::vector<std::uint8_t> ewram_;
  std::vector<std::uint8_t> iwram_;
  std::vector<std::uint8_t> io_;
  std::vector<std::uint8_t> palette_;
  std::vector<std::uint8_t> vram_;
  std::vector<std::uint8_t> oam_;
  std::vector<std::uint8_t> rom_;
  std::vector<std::uint8_t> save_data_;
  SaveType save_type_ = SaveType::None;
  bool flash_id_mode_ = false;
  int flash_unlock_stage_ = 0;
  bool flash_expect_program_ = false;
  bool flash_expect_bank_ = false;
  bool flash_erase_armed_ = false;
  std::uint8_t flash_bank_ = 0;
  mutable EepromMode eeprom_mode_ = EepromMode::Idle;
  int eeprom_addr_bits_ = 0;
  mutable std::vector<std::uint8_t> eeprom_cmd_bits_;
  mutable std::vector<std::uint8_t> eeprom_read_bits_;
  mutable std::size_t eeprom_read_index_ = 0;
  std::uint32_t rom_mask_ = 0;
  bool rom_size_pow2_ = false;
  bool bios_enabled_ = true;
  bool trace_io_ = false;
  int trace_io_limit_ = 0;
  int watch_video_io_limit_ = 0;
  int watch_video_io_count_ = 0;
  bool watch_io_read_enabled_ = false;
  int watch_io_read_limit_ = 0;
  mutable int watch_io_read_count_ = 0;
  std::vector<Watchpoint> watchpoints_;
  int watchpoint_limit_ = 0;
  mutable int watchpoint_count_ = 0;
  std::uint32_t last_pc_ = 0;
  bool postflg_pending_ = false;
  std::uint32_t postflg_pc_ = 0;
  std::uint8_t postflg_value_ = 0;
  bool halt_requested_ = false;
  bool stop_requested_ = false;
  std::vector<std::uint8_t> mgba_debug_io_;
  bool mgba_debug_enabled_ = false;
  std::string mgba_debug_output_;
  bool timing_active_ = false;
  mutable int pending_timing_cycles_ = 0;
  mutable bool fetch_stream_active_ = false;
  mutable std::uint32_t fetch_expected_addr_ = 0;
  mutable int prefetch_halfwords_ = 0;
  mutable bool data_stream_active_ = false;
  mutable std::uint32_t data_expected_addr_ = 0;
};

} // namespace gbemu::core
