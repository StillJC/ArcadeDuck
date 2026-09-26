// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "game_database.h"
#include "core/arcade/arcade_database.h"
#include "host.h"
#include "system.h"

#include "util/cd_image.h"
#include "util/imgui_manager.h"

#include "common/assert.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"

#include <optional>
#include <span>

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"

Log_SetChannel(GameDatabase);

namespace GameDatabase {

static Entry* GetMutableEntry(std::string_view serial);
static const Entry* GetEntryForId(std::string_view code);

static constexpr const std::array<const char*, static_cast<u32>(GameDatabase::Trait::Count)> s_trait_names = {{
  "ForceInterpreter",
  "ForceSoftwareRenderer",
  "ForceSoftwareRendererForReadbacks",
  "ForceRoundTextureCoordinates",
  "ForceAccurateBlending",
  "ForceInterlacing",
  "DisableAutoAnalogMode",
  "DisableTrueColor",
  "DisableUpscaling",
  "DisableTextureFiltering",
  "DisableSpriteTextureFiltering",
  "DisableScaledDithering",
  "DisableForceNTSCTimings",
  "DisableWidescreen",
  "DisablePGXP",
  "DisablePGXPCulling",
  "DisablePGXPTextureCorrection",
  "DisablePGXPColorCorrection",
  "DisablePGXPDepthBuffer",
  "DisablePGXPPreserveProjFP",
  "DisablePGXPOn2DPolygons",
  "ForcePGXPVertexCache",
  "ForcePGXPCPUMode",
  "ForceRecompilerMemoryExceptions",
  "ForceRecompilerICache",
  "ForceRecompilerLUTFastmem",
  "IsLibCryptProtected",
}};

static constexpr const std::array<const char*, static_cast<u32>(GameDatabase::Trait::Count)> s_trait_display_names = {{
  TRANSLATE_NOOP("GameDatabase", "Force Interpreter"),
  TRANSLATE_NOOP("GameDatabase", "Force Software Renderer"),
  TRANSLATE_NOOP("GameDatabase", "Force Software Renderer For Readbacks"),
  TRANSLATE_NOOP("GameDatabase", "Force Round Texture Coordinates"),
  TRANSLATE_NOOP("GameDatabase", "Force Accurate Blending"),
  TRANSLATE_NOOP("GameDatabase", "Force Interlacing"),
  TRANSLATE_NOOP("GameDatabase", "Disable Automatic Analog Mode"),
  TRANSLATE_NOOP("GameDatabase", "Disable True Color"),
  TRANSLATE_NOOP("GameDatabase", "Disable Upscaling"),
  TRANSLATE_NOOP("GameDatabase", "Disable Texture Filtering"),
  TRANSLATE_NOOP("GameDatabase", "Disable Sprite Texture Filtering"),
  TRANSLATE_NOOP("GameDatabase", "Disable Scaled Dithering"),
  TRANSLATE_NOOP("GameDatabase", "Disable Force NTSC Timings"),
  TRANSLATE_NOOP("GameDatabase", "Disable Widescreen"),
  TRANSLATE_NOOP("GameDatabase", "Disable PGXP"),
  TRANSLATE_NOOP("GameDatabase", "Disable PGXP Culling"),
  TRANSLATE_NOOP("GameDatabase", "Disable PGXP Texture Correction"),
  TRANSLATE_NOOP("GameDatabase", "Disable PGXP Color Correction"),
  TRANSLATE_NOOP("GameDatabase", "Disable PGXP Depth Buffer"),
  TRANSLATE_NOOP("GameDatabase", "Disable PGXP Preserve Projection Floating Point"),
  TRANSLATE_NOOP("GameDatabase", "Disable PGXP on 2D Polygons"),
  TRANSLATE_NOOP("GameDatabase", "Force PGXP Vertex Cache"),
  TRANSLATE_NOOP("GameDatabase", "Force PGXP CPU Mode"),
  TRANSLATE_NOOP("GameDatabase", "Force Recompiler Memory Exceptions"),
  TRANSLATE_NOOP("GameDatabase", "Force Recompiler ICache"),
  TRANSLATE_NOOP("GameDatabase", "Force Recompiler LUT Fastmem"),
  TRANSLATE_NOOP("GameDatabase", "Is LibCrypt Protected"),
}};

static bool s_loaded = false;
static bool s_track_hashes_loaded = false;

static std::vector<GameDatabase::Entry> s_entries;
static PreferUnorderedStringMap<u32> s_code_lookup;

static TrackHashesMap s_track_hashes_map;
} // namespace GameDatabase

