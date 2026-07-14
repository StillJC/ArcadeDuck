#pragma once

#include "common/types.h"

// Legacy Konami GV NCR53CF96/SCSI shim interface.
//
// The implementation will initially be moved from konami.cpp unchanged.
// Behavior will be preserved until the existing shim has been isolated and
// regression-tested.

void KonamiGVScsiInitialize();
void KonamiGVScsiShutdown();

void KonamiGVScsiDmaRead(u32* Data, u32 WordCount);
void KonamiGVScsiDmaWrite(const u32* Data, u32 WordCount);

void KonamiScsiRead(u32 Size, u32 Offset, u32& Value);
void KonamiScsiWrite(u32 Size, u32 Offset, u32 Value);

void KonamiScsiIrqDeassert(void);