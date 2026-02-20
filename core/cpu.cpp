#include "cpu.h"

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
  halted_ = false;
  stopped_ = false;
  faulted_ = false;
  last_opcode_ = 0;
  last_pc_ = 0;
  fault_reason_.clear();
}

int Cpu::step() {
  if (faulted_ || stopped_) {
    return 0;
  }
  if (halted_) {
    return 4;
  }
  if (!mmu_) {
    fault("MMU not connected");
    return 0;
  }

  last_pc_ = regs_.pc;
  last_opcode_ = read_u8(regs_.pc);
  const Opcode& op = ops_[last_opcode_];
  regs_.pc = static_cast<std::uint16_t>(regs_.pc + op.bytes);

  if (!op.fn) {
    fault("Missing opcode handler");
    return 0;
  }

  (this->*op.fn)();
  return op.cycles;
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

std::uint16_t Cpu::last_pc() const {
  return last_pc_;
}

const std::string& Cpu::fault_reason() const {
  return fault_reason_;
}

void Cpu::init_ops() {
  for (auto& op : ops_) {
    op = {"UNIMPL", 1, 4, &Cpu::op_unimplemented};
  }

  ops_[0x00] = {"NOP", 1, 4, &Cpu::op_nop};
  ops_[0x31] = {"LD SP,d16", 3, 12, &Cpu::op_ld_sp_d16};
  ops_[0x3E] = {"LD A,d8", 2, 8, &Cpu::op_ld_a_d8};
  ops_[0x76] = {"HALT", 1, 4, &Cpu::op_halt};
  ops_[0xAF] = {"XOR A", 1, 4, &Cpu::op_xor_a};
  ops_[0xC3] = {"JP a16", 3, 16, &Cpu::op_jp_a16};
  ops_[0xEA] = {"LD (a16),A", 3, 16, &Cpu::op_ld_a16_a};
  ops_[0xF3] = {"DI", 1, 4, &Cpu::op_di};
  ops_[0xFA] = {"LD A,(a16)", 3, 16, &Cpu::op_ld_a_a16};
  ops_[0xFB] = {"EI", 1, 4, &Cpu::op_ei};
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

void Cpu::set_flags(bool z, bool n, bool h, bool c) {
  regs_.f = 0;
  if (z) regs_.f |= Z;
  if (n) regs_.f |= N;
  if (h) regs_.f |= H;
  if (c) regs_.f |= C;
}

void Cpu::fault(const std::string& reason) {
  faulted_ = true;
  fault_reason_ = reason;
}

void Cpu::op_unimplemented() {
  fault("Unimplemented opcode");
}

void Cpu::op_nop() {}

void Cpu::op_halt() {
  halted_ = true;
}

void Cpu::op_di() {
  ime_ = false;
}

void Cpu::op_ei() {
  ime_ = true;
}

void Cpu::op_ld_sp_d16() {
  regs_.sp = imm16();
}

void Cpu::op_ld_a_d8() {
  regs_.a = imm8();
}

void Cpu::op_xor_a() {
  regs_.a ^= regs_.a;
  set_flags(regs_.a == 0, false, false, false);
}

void Cpu::op_jp_a16() {
  regs_.pc = imm16();
}

void Cpu::op_ld_a16_a() {
  write_u8(imm16(), regs_.a);
}

void Cpu::op_ld_a_a16() {
  regs_.a = read_u8(imm16());
}

} // namespace gbemu::core
