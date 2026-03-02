#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
  void set_cpsr(std::uint32_t value);
  void set_mode(std::uint32_t mode);
  void set_irq_disable(bool disabled);
  void set_spsr_for_mode(std::uint32_t mode, std::uint32_t value);
  void set_banked_sp(std::uint32_t mode, std::uint32_t value);
  void set_log_unimplemented(int limit);
  void set_log_swi(int limit);
  void serialize(std::vector<std::uint8_t>* out) const;
  bool deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error);
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
  std::uint32_t operand_reg(int index) const;
  void write_alu_result(std::uint32_t rd, std::uint32_t value, bool s);
  void switch_mode(std::uint32_t new_mode);
  void save_banked(std::uint32_t mode);
  void load_banked(std::uint32_t mode);
  std::uint32_t* spsr_for_mode(std::uint32_t mode);
  void log_unimplemented(bool thumb, std::uint32_t pc, std::uint32_t op);
  void log_swi(bool thumb, std::uint32_t pc, std::uint32_t imm);

  Registers regs_{};
  bool thumb_ = false;
  bool faulted_ = false;
  std::string fault_reason_;
  int unimplemented_count_ = 0;
  std::uint32_t spsr_ = 0;
  std::array<std::uint32_t, 5> shared_r8_12_{};
  std::array<std::uint32_t, 5> fiq_r8_12_{};
  std::uint32_t banked_r13_usr_ = 0;
  std::uint32_t banked_r14_usr_ = 0;
  std::uint32_t banked_r13_fiq_ = 0;
  std::uint32_t banked_r14_fiq_ = 0;
  std::uint32_t banked_r13_irq_ = 0;
  std::uint32_t banked_r14_irq_ = 0;
  std::uint32_t banked_r13_svc_ = 0;
  std::uint32_t banked_r14_svc_ = 0;
  std::uint32_t banked_r13_abt_ = 0;
  std::uint32_t banked_r14_abt_ = 0;
  std::uint32_t banked_r13_und_ = 0;
  std::uint32_t banked_r14_und_ = 0;
  std::uint32_t spsr_fiq_ = 0;
  std::uint32_t spsr_irq_ = 0;
  std::uint32_t spsr_svc_ = 0;
  std::uint32_t spsr_abt_ = 0;
  std::uint32_t spsr_und_ = 0;
  int log_unimplemented_limit_ = 0;
  int log_unimplemented_count_ = 0;
  int log_swi_limit_ = 0;
  int log_swi_count_ = 0;
};

} // namespace gbemu::core
