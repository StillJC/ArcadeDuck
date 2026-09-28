// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "types.h"

#include "util/audio_stream.h"

#include "common/log.h"
#include "common/settings_interface.h"
#include "common/small_string.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class RenderAPI : u32;
enum class MediaCaptureBackend : u8;

struct SettingInfo
{
  enum class Type
  {
    Boolean,
    Integer,
    IntegerList,
    Float,
    String,
    Path,
  };

  Type type;
  const char* name;
  const char* display_name;
  const char* description;
  const char* default_value;
  const char* min_value;
  const char* max_value;
  const char* step_value;
  const char* format;
  const char** options;
  float multiplier;

  const char* StringDefaultValue() const;
  bool BooleanDefaultValue() const;
  s32 IntegerDefaultValue() const;
  s32 IntegerMinValue() const;
  s32 IntegerMaxValue() const;
  s32 IntegerStepValue() const;
  float FloatDefaultValue() const;
  float FloatMinValue() const;
  float FloatMaxValue() const;
  float FloatStepValue() const;
};

struct Settings
{
  Settings() = default;

  ConsoleRegion region = DEFAULT_CONSOLE_REGION;

  CPUExecutionMode cpu_execution_mode = DEFAULT_CPU_EXECUTION_MODE;
  CPUFastmemMode cpu_fastmem_mode = DEFAULT_CPU_FASTMEM_MODE;
  bool cpu_overclock_enable : 1 = false;
  bool cpu_overclock_active : 1 = false;
  bool cpu_recompiler_memory_exceptions : 1 = false;
  bool cpu_recompiler_block_linking : 1 = true;
  bool cpu_recompiler_icache : 1 = false;
  u32 cpu_overclock_numerator = 1;
  u32 cpu_overclock_denominator = 1;

  float emulation_speed = 1.0f;
  float fast_forward_speed = 0.0f;
  float turbo_speed = 0.0f;
  bool sync_to_host_refresh_rate : 1 = false;
  bool increase_timer_resolution : 1 = true;
  bool inhibit_screensaver : 1 = true;
  bool start_paused : 1 = false;
  bool start_fullscreen : 1 = false;
  bool pause_on_focus_loss : 1 = false;
  bool pause_on_controller_disconnection : 1 = false;
  bool save_state_on_exit : 1 = true;
  bool create_save_state_backups : 1 = DEFAULT_SAVE_STATE_BACKUPS;
  bool confim_power_off : 1 = true;
  bool load_devices_from_save_states : 1 = false;
  bool apply_compatibility_settings : 1 = true;
  bool apply_game_settings : 1 = true;
  bool enable_cheats : 1 = false;
  bool disable_all_enhancements : 1 = false;
  bool enable_discord_presence : 1 = false;

  // Network transport for supported linked arcade hardware.
  bool system_link_enabled : 1 = false;
  u16 system_link_port = 19702;
  std::string system_link_server_address = "127.0.0.1";

  // External cabinet output publishing. Protocol: 0 = Win32, 1 = TCP, 2 = both.
  bool arcade_external_outputs_enabled : 1 = false;
  u8 arcade_external_outputs_protocol = 0;

  bool rewind_enable : 1 = false;
  float rewind_save_frequency = 10.0f;
  u32 rewind_save_slots = 10;
  u32 runahead_frames = 0;

