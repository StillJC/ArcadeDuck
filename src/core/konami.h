#pragma once

#include "common/types.h"

void KonamiInit(void);
void KonamiTerm(void);

void KonamiDmaControlWrite(u32& ControlBits, u32& Address, u32 Value);

void KonamiScsiRead(u32 Size, u32 Offset, u32& Value);
void KonamiScsiWrite(u32 Size, u32 Offset, u32 Value);

void KonamiP1Read(u32 Size, u32 Offset, u32& Value);
void KonamiP1Write(u32 Size, u32 Offset, u32 Value);

void KonamiP2Read(u32 Size, u32 Offset, u32& Value);
void KonamiP2Write(u32 Size, u32 Offset, u32 Value);

void KonamiFlashRead(u32 Size, u32 Offset, u32& Value);
void KonamiFlashWrite(u32 Size, u32 Offset, u32 Value);

void KonamiKDeadEyeFlashRead(u32 Size, u32 Offset, u32& Value);
void KonamiKDeadEyeFlashWrite(u32 Size, u32 Offset, u32 Value);

bool KonamiIsKDeadEye();

void KonamiEepromRead(u32 Size, u32 Offset, u32& Value);
void KonamiEepromWrite(u32 Size, u32 Offset, u32 Value);

void KonamiTrackballRead(u32 Size, u32 Offset, u32& Value);
void KonamiTrackballWrite(u32 Size, u32 Offset, u32 Value);

void KonamiLightgunX1Read(u32 Size, u32 Offset, u32& Value);
void KonamiLightgunY1Read(u32 Size, u32 Offset, u32& Value);
void KonamiLightgunX2Read(u32 Size, u32 Offset, u32& Value);
void KonamiLightgunY2Read(u32 Size, u32 Offset, u32& Value);
void KonamiLightgunButtonsRead(u32 Size, u32 Offset, u32& Value);
void KonamiLightgunWrite(u32 Size, u32 Offset, u32 Value);

void KonamiLightgunSetPosition(u32 Player, float X, float Y);
void KonamiLightgunSetTrigger(u32 Player, bool Pressed);

void KonamiButtonsSet(u32 Buttons);

void KonamiTrackballSetXY(u16 X, u16 Y);
void KonamiTrackballAddDelta(s32 X, s32 Y);
void KonamiTrackballReset();

void KonamiScsiIrqDeassert(void);
void KonamiEepromFixup(void);

void KonamiScoreInit(void);
void KonamiScoreUpdate(void);
