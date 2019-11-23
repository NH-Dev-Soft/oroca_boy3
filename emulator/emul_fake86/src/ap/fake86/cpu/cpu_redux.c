/*
  Fake86: A portable, open-source 8086 PC emulator.
  Copyright (C)2019      Aidan Dodds

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
  USA.
*/

#ifdef _MSC_VER
#include <intrin.h>
#else
//#include <x86intrin.h>
#endif

#include "cpu_priv.h"
#include "cpu_mod_rm.h"


//
static bool _did_run_temp = true;
static uint8_t _seg_ovr;

// forward declare opcode table
typedef void (*opcode_t)(const uint8_t *code);
static const opcode_t _op_table[256];

// shift register used to delay STI until next instruction
static uint8_t _sti_sr = 0;

#define OPCODE(NAME)                                                          \
  static void NAME (const uint8_t *code)


struct cpu_io_t _cpu_io;

void cpu_set_io(const struct cpu_io_t *io) {
  memcpy(&_cpu_io, io, sizeof(struct cpu_io_t));
}


// raise an interupt
static inline void _raise_int(uint8_t num) {
  _cpu_io.int_call(num);
}

// effective instruction pointer
static inline uint32_t _eip(void) {
  return (cpu_regs.cs << 4) + cpu_regs.ip;
}

// effective stack pointer
static inline uint32_t _esp(void) {
  return (cpu_regs.ss << 4) + cpu_regs.sp;
}

// push byte to stack
static inline void _push_b(const uint8_t val) {
  cpu_regs.sp -= 1;
  _cpu_io.mem_write_8(_esp(), val);
}

// push word to stack
static inline void _push_w(const uint16_t val) {
  cpu_regs.sp -= 2;
  _cpu_io.mem_write_16(_esp(), val);
}

// pop byte from stack
static inline uint8_t _pop_b(void) {
  const uint8_t out = _cpu_io.mem_read_8(_esp());
  cpu_regs.sp += 1;
  return out;
}

// pop word from stack
static inline uint16_t _pop_w(void) {
  const uint16_t out = _cpu_io.mem_read_16(_esp());
  cpu_regs.sp += 2;
  return out;
}

// step instruction pointer
static inline void _step_ip(const int16_t rel) {
  cpu_regs.ip += rel;
}

// set parity flag
static inline void _set_pf(uint16_t val) {
  val &= 0xff;
#ifdef _MSC_VER
  cpu_flags.pf = ((~__popcnt16(val)) & 1);
#else
  cpu_flags.pf = __builtin_parity(val);
#endif
}

// get parity flag
static inline uint8_t _get_pf(void) {
  return cpu_flags.pf;
}

// set zero and sign flags
static inline void _set_zf_sf_b(const uint8_t val) {
  cpu_flags.zf = (val == 0);
  cpu_flags.sf = (val & 0x80) ? 1 : 0;
}

// set zero and sign flags
static inline void _set_zf_sf_w(const uint16_t val) {
  cpu_flags.zf = (val == 0);
  cpu_flags.sf = (val & 0x8000) ? 1 : 0;
}

uint16_t cpu_get_flags(void) {
  return
    (cpu_flags.cf  ? 0x0001 : 0) |
    (cpu_flags.pf  ? 0x0004 : 0) |
    (cpu_flags.af  ? 0x0010 : 0) |
    (cpu_flags.zf  ? 0x0040 : 0) |
    (cpu_flags.sf  ? 0x0080 : 0) |
    (cpu_flags.tf  ? 0x0100 : 0) |
    (cpu_flags.ifl ? 0x0200 : 0) |
    (cpu_flags.df  ? 0x0400 : 0) |
    (cpu_flags.of  ? 0x0800 : 0);
}

void cpu_set_flags(const uint16_t f) {
  cpu_flags.cf  = (f & 0x0001) ? 1 : 0;
  cpu_flags.pf  = (f & 0x0004) ? 1 : 0;
  cpu_flags.af  = (f & 0x0010) ? 1 : 0;
  cpu_flags.zf  = (f & 0x0040) ? 1 : 0;
  cpu_flags.sf  = (f & 0x0080) ? 1 : 0;
  cpu_flags.tf  = (f & 0x0100) ? 1 : 0;
  cpu_flags.ifl = (f & 0x0200) ? 1 : 0;
  cpu_flags.df  = (f & 0x0400) ? 1 : 0;
  cpu_flags.of  = (f & 0x0800) ? 1 : 0;
}

