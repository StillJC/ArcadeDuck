#ifndef H8_EMU_H
#define H8_EMU_H

#include "config.h"
#include "device.h"
#include "ir.h"
#include "registers.h"
#include "rtc.h"
#include "types.h"

typedef union
{
  H8_BITFIELD_8
  (
    /* Carry Flag */
    h8_u8 c : 1,

    /* Overflow Flag */
    h8_u8 v : 1,

    /* Zero Flag */
    h8_u8 z : 1,

    /* Negative Flag */
    h8_u8 n : 1,

    /**
     * User Bit A
     * Can be written and read by LDC, STC, ANDC, ORC, and XORC.
     */
    h8_u8 u : 1,

    /**
     * Half-Carry Flag
     * On byte-size instructions, set on carry on bit 3.
     * On word-size instructions, set on carry on bit 11.
     * On long-size instructions, set on carry on bit 27.
     */
    h8_u8 h : 1,

    /**
     * User Bit B
     * Can be written and read by LDC, STC, ANDC, ORC, and XORC.
     */
    h8_u8 ui : 1,

    /**
     * Interrupt Mask Bit
     * Enables/disables interrupts. Initialized to 1.
     */
    h8_u8 i : 1
  ) flags;
  h8_byte_t raw;
} h8_ccr_t;

typedef union
{
  struct
  {
#if H8_BIG_ENDIAN
    h8_word_t unusable;
    h8_byte_t rh;
    h8_byte_t rl;
#else
    /**
     * The low byte of the R register, ie: 0x67.
     */
    h8_byte_t rl;

    /**
     * The high byte of the R register, ie: 0x45.
     */
    h8_byte_t rh;

    /**
     * Identical to E. Should not be used.
     */
    h8_word_t unusable;
#endif
  } byte;

  struct
  {
#if H8_BIG_ENDIAN
    h8_word_t e;
    h8_word_t r;
#else
    /**
     * The low 16-bit word of a general register, R, ie: 0x4567.
     * Can also be accessed as a set of two 8-bit registers.
     */
    h8_word_t r;

    /**
     * The high 16-bit word of a general register, E, ie: 0x0123.
     * Also known as an extended register.
     */
    h8_word_t e;
#endif
  } word;

  /**
   * A full 32-bit long register, ie: 0x1234567.
   * The combination of the E and R registers.
   */
  h8_long_t er;

} h8_general_reg_t;

typedef struct
{
  /**
   * This 24-bit counter indicates the address of the next instruction the CPU
   * will execute. The length of all CPU instructions is 2 bytes (one word), so
   * the least significant PC bit is ignored. (When an instruction is fetched,
   * the least significant PC bit is regarded as 0). The PC is initialized when
   * the start address is loaded by the vector address generated during reset
   * exception-handling sequence.
   */
  unsigned pc : 24;

  /**
   * This 8-bit register contains internal CPU status information, including an
   * interrupt mask bit (I) and half-carry (H), negative (N), zero (Z),
   * overflow (V), and carry (C) flags. The I bit is initialized to 1 by reset
   * exception-handling sequence, but other bits are not initialized.
   * Some instructions leave flag bits unchanged. Operations can be performed
   * on the CCR bits by the LDC, STC, ANDC, ORC, and XORC instructions. The
   * N, Z, V, and C flags are used as branching conditions for conditional
   * branch (Bcc) instructions.
   */
  h8_ccr_t ccr;

  h8_general_reg_t regs[8];
} h8_cpu_t;

typedef enum
{
  /** Nothing is wrong! */
  H8_DEBUG_NO_ERROR = 0,

  /** The program executed an instruction the emulator does not yet support */
  H8_DEBUG_UNIMPLEMENTED_OPCODE,

  /** The next opcode is unable to be decoded */
  H8_DEBUG_MALFORMED_OPCODE,

  /** An SSU read was attempted on an invalid device */
  H8_DEBUG_BAD_SSU_ACCESS,

  /** The program counter contains an invalid value */
  H8_DEBUG_BAD_PC,

  /** The emulator somehow reached a condition that cannot happen */
  H8_DEBUG_UNREACHABLE_CODE,

  H8_DEBUG_SIZE
} h8_error;

