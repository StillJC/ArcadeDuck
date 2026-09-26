// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <vector>

class Error;

namespace Arcade::Database {
struct FirmwareDefinition;
struct GameDefinition;
}

namespace NamcoSystem11 {

enum class KeycusType : u8
{
  None = 0,
  C406,
  C409,
  C410,
  C411,
  C430,
  C431,
  C432,
  C442,
  C443,
};

enum class C76InputProfile : u8
{
  Standard = 0,
  Tekken,
  SoulEdge,
  MyAngel3,
  PocketRacer,
  PointBlank2,
};

struct LoadedContent
{
  std::string set_name;
  std::vector<u8> program_rom;
  std::vector<u8> banked_rom;
  std::vector<u8> c76_internal;
  std::vector<u8> c76_program;
  std::vector<u8> c352_samples;
  std::vector<u8> iomcu_program;
  KeycusType keycus_type = KeycusType::None;
  C76InputProfile input_profile = C76InputProfile::Standard;
  bool has_gun_interface = false;
  bool has_family_bowl_io = false;
};

std::optional<LoadedContent> LoadSystem11Content(const char* archive_path,
                                               const Arcade::Database::GameDefinition& game,
                                               const char* firmware_archive_path,
                                               const Arcade::Database::FirmwareDefinition& firmware, Error* error);

bool Initialize(LoadedContent content, Error* error);
void Reset();
void Shutdown();
bool IsActive();
bool BeginMainBoardReset();
void EndMainBoardReset();
bool IsFamilyBowlActive();
void AddFamilyBowlTrackballDelta(u32 port, s32 delta_x, s32 delta_y);
void GenerateAudioFrame(s32* left, s32* right);

u32 ReadProgramROM(u32 width, u32 offset);
u32 ReadBankedROM(u32 width, u32 offset);
u32 ReadC76SharedRAM(u32 width, u32 offset);
void WriteC76SharedRAM(u32 width, u32 offset, u32 value);
u32 ReadEEPROM(u32 width, u32 offset);
void WriteEEPROM(u32 width, u32 offset, u32 value);
u32 ReadKEYCUS(u32 width, u32 offset);
void WriteKEYCUS(u32 width, u32 offset, u32 value);
void WriteBankRegister(u32 width, u32 offset, u32 value);
void WriteBankUpperRegister(u32 width, u32 offset, u32 value);
void WriteGunOutput(u32 width, u32 offset, u32 value);

} // namespace NamcoSystem11