void GameDatabase::EnsureLoaded()
{
  if (s_loaded)
    return;

  Common::Timer timer;
  s_loaded = true;
  s_entries = {};
  s_code_lookup = {};

  if (!Arcade::Database::EnsureLoaded())
  {
    ERROR_LOG("Universal arcade database failed to load.");
    return;
  }

  const std::span<const Arcade::Database::GameDefinition> games = Arcade::Database::GetGames();
  s_entries.reserve(games.size());
  for (const Arcade::Database::GameDefinition& game : games)
  {
    const u32 index = static_cast<u32>(s_entries.size());
    Entry& entry = s_entries.emplace_back();
    entry.serial = game.id;
    entry.title = game.title;
    s_code_lookup.emplace(entry.serial, index);
  }

  INFO_LOG("Arcade game settings view loaded {} entries in {:.0f}ms.", s_entries.size(), timer.GetTimeMilliseconds());
}

void GameDatabase::Unload()
{
  s_entries = {};
  s_code_lookup = {};
  s_loaded = false;
}

const GameDatabase::Entry* GameDatabase::GetEntryForId(std::string_view code)
{
  if (code.empty())
    return nullptr;

  EnsureLoaded();

  auto iter = s_code_lookup.find(code);
  return (iter != s_code_lookup.end()) ? &s_entries[iter->second] : nullptr;
}

const GameDatabase::Entry* GameDatabase::GetEntryForDisc(CDImage* image)
{
  std::string id;
  System::GameHash hash;
  System::GetGameDetailsFromImage(image, &id, &hash);
  const Entry* entry = GetEntryForGameDetails(id, hash);
  if (entry)
    return entry;

  WARNING_LOG("No entry found for disc '{}'", id);
  return nullptr;
}

const GameDatabase::Entry* GameDatabase::GetEntryForGameDetails(const std::string& id, u64 hash)
{
  const Entry* entry;

  if (!id.empty())
  {
    entry = GetEntryForId(id);
    if (entry)
      return entry;
  }

  // some games with invalid serials use the hash
  entry = GetEntryForId(System::GetGameHashId(hash));
  if (entry)
    return entry;

  return nullptr;
}

const GameDatabase::Entry* GameDatabase::GetEntryForSerial(std::string_view serial)
{
  EnsureLoaded();

  return GetMutableEntry(serial);
}

GameDatabase::Entry* GameDatabase::GetMutableEntry(std::string_view serial)
{
  for (Entry& entry : s_entries)
  {
    if (entry.serial == serial)
      return &entry;
  }

  return nullptr;
}

const char* GameDatabase::GetTraitName(Trait trait)
{
  return s_trait_names[static_cast<size_t>(trait)];
}

const char* GameDatabase::GetTraitDisplayName(Trait trait)
{
  return Host::TranslateToCString("GameDatabase", s_trait_display_names[static_cast<size_t>(trait)]);
}

void GameDatabase::Entry::ApplySettings(Settings& settings, bool display_osd_messages) const
{
  if (display_active_start_offset.has_value())
  {
    settings.display_active_start_offset = display_active_start_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display active start offset set to {}.", settings.display_active_start_offset);
  }
  if (display_active_end_offset.has_value())
  {
    settings.display_active_end_offset = display_active_end_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display active end offset set to {}.", settings.display_active_end_offset);
  }
  if (display_line_start_offset.has_value())
  {
    settings.display_line_start_offset = display_line_start_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display line start offset set to {}.", settings.display_line_start_offset);
  }
  if (display_line_end_offset.has_value())
  {
    settings.display_line_end_offset = display_line_end_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display line end offset set to {}.", settings.display_line_start_offset);
  }
  if (dma_max_slice_ticks.has_value())
  {
    settings.dma_max_slice_ticks = dma_max_slice_ticks.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: DMA max slice ticks set to {}.", settings.dma_max_slice_ticks);
  }
  if (dma_halt_ticks.has_value())
  {
    settings.dma_halt_ticks = dma_halt_ticks.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: DMA halt ticks set to {}.", settings.dma_halt_ticks);
  }
  if (gpu_fifo_size.has_value())
  {
    settings.gpu_fifo_size = gpu_fifo_size.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU FIFO size set to {}.", settings.gpu_fifo_size);
  }
  if (gpu_max_run_ahead.has_value())
  {
    settings.gpu_max_run_ahead = gpu_max_run_ahead.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU max runahead set to {}.", settings.gpu_max_run_ahead);
  }
  if (gpu_pgxp_tolerance.has_value())
  {
    settings.gpu_pgxp_tolerance = gpu_pgxp_tolerance.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU PGXP tolerance set to {}.", settings.gpu_pgxp_tolerance);
  }
  if (gpu_pgxp_depth_threshold.has_value())
  {
    settings.SetPGXPDepthClearThreshold(gpu_pgxp_depth_threshold.value());
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU depth clear threshold set to {}.", settings.GetPGXPDepthClearThreshold());
  }
  if (gpu_line_detect_mode.has_value())
  {
    settings.gpu_line_detect_mode = gpu_line_detect_mode.value();
    if (display_osd_messages)
    {
      INFO_LOG("GameDB: GPU line detect mode set to {}.",
               Settings::GetLineDetectModeName(settings.gpu_line_detect_mode));
    }
  }

  SmallStackString<512> messages;
#define APPEND_MESSAGE(msg)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    messages.append("\n        \u2022 ");                                                                              \
    messages.append(msg);                                                                                              \
  } while (0)
