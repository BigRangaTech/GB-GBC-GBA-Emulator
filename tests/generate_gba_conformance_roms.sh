#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-Test-Games/Conformance/_Generated}"
mkdir -p "${OUT_DIR}"

write_u8() {
  local out="$1"
  local offset="$2"
  local value="$3"
  local hex
  hex="$(printf '%02x' $((value & 0xFF)))"
  printf '%b' "\\x${hex}" | dd of="${out}" bs=1 seek="$((offset))" conv=notrunc status=none
}

write_u16_le() {
  local out="$1"
  local offset="$2"
  local value="$3"
  write_u8 "${out}" "${offset}" $((value))
  write_u8 "${out}" $((offset + 1)) $((value >> 8))
}

write_u32_le() {
  local out="$1"
  local offset="$2"
  local value="$3"
  write_u8 "${out}" "${offset}" $((value))
  write_u8 "${out}" $((offset + 1)) $((value >> 8))
  write_u8 "${out}" $((offset + 2)) $((value >> 16))
  write_u8 "${out}" $((offset + 3)) $((value >> 24))
}

write_ascii() {
  local out="$1"
  local offset="$2"
  local text="$3"
  printf '%s' "${text}" | dd of="${out}" bs=1 seek="$((offset))" conv=notrunc status=none
}

ASM_OUT=""
ASM_PC=0
ASM_LIT_NEXT=0
LIT_OFFSETS=()
LIT_VALUES=()

asm_begin_rom() {
  local out="$1"
  local title="$2"
  local game_code="$3"

  dd if=/dev/zero of="${out}" bs=1 count=2048 status=none

  # Cartridge entry branch to payload at 0x100.
  write_u32_le "${out}" 0x00 0xEA00003E
  write_ascii "${out}" 0xA0 "${title}"
  write_ascii "${out}" 0xAC "${game_code}"
  write_u8 "${out}" 0xB2 0x96

  ASM_OUT="${out}"
  ASM_PC=0x100
  ASM_LIT_NEXT=0x300
  LIT_OFFSETS=()
  LIT_VALUES=()
}