  GPURenderer gpu_renderer = DEFAULT_GPU_RENDERER;
  std::string gpu_adapter;
  u8 gpu_resolution_scale = 1;
  u8 gpu_multisamples = 1;
  bool gpu_use_thread : 1 = true;
  bool gpu_use_software_renderer_for_readbacks : 1 = false;
  bool gpu_threaded_presentation : 1 = DEFAULT_THREADED_PRESENTATION;
  bool gpu_use_debug_device : 1 = false;
  bool gpu_disable_shader_cache : 1 = false;
  bool gpu_disable_dual_source_blend : 1 = false;
  bool gpu_disable_framebuffer_fetch : 1 = false;
  bool gpu_disable_texture_buffers : 1 = false;
  bool gpu_disable_texture_copy_to_self : 1 = false;
  bool gpu_disable_memory_import : 1 = false;
  bool gpu_disable_raster_order_views : 1 = false;
  bool gpu_per_sample_shading : 1 = false;
  bool gpu_true_color : 1 = false;
  bool gpu_debanding : 1 = false;
  bool gpu_scaled_dithering : 1 = true;
  bool gpu_force_round_texcoords : 1 = false;
  bool gpu_accurate_blending : 1 = false;
  bool gpu_disable_interlacing : 1 = false;
  bool gpu_force_ntsc_timings : 1 = false;
  bool gpu_widescreen_hack : 1 = false;
  bool gpu_pgxp_enable : 1 = false;
  bool gpu_pgxp_culling : 1 = true;
  bool gpu_pgxp_texture_correction : 1 = true;
  bool gpu_pgxp_color_correction : 1 = false;
  bool gpu_pgxp_vertex_cache : 1 = false;
  bool gpu_pgxp_cpu : 1 = false;
  bool gpu_pgxp_preserve_proj_fp : 1 = false;
  bool gpu_pgxp_depth_buffer : 1 = false;
  bool gpu_pgxp_disable_2d : 1 = false;
  GPUTextureFilter gpu_texture_filter = DEFAULT_GPU_TEXTURE_FILTER;
  GPUTextureFilter gpu_sprite_texture_filter = DEFAULT_GPU_TEXTURE_FILTER;
  GPULineDetectMode gpu_line_detect_mode = DEFAULT_GPU_LINE_DETECT_MODE;
  GPUDownsampleMode gpu_downsample_mode = DEFAULT_GPU_DOWNSAMPLE_MODE;
  u8 gpu_downsample_scale = 1;
  GPUWireframeMode gpu_wireframe_mode = DEFAULT_GPU_WIREFRAME_MODE;
  DisplayDeinterlacingMode display_deinterlacing_mode = DEFAULT_DISPLAY_DEINTERLACING_MODE;
  DisplayCropMode display_crop_mode = DEFAULT_DISPLAY_CROP_MODE;
  DisplayAspectRatio display_aspect_ratio = DEFAULT_DISPLAY_ASPECT_RATIO;
  DisplayAlignment display_alignment = DEFAULT_DISPLAY_ALIGNMENT;
  DisplayRotation display_rotation = DEFAULT_DISPLAY_ROTATION;
  DisplayScalingMode display_scaling = DEFAULT_DISPLAY_SCALING;
  DisplayExclusiveFullscreenControl display_exclusive_fullscreen_control = DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL;
  DisplayScreenshotMode display_screenshot_mode = DEFAULT_DISPLAY_SCREENSHOT_MODE;
  DisplayScreenshotFormat display_screenshot_format = DEFAULT_DISPLAY_SCREENSHOT_FORMAT;
  u8 display_screenshot_quality = DEFAULT_DISPLAY_SCREENSHOT_QUALITY;
  u16 display_aspect_ratio_custom_numerator = 0;
  u16 display_aspect_ratio_custom_denominator = 0;
  std::string display_bezel_path;
  s16 display_active_start_offset = 0;
  s16 display_active_end_offset = 0;
  s8 display_line_start_offset = 0;
  s8 display_line_end_offset = 0;
  u8 display_arcade_monitor_crop_top = 0;
  u8 display_arcade_monitor_crop_bottom = 0;
  s8 display_arcade_monitor_offset_x = 0;
  bool display_optimal_frame_pacing : 1 = false;
  bool display_pre_frame_sleep : 1 = false;
  bool display_skip_presenting_duplicate_frames : 1 = false;
  bool display_vsync : 1 = false;
  bool display_disable_mailbox_presentation : 1 = false;
  bool display_force_4_3_for_24bit : 1 = false;
  bool display_bezel_enabled : 1 = false;
  bool display_24bit_chroma_smoothing : 1 = false;
  bool display_show_osd_messages : 1 = true;
  bool display_show_fps : 1 = false;
  bool display_show_speed : 1 = false;
  bool display_show_gpu_stats : 1 = false;
  bool display_show_resolution : 1 = false;
  bool display_show_latency_stats : 1 = false;
  bool display_show_cpu_usage : 1 = false;
  bool display_show_gpu_usage : 1 = false;
  bool display_show_frame_times : 1 = false;
  bool display_show_status_indicators : 1 = true;
  bool display_show_inputs : 1 = false;
  bool display_show_enhancements : 1 = false;
  bool display_stretch_vertically : 1 = false;
  float display_pre_frame_sleep_buffer = DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER;
  float display_osd_scale = 100.0f;
  float gpu_pgxp_tolerance = -1.0f;
  float gpu_pgxp_depth_clear_threshold = DEFAULT_GPU_PGXP_DEPTH_THRESHOLD / GPU_PGXP_DEPTH_THRESHOLD_SCALE;