void cpu_mod_flags(uint16_t in, uint16_t mask) {
  const uint16_t cur = cpu_get_flags();
  const uint16_t res = (in & mask) | (cur & ~mask);
  cpu_set_flags(res);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define ADD_FLAGS_B(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_b(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = ((lhs + rhs) > 0xff) ? 1 : 0;                              \
    cpu_flags.af = ((res ^ lhs ^ rhs)         & 0x10) ? 1 : 0;                \
    cpu_flags.of = ((res ^ lhs) & (res ^ rhs) & 0x80) ? 1 : 0;                \
  }

#define ADD_FLAGS_W(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_w(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = ((lhs + rhs) > 0xffff) ? 1 : 0;                            \
    cpu_flags.af = ((res ^ lhs ^ rhs)         & 0x10)   ? 1 : 0;              \
    cpu_flags.of = ((res ^ lhs) & (res ^ rhs) & 0x8000) ? 1 : 0;              \
  }

// ADD m/r, reg  (byte)
OPCODE(_00) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t tmp = lhs + rhs;
  ADD_FLAGS_B(lhs, rhs, tmp);
  _write_rm_b(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// ADD m/r, reg  (word)
OPCODE(_01) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t tmp = lhs + rhs;
  ADD_FLAGS_W(lhs, rhs, tmp);
  _write_rm_w(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// ADD reg, m/r  (byte)
OPCODE(_02) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint8_t tmp = lhs + rhs;
  ADD_FLAGS_B(lhs, rhs, tmp);
  _set_reg_b(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// ADD reg, m/r  (word)
OPCODE(_03) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint16_t tmp = lhs + rhs;
  ADD_FLAGS_W(lhs, rhs, tmp);
  _set_reg_w(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// ADD al, imm8
OPCODE(_04) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1);
  const uint8_t tmp = lhs + rhs;
  ADD_FLAGS_B(lhs, rhs, tmp);
  cpu_regs.al = tmp;
  _step_ip(2);
}

// ADD ax, imm16
OPCODE(_05) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1);
  const uint16_t tmp = lhs + rhs;
  ADD_FLAGS_W(lhs, rhs, tmp);
  cpu_regs.ax = tmp;
  _step_ip(3);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// PUSH ES - push segment register ES
OPCODE(_06) {
  _push_w(cpu_regs.es);
  _step_ip(1);
}

// POP ES - pop segment register ES
OPCODE(_07) {
  cpu_regs.es = _pop_w();
  _step_ip(1);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define OR_FLAGS_B(lhs, rhs, res)                                             \
  {                                                                           \
    _set_zf_sf_b(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

#define OR_FLAGS_W(lhs, rhs, res)                                             \
  {                                                                           \
    _set_zf_sf_w(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

// OR m/r, reg  (byte)
OPCODE(_08) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t tmp = lhs | rhs;
  OR_FLAGS_B(lhs, rhs, tmp);
  _write_rm_b(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// OR m/r, reg  (word)
OPCODE(_09) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t tmp = lhs | rhs;
  OR_FLAGS_W(lhs, rhs, tmp);
  _write_rm_w(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// OR reg, m/r  (byte)
OPCODE(_0A) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint8_t tmp = lhs | rhs;
  OR_FLAGS_B(lhs, rhs, tmp);
  _set_reg_b(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// OR reg, m/r  (word)
OPCODE(_0B) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint16_t tmp = lhs | rhs;
  OR_FLAGS_W(lhs, rhs, tmp);
  _set_reg_w(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// OR al, imm8
OPCODE(_0C) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1);
  const uint8_t tmp = lhs | rhs;
  OR_FLAGS_B(lhs, rhs, tmp);
  cpu_regs.al = tmp;
  _step_ip(2);
}

// OR ax, imm16
OPCODE(_0D) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1);
  const uint16_t tmp = lhs | rhs;
  OR_FLAGS_W(lhs, rhs, tmp);
  cpu_regs.ax = tmp;
  _step_ip(3);
}

#undef OR_FLAGS_B
#undef OR_FLAGS_W

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// PUSH CS - push segment register CS
OPCODE(_0E) {
  _push_w(cpu_regs.cs);
  _step_ip(1);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define ADC_FLAGS_B(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_b(res & 0xff);                                                 \
    _set_pf(res & 0xff);                                                      \
    cpu_flags.cf = (res > 0xff) ? 1 : 0;                                      \
    cpu_flags.af = (((res ^ lhs ^ rhs)         & 0x10) == 0x10) ? 1 : 0;      \
    cpu_flags.of = (((res ^ lhs) & (res ^ rhs) & 0x80) == 0x80) ? 1 : 0;      \
  }

#define ADC_FLAGS_W(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_w(res & 0xffff);                                               \
    _set_pf(res & 0xffff);                                                    \
    cpu_flags.cf = (res > 0xffff) ? 1 : 0;                                    \
    cpu_flags.af = (((res ^ lhs ^ rhs)         & 0x10) == 0x10) ? 1 : 0;      \
    cpu_flags.of = (((res ^ lhs) & (res ^ rhs) & 0x8000) == 0x8000) ? 1 : 0;  \
  }

// ADC m/r, reg  (byte)
OPCODE(_10) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint16_t tmp = lhs + rhs + cpu_flags.cf;
  ADC_FLAGS_B(lhs, rhs, tmp);
  _write_rm_b(&m, (uint8_t)tmp);
  _step_ip(1 + m.num_bytes);
}

// ADC m/r, reg  (word)
OPCODE(_11) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint32_t tmp = lhs + rhs + cpu_flags.cf;
  ADC_FLAGS_W(lhs, rhs, tmp);
  _write_rm_w(&m, (uint16_t)tmp);
  _step_ip(1 + m.num_bytes);
}

// ADC reg, m/r  (byte)
OPCODE(_12) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint16_t tmp = lhs + rhs + cpu_flags.cf;
  ADC_FLAGS_B(lhs, rhs, tmp);
  _set_reg_b(m.reg, (uint8_t)tmp);
  _step_ip(1 + m.num_bytes);
}

// ADC reg, m/r  (word)
OPCODE(_13) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint32_t tmp = lhs + rhs + cpu_flags.cf;
  ADC_FLAGS_W(lhs, rhs, tmp);
  _set_reg_w(m.reg, (uint16_t)tmp);
  _step_ip(1 + m.num_bytes);
}

// ADC al, imm8
OPCODE(_14) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1);
  const uint16_t tmp = lhs + rhs + cpu_flags.cf;
  ADC_FLAGS_B(lhs, rhs, tmp);
  cpu_regs.al = (uint8_t)tmp;
  _step_ip(2);
}

// ADC ax, imm16
OPCODE(_15) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1);
  const uint32_t tmp = lhs + rhs + cpu_flags.cf;
  ADC_FLAGS_W(lhs, rhs, tmp);
  cpu_regs.ax = (uint16_t)tmp;
  _step_ip(3);
}

#undef ADC_FLAGS_B
#undef ADC_FLAGS_W

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- 

// PUSH SS - push segment register SS
OPCODE(_16) {
  _push_w(cpu_regs.ss);
  _step_ip(1);
}

