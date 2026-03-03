#include "cpu.h"

#include "state_io.h"

namespace gbemu::core {

Cpu::Cpu() {
  init_ops();
}

void Cpu::connect(Mmu* mmu) {
  mmu_ = mmu;
}

void Cpu::reset() {
  regs_ = {};
  ime_ = false;
  ime_pending_ = false;
  halted_ = false;
  stopped_ = false;
  faulted_ = false;
  halt_bug_ = false;
  last_opcode_ = 0;
  last_cb_opcode_ = 0;
  last_pc_ = 0;
  current_cycles_ = 0;
  fault_reason_.clear();
  trace_index_ = 0;
  trace_size_ = 0;
}

int Cpu::step() {
  if (faulted_ || stopped_) {
    return 0;
  }
  if (!mmu_) {
    fault("MMU not connected");
    return 0;
  }

  if (halted_) {
    if (service_interrupts()) {
      return current_cycles_;
    }
    if (halted_) {
      return 4;
    }
  }

  if (service_interrupts()) {
    return current_cycles_;
  }

  last_pc_ = regs_.pc;
  last_opcode_ = read_u8(regs_.pc);
  trace_add(last_pc_, last_opcode_);
  const Opcode& op = ops_[last_opcode_];
  regs_.pc = static_cast<std::uint16_t>(regs_.pc + op.bytes);
  if (halt_bug_) {
    halt_bug_ = false;
    regs_.pc = static_cast<std::uint16_t>(regs_.pc - 1);
  }
  current_cycles_ = op.cycles;

  if (!op.fn) {
    fault("Missing opcode handler");
    return 0;
  }

  (this->*op.fn)();
  // EI becomes active after the next instruction, not immediately.
  if (ime_pending_ && last_opcode_ != 0xFB) {
    ime_ = true;
    ime_pending_ = false;
  }
  return current_cycles_;
}

const Cpu::Registers& Cpu::regs() const {
  return regs_;
}

bool Cpu::halted() const {
  return halted_;
}

bool Cpu::stopped() const {
  return stopped_;
}

bool Cpu::faulted() const {
  return faulted_;
}

std::uint8_t Cpu::last_opcode() const {
  return last_opcode_;
}

std::uint8_t Cpu::last_cb_opcode() const {
  return last_cb_opcode_;
}

std::uint16_t Cpu::last_pc() const {
  return last_pc_;
}

const std::string& Cpu::fault_reason() const {
  return fault_reason_;
}

void Cpu::set_trace_enabled(bool enabled) {
  trace_enabled_ = enabled;
  if (trace_enabled_ && trace_buffer_.empty()) {
    trace_buffer_.assign(trace_capacity_, {});
  }
  if (!trace_enabled_) {
    trace_index_ = 0;
    trace_size_ = 0;
  }
}

void Cpu::serialize(std::vector<std::uint8_t>* out) const {
  if (!out) {
    return;
  }
  state_io::write_u8(*out, regs_.a);
  state_io::write_u8(*out, regs_.f);
  state_io::write_u8(*out, regs_.b);
  state_io::write_u8(*out, regs_.c);
  state_io::write_u8(*out, regs_.d);
  state_io::write_u8(*out, regs_.e);
  state_io::write_u8(*out, regs_.h);
  state_io::write_u8(*out, regs_.l);
  state_io::write_u16(*out, regs_.sp);
  state_io::write_u16(*out, regs_.pc);
  state_io::write_bool(*out, ime_);
  state_io::write_bool(*out, ime_pending_);
  state_io::write_bool(*out, halted_);
  state_io::write_bool(*out, stopped_);
  state_io::write_bool(*out, halt_bug_);
}

bool Cpu::deserialize(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string* error) {
  std::uint8_t v8 = 0;
  std::uint16_t v16 = 0;
  bool vb = false;

  if (!state_io::read_u8(data, offset, regs_.a)) return false;
  if (!state_io::read_u8(data, offset, regs_.f)) return false;
  if (!state_io::read_u8(data, offset, regs_.b)) return false;
  if (!state_io::read_u8(data, offset, regs_.c)) return false;
  if (!state_io::read_u8(data, offset, regs_.d)) return false;
  if (!state_io::read_u8(data, offset, regs_.e)) return false;
  if (!state_io::read_u8(data, offset, regs_.h)) return false;
  if (!state_io::read_u8(data, offset, regs_.l)) return false;
  if (!state_io::read_u16(data, offset, regs_.sp)) return false;
  if (!state_io::read_u16(data, offset, regs_.pc)) return false;
  if (!state_io::read_bool(data, offset, ime_)) return false;
  if (!state_io::read_bool(data, offset, ime_pending_)) return false;
  if (!state_io::read_bool(data, offset, halted_)) return false;
  if (!state_io::read_bool(data, offset, stopped_)) return false;
  if (!state_io::read_bool(data, offset, halt_bug_)) return false;

  faulted_ = false;
  fault_reason_.clear();
  last_opcode_ = 0;
  last_cb_opcode_ = 0;
  last_pc_ = regs_.pc;
  current_cycles_ = 0;
  (void)v8;
  (void)v16;
  (void)vb;
  return true;
}

std::vector<Cpu::TraceEntry> Cpu::trace() const {
  std::vector<TraceEntry> out;
  if (!trace_enabled_ || trace_size_ == 0 || trace_buffer_.empty()) {
    return out;
  }
  out.reserve(trace_size_);
  std::size_t capacity = trace_buffer_.size();
  std::size_t start = (trace_index_ + capacity - trace_size_) % capacity;
  for (std::size_t i = 0; i < trace_size_; ++i) {
    out.push_back(trace_buffer_[(start + i) % capacity]);
  }
  return out;
}

void Cpu::init_ops() {
  for (auto& op : ops_) {
    op = {"UNIMPL", 1, 4, &Cpu::op_unimplemented};
  }

  ops_[0x00] = {"NOP", 1, 4, &Cpu::op_nop};
  ops_[0x10] = {"STOP", 2, 4, &Cpu::op_stop};
  ops_[0x76] = {"HALT", 1, 4, &Cpu::op_halt};
  ops_[0xF3] = {"DI", 1, 4, &Cpu::op_di};
  ops_[0xFB] = {"EI", 1, 4, &Cpu::op_ei};
  ops_[0xCB] = {"PREFIX", 2, 4, &Cpu::op_cb_prefix};

  ops_[0x01] = {"LD BC,d16", 3, 12, &Cpu::op_ld_rr_d16};
  ops_[0x11] = {"LD DE,d16", 3, 12, &Cpu::op_ld_rr_d16};
  ops_[0x21] = {"LD HL,d16", 3, 12, &Cpu::op_ld_rr_d16};
  ops_[0x31] = {"LD SP,d16", 3, 12, &Cpu::op_ld_sp_d16};
  ops_[0x08] = {"LD (a16),SP", 3, 20, &Cpu::op_ld_a16_sp};
  ops_[0xF9] = {"LD SP,HL", 1, 8, &Cpu::op_ld_sp_hl};
  ops_[0xF8] = {"LD HL,SP+e8", 2, 12, &Cpu::op_ld_hl_sp_e8};
  ops_[0xE8] = {"ADD SP,e8", 2, 16, &Cpu::op_add_sp_e8};

  ops_[0x06] = {"LD B,d8", 2, 8, &Cpu::op_ld_r_d8};
  ops_[0x0E] = {"LD C,d8", 2, 8, &Cpu::op_ld_r_d8};
  ops_[0x16] = {"LD D,d8", 2, 8, &Cpu::op_ld_r_d8};
  ops_[0x1E] = {"LD E,d8", 2, 8, &Cpu::op_ld_r_d8};
  ops_[0x26] = {"LD H,d8", 2, 8, &Cpu::op_ld_r_d8};
  ops_[0x2E] = {"LD L,d8", 2, 8, &Cpu::op_ld_r_d8};
  ops_[0x36] = {"LD (HL),d8", 2, 12, &Cpu::op_ld_r_d8};
  ops_[0x3E] = {"LD A,d8", 2, 8, &Cpu::op_ld_a_d8};

  for (std::uint8_t op = 0x40; op <= 0x7F; ++op) {
    if (op == 0x76) {
      continue;
    }
    ops_[op] = {"LD r,r", 1, 4, &Cpu::op_ld_r_r};
  }

  ops_[0x02] = {"LD (BC),A", 1, 8, &Cpu::op_ld_bc_a};
  ops_[0x12] = {"LD (DE),A", 1, 8, &Cpu::op_ld_de_a};
  ops_[0x0A] = {"LD A,(BC)", 1, 8, &Cpu::op_ld_a_bc};
  ops_[0x1A] = {"LD A,(DE)", 1, 8, &Cpu::op_ld_a_de};
  ops_[0x22] = {"LD (HL+),A", 1, 8, &Cpu::op_ld_hli_a};
  ops_[0x32] = {"LD (HL-),A", 1, 8, &Cpu::op_ld_hld_a};
  ops_[0x2A] = {"LD A,(HL+)", 1, 8, &Cpu::op_ld_a_hli};
  ops_[0x3A] = {"LD A,(HL-)", 1, 8, &Cpu::op_ld_a_hld};
  ops_[0xEA] = {"LD (a16),A", 3, 16, &Cpu::op_ld_a16_a};
  ops_[0xFA] = {"LD A,(a16)", 3, 16, &Cpu::op_ld_a_a16};
  ops_[0xE0] = {"LDH (a8),A", 2, 12, &Cpu::op_ldh_a8_a};
  ops_[0xF0] = {"LDH A,(a8)", 2, 12, &Cpu::op_ldh_a_a8};
  ops_[0xE2] = {"LD (C),A", 1, 8, &Cpu::op_ldh_c_a};
  ops_[0xF2] = {"LD A,(C)", 1, 8, &Cpu::op_ldh_a_c};

  ops_[0x04] = {"INC B", 1, 4, &Cpu::op_inc_r};
  ops_[0x0C] = {"INC C", 1, 4, &Cpu::op_inc_r};
  ops_[0x14] = {"INC D", 1, 4, &Cpu::op_inc_r};
  ops_[0x1C] = {"INC E", 1, 4, &Cpu::op_inc_r};
  ops_[0x24] = {"INC H", 1, 4, &Cpu::op_inc_r};
  ops_[0x2C] = {"INC L", 1, 4, &Cpu::op_inc_r};
  ops_[0x34] = {"INC (HL)", 1, 12, &Cpu::op_inc_r};
  ops_[0x3C] = {"INC A", 1, 4, &Cpu::op_inc_r};

  ops_[0x05] = {"DEC B", 1, 4, &Cpu::op_dec_r};
  ops_[0x0D] = {"DEC C", 1, 4, &Cpu::op_dec_r};
  ops_[0x15] = {"DEC D", 1, 4, &Cpu::op_dec_r};
  ops_[0x1D] = {"DEC E", 1, 4, &Cpu::op_dec_r};
  ops_[0x25] = {"DEC H", 1, 4, &Cpu::op_dec_r};
  ops_[0x2D] = {"DEC L", 1, 4, &Cpu::op_dec_r};
  ops_[0x35] = {"DEC (HL)", 1, 12, &Cpu::op_dec_r};
  ops_[0x3D] = {"DEC A", 1, 4, &Cpu::op_dec_r};

  ops_[0x03] = {"INC BC", 1, 8, &Cpu::op_inc_rr};
  ops_[0x13] = {"INC DE", 1, 8, &Cpu::op_inc_rr};
  ops_[0x23] = {"INC HL", 1, 8, &Cpu::op_inc_rr};
  ops_[0x33] = {"INC SP", 1, 8, &Cpu::op_inc_rr};
  ops_[0x0B] = {"DEC BC", 1, 8, &Cpu::op_dec_rr};
  ops_[0x1B] = {"DEC DE", 1, 8, &Cpu::op_dec_rr};
  ops_[0x2B] = {"DEC HL", 1, 8, &Cpu::op_dec_rr};
  ops_[0x3B] = {"DEC SP", 1, 8, &Cpu::op_dec_rr};

  ops_[0x09] = {"ADD HL,BC", 1, 8, &Cpu::op_add_hl_rr};
  ops_[0x19] = {"ADD HL,DE", 1, 8, &Cpu::op_add_hl_rr};
  ops_[0x29] = {"ADD HL,HL", 1, 8, &Cpu::op_add_hl_rr};
  ops_[0x39] = {"ADD HL,SP", 1, 8, &Cpu::op_add_hl_rr};

  ops_[0x07] = {"RLCA", 1, 4, &Cpu::op_rlca};
  ops_[0x0F] = {"RRCA", 1, 4, &Cpu::op_rrca};
  ops_[0x17] = {"RLA", 1, 4, &Cpu::op_rla};
  ops_[0x1F] = {"RRA", 1, 4, &Cpu::op_rra};
  ops_[0x27] = {"DAA", 1, 4, &Cpu::op_daa};
  ops_[0x2F] = {"CPL", 1, 4, &Cpu::op_cpl};
  ops_[0x37] = {"SCF", 1, 4, &Cpu::op_scf};
  ops_[0x3F] = {"CCF", 1, 4, &Cpu::op_ccf};

  ops_[0x18] = {"JR e8", 2, 12, &Cpu::op_jr_e8};
  ops_[0x20] = {"JR NZ,e8", 2, 12, &Cpu::op_jr_cc_e8};
  ops_[0x28] = {"JR Z,e8", 2, 12, &Cpu::op_jr_cc_e8};
  ops_[0x30] = {"JR NC,e8", 2, 12, &Cpu::op_jr_cc_e8};
  ops_[0x38] = {"JR C,e8", 2, 12, &Cpu::op_jr_cc_e8};
  ops_[0xC3] = {"JP a16", 3, 16, &Cpu::op_jp_a16};
  ops_[0xE9] = {"JP (HL)", 1, 4, &Cpu::op_jp_hl};
  ops_[0xC2] = {"JP NZ,a16", 3, 16, &Cpu::op_jp_cc_a16};
  ops_[0xCA] = {"JP Z,a16", 3, 16, &Cpu::op_jp_cc_a16};
  ops_[0xD2] = {"JP NC,a16", 3, 16, &Cpu::op_jp_cc_a16};
  ops_[0xDA] = {"JP C,a16", 3, 16, &Cpu::op_jp_cc_a16};

  ops_[0xCD] = {"CALL a16", 3, 24, &Cpu::op_call_a16};
  ops_[0xC4] = {"CALL NZ,a16", 3, 24, &Cpu::op_call_cc_a16};
  ops_[0xCC] = {"CALL Z,a16", 3, 24, &Cpu::op_call_cc_a16};
  ops_[0xD4] = {"CALL NC,a16", 3, 24, &Cpu::op_call_cc_a16};
  ops_[0xDC] = {"CALL C,a16", 3, 24, &Cpu::op_call_cc_a16};

  ops_[0xC9] = {"RET", 1, 16, &Cpu::op_ret};
  ops_[0xC0] = {"RET NZ", 1, 20, &Cpu::op_ret_cc};
  ops_[0xC8] = {"RET Z", 1, 20, &Cpu::op_ret_cc};
  ops_[0xD0] = {"RET NC", 1, 20, &Cpu::op_ret_cc};
  ops_[0xD8] = {"RET C", 1, 20, &Cpu::op_ret_cc};
  ops_[0xD9] = {"RETI", 1, 16, &Cpu::op_reti};

  ops_[0xC5] = {"PUSH BC", 1, 16, &Cpu::op_push_rr};
  ops_[0xD5] = {"PUSH DE", 1, 16, &Cpu::op_push_rr};
  ops_[0xE5] = {"PUSH HL", 1, 16, &Cpu::op_push_rr};
  ops_[0xF5] = {"PUSH AF", 1, 16, &Cpu::op_push_rr};

  ops_[0xC1] = {"POP BC", 1, 12, &Cpu::op_pop_rr};
  ops_[0xD1] = {"POP DE", 1, 12, &Cpu::op_pop_rr};
  ops_[0xE1] = {"POP HL", 1, 12, &Cpu::op_pop_rr};
  ops_[0xF1] = {"POP AF", 1, 12, &Cpu::op_pop_rr};

  ops_[0xC7] = {"RST 00", 1, 16, &Cpu::op_rst};
  ops_[0xCF] = {"RST 08", 1, 16, &Cpu::op_rst};
  ops_[0xD7] = {"RST 10", 1, 16, &Cpu::op_rst};
  ops_[0xDF] = {"RST 18", 1, 16, &Cpu::op_rst};
  ops_[0xE7] = {"RST 20", 1, 16, &Cpu::op_rst};
  ops_[0xEF] = {"RST 28", 1, 16, &Cpu::op_rst};
  ops_[0xF7] = {"RST 30", 1, 16, &Cpu::op_rst};
  ops_[0xFF] = {"RST 38", 1, 16, &Cpu::op_rst};

  for (std::uint8_t op = 0x80; op <= 0x87; ++op) {
    ops_[op] = {"ADD A,r", 1, 4, &Cpu::op_add_a_r};
  }
  for (std::uint8_t op = 0x88; op <= 0x8F; ++op) {
    ops_[op] = {"ADC A,r", 1, 4, &Cpu::op_adc_a_r};
  }
  for (std::uint8_t op = 0x90; op <= 0x97; ++op) {
    ops_[op] = {"SUB r", 1, 4, &Cpu::op_sub_a_r};
  }
  for (std::uint8_t op = 0x98; op <= 0x9F; ++op) {
    ops_[op] = {"SBC A,r", 1, 4, &Cpu::op_sbc_a_r};
  }
  for (std::uint8_t op = 0xA0; op <= 0xA7; ++op) {
    ops_[op] = {"AND r", 1, 4, &Cpu::op_and_a_r};
  }
  for (std::uint8_t op = 0xA8; op <= 0xAF; ++op) {
    ops_[op] = {"XOR r", 1, 4, &Cpu::op_xor_a_r};
  }
  for (std::uint8_t op = 0xB0; op <= 0xB7; ++op) {
    ops_[op] = {"OR r", 1, 4, &Cpu::op_or_a_r};
  }
  for (std::uint8_t op = 0xB8; op <= 0xBF; ++op) {
    ops_[op] = {"CP r", 1, 4, &Cpu::op_cp_a_r};
  }

  ops_[0xC6] = {"ADD A,d8", 2, 8, &Cpu::op_add_a_d8};
  ops_[0xCE] = {"ADC A,d8", 2, 8, &Cpu::op_adc_a_d8};
  ops_[0xD6] = {"SUB d8", 2, 8, &Cpu::op_sub_a_d8};
  ops_[0xDE] = {"SBC A,d8", 2, 8, &Cpu::op_sbc_a_d8};
  ops_[0xE6] = {"AND d8", 2, 8, &Cpu::op_and_a_d8};
  ops_[0xEE] = {"XOR d8", 2, 8, &Cpu::op_xor_a_d8};
  ops_[0xF6] = {"OR d8", 2, 8, &Cpu::op_or_a_d8};
  ops_[0xFE] = {"CP d8", 2, 8, &Cpu::op_cp_a_d8};
}

std::uint8_t Cpu::read_u8(std::uint16_t address) {
  if (!mmu_) {
    fault("MMU not connected");
    return 0xFF;
  }
  return mmu_->read_u8(address);
}

std::uint16_t Cpu::read_u16(std::uint16_t address) {
  std::uint8_t lo = read_u8(address);
  std::uint8_t hi = read_u8(static_cast<std::uint16_t>(address + 1));
  return static_cast<std::uint16_t>(lo | (hi << 8));
}

void Cpu::write_u8(std::uint16_t address, std::uint8_t value) {
  if (!mmu_) {
    fault("MMU not connected");
    return;
  }
  mmu_->write_u8(address, value);
}

void Cpu::push_u16(std::uint16_t value) {
  regs_.sp = static_cast<std::uint16_t>(regs_.sp - 1);
  write_u8(regs_.sp, static_cast<std::uint8_t>((value >> 8) & 0xFF));
  regs_.sp = static_cast<std::uint16_t>(regs_.sp - 1);
  write_u8(regs_.sp, static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t Cpu::pop_u16() {
  std::uint8_t lo = read_u8(regs_.sp);
  regs_.sp = static_cast<std::uint16_t>(regs_.sp + 1);
  std::uint8_t hi = read_u8(regs_.sp);
  regs_.sp = static_cast<std::uint16_t>(regs_.sp + 1);
  return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint8_t Cpu::imm8() const {
  return mmu_ ? mmu_->read_u8(static_cast<std::uint16_t>(last_pc_ + 1)) : 0xFF;
}

std::uint16_t Cpu::imm16() const {
  if (!mmu_) {
    return 0xFFFF;
  }
  std::uint8_t lo = mmu_->read_u8(static_cast<std::uint16_t>(last_pc_ + 1));
  std::uint8_t hi = mmu_->read_u8(static_cast<std::uint16_t>(last_pc_ + 2));
  return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint16_t Cpu::get_af() const {
  return static_cast<std::uint16_t>((regs_.a << 8) | (regs_.f & 0xF0));
}

std::uint16_t Cpu::get_bc() const {
  return static_cast<std::uint16_t>((regs_.b << 8) | regs_.c);
}

std::uint16_t Cpu::get_de() const {
  return static_cast<std::uint16_t>((regs_.d << 8) | regs_.e);
}

std::uint16_t Cpu::get_hl() const {
  return static_cast<std::uint16_t>((regs_.h << 8) | regs_.l);
}

void Cpu::set_af(std::uint16_t value) {
  regs_.a = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  regs_.f = static_cast<std::uint8_t>(value & 0xF0);
}

void Cpu::set_bc(std::uint16_t value) {
  regs_.b = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  regs_.c = static_cast<std::uint8_t>(value & 0xFF);
}

void Cpu::set_de(std::uint16_t value) {
  regs_.d = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  regs_.e = static_cast<std::uint8_t>(value & 0xFF);
}

void Cpu::set_hl(std::uint16_t value) {
  regs_.h = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  regs_.l = static_cast<std::uint8_t>(value & 0xFF);
}

bool Cpu::flag(Flag f) const {
  return (regs_.f & f) != 0;
}

void Cpu::set_flag(Flag f, bool value) {
  if (value) {
    regs_.f |= f;
  } else {
    regs_.f &= static_cast<std::uint8_t>(~f);
  }
  regs_.f &= 0xF0;
}

void Cpu::set_flags(bool z, bool n, bool h, bool c) {
  regs_.f = 0;
  if (z) regs_.f |= Z;
  if (n) regs_.f |= N;
  if (h) regs_.f |= H;
  if (c) regs_.f |= C;
  regs_.f &= 0xF0;
}

void Cpu::fault(const std::string& reason) {
  faulted_ = true;
  fault_reason_ = reason;
}

std::uint8_t Cpu::read_reg(int index) {
  switch (index) {
    case 0: return regs_.b;
    case 1: return regs_.c;
    case 2: return regs_.d;
    case 3: return regs_.e;
    case 4: return regs_.h;
    case 5: return regs_.l;
    case 6: return read_u8(get_hl());
    case 7: return regs_.a;
    default: return 0xFF;
  }
}

void Cpu::write_reg(int index, std::uint8_t value) {
  switch (index) {
    case 0: regs_.b = value; break;
    case 1: regs_.c = value; break;
    case 2: regs_.d = value; break;
    case 3: regs_.e = value; break;
    case 4: regs_.h = value; break;
    case 5: regs_.l = value; break;
    case 6: write_u8(get_hl(), value); break;
    case 7: regs_.a = value; break;
    default: break;
  }
}

std::uint8_t Cpu::inc8(std::uint8_t value) {
  std::uint8_t result = static_cast<std::uint8_t>(value + 1);
  set_flag(Z, result == 0);
  set_flag(N, false);
  set_flag(H, (value & 0x0F) == 0x0F);
  return result;
}

std::uint8_t Cpu::dec8(std::uint8_t value) {
  std::uint8_t result = static_cast<std::uint8_t>(value - 1);
  set_flag(Z, result == 0);
  set_flag(N, true);
  set_flag(H, (value & 0x0F) == 0x00);
  return result;
}

void Cpu::add_a(std::uint8_t value, bool carry) {
  std::uint16_t c = carry && flag(C) ? 1 : 0;
  std::uint16_t sum = static_cast<std::uint16_t>(regs_.a) + value + c;
  set_flag(Z, (sum & 0xFF) == 0);
  set_flag(N, false);
  set_flag(H, ((regs_.a & 0x0F) + (value & 0x0F) + c) > 0x0F);
  set_flag(C, sum > 0xFF);
  regs_.a = static_cast<std::uint8_t>(sum & 0xFF);
}

void Cpu::sub_a(std::uint8_t value, bool carry) {
  std::uint16_t c = carry && flag(C) ? 1 : 0;
  std::uint16_t diff = static_cast<std::uint16_t>(regs_.a) - value - c;
  set_flag(Z, (diff & 0xFF) == 0);
  set_flag(N, true);
  set_flag(H, (regs_.a & 0x0F) < ((value & 0x0F) + c));
  set_flag(C, regs_.a < static_cast<std::uint16_t>(value + c));
  regs_.a = static_cast<std::uint8_t>(diff & 0xFF);
}

void Cpu::and_a(std::uint8_t value) {
  regs_.a = static_cast<std::uint8_t>(regs_.a & value);
  set_flags(regs_.a == 0, false, true, false);
}

void Cpu::or_a(std::uint8_t value) {
  regs_.a = static_cast<std::uint8_t>(regs_.a | value);
  set_flags(regs_.a == 0, false, false, false);
}

void Cpu::xor_a(std::uint8_t value) {
  regs_.a = static_cast<std::uint8_t>(regs_.a ^ value);
  set_flags(regs_.a == 0, false, false, false);
}

void Cpu::cp_a(std::uint8_t value) {
  std::uint16_t diff = static_cast<std::uint16_t>(regs_.a) - value;
  set_flag(Z, (diff & 0xFF) == 0);
  set_flag(N, true);
  set_flag(H, (regs_.a & 0x0F) < (value & 0x0F));
  set_flag(C, regs_.a < value);
}

void Cpu::add_hl(std::uint16_t value) {
  std::uint32_t sum = static_cast<std::uint32_t>(get_hl()) + value;
  set_flag(N, false);
  set_flag(H, ((get_hl() & 0x0FFF) + (value & 0x0FFF)) > 0x0FFF);
  set_flag(C, sum > 0xFFFF);
  set_hl(static_cast<std::uint16_t>(sum & 0xFFFF));
}

bool Cpu::check_cond(std::uint8_t opcode) const {
  int code = (opcode >> 3) & 0x03;
  switch (code) {
    case 0: return !flag(Z);
    case 1: return flag(Z);
    case 2: return !flag(C);
    case 3: return flag(C);
    default: return false;
  }
}

std::uint8_t Cpu::rlc(std::uint8_t value) {
  std::uint8_t out = static_cast<std::uint8_t>((value << 1) | (value >> 7));
  set_flag(Z, out == 0);
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, (value & 0x80) != 0);
  return out;
}

std::uint8_t Cpu::rrc(std::uint8_t value) {
  std::uint8_t out = static_cast<std::uint8_t>((value >> 1) | (value << 7));
  set_flag(Z, out == 0);
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, (value & 0x01) != 0);
  return out;
}

std::uint8_t Cpu::rl(std::uint8_t value) {
  std::uint8_t carry = flag(C) ? 1 : 0;
  std::uint8_t out = static_cast<std::uint8_t>((value << 1) | carry);
  set_flag(Z, out == 0);
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, (value & 0x80) != 0);
  return out;
}

std::uint8_t Cpu::rr(std::uint8_t value) {
  std::uint8_t carry = flag(C) ? 0x80 : 0x00;
  std::uint8_t out = static_cast<std::uint8_t>((value >> 1) | carry);
  set_flag(Z, out == 0);
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, (value & 0x01) != 0);
  return out;
}

std::uint8_t Cpu::sla(std::uint8_t value) {
  std::uint8_t out = static_cast<std::uint8_t>(value << 1);
  set_flag(Z, out == 0);
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, (value & 0x80) != 0);
  return out;
}