/**
 * The interrupt vector address table. 0x50 bytes at the beginning of ROM.
 * Notably, the first address is the program entrypoint on boot/reset.
 */
typedef struct
{
  h8_word_be_t reset;
  h8_word_be_t reserved1;
  h8_word_be_t reserved2;
  h8_word_be_t reserved3;
  h8_word_be_t reserved4;
  h8_word_be_t reserved5;
  h8_word_be_t reserved6;
  h8_word_be_t nmi;
  h8_word_be_t trapa0;
  h8_word_be_t trapa1;
  h8_word_be_t trapa2;
  h8_word_be_t trapa3;
  h8_word_be_t reserved12;
  h8_word_be_t sleep;
  h8_word_be_t reserved14;
  h8_word_be_t reserved15;
  h8_word_be_t irq0;
  h8_word_be_t irq1;
  h8_word_be_t irqaec;
  h8_word_be_t reserved19;
  h8_word_be_t reserved20;
  h8_word_be_t comp0;
  h8_word_be_t comp1;
  h8_word_be_t rtc_quarter_second;
  h8_word_be_t rtc_half_second;
  h8_word_be_t rtc_second;
  h8_word_be_t rtc_minute;
  h8_word_be_t rtc_hour;
  h8_word_be_t rtc_day;
  h8_word_be_t rtc_week;
  h8_word_be_t rtc_free_running;
  h8_word_be_t watchdog_timer;
  h8_word_be_t async_event_counter;
  h8_word_be_t timer_b1;
  h8_word_be_t ssu_iic2;
  h8_word_be_t timer_w;
  h8_word_be_t reserved36;
  h8_word_be_t sci3;
  h8_word_be_t ad_conversion_end;
  h8_word_be_t reserved39;
} h8_ivat_t;

typedef struct
{
  h8_byte_t rom[5];
  h8_rtc_t rtc;
  h8_byte_t unimplemented1[0xB3];
  h8_ssu_t ssu;
  h8_tw_t tw;
} h8_io1_t;

typedef struct
{
  h8_byte_t unknown1[0x0C];
  h8_aec_sci3_t aec_sci3;
  h8_wdt_t wdt;
  h8_byte_t unknown2[0x08];
  h8_adc_t adc;
  h8_byte_t unimplemented[0x40];
} h8_io2_t;

#define H8_MEMORY_REGION_IO1 0xF020
#define H8_MEMORY_REGION_RAM_2K 0xF780
#define H8_MEMORY_REGION_RAM_1K 0xFB80
#define H8_MEMORY_REGION_IO2 0xFF80

typedef union
{
  struct
  {
    h8_ivat_t ivat;
    h8_byte_t data[0xF020 - 0x0050];
    h8_io1_t io1;
    h8_byte_t data2[0xF780 - 0xF100];
    h8_byte_t ram[0xFF80 - 0xF780];
    h8_io2_t io2;
  } parts;
  h8_byte_t raw[0x10000];
} h8_addrspace_t;

/* An arbitrary maximum number of devices that can be connected to a system */
#define H8_DEVICES_MAX 8

struct h8_system_t;

typedef void (*H8_IN_T)(struct h8_system_t*, h8_byte_t*);
typedef void (*H8_OUT_T)(struct h8_system_t*, h8_byte_t*, const h8_byte_t);

/*
 * ArcadeDuck extension: optional byte-wide external bus callbacks.  A TRUE
 * return means the host handled the access.  This keeps the upstream 64 KiB
 * device map intact while allowing H8/300H 24-bit System 12 addressing.
 */
typedef h8_bool (*H8_BUS_READ_T)(void *opaque, unsigned address, h8_byte_t *value);
typedef h8_bool (*H8_BUS_WRITE_T)(void *opaque, unsigned address, h8_byte_t value);

#define H8_IN(a) void a(h8_system_t* system, h8_byte_t* byte)
#define H8_OUT(a) void a(h8_system_t* system, h8_byte_t* byte, h8_byte_t value)

typedef struct
{
  H8D_OP_PDR_IN_T *func;
  h8_device_t *device;
} h8_system_pin_in_t;

