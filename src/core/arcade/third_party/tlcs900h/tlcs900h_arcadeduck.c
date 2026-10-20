// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
#include "tlcs900h_arcadeduck.h"
#include "TLCS900h_interpret.h"
#include "TLCS900h_registers.h"

static ArcadeDuckTLCS900HCallbacks s_callbacks;
static bool s_halted;

void ArcadeDuckTLCS900HSetCallbacks(const ArcadeDuckTLCS900HCallbacks* callbacks)
{
  s_callbacks = *callbacks;
}

void ArcadeDuckTLCS900HReset(uint32 reset_pc, uint32 xsp)
{
  reset_registers();
  pc = reset_pc & 0xFFFFFFu;
  REGXSP = xsp;
  s_halted = false;
}

int32 ArcadeDuckTLCS900HStep(void)
{
  if (s_halted)
    return 8;

  return TLCS900h_interpret();
}

uint8 ArcadeDuckTLCS900HGetLastOpcode(void)
{
  return first;
}

bool ArcadeDuckTLCS900HIsHalted(void)
{
  return s_halted;
}

uint32 ArcadeDuckTLCS900HGetPC(void)
{
  return pc & 0xFFFFFFu;
}

uint8 ArcadeDuckTLCS900HGetIFF(void)
{
  return statusIFF();
}

void ArcadeDuckTLCS900HTakeInterrupt(uint8 level, uint8 vector)
{
  push32(pc);
  push16(sr);
  setStatusIFF(level);
  pc = loadL(0xFFFF00u + (uint32)vector) & 0xFFFFFFu;
  s_halted = false;
}

uint8 loadB(uint32 address) { return s_callbacks.read8(s_callbacks.context, address & 0xFFFFFFu); }
uint16 loadW(uint32 address) { return s_callbacks.read16(s_callbacks.context, address & 0xFFFFFFu); }
uint32 loadL(uint32 address) { return s_callbacks.read32(s_callbacks.context, address & 0xFFFFFFu); }
void storeB(uint32 address, uint8 value) { s_callbacks.write8(s_callbacks.context, address & 0xFFFFFFu, value); }
void storeW(uint32 address, uint16 value) { s_callbacks.write16(s_callbacks.context, address & 0xFFFFFFu, value); }
void storeL(uint32 address, uint32 value) { s_callbacks.write32(s_callbacks.context, address & 0xFFFFFFu, value); }

// Interrupt acceptance is evaluated by the host between instructions.
void int_check_pending(void) {}

void cpu_halt(void)
{
  s_halted = true;
}

uint8 dmaLoadB(uint8 reg) { return (uint8)s_callbacks.control_read(s_callbacks.context, reg, 1); }
uint16 dmaLoadW(uint8 reg) { return (uint16)s_callbacks.control_read(s_callbacks.context, reg, 2); }
uint32 dmaLoadL(uint8 reg) { return s_callbacks.control_read(s_callbacks.context, reg, 4); }
void dmaStoreB(uint8 reg, uint8 value) { s_callbacks.control_write(s_callbacks.context, reg, 1, value); }
void dmaStoreW(uint8 reg, uint16 value) { s_callbacks.control_write(s_callbacks.context, reg, 2, value); }
void dmaStoreL(uint8 reg, uint32 value) { s_callbacks.control_write(s_callbacks.context, reg, 4, value); }
