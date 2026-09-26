// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once
#include "core/types.h"
#include "util/cd_image_hasher.h"
#include <bitset>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CDImage;

struct Settings;

namespace GameDatabase {
enum class Trait : u32
{
  ForceInterpreter,
  ForceSoftwareRenderer,
  ForceSoftwareRendererForReadbacks,
  ForceRoundUpscaledTextureCoordinates,
  ForceAccurateBlending,
  ForceInterlacing,
  DisableAutoAnalogMode,
  DisableTrueColor,
  DisableUpscaling,
  DisableTextureFiltering,
  DisableSpriteTextureFiltering,
  DisableScaledDithering,
  DisableForceNTSCTimings,
  DisableWidescreen,
  DisablePGXP,
  DisablePGXPCulling,
  DisablePGXPTextureCorrection,
  DisablePGXPColorCorrection,
  DisablePGXPDepthBuffer,
  DisablePGXPPreserveProjFP,
  DisablePGXPOn2DPolygons,
  ForcePGXPVertexCache,
  ForcePGXPCPUMode,
  ForceRecompilerMemoryExceptions,
  ForceRecompilerICache,
  ForceRecompilerLUTFastmem,
  IsLibCryptProtected,

  Count
};

struct Entry
{
  // TODO: Make string_view.
  std::string serial;
  std::string title;
  std::bitset<static_cast<int>(Trait::Count)> traits{};
  std::optional<s16> display_active_start_offset;
  std::optional<s16> display_active_end_offset;
  std::optional<s8> display_line_start_offset;
  std::optional<s8> display_line_end_offset;
  std::optional<DisplayCropMode> display_crop_mode;
  std::optional<DisplayDeinterlacingMode> display_deinterlacing_mode;
  std::optional<GPULineDetectMode> gpu_line_detect_mode;
  std::optional<u32> dma_max_slice_ticks;
  std::optional<u32> dma_halt_ticks;
  std::optional<u32> gpu_fifo_size;
  std::optional<u32> gpu_max_run_ahead;
  std::optional<float> gpu_pgxp_tolerance;
  std::optional<float> gpu_pgxp_depth_threshold;

  ALWAYS_INLINE bool HasTrait(Trait trait) const { return traits[static_cast<int>(trait)]; }

  void ApplySettings(Settings& settings, bool display_osd_messages) const;

};

void EnsureLoaded();
void Unload();

const Entry* GetEntryForDisc(CDImage* image);
const Entry* GetEntryForGameDetails(const std::string& id, u64 hash);
const Entry* GetEntryForSerial(std::string_view serial);
const char* GetTraitName(Trait trait);
const char* GetTraitDisplayName(Trait trait);

/// Map of track hashes for image verification
struct TrackData
{
  TrackData(std::string serial_, std::string revision_str_, uint32_t revision_)
    : serial(std::move(serial_)), revision_str(std::move(revision_str_)), revision(revision_)
  {
  }

  friend bool operator==(const TrackData& left, const TrackData& right)
  {
    // 'revisionString' is deliberately ignored in comparisons as it's redundant with comparing 'revision'! Do not
    // change!
    return left.serial == right.serial && left.revision == right.revision;
  }

  std::string serial;
  std::string revision_str;
  u32 revision;
};

using TrackHashesMap = std::multimap<CDImageHasher::Hash, TrackData>;
const TrackHashesMap& GetTrackHashesMap();
void EnsureTrackHashesMapLoaded();

} // namespace GameDatabase