asm_finalize_rom() {
  local i
  for ((i = 0; i < ${#LIT_OFFSETS[@]}; ++i)); do
    write_u32_le "${ASM_OUT}" "${LIT_OFFSETS[$i]}" "${LIT_VALUES[$i]}"
  done
}

asm_seek() {
  ASM_PC="$1"
}

asm_alloc_lit() {
  local out_var="$1"
  local value="$2"
  local off="${ASM_LIT_NEXT}"
  LIT_OFFSETS+=("${off}")
  LIT_VALUES+=("$((value & 0xFFFFFFFF))")
  ASM_LIT_NEXT=$((ASM_LIT_NEXT + 4))
  printf -v "${out_var}" '%d' "$((0x08000000 + off))"
}

asm_emit32() {
  local value="$1"
  write_u32_le "${ASM_OUT}" "${ASM_PC}" "$((value & 0xFFFFFFFF))"
  ASM_PC=$((ASM_PC + 4))
}

asm_emit16() {
  local value="$1"
  write_u16_le "${ASM_OUT}" "${ASM_PC}" "$((value & 0xFFFF))"
  ASM_PC=$((ASM_PC + 2))
}

asm_emit_ldr_lit() {
  local rd="$1"
  local lit_addr="$2"
  local cond="${3:-14}" # AL
  local pc_abs=$((0x08000000 + ASM_PC))
  local rel=$((lit_addr - (pc_abs + 8)))
  if ((rel < 0 || rel > 0xFFF)); then
    echo "Literal out of range: pc=0x$(printf '%x' "${ASM_PC}") rel=${rel}" >&2
    exit 1
  fi
  asm_emit32 $(((cond << 28) | 0x059F0000 | (rd << 12) | rel))
}

asm_emit_mov_imm() {
  local rd="$1"
  local imm="$2"
  local cond="${3:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x03A00000 | (rd << 12) | (imm & 0xFF)))
}

asm_emit_mov_reg() {
  local rd="$1"
  local rm="$2"
  local cond="${3:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x01A00000 | (rd << 12) | rm))
}

asm_emit_cmp_imm() {
  local rn="$1"
  local imm="$2"
  local cond="${3:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x03500000 | (rn << 16) | (imm & 0xFF)))
}

asm_emit_cmp_reg() {
  local rn="$1"
  local rm="$2"
  local cond="${3:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x01500000 | (rn << 16) | rm))
}

asm_emit_ldr_imm() {
  local rd="$1"
  local rn="$2"
  local imm="$3"
  local cond="${4:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x05900000 | (rn << 16) | (rd << 12) | (imm & 0xFFF)))
}

asm_emit_str_imm() {
  local rd="$1"
  local rn="$2"
  local imm="$3"
  local cond="${4:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x05800000 | (rn << 16) | (rd << 12) | (imm & 0xFFF)))
}

asm_emit_strb_imm() {
  local rd="$1"
  local rn="$2"
  local imm="$3"
  local cond="${4:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x05C00000 | (rn << 16) | (rd << 12) | (imm & 0xFFF)))
}

asm_emit_ldrh_imm0() {
  local rd="$1"
  local rn="$2"
  local cond="${3:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x01D000B0 | (rn << 16) | (rd << 12)))
}

asm_emit_strh_imm0() {
  local rd="$1"
  local rn="$2"
  local cond="${3:-14}" # AL
  asm_emit32 $(((cond << 28) | 0x01C000B0 | (rn << 16) | (rd << 12)))
}

asm_emit_and_reg() {
  local rd="$1"
  local rn="$2"
  local rm="$3"
  local cond="${4:-14}" # AL
  asm_emit32 $(((cond << 28) | (rn << 16) | (rd << 12) | rm))
}

asm_emit_swi() {
  local imm="$1"
  # BIOS SWI dispatch on ARM expects the SWI ID in bits 23:16
  # (`swi 0xXX0000`), not in the low byte.
  asm_emit32 $((0xEF000000 | ((imm & 0xFF) << 16)))
}

asm_emit_nop() {
  asm_emit32 0xE1A00000
}

asm_emit_mgba_verdict() {
  # Requires:
  # - r4 = 0x04FFF600 (mGBA debug string base)
  # - condition flags from `cmp r8, #0` where Z=1 means PASS and Z=0 means FAIL.
  asm_emit_mov_imm 1 0xDE
  asm_emit_strb_imm 1 4 0x180
  asm_emit_mov_imm 1 0xC0
  asm_emit_strb_imm 1 4 0x181

  asm_emit_mov_imm 1 0x50 # P
  asm_emit_strb_imm 1 4 0x000
  asm_emit_mov_imm 1 0x41 # A
  asm_emit_strb_imm 1 4 0x001
  asm_emit_mov_imm 1 0x53 # S
  asm_emit_strb_imm 1 4 0x002
  asm_emit_mov_imm 1 0x53 # S
  asm_emit_strb_imm 1 4 0x003

  asm_emit_mov_imm 1 0x46 1 # NE: F
  asm_emit_strb_imm 1 4 0x000 1
  asm_emit_mov_imm 1 0x41 1 # NE: A
  asm_emit_strb_imm 1 4 0x001 1
  asm_emit_mov_imm 1 0x49 1 # NE: I
  asm_emit_strb_imm 1 4 0x002 1
  asm_emit_mov_imm 1 0x4C 1 # NE: L
  asm_emit_strb_imm 1 4 0x003 1

  asm_emit_mov_imm 1 0x00
  asm_emit_strb_imm 1 4 0x004
  asm_emit_mov_imm 1 0x01
  asm_emit_strb_imm 1 4 0x100
  asm_emit32 0xEAFFFFFE # b .
}

generate_smoke_rom() {
  local out="${OUT_DIR}/gen_smoke.gba"
  asm_begin_rom "${out}" "MGBASMOKETST" "MGSM"

  local lit_mgba lit_mode lit_dispcnt
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_mode 0x00000403 # Mode 3 + BG2
  asm_alloc_lit lit_dispcnt 0x04000000

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_mov_imm 0 2
  asm_emit_mov_imm 1 3
  asm_emit32 0xE0802001 # add r2, r0, r1
  asm_emit_cmp_imm 2 5
  asm_emit_mov_imm 8 1 1 # movne r8,#1

  asm_emit_ldr_lit 1 "${lit_mode}"
  asm_emit_ldr_lit 2 "${lit_dispcnt}"
  asm_emit_str_imm 1 2 0
  asm_emit_ldr_imm 3 2 0
  asm_emit_cmp_reg 3 1
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_smoke_io_rom() {
  local out="${OUT_DIR}/gen_smoke_io.gba"
  asm_begin_rom "${out}" "MGBASMOKEIO" "MGSI"

  local lit_mgba lit_waitcnt lit_wait_a lit_wait_b lit_ime lit_ffff
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_waitcnt 0x04000204
  asm_alloc_lit lit_wait_a 0x00004030
  asm_alloc_lit lit_wait_b 0x00000060
  asm_alloc_lit lit_ime 0x04000208
  asm_alloc_lit lit_ffff 0x0000FFFF

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 1 "${lit_waitcnt}"
  asm_emit_ldr_lit 0 "${lit_wait_a}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldrh_imm0 3 1
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 0 "${lit_wait_b}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldrh_imm0 3 1
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 2 "${lit_ime}"
  asm_emit_ldr_lit 0 "${lit_ffff}"
  asm_emit_str_imm 0 2 0
  asm_emit_ldrh_imm0 3 2
  asm_emit_cmp_imm 3 1
  asm_emit_mov_imm 8 1 1

  asm_emit_mov_imm 0 0
  asm_emit_str_imm 0 2 0
  asm_emit_ldrh_imm0 3 2
  asm_emit_cmp_imm 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_cpu_arm_rom() {
  local out="${OUT_DIR}/gen_cpu_arm.gba"
  asm_begin_rom "${out}" "MGBACPUARMT" "MGCA"

  local lit_mgba lit_addr lit_new lit_old lit_old2 lit_new2
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_addr 0x02000100
  asm_alloc_lit lit_new 0x11223344
  asm_alloc_lit lit_old 0xAABBCCDD
  asm_alloc_lit lit_old2 0xAABBCCDD
  asm_alloc_lit lit_new2 0x11223344

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_ldr_lit 2 "${lit_addr}"
  asm_emit_ldr_lit 1 "${lit_new}"
  asm_emit_ldr_lit 0 "${lit_old}"
  asm_emit_str_imm 0 2 0
  asm_emit32 0xE1023091 # swp r3, r1, [r2]
  asm_emit_ldr_imm 0 2 0
  asm_emit_ldr_lit 5 "${lit_old2}"
  asm_emit_cmp_reg 3 5
  asm_emit_mov_imm 8 1 1
  asm_emit_ldr_lit 5 "${lit_new2}"
  asm_emit_cmp_reg 0 5
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_cpu_arm_mul_rom() {
  local out="${OUT_DIR}/gen_cpu_arm_mul.gba"
  asm_begin_rom "${out}" "MGBAARMMUL" "MGCM"

  local lit_mgba lit_neg1 lit_two lit_umull_lo lit_umull_hi lit_acc lit_smlal_lo lit_smlal_hi
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_neg1 0xFFFFFFFF
  asm_alloc_lit lit_two 0x00000002
  asm_alloc_lit lit_umull_lo 0xFFFFFFFE
  asm_alloc_lit lit_umull_hi 0x00000001
  asm_alloc_lit lit_acc 0x00000003
  asm_alloc_lit lit_smlal_lo 0x00000001
  asm_alloc_lit lit_smlal_hi 0x00000000

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 2 "${lit_neg1}"
  asm_emit_ldr_lit 3 "${lit_two}"
  asm_emit32 0xE0810392 # UMULL r0, r1, r2, r3
  asm_emit_ldr_lit 5 "${lit_umull_lo}"
  asm_emit_cmp_reg 0 5
  asm_emit_mov_imm 8 1 1
  asm_emit_ldr_lit 5 "${lit_umull_hi}"
  asm_emit_cmp_reg 1 5
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 0 "${lit_acc}"
  asm_emit_mov_imm 1 0
  asm_emit_ldr_lit 2 "${lit_neg1}"
  asm_emit_ldr_lit 3 "${lit_two}"
  asm_emit32 0xE0E10392 # SMLAL r0, r1, r2, r3
  asm_emit_ldr_lit 5 "${lit_smlal_lo}"
  asm_emit_cmp_reg 0 5
  asm_emit_mov_imm 8 1 1
  asm_emit_ldr_lit 5 "${lit_smlal_hi}"
  asm_emit_cmp_reg 1 5
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_cpu_thumb_rom() {
  local out="${OUT_DIR}/gen_cpu_thumb.gba"
  asm_begin_rom "${out}" "MGBACPUTHMB" "MGCT"

  local lit_mgba lit_thumb lit_resume
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_thumb 0x080001A1
  asm_alloc_lit lit_resume 0x08000118

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_ldr_lit 0 "${lit_thumb}"
  asm_emit_ldr_lit 1 "${lit_resume}"
  asm_emit32 0xE12FFF10 # bx r0
  asm_emit_nop
  asm_emit_cmp_imm 2 0x2A
  asm_emit_mov_imm 8 1 1
  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict

  asm_seek 0x1A0
  asm_emit16 0x222A # movs r2, #0x2A
  asm_emit16 0x4708 # bx r1
  asm_emit16 0x46C0 # nop
  asm_finalize_rom
}

generate_cpu_thumb_add_rom() {
  local out="${OUT_DIR}/gen_cpu_thumb_add.gba"
  asm_begin_rom "${out}" "MGBATHMADDS" "MGTA"

  local lit_mgba lit_thumb lit_resume
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_thumb 0x080001C1
  asm_alloc_lit lit_resume 0x08000118

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_ldr_lit 0 "${lit_thumb}"
  asm_emit_ldr_lit 1 "${lit_resume}"
  asm_emit32 0xE12FFF10 # bx r0
  asm_emit_nop
  asm_emit_cmp_imm 2 0x2A
  asm_emit_mov_imm 8 1 1
  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict

  asm_seek 0x1C0
  asm_emit16 0x2220 # movs r2, #0x20
  asm_emit16 0x320A # adds r2, #0x0A
  asm_emit16 0x4708 # bx r1
  asm_emit16 0x46C0 # nop
  asm_finalize_rom
}

generate_dma_timer_rom() {
  local out="${OUT_DIR}/gen_dma_timer.gba"
  asm_begin_rom "${out}" "MGBADMATIMER" "MGDT"

  local lit_mgba lit_dma lit_src lit_dst lit_expected lit_ctrl lit_tm0 lit_tm0_reload
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_dma 0x040000B0
  asm_alloc_lit lit_src 0x02000000
  asm_alloc_lit lit_dst 0x02000020
  asm_alloc_lit lit_expected 0x89ABCDEF
  asm_alloc_lit lit_ctrl 0x84000001
  asm_alloc_lit lit_tm0 0x04000100
  asm_alloc_lit lit_tm0_reload 0x00001234

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 6 "${lit_dma}"
  asm_emit_ldr_lit 5 "${lit_src}"
  asm_emit_ldr_lit 7 "${lit_dst}"
  asm_emit_ldr_lit 0 "${lit_expected}"
  asm_emit_str_imm 0 5 0
  asm_emit_str_imm 5 6 0
  asm_emit_str_imm 7 6 4
  asm_emit_ldr_lit 0 "${lit_ctrl}"
  asm_emit_str_imm 0 6 8
  asm_emit_ldr_imm 1 7 0
  asm_emit_ldr_lit 2 "${lit_expected}"
  asm_emit_cmp_reg 1 2
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 9 "${lit_tm0}"
  asm_emit_ldr_lit 0 "${lit_tm0_reload}"
  asm_emit_str_imm 0 9 0
  asm_emit_ldr_imm 3 9 0
  asm_emit_ldr_lit 2 "${lit_tm0_reload}"
  asm_emit_cmp_reg 3 2
  asm_emit_mov_imm 8 1 1 # movne r8,#1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_dma_timer_irq_rom() {
  local out="${OUT_DIR}/gen_dma_timer_irq.gba"
  asm_begin_rom "${out}" "MGBADTIRQ" "MGDI"

  local lit_mgba lit_dma lit_src lit_dst lit_expected lit_ctrl lit_if lit_dma0_mask
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_dma 0x040000B0
  asm_alloc_lit lit_src 0x02000080
  asm_alloc_lit lit_dst 0x020000C0
  asm_alloc_lit lit_expected 0x1234ABCD
  asm_alloc_lit lit_ctrl 0xC4000001
  asm_alloc_lit lit_if 0x04000202
  asm_alloc_lit lit_dma0_mask 0x00000100

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 6 "${lit_dma}"
  asm_emit_ldr_lit 5 "${lit_src}"
  asm_emit_ldr_lit 7 "${lit_dst}"
  asm_emit_ldr_lit 0 "${lit_expected}"
  asm_emit_str_imm 0 5 0
  asm_emit_str_imm 5 6 0
  asm_emit_str_imm 7 6 4
  asm_emit_ldr_lit 0 "${lit_ctrl}"
  asm_emit_str_imm 0 6 8

  asm_emit_ldr_imm 1 7 0
  asm_emit_ldr_lit 2 "${lit_expected}"
  asm_emit_cmp_reg 1 2
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 2 "${lit_if}"
  asm_emit_ldrh_imm0 3 2
  asm_emit_ldr_lit 5 "${lit_dma0_mask}"
  asm_emit_and_reg 6 3 5
  asm_emit_cmp_reg 6 5
  asm_emit_mov_imm 8 1 1

  asm_emit_strh_imm0 5 2
  asm_emit_ldrh_imm0 3 2
  asm_emit_and_reg 6 3 5
  asm_emit_cmp_imm 6 0
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_mem_timing_rom() {
  local out="${OUT_DIR}/gen_mem_timing.gba"
  asm_begin_rom "${out}" "MGBAMEMTIME" "MGMT"

  local lit_mgba lit_waitcnt lit_ws0 lit_ws12
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_waitcnt 0x04000204
  asm_alloc_lit lit_ws0 0x00004030
  asm_alloc_lit lit_ws12 0x00001E60

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_ldr_lit 1 "${lit_waitcnt}"

  asm_emit_ldr_lit 0 "${lit_ws0}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldr_imm 3 1 0
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 0 "${lit_ws12}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldr_imm 3 1 0
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_mem_timing_prefetch_rom() {
  local out="${OUT_DIR}/gen_mem_timing_prefetch.gba"
  asm_begin_rom "${out}" "MGBAMEMPREF" "MGMP"

  local lit_mgba lit_waitcnt lit_pref_on lit_pref_off lit_pref_mask
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_waitcnt 0x04000204
  asm_alloc_lit lit_pref_on 0x00004030
  asm_alloc_lit lit_pref_off 0x00000030
  asm_alloc_lit lit_pref_mask 0x00004000

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_ldr_lit 1 "${lit_waitcnt}"

  asm_emit_ldr_lit 0 "${lit_pref_on}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldrh_imm0 3 1
  asm_emit_ldr_lit 5 "${lit_pref_mask}"
  asm_emit_and_reg 6 3 5
  asm_emit_cmp_reg 6 5
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 0 "${lit_pref_off}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldrh_imm0 3 1
  asm_emit_and_reg 6 3 5
  asm_emit_cmp_imm 6 0
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_ppu_rom() {
  local out="${OUT_DIR}/gen_ppu.gba"
  asm_begin_rom "${out}" "MGBAPPUCHECK" "MGPP"

  local lit_mgba lit_dispcnt lit_winin lit_bldcnt lit_mode5 lit_page1 lit_win_val lit_bld_val
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_dispcnt 0x04000000
  asm_alloc_lit lit_winin 0x04000048
  asm_alloc_lit lit_bldcnt 0x04000050
  asm_alloc_lit lit_mode5 0x00000405
  asm_alloc_lit lit_page1 0x00000415
  asm_alloc_lit lit_win_val 0x00003F3F
  asm_alloc_lit lit_bld_val 0x00003F40

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 1 "${lit_dispcnt}"
  asm_emit_ldr_lit 0 "${lit_mode5}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldr_imm 3 1 0
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 0 "${lit_page1}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldr_imm 3 1 0
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 1 "${lit_winin}"
  asm_emit_ldr_lit 0 "${lit_win_val}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldr_imm 3 1 0
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 1 "${lit_bldcnt}"
  asm_emit_ldr_lit 0 "${lit_bld_val}"
  asm_emit_str_imm 0 1 0
  asm_emit_ldr_imm 3 1 0
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_ppu_window_rom() {
  local out="${OUT_DIR}/gen_ppu_window.gba"
  asm_begin_rom "${out}" "MGBAPPUWIN" "MGPW"

  local lit_mgba lit_winout lit_bldy lit_winout_val lit_bldy_val lit_dispcnt lit_mode3
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_winout 0x0400004A
  asm_alloc_lit lit_bldy 0x04000054
  asm_alloc_lit lit_winout_val 0x00003F3F
  asm_alloc_lit lit_bldy_val 0x00000010
  asm_alloc_lit lit_dispcnt 0x04000000
  asm_alloc_lit lit_mode3 0x00002403

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 1 "${lit_dispcnt}"
  asm_emit_ldr_lit 0 "${lit_mode3}"
  asm_emit_strh_imm0 0 1
  asm_emit_ldrh_imm0 3 1
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 1 "${lit_winout}"
  asm_emit_ldr_lit 0 "${lit_winout_val}"
  asm_emit_strh_imm0 0 1
  asm_emit_ldrh_imm0 3 1
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 1 "${lit_bldy}"
  asm_emit_ldr_lit 0 "${lit_bldy_val}"
  asm_emit_strh_imm0 0 1
  asm_emit_ldrh_imm0 3 1
  asm_emit_cmp_reg 3 0
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_swi_bios_rom() {
  local out="${OUT_DIR}/gen_swi_bios.gba"
  asm_begin_rom "${out}" "MGBASWIBIOS" "MGSB"

  local lit_mgba lit_num lit_den lit_q lit_r lit_sqrt_in
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_num 0x00000007
  asm_alloc_lit lit_den 0x00000002
  asm_alloc_lit lit_q 0x00000003
  asm_alloc_lit lit_r 0x00000001
  asm_alloc_lit lit_sqrt_in 0x00000051 # 81

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 0 "${lit_num}"
  asm_emit_ldr_lit 1 "${lit_den}"
  asm_emit_swi 0x06
  asm_emit_ldr_lit 2 "${lit_q}"
  asm_emit_cmp_reg 0 2
  asm_emit_mov_imm 8 1 1
  asm_emit_ldr_lit 2 "${lit_r}"
  asm_emit_cmp_reg 1 2
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 0 "${lit_sqrt_in}"
  asm_emit_swi 0x08
  asm_emit_cmp_imm 0 9
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_swi_bios_divarm_rom() {
  local out="${OUT_DIR}/gen_swi_bios_divarm.gba"
  asm_begin_rom "${out}" "MGBASWIDVAR" "MGSD"

  local lit_mgba lit_den lit_num lit_q lit_r lit_sqrt
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_den 0xFFFFFFF9
  asm_alloc_lit lit_num 0x00000028
  asm_alloc_lit lit_q 0xFFFFFFFB
  asm_alloc_lit lit_r 0x00000005
  asm_alloc_lit lit_sqrt 0x00000100

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0

  asm_emit_ldr_lit 0 "${lit_den}"
  asm_emit_ldr_lit 1 "${lit_num}"
  asm_emit_swi 0x07
  asm_emit_ldr_lit 2 "${lit_q}"
  asm_emit_cmp_reg 0 2
  asm_emit_mov_imm 8 1 1
  asm_emit_ldr_lit 2 "${lit_r}"
  asm_emit_cmp_reg 1 2
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_lit 0 "${lit_sqrt}"
  asm_emit_swi 0x08
  asm_emit_cmp_imm 0 16
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_swi_compat_rom() {
  local out="${OUT_DIR}/gen_swi_compat.gba"
  asm_begin_rom "${out}" "MGBASWICOMP" "MGSC"

  local lit_mgba lit_src lit_dst lit_v0 lit_v1 lit_ctrl
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_src 0x02000080
  asm_alloc_lit lit_dst 0x02000100
  asm_alloc_lit lit_v0 0x11223344
  asm_alloc_lit lit_v1 0x55667788
  asm_alloc_lit lit_ctrl 0x04000002 # CpuSet: 32-bit, 2 units

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_ldr_lit 0 "${lit_src}"
  asm_emit_ldr_lit 1 "${lit_dst}"
  asm_emit_ldr_lit 3 "${lit_v0}"
  asm_emit_str_imm 3 0 0
  asm_emit_ldr_lit 3 "${lit_v1}"
  asm_emit_str_imm 3 0 4
  asm_emit_ldr_lit 2 "${lit_ctrl}"
  asm_emit_swi 0x0B

  # Real BIOS CpuSet advances R0/R1; reload destination base for verification.
  asm_emit_ldr_lit 6 "${lit_dst}"
  asm_emit_ldr_imm 3 6 0
  asm_emit_ldr_lit 5 "${lit_v0}"
  asm_emit_cmp_reg 3 5
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_imm 3 6 4
  asm_emit_ldr_lit 5 "${lit_v1}"
  asm_emit_cmp_reg 3 5
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_swi_compat_fastset_rom() {
  local out="${OUT_DIR}/gen_swi_compat_fastset.gba"
  asm_begin_rom "${out}" "MGBASWIFAST" "MGSF"

  local lit_mgba lit_src lit_dst lit_fill lit_ctrl
  asm_alloc_lit lit_mgba 0x04FFF600
  asm_alloc_lit lit_src 0x02000200
  asm_alloc_lit lit_dst 0x02000280
  asm_alloc_lit lit_fill 0xCAFEBABE
  asm_alloc_lit lit_ctrl 0x01000008

  asm_emit_ldr_lit 4 "${lit_mgba}"
  asm_emit_mov_imm 8 0
  asm_emit_ldr_lit 0 "${lit_src}"
  asm_emit_ldr_lit 1 "${lit_dst}"
  asm_emit_ldr_lit 3 "${lit_fill}"
  asm_emit_str_imm 3 0 0
  asm_emit_ldr_lit 2 "${lit_ctrl}"
  asm_emit_swi 0x0C

  asm_emit_ldr_lit 6 "${lit_dst}"
  asm_emit_ldr_imm 3 6 0
  asm_emit_ldr_lit 5 "${lit_fill}"
  asm_emit_cmp_reg 3 5
  asm_emit_mov_imm 8 1 1

  asm_emit_ldr_imm 3 6 28
  asm_emit_cmp_reg 3 5
  asm_emit_mov_imm 8 1 1

  asm_emit_cmp_imm 8 0
  asm_emit_mgba_verdict
  asm_finalize_rom
}

generate_smoke_rom
generate_smoke_io_rom
generate_cpu_arm_rom
generate_cpu_arm_mul_rom
generate_cpu_thumb_rom
generate_cpu_thumb_add_rom
generate_dma_timer_rom
generate_dma_timer_irq_rom
generate_mem_timing_rom
generate_mem_timing_prefetch_rom
generate_ppu_rom
generate_ppu_window_rom
generate_swi_bios_rom
generate_swi_bios_divarm_rom
generate_swi_compat_rom
generate_swi_compat_fastset_rom

echo "Generated GBA conformance ROMs in ${OUT_DIR}"