std::uint8_t Cpu::sra(std::uint8_t value) {
  std::uint8_t out = static_cast<std::uint8_t>((value >> 1) | (value & 0x80));
  set_flag(Z, out == 0);
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, (value & 0x01) != 0);
  return out;
}

std::uint8_t Cpu::srl(std::uint8_t value) {
  std::uint8_t out = static_cast<std::uint8_t>(value >> 1);
  set_flag(Z, out == 0);
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, (value & 0x01) != 0);
  return out;
}

std::uint8_t Cpu::swap(std::uint8_t value) {
  std::uint8_t out = static_cast<std::uint8_t>((value << 4) | (value >> 4));
  set_flags(out == 0, false, false, false);
  return out;
}

void Cpu::op_unimplemented() {
  fault("Unimplemented opcode");
}

void Cpu::op_nop() {}

void Cpu::op_stop() {
  if (mmu_ && mmu_->handle_stop()) {
    return;
  }
  stopped_ = true;
}

void Cpu::op_halt() {
  if (mmu_) {
    std::uint8_t pending = static_cast<std::uint8_t>(mmu_->interrupt_enable() &
                                                    mmu_->interrupt_flags() & 0x1F);
    if (!ime_ && pending != 0) {
      halt_bug_ = true;
      return;
    }
  }
  halted_ = true;
}