  SaveStateCompressionMode save_state_compression = DEFAULT_SAVE_STATE_COMPRESSION_MODE;

  u8 cdrom_readahead_sectors = DEFAULT_CDROM_READAHEAD_SECTORS;
  CDROMMechaconVersion cdrom_mechacon_version = DEFAULT_CDROM_MECHACON_VERSION;
  bool cdrom_region_check : 1 = false;
  bool cdrom_load_image_to_ram : 1 = false;
  bool cdrom_load_image_patches : 1 = false;
  bool cdrom_mute_cd_audio : 1 = false;
  u32 cdrom_read_speedup = 1;
  u32 cdrom_seek_speedup = 1;

  std::string audio_driver;
  std::string audio_output_device;
  u32 audio_output_volume = 100;
  u32 audio_fast_forward_volume = 100;
  s32 audio_arcade_gain_adjustment_db = 0;
  AudioStreamParameters audio_stream_parameters;
  AudioBackend audio_backend = AudioStream::DEFAULT_BACKEND;
  bool audio_output_muted : 1 = false;

  // Arcade machine configuration.
  bool arcade_crypt_killer_stereo : 1 = true;
  bool arcade_crypt_killer_endless_stages : 1 = false;
  bool arcade_crypt_killer_mirror : 1 = false;
  bool arcade_crypt_killer_woofer : 1 = false;
  bool arcade_crypt_killer_three_players : 1 = true;
  bool arcade_crypt_killer_common_coin_mechanism : 1 = true;

  // Namco System 11 physical DIP SW2 bank.
  bool arcade_namco_system11_dip_test : 1 = false;
  bool arcade_namco_system11_dip_freeze : 1 = false;

  // Common Sony ZN motherboard S551 settings used outside the Video System profile.
  bool arcade_sony_zn_bios_service_mode : 1 = false;
  bool arcade_sony_zn_game_test_mode : 1 = false;

  // Video System ZN-1 / COH-1002V motherboard S551 DIP bank.
  bool arcade_video_system_zn1_bios_service_mode : 1 = false;
  bool arcade_video_system_zn1_test_mode : 1 = false;
  bool arcade_video_system_zn1_save : 1 = true;

  // Bust-A-Move 2 COH-1002E S551 profile.
  u8 arcade_bust_a_move_2_region = 3;

  bool use_old_mdec_routines : 1 = false;
  bool export_shared_memory : 1 = false;

  // timing hacks section
  TickCount dma_max_slice_ticks = DEFAULT_DMA_MAX_SLICE_TICKS;
  TickCount dma_halt_ticks = DEFAULT_DMA_HALT_TICKS;
  u32 gpu_fifo_size = DEFAULT_GPU_FIFO_SIZE;
  TickCount gpu_max_run_ahead = DEFAULT_GPU_MAX_RUN_AHEAD;

  // achievements
  bool achievements_enabled : 1 = false;
  bool achievements_hardcore_mode : 1 = false;
  bool achievements_notifications : 1 = true;
  bool achievements_leaderboard_notifications : 1 = true;
  bool achievements_sound_effects : 1 = true;
  bool achievements_overlays : 1 = true;
  bool achievements_encore_mode : 1 = false;
  bool achievements_spectator_mode : 1 = false;
  bool achievements_unofficial_test_mode : 1 = false;
  bool achievements_use_raintegration : 1 = false;
  s32 achievements_notification_duration = DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME;
  s32 achievements_leaderboard_duration = DEFAULT_LEADERBOARD_NOTIFICATION_TIME;

