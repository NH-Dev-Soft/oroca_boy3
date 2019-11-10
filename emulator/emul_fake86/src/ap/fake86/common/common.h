/*
  Fake86: A portable, open-source 8086 PC emulator.
  Copyright (C)2010-2013 Mike Chambers
               2019      Aidan Dodds

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

#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hw_def.h"
#include "config.h"
#include "files.h"





// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- misc

#if NDEBUG
  #ifdef _MSC_VER
    #define UNREACHABLE() { __assume(0); }
  #else
    #define UNREACHABLE() { __builtin_unreachable(); }
  #endif
#else
  #ifdef _MSC_VER
    #define UNREACHABLE() { __debugbreak(); abort(); }
  #else
    #define __debugbreak() {}
    #define UNREACHABLE() { abort(); }
  #endif
#endif

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- audio.c
void audio_init(uint32_t sample_rate);
void audio_close(void);
uint32_t audio_callback(int16_t *samples, uint32_t num_samples);
void audio_tick(const uint64_t cycles);
void audio_pc_speaker_freq(const uint32_t freq);
void audio_pc_speaker_enable(bool enable);
void audio_disk_seek(const uint32_t sects);

extern bool audio_enable;

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- i8259.c
struct structpic {
  // mask register
  uint8_t imr;
  // request register
  uint8_t irr; 
  // service register
  uint8_t isr;
  // used during initialization to keep track of which ICW we're at
  uint8_t icwstep; 
  uint8_t icw[5];
  // interrupt vector offset
  uint8_t intoffset;
  // which IRQ has highest priority
  uint8_t priority;
  // automatic EOI mode
  uint8_t autoeoi;
  // remember what to return on read register from OCW3
  uint8_t readmode; 
  uint8_t enabled;
};
void i8259_init();
void i8259_doirq(uint8_t irqnum);
uint8_t i8259_nextintr(void);
bool i8259_irq_pending(void);
void i8259_tick(uint64_t cycles);
void i8259_state_save(FILE *fd);
void i8259_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- video.c
bool video_int_handler(int intnum);
void video_tick(const uint64_t cycles);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- sermouse.c
struct sermouse_s {
  uint8_t reg[8];
  uint8_t buf[16];
  int8_t bufptr;
};

void mouse_port_write(uint16_t portnum, uint8_t value);
uint8_t mouse_port_read(uint16_t portnum);
void mouse_init(uint16_t baseport, uint8_t irq);
void mouse_post_event(uint8_t lmb, uint8_t rmb, int32_t xrel, int32_t yrel);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- memory.c
extern uint8_t RAM[0x100000];

void write86(uint32_t addr32, uint8_t value);
void writew86(uint32_t addr32, uint16_t value);

uint8_t read86(uint32_t addr32);
uint16_t readw86(uint32_t addr32);

void mem_init(void);
uint32_t mem_loadbinary(uint32_t addr32, const char *filename, uint8_t roflag);
uint32_t mem_loadrom(uint32_t addr32, const char *filename, uint8_t failure_fatal);
uint32_t mem_loadbios(const char *filename);
void mem_dump(const char *path);
void mem_write(uint32_t addr, const uint8_t *src, size_t size);
void mem_state_save(FILE *fd);
void mem_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- i8237.c
struct dmachan_s {
  uint32_t page;
  uint32_t addr;
  uint32_t reload;
  uint32_t count;
  uint8_t direction;
  uint8_t autoinit;
  uint8_t writemode;
  uint8_t masked;
};
void i8237_tick(uint64_t cycles);
bool i8237_init(void);
void i8237_state_save(FILE *fd);
void i8237_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- i8253.c
void i8253_init(void);
void i8253_tick(uint64_t cycles);
uint32_t i8253_frequency(int channel);
uint8_t i8253_channel2_out(void);
int64_t i8253_cycles_before_irq(void);
void i8253_state_save(FILE *fd);
void i8253_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- i8255.c
struct i8255_t {
  uint8_t port_in[3];
  uint8_t port_out[3];
  uint8_t ctrl_word;
};
extern struct i8255_t i8255;

bool i8255_init(void);
void i8255_reset(void);
void i8255_tick(uint64_t cycles);
bool i8255_speaker_on(void);
void i8255_key_required(void);
void i8255_key_push(uint8_t key);
void i8255_state_save(FILE *fd);
void i8255_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- disk.c
bool disk_is_inserted(int num);
bool disk_insert(uint8_t drivenum, const char *filename);
bool disk_insert_mem(uint8_t drivenum, const char *filename);
void disk_eject(uint8_t drivenum);
void disk_int_handler(int intnum);
void disk_bootstrap(int intnum);

extern uint8_t bootdrive, hdcount, fdcount;

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- log.c
enum {
  LOG_CHAN_ALL,
  LOG_CHAN_DISK,
  LOG_CHAN_FRONTEND,
  LOG_CHAN_SDL,
  LOG_CHAN_CPU,
  LOG_CHAN_MEM,
  LOG_CHAN_VIDEO,
  LOG_CHAN_AUDIO,
  LOG_CHAN_PORT,
  LOG_CHAN_DOS,
};

void log_init(void);
void log_close(void);
void log_mute(bool enable);
void log_printf(int channel, const char *fmt, ...);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ports.c
typedef void (*port_write_b_t)(uint16_t portnum, uint8_t value);
typedef uint8_t (*port_read_b_t)(uint16_t portnum);

void portout(uint16_t portnum, uint8_t value);
void portout16(uint16_t portnum, uint16_t value);
uint8_t portin(uint16_t portnum);
uint16_t portin16(uint16_t portnum);

extern uint8_t portram[0x10000];

void set_port_write_redirector(uint16_t startport, uint16_t endport,
                               void *callback);
void set_port_read_redirector(uint16_t startport, uint16_t endport,
                              void *callback);

void port_state_save(FILE *fd);
void port_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- interrupt.c
extern void intcall86(uint16_t intnum);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- rom.c
bool rom_insert();

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- video_neo.c

// memory access for video cards
uint8_t neo_mem_read_A0000(uint32_t addr);
void neo_mem_write_A0000(uint32_t addr, uint8_t value);

bool neo_init(void);
bool neo_int10_handler(void);
void neo_tick(uint64_t cycles);
int neo_get_video_mode(void);

void neo_state_save(FILE *fd);
void neo_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- vga_timing.c

void vga_timing_init(void);
void vga_timing_advance(const uint64_t cycles);
uint8_t vga_timing_get_3da(void);
bool vga_timing_should_flip(void);
void vga_timing_did_flip(void);

void vga_timing_state_save(FILE *fd);
void vga_timing_state_load(FILE *fd);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- cmos.c
void cmos_init(void);
bool cmos_nmi_enabled(void);

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- dos.c
bool on_dos_int(void);