void Cpu::op_di() {
  ime_ = false;
  ime_pending_ = false;
}

void Cpu::op_ei() {
  ime_pending_ = true;
}

void Cpu::op_cb_prefix() {
  last_cb_opcode_ = imm8();
  std::uint8_t opcode = last_cb_opcode_;
  trace_set_cb(opcode);
  int reg = opcode & 0x07;
  int bit = (opcode >> 3) & 0x07;
  bool hl = (reg == 6);

  if (opcode < 0x08) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, rlc(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x10) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, rrc(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x18) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, rl(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x20) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, rr(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x28) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, sla(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x30) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, sra(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x38) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, swap(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x40) {
    std::uint8_t value = read_reg(reg);
    write_reg(reg, srl(value));
    current_cycles_ = hl ? 16 : 8;
    return;
  }
  if (opcode < 0x80) {
    std::uint8_t value = read_reg(reg);
    bool zero = ((value >> bit) & 0x01) == 0;
    set_flag(Z, zero);
    set_flag(N, false);
    set_flag(H, true);
    // BIT b,(HL) uses 12 cycles; other CB (HL) ops are 16.
    current_cycles_ = hl ? 12 : 8;
    return;
  }
  if (opcode < 0xC0) {
    std::uint8_t value = read_reg(reg);
    value = static_cast<std::uint8_t>(value & ~(1u << bit));
    write_reg(reg, value);
    current_cycles_ = hl ? 16 : 8;
    return;
  }

  std::uint8_t value = read_reg(reg);
  value = static_cast<std::uint8_t>(value | (1u << bit));
  write_reg(reg, value);
  current_cycles_ = hl ? 16 : 8;
}

