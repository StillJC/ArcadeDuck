// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <vector>

class Error;

namespace Arcade::Database {
struct GameDefinition;
}

namespace NamcoSystem12 {

enum class ROMBoardProfile : u8
{
  Ordinary,
  AlternateBank,
  M8F4Protected
};
struct LoadedContent
{
  std::string set_name;
  std::string machine_config;
  std::string state_class;
  std::string input_profile;
  ROMBoardProfile rom_board_profile = ROMBoardProfile::Ordinary;
  std::vector<u8> program_rom;
  std::vector<u8> banked_rom;
  std::vector<u8> sub_program;
  std::vector<u8> c352_samples;
  std::vector<u8> cyberlead_led_firmware;
  bool alternate_bank = false;
  bool requires_ram_preserving_boot_reset = false;
};

std::optional<LoadedContent> LoadSystem12Content(const char* archive_path,
                                                const Arcade::Database::GameDefinition& game, Error* error);

bool Initialize(LoadedContent content, Error* error);
void Reset();
void Shutdown();
bool IsActive();
void SetVBlank(bool state);
bool BeginMainBoardReset();
void EndMainBoardReset();
void GenerateAudioFrame(s32* left, s32* right);

u32 ReadProgramROM(u32 width, u32 offset);
u32 ReadEXP1(u32 width, u32 offset);
bool WriteEXP1(u32 width, u32 offset, u32 value);
u32 ReadEXP3(u32 width, u32 offset);
bool WriteEXP3(u32 width, u32 offset, u32 value);
u32 ReadBankedROM(u32 width, u32 offset);

void DMARead(u32* destination, u32 word_count);

} // namespace NamcoSystem12
