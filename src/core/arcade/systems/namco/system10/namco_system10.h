// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Error;

namespace Arcade::Database {
struct GameDefinition;
}

namespace NamcoSystem10 {

enum class BoardKind : u8
{
  MemN = 0,
  MemM,
};

struct MemNProfile;

struct MemNLoadedContent
{
  std::string set_name;
  const MemNProfile* profile = nullptr;
  std::vector<std::vector<u8>> nands;
  std::vector<u8> nonlinear_table;
};

enum class MemMBoardProfile : u8
{
  Unknown = 0,
  MrDriller2,
};

struct MemMLoadedContent
{
  std::string set_name;
  MemMBoardProfile board_profile = MemMBoardProfile::Unknown;
  std::vector<u8> program_flash;
  std::vector<u8> data_rom;
};

struct BoardProfile
{
  std::string_view hardware_profile;
  BoardKind kind;
  const MemNProfile* memn_profile = nullptr;
  MemMBoardProfile memm_profile = MemMBoardProfile::Unknown;
};

std::optional<BoardProfile> FindBoardProfile(std::string_view hardware_profile);

/// Loads the Mr. Driller 2 MEM(M) board without modifying the dumped flash or
/// mask-ROM bytes. Static/dynamic board transforms are applied at read time.
std::optional<MemMLoadedContent> LoadMrDriller2Content(const char* archive_path,
                                                       const Arcade::Database::GameDefinition& game, Error* error);

/// Loads a MEM(N)-family board without transforming the NAND bytes. The raw
/// 0x210-byte page image, including spare/OOB, is retained.
std::optional<MemNLoadedContent> LoadMemNContent(const char* archive_path, const Arcade::Database::GameDefinition& game,
                                                Error* error);

std::optional<MemNLoadedContent> LoadExperimentalMemNProbeContent(
  const char* archive_path, const Arcade::Database::GameDefinition& game, bool* eligible, Error* error);

bool InitializeMemN(MemNLoadedContent content, Error* error);
bool InitializeMemM(MemMLoadedContent content, Error* error);
void Reset();
void Shutdown();
bool IsActive();

/// Reads the active System 10 profile's boot/program aperture as seen through
/// the PlayStation BIOS window. Unsupported/out-of-range reads return all ones.
u32 ReadProgramROM(u32 width, u32 offset);

/// Handles System 10 devices mapped into the PlayStation EXP1 window.
/// Returns false when the offset is not owned by the active System 10 profile.
bool ReadEXP1(u32 width, u32 offset, u32* value);
bool WriteEXP1(u32 width, u32 offset, u32 value);

/// Reads MEM(N) data through System 10's PlayStation PIO DMA channel 5 path.
/// Returns false when no MEM(N) runtime is active.
bool DMARead(u32* words, u32 word_count);

/// Handles the currently modeled System 10 base-board EXP3 registers:
/// I2C clock/data plus interrupt cause/acknowledge. Attached-device contents,
/// direct input registers, EXIO identification, and other EXP3 devices remain OPEN.
bool ReadEXP3(u32 width, u32 offset, u32* value);
bool WriteEXP3(u32 width, u32 offset, u32 value);

} // namespace NamcoSystem10