void Cpu::op_ld_rr_d16() {
  std::uint16_t value = imm16();
  switch (last_opcode_) {
    case 0x01: set_bc(value); break;
    case 0x11: set_de(value); break;
    case 0x21: set_hl(value); break;
    case 0x31: regs_.sp = value; break;
    default: break;
  }
}

void Cpu::op_ld_r_d8() {
  int reg = (last_opcode_ >> 3) & 0x07;
  std::uint8_t value = imm8();
  write_reg(reg, value);
  current_cycles_ = (reg == 6) ? 12 : 8;
}

void Cpu::op_ld_r_r() {
  int dest = (last_opcode_ >> 3) & 0x07;
  int src = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(src);
  write_reg(dest, value);
  current_cycles_ = (dest == 6 || src == 6) ? 8 : 4;
}

void Cpu::op_ld_a_d8() {
  regs_.a = imm8();
}

void Cpu::op_ld_sp_d16() {
  regs_.sp = imm16();
}

void Cpu::op_ld_sp_hl() {
  regs_.sp = get_hl();
}

void Cpu::op_ld_hl_sp_e8() {
  std::int8_t offset = static_cast<std::int8_t>(imm8());
  std::uint16_t sp = regs_.sp;
  std::uint16_t result = static_cast<std::uint16_t>(sp + offset);
  set_flag(Z, false);
  set_flag(N, false);
  set_flag(H, ((sp & 0x0F) + (static_cast<std::uint16_t>(offset) & 0x0F)) > 0x0F);
  set_flag(C, ((sp & 0xFF) + (static_cast<std::uint16_t>(offset) & 0xFF)) > 0xFF);
  set_hl(result);
}

