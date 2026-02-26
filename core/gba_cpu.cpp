#include "gba_cpu.h"

#include "gba_bus.h"

#include <bit>
#include <iomanip>
#include <iostream>
#include <limits>

namespace gbemu::core {

namespace {

struct AddResult {
  std::uint32_t value = 0;
  bool carry = false;
  bool overflow = false;
};

AddResult add_with_carry(std::uint32_t a, std::uint32_t b, bool carry_in) {
  std::uint64_t wide = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b) +
                       (carry_in ? 1u : 0u);
  std::uint32_t result = static_cast<std::uint32_t>(wide);
  bool carry = wide > 0xFFFFFFFFu;
  std::int64_t signed_sum = static_cast<std::int64_t>(static_cast<std::int32_t>(a)) +
                            static_cast<std::int64_t>(static_cast<std::int32_t>(b)) +
                            (carry_in ? 1 : 0);
  bool overflow = signed_sum > std::numeric_limits<std::int32_t>::max() ||
                  signed_sum < std::numeric_limits<std::int32_t>::min();
  return {result, carry, overflow};
}

struct ShiftResult {
  std::uint32_t value = 0;
  bool carry = false;
};

ShiftResult shift_value(std::uint32_t value,
                        std::uint32_t type,
                        std::uint32_t amount,
                        bool carry_in,
                        bool imm) {
  ShiftResult out{value, carry_in};
  if (!imm && amount == 0) {
    return out;
  }
  if (imm && amount == 0) {
    if (type == 0) {
      return out;
    }
    if (type == 1) {
      out.carry = (value & 0x80000000u) != 0;
      out.value = 0;
      return out;
    }
    if (type == 2) {
      out.carry = (value & 0x80000000u) != 0;
      out.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
      return out;
    }
    out.carry = (value & 0x1u) != 0;
    out.value = (carry_in ? 0x80000000u : 0u) | (value >> 1);
    return out;
  }

  std::uint32_t shift = amount & 0xFF;
  if (shift == 0) {
    return out;
  }
  switch (type) {
    case 0: {
      if (shift >= 32) {
        out.carry = (shift == 32) ? ((value & 0x1u) != 0) : false;
        out.value = 0;
      } else {
        out.carry = ((value >> (32 - shift)) & 1u) != 0;
        out.value = value << shift;
      }
      break;
    }
    case 1: {
      if (shift >= 32) {
        out.carry = (shift == 32) ? ((value >> 31) & 1u) != 0 : false;
        out.value = 0;
      } else {
        out.carry = ((value >> (shift - 1)) & 1u) != 0;
        out.value = value >> shift;
      }
      break;
    }
    case 2: {
      if (shift >= 32) {
        out.carry = (value & 0x80000000u) != 0;
        out.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
      } else {
        out.carry = ((value >> (shift - 1)) & 1u) != 0;
        out.value = static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> shift);
      }
      break;
    }
    case 3: {
      if (shift == 0) {
        return out;
      }
      std::uint32_t rot = shift & 31u;
      if (rot == 0) {
        out.carry = (value & 0x80000000u) != 0;
        out.value = value;
      } else {
        out.value = (value >> rot) | (value << (32 - rot));
        out.carry = (out.value & 0x80000000u) != 0;
      }
      break;
    }
    default:
      break;
  }
  return out;
}

} // namespace

GbaCpu::GbaCpu() = default;

void GbaCpu::reset() {
  regs_ = Registers{};
  regs_.r[15] = 0x00000000u;
  regs_.cpsr = 0x000000D3u;
  thumb_ = false;
  faulted_ = false;
  fault_reason_.clear();
  unimplemented_count_ = 0;
  spsr_ = 0;
  shared_r8_12_.fill(0);
  fiq_r8_12_.fill(0);
  banked_r13_usr_ = 0;
  banked_r14_usr_ = 0;
  banked_r13_fiq_ = 0;
  banked_r14_fiq_ = 0;
  banked_r13_irq_ = 0;
  banked_r14_irq_ = 0;
  banked_r13_svc_ = 0;
  banked_r14_svc_ = 0;
  banked_r13_abt_ = 0;
  banked_r14_abt_ = 0;
  banked_r13_und_ = 0;
  banked_r14_und_ = 0;
  spsr_fiq_ = 0;
  spsr_irq_ = 0;
  spsr_svc_ = 0;
  spsr_abt_ = 0;
  spsr_und_ = 0;
}

void GbaCpu::fault(const std::string& reason) {
  faulted_ = true;
  fault_reason_ = reason;
}

void GbaCpu::set_pc(std::uint32_t pc) {
  regs_.r[15] = pc;
}

void GbaCpu::set_thumb(bool thumb) {
  thumb_ = thumb;
  if (thumb_) {
    regs_.cpsr |= (1u << 5);
  } else {
    regs_.cpsr &= ~(1u << 5);
  }
}

std::uint32_t GbaCpu::reg(int index) const {
  if (index < 0 || index >= static_cast<int>(regs_.r.size())) {
    return 0;
  }
  return regs_.r[static_cast<std::size_t>(index)];
}

void GbaCpu::set_reg(int index, std::uint32_t value) {
  if (index < 0 || index >= static_cast<int>(regs_.r.size())) {
    return;
  }
  regs_.r[static_cast<std::size_t>(index)] = value;
}

void GbaCpu::set_cpsr(std::uint32_t value) {
  std::uint32_t old_mode = regs_.cpsr & 0x1F;
  std::uint32_t new_mode = value & 0x1F;
  if (old_mode != new_mode) {
    switch_mode(new_mode);
  }
  regs_.cpsr = value;
  thumb_ = (regs_.cpsr & (1u << 5)) != 0;
}

void GbaCpu::set_mode(std::uint32_t mode) {
  set_cpsr((regs_.cpsr & ~0x1Fu) | (mode & 0x1F));
}

void GbaCpu::set_irq_disable(bool disabled) {
  if (disabled) {
    regs_.cpsr |= (1u << 7);
  } else {
    regs_.cpsr &= ~(1u << 7);
  }
}

void GbaCpu::set_spsr_for_mode(std::uint32_t mode, std::uint32_t value) {
  if (std::uint32_t* target = spsr_for_mode(mode)) {
    *target = value;
    if ((regs_.cpsr & 0x1F) == (mode & 0x1F)) {
      spsr_ = value;
    }
  }
}