// POP SS - pop segment register SS
OPCODE(_17) {
  cpu_regs.ss = _pop_w();
  _step_ip(1);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

static inline uint8_t _do_sbb_b(const uint8_t lhs, const uint8_t rhs, const uint8_t c) {
#if 0
  uint8_t tmp = 0;
  uint16_t f = 0;
  __asm {
  __asm mov al, c
  __asm test al, al
  __asm clc
  __asm jz do_sbb
  __asm stc
  __asm do_sbb:
  __asm mov al, lhs
  __asm mov bl, rhs
  __asm sbb al, bl
  __asm pushf
  __asm mov tmp, al
  __asm pop ax
  __asm mov f, ax
  };
  cpu_mod_flags(f, CF | OF | SF | ZF | AF | PF);
#else
  //XXX: rhs += cpu_flags.cf
  const uint8_t tmp = lhs - (rhs + cpu_flags.cf);
  _set_zf_sf_b(tmp);
  _set_pf(tmp);
  cpu_flags.cf = (lhs < (rhs + cpu_flags.cf)) ? 1 : 0;
  cpu_flags.af = ((tmp ^ lhs ^ rhs) & 0x10) ? 1 : 0;
  const uint16_t X = (uint16_t)((int16_t)lhs - (int16_t)rhs);
  cpu_flags.of = ((tmp ^ lhs) & (lhs ^ rhs) & 0x80) ? 1 : 0;
#endif
  return tmp;
}

static inline uint16_t _do_sbb_w(const uint16_t lhs, const uint16_t rhs, const uint8_t c) {
#if 0
  uint16_t tmp = 0;
  uint16_t f = 0;
  __asm {
  __asm mov al, c
  __asm test al, al
  __asm clc
  __asm jz do_sbb
  __asm stc
  __asm do_sbb:
  __asm mov ax, lhs
  __asm mov bx, rhs
  __asm sbb ax, bx
  __asm pushf
  __asm mov tmp, ax
  __asm pop ax
  __asm mov f, ax
  };
  cpu_mod_flags(f, CF | OF | SF | ZF | AF | PF);
#else
  //XXX: rhs += cpu_flags.cf
  const uint16_t tmp = lhs - (rhs + cpu_flags.cf);
  _set_zf_sf_w(tmp);
  _set_pf(tmp);
  cpu_flags.cf = (lhs < (rhs  + cpu_flags.cf)) ? 1 : 0;
  cpu_flags.af = ((tmp ^ lhs ^ rhs) & 0x10) ? 1 : 0;
  cpu_flags.of = ((tmp ^ lhs) & (lhs ^ rhs) & 0x8000) ? 1 : 0;
#endif
  return tmp;
}

// SBB m/r, reg  (byte)
OPCODE(_18) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t tmp = _do_sbb_b(lhs, rhs, cpu_flags.cf);
  _write_rm_b(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// SBB m/r, reg  (word)
OPCODE(_19) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t tmp = _do_sbb_w(lhs, rhs, cpu_flags.cf);
  _write_rm_w(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// SBB reg, m/r  (byte)
OPCODE(_1A) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint8_t tmp = _do_sbb_b(lhs, rhs, cpu_flags.cf);
  _set_reg_b(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// SBB reg, m/r  (word)
OPCODE(_1B) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint16_t tmp = _do_sbb_w(lhs, rhs, cpu_flags.cf);
  _set_reg_w(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// SBB al, imm8
OPCODE(_1C) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1) + cpu_flags.cf;
  cpu_regs.al = _do_sbb_b(lhs, rhs, cpu_flags.cf);
  _step_ip(2);
}

// SBB ax, imm16
OPCODE(_1D) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1) + cpu_flags.cf;
  cpu_regs.ax = _do_sbb_w(lhs, rhs, cpu_flags.cf);
  _step_ip(3);
}

#undef SBB_FLAGS_B
#undef SBB_FLAGS_W

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// PUSH DS - push segment register DS
OPCODE(_1E) {
  _push_w(cpu_regs.ds);
  _step_ip(1);
}

// POP DS - pop segment register DS
OPCODE(_1F) {
  cpu_regs.ds = _pop_w();
  _step_ip(1);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define AND_FLAGS_B(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_b(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

#define AND_FLAGS_W(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_w(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

// AND m/r, reg  (byte)
OPCODE(_20) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t tmp = lhs & rhs;
  AND_FLAGS_B(lhs, rhs, tmp);
  _write_rm_b(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// AND m/r, reg  (word)
OPCODE(_21) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t tmp = lhs & rhs;
  AND_FLAGS_W(lhs, rhs, tmp);
  _write_rm_w(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// AND reg, m/r  (byte)
OPCODE(_22) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint8_t tmp = lhs & rhs;
  AND_FLAGS_B(lhs, rhs, tmp);
  _set_reg_b(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// AND reg, m/r  (word)
OPCODE(_23) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint16_t tmp = lhs & rhs;
  AND_FLAGS_W(lhs, rhs, tmp);
  _set_reg_w(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// AND al, imm8
OPCODE(_24) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1);
  const uint8_t tmp = lhs & rhs;
  AND_FLAGS_B(lhs, rhs, tmp);
  cpu_regs.al = tmp;
  _step_ip(2);
}

// AND ax, imm16
OPCODE(_25) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1);
  const uint16_t tmp = lhs & rhs;
  AND_FLAGS_W(lhs, rhs, tmp);
  cpu_regs.ax = tmp;
  _step_ip(3);
}

#undef AND_FLAGS_B
#undef AND_FLAGS_W

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define SEGOVR(OP)                                                            \
{                                                                             \
  _seg_ovr = OP;                                                              \
  _did_run_temp = false;                                                      \
  if (_op_table[code[1]]) {                                                   \
    _step_ip(1);                                                              \
    _op_table[code[1]](code + 1);                                             \
    _did_run_temp = true;                                                     \
  }                                                                           \
  _seg_ovr = 0x0;                                                             \
}

// Prefix - Segment Override ES
OPCODE(_26) {
  SEGOVR(0x26);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// DAA 0x27

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define SUB_FLAGS_B(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_b(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = ((lhs < rhs)) ? 1 : 0;                                     \
    cpu_flags.af = ((res ^ lhs ^ rhs)         & 0x10) ? 1 : 0;                \
    cpu_flags.of = ((res ^ lhs) & (lhs ^ rhs) & 0x80) ? 1 : 0;                \
  }

#define SUB_FLAGS_W(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_w(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = ((lhs < rhs)) ? 1 : 0;                                     \
    cpu_flags.af = ((res ^ lhs ^ rhs)         & 0x10)   ? 1 : 0;              \
    cpu_flags.of = ((res ^ lhs) & (lhs ^ rhs) & 0x8000) ? 1 : 0;              \
  }

// SUB m/r, reg  (byte)
OPCODE(_28) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t tmp = lhs - rhs;
  SUB_FLAGS_B(lhs, rhs, tmp);
  _write_rm_b(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// SUB m/r, reg  (word)
OPCODE(_29) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t tmp = lhs - rhs;
  SUB_FLAGS_W(lhs, rhs, tmp);
  _write_rm_w(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// SUB reg, m/r  (byte)
OPCODE(_2A) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint8_t tmp = lhs - rhs;
  SUB_FLAGS_B(lhs, rhs, tmp);
  _set_reg_b(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// SUB reg, m/r  (word)
OPCODE(_2B) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint16_t tmp = lhs - rhs;
  SUB_FLAGS_W(lhs, rhs, tmp);
  _set_reg_w(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// SUB al, imm8
OPCODE(_2C) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1);
  const uint8_t tmp = lhs - rhs;
  SUB_FLAGS_B(lhs, rhs, tmp);
  cpu_regs.al = tmp;
  _step_ip(2);
}

// SUB ax, imm16
OPCODE(_2D) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1);
  const uint16_t tmp = lhs - rhs;
  SUB_FLAGS_W(lhs, rhs, tmp);
  cpu_regs.ax = tmp;
  _step_ip(3);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// Prefix - Segment Override CS
OPCODE(_2E) {
  SEGOVR(0x2E);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define XOR_FLAGS_B(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_b(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

#define XOR_FLAGS_W(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_w(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

// XOR m/r, reg  (byte)
OPCODE(_30) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t tmp = lhs ^ rhs;
  XOR_FLAGS_B(lhs, rhs, tmp);
  _write_rm_b(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// XOR m/r, reg  (word)
OPCODE(_31) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t tmp = lhs ^ rhs;
  XOR_FLAGS_W(lhs, rhs, tmp);
  _write_rm_w(&m, tmp);
  _step_ip(1 + m.num_bytes);
}

// XOR reg, m/r  (byte)
OPCODE(_32) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint8_t tmp = lhs ^ rhs;
  XOR_FLAGS_B(lhs, rhs, tmp);
  _set_reg_b(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// XOR reg, m/r  (word)
OPCODE(_33) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint16_t tmp = lhs ^ rhs;
  XOR_FLAGS_W(lhs, rhs, tmp);
  _set_reg_w(m.reg, tmp);
  _step_ip(1 + m.num_bytes);
}

// XOR al, imm8
OPCODE(_34) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1);
  const uint8_t tmp = lhs ^ rhs;
  XOR_FLAGS_B(lhs, rhs, tmp);
  cpu_regs.al = tmp;
  _step_ip(2);
}

// XOR ax, imm16
OPCODE(_35) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1);
  const uint16_t tmp = lhs ^ rhs;
  XOR_FLAGS_W(lhs, rhs, tmp);
  cpu_regs.ax = tmp;
  _step_ip(3);
}

#undef XOR_FLAGS_B
#undef XOR_FLAGS_W

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// Prefix - Segment Override SS
OPCODE(_36) {
  SEGOVR(0x36);
}

// 0x37 = AAA

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define CMP_FLAGS_B(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_b(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = ((lhs < rhs)) ? 1 : 0;                                     \
    cpu_flags.af = ((res ^ lhs ^ rhs)         & 0x10) ? 1 : 0;                \
    cpu_flags.of = ((res ^ lhs) & (lhs ^ rhs) & 0x80) ? 1 : 0;                \
  }

#define CMP_FLAGS_W(lhs, rhs, res)                                            \
  {                                                                           \
    _set_zf_sf_w(res);                                                        \
    _set_pf(res);                                                             \
    cpu_flags.cf = ((lhs < rhs)) ? 1 : 0;                                     \
    cpu_flags.af = ((res ^ lhs ^ rhs)         & 0x10)   ? 1 : 0;              \
    cpu_flags.of = ((res ^ lhs) & (lhs ^ rhs) & 0x8000) ? 1 : 0;              \
  }

// CMP m/r, reg  (byte)
OPCODE(_38) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t tmp = lhs - rhs;
  CMP_FLAGS_B(lhs, rhs, tmp);
  _step_ip(1 + m.num_bytes);
}

// CMP m/r, reg  (word)
OPCODE(_39) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t tmp = lhs - rhs;
  CMP_FLAGS_W(lhs, rhs, tmp);
  _step_ip(1 + m.num_bytes);
}

// CMP reg, m/r  (byte)
OPCODE(_3A) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _get_reg_b(m.reg);
  const uint8_t rhs = _read_rm_b(&m);
  const uint8_t tmp = lhs - rhs;
  CMP_FLAGS_B(lhs, rhs, tmp);
  _step_ip(1 + m.num_bytes);
}

// CMP reg, m/r  (word)
OPCODE(_3B) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _get_reg_w(m.reg);
  const uint16_t rhs = _read_rm_w(&m);
  const uint16_t tmp = lhs - rhs;
  CMP_FLAGS_W(lhs, rhs, tmp);
  _step_ip(1 + m.num_bytes);
}

// CMP al, imm8
OPCODE(_3C) {
  const uint8_t lhs = cpu_regs.al;
  const uint8_t rhs = GET_CODE(uint8_t, 1);
  const uint8_t tmp = lhs - rhs;
  CMP_FLAGS_B(lhs, rhs, tmp);
  _step_ip(2);
}

// CMP ax, imm16
OPCODE(_3D) {
  const uint16_t lhs = cpu_regs.ax;
  const uint16_t rhs = GET_CODE(uint16_t, 1);
  const uint16_t tmp = lhs - rhs;
  CMP_FLAGS_W(lhs, rhs, tmp);
  _step_ip(3);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// Prefix - Segment Override DS
OPCODE(_3E) {
  SEGOVR(0x3e);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define INC(REG)                                                              \
  {                                                                           \
    cpu_flags.of = (REG == 0x7fff);                                           \
    cpu_flags.af = (REG & 0x0f) == 0x0f;                                      \
    REG += 1;                                                                 \
    _set_zf_sf_w(REG);                                                        \
    _set_pf(REG);                                                             \
    _step_ip(1);                                                              \
  }

// INC AX - increment register
OPCODE(_40) {
  INC(cpu_regs.ax);
}

// INC CX - increment register
OPCODE(_41) {
  INC(cpu_regs.cx);
}

// INC DX - increment register
OPCODE(_42) {
  INC(cpu_regs.dx);
}

// INC BX - increment register
OPCODE(_43) {
  INC(cpu_regs.bx);
}

// INC SP - increment register
OPCODE(_44) {
  INC(cpu_regs.sp);
}

// INC BP - increment register
OPCODE(_45) {
  INC(cpu_regs.bp);
}

// INC SI - increment register
OPCODE(_46) {
  INC(cpu_regs.si);
}

// INC DI - increment register
OPCODE(_47) {
  INC(cpu_regs.di);
}

#undef INC

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define DEC(REG)                                                              \
  {                                                                           \
    cpu_flags.of = (REG == 0x8000);                                           \
    cpu_flags.af = (REG & 0x0f) == 0x0;                                       \
    REG -= 1;                                                                 \
    _set_zf_sf_w(REG);                                                        \
    _set_pf(REG);                                                             \
    _step_ip(1);                                                              \
  }

// DEC AX - increment register
OPCODE(_48) {
  DEC(cpu_regs.ax);
}

// DEC CX - increment register
OPCODE(_49) {
  DEC(cpu_regs.cx);
}

// DEC DX - increment register
OPCODE(_4A) {
  DEC(cpu_regs.dx);
}

// DEC BX - increment register
OPCODE(_4B) {
  DEC(cpu_regs.bx);
}

// DEC SP - increment register
OPCODE(_4C) {
  DEC(cpu_regs.sp);
}

// DEC BP - increment register
OPCODE(_4D) {
  DEC(cpu_regs.bp);
}

// DEC SI - increment register
OPCODE(_4E) {
  DEC(cpu_regs.si);
}

// DEC DI - increment register
OPCODE(_4F) {
  DEC(cpu_regs.di);
}

#undef DEC

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// PUSH AX - push register
OPCODE(_50) {
  _push_w(cpu_regs.ax);
  _step_ip(1);
}

// PUSH CX - push register
OPCODE(_51) {
  _push_w(cpu_regs.cx);
  _step_ip(1);
}

// PUSH DX - push register
OPCODE(_52) {
  _push_w(cpu_regs.dx);
  _step_ip(1);
}

// PUSH BX - push register
OPCODE(_53) {
  _push_w(cpu_regs.bx);
  _step_ip(1);
}

// PUSH SP - push register
OPCODE(_54) {
  _push_w(cpu_regs.sp);
  _step_ip(1);
}

// PUSH BP - push register
OPCODE(_55) {
  _push_w(cpu_regs.bp);
  _step_ip(1);
}

// PUSH SI - push register
OPCODE(_56) {
  _push_w(cpu_regs.si);
  _step_ip(1);
}

// PUSH DI - push register
OPCODE(_57) {
  _push_w(cpu_regs.di);
  _step_ip(1);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// POP AX - pop register
OPCODE(_58) {
  cpu_regs.ax = _pop_w();
  _step_ip(1);
}

// POP CX - pop register
OPCODE(_59) {
  cpu_regs.cx = _pop_w();
  _step_ip(1);
}

// POP DX - pop register
OPCODE(_5A) {
  cpu_regs.dx = _pop_w();
  _step_ip(1);
}

// POP BX - pop register
OPCODE(_5B) {
  cpu_regs.bx = _pop_w();
  _step_ip(1);
}

// POP SP - pop register
OPCODE(_5C) {
  cpu_regs.sp = _pop_w();
  _step_ip(1);
}

// POP BP - pop register
OPCODE(_5D) {
  cpu_regs.bp = _pop_w();
  _step_ip(1);
}

// POP SI - pop register
OPCODE(_5E) {
  cpu_regs.si = _pop_w();
  _step_ip(1);
}

// POP DI - pop register
OPCODE(_5F) {
  cpu_regs.di = _pop_w();
  _step_ip(1);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// JO - jump on overflow
OPCODE(_70) {
  _step_ip(2);
  if (cpu_flags.of) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JNO - jump not overflow
OPCODE(_71) {
  _step_ip(2);
  if (!cpu_flags.of) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JB - jump if below
OPCODE(_72) {
  _step_ip(2);
  if (cpu_flags.cf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JAE - jump above or equal
OPCODE(_73) {
  _step_ip(2);
  if (!cpu_flags.cf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JZ - jump not zero
OPCODE(_74) {
  _step_ip(2);
  if (cpu_flags.zf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JNZ - jump not zero
OPCODE(_75) {
  _step_ip(2);
  if (!cpu_flags.zf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JBE - jump below or equal
OPCODE(_76) {
  _step_ip(2);
  if (cpu_flags.cf || cpu_flags.zf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JA - jump if above
OPCODE(_77) {
  _step_ip(2);
  if (!cpu_flags.cf && !cpu_flags.zf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JS - jump if sign
OPCODE(_78) {
  _step_ip(2);
  if (cpu_flags.sf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JNS - jump not sign
OPCODE(_79) {
  _step_ip(2);
  if (!cpu_flags.sf) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JP - jump parity
OPCODE(_7A) {
  _step_ip(2);
  if (_get_pf()) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JNP - jump not parity
OPCODE(_7B) {
  _step_ip(2);
  if (!_get_pf()) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JL - jump less than
OPCODE(_7C) {
  _step_ip(2);
  if (cpu_flags.sf != cpu_flags.of) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JGE - jump greater than or equal
OPCODE(_7D) {
  _step_ip(2);
  if (cpu_flags.sf == cpu_flags.of) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JLE - jump if less or equal
OPCODE(_7E) {
  _step_ip(2);
  if (cpu_flags.zf || (cpu_flags.sf != cpu_flags.of)) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// JG - jump if greater
OPCODE(_7F) {
  _step_ip(2);
  if (!((cpu_flags.sf != cpu_flags.of) || cpu_flags.zf)) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

#define TEST_B(TMP)                                                           \
  {                                                                           \
    _set_zf_sf_b(TMP);                                                        \
    _set_pf(TMP);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

// TEST - r/m8, r8
OPCODE(_84) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint8_t lhs = _read_rm_b(&m);
  const uint8_t rhs = _get_reg_b(m.reg);
  const uint8_t res = lhs & rhs;
  TEST_B(res);
  // step instruction pointer
  _step_ip(1 + m.num_bytes);
}

#define TEST_W(TMP)                                                           \
  {                                                                           \
    _set_zf_sf_w(TMP);                                                        \
    _set_pf(TMP);                                                             \
    cpu_flags.cf = 0;                                                         \
    cpu_flags.of = 0;                                                         \
  }

// TEST - r/m16, r16
OPCODE(_85) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  const uint16_t lhs = _read_rm_w(&m);
  const uint16_t rhs = _get_reg_w(m.reg);
  const uint16_t res = lhs & rhs;
  TEST_W(res);
  // step instruction pointer
  _step_ip(1 + m.num_bytes);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// MOV - r/m8, r8
OPCODE(_88) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  // do the transfer
  _write_rm_b(&m, _get_reg_b(m.reg));
  // step instruction pointer
  _step_ip(1 + m.num_bytes);
}

// MOV - r/m16, r16
OPCODE(_89) {
  struct cpu_mod_rm_t m;
  _decode_mod_rm(code, &m);
  // do the transfer
  _write_rm_w(&m, _get_reg_w(m.reg));
  // step instruction pointer
  _step_ip(1 + m.num_bytes);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// NOP - no operation (XCHG AX AX)
OPCODE(_90) {
  _step_ip(1);
}

#define XCHG(REG)                                                             \
  {                                                                           \
    const uint16_t t = REG;                                                   \
    REG = cpu_regs.ax;                                                        \
    cpu_regs.ax = t;                                                          \
    _step_ip(1);                                                              \
  }

// XCHG CX - exchange AX and CX
OPCODE(_91) {
  XCHG(cpu_regs.cx);
}

// XCHG DX - exchange AX and DX
OPCODE(_92) {
  XCHG(cpu_regs.dx);
}

// XCHG BX - exchange AX and BX
OPCODE(_93) {
  XCHG(cpu_regs.bx);
}

// XCHG SP - exchange AX and SP
OPCODE(_94) {
  XCHG(cpu_regs.sp);
}

// XCHG BP - exchange AX and BP
OPCODE(_95) {
  XCHG(cpu_regs.bp);
}

// XCHG SI - exchange AX and SI
OPCODE(_96) {
  XCHG(cpu_regs.si);
}

// XCHG DI - exchange AX and DI
OPCODE(_97) {
  XCHG(cpu_regs.di);
}

#undef XCHG

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// CBW - convert byte to word
OPCODE(_98) {
  cpu_regs.ah = (cpu_regs.al & 0x80) ? 0xff : 0x00;
  _step_ip(1);
}

// CWD - convert word to dword
OPCODE(_99) {
  cpu_regs.dx = (cpu_regs.ax & 0x8000) ? 0xffff : 0x0000;
  _step_ip(1);
}

// WAIT - wait for test pin assertion
OPCODE(_9B) {
  _step_ip(1);
}

static uint32_t _get_addr(const enum cpu_seg_t seg, uint16_t offs) {
  return (_get_seg(seg) << 4) + offs;
}

// MOV AL, [imm16]
OPCODE(_A0) {
  const uint16_t imm = GET_CODE(uint16_t, 1);
  cpu_regs.al = _cpu_io.mem_read_8(_get_addr(CPU_SEG_DS, imm));
  _step_ip(3);
}

// MOV AX, [imm16]
OPCODE(_A1) {
  const uint16_t imm = GET_CODE(uint16_t, 1);
  cpu_regs.ax = _cpu_io.mem_read_16(_get_addr(CPU_SEG_DS, imm));
  _step_ip(3);
}

// MOV [imm16], AL
OPCODE(_A2) {
  const uint16_t imm = GET_CODE(uint16_t, 1);
  _cpu_io.mem_write_8(_get_addr(CPU_SEG_DS, imm), cpu_regs.al);
  _step_ip(3);
}

// MOV [imm16], AX
OPCODE(_A3) {
  const uint16_t imm = GET_CODE(uint16_t, 1);
  _cpu_io.mem_write_16(_get_addr(CPU_SEG_DS, imm), cpu_regs.ax);
  _step_ip(3);
}

// TEST AL, imm8
OPCODE(_A8) {
  const uint8_t imm = GET_CODE(uint8_t, 1);
  const uint8_t res = cpu_regs.al & imm;
  TEST_B(res);
  _step_ip(2);
}

// TEST AX, imm16
OPCODE(_A9) {
  const uint16_t imm = GET_CODE(uint16_t, 1);
  const uint16_t res = cpu_regs.ax & imm;
  TEST_W(res);
  _step_ip(3);
}

// RET - near return and add to stack pointer
OPCODE(_C2) {
  const uint16_t disp16 = GET_CODE(uint16_t, 1);
  cpu_regs.ip = _pop_w();
  cpu_regs.sp += disp16;
}

// RET - near return
OPCODE(_C3) {
  cpu_regs.ip = _pop_w();
}

// MOV al, imm8
OPCODE(_B0) {
  cpu_regs.al = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV cl, imm8
OPCODE(_B1) {
  cpu_regs.cl = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV dl, imm8
OPCODE(_B2) {
  cpu_regs.dl = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV bl, imm8
OPCODE(_B3) {
  cpu_regs.bl = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV ah, imm8
OPCODE(_B4) {
  cpu_regs.ah = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV ch, imm8
OPCODE(_B5) {
  cpu_regs.ch = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV dh, imm8
OPCODE(_B6) {
  cpu_regs.dh = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV bh, imm8
OPCODE(_B7) {
  cpu_regs.bh = GET_CODE(uint8_t, 1);
  _step_ip(2);
}

// MOV ax, imm16
OPCODE(_B8) {
  cpu_regs.ax = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// MOV cx, imm16
OPCODE(_B9) {
  cpu_regs.cx = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// MOV dx, imm16
OPCODE(_BA) {
  cpu_regs.dx = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// MOV bx, imm16
OPCODE(_BB) {
  cpu_regs.bx = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// MOV sp, imm16
OPCODE(_BC) {
  cpu_regs.sp = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// MOV bp, imm16
OPCODE(_BD) {
  cpu_regs.bp = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// MOV si, imm16
OPCODE(_BE) {
  cpu_regs.si = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// MOV di, imm16
OPCODE(_BF) {
  cpu_regs.di = GET_CODE(uint16_t, 1);
  _step_ip(3);
}

// INT 3
OPCODE(_CC) {
  _step_ip(1);
  _raise_int(3);
}

// INT imm8
OPCODE(_CD) {
  _step_ip(2);
  const uint8_t num = GET_CODE(uint8_t, 1);
  _raise_int(num);
}

// XLAT
OPCODE(_D7) {
  cpu_regs.al = _cpu_io.mem_read_8(
    _get_addr(CPU_SEG_DS, cpu_regs.bx + cpu_regs.al));
  _step_ip(1);
}

// RETF - far return and add to stack pointer
OPCODE(_CA) {
  const uint16_t disp16 = GET_CODE(uint16_t, 1);
  cpu_regs.ip = _pop_w();
  cpu_regs.cs = _pop_w();
  cpu_regs.sp += disp16;
}

// RETF - far return
OPCODE(_CB) {
  cpu_regs.ip = _pop_w();
  cpu_regs.cs = _pop_w();
}

//
static inline void _shift_8(struct cpu_mod_rm_t *mod, uint16_t count) {
  uint8_t opr = _read_rm_b(mod);
  switch (mod->reg) {
  case 0x0:  // ROL
    break;
  case 0x1:  // ROR
    break;
  case 0x2:  // RCL
    break;
  case 0x3:  // RCR
    break;
  case 0x4:  // SHL/SAL
    break;
  case 0x5:  // SHR
    break;
  case 0x7:  // SAR
    break;
  default:
    UNREACHABLE();
  }
  _write_rm_b(mod, opr);
}

//
static inline void _shift_16(struct cpu_mod_rm_t *mod, uint16_t count) {
  uint16_t opr = _read_rm_w(mod);
  switch (mod->reg) {
  case 0x0:  // ROL
    break;
  case 0x1:  // ROR
    break;
  case 0x2:  // RCL
    break;
  case 0x3:  // RCR
    break;
  case 0x4:  // SHL/SAL
    break;
  case 0x5:  // SHR
    break;
  case 0x7:  // SAR
    break;
  default:
    UNREACHABLE();
  }
  _write_rm_w(mod, opr);
}

// SHIFT r/m8  - 1 time
OPCODE(_D0) {
  struct cpu_mod_rm_t mod;
  _decode_mod_rm(code, &mod);
  _shift_8(&mod, 1);
  _step_ip(1 + mod.num_bytes);
}

// SHIFT r/m16  - 1 time
OPCODE(_D1) {
  struct cpu_mod_rm_t mod;
  _decode_mod_rm(code, &mod);
  _shift_16(&mod, 1);
  _step_ip(1 + mod.num_bytes);
}

// SHIFT r/m8  - CL times
OPCODE(_D2) {
  struct cpu_mod_rm_t mod;
  _decode_mod_rm(code, &mod);
  _shift_8(&mod, cpu_regs.cl);
  _step_ip(1 + mod.num_bytes);
}

// SHIFT r/m16  - CL times
OPCODE(_D3) {
  struct cpu_mod_rm_t mod;
  _decode_mod_rm(code, &mod);
  _shift_16(&mod, cpu_regs.cl);
  _step_ip(1 + mod.num_bytes);
}

// LOOPNZ
OPCODE(_E0) {
  _step_ip(2);
  --cpu_regs.cx;
  if (cpu_regs.cx && !cpu_flags.zf) {
    const int8_t disp = GET_CODE(int8_t, 1);
    cpu_regs.ip += disp;
  }
}

// LOOPZ
OPCODE(_E1) {
  _step_ip(2);
  --cpu_regs.cx;
  if (cpu_regs.cx && cpu_flags.zf) {
    const int8_t disp = GET_CODE(int8_t, 1);
    cpu_regs.ip += disp;
  }
}

// LOOP
OPCODE(_E2) {
  _step_ip(2);
  --cpu_regs.cx;
  if (cpu_regs.cx) {
    const int8_t disp = GET_CODE(int8_t, 1);
    cpu_regs.ip += disp;
  }
}

// JCXZ - jump if CX is zero
OPCODE(_E3) {
  _step_ip(2);
  if (cpu_regs.cx == 0) {
    cpu_regs.ip += GET_CODE(int8_t, 1);
  }
}

// IN AL, port
OPCODE(_E4) {
  const uint8_t port = GET_CODE(uint8_t, 1);
  cpu_regs.al = _cpu_io.port_read_8(port);
  _step_ip(2);
}

// IN AX, port
OPCODE(_E5) {
  const uint8_t port = GET_CODE(uint8_t, 1);
  cpu_regs.ax = _cpu_io.port_read_16(port);
  _step_ip(2);
}

// OUT AL, port
OPCODE(_E6) {
  const uint8_t port = GET_CODE(uint8_t, 1);
  _cpu_io.port_write_8(port, cpu_regs.al);
  _step_ip(2);
}

// OUT AX, port
OPCODE(_E7) {
  const uint8_t port = GET_CODE(uint8_t, 1);
  _cpu_io.port_write_16(port, cpu_regs.ax);
  _step_ip(2);
}

// CALL disp16
OPCODE(_E8) {
  // step over call
  _step_ip(3);
  // push return address
  _push_w(cpu_regs.ip);
  // set new ip
  cpu_regs.ip += GET_CODE(uint16_t, 1);
}

// JMP disp16 - jump with signed word displacement
OPCODE(_E9) {
  cpu_regs.ip += 3 + GET_CODE(uint16_t, 1);
}

// JMP far - intersegment jump
OPCODE(_EA) {
  cpu_regs.ip = GET_CODE(uint16_t, 1);
  cpu_regs.cs = GET_CODE(uint16_t, 3);
}

// JMP disp8 - jump with signed byte displacement
OPCODE(_EB) {
  cpu_regs.ip += 2 + GET_CODE(int8_t, 1);
}

// IN AL, DX
OPCODE(_EC) {
  cpu_regs.al = _cpu_io.port_read_8(cpu_regs.dx);
  _step_ip(1);
}

// IN AX, DX
OPCODE(_ED) {
  cpu_regs.ax = _cpu_io.port_read_16(cpu_regs.dx);
  _step_ip(1);
}

// OUT AL, DX
OPCODE(_EE) {
  _cpu_io.port_write_8(cpu_regs.dx, cpu_regs.al);
  _step_ip(1);
}

// OUT AX, DX
OPCODE(_EF) {
  _cpu_io.port_write_16(cpu_regs.dx, cpu_regs.ax);
  _step_ip(1);
}

// LOCK - lock prefix
OPCODE(_F0) {
  _step_ip(1);
}

// CMC - compliment carry flag
OPCODE(_F5) {
  cpu_flags.cf ^= 1;
  _step_ip(1);
}

// CLC - clear carry flag
OPCODE(_F8) {
  cpu_flags.cf = 0;
  _step_ip(1);
}

// STC - set carry flag
OPCODE(_F9) {
  cpu_flags.cf = 1;
  _step_ip(1);
}

// CLI - clear interrupt flag
OPCODE(_FA) {
  cpu_flags.ifl = 0;
  _sti_sr = 0x0;
  _step_ip(1);
}

// STI - set interrupt flag
OPCODE(_FB) {
  // STI is delayed one instruction
  _sti_sr = 0x3;
  _step_ip(1);
}

// CLD - clear direction flag
OPCODE(_FC) {
  // STI is delayed one instruction
  cpu_flags.df = 0;
  _step_ip(1);
}

// STD - set direction flag
OPCODE(_FD) {
  cpu_flags.df = 1;
  _step_ip(1);
}

// [27] daa
// [37] aaa

// [86, 87] xchg
// [A4, A7] movsb movw cmpsb cmpsw
// [D4, D5] AAM AAD

// [E0, E2] loopnz loopz loop

#define ___ 0
#define XXX 0
static const opcode_t _op_table[256] = {
// 00   01   02   03   04   05   06   07   08   09   0A   0B   0C   0D   0E   0F
  _00, _01, _02, _03, _04, _05, _06, _07, _08, _09, _0A, _0B, _0C, _0D, _0E, XXX, // 00
  _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _1A, _1B, _1C, _1D, _1E, _1F, // 10
  _20, _21, _22, _23, _24, _25, _26, ___, _28, _29, _2A, _2B, _2C, _2D, _2E, ___, // 20
  _30, _31, _32, _33, _34, _35, _36, ___, _38, _39, _3A, _3B, _3C, _3D, _3E, ___, // 30
  _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _4A, _4B, _4C, _4D, _4E, _4F, // 40
  _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _5A, _5B, _5C, _5D, _5E, _5F, // 50
  XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, // 60
  _70, _71, _72, _73, _74, _75, _76, _77, _78, _79, _7A, _7B, _7C, _7D, _7E, _7F, // 70
  ___, ___, ___, ___, _84, _85, ___, ___, ___, ___, ___, ___, ___, ___, ___, ___, // 80
  _90, _91, _92, _93, _94, _95, _96, _97, _98, _99, ___, _9B, ___, ___, ___, ___, // 90
  _A0, _A1, _A2, _A3, ___, ___, ___, ___, _A8, _A9, ___, ___, ___, ___, ___, ___, // A0
  _B0, _B1, _B2, _B3, _B4, _B5, _B6, _B7, _B8, _B9, _BA, _BB, _BC, _BD, _BE, _BF, // B0
  XXX, XXX, _C2, _C3, ___, ___, ___, ___, XXX, XXX, _CA, _CB, _CC, _CD, ___, ___, // C0
  ___, ___, ___, ___, ___, ___, XXX, _D7, XXX, XXX, XXX, XXX, XXX, XXX, XXX, XXX, // D0
  _E0, _E1, _E2, _E3, _E4, _E5, _E6, _E7, _E8, _E9, _EA, _EB, _EC, _ED, _EE, _EF, // E0
  _F0, XXX, ___, ___, ___, _F5, ___, ___, _F8, _F9, _FA, _FB, _FC, _FD, ___, ___, // F0
};
#undef ___
#undef XXX

bool cpu_redux_exec(void) {

  // delay setting IFL for one instruction after STI
  _sti_sr >>= 1;
  cpu_flags.ifl |= _sti_sr & 1;

  // get effective pc
  const uint32_t eip = (cpu_regs.cs << 4) + cpu_regs.ip;
  // find the code stream
  const uint8_t *code = _cpu_io.ram + eip;
  // lookup opcode handler
  const opcode_t op = _op_table[*code];
  if (op == NULL) {
    return false;
  }

  // execute opcode
  op(code);

  // this is a little hack for the segment override prefix so we can bail out
  // if the next opcode wasnt implemented yet
  if (_did_run_temp == false) {  // XXX: remove
    return false;
  }

  return true;
}