  struct DebugSettings
  {
    bool show_vram : 1 = false;
    bool dump_cpu_to_vram_copies : 1 = false;
    bool dump_vram_to_cpu_copies : 1 = false;

    bool enable_gdb_server : 1 = false;
    u16 gdb_server_port = 1234;

    // Mutable because the imgui window can close itself.
    mutable bool show_gpu_state = false;
    mutable bool show_cdrom_state = false;
    mutable bool show_spu_state = false;
    mutable bool show_timers_state = false;
    mutable bool show_mdec_state = false;
    mutable bool show_dma_state = false;
    mutable bool show_arcade_outputs = false;
  } debugging;

  // texture replacements
  struct TextureReplacementSettings
  {
    bool enable_vram_write_replacements : 1 = false;
    bool preload_textures : 1 = false;

    bool dump_vram_writes : 1 = false;
    bool dump_vram_write_force_alpha_channel : 1 = true;
    u32 dump_vram_write_width_threshold = 128;
    u32 dump_vram_write_height_threshold = 128;

    ALWAYS_INLINE bool AnyReplacementsEnabled() const { return enable_vram_write_replacements; }

    ALWAYS_INLINE bool ShouldDumpVRAMWrite(u32 width, u32 height)
    {
      return dump_vram_writes && width >= dump_vram_write_width_threshold && height >= dump_vram_write_height_threshold;
    }
  } texture_replacements;

  bool bios_tty_logging : 1 = false;
  bool bios_patch_fast_boot : 1 = DEFAULT_FAST_BOOT_VALUE;
  bool enable_8mb_ram : 1 = false;


  LOGLEVEL log_level = DEFAULT_LOG_LEVEL;
  std::string log_filter;
  bool log_timestamps : 1 = true;
  bool enable_debug_logging : 1 = false;
  bool log_to_console : 1 = DEFAULT_LOG_TO_CONSOLE;
  bool log_to_debug : 1 = false;
  bool log_to_window : 1 = false;
  bool log_to_file : 1 = false;

  ALWAYS_INLINE bool IsUsingSoftwareRenderer() const { return (gpu_renderer == GPURenderer::Software); }
  ALWAYS_INLINE bool IsUsingAccurateBlending() const { return (gpu_accurate_blending && !gpu_true_color); }
  ALWAYS_INLINE bool IsRunaheadEnabled() const { return (runahead_frames > 0); }

  ALWAYS_INLINE PGXPMode GetPGXPMode()
  {
    return gpu_pgxp_enable ? (gpu_pgxp_cpu ? PGXPMode::CPU : PGXPMode::Memory) : PGXPMode::Disabled;
  }

  ALWAYS_INLINE bool UsingPGXPDepthBuffer() const { return gpu_pgxp_enable && gpu_pgxp_depth_buffer; }
  ALWAYS_INLINE bool UsingPGXPCPUMode() const { return gpu_pgxp_enable && gpu_pgxp_cpu; }
  ALWAYS_INLINE float GetPGXPDepthClearThreshold() const
  {
    return gpu_pgxp_depth_clear_threshold * GPU_PGXP_DEPTH_THRESHOLD_SCALE;
  }
  ALWAYS_INLINE void SetPGXPDepthClearThreshold(float value)
  {
    gpu_pgxp_depth_clear_threshold = value / GPU_PGXP_DEPTH_THRESHOLD_SCALE;
  }

  ALWAYS_INLINE s32 GetAudioOutputVolume(bool fast_forwarding) const
  {
    return audio_output_muted ? 0 : (fast_forwarding ? audio_fast_forward_volume : audio_output_volume);
  }

  float GetDisplayAspectRatioValue() const;

  static void CPUOverclockPercentToFraction(u32 percent, u32* numerator, u32* denominator);
  static u32 CPUOverclockFractionToPercent(u32 numerator, u32 denominator);

  void SetCPUOverclockPercent(u32 percent);
  u32 GetCPUOverclockPercent() const;
  void UpdateOverclockActive();

