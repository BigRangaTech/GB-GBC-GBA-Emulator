#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "mmu.h"

namespace gbemu::core {

class Cpu {
 public:
  struct Registers {
    std::uint8_t a = 0;
    std::uint8_t f = 0;
    std::uint8_t b = 0;
    std::uint8_t c = 0;
    std::uint8_t d = 0;
    std::uint8_t e = 0;
    std::uint8_t h = 0;
    std::uint8_t l = 0;
    std::uint16_t sp = 0;
    std::uint16_t pc = 0;
  };

  Cpu();

  void connect(Mmu* mmu);
  void reset();

  int step();

  const Registers& regs() const;
  bool halted() const;
  bool stopped() const;
  bool faulted() const;
  std::uint8_t last_opcode() const;
  std::uint16_t last_pc() const;
  const std::string& fault_reason() const;

 private:
  using OpFn = void (Cpu::*)();

  struct Opcode {
    const char* name = "";
    std::uint8_t bytes = 1;
    std::uint8_t cycles = 4;
    OpFn fn = nullptr;
  };

  enum Flag : std::uint8_t {
    Z = 0x80,
    N = 0x40,
    H = 0x20,
    C = 0x10,
  };

  void init_ops();

  std::uint8_t read_u8(std::uint16_t address);
  std::uint16_t read_u16(std::uint16_t address);
  void write_u8(std::uint16_t address, std::uint8_t value);
  void push_u16(std::uint16_t value);
  std::uint16_t pop_u16();

  std::uint8_t imm8() const;
  std::uint16_t imm16() const;

  void set_flags(bool z, bool n, bool h, bool c);
  void fault(const std::string& reason);

  void op_unimplemented();
  void op_nop();
  void op_halt();
  void op_di();
  void op_ei();
  void op_ld_sp_d16();
  void op_ld_a_d8();
  void op_xor_a();
  void op_jp_a16();
  void op_ld_a16_a();
  void op_ld_a_a16();

  Mmu* mmu_ = nullptr;
  Registers regs_{};
  bool ime_ = false;
  bool halted_ = false;
  bool stopped_ = false;
  bool faulted_ = false;
  std::uint8_t last_opcode_ = 0;
  std::uint16_t last_pc_ = 0;
  std::string fault_reason_;

  std::array<Opcode, 256> ops_{};
};

} // namespace gbemu::core