void GbaCpu::set_banked_sp(std::uint32_t mode, std::uint32_t value) {
  switch (mode & 0x1F) {
    case 0x10:
    case 0x1F:
      banked_r13_usr_ = value;
      break;
    case 0x11:
      banked_r13_fiq_ = value;
      break;
    case 0x12:
      banked_r13_irq_ = value;
      break;
    case 0x13:
      banked_r13_svc_ = value;
      break;
    case 0x17:
      banked_r13_abt_ = value;
      break;
    case 0x1B:
      banked_r13_und_ = value;
      break;
    default:
      break;
  }
  std::uint32_t current = regs_.cpsr & 0x1F;
  if ((current == 0x10 || current == 0x1F) && (mode == 0x10 || mode == 0x1F)) {
    regs_.r[13] = value;
  } else if (current == (mode & 0x1F)) {
    regs_.r[13] = value;
  }
}

void GbaCpu::set_log_unimplemented(int limit) {
  if (limit < 0) {
    limit = 0;
  }
  log_unimplemented_limit_ = limit;
  log_unimplemented_count_ = 0;
}

void GbaCpu::set_log_swi(int limit) {
  if (limit < 0) {
    limit = 0;
  }
  log_swi_limit_ = limit;
  log_swi_count_ = 0;
}

void GbaCpu::log_unimplemented(bool thumb, std::uint32_t pc, std::uint32_t op) {
  if (log_unimplemented_limit_ <= 0) {
    return;
  }
  if (log_unimplemented_count_ >= log_unimplemented_limit_) {
    return;
  }
  ++log_unimplemented_count_;
  std::cout << "GBA UNIMP " << (thumb ? "T" : "A") << " PC=0x"
            << std::hex << std::setw(8) << std::setfill('0') << pc
            << " OP=0x" << std::setw(thumb ? 4 : 8) << op
            << std::dec << "\n";
}

void GbaCpu::log_swi(bool thumb, std::uint32_t pc, std::uint32_t imm) {
  if (log_swi_limit_ <= 0) {
    return;
  }
  if (log_swi_count_ >= log_swi_limit_) {
    return;
  }
  ++log_swi_count_;
  std::cout << "GBA SWI " << (thumb ? "T" : "A") << " PC=0x"
            << std::hex << std::setw(8) << std::setfill('0') << pc
            << " IMM=0x" << std::setw(thumb ? 2 : 6) << (imm & (thumb ? 0xFFu : 0x00FFFFFFu))
            << std::dec << "\n";
}

void GbaCpu::switch_mode(std::uint32_t new_mode) {
  std::uint32_t old_mode = regs_.cpsr & 0x1F;
  if (old_mode == new_mode) {
    return;
  }
  if (old_mode == 0x11) {
    for (int i = 0; i < 5; ++i) {
      fiq_r8_12_[static_cast<std::size_t>(i)] = regs_.r[8 + i];
    }
  } else if (new_mode == 0x11) {
    for (int i = 0; i < 5; ++i) {
      shared_r8_12_[static_cast<std::size_t>(i)] = regs_.r[8 + i];
    }
  }
  save_banked(old_mode);
  load_banked(new_mode);
  if (old_mode == 0x11 && new_mode != 0x11) {
    for (int i = 0; i < 5; ++i) {
      regs_.r[8 + i] = shared_r8_12_[static_cast<std::size_t>(i)];
    }
  }
  if (new_mode == 0x11) {
    for (int i = 0; i < 5; ++i) {
      regs_.r[8 + i] = fiq_r8_12_[static_cast<std::size_t>(i)];
    }
  }
}

void GbaCpu::save_banked(std::uint32_t mode) {
  switch (mode) {
    case 0x10:
    case 0x1F:
      banked_r13_usr_ = regs_.r[13];
      banked_r14_usr_ = regs_.r[14];
      break;
    case 0x11:
      banked_r13_fiq_ = regs_.r[13];
      banked_r14_fiq_ = regs_.r[14];
      spsr_fiq_ = spsr_;
      break;
    case 0x12:
      banked_r13_irq_ = regs_.r[13];
      banked_r14_irq_ = regs_.r[14];
      spsr_irq_ = spsr_;
      break;
    case 0x13:
      banked_r13_svc_ = regs_.r[13];
      banked_r14_svc_ = regs_.r[14];
      spsr_svc_ = spsr_;
      break;
    case 0x17:
      banked_r13_abt_ = regs_.r[13];
      banked_r14_abt_ = regs_.r[14];
      spsr_abt_ = spsr_;
      break;
    case 0x1B:
      banked_r13_und_ = regs_.r[13];
      banked_r14_und_ = regs_.r[14];
      spsr_und_ = spsr_;
      break;
    default:
      break;
  }
}

void GbaCpu::load_banked(std::uint32_t mode) {
  switch (mode) {
    case 0x10:
    case 0x1F:
      regs_.r[13] = banked_r13_usr_;
      regs_.r[14] = banked_r14_usr_;
      spsr_ = 0;
      break;
    case 0x11:
      regs_.r[13] = banked_r13_fiq_;
      regs_.r[14] = banked_r14_fiq_;
      spsr_ = spsr_fiq_;
      break;
    case 0x12:
      regs_.r[13] = banked_r13_irq_;
      regs_.r[14] = banked_r14_irq_;
      spsr_ = spsr_irq_;
      break;
    case 0x13:
      regs_.r[13] = banked_r13_svc_;
      regs_.r[14] = banked_r14_svc_;
      spsr_ = spsr_svc_;
      break;
    case 0x17:
      regs_.r[13] = banked_r13_abt_;
      regs_.r[14] = banked_r14_abt_;
      spsr_ = spsr_abt_;
      break;
    case 0x1B:
      regs_.r[13] = banked_r13_und_;
      regs_.r[14] = banked_r14_und_;
      spsr_ = spsr_und_;
      break;
    default:
      spsr_ = 0;
      break;
  }
}

std::uint32_t* GbaCpu::spsr_for_mode(std::uint32_t mode) {
  switch (mode & 0x1F) {
    case 0x11:
      return &spsr_fiq_;
    case 0x12:
      return &spsr_irq_;
    case 0x13:
      return &spsr_svc_;
    case 0x17:
      return &spsr_abt_;
    case 0x1B:
      return &spsr_und_;
    default:
      return nullptr;
  }
}

std::uint32_t GbaCpu::get_flag_mask(std::uint32_t mask) const {
  return regs_.cpsr & mask;
}

void GbaCpu::set_flag_mask(std::uint32_t mask, bool value) {
  if (value) {
    regs_.cpsr |= mask;
  } else {
    regs_.cpsr &= ~mask;
  }
}

void GbaCpu::write_alu_result(std::uint32_t rd, std::uint32_t value, bool s) {
  if (rd >= regs_.r.size()) {
    return;
  }
  if (rd != 15) {
    regs_.r[rd] = value;
    return;
  }
  if (s) {
    std::uint32_t mode = regs_.cpsr & 0x1Fu;
    if (mode != 0x10 && mode != 0x1F) {
      set_cpsr(spsr_);
    }
  }
  std::uint32_t mask = thumb_ ? ~1u : ~3u;
  regs_.r[15] = value & mask;
}