  enum : u32
  {
    DEFAULT_DMA_MAX_SLICE_TICKS = 1000,
    DEFAULT_DMA_HALT_TICKS = 100,
    DEFAULT_GPU_FIFO_SIZE = 16,
    DEFAULT_GPU_MAX_RUN_AHEAD = 128,
    DEFAULT_VRAM_WRITE_DUMP_WIDTH_THRESHOLD = 128,
    DEFAULT_VRAM_WRITE_DUMP_HEIGHT_THRESHOLD = 128,
  };

  void Load(SettingsInterface& si, SettingsInterface& controller_si);
  void Save(SettingsInterface& si, bool ignore_base) const;
  static void Clear(SettingsInterface& si);

  void FixIncompatibleSettings(bool display_osd_messages);

  /// Initializes configuration.
  void UpdateLogSettings();

  static void SetDefaultControllerConfig(SettingsInterface& si);
  static void RemoveStandardControllerConfig(SettingsInterface& si);
  static void SetDefaultHotkeyConfig(SettingsInterface& si);

  static std::optional<LOGLEVEL> ParseLogLevelName(const char* str);
  static const char* GetLogLevelName(LOGLEVEL level);
  static const char* GetLogLevelDisplayName(LOGLEVEL level);
  static std::span<const char*> GetLogFilters();

  static std::optional<ConsoleRegion> ParseConsoleRegionName(const char* str);
  static const char* GetConsoleRegionName(ConsoleRegion region);

  static const char* GetDiscRegionName(DiscRegion region);

  static std::optional<CPUExecutionMode> ParseCPUExecutionMode(const char* str);
  static const char* GetCPUExecutionModeName(CPUExecutionMode mode);
  static const char* GetCPUExecutionModeDisplayName(CPUExecutionMode mode);

  static std::optional<CPUFastmemMode> ParseCPUFastmemMode(const char* str);
  static const char* GetCPUFastmemModeName(CPUFastmemMode mode);
  static const char* GetCPUFastmemModeDisplayName(CPUFastmemMode mode);

  static std::optional<GPURenderer> ParseRendererName(const char* str);
  static const char* GetRendererName(GPURenderer renderer);
  static const char* GetRendererDisplayName(GPURenderer renderer);
  static RenderAPI GetRenderAPIForRenderer(GPURenderer renderer);
  static GPURenderer GetRendererForRenderAPI(RenderAPI api);
  static GPURenderer GetAutomaticRenderer();

  static std::optional<GPUTextureFilter> ParseTextureFilterName(const char* str);
  static const char* GetTextureFilterName(GPUTextureFilter filter);
  static const char* GetTextureFilterDisplayName(GPUTextureFilter filter);

  static std::optional<GPULineDetectMode> ParseLineDetectModeName(const char* str);
  static const char* GetLineDetectModeName(GPULineDetectMode filter);
  static const char* GetLineDetectModeDisplayName(GPULineDetectMode filter);

  static std::optional<GPUDownsampleMode> ParseDownsampleModeName(const char* str);
  static const char* GetDownsampleModeName(GPUDownsampleMode mode);
  static const char* GetDownsampleModeDisplayName(GPUDownsampleMode mode);

  static std::optional<GPUWireframeMode> ParseGPUWireframeMode(const char* str);
  static const char* GetGPUWireframeModeName(GPUWireframeMode mode);
  static const char* GetGPUWireframeModeDisplayName(GPUWireframeMode mode);

  static std::optional<DisplayDeinterlacingMode> ParseDisplayDeinterlacingMode(const char* str);
  static const char* GetDisplayDeinterlacingModeName(DisplayDeinterlacingMode mode);
  static const char* GetDisplayDeinterlacingModeDisplayName(DisplayDeinterlacingMode mode);

  static std::optional<DisplayCropMode> ParseDisplayCropMode(const char* str);
  static const char* GetDisplayCropModeName(DisplayCropMode crop_mode);
  static const char* GetDisplayCropModeDisplayName(DisplayCropMode crop_mode);

  static std::optional<DisplayAspectRatio> ParseDisplayAspectRatio(const char* str);
  static const char* GetDisplayAspectRatioName(DisplayAspectRatio ar);
  static const char* GetDisplayAspectRatioDisplayName(DisplayAspectRatio ar);

