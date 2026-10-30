// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;

typedef struct ArcadeDuckTLCS900HCallbacks
{
  void* context;
  uint8 (*read8)(void* context, uint32 address);
  uint16 (*read16)(void* context, uint32 address);
  uint32 (*read32)(void* context, uint32 address);
  void (*write8)(void* context, uint32 address, uint8 value);
  void (*write16)(void* context, uint32 address, uint16 value);
  void (*write32)(void* context, uint32 address, uint32 value);
  // LDC access to the micro-DMA control registers (DMAS/DMAD/DMAC/DMAM).
  uint32 (*control_read)(void* context, uint8 reg, unsigned size);
  void (*control_write)(void* context, uint8 reg, unsigned size, uint32 value);
} ArcadeDuckTLCS900HCallbacks;

// The interpreter keeps its register file in globals, so one core instance exists at a time.
void ArcadeDuckTLCS900HSetCallbacks(const ArcadeDuckTLCS900HCallbacks* callbacks);
void ArcadeDuckTLCS900HReset(uint32 pc, uint32 xsp);
int32 ArcadeDuckTLCS900HStep(void);
// First opcode byte of the instruction the last Step() executed.
uint8 ArcadeDuckTLCS900HGetLastOpcode(void);
bool ArcadeDuckTLCS900HIsHalted(void);
uint32 ArcadeDuckTLCS900HGetPC(void);
uint8 ArcadeDuckTLCS900HGetIFF(void);

// Pushes PC and SR, raises the interrupt mask to level and jumps through the
// vector table at 0xFFFF00. Leaves HALT.
void ArcadeDuckTLCS900HTakeInterrupt(uint8 level, uint8 vector);

// Save-state support: visits the global register file, flags, register bank, error register, HALT and the
// interpreter's per-instruction scratch in a fixed order. Register maps are static tables indexed by the bank.
typedef void (*ArcadeDuckTLCS900HStateCallback)(void* context, void* data, size_t size);
void ArcadeDuckTLCS900HDoState(ArcadeDuckTLCS900HStateCallback callback, void* context);

uint8 loadB(uint32 address);
uint16 loadW(uint32 address);
uint32 loadL(uint32 address);
void storeB(uint32 address, uint8 value);
void storeW(uint32 address, uint16 value);
void storeL(uint32 address, uint32 value);
void int_check_pending(void);
void cpu_halt(void);
uint8 dmaLoadB(uint8 reg);
uint16 dmaLoadW(uint8 reg);
uint32 dmaLoadL(uint8 reg);
void dmaStoreB(uint8 reg, uint8 value);
void dmaStoreW(uint8 reg, uint16 value);
void dmaStoreL(uint8 reg, uint32 value);

#ifdef __cplusplus
}
#endif