void Cpu::op_add_sp_e8() {
  std::int8_t offset = static_cast<std::int8_t>(imm8());
  std::uint16_t sp = regs_.sp;
  std::uint16_t result = static_cast<std::uint16_t>(sp + offset);
  set_flag(Z, false);
  set_flag(N, false);
  set_flag(H, ((sp & 0x0F) + (static_cast<std::uint16_t>(offset) & 0x0F)) > 0x0F);
  set_flag(C, ((sp & 0xFF) + (static_cast<std::uint16_t>(offset) & 0xFF)) > 0xFF);
  regs_.sp = result;
}

void Cpu::op_ld_a16_sp() {
  std::uint16_t addr = imm16();
  write_u8(addr, static_cast<std::uint8_t>(regs_.sp & 0xFF));
  write_u8(static_cast<std::uint16_t>(addr + 1), static_cast<std::uint8_t>((regs_.sp >> 8) & 0xFF));
}

void Cpu::op_ld_bc_a() {
  write_u8(get_bc(), regs_.a);
}

void Cpu::op_ld_de_a() {
  write_u8(get_de(), regs_.a);
}

void Cpu::op_ld_a_bc() {
  regs_.a = read_u8(get_bc());
}

void Cpu::op_ld_a_de() {
  regs_.a = read_u8(get_de());
}