typedef struct
{
  H8D_OP_PDR_OUT_T *func;
  h8_device_t *device;
} h8_system_pin_out_t;

typedef struct
{
  H8D_OP_ADRR_T *func;
  h8_device_t *device;
} h8_system_adc_t;

typedef struct h8_system_t
{
  h8_cpu_t cpu;
  h8_instruction_t dbus;
  h8_addrspace_t vmem;

  h8_error error_code;
  unsigned error_line;
  int cycles;

  void *bus_opaque;
  H8_BUS_READ_T bus_read;
  H8_BUS_WRITE_T bus_write;

  /* ArcadeDuck extension: original H8/300 normal-mode selector. */
  h8_bool normal_mode;

  H8_IN_T io_read[0x160];
  H8_OUT_T io_write[0x160];

  h8_device_t devices[H8_DEVICES_MAX];

  h8_ir_t ir;

  /** The number of devices initialized within `devices` */
  unsigned device_count;

  h8_system_pin_in_t pdr1_in[3];
  h8_system_pin_out_t pdr1_out[3];

  h8_system_pin_in_t pdr3_in[3];
  h8_system_pin_out_t pdr3_out[3];

  h8_system_pin_in_t pdr8_in[3];
  h8_system_pin_out_t pdr8_out[3];

  h8_system_pin_in_t pdr9_in[4];
  h8_system_pin_out_t pdr9_out[4];

  h8_system_pin_in_t pdrb_in[6];
  h8_system_pin_out_t pdrb_out[6];

  h8_system_adc_t adc[6];

  /** Whether or not SLEEP mode is currently active */
  h8_bool sleep;

#if H8_PROFILING
  unsigned instructions;
  unsigned char reads[0x10000];
  unsigned char writes[0x10000];
  unsigned char executes[0x10000];
#endif
} h8_system_t;

/**
 * @brief Writes an amount of data from the system.
 * Wrties an amount of data to the H8 system from a buffer. Should the
 * parameters exceed memory limits, as much data as possible will be written.
 * If force is true, writes will succeed even on read-only segments.
 * Does not call any I/O functions.
 * @param system The H8 system
 * @param buffer The buffer to output from
 * @param address The virtual memory address to write to
 * @param size The number of bytes to write
 * @param force If TRUE, writes will succeed on read-only addresses
 * @return The number of bytes actually written to memory
 */
unsigned h8_write(h8_system_t *system, const void *buffer,
                  const unsigned address, const unsigned size,
                  const h8_bool force);

/**
 * @brief Reads an amount of data from the system.
 * Reads an amount of data from the H8 system into a buffer. Should the
 * parameters exceed memory limits, as much data as possible will be read.
 * Does not call any I/O functions.
 * @param system The H8 system
 * @param buffer The buffer to output into
 * @param address The virtual memory address to read from
 * @param size The number of bytes to read
 * @return The number of bytes actually read into the buffer
 */
unsigned h8_read(const h8_system_t *system, void *buffer,
                 const unsigned address, unsigned size);

void h8_init(h8_system_t *system);

/* ArcadeDuck extension: generic H8/300H advanced-mode interrupt entry. */
h8_bool h8_interrupt(h8_system_t *system, unsigned vector);

/**
 * Runs the next instruction for the H8 system state
 */
void h8_step(h8_system_t *system);

/**
 * Runs one frame (1/60 of a second) of H8 system state
 */
void h8_run(h8_system_t *system);

/**
 * Runs unit tests for the emulator, exiting with either an error code or 0
 */
void h8_test(void);

h8_bool h8_system_init(h8_system_t *system, const h8_system_id id);

h8_byte_t h8_peek_b(h8_system_t *system, const unsigned address);

h8_word_t h8_peek_w(h8_system_t *system, const unsigned address);

h8_long_t h8_peek_l(h8_system_t *system, const unsigned address);

void h8_poke_b(h8_system_t *system, const unsigned address,
               const h8_byte_t val);

void h8_poke_w(h8_system_t *system, const unsigned address,
               const h8_word_t val);

void h8_poke_l(h8_system_t *system, const unsigned address,
               const h8_long_t val);

#endif
