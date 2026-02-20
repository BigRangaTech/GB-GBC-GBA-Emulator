#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
  std::uint8_t last_cb_opcode() const;
  std::uint16_t last_pc() const;
  const std::string& fault_reason() const;
  void set_trace_enabled(bool enabled);
  void serialize(std::vector<std::uint8_t>* out) const;
  bool deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error);

  struct TraceEntry {
    std::uint16_t pc = 0;
    std::uint8_t opcode = 0;
    std::uint8_t cb_opcode = 0;
  };

  std::vector<TraceEntry> trace() const;

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

  std::uint16_t get_af() const;
  std::uint16_t get_bc() const;
  std::uint16_t get_de() const;
  std::uint16_t get_hl() const;
  void set_af(std::uint16_t value);
  void set_bc(std::uint16_t value);
  void set_de(std::uint16_t value);
  void set_hl(std::uint16_t value);

  bool flag(Flag f) const;
  void set_flag(Flag f, bool value);
  void set_flags(bool z, bool n, bool h, bool c);
  void fault(const std::string& reason);

  std::uint8_t read_reg(int index);
  void write_reg(int index, std::uint8_t value);

  std::uint8_t inc8(std::uint8_t value);
  std::uint8_t dec8(std::uint8_t value);
  void add_a(std::uint8_t value, bool carry);
  void sub_a(std::uint8_t value, bool carry);
  void and_a(std::uint8_t value);
  void or_a(std::uint8_t value);
  void xor_a(std::uint8_t value);
  void cp_a(std::uint8_t value);
  void add_hl(std::uint16_t value);
  bool check_cond(std::uint8_t opcode) const;

  std::uint8_t rlc(std::uint8_t value);
  std::uint8_t rrc(std::uint8_t value);
  std::uint8_t rl(std::uint8_t value);
  std::uint8_t rr(std::uint8_t value);
  std::uint8_t sla(std::uint8_t value);
  std::uint8_t sra(std::uint8_t value);
  std::uint8_t srl(std::uint8_t value);
  std::uint8_t swap(std::uint8_t value);

  bool service_interrupts();

  void op_unimplemented();
  void op_nop();
  void op_stop();
  void op_halt();
  void op_di();
  void op_ei();
  void op_cb_prefix();

  void op_ld_rr_d16();
  void op_ld_r_d8();
  void op_ld_r_r();
  void op_ld_a_d8();
  void op_ld_sp_d16();
  void op_ld_sp_hl();
  void op_ld_hl_sp_e8();
  void op_add_sp_e8();

  void op_ld_a16_sp();
  void op_ld_bc_a();
  void op_ld_de_a();
  void op_ld_a_bc();
  void op_ld_a_de();
  void op_ld_hli_a();
  void op_ld_hld_a();
  void op_ld_a_hli();
  void op_ld_a_hld();
  void op_ld_a16_a();
  void op_ld_a_a16();
  void op_ldh_a8_a();
  void op_ldh_a_a8();
  void op_ldh_c_a();
  void op_ldh_a_c();

  void op_inc_r();
  void op_dec_r();
  void op_inc_rr();
  void op_dec_rr();

  void op_add_hl_rr();

  void op_rlca();
  void op_rrca();
  void op_rla();
  void op_rra();
  void op_daa();
  void op_cpl();
  void op_scf();
  void op_ccf();

  void op_jr_e8();
  void op_jr_cc_e8();
  void op_jp_a16();
  void op_jp_hl();
  void op_jp_cc_a16();
  void op_call_a16();
  void op_call_cc_a16();
  void op_ret();
  void op_ret_cc();
  void op_reti();
  void op_rst();
  void op_push_rr();
  void op_pop_rr();

  void op_add_a_r();
  void op_adc_a_r();
  void op_sub_a_r();
  void op_sbc_a_r();
  void op_and_a_r();
  void op_or_a_r();
  void op_xor_a_r();
  void op_cp_a_r();

  void op_add_a_d8();
  void op_adc_a_d8();
  void op_sub_a_d8();
  void op_sbc_a_d8();
  void op_and_a_d8();
  void op_or_a_d8();
  void op_xor_a_d8();
  void op_cp_a_d8();

  void trace_add(std::uint16_t pc, std::uint8_t opcode);
  void trace_set_cb(std::uint8_t cb_opcode);

  Mmu* mmu_ = nullptr;
  Registers regs_{};
  bool ime_ = false;
  bool halted_ = false;
  bool stopped_ = false;
  bool faulted_ = false;
  bool ime_pending_ = false;
  bool halt_bug_ = false;
  std::uint8_t last_opcode_ = 0;
  std::uint8_t last_cb_opcode_ = 0;
  std::uint16_t last_pc_ = 0;
  int current_cycles_ = 0;
  std::string fault_reason_;
  bool trace_enabled_ = false;
  std::size_t trace_capacity_ = 64;
  std::vector<TraceEntry> trace_buffer_;
  std::size_t trace_index_ = 0;
  std::size_t trace_size_ = 0;

  std::array<Opcode, 256> ops_{};
};

} // namespace gbemu::core
