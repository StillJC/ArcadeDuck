// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/bios.h"
#include "core/types.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Error;

namespace Arcade::Database {
struct FirmwareDefinition;
struct GameDefinition;
}

namespace SonyZN {

struct CapcomZNContent
{
  bool is_zn2 = false;
  std::string set_name;
  bool use_2mb_vram = false;
  bool qsound_enabled = true;
  std::vector<u8> country_rom;
  std::vector<u8> banked_rom;
  std::vector<u8> audio_cpu_rom;
  std::vector<u8> qsound_rom;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

enum class AcclaimZN1Game : u8
{
  NBAJamExtreme,
  JudgeDredd,
};

struct VideoSystemZN1Content
{
  std::string set_name;
  std::vector<u8> fixed_rom;
  std::vector<u8> banked_rom;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

struct AtlusZN1Content
{
  std::string set_name;
  std::vector<u8> banked_rom;
  std::vector<u8> audio_cpu_rom;
  std::vector<u8> ymz280b_rom;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

struct EightingRaizingZN1Content
{
  std::string set_name;
  std::vector<u8> banked_rom;
  std::vector<u8> audio_cpu_rom;
  std::vector<u8> ymf271_rom;
  std::vector<u8> at28_initial;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
  bool has_ps9805_flash = false;
  bool uses_tecmo_motherboard = false;
};

enum class BustAMove2Media : u8
{
  HardDisk,
  CDROM,
};

struct BustAMove2ZN1Content
{
  std::string set_name;
  std::vector<u8> banked_rom;
  std::string media_path;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
  BustAMove2Media media = BustAMove2Media::HardDisk;
};

struct AcclaimZN1Content
{
  std::string set_name;
  AcclaimZN1Game game = AcclaimZN1Game::NBAJamExtreme;
  std::vector<u8> banked_rom;
  std::vector<u8> rax_rom;
  std::string harddisk_path;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

struct TimeWarnerZN1Content
{
  std::string set_name;
  std::vector<u8> program_rom;
  std::vector<u8> at28_initial;
  std::string harddisk_path;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

struct TaitoFX1AContent
{
  std::string set_name;
  std::vector<u8> banked_rom;
  std::vector<u8> audio_cpu_rom;
  std::vector<u8> ym2610_adpcma_rom;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

struct TaitoGNetContent
{
  std::string set_name;
  std::vector<u8> u30_flash;
  std::vector<u8> f35_eprom;
  std::string pccard_path;
  bool communication_board = false;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> fc_cat702_key{};
};
struct TaitoFX1BContent
{
  std::string set_name;
  bool use_2mb_vram = false;
  std::vector<u8> banked_rom;
  std::vector<u8> mn10200_rom;
  std::vector<u8> zsg2_rom;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

struct TecmoTPSContent
{
  std::string set_name;
  bool cbaj_sound_enabled = false;
  bool gr2_link_enabled = false;
  std::vector<u8> banked_rom;
  std::vector<u8> at28_initial;
  std::vector<u8> audio_cpu_rom;
  std::vector<u8> ymz280b_rom;
  std::vector<u8> link_cpu_rom;
  std::array<u8, 8> motherboard_cat702_key{};
  std::array<u8, 8> game_cat702_key{};
};

std::optional<BIOS::Image> LoadFirmwareBIOS(const char* firmware_archive_path,
                                            const Arcade::Database::FirmwareDefinition& firmware,
                                            std::string_view bios_variant, Error* error);

std::optional<CapcomZNContent> LoadCapcomZNContent(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error);
std::optional<VideoSystemZN1Content> LoadVideoSystemZN1Content(const char* archive_path,
                                                                const Arcade::Database::GameDefinition& game,
                                                                const char* firmware_archive_path,
                                                                const Arcade::Database::FirmwareDefinition& firmware,
                                                                Error* error);
std::optional<AtlusZN1Content> LoadAtlusZN1Content(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error);
std::optional<EightingRaizingZN1Content> LoadEightingRaizingZN1Content(
  const char* archive_path, const Arcade::Database::GameDefinition& game, const char* firmware_archive_path,
  const Arcade::Database::FirmwareDefinition& firmware, Error* error);
std::optional<BustAMove2ZN1Content> LoadBustAMove2ZN1Content(
  const char* archive_path, const Arcade::Database::GameDefinition& game, const char* firmware_archive_path,
  const Arcade::Database::FirmwareDefinition& firmware, Error* error);
std::optional<AcclaimZN1Content> LoadAcclaimZN1Content(const char* archive_path,
                                                      const Arcade::Database::GameDefinition& game,
                                                      const char* firmware_archive_path,
                                                      const Arcade::Database::FirmwareDefinition& firmware,
                                                      Error* error);
std::optional<TimeWarnerZN1Content> LoadTimeWarnerZN1Content(
  const char* archive_path, const Arcade::Database::GameDefinition& game, const char* firmware_archive_path,
  const Arcade::Database::FirmwareDefinition& firmware, Error* error);
std::optional<TaitoFX1AContent> LoadTaitoFX1AContent(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error);
std::optional<TaitoFX1BContent> LoadTaitoFX1BContent(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error);

std::optional<TaitoGNetContent> LoadTaitoGNetContent(const char* archive_path,
                                                      const Arcade::Database::GameDefinition& game,
                                                      const char* firmware_archive_path,
                                                      const Arcade::Database::FirmwareDefinition& firmware,
                                                      Error* error);
std::optional<TecmoTPSContent> LoadTecmoTPSContent(const char* archive_path,
                                                  const Arcade::Database::GameDefinition& game,
                                                  const char* firmware_archive_path,
                                                  const Arcade::Database::FirmwareDefinition& firmware,
                                                  Error* error);

bool InitializeTaitoGNet(const BIOS::Image& bios, TaitoGNetContent content, std::string_view persistence_directory,
                         Error* error);
bool InitializeCapcomZN(const BIOS::Image& bios, CapcomZNContent content, std::string_view persistence_directory,
                        Error* error);
bool InitializeVideoSystemZN1(const BIOS::Image& bios, VideoSystemZN1Content content,
                              std::string_view persistence_directory, Error* error);
bool InitializeAtlusZN1(const BIOS::Image& bios, AtlusZN1Content content, std::string_view persistence_directory,
                         Error* error);
bool InitializeEightingRaizingZN1(const BIOS::Image& bios, EightingRaizingZN1Content content,
                                  std::string_view persistence_directory, Error* error);
bool InitializeBustAMove2ZN1(const BIOS::Image& bios, BustAMove2ZN1Content content,
                            std::string_view persistence_directory, Error* error);
bool InitializeAcclaimZN1(const BIOS::Image& bios, AcclaimZN1Content content, std::string_view persistence_directory,
                          Error* error);
bool InitializeTimeWarnerZN1(const BIOS::Image& bios, TimeWarnerZN1Content content,
                             std::string_view persistence_directory, Error* error);
bool InitializeTaitoFX1A(const BIOS::Image& bios, TaitoFX1AContent content, std::string_view persistence_directory,
                         Error* error);
bool InitializeTaitoFX1B(const BIOS::Image& bios, TaitoFX1BContent content, std::string_view persistence_directory,
                         Error* error);
bool InitializeTecmoTPS(const BIOS::Image& bios, TecmoTPSContent content, std::string_view persistence_directory,
                        Error* error);
void PrepareForCPUClockChange();
void CompleteCPUClockChange();
void PrepareForTimingEpochReset();
void Reset();
void Shutdown();
bool IsActive();
void ApplySPUOutputGain(s32* left, s32* right);
void ProcessFrame();

// ZN daughterboard watchdog full-board reset handshake, consumed at a safe frame boundary.
bool BeginMainBoardReset();
void EndMainBoardReset();

u32 ReadEXP1(u32 width, u32 offset);
bool WriteEXP1(u32 width, u32 offset, u32 value);
u32 ReadEXP3(u32 width, u32 offset);
bool ReadEXP3InstructionWord(u32 offset, u32* value);
void WriteEXP3(u32 width, u32 offset, u32 value);

// PSX SIO0/controller-port register block used by ZN security hardware.
u32 ReadSIO0Register(u32 offset);
void WriteSIO0Register(u32 offset, u32 value);

void GenerateAudioFrame(s32* left, s32* right);

} // namespace SonyZN