#pragma once

#include "common/types.h"

// Konami GV SCSI CD-ROM media interface.
//
// This layer owns an independent CDImage instance so GV SCSI reads do not
// change the position of DuckStation's normal PlayStation CD-ROM reader.

void KonamiGVCDROMInitialize();
void KonamiGVCDROMShutdown();

bool KonamiGVCDROMReadDataSector(u32 lba, u8* sector);
u32 KonamiGVCDROMReadTOC(const u8* cdb, u8* response, u32 response_size);
