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
  void set_trace_io_limit(int limit);
  void set_watch_video_io_limit(int limit);
  void set_watch_io_read_limit(int limit);
  void clear_watchpoints();
  void add_watchpoint(std::uint32_t start, std::uint32_t end, bool read, bool write);
  void set_watchpoint_limit(int limit);
  void set_last_pc(std::uint32_t pc);
  bool take_postflg_write(std::uint32_t* pc, std::uint8_t* value);
  bool take_halt_request(bool* stop);
  bool patch_rom16(std::uint32_t address, std::uint16_t value);
  bool patch_rom32(std::uint32_t address, std::uint32_t value);

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
  void write8_internal(std::uint32_t address, std::uint8_t value, bool allow_trace);
  void log_io_write(std::uint32_t address, std::uint32_t value, int bits);
  void log_video_io_write(std::uint32_t address, std::uint32_t value, int bits);
  void log_io_read(std::uint32_t address, std::uint32_t value, int bits) const;
  void log_watchpoint(std::uint32_t address,
                      std::uint32_t value,
                      int bits,
                      bool write) const;
  bool rom_offset_for(std::uint32_t address, std::uint32_t* offset) const;

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
  std::vector<std::uint8_t> sram_;
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
};

} // namespace gbemu::core