void Cpu::op_ld_hli_a() {
  write_u8(get_hl(), regs_.a);
  set_hl(static_cast<std::uint16_t>(get_hl() + 1));
}

void Cpu::op_ld_hld_a() {
  write_u8(get_hl(), regs_.a);
  set_hl(static_cast<std::uint16_t>(get_hl() - 1));
}

void Cpu::op_ld_a_hli() {
  regs_.a = read_u8(get_hl());
  set_hl(static_cast<std::uint16_t>(get_hl() + 1));
}

void Cpu::op_ld_a_hld() {
  regs_.a = read_u8(get_hl());
  set_hl(static_cast<std::uint16_t>(get_hl() - 1));
}

void Cpu::op_ld_a16_a() {
  write_u8(imm16(), regs_.a);
}

void Cpu::op_ld_a_a16() {
  regs_.a = read_u8(imm16());
}

void Cpu::op_ldh_a8_a() {
  write_u8(static_cast<std::uint16_t>(0xFF00 + imm8()), regs_.a);
}

void Cpu::op_ldh_a_a8() {
  regs_.a = read_u8(static_cast<std::uint16_t>(0xFF00 + imm8()));
}

void Cpu::op_ldh_c_a() {
  write_u8(static_cast<std::uint16_t>(0xFF00 + regs_.c), regs_.a);
}

