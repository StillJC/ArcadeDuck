// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gq/konami_gq.h"
#include "core/arcade/arcade_database.h"
#include "core/arcade/arcade_rom_loader.h"
#include "core/arcade/arcade_input.h"
#include "core/arcade/arcade_output.h"
#include "core/arcade/systems/konami/gq/konami_gq_eeprom.h"
#include "core/arcade/systems/konami/gq/konami_gq_hdd.h"
#include "core/arcade/systems/konami/gq/konami_gq_k056800.h"
#include "core/arcade/systems/konami/gq/konami_gq_pcm.h"
#include "core/arcade/systems/konami/gq/konami_gq_scsi.h"
#include "core/arcade/systems/konami/gq/konami_gq_sound_cpu.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"
#include "core/bios.h"
#include "core/bus.h"
#include "util/cd_image.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

Log_SetChannel(Konami);

namespace Konami {

namespace {

constexpr u16 GQ_LIGHTGUN_X_MIN = 0x007d;
constexpr u16 GQ_LIGHTGUN_X_MAX = 0x01bc;
constexpr u16 GQ_LIGHTGUN_Y_MIN = 0x0000;
constexpr u16 GQ_LIGHTGUN_Y_MAX = 0x00ef;
constexpr u16 GQ_LIGHTGUN_X_CENTER = 0x011d;
constexpr u16 GQ_LIGHTGUN_Y_CENTER = 0x0078;

std::mutex s_lightgun_mutex;

struct GQRuntimeState
{
  const GQGameDefinition* definition = nullptr;
  std::string set_name;
  std::string title;
  std::string hardware_profile;
  std::array<u8, BIOS::BIOS_SIZE> bios;
  std::vector<u8> sound_program;
  std::vector<u8> pcm_samples;
  std::string chd_path;
  std::array<u32, 3> buttons = {{0xffffffff, 0xffffffff, 0xffffffff}};
  std::array<u32, 2> gq_output_registers = {};
  std::array<bool, 2> gq_output_register_valid = {};
  std::array<float, 3> lightgun_normalized_x = {{0.5f, 0.5f, 0.5f}};
  std::array<float, 3> lightgun_normalized_y = {{0.5f, 0.5f, 0.5f}};
  bool test_pressed = false;
  bool bios_installed = false;
  bool active = false;
};

std::optional<GQRuntimeState> s_gq_runtime;

static u16 ScaleLightgunAxis(float normalized, u16 min_value, u16 max_value)
{
  normalized = std::clamp(normalized, 0.0f, 1.0f);
  return static_cast<u16>(static_cast<float>(min_value) +
                          (normalized * static_cast<float>(max_value - min_value)));
}

static void ResetGQCabinetIO(GQRuntimeState& runtime)
{
  runtime.buttons = {{0xffffffff, 0xffffffff, 0xffffffff}};
  runtime.gq_output_registers.fill(0);
  runtime.gq_output_register_valid.fill(false);
  runtime.test_pressed = false;

  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  runtime.lightgun_normalized_x = {{0.5f, 0.5f, 0.5f}};
  runtime.lightgun_normalized_y = {{0.5f, 0.5f, 0.5f}};
}

static u16 GetGQLightgunX(GQRuntimeState& runtime, u32 player)
{
  if (player >= runtime.lightgun_normalized_x.size())
    return GQ_LIGHTGUN_X_CENTER;
  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  return ScaleLightgunAxis(runtime.lightgun_normalized_x[player], GQ_LIGHTGUN_X_MIN, GQ_LIGHTGUN_X_MAX);
}

static u16 GetGQLightgunY(GQRuntimeState& runtime, u32 player)
{
  if (player >= runtime.lightgun_normalized_y.size())
    return GQ_LIGHTGUN_Y_CENTER;
  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  return ScaleLightgunAxis(runtime.lightgun_normalized_y[player], GQ_LIGHTGUN_Y_MIN, GQ_LIGHTGUN_Y_MAX);
}

static u32 ExtractGQInputRegisterValue(u32 value, u32 size, u32 offset, u32 register_offset)
{
  const u32 byte_offset = (offset - register_offset) & 3U;
  switch (size)
  {
    case 1: return (value >> (byte_offset * 8U)) & 0xffU;
    case 2: return (value >> ((byte_offset & 2U) * 8U)) & 0xffffU;
    case 4: return value;
    default: return 0xffffffffU;
  }
}

std::vector<GQGameDefinition> s_gq_game_definitions;
bool s_gq_game_definitions_initialized = false;

void EnsureGQGameDefinitionsInitialized()
{
  if (s_gq_game_definitions_initialized)
    return;

  s_gq_game_definitions_initialized = true;
  if (!Arcade::Database::EnsureLoaded())
    return;

  for (const Arcade::Database::GameDefinition& game : Arcade::Database::GetGames())
  {
    const Arcade::Database::SystemDefinition* const system = Arcade::Database::GetSystem(game.system_id);
    if (!system || system->machine_handler != "konami_gq")
      continue;

    const Arcade::Database::MediaDefinition* const harddisk =
      Arcade::Database::GetMediaByRole(game, "harddisk");
    if (!harddisk)
    {
      ERROR_LOG("Konami GQ database entry '{}' is missing its hard-disk manifest.", game.id);
      continue;
    }

    s_gq_game_definitions.push_back({game.id, game.title, game.hardware_profile, harddisk->name, harddisk->sha1});
  }
}

} // namespace

const GQGameDefinition* GetGQGameDefinition(std::string_view set_name)
{
  EnsureGQGameDefinitionsInitialized();
  for (const GQGameDefinition& definition : s_gq_game_definitions)
  {
    if (StringUtil::EqualNoCase(definition.set_name, set_name))
      return &definition;
  }

  return nullptr;
}

bool IsGQArchivePath(std::string_view path)
{
  return Arcade::Database::IsArchivePath(path);
}

std::string_view GetGQSetNameFromArchivePath(std::string_view path)
{
  return IsGQArchivePath(path) ? Path::GetFileTitle(path) : std::string_view{};
}

const GQGameDefinition* IdentifyGQArchive(std::string_view path)
{
  const Arcade::Database::GameDefinition* const game = Arcade::Database::IdentifyArchive(path);
  return game ? GetGQGameDefinition(game->id) : nullptr;
}

std::string GetGQCHDFilename(const GQGameDefinition& definition)
{
  return Path::GetExtension(definition.chd_disk_name).empty() ? std::string(definition.chd_disk_name) + ".chd" :
                                                                std::string(definition.chd_disk_name);
}

std::string GetGQCompanionCHDPath(std::string_view archive_path, const GQGameDefinition& definition)
{
  const Arcade::Database::GameDefinition* const game = Arcade::Database::GetGame(definition.set_name);
  const Arcade::Database::MediaDefinition* const harddisk =
    game ? Arcade::Database::GetMediaByRole(*game, "harddisk") : nullptr;
  if (game && harddisk)
    return Arcade::Database::GetCompanionMediaPath(archive_path, *game, *harddisk);

  const std::string filename = GetGQCHDFilename(definition);
  return Arcade::Database::GetCompanionMediaPath(archive_path, definition.set_name, std::string_view{}, filename);
}

bool InitializeGQ(const BIOS::Image& bios, GQLoadedContent content, std::string_view persistence_directory,
                  Error* error)
{
  ShutdownGQ();
  if (!content.definition || content.set_name != content.definition->set_name || content.chd_path.empty() ||
      bios.data.size() != BIOS::BIOS_SIZE)
  {
    Error::SetStringView(error, "Invalid Konami GQ lifecycle initialization data.");
    ERROR_LOG("KonamiGQ.Lifecycle initialization_failed reason='invalid_content'");
    return false;
  }

  GQRuntimeState runtime;
  runtime.definition = content.definition;
  runtime.set_name.assign(content.definition->set_name);
  runtime.title.assign(content.definition->title);
  runtime.hardware_profile.assign(content.definition->hardware_profile);
  std::memcpy(runtime.bios.data(), bios.data.data(), runtime.bios.size());
  runtime.sound_program = std::move(content.sound_program);
  runtime.pcm_samples = std::move(content.pcm_samples);
  runtime.chd_path = std::move(content.chd_path);
  ResetGQCabinetIO(runtime);

  if (!KonamiGQHDD::Initialize(runtime.chd_path.c_str(), error))
    return false;
  if (!KonamiGQEEPROM::Initialize(runtime.set_name, persistence_directory, error))
  {
    KonamiGQHDD::Shutdown();
    return false;
  }
  KonamiGQK056800::Initialize();
  KonamiGQPCM::Initialize();
  if (!KonamiGQSoundCPU::Initialize(runtime.sound_program, runtime.pcm_samples))
  {
    Error::SetStringView(error, "Failed to initialize the Konami GQ 68000 sound CPU.");
    KonamiGQPCM::Shutdown();
    KonamiGQK056800::Shutdown();
    KonamiGQEEPROM::Shutdown();
    KonamiGQHDD::Shutdown();
    return false;
  }

  std::memcpy(Bus::g_bios, runtime.bios.data(), runtime.bios.size());
  runtime.bios_installed = true;
  runtime.active = true;
  s_gq_runtime.emplace(std::move(runtime));
  KonamiGQScsi::Initialize();

  INFO_LOG("KonamiGQ.Lifecycle initialized canonical_set='{}' title='{}' hardware_profile='{}'",
           s_gq_runtime->set_name, s_gq_runtime->title, s_gq_runtime->hardware_profile);
  VERBOSE_LOG("KonamiGQ.Lifecycle bios_installed canonical_set='{}'", s_gq_runtime->set_name);
  VERBOSE_LOG("KonamiGQ.EXP1 dispatch_activated canonical_set='{}' scsi='0x1F000000-0x1F00001F' k056800='0x1F100000-0x1F10001F' eeprom='0x1F180000-0x1F180001' guns='0x1F200000-0x1F228003' inputs='0x1F230000-0x1F230007' dsw='0x1F238000-0x1F238003' pcm_ram='0x1F300000-0x1F5FFFFF' sound_cpu='MC68000@8MHz'",
           s_gq_runtime->set_name);
  return true;
}

void ResetGQ()
{
  if (!s_gq_runtime || !s_gq_runtime->active)
    return;

  KonamiGQEEPROM::Reset();
  KonamiGQK056800::Reset();
  KonamiGQPCM::Reset();
  KonamiGQSoundCPU::Reset();
  KonamiGQScsi::Reset();
  ResetGQCabinetIO(*s_gq_runtime);
  VERBOSE_LOG("KonamiGQ.Lifecycle reset canonical_set='{}'", s_gq_runtime->set_name);
}

void ShutdownGQ()
{
  if (s_gq_runtime)
  {
    KonamiGQScsi::Shutdown();
    KonamiGQSoundCPU::Shutdown();
    KonamiGQPCM::Shutdown();
    KonamiGQK056800::Shutdown();
    KonamiGQEEPROM::Shutdown();
    KonamiGQHDD::Shutdown();
    VERBOSE_LOG("KonamiGQ.EXP1 dispatch_shutdown canonical_set='{}'", s_gq_runtime->set_name);
    VERBOSE_LOG("KonamiGQ.Lifecycle shutdown canonical_set='{}'", s_gq_runtime->set_name);
  }
  else
  {
    KonamiGQScsi::Shutdown();
    KonamiGQSoundCPU::Shutdown();
    KonamiGQPCM::Shutdown();
    KonamiGQK056800::Shutdown();
    KonamiGQEEPROM::Shutdown();
    KonamiGQHDD::Shutdown();
  }
  s_gq_runtime.reset();
}

bool IsGQActive()
{
  return s_gq_runtime.has_value() && s_gq_runtime->active;
}

bool HasValidGQHardDiskContent()
{
  return IsGQActive() && KonamiGQHDD::IsOpen();
}

std::string_view GetGQSetName()
{
  return s_gq_runtime ? std::string_view(s_gq_runtime->set_name) : std::string_view{};
}

std::string_view GetGQTitle()
{
  return s_gq_runtime ? std::string_view(s_gq_runtime->title) : std::string_view{};
}

std::string_view GetGQCHDPath()
{
  return s_gq_runtime ? std::string_view(s_gq_runtime->chd_path) : std::string_view{};
}

void WriteGQCabinetOutput(u32 size, u32 offset, u32 value)
{
  if (!s_gq_runtime || !s_gq_runtime->active || s_gq_runtime->set_name != "cryptklr")
    return;

  u32 base;
  u32 register_index;

  if (offset >= 0x198000 && offset <= 0x198003)
  {
    base = 0x198000;
    register_index = 0;
  }
  else if (offset >= 0x1a0000 && offset <= 0x1a0003)
  {
    base = 0x1a0000;
    register_index = 1;
  }
  else
  {
    return;
  }

  if (size != 1 && size != 2 && size != 4)
    return;

  GQRuntimeState& runtime = *s_gq_runtime;
  const u32 previous = runtime.gq_output_registers[register_index];
  u32 current = previous;
  const u32 first_byte = offset - base;

  for (u32 i = 0; i < size && (first_byte + i) < 4; i++)
  {
    const u32 register_byte = first_byte + i;
    const u32 register_shift = register_byte * 8u;
    const u32 value_shift = i * 8u;
    const u32 mask = UINT32_C(0xff) << register_shift;
    const u32 byte_value = ((value >> value_shift) & UINT32_C(0xff)) << register_shift;
    current = (current & ~mask) | byte_value;
  }

  runtime.gq_output_registers[register_index] = current;

  // 0x1F198000 bits 0..2 are the three Start button lamps.
  //
  // Other bits in this register, and the 0x1F1A0000 register as a whole,
  // remain latched above to preserve register behavior, but remain unpublished
  // until their physical function is known.
  if (register_index == 0)
  {
    static constexpr const char* START_LAMP_NAMES[3] = {"led0", "led1", "led2"};
    const u32 changed = runtime.gq_output_register_valid[register_index] ? (previous ^ current) : UINT32_C(0x7);

    for (u32 bit = 0; bit < 3; bit++)
    {
      if ((changed & (UINT32_C(1) << bit)) != 0)
      {
        Arcade::Output::SetValue(START_LAMP_NAMES[bit], static_cast<s32>((current >> bit) & 1u),
                                 "Crypt Killer Start lamp");
      }
    }
  }

  runtime.gq_output_register_valid[register_index] = true;
}

u32 ReadGQInput(u32 size, u32 offset)
{
  if (!s_gq_runtime || !s_gq_runtime->active)
    return 0xffffffff;

  GQRuntimeState& runtime = *s_gq_runtime;
  u32 value;
  const u32 register_offset = offset & ~3U;
  switch (register_offset)
  {
    case 0x00200000: value = GetGQLightgunX(runtime, 0); break;
    case 0x00208000: value = GetGQLightgunY(runtime, 0); break;
    case 0x00210000: value = GetGQLightgunX(runtime, 1); break;
    case 0x00218000: value = GetGQLightgunY(runtime, 1); break;
    case 0x00220000: value = GetGQLightgunX(runtime, 2); break;
    case 0x00228000: value = GetGQLightgunY(runtime, 2); break;

    case 0x00230000:
      value = (runtime.buttons[0] & 0x000000ffU) | ((runtime.buttons[1] & 0x000000ffU) << 16);
      break;

    case 0x00230004:
      value = (runtime.buttons[2] & 0x000000ffU) | 0x00ff0000U;
      if (runtime.test_pressed)
        value &= ~0x00200000U;
      break;

    default:
      return 0xffffffff;
  }

  return ExtractGQInputRegisterValue(value, size, offset, register_offset);
}

void ProcessGQFrame()
{
  if (s_gq_runtime && s_gq_runtime->active)
    ArcadeInput::Update();
}

void ResetGQArcadeInputs()
{
  if (s_gq_runtime)
    ResetGQCabinetIO(*s_gq_runtime);
}

void SetGQArcadeButton(u32 player, u32 button_mask, bool pressed)
{
  if (!s_gq_runtime || player >= s_gq_runtime->buttons.size())
    return;

  if (pressed)
    s_gq_runtime->buttons[player] &= ~button_mask;
  else
    s_gq_runtime->buttons[player] |= button_mask;
}

void SetGQTest(bool pressed)
{
  if (s_gq_runtime)
    s_gq_runtime->test_pressed = pressed;
}

void SetGQLightgunPosition(u32 player, float x, float y)
{
  if (!s_gq_runtime || player >= s_gq_runtime->lightgun_normalized_x.size())
    return;

  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  s_gq_runtime->lightgun_normalized_x[player] = std::clamp(x, 0.0f, 1.0f);
  s_gq_runtime->lightgun_normalized_y[player] = std::clamp(y, 0.0f, 1.0f);
}

std::optional<GQLoadedContent> LoadGQContent(const char* archive_path,
                                             const Arcade::Database::GameDefinition& game, Error* error)
{
  const GQGameDefinition* const definition = GetGQGameDefinition(game.id);
  if (!definition)
  {
    Error::SetStringFmt(error, "Konami GQ game '{}' is not a recognized GQ set.", game.id);
    return std::nullopt;
  }

  VERBOSE_LOG("KonamiGQ.Content opening_archive canonical_set='{}' archive_anchor='{}'", definition->set_name,
              archive_path);
  GQLoadedContent content = {definition, definition->set_name, {}, {}, {}, definition->chd_sha1};
  if (!Arcade::LoadGameROMByRole(archive_path, game, "sound_program", &content.sound_program, error) ||
      !Arcade::LoadGameROMByRole(archive_path, game, "pcm_samples", &content.pcm_samples, error))
  {
    return std::nullopt;
  }

  content.chd_path = GetGQCompanionCHDPath(archive_path, *definition);
  VERBOSE_LOG("KonamiGQ.Content expected_chd canonical_set='{}' disk_basename='{}' candidate_path='{}'",
           definition->set_name, definition->chd_disk_name, content.chd_path);
  if (!FileSystem::FileExists(content.chd_path.c_str()))
  {
    Error::SetStringFmt(error, "Konami GQ CHD for '{}' was not found:\n{}", definition->set_name, content.chd_path);
    return std::nullopt;
  }

  std::array<u8, SHA1Digest::DIGEST_SIZE> chd_sha1;
  if (!CDImage::GetCHDImageSHA1(content.chd_path.c_str(), &chd_sha1, error))
    return std::nullopt;
  const std::string chd_sha1_string = SHA1Digest::DigestToString(chd_sha1);
  if (!StringUtil::EqualNoCase(chd_sha1_string, definition->chd_sha1))
  {
    Error::SetStringFmt(error, "Konami GQ CHD '{}' SHA-1 mismatch.", content.chd_path);
    return std::nullopt;
  }

  VERBOSE_LOG("KonamiGQ.Content preflight_complete canonical_set='{}' chd_sha1='{}'", definition->set_name,
           definition->chd_sha1);
  return content;
}

std::optional<GQLoadedContent> LoadGQContent(const char* archive_path, Error* error)
{
  const Arcade::Database::GameDefinition* const game = Arcade::Database::IdentifyArchive(archive_path);
  if (!game)
  {
    Error::SetStringFmt(error, "Konami GQ content archive '{}' is not a recognized arcade set.", archive_path);
    return std::nullopt;
  }

  return LoadGQContent(archive_path, *game, error);
}

bool IsGQSet(std::string_view set_name)
{
  return (GetGQGameDefinition(set_name) != nullptr);
}

} // namespace Konami