std::uint32_t GbaCpu::operand_reg(int index) const {
  if (index < 0 || index >= static_cast<int>(regs_.r.size())) {
    return 0;
  }
  if (index == 15) {
    return regs_.r[15] + (thumb_ ? 2u : 4u);
  }
  return regs_.r[static_cast<std::size_t>(index)];
}

void GbaCpu::set_flags_nz(std::uint32_t result) {
  set_flag_mask(1u << 31, (result & 0x80000000u) != 0);
  set_flag_mask(1u << 30, result == 0);
}

void GbaCpu::set_flags_add(std::uint32_t a, std::uint32_t b, std::uint32_t result) {
  set_flags_nz(result);
  std::uint64_t wide = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
  set_flag_mask(1u << 29, wide > 0xFFFFFFFFu);
  bool overflow = (~(a ^ b) & (a ^ result) & 0x80000000u) != 0;
  set_flag_mask(1u << 28, overflow);
}

void GbaCpu::set_flags_sub(std::uint32_t a, std::uint32_t b, std::uint32_t result) {
  set_flags_nz(result);
  set_flag_mask(1u << 29, a >= b);
  bool overflow = ((a ^ b) & (a ^ result) & 0x80000000u) != 0;
  set_flag_mask(1u << 28, overflow);
}

bool GbaCpu::cond_passed(std::uint8_t cond) const {
  bool n = get_flag_mask(1u << 31) != 0;
  bool z = get_flag_mask(1u << 30) != 0;
  bool c = get_flag_mask(1u << 29) != 0;
  bool v = get_flag_mask(1u << 28) != 0;
  switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && (n == v);
    case 0xD: return z || (n != v);
    case 0xE: return true;
    default: return false;
  }
}

std::uint32_t GbaCpu::rot_imm(std::uint32_t imm8, std::uint32_t rot) const {
  if (rot == 0) {
    return imm8;
  }
  std::uint32_t r = (rot & 0xF) * 2;
  return (imm8 >> r) | (imm8 << (32 - r));
}

int GbaCpu::step(GbaBus* bus) {
  if (faulted_) {
    return 0;
  }
  if (!bus) {
    fault("GBA bus is null");
    return 0;
  }

  std::uint32_t pc = regs_.r[15];
  if (thumb_) {
    std::uint16_t op = bus->read16(pc);
    regs_.r[15] = pc + 2;
    std::uint16_t op_high = op & 0xF800;
    if ((op & 0xF800) == 0x0000 ||
        (op & 0xF800) == 0x0800 ||
        (op & 0xF800) == 0x1000) {
      std::uint32_t type = (op >> 11) & 0x3;
      std::uint32_t imm5 = (op >> 6) & 0x1F;
      int rs = (op >> 3) & 0x7;
      int rd = op & 0x7;
      bool carry_in = get_flag_mask(1u << 29) != 0;
      ShiftResult sh = shift_value(operand_reg(rs), type, imm5, carry_in, true);
      regs_.r[rd] = sh.value;
      set_flags_nz(sh.value);
      set_flag_mask(1u << 29, sh.carry);
      return 2;
    }
    if ((op & 0xFC00) == 0x4000) {
      std::uint32_t opcode = (op >> 6) & 0xF;
      int rs = (op >> 3) & 0x7;
      int rd = op & 0x7;
      std::uint32_t a = operand_reg(rd);
      std::uint32_t b = operand_reg(rs);
      bool carry_in = get_flag_mask(1u << 29) != 0;
      switch (opcode) {
        case 0x0: {
          std::uint32_t result = a & b;
          regs_.r[rd] = result;
          set_flags_nz(result);
          return 2;
        }
        case 0x1: {
          std::uint32_t result = a ^ b;
          regs_.r[rd] = result;
          set_flags_nz(result);
          return 2;
        }
        case 0x2: {
          ShiftResult sh = shift_value(a, 0, b & 0xFF, carry_in, false);
          regs_.r[rd] = sh.value;
          set_flags_nz(sh.value);
          set_flag_mask(1u << 29, sh.carry);
          return 2;
        }
        case 0x3: {
          ShiftResult sh = shift_value(a, 1, b & 0xFF, carry_in, false);
          regs_.r[rd] = sh.value;
          set_flags_nz(sh.value);
          set_flag_mask(1u << 29, sh.carry);
          return 2;
        }
        case 0x4: {
          ShiftResult sh = shift_value(a, 2, b & 0xFF, carry_in, false);
          regs_.r[rd] = sh.value;
          set_flags_nz(sh.value);
          set_flag_mask(1u << 29, sh.carry);
          return 2;
        }
        case 0x5: {
          AddResult ar = add_with_carry(a, b, carry_in);
          regs_.r[rd] = ar.value;
          set_flags_nz(ar.value);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
          return 2;
        }
        case 0x6: {
          AddResult ar = add_with_carry(a, ~b, carry_in);
          regs_.r[rd] = ar.value;
          set_flags_nz(ar.value);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
          return 2;
        }
        case 0x7: {
          ShiftResult sh = shift_value(a, 3, b & 0xFF, carry_in, false);
          regs_.r[rd] = sh.value;
          set_flags_nz(sh.value);
          set_flag_mask(1u << 29, sh.carry);
          return 2;
        }
        case 0x8: {
          std::uint32_t result = a & b;
          set_flags_nz(result);
          return 2;
        }
        case 0x9: {
          std::uint32_t result = 0 - b;
          regs_.r[rd] = result;
          set_flags_sub(0, b, result);
          return 2;
        }
        case 0xA: {
          std::uint32_t result = a - b;
          set_flags_sub(a, b, result);
          return 2;
        }
        case 0xB: {
          std::uint32_t result = a + b;
          set_flags_add(a, b, result);
          return 2;
        }
        case 0xC: {
          std::uint32_t result = a | b;
          regs_.r[rd] = result;
          set_flags_nz(result);
          return 2;
        }
        case 0xD: {
          std::uint32_t result = a * b;
          regs_.r[rd] = result;
          set_flags_nz(result);
          return 2;
        }
        case 0xE: {
          std::uint32_t result = a & ~b;
          regs_.r[rd] = result;
          set_flags_nz(result);
          return 2;
        }
        case 0xF: {
          std::uint32_t result = ~b;
          regs_.r[rd] = result;
          set_flags_nz(result);
          return 2;
        }
        default:
          break;
      }
    }
    if ((op & 0xFC00) == 0x4400) {
      std::uint32_t opcode = (op >> 8) & 0x3;
      std::uint32_t h1 = (op >> 7) & 0x1;
      std::uint32_t h2 = (op >> 6) & 0x1;
      std::uint32_t rs = ((op >> 3) & 0x7) | (h2 << 3);
      std::uint32_t rd = (op & 0x7) | (h1 << 3);
      std::uint32_t val = operand_reg(static_cast<int>(rs));
      switch (opcode) {
        case 0x0: {
          std::uint32_t result = operand_reg(static_cast<int>(rd)) + val;
          if (rd == 15) {
            // ADD (high register) to PC is a branch in Thumb state; bit0 is ignored.
            regs_.r[15] = result & ~1u;
          } else {
            regs_.r[rd] = result;
          }
          return 2;
        }
        case 0x1: {
          std::uint32_t lhs = operand_reg(static_cast<int>(rd));
          std::uint32_t result = lhs - val;
          set_flags_sub(lhs, val, result);
          return 2;
        }
        case 0x2: {
          if (rd == 15) {
            // MOV (high register) to PC stays in Thumb; use BX for interworking.
            regs_.r[15] = val & ~1u;
          } else {
            regs_.r[rd] = val;
          }
          return 2;
        }
        case 0x3: {
          std::uint32_t target = val;
          set_thumb((target & 1u) != 0);
          regs_.r[15] = target & ~1u;
          return 2;
        }
        default:
          break;
      }
    }
    if (op_high == 0x2000) {
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = op & 0xFF;
      regs_.r[rd] = imm;
      set_flags_nz(regs_.r[rd]);
      return 2;
    }
    if (op_high == 0x2800) {
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = op & 0xFF;
      std::uint32_t result = regs_.r[rd] - imm;
      set_flags_sub(regs_.r[rd], imm, result);
      return 2;
    }
    if (op_high == 0x3000) {
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = op & 0xFF;
      std::uint32_t result = regs_.r[rd] + imm;
      regs_.r[rd] = result;
      set_flags_add(regs_.r[rd] - imm, imm, result);
      return 2;
    }
    if (op_high == 0x3800) {
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = op & 0xFF;
      std::uint32_t result = regs_.r[rd] - imm;
      regs_.r[rd] = result;
      set_flags_sub(regs_.r[rd] + imm, imm, result);
      return 2;
    }
    if ((op & 0xFF00) == 0x4700) {
      int rm = (op >> 3) & 0xF;
      std::uint32_t target = operand_reg(rm);
      set_thumb((target & 1u) != 0);
      regs_.r[15] = target & ~1u;
      return 2;
    }
    if ((op & 0xF800) == 0x4800) {
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = (op & 0xFF) << 2;
      std::uint32_t base = (pc + 4) & ~3u;
      regs_.r[rd] = bus->read32(base + imm);
      return 3;
    }
    if ((op & 0xF000) == 0x5000) {
      std::uint32_t opcode = (op >> 9) & 0x7;
      int rm = (op >> 6) & 0x7;
      int rn = (op >> 3) & 0x7;
      int rd = op & 0x7;
      std::uint32_t addr = operand_reg(rn) + operand_reg(rm);
      switch (opcode) {
        case 0x0:
          bus->write32(addr, regs_.r[rd]);
          return 3;
        case 0x1:
          bus->write16(addr, static_cast<std::uint16_t>(regs_.r[rd] & 0xFFFF));
          return 3;
        case 0x2:
          bus->write8(addr, static_cast<std::uint8_t>(regs_.r[rd] & 0xFF));
          return 3;
        case 0x3: {
          std::int8_t value = static_cast<std::int8_t>(bus->read8(addr));
          regs_.r[rd] = static_cast<std::int32_t>(value);
          return 3;
        }
        case 0x4:
          regs_.r[rd] = bus->read32(addr);
          return 3;
        case 0x5:
          regs_.r[rd] = bus->read16(addr);
          return 3;
        case 0x6:
          regs_.r[rd] = bus->read8(addr);
          return 3;
        case 0x7: {
          std::int16_t value = static_cast<std::int16_t>(bus->read16(addr));
          regs_.r[rd] = static_cast<std::int32_t>(value);
          return 3;
        }
        default:
          break;
      }
    }
    if ((op & 0xF000) == 0x6000) {
      bool load = (op & 0x0800) != 0;
      std::uint32_t imm5 = (op >> 6) & 0x1F;
      int rb = (op >> 3) & 0x7;
      int rd = op & 0x7;
      std::uint32_t addr = regs_.r[rb] + (imm5 << 2);
      if (load) {
        regs_.r[rd] = bus->read32(addr);
      } else {
        bus->write32(addr, regs_.r[rd]);
      }
      return 3;
    }
    if ((op & 0xF000) == 0x7000) {
      bool load = (op & 0x0800) != 0;
      std::uint32_t imm5 = (op >> 6) & 0x1F;
      int rb = (op >> 3) & 0x7;
      int rd = op & 0x7;
      std::uint32_t addr = regs_.r[rb] + imm5;
      if (load) {
        regs_.r[rd] = bus->read8(addr);
      } else {
        bus->write8(addr, static_cast<std::uint8_t>(regs_.r[rd] & 0xFF));
      }
      return 3;
    }
    if ((op & 0xF000) == 0x8000) {
      bool load = (op & 0x0800) != 0;
      std::uint32_t imm5 = (op >> 6) & 0x1F;
      int rb = (op >> 3) & 0x7;
      int rd = op & 0x7;
      std::uint32_t addr = regs_.r[rb] + (imm5 << 1);
      if (load) {
        regs_.r[rd] = bus->read16(addr);
      } else {
        bus->write16(addr, static_cast<std::uint16_t>(regs_.r[rd] & 0xFFFF));
      }
      return 3;
    }
    if ((op & 0xF000) == 0x9000) {
      bool load = (op & 0x0800) != 0;
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = (op & 0xFF) << 2;
      std::uint32_t addr = regs_.r[13] + imm;
      if (load) {
        regs_.r[rd] = bus->read32(addr);
      } else {
        bus->write32(addr, regs_.r[rd]);
      }
      return 3;
    }
    if ((op & 0xF800) == 0xA000) {
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = (op & 0xFF) << 2;
      std::uint32_t base = (pc + 4) & ~3u;
      regs_.r[rd] = base + imm;
      return 2;
    }
    if ((op & 0xF800) == 0xA800) {
      int rd = (op >> 8) & 0x7;
      std::uint32_t imm = (op & 0xFF) << 2;
      regs_.r[rd] = regs_.r[13] + imm;
      return 2;
    }
    if ((op & 0xFF00) == 0xB000) {
      std::uint32_t imm = (op & 0x7F) << 2;
      if (op & 0x0080) {
        regs_.r[13] -= imm;
      } else {
        regs_.r[13] += imm;
      }
      return 2;
    }
    if ((op & 0xF600) == 0xB400) {
      bool load = (op & 0x0800) != 0;
      bool extra = (op & 0x0100) != 0;
      std::uint32_t addr = regs_.r[13];
      if (!load) {
        if (extra) {
          addr -= 4;
          bus->write32(addr, regs_.r[14]);
        }
        for (int r = 7; r >= 0; --r) {
          if (op & (1u << r)) {
            addr -= 4;
            bus->write32(addr, regs_.r[r]);
          }
        }
        regs_.r[13] = addr;
      } else {
        for (int r = 0; r < 8; ++r) {
          if (op & (1u << r)) {
            regs_.r[r] = bus->read32(addr);
            addr += 4;
          }
        }
        if (extra) {
          std::uint32_t value = bus->read32(addr);
          addr += 4;
          set_thumb((value & 1u) != 0);
          regs_.r[15] = value & ~1u;
        }
        regs_.r[13] = addr;
      }
      return 4;
    }
    if ((op & 0xF000) == 0xC000) {
      bool load = (op & 0x0800) != 0;
      int rn = (op >> 8) & 0x7;
      std::uint32_t addr = regs_.r[rn];
      for (int r = 0; r < 8; ++r) {
        if (op & (1u << r)) {
          if (load) {
            regs_.r[r] = bus->read32(addr);
          } else {
            bus->write32(addr, regs_.r[r]);
          }
          addr += 4;
        }
      }
      regs_.r[rn] = addr;
      return 4;
    }
    if ((op & 0xFC00) == 0x1800) {
      bool sub = (op & 0x0200) != 0;
      std::uint32_t rn = (op >> 6) & 0x7;
      std::uint32_t rs = (op >> 3) & 0x7;
      std::uint32_t rd = op & 0x7;
      std::uint32_t lhs = regs_.r[rs];
      std::uint32_t rhs = regs_.r[rn];
      std::uint32_t result = sub ? (lhs - rhs) : (lhs + rhs);
      regs_.r[rd] = result;
      if (sub) {
        set_flags_sub(lhs, rhs, result);
      } else {
        set_flags_add(lhs, rhs, result);
      }
      return 2;
    }
    if ((op & 0xFC00) == 0x1C00) {
      bool immediate = (op & 0x0400) != 0;
      bool sub = (op & 0x0200) != 0;
      std::uint32_t rn = (op >> 3) & 0x7;
      std::uint32_t rd = op & 0x7;
      std::uint32_t op2 = immediate ? ((op >> 6) & 0x7) : regs_.r[(op >> 6) & 0x7];
      std::uint32_t result = 0;
      if (sub) {
        result = regs_.r[rn] - op2;
        regs_.r[rd] = result;
        set_flags_sub(regs_.r[rn], op2, result);
      } else {
        result = regs_.r[rn] + op2;
        regs_.r[rd] = result;
        set_flags_add(regs_.r[rn], op2, result);
      }
      return 2;
    }
    if ((op & 0xFF00) == 0xDF00) {
      log_swi(true, pc, static_cast<std::uint32_t>(op & 0xFF));
      std::uint32_t return_addr = pc + 2;
      if (std::uint32_t* spsr = spsr_for_mode(0x13)) {
        *spsr = regs_.cpsr;
      }
      set_mode(0x13);
      regs_.r[14] = return_addr;
      set_thumb(false);
      set_irq_disable(true);
      regs_.r[15] = 0x00000008u;
      return 3;
    }
    if ((op & 0xF000) == 0xD000 && (op & 0x0F00) != 0x0F00) {
      std::uint8_t cond = static_cast<std::uint8_t>((op >> 8) & 0x0F);
      std::int8_t imm8 = static_cast<std::int8_t>(op & 0xFF);
      if (cond_passed(cond)) {
        std::int32_t offset = static_cast<std::int32_t>(imm8) * 2;
        regs_.r[15] = pc + 4 + static_cast<std::uint32_t>(offset);
      }
      return 2;
    }
    if ((op & 0xF800) == 0xE000) {
      std::int32_t imm11 = static_cast<std::int32_t>(op & 0x7FF);
      if (imm11 & 0x400) {
        imm11 |= ~0x7FF;
      }
      std::int32_t offset = imm11 * 2;
      regs_.r[15] = pc + 4 + static_cast<std::uint32_t>(offset);
      return 2;
    }
    if ((op & 0xF800) == 0xF000) {
      std::int32_t imm11 = static_cast<std::int32_t>(op & 0x7FF);
      if (imm11 & 0x400) {
        imm11 |= ~0x7FF;
      }
      std::int32_t offset = imm11 * 4096;
      regs_.r[14] = (pc + 4) + static_cast<std::uint32_t>(offset);
      return 2;
    }
    if ((op & 0xF800) == 0xF800) {
      std::uint32_t imm11 = op & 0x7FF;
      std::uint32_t target = regs_.r[14] + (imm11 << 1);
      regs_.r[14] = (pc + 2) | 1u;
      regs_.r[15] = target;
      return 2;
    }
    log_unimplemented(true, pc, op);
    ++unimplemented_count_;
    return 2;
  }

  std::uint32_t op = bus->read32(pc);
  regs_.r[15] = pc + 4;

  std::uint8_t cond = static_cast<std::uint8_t>(op >> 28);
  if (!cond_passed(cond)) {
    return 4;
  }

  if ((op & 0x0FFFFFF0u) == 0x012FFF10u) {
    int rm = op & 0xF;
    std::uint32_t target = operand_reg(rm);
    set_thumb((target & 1u) != 0);
    regs_.r[15] = target & ~1u;
    return 4;
  }

  if ((op & 0x0F000000u) == 0x0F000000u) {
    log_swi(false, pc, op & 0x00FFFFFFu);
    std::uint32_t return_addr = pc + 4;
    if (std::uint32_t* spsr = spsr_for_mode(0x13)) {
      *spsr = regs_.cpsr;
    }
    set_mode(0x13);
    regs_.r[14] = return_addr;
    set_thumb(false);
    set_irq_disable(true);
    regs_.r[15] = 0x00000008u;
    return 4;
  }

  if ((op & 0x0FFFF000u) == 0xE25EF000u && (op & 0x00000FFFu) == 0x00000004u) {
    std::uint32_t target = regs_.r[14] - 4;
    std::uint32_t mode = regs_.cpsr & 0x1Fu;
    if (mode != 0x10 && mode != 0x1F) {
      std::uint32_t restore = spsr_;
      set_cpsr(restore);
    }
    regs_.r[15] = target & ~1u;
    return 4;
  }

  if ((op & 0x0FFFFFFFu) == 0xE1B0F00Eu) {
    std::uint32_t target = regs_.r[14];
    std::uint32_t mode = regs_.cpsr & 0x1Fu;
    if (mode != 0x10 && mode != 0x1F) {
      std::uint32_t restore = spsr_;
      set_cpsr(restore);
    }
    regs_.r[15] = target & ~1u;
    return 4;
  }

  if ((op & 0x0E000000u) == 0x0A000000u) {
    std::int32_t imm24 = static_cast<std::int32_t>(op & 0x00FFFFFFu);
    if (imm24 & 0x00800000) {
      imm24 |= ~0x00FFFFFF;
    }
    std::int32_t offset = imm24 * 4;
    std::uint32_t target = pc + 8 + static_cast<std::uint32_t>(offset);
    if (op & (1u << 24)) {
      regs_.r[14] = pc + 4;
    }
    regs_.r[15] = target;
    return 4;
  }

  if ((op & 0x0FC000F0u) == 0x00000090u) {
    bool accumulate = (op & (1u << 21)) != 0;
    bool s = (op & (1u << 20)) != 0;
    std::uint32_t rd = (op >> 16) & 0xF;
    std::uint32_t rn = (op >> 12) & 0xF;
    std::uint32_t rs = (op >> 8) & 0xF;
    std::uint32_t rm = op & 0xF;
    std::uint64_t result = static_cast<std::uint64_t>(regs_.r[rm]) *
                            static_cast<std::uint64_t>(regs_.r[rs]);
    if (accumulate) {
      result += regs_.r[rn];
    }
    regs_.r[rd] = static_cast<std::uint32_t>(result);
    if (s) {
      set_flags_nz(regs_.r[rd]);
    }
    return 4;
  }

  // Halfword/signed transfer (LDRH/STRH/LDRSB/LDRSH) accepts both
  // register-offset (I=0) and immediate-offset (I=1) encodings.
  if ((op & 0x0E000090u) == 0x00000090u && (op & 0x00000060u) != 0) {
    bool p = (op & (1u << 24)) != 0;
    bool u = (op & (1u << 23)) != 0;
    bool w = (op & (1u << 21)) != 0;
    bool l = (op & (1u << 20)) != 0;
    bool s = (op & (1u << 6)) != 0;
    bool h = (op & (1u << 5)) != 0;
    std::uint32_t rn = (op >> 16) & 0xF;
    std::uint32_t rd = (op >> 12) & 0xF;
    std::uint32_t offset = 0;
    if (op & (1u << 22)) {
      std::uint32_t hi = (op >> 8) & 0xF;
      std::uint32_t lo = op & 0xF;
      offset = (hi << 4) | lo;
    } else {
      std::uint32_t rm = op & 0xF;
      offset = operand_reg(static_cast<int>(rm));
    }
    std::uint32_t base = operand_reg(static_cast<int>(rn));
    std::uint32_t addr = base;
    if (p) {
      addr = u ? (base + offset) : (base - offset);
      if (w) {
        regs_.r[rn] = addr;
      }
    } else {
      addr = base;
      std::uint32_t next = u ? (base + offset) : (base - offset);
      regs_.r[rn] = next;
    }
    if (l) {
      if (h && s) {
        std::int16_t value = static_cast<std::int16_t>(bus->read16(addr));
        regs_.r[rd] = static_cast<std::int32_t>(value);
      } else if (h) {
        regs_.r[rd] = bus->read16(addr);
      } else if (s) {
        std::int8_t value = static_cast<std::int8_t>(bus->read8(addr));
        regs_.r[rd] = static_cast<std::int32_t>(value);
      } else {
        regs_.r[rd] = bus->read16(addr);
      }
    } else {
      if (h) {
        bus->write16(addr, static_cast<std::uint16_t>(regs_.r[rd] & 0xFFFF));
      } else {
        bus->write8(addr, static_cast<std::uint8_t>(regs_.r[rd] & 0xFF));
      }
    }
    return 4;
  }

  if ((op & 0x0FBF0FFFu) == 0x010F0000u) {
    std::uint32_t rd = (op >> 12) & 0xF;
    bool spsr = (op & (1u << 22)) != 0;
    if (spsr) {
      regs_.r[rd] = spsr_;
    } else {
      regs_.r[rd] = regs_.cpsr;
    }
    return 4;
  }

  if ((op & 0x0FB0FFF0u) == 0x0120F000u) {
    std::uint32_t rm = op & 0xF;
    std::uint32_t value = operand_reg(static_cast<int>(rm));
    std::uint32_t mask = 0;
    if (op & (1u << 16)) mask |= 0x000000FFu;
    if (op & (1u << 17)) mask |= 0x0000FF00u;
    if (op & (1u << 18)) mask |= 0x00FF0000u;
    if (op & (1u << 19)) mask |= 0xFF000000u;
    bool to_spsr = (op & (1u << 22)) != 0;
    if (to_spsr) {
      if (std::uint32_t* target = spsr_for_mode(regs_.cpsr & 0x1F)) {
        *target = (*target & ~mask) | (value & mask);
        if ((regs_.cpsr & 0x1F) != 0x10 && (regs_.cpsr & 0x1F) != 0x1F) {
          spsr_ = *target;
        }
      }
    } else {
      std::uint32_t mode = regs_.cpsr & 0x1F;
      if (mode == 0x10) {
        mask &= 0xFF000000u;
      }
      set_cpsr((regs_.cpsr & ~mask) | (value & mask));
    }
    return 4;
  }

  if (((op >> 26) & 0x3) == 0x1) {
    bool i = (op & (1u << 25)) != 0;
    bool p = (op & (1u << 24)) != 0;
    bool u = (op & (1u << 23)) != 0;
    bool b = (op & (1u << 22)) != 0;
    bool w = (op & (1u << 21)) != 0;
    bool l = (op & (1u << 20)) != 0;
    if (!i) {
      std::uint32_t rn = (op >> 16) & 0xF;
      std::uint32_t rd = (op >> 12) & 0xF;
      std::uint32_t offset = op & 0xFFF;
      std::uint32_t base = operand_reg(static_cast<int>(rn));
      std::uint32_t addr = base;
      if (p) {
        addr = u ? (base + offset) : (base - offset);
        if (w) {
          regs_.r[rn] = addr;
        }
      } else {
        addr = base;
        std::uint32_t next = u ? (base + offset) : (base - offset);
        regs_.r[rn] = next;
      }
      if (l) {
        std::uint32_t value = b ? bus->read8(addr) : bus->read32(addr);
        if (rd == 15) {
          // ARM7TDMI does not interwork on LDR/POP PC in ARM state.
          regs_.r[15] = value & ~3u;
        } else {
          regs_.r[rd] = value;
        }
      } else {
        if (b) {
          bus->write8(addr, static_cast<std::uint8_t>(regs_.r[rd] & 0xFF));
        } else {
          bus->write32(addr, regs_.r[rd]);
        }
      }
      return 4;
    }
    if (i && (op & 0x10) == 0) {
      std::uint32_t rn = (op >> 16) & 0xF;
      std::uint32_t rd = (op >> 12) & 0xF;
      std::uint32_t rm = op & 0xF;
      std::uint32_t shift_type = (op >> 5) & 0x3;
      std::uint32_t shift = (op >> 7) & 0x1F;
      std::uint32_t offset = operand_reg(static_cast<int>(rm));
      switch (shift_type) {
        case 0:
          offset = offset << shift;
          break;
        case 1:
          offset = (shift == 0) ? 0 : (offset >> shift);
          break;
        case 2:
          if (shift == 0) {
            offset = (offset & 0x80000000u) ? 0xFFFFFFFFu : 0u;
          } else {
            offset = static_cast<std::uint32_t>(static_cast<std::int32_t>(offset) >> shift);
          }
          break;
        case 3:
          if (shift == 0) {
            offset = (offset >> 1) | (offset << 31);
          } else {
            offset = (offset >> shift) | (offset << (32 - shift));
          }
          break;
        default:
          break;
      }
      std::uint32_t base = operand_reg(static_cast<int>(rn));
      std::uint32_t addr = base;
      if (p) {
        addr = u ? (base + offset) : (base - offset);
        if (w) {
          regs_.r[rn] = addr;
        }
      } else {
        addr = base;
        std::uint32_t next = u ? (base + offset) : (base - offset);
        regs_.r[rn] = next;
      }
      if (l) {
        std::uint32_t value = b ? bus->read8(addr) : bus->read32(addr);
        if (rd == 15) {
          regs_.r[15] = value & ~3u;
        } else {
          regs_.r[rd] = value;
        }
      } else {
        if (b) {
          bus->write8(addr, static_cast<std::uint8_t>(regs_.r[rd] & 0xFF));
        } else {
          bus->write32(addr, regs_.r[rd]);
        }
      }
      return 4;
    }
    if (i && (op & 0x10) != 0) {
      std::uint32_t rn = (op >> 16) & 0xF;
      std::uint32_t rd = (op >> 12) & 0xF;
      std::uint32_t rm = op & 0xF;
      std::uint32_t rs = (op >> 8) & 0xF;
      std::uint32_t shift_type = (op >> 5) & 0x3;
      bool carry_in = get_flag_mask(1u << 29) != 0;
      std::uint32_t shift = regs_.r[rs] & 0xFF;
      ShiftResult sh = shift_value(operand_reg(static_cast<int>(rm)), shift_type, shift, carry_in, false);
      std::uint32_t offset = sh.value;
      std::uint32_t base = operand_reg(static_cast<int>(rn));
      std::uint32_t addr = base;
      if (p) {
        addr = u ? (base + offset) : (base - offset);
        if (w) {
          regs_.r[rn] = addr;
        }
      } else {
        addr = base;
        std::uint32_t next = u ? (base + offset) : (base - offset);
        regs_.r[rn] = next;
      }
      if (l) {
        std::uint32_t value = b ? bus->read8(addr) : bus->read32(addr);
        if (rd == 15) {
          regs_.r[15] = value & ~3u;
        } else {
          regs_.r[rd] = value;
        }
      } else {
        if (b) {
          bus->write8(addr, static_cast<std::uint8_t>(regs_.r[rd] & 0xFF));
        } else {
          bus->write32(addr, regs_.r[rd]);
        }
      }
      return 4;
    }
  }

  if (((op >> 25) & 0x7) == 0x4) {
    bool p = (op & (1u << 24)) != 0;
    bool u = (op & (1u << 23)) != 0;
    bool s = (op & (1u << 22)) != 0;
    bool w = (op & (1u << 21)) != 0;
    bool l = (op & (1u << 20)) != 0;
    std::uint32_t rn = (op >> 16) & 0xF;
    std::uint32_t reg_list = op & 0xFFFF;
    if (reg_list == 0) {
      reg_list = 1u << 15;
    }
    int count = 0;
    for (int i = 0; i < 16; ++i) {
      if (reg_list & (1u << i)) {
        ++count;
      }
    }
    std::uint32_t addr = regs_.r[rn];
    if (u) {
      if (p) {
        addr += 4;
      }
    } else {
      if (p) {
        addr -= static_cast<std::uint32_t>(count) * 4u;
      } else {
        addr -= static_cast<std::uint32_t>(count) * 4u;
        addr += 4;
      }
    }
    bool user_bank = s && !(reg_list & (1u << 15)) &&
                     ((regs_.cpsr & 0x1F) != 0x10) && ((regs_.cpsr & 0x1F) != 0x1F);
    auto read_user_reg = [this](int index) -> std::uint32_t {
      if (index == 13) {
        return banked_r13_usr_;
      }
      if (index == 14) {
        return banked_r14_usr_;
      }
      if ((regs_.cpsr & 0x1F) == 0x11 && index >= 8 && index <= 12) {
        return shared_r8_12_[static_cast<std::size_t>(index - 8)];
      }
      return regs_.r[static_cast<std::size_t>(index)];
    };
    auto write_user_reg = [this](int index, std::uint32_t value) {
      if (index == 13) {
        banked_r13_usr_ = value;
        return;
      }
      if (index == 14) {
        banked_r14_usr_ = value;
        return;
      }
      if ((regs_.cpsr & 0x1F) == 0x11 && index >= 8 && index <= 12) {
        shared_r8_12_[static_cast<std::size_t>(index - 8)] = value;
        return;
      }
      regs_.r[static_cast<std::size_t>(index)] = value;
    };
    std::uint32_t loaded_pc = 0;
    bool loaded_pc_valid = false;
    for (int i = 0; i < 16; ++i) {
      if (!(reg_list & (1u << i))) {
        continue;
      }
      if (l) {
        std::uint32_t value = bus->read32(addr);
        if (i == 15) {
          loaded_pc = value;
          loaded_pc_valid = true;
        } else {
          if (user_bank) {
            write_user_reg(i, value);
          } else {
            regs_.r[i] = value;
          }
        }
      } else {
        std::uint32_t value = (i == 15) ? (regs_.r[15] + 4)
                                         : (user_bank ? read_user_reg(i) : regs_.r[i]);
        bus->write32(addr, value);
      }
      addr += 4;
    }
    if (l && s && (reg_list & (1u << 15))) {
      std::uint32_t mode = regs_.cpsr & 0x1Fu;
      if (mode != 0x10 && mode != 0x1F) {
        set_cpsr(spsr_);
      }
    }
    if (l && loaded_pc_valid) {
      if (s && (reg_list & (1u << 15))) {
        regs_.r[15] = loaded_pc & (thumb_ ? ~1u : ~3u);
      } else {
        regs_.r[15] = loaded_pc & ~3u;
      }
    }
    if (w) {
      std::uint32_t delta = static_cast<std::uint32_t>(count) * 4u;
      regs_.r[rn] = u ? (regs_.r[rn] + delta) : (regs_.r[rn] - delta);
    }
    return 4 + count;
  }

  if (((op >> 25) & 0x7) == 0x1) {
    std::uint32_t opcode = (op >> 21) & 0xF;
    bool s = (op & (1u << 20)) != 0;
    std::uint32_t rn = (op >> 16) & 0xF;
    std::uint32_t rd = (op >> 12) & 0xF;
    std::uint32_t imm8 = op & 0xFF;
    std::uint32_t rot = (op >> 8) & 0xF;
    std::uint32_t imm = rot_imm(imm8, rot);
    bool carry_in = get_flag_mask(1u << 29) != 0;
    bool sh_carry = (rot == 0) ? carry_in : ((imm >> 31) & 1u) != 0;
    std::uint32_t result = 0;
    switch (opcode) {
      case 0x0:
        result = operand_reg(static_cast<int>(rn)) & imm;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh_carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0x1:
        result = operand_reg(static_cast<int>(rn)) ^ imm;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh_carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0x2: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), ~imm, true);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x3: {
        AddResult ar = add_with_carry(imm, ~operand_reg(static_cast<int>(rn)), true);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x4: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), imm, false);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x5: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), imm, carry_in);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x6: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), ~imm, carry_in);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x7: {
        AddResult ar = add_with_carry(imm, ~operand_reg(static_cast<int>(rn)), carry_in);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x8:
        result = operand_reg(static_cast<int>(rn)) & imm;
        set_flags_nz(result);
        set_flag_mask(1u << 29, sh_carry);
        return 4;
      case 0x9:
        result = operand_reg(static_cast<int>(rn)) ^ imm;
        set_flags_nz(result);
        set_flag_mask(1u << 29, sh_carry);
        return 4;
      case 0xA: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), ~imm, true);
        set_flags_nz(ar.value);
        set_flag_mask(1u << 29, ar.carry);
        set_flag_mask(1u << 28, ar.overflow);
        return 4;
      }
      case 0xB: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), imm, false);
        set_flags_nz(ar.value);
        set_flag_mask(1u << 29, ar.carry);
        set_flag_mask(1u << 28, ar.overflow);
        return 4;
      }
      case 0xC:
        result = operand_reg(static_cast<int>(rn)) | imm;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh_carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0xD:
        result = imm;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh_carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0xE:
        result = operand_reg(static_cast<int>(rn)) & ~imm;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh_carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0xF:
        result = ~imm;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh_carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      default:
        break;
    }
  }

  if (((op >> 25) & 0x7) == 0x0) {
    std::uint32_t opcode = (op >> 21) & 0xF;
    bool s = (op & (1u << 20)) != 0;
    std::uint32_t rn = (op >> 16) & 0xF;
    std::uint32_t rd = (op >> 12) & 0xF;
    std::uint32_t rm = op & 0xF;
    std::uint32_t op2 = operand_reg(static_cast<int>(rm));
    std::uint32_t shift_type = (op >> 5) & 0x3;
    bool carry_in = get_flag_mask(1u << 29) != 0;
    ShiftResult sh = {op2, carry_in};
    if ((op & 0x10) == 0) {
      std::uint32_t shift = (op >> 7) & 0x1F;
      sh = shift_value(op2, shift_type, shift, carry_in, true);
    } else {
      std::uint32_t rs = (op >> 8) & 0xF;
      std::uint32_t shift = regs_.r[rs] & 0xFF;
      sh = shift_value(op2, shift_type, shift, carry_in, false);
    }
    op2 = sh.value;
    std::uint32_t result = 0;
    switch (opcode) {
      case 0x0:
        result = operand_reg(static_cast<int>(rn)) & op2;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh.carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0x1:
        result = operand_reg(static_cast<int>(rn)) ^ op2;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh.carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0x2:
        result = operand_reg(static_cast<int>(rn)) - op2;
        if (s) set_flags_sub(operand_reg(static_cast<int>(rn)), op2, result);
        write_alu_result(rd, result, s);
        return 4;
      case 0x3:
        result = op2 - operand_reg(static_cast<int>(rn));
        if (s) set_flags_sub(op2, operand_reg(static_cast<int>(rn)), result);
        write_alu_result(rd, result, s);
        return 4;
      case 0x4:
        result = operand_reg(static_cast<int>(rn)) + op2;
        if (s) set_flags_add(operand_reg(static_cast<int>(rn)), op2, result);
        write_alu_result(rd, result, s);
        return 4;
      case 0x5: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), op2, carry_in);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x6: {
        AddResult ar = add_with_carry(operand_reg(static_cast<int>(rn)), ~op2, carry_in);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x7: {
        AddResult ar = add_with_carry(op2, ~operand_reg(static_cast<int>(rn)), carry_in);
        result = ar.value;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, ar.carry);
          set_flag_mask(1u << 28, ar.overflow);
        }
        write_alu_result(rd, result, s);
        return 4;
      }
      case 0x8:
        result = operand_reg(static_cast<int>(rn)) & op2;
        set_flags_nz(result);
        set_flag_mask(1u << 29, sh.carry);
        return 4;
      case 0x9:
        result = operand_reg(static_cast<int>(rn)) ^ op2;
        set_flags_nz(result);
        set_flag_mask(1u << 29, sh.carry);
        return 4;
      case 0xA:
        result = operand_reg(static_cast<int>(rn)) - op2;
        set_flags_sub(operand_reg(static_cast<int>(rn)), op2, result);
        return 4;
      case 0xB:
        result = operand_reg(static_cast<int>(rn)) + op2;
        set_flags_add(operand_reg(static_cast<int>(rn)), op2, result);
        return 4;
      case 0xC:
        result = operand_reg(static_cast<int>(rn)) | op2;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh.carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0xD:
        result = op2;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh.carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0xE:
        result = operand_reg(static_cast<int>(rn)) & ~op2;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh.carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      case 0xF:
        result = ~op2;
        if (s) {
          set_flags_nz(result);
          set_flag_mask(1u << 29, sh.carry);
        }
        write_alu_result(rd, result, s);
        return 4;
      default:
        break;
    }
  }

  if ((op & 0x0F000000u) == 0x0F000000u) {
    log_unimplemented(false, pc, op);
    ++unimplemented_count_;
    return 4;
  }

  log_unimplemented(false, pc, op);
  ++unimplemented_count_;
  return 4;
}

} // namespace gbemu::core