void Cpu::op_ldh_a_c() {
  regs_.a = read_u8(static_cast<std::uint16_t>(0xFF00 + regs_.c));
}

void Cpu::op_inc_r() {
  int reg = (last_opcode_ >> 3) & 0x07;
  std::uint8_t value = read_reg(reg);
  value = inc8(value);
  write_reg(reg, value);
  current_cycles_ = (reg == 6) ? 12 : 4;
}

void Cpu::op_dec_r() {
  int reg = (last_opcode_ >> 3) & 0x07;
  std::uint8_t value = read_reg(reg);
  value = dec8(value);
  write_reg(reg, value);
  current_cycles_ = (reg == 6) ? 12 : 4;
}

void Cpu::op_inc_rr() {
  switch (last_opcode_) {
    case 0x03: set_bc(static_cast<std::uint16_t>(get_bc() + 1)); break;
    case 0x13: set_de(static_cast<std::uint16_t>(get_de() + 1)); break;
    case 0x23: set_hl(static_cast<std::uint16_t>(get_hl() + 1)); break;
    case 0x33: regs_.sp = static_cast<std::uint16_t>(regs_.sp + 1); break;
    default: break;
  }
}

void Cpu::op_dec_rr() {
  switch (last_opcode_) {
    case 0x0B: set_bc(static_cast<std::uint16_t>(get_bc() - 1)); break;
    case 0x1B: set_de(static_cast<std::uint16_t>(get_de() - 1)); break;
    case 0x2B: set_hl(static_cast<std::uint16_t>(get_hl() - 1)); break;
    case 0x3B: regs_.sp = static_cast<std::uint16_t>(regs_.sp - 1); break;
    default: break;
  }
}

void Cpu::op_add_hl_rr() {
  switch (last_opcode_) {
    case 0x09: add_hl(get_bc()); break;
    case 0x19: add_hl(get_de()); break;
    case 0x29: add_hl(get_hl()); break;
    case 0x39: add_hl(regs_.sp); break;
    default: break;
  }
}

void Cpu::op_rlca() {
  std::uint8_t carry = static_cast<std::uint8_t>((regs_.a >> 7) & 0x01);
  regs_.a = static_cast<std::uint8_t>((regs_.a << 1) | carry);
  set_flags(false, false, false, carry != 0);
}

void Cpu::op_rrca() {
  std::uint8_t carry = static_cast<std::uint8_t>(regs_.a & 0x01);
  regs_.a = static_cast<std::uint8_t>((regs_.a >> 1) | (carry << 7));
  set_flags(false, false, false, carry != 0);
}

void Cpu::op_rla() {
  std::uint8_t carry_in = flag(C) ? 1 : 0;
  std::uint8_t carry_out = static_cast<std::uint8_t>((regs_.a >> 7) & 0x01);
  regs_.a = static_cast<std::uint8_t>((regs_.a << 1) | carry_in);
  set_flags(false, false, false, carry_out != 0);
}

void Cpu::op_rra() {
  std::uint8_t carry_in = flag(C) ? 0x80 : 0x00;
  std::uint8_t carry_out = static_cast<std::uint8_t>(regs_.a & 0x01);
  regs_.a = static_cast<std::uint8_t>((regs_.a >> 1) | carry_in);
  set_flags(false, false, false, carry_out != 0);
}

void Cpu::op_daa() {
  std::uint8_t a = regs_.a;
  bool n = flag(N);
  bool c = flag(C);

  if (!n) {
    if (flag(H) || (a & 0x0F) > 0x09) {
      a = static_cast<std::uint8_t>(a + 0x06);
    }
    if (c || a > 0x9F) {
      a = static_cast<std::uint8_t>(a + 0x60);
      c = true;
    }
  } else {
    if (flag(H)) {
      a = static_cast<std::uint8_t>(a - 0x06);
    }
    if (c) {
      a = static_cast<std::uint8_t>(a - 0x60);
    }
  }

  regs_.a = a;
  set_flag(Z, regs_.a == 0);
  set_flag(H, false);
  set_flag(C, c);
}

void Cpu::op_cpl() {
  regs_.a = static_cast<std::uint8_t>(~regs_.a);
  set_flag(N, true);
  set_flag(H, true);
}

void Cpu::op_scf() {
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, true);
}