  static std::optional<DisplayAlignment> ParseDisplayAlignment(const char* str);
  static const char* GetDisplayAlignmentName(DisplayAlignment alignment);
  static const char* GetDisplayAlignmentDisplayName(DisplayAlignment alignment);

  static std::optional<DisplayRotation> ParseDisplayRotation(const char* str);
  static const char* GetDisplayRotationName(DisplayRotation alignment);
  static const char* GetDisplayRotationDisplayName(DisplayRotation alignment);

  static std::optional<DisplayScalingMode> ParseDisplayScaling(const char* str);
  static const char* GetDisplayScalingName(DisplayScalingMode mode);
  static const char* GetDisplayScalingDisplayName(DisplayScalingMode mode);

  static std::optional<DisplayExclusiveFullscreenControl> ParseDisplayExclusiveFullscreenControl(const char* str);
  static const char* GetDisplayExclusiveFullscreenControlName(DisplayExclusiveFullscreenControl mode);
  static const char* GetDisplayExclusiveFullscreenControlDisplayName(DisplayExclusiveFullscreenControl mode);

  static std::optional<DisplayScreenshotMode> ParseDisplayScreenshotMode(const char* str);
  static const char* GetDisplayScreenshotModeName(DisplayScreenshotMode mode);
  static const char* GetDisplayScreenshotModeDisplayName(DisplayScreenshotMode mode);

  static std::optional<DisplayScreenshotFormat> ParseDisplayScreenshotFormat(const char* str);
  static const char* GetDisplayScreenshotFormatName(DisplayScreenshotFormat mode);
  static const char* GetDisplayScreenshotFormatDisplayName(DisplayScreenshotFormat mode);
  static const char* GetDisplayScreenshotFormatExtension(DisplayScreenshotFormat mode);

  static std::optional<CDROMMechaconVersion> ParseCDROMMechVersionName(const char* str);
  static const char* GetCDROMMechVersionName(CDROMMechaconVersion mode);
  static const char* GetCDROMMechVersionDisplayName(CDROMMechaconVersion mode);

  static std::optional<SaveStateCompressionMode> ParseSaveStateCompressionModeName(const char* str);
  static const char* GetSaveStateCompressionModeName(SaveStateCompressionMode mode);
  static const char* GetSaveStateCompressionModeDisplayName(SaveStateCompressionMode mode);

  static constexpr GPURenderer DEFAULT_GPU_RENDERER = GPURenderer::Automatic;
  static constexpr GPUTextureFilter DEFAULT_GPU_TEXTURE_FILTER = GPUTextureFilter::Nearest;
  static constexpr GPULineDetectMode DEFAULT_GPU_LINE_DETECT_MODE = GPULineDetectMode::Disabled;
  static constexpr GPUDownsampleMode DEFAULT_GPU_DOWNSAMPLE_MODE = GPUDownsampleMode::Disabled;
  static constexpr GPUWireframeMode DEFAULT_GPU_WIREFRAME_MODE = GPUWireframeMode::Disabled;
  static constexpr ConsoleRegion DEFAULT_CONSOLE_REGION = ConsoleRegion::Auto;
  static constexpr float DEFAULT_GPU_PGXP_DEPTH_THRESHOLD = 300.0f;
  static constexpr float GPU_PGXP_DEPTH_THRESHOLD_SCALE = 4096.0f;

  // Prefer oldrec over newrec for now. Except on RISC-V, where there is no oldrec.
#if defined(CPU_ARCH_RISCV64)
  static constexpr CPUExecutionMode DEFAULT_CPU_EXECUTION_MODE = CPUExecutionMode::NewRec;
#else
  static constexpr CPUExecutionMode DEFAULT_CPU_EXECUTION_MODE = CPUExecutionMode::Recompiler;
#endif

  // LUT still ends up faster on Apple Silicon for now, because of 16K pages.
#if defined(ENABLE_MMAP_FASTMEM) && (!defined(__APPLE__) || !defined(__aarch64__))
  static constexpr CPUFastmemMode DEFAULT_CPU_FASTMEM_MODE = CPUFastmemMode::MMap;
#else
  static constexpr CPUFastmemMode DEFAULT_CPU_FASTMEM_MODE = CPUFastmemMode::LUT;
#endif

