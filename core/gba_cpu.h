#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace gbemu::core {

class GbaBus;

class GbaCpu {
 public:
  struct Registers {
    std::array<std::uint32_t, 16> r{};
    std::uint32_t cpsr = 0;
  };

  GbaCpu();

  void reset();
  int step(GbaBus* bus);
  void set_pc(std::uint32_t pc);
  void set_thumb(bool thumb);
  std::uint32_t reg(int index) const;
  void set_reg(int index, std::uint32_t value);
  std::uint32_t cpsr() const { return regs_.cpsr; }
  void set_cpsr(std::uint32_t value) { regs_.cpsr = value; }
  void set_mode(std::uint32_t mode);
  void set_irq_disable(bool disabled);
  int unimplemented_count() const { return unimplemented_count_; }
  void clear_unimplemented_count() { unimplemented_count_ = 0; }

  const Registers& regs() const { return regs_; }
  std::uint32_t pc() const { return regs_.r[15]; }
  bool thumb() const { return thumb_; }

  bool faulted() const { return faulted_; }
  const std::string& fault_reason() const { return fault_reason_; }

 private:
  void fault(const std::string& reason);
  bool cond_passed(std::uint8_t cond) const;
  std::uint32_t rot_imm(std::uint32_t imm8, std::uint32_t rot) const;
  void set_flags_nz(std::uint32_t result);
  void set_flags_add(std::uint32_t a, std::uint32_t b, std::uint32_t result);
  void set_flags_sub(std::uint32_t a, std::uint32_t b, std::uint32_t result);
  std::uint32_t get_flag_mask(std::uint32_t mask) const;
  void set_flag_mask(std::uint32_t mask, bool value);

  Registers regs_{};
  bool thumb_ = false;
  bool faulted_ = false;
  std::string fault_reason_;
  int unimplemented_count_ = 0;
};

} // namespace gbemu::core