void Cpu::op_ccf() {
  set_flag(N, false);
  set_flag(H, false);
  set_flag(C, !flag(C));
}

void Cpu::op_jr_e8() {
  std::int8_t offset = static_cast<std::int8_t>(imm8());
  regs_.pc = static_cast<std::uint16_t>(regs_.pc + offset);
}

void Cpu::op_jr_cc_e8() {
  if (check_cond(last_opcode_)) {
    std::int8_t offset = static_cast<std::int8_t>(imm8());
    regs_.pc = static_cast<std::uint16_t>(regs_.pc + offset);
    current_cycles_ = 12;
  } else {
    current_cycles_ = 8;
  }
}

void Cpu::op_jp_a16() {
  regs_.pc = imm16();
}

void Cpu::op_jp_hl() {
  regs_.pc = get_hl();
}

void Cpu::op_jp_cc_a16() {
  if (check_cond(last_opcode_)) {
    regs_.pc = imm16();
    current_cycles_ = 16;
  } else {
    current_cycles_ = 12;
  }
}

void Cpu::op_call_a16() {
  std::uint16_t addr = imm16();
  push_u16(regs_.pc);
  regs_.pc = addr;
}

void Cpu::op_call_cc_a16() {
  if (check_cond(last_opcode_)) {
    std::uint16_t addr = imm16();
    push_u16(regs_.pc);
    regs_.pc = addr;
    current_cycles_ = 24;
  } else {
    current_cycles_ = 12;
  }
}

void Cpu::op_ret() {
  regs_.pc = pop_u16();
}

void Cpu::op_ret_cc() {
  if (check_cond(last_opcode_)) {
    regs_.pc = pop_u16();
    current_cycles_ = 20;
  } else {
    current_cycles_ = 8;
  }
}

void Cpu::op_reti() {
  regs_.pc = pop_u16();
  ime_ = true;
  ime_pending_ = false;
}

void Cpu::op_rst() {
  std::uint16_t addr = static_cast<std::uint16_t>(last_opcode_ & 0x38);
  push_u16(regs_.pc);
  regs_.pc = addr;
}

void Cpu::op_push_rr() {
  switch (last_opcode_) {
    case 0xC5: push_u16(get_bc()); break;
    case 0xD5: push_u16(get_de()); break;
    case 0xE5: push_u16(get_hl()); break;
    case 0xF5: push_u16(get_af()); break;
    default: break;
  }
}

void Cpu::op_pop_rr() {
  switch (last_opcode_) {
    case 0xC1: set_bc(pop_u16()); break;
    case 0xD1: set_de(pop_u16()); break;
    case 0xE1: set_hl(pop_u16()); break;
    case 0xF1: set_af(pop_u16()); break;
    default: break;
  }
}

void Cpu::op_add_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  add_a(value, false);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_adc_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  add_a(value, true);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_sub_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  sub_a(value, false);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_sbc_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  sub_a(value, true);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_and_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  and_a(value);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_or_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  or_a(value);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_xor_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  xor_a(value);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_cp_a_r() {
  int reg = last_opcode_ & 0x07;
  std::uint8_t value = read_reg(reg);
  cp_a(value);
  current_cycles_ = (reg == 6) ? 8 : 4;
}

void Cpu::op_add_a_d8() {
  add_a(imm8(), false);
}

void Cpu::op_adc_a_d8() {
  add_a(imm8(), true);
}

void Cpu::op_sub_a_d8() {
  sub_a(imm8(), false);
}

void Cpu::op_sbc_a_d8() {
  sub_a(imm8(), true);
}

void Cpu::op_and_a_d8() {
  and_a(imm8());
}

void Cpu::op_or_a_d8() {
  or_a(imm8());
}

void Cpu::op_xor_a_d8() {
  xor_a(imm8());
}

void Cpu::op_cp_a_d8() {
  cp_a(imm8());
}

void Cpu::trace_add(std::uint16_t pc, std::uint8_t opcode) {
  if (!trace_enabled_) {
    return;
  }
  if (trace_buffer_.empty()) {
    trace_buffer_.assign(trace_capacity_, {});
  }
  trace_buffer_[trace_index_] = {pc, opcode, 0};
  trace_index_ = (trace_index_ + 1) % trace_buffer_.size();
  if (trace_size_ < trace_buffer_.size()) {
    ++trace_size_;
  }
}

void Cpu::trace_set_cb(std::uint8_t cb_opcode) {
  if (!trace_enabled_ || trace_size_ == 0 || trace_buffer_.empty()) {
    return;
  }
  std::size_t last = (trace_index_ + trace_buffer_.size() - 1) % trace_buffer_.size();
  trace_buffer_[last].cb_opcode = cb_opcode;
}

bool Cpu::service_interrupts() {
  if (!mmu_) {
    return false;
  }

  std::uint8_t ie = mmu_->interrupt_enable();
  std::uint8_t iflags = mmu_->interrupt_flags();
  std::uint8_t pending = static_cast<std::uint8_t>(ie & iflags & 0x1F);

  if (pending == 0) {
    if (!ime_ && halted_ && (iflags & 0x1F) != 0) {
      halted_ = false;
    }
    return false;
  }

  if (!ime_) {
    if (halted_) {
      halted_ = false;
    }
    return false;
  }

  halted_ = false;
  ime_ = false;

  std::uint16_t vector = 0x40;
  std::uint8_t bit = 0;
  for (; bit < 5; ++bit) {
    if (pending & (1u << bit)) {
      vector = static_cast<std::uint16_t>(0x40 + 8 * bit);
      break;
    }
  }

  mmu_->set_interrupt_flags(static_cast<std::uint8_t>(iflags & ~(1u << bit)));
  push_u16(regs_.pc);
  regs_.pc = vector;
  current_cycles_ = 20;
  return true;
}

} // namespace gbemu::core