  static constexpr DisplayDeinterlacingMode DEFAULT_DISPLAY_DEINTERLACING_MODE = DisplayDeinterlacingMode::Disabled;
  static constexpr DisplayCropMode DEFAULT_DISPLAY_CROP_MODE = DisplayCropMode::Overscan;
  static constexpr DisplayAspectRatio DEFAULT_DISPLAY_ASPECT_RATIO = DisplayAspectRatio::Auto;
  static constexpr DisplayAlignment DEFAULT_DISPLAY_ALIGNMENT = DisplayAlignment::Center;
  static constexpr DisplayRotation DEFAULT_DISPLAY_ROTATION = DisplayRotation::Normal;
  static constexpr DisplayScalingMode DEFAULT_DISPLAY_SCALING = DisplayScalingMode::BilinearSmooth;
  static constexpr DisplayExclusiveFullscreenControl DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL =
    DisplayExclusiveFullscreenControl::Automatic;
  static constexpr DisplayScreenshotMode DEFAULT_DISPLAY_SCREENSHOT_MODE = DisplayScreenshotMode::ScreenResolution;
  static constexpr DisplayScreenshotFormat DEFAULT_DISPLAY_SCREENSHOT_FORMAT = DisplayScreenshotFormat::PNG;
  static constexpr u8 DEFAULT_DISPLAY_SCREENSHOT_QUALITY = 85;
  static constexpr float DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER = 2.0f;
  static constexpr float DEFAULT_OSD_SCALE = 100.0f;

  static constexpr u8 DEFAULT_CDROM_READAHEAD_SECTORS = 8;
  static constexpr CDROMMechaconVersion DEFAULT_CDROM_MECHACON_VERSION = CDROMMechaconVersion::VC1A;

  static constexpr s32 DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME = 5;
  static constexpr s32 DEFAULT_LEADERBOARD_NOTIFICATION_TIME = 10;

  static constexpr LOGLEVEL DEFAULT_LOG_LEVEL = LOGLEVEL_INFO;

  static constexpr SaveStateCompressionMode DEFAULT_SAVE_STATE_COMPRESSION_MODE = SaveStateCompressionMode::ZstDefault;

  static const MediaCaptureBackend DEFAULT_MEDIA_CAPTURE_BACKEND;
  static constexpr const char* DEFAULT_MEDIA_CAPTURE_CONTAINER = "mp4";
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH = 640;
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT = 480;
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE = 6000;
  static constexpr u32 DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE = 128;

  // Enable console logging by default on Linux platforms.
#if defined(__linux__)
  static constexpr bool DEFAULT_LOG_TO_CONSOLE = true;
#else
  static constexpr bool DEFAULT_LOG_TO_CONSOLE = false;
#endif

  static constexpr bool DEFAULT_SAVE_STATE_BACKUPS = true;
  static constexpr bool DEFAULT_FAST_BOOT_VALUE = false;
  static constexpr bool DEFAULT_THREADED_PRESENTATION = false;


};

extern Settings g_settings;

namespace EmuFolders {
extern std::string AppRoot;
extern std::string DataRoot;
extern std::string Bios;
extern std::string Cache;
extern std::string Cheats;
extern std::string Covers;
extern std::string Bezels;
extern std::string Crosshairs;
extern std::string Dumps;
extern std::string GameIcons;
extern std::string GameSettings;
extern std::string InputProfiles;
extern std::string Resources;
extern std::string SaveStates;
extern std::string Screenshots;
extern std::string Shaders;
extern std::string Textures;
extern std::string UserResources;
extern std::string Videos;

// Assumes that AppRoot and DataRoot have been initialized.
void SetDefaults();
bool EnsureFoldersExist();
void LoadConfig(SettingsInterface& si);
void Save(SettingsInterface& si);

/// Updates the variables in the EmuFolders namespace, reloading subsystems if needed.
void Update();

/// Returns the path to a resource file, allowing the user to override it.
std::string GetOverridableResourcePath(std::string_view name);
} // namespace EmuFolders