#define APPEND_MESSAGE_FMT(...)                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    messages.append("\n        \u2022 ");                                                                              \
    messages.append_format(__VA_ARGS__);                                                                               \
  } while (0)

  if (display_crop_mode.has_value())
  {
    if (display_osd_messages && settings.display_crop_mode != display_crop_mode.value())
    {
      APPEND_MESSAGE_FMT(TRANSLATE_FS("GameDatabase", "Display cropping set to {}."),
                         Settings::GetDisplayCropModeDisplayName(display_crop_mode.value()));
    }

    settings.display_crop_mode = display_crop_mode.value();
  }

  if (display_deinterlacing_mode.has_value())
  {
    if (display_osd_messages && settings.display_deinterlacing_mode != display_deinterlacing_mode.value())
    {
      APPEND_MESSAGE_FMT(TRANSLATE_FS("GameDatabase", "Deinterlacing set to {}."),
                         Settings::GetDisplayDeinterlacingModeDisplayName(display_deinterlacing_mode.value()));
    }

    settings.display_deinterlacing_mode = display_deinterlacing_mode.value();
  }

  if (HasTrait(Trait::ForceInterpreter))
  {
    if (display_osd_messages && settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "CPU recompiler disabled."));

    settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
  }

  if (HasTrait(Trait::ForceSoftwareRenderer))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Hardware rendering disabled."));

    settings.gpu_renderer = GPURenderer::Software;
  }

  if (HasTrait(Trait::ForceSoftwareRendererForReadbacks))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Software renderer readbacks enabled."));

    settings.gpu_use_software_renderer_for_readbacks = true;
  }

  if (HasTrait(Trait::ForceRoundUpscaledTextureCoordinates))
  {
    settings.gpu_force_round_texcoords = true;
  }

  if (HasTrait(Trait::ForceAccurateBlending))
  {
    if (display_osd_messages && !settings.IsUsingSoftwareRenderer() && !settings.gpu_accurate_blending)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Accurate blending enabled."));

    settings.gpu_accurate_blending = true;
  }

  if (HasTrait(Trait::ForceInterlacing))
  {
    if (display_osd_messages && settings.gpu_disable_interlacing)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Interlaced rendering enabled."));

    settings.gpu_disable_interlacing = false;
  }

  if (HasTrait(Trait::DisableTrueColor))
  {
    if (display_osd_messages && settings.gpu_true_color)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "True color disabled."));

    settings.gpu_true_color = false;
  }

  if (HasTrait(Trait::DisableUpscaling))
  {
    if (display_osd_messages && settings.gpu_resolution_scale > 1)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Upscaling disabled."));

    settings.gpu_resolution_scale = 1;
  }

  if (HasTrait(Trait::DisableTextureFiltering))
  {
    if (display_osd_messages && (settings.gpu_texture_filter != GPUTextureFilter::Nearest ||
                                 g_settings.gpu_sprite_texture_filter != GPUTextureFilter::Nearest))
    {
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Texture filtering disabled."));
    }

    settings.gpu_texture_filter = GPUTextureFilter::Nearest;
    settings.gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
  }

  if (HasTrait(Trait::DisableSpriteTextureFiltering))
  {
    if (display_osd_messages && g_settings.gpu_sprite_texture_filter != GPUTextureFilter::Nearest)
    {
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Sprite texture filtering disabled."));
    }

    settings.gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
  }

  if (HasTrait(Trait::DisableScaledDithering))
  {
    if (display_osd_messages && settings.gpu_scaled_dithering)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Scaled dithering."));

    settings.gpu_scaled_dithering = false;
  }

  if (HasTrait(Trait::DisableWidescreen))
  {
    if (display_osd_messages && settings.gpu_widescreen_hack)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Widescreen rendering disabled."));

    settings.gpu_widescreen_hack = false;
  }

  if (HasTrait(Trait::DisableForceNTSCTimings))
  {
    if (display_osd_messages && settings.gpu_force_ntsc_timings)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Force NTSC timings disabled."));

    settings.gpu_force_ntsc_timings = false;
  }

  if (HasTrait(Trait::DisablePGXP))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP geometry correction disabled."));

    settings.gpu_pgxp_enable = false;
  }

  if (HasTrait(Trait::DisablePGXPCulling))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_culling)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP culling correction disabled."));

    settings.gpu_pgxp_culling = false;
  }

  if (HasTrait(Trait::DisablePGXPTextureCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP perspective correct textures disabled."));

    settings.gpu_pgxp_texture_correction = false;
  }

  if (HasTrait(Trait::DisablePGXPColorCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction &&
        settings.gpu_pgxp_color_correction)
    {
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP perspective correct colors disabled."));
    }

    settings.gpu_pgxp_color_correction = false;
  }

  if (HasTrait(Trait::DisablePGXPPreserveProjFP))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_preserve_proj_fp)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP preserve projection precision disabled."));

    settings.gpu_pgxp_preserve_proj_fp = false;
  }

  if (HasTrait(Trait::ForcePGXPVertexCache))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_vertex_cache)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP vertex cache enabled."));

    settings.gpu_pgxp_vertex_cache = settings.gpu_pgxp_enable;
  }
  else if (settings.gpu_pgxp_enable && settings.gpu_pgxp_vertex_cache)
  {
    Host::AddIconOSDMessage(
      "gamedb_force_pgxp_vertex_cache", ICON_EMOJI_WARNING,
      TRANSLATE_STR(
        "GameDatabase",
        "PGXP Vertex Cache is enabled, but it is not required for this game. This may cause rendering errors."),
      Host::OSD_WARNING_DURATION);
  }

  if (HasTrait(Trait::ForcePGXPCPUMode))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_cpu)
    {
#ifndef __ANDROID__
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP CPU mode enabled."));
#else
      Host::AddIconOSDMessage("gamedb_force_pgxp_cpu", ICON_EMOJI_WARNING,
                              "This game requires PGXP CPU mode, which increases system requirements.\n"
                              "      If the game runs too slow, disable PGXP for this game.",
                              Host::OSD_WARNING_DURATION);
#endif
    }

    settings.gpu_pgxp_cpu = settings.gpu_pgxp_enable;
  }
  else if (settings.UsingPGXPCPUMode())
  {
    Host::AddIconOSDMessage(
      "gamedb_force_pgxp_cpu", ICON_EMOJI_WARNING,
      TRANSLATE_STR("GameDatabase",
                    "PGXP CPU mode is enabled, but it is not required for this game. This may cause rendering errors."),
      Host::OSD_WARNING_DURATION);
  }

  if (HasTrait(Trait::DisablePGXPDepthBuffer))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_depth_buffer)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP depth buffer disabled."));

    settings.gpu_pgxp_depth_buffer = false;
  }

  if (HasTrait(Trait::DisablePGXPOn2DPolygons))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_disable_2d)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP disabled on 2D polygons."));

    g_settings.gpu_pgxp_disable_2d = true;
  }

  if (HasTrait(Trait::ForceRecompilerMemoryExceptions))
  {
    WARNING_LOG("Memory exceptions for recompiler forced by compatibility settings.");
    settings.cpu_recompiler_memory_exceptions = true;
  }

  if (HasTrait(Trait::ForceRecompilerICache))
  {
    WARNING_LOG("ICache for recompiler forced by compatibility settings.");
    settings.cpu_recompiler_icache = true;
  }

  if (settings.cpu_fastmem_mode == CPUFastmemMode::MMap && HasTrait(Trait::ForceRecompilerLUTFastmem))
  {
    WARNING_LOG("LUT fastmem for recompiler forced by compatibility settings.");
    settings.cpu_fastmem_mode = CPUFastmemMode::LUT;
  }

  if (!messages.empty())
  {
    Host::AddIconOSDMessage(
      "GameDBCompatibility", ICON_EMOJI_INFORMATION,
      fmt::format("{}{}", TRANSLATE_SV("GameDatabase", "Compatibility settings for this game have been applied."),
                  messages.view()),
      Host::OSD_WARNING_DURATION);
  }

#undef APPEND_MESSAGE_FMT
#undef APPEND_MESSAGE

}

void GameDatabase::EnsureTrackHashesMapLoaded()
{
  if (s_track_hashes_loaded)
    return;

  // Retail PlayStation disc verification data is intentionally not part of ArcadeDuck's arcade database.
  s_track_hashes_loaded = true;
  s_track_hashes_map.clear();
}

const GameDatabase::TrackHashesMap& GameDatabase::GetTrackHashesMap()
{
  EnsureTrackHashesMapLoaded();
  return s_track_hashes_map;
}
