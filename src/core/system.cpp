// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "system.h"
#include "achievements.h"
#include "bios.h"
#include "bus.h"
#include "cdrom.h"
#include "cheats.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_pgxp.h"
#include "dma.h"
#include "fullscreen_ui.h"
#include "game_database.h"
#include "game_list.h"
#include "gpu.h"
#include "gte.h"
#include "host.h"
#include "host_interface_progress_callback.h"
#include "imgui_overlays.h"
#include "interrupt_controller.h"
#include "core/arcade/arcade_database.h"
#include "core/arcade/arcade_machine_handler.h"
#include "core/arcade/arcade_output.h"
#include "core/arcade/systems/konami/konami.h"
#include "core/arcade/systems/namco/system11/namco_system11.h"
#include "core/arcade/systems/sony/zn/sony_zn.h"
#include "legacy_pad_state.h"
#include "mdec.h"
#include "pcdrv.h"
#include "save_state_version.h"
#include "sio.h"
#include "spu.h"
#include "texture_replacements.h"
#include "timers.h"

#include "scmversion/arcadeduck_version.h"
#include "scmversion/scmversion.h"

#include "util/audio_stream.h"
#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/iso_reader.h"
#include "util/media_capture.h"
#include "util/platform_misc.h"
#include "util/postprocessing.h"
#include "util/sockets.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/binary_reader_writer.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/layered_settings_interface.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/threading.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"

#include "cpuinfo.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "imgui.h"
#include "xxhash.h"

#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <thread>
#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

Log_SetChannel(System);

#ifdef _WIN32
#include "common/windows_headers.h"
#include <Objbase.h>
#include <mmsystem.h>
#include <objbase.h>
#endif

#ifndef __ANDROID__
#define ENABLE_DISCORD_PRESENCE 1
#define ENABLE_PINE_SERVER 1
#define ENABLE_GDB_SERVER 1
#define ENABLE_SOCKET_MULTIPLEXER 1
#include "gdb_server.h"
#include "pine_server.h"
#endif

// #define PROFILE_MEMORY_SAVE_STATES 1

SystemBootParameters::SystemBootParameters() = default;

SystemBootParameters::SystemBootParameters(const SystemBootParameters&) = default;

SystemBootParameters::SystemBootParameters(SystemBootParameters&& other) = default;

SystemBootParameters::SystemBootParameters(std::string filename_) : filename(std::move(filename_))
{
}

SystemBootParameters::~SystemBootParameters() = default;

namespace System {
/// Memory save states - only for internal use.
namespace {
struct SaveStateBuffer
{
  std::string serial;
  std::string title;
  std::string media_path;
  u32 media_subimage_index;
  u32 version;
  RGBA8Image screenshot;
  DynamicHeapArray<u8> state_data;
  size_t state_size;
};
struct MemorySaveState
{
  std::unique_ptr<GPUTexture> vram_texture;
  DynamicHeapArray<u8> state_data;
#ifdef PROFILE_MEMORY_SAVE_STATES
  size_t state_size;
#endif
};
} // namespace

static void CheckCacheLineSize();
static void LogStartupInformation();

static LayeredSettingsInterface GetControllerSettingsLayers(std::unique_lock<std::mutex>& lock);
static LayeredSettingsInterface GetHotkeySettingsLayer(std::unique_lock<std::mutex>& lock);

static std::string GetExecutableNameForImage(IsoReader& iso, bool strip_subdirectories);
static bool ReadExecutableFromImage(IsoReader& iso, std::string* out_executable_name,
                                    std::vector<u8>* out_executable_data);
static GameHash GetGameHashFromBuffer(std::string_view exe_name, std::span<const u8> exe_buffer,
                                      const IsoReader::ISOPrimaryVolumeDescriptor& iso_pvd, u32 track_1_length);

/// Checks for settings changes, std::move() the old settings away for comparing beforehand.
static void CheckForSettingsChanges(const Settings& old_settings);
static void WarnAboutUnsafeSettings();
static void LogUnsafeSettingsToConsole(const SmallStringBase& messages);

static bool Initialize(bool force_software_renderer, u32 ram_size, Error* error);
static bool LoadBIOS(Error* error);
static bool SetBootMode(BootMode new_boot_mode, Error* error);
static void InternalReset();
static void ClearRunningGame();
static void DestroySystem();

static bool CreateGPU(GPURenderer renderer, bool is_switching, Error* error);
static bool RecreateGPU(GPURenderer renderer, bool force_recreate_device = false, bool update_display = true);

/// Updates the throttle period, call when target emulation speed changes.
static void UpdateThrottlePeriod();
static void ResetThrottler();

/// Throttles the system, i.e. sleeps until it's time to execute the next frame.
static void Throttle(Common::Timer::Value current_time);
static void UpdatePerformanceCounters();
static void AccumulatePreFrameSleepTime();
static void UpdatePreFrameSleepTime();
static void UpdateDisplayVSync();
static void ResetPerformanceCounters();

static bool UpdateGameSettingsLayer();
static void UpdateRunningGame(const std::string_view path, CDImage* image, bool booting);
static bool CheckForSBIFile(CDImage* image, Error* error);

/// Returns the maximum size of a save state, considering the current configuration.
static size_t GetMaxSaveStateSize();

static std::string GetMediaPathFromSaveState(const char* path);
static bool SaveUndoLoadState();
static void UpdateMemorySaveStateSettings();
static bool LoadRewindState(u32 skip_saves = 0, bool consume_state = true);
static bool SaveMemoryState(MemorySaveState* mss);
static bool LoadMemoryState(const MemorySaveState& mss);
static bool LoadStateFromBuffer(const SaveStateBuffer& buffer, Error* error, bool update_display);
static bool LoadStateBufferFromFile(SaveStateBuffer* buffer, std::FILE* fp, Error* error, bool read_title,
                                    bool read_media_path, bool read_screenshot, bool read_data);
static bool ReadAndDecompressStateData(std::FILE* fp, std::span<u8> dst, u32 file_offset, u32 compressed_size,
                                       SAVE_STATE_HEADER::CompressionType method, Error* error);
static bool SaveStateToBuffer(SaveStateBuffer* buffer, Error* error, u32 screenshot_size = 256);
static bool CheckSaveStateSupport(Error* error);
static bool SaveStateBufferToFile(const SaveStateBuffer& buffer, std::FILE* fp, Error* error,
                                  SaveStateCompressionMode compression_mode);
static u32 CompressAndWriteStateData(std::FILE* fp, std::span<const u8> src, SaveStateCompressionMode method,
                                     u32* header_type, Error* error);
static bool DoState(StateWrapper& sw, GPUTexture** host_texture, bool update_display, bool is_memory_state);

static bool IsExecutionInterrupted();
static void CheckForAndExitExecution();

static void SetRewinding(bool enabled);
static bool SaveRewindState();
static void DoRewind();

static void SaveRunaheadState();
static bool DoRunahead();

static void UpdateSessionTime(const std::string& prev_serial);

static void SetTimerResolutionIncreased(bool enabled);

#ifdef ENABLE_DISCORD_PRESENCE
static void InitializeDiscordPresence();
static void ShutdownDiscordPresence();
static void PollDiscordPresence();
#endif
} // namespace System

static constexpr const float PERFORMANCE_COUNTER_UPDATE_INTERVAL = 1.0f;
static constexpr const char FALLBACK_EXE_NAME[] = "PSX.EXE";
static constexpr const char SAVE_STATE_UNSUPPORTED_MESSAGE[] =
  "Save states are not yet implemented for this arcade system and are currently under investigation.";
static constexpr u32 MAX_SKIPPED_DUPLICATE_FRAME_COUNT = 2; // 20fps minimum
static constexpr u32 MAX_SKIPPED_TIMEOUT_FRAME_COUNT = 1;   // 30fps minimum

static std::unique_ptr<INISettingsInterface> s_game_settings_interface;
static std::unique_ptr<INISettingsInterface> s_input_settings_interface;
static std::string s_input_profile_name;

static System::State s_state = System::State::Shutdown;
static std::atomic_bool s_startup_cancelled{false};
static bool s_keep_gpu_device_on_shutdown = false;

static ConsoleRegion s_region = ConsoleRegion::NTSC_U;
static Arcade::VideoTimingStandard s_active_video_timing = Arcade::VideoTimingStandard::Invalid;
static bool s_active_video_timing_is_arcade = false;
TickCount System::g_ticks_per_second = System::MASTER_CLOCK;
static TickCount s_max_slice_ticks = System::MASTER_CLOCK / 10;
static u32 s_frame_number = 1;
static u32 s_internal_frame_number = 1;
static const BIOS::ImageInfo* s_bios_image_info = nullptr;
static BIOS::ImageInfo::Hash s_bios_hash = {};

static std::string s_running_game_path;
static std::string s_running_game_serial;
static std::string s_running_game_title;
static const GameDatabase::Entry* s_running_game_entry = nullptr;
static System::GameHash s_running_game_hash;
static System::BootMode s_boot_mode = System::BootMode::None;
static bool s_running_game_custom_title = false;
static bool s_save_states_supported = true;

static bool s_system_executing = false;
static bool s_system_interrupted = false;
static bool s_frame_step_request = false;
static bool s_fast_forward_enabled = false;
static bool s_turbo_enabled = false;
static bool s_throttler_enabled = false;
static bool s_optimal_frame_pacing = false;
static bool s_pre_frame_sleep = false;
static bool s_can_sync_to_host = false;
static bool s_syncing_to_host = false;
static bool s_syncing_to_host_with_vsync = false;
static bool s_skip_presenting_duplicate_frames = false;
static u32 s_skipped_frame_count = 0;
static u32 s_last_presented_internal_frame_number = 0;

static float s_throttle_frequency = 0.0f;
static float s_target_speed = 0.0f;

static Common::Timer::Value s_frame_period = 0;
static Common::Timer::Value s_next_frame_time = 0;

static Common::Timer::Value s_frame_start_time = 0;
static Common::Timer::Value s_last_active_frame_time = 0;
static Common::Timer::Value s_pre_frame_sleep_time = 0;
static Common::Timer::Value s_max_active_frame_time = 0;

static float s_average_frame_time_accumulator = 0.0f;
static float s_minimum_frame_time_accumulator = 0.0f;
static float s_maximum_frame_time_accumulator = 0.0f;

static float s_vps = 0.0f;
static float s_fps = 0.0f;
static float s_speed = 0.0f;
static float s_minimum_frame_time = 0.0f;
static float s_maximum_frame_time = 0.0f;
static float s_average_frame_time = 0.0f;
static float s_cpu_thread_usage = 0.0f;
static float s_cpu_thread_time = 0.0f;
static float s_sw_thread_usage = 0.0f;
static float s_sw_thread_time = 0.0f;
static float s_average_gpu_time = 0.0f;
static float s_accumulated_gpu_time = 0.0f;
static float s_gpu_usage = 0.0f;
static System::FrameTimeHistory s_frame_time_history;
static u32 s_frame_time_history_pos = 0;
static u32 s_last_frame_number = 0;
static u32 s_last_internal_frame_number = 0;
static GlobalTicks s_last_global_tick_counter = 0;
static u64 s_last_cpu_time = 0;
static u64 s_last_sw_time = 0;
static u32 s_presents_since_last_update = 0;
static Common::Timer s_fps_timer;
static Common::Timer s_frame_timer;
static Threading::ThreadHandle s_cpu_thread_handle;

static std::unique_ptr<CheatList> s_cheat_list;
static std::unique_ptr<MediaCapture> s_media_capture;

// temporary save state, created when loading, used to undo load state
static std::optional<System::SaveStateBuffer> s_undo_load_state;

static bool s_memory_saves_enabled = false;

static std::deque<System::MemorySaveState> s_rewind_states;
static s32 s_rewind_load_frequency = -1;
static s32 s_rewind_load_counter = -1;
static s32 s_rewind_save_frequency = -1;
static s32 s_rewind_save_counter = -1;
static bool s_rewinding_first_save = false;

static std::deque<System::MemorySaveState> s_runahead_states;
static bool s_runahead_replay_pending = false;
static u32 s_runahead_frames = 0;
static u32 s_runahead_replay_frames = 0;

// Used to track play time. We use a monotonic timer here, in case of clock changes.
static u64 s_session_start_time = 0;

#ifdef ENABLE_SOCKET_MULTIPLEXER
static std::unique_ptr<SocketMultiplexer> s_socket_multiplexer;
#endif

#ifdef ENABLE_DISCORD_PRESENCE
static bool s_discord_presence_active = false;
static time_t s_discord_presence_time_epoch;
#endif

static struct tm FormatLocalTime(time_t t)
{
  struct tm lt = {};
#ifdef _MSC_VER
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif
  return lt;
}

static TinyString GetTimestampStringForFileName()
{
  return TinyString::from_format("{:%Y-%m-%d-%H-%M-%S}", FormatLocalTime(std::time(nullptr)));
}

bool System::Internal::PerformEarlyHardwareChecks(Error* error)
{
  // This shouldn't fail... if it does, just hope for the best.
  cpuinfo_initialize();

#ifdef CPU_ARCH_X64
  if (!cpuinfo_has_x86_sse4_1())
  {
    Error::SetStringFmt(error, "Your CPU does not support the SSE4.1 instruction set.\n"
                               "A CPU from 2008 or newer is required to run ArcadeDuck.");
    return false;
  }
#endif

  // Check page size. If it doesn't match, it is a fatal error.
  const size_t runtime_host_page_size = PlatformMisc::GetRuntimePageSize();
  if (runtime_host_page_size == 0)
  {
    Error::SetStringFmt(error, "Cannot determine size of page. Continuing with expectation of {} byte pages.",
                        runtime_host_page_size);
  }
  else if (HOST_PAGE_SIZE != runtime_host_page_size)
  {
    Error::SetStringFmt(
      error, "Page size mismatch. This build was compiled with {} byte pages, but the system has {} byte pages.",
      HOST_PAGE_SIZE, runtime_host_page_size);
    CPUThreadShutdown();
    return false;
  }

  return true;
}

void System::CheckCacheLineSize()
{
  u32 max_line_size = 0;
  if (cpuinfo_initialize())
  {
    const u32 num_l1is = cpuinfo_get_l1i_caches_count();
    const u32 num_l1ds = cpuinfo_get_l1d_caches_count();
    const u32 num_l2s = cpuinfo_get_l2_caches_count();
    for (u32 i = 0; i < num_l1is; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l1i_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
    for (u32 i = 0; i < num_l1ds; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l1d_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
    for (u32 i = 0; i < num_l2s; i++)
    {
      const cpuinfo_cache* cache = cpuinfo_get_l2_cache(i);
      if (cache)
        max_line_size = std::max(max_line_size, cache->line_size);
    }
  }

  if (max_line_size == 0)
  {
    ERROR_LOG("Cannot determine size of cache line. Continuing with expectation of {} byte lines.",
              HOST_CACHE_LINE_SIZE);
  }
  else if (HOST_CACHE_LINE_SIZE != max_line_size)
  {
    // Not fatal, but does have performance implications.
    WARNING_LOG(
      "Cache line size mismatch. This build was compiled with {} byte lines, but the system has {} byte lines.",
      HOST_CACHE_LINE_SIZE, max_line_size);
  }
}

void System::LogStartupInformation()
{
  INFO_LOG("{}", g_scm_version_str);
  INFO_LOG("SCM Revision: {} ({}, {})", g_scm_hash_str, g_scm_branch_str, g_scm_tag_str);
  INFO_LOG("SCM Timestamp: {}", g_scm_date_str);
  INFO_LOG("GPL baseline: {}", ARCADEDUCK_GPL_BASELINE_HASH);
  INFO_LOG("Build Timestamp: {} {}", __DATE__, __TIME__);
  if (const cpuinfo_package* package = cpuinfo_get_package(0)) [[likely]]
  {
    INFO_LOG("Host CPU: {}", package->name);
    INFO_LOG("CPU has {} logical processor(s) and {} core(s) across {} cluster(s).", package->processor_count,
             package->core_count, package->cluster_count);
  }
}

bool System::Internal::ProcessStartup(Error* error)
{
  Common::Timer timer;

  // Allocate JIT memory as soon as possible.
  if (!CPU::CodeCache::ProcessStartup(error))
    return false;

  // g_settings is not valid at this point, query global config directly.
  const bool export_shared_memory = Host::GetBoolSettingValue("Hacks", "ExportSharedMemory", false);

  // Fastmem alloc *must* come after JIT alloc, otherwise it tends to eat the 4GB region after the executable on MacOS.
  if (!Bus::AllocateMemory(export_shared_memory, error))
  {
    CPU::CodeCache::ProcessShutdown();
    return false;
  }

  VERBOSE_LOG("Memory allocation took {} ms.", timer.GetTimeMilliseconds());

  CheckCacheLineSize();

  return true;
}

void System::Internal::ProcessShutdown()
{
  Bus::ReleaseMemory();
  CPU::CodeCache::ProcessShutdown();
}

bool System::Internal::CPUThreadInitialize(Error* error)
{
  Threading::SetNameOfCurrentThread("CPU Thread");

#ifdef _WIN32
  // On Win32, we have a bunch of things which use COM (e.g. SDL, Cubeb, etc).
  // We need to initialize COM first, before anything else does, because otherwise they might
  // initialize it in single-threaded/apartment mode, which can't be changed to multithreaded.
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "CoInitializeEx() failed: ", hr);
    return false;
  }
#endif

  // This will call back to Host::LoadSettings() -> ReloadSources().
  LoadSettings(false);

  LogStartupInformation();

  if (g_settings.achievements_enabled)
    Achievements::Initialize();

#ifdef ENABLE_DISCORD_PRESENCE
  if (g_settings.enable_discord_presence)
    InitializeDiscordPresence();
#endif

#ifdef ENABLE_PINE_SERVER
  if (g_settings.pine_enable)
    PINEServer::Initialize(g_settings.pine_slot);
#endif

  return true;
}

void System::Internal::CPUThreadShutdown()
{
#ifdef ENABLE_PINE_SERVER
  PINEServer::Shutdown();
#endif

#ifdef ENABLE_DISCORD_PRESENCE
  ShutdownDiscordPresence();
#endif

  Achievements::Shutdown(false);

  InputManager::CloseSources();

#ifdef _WIN32
  CoUninitialize();
#endif
}

void System::Internal::IdlePollUpdate()
{
  InputManager::PollSources();

#ifdef ENABLE_DISCORD_PRESENCE
  PollDiscordPresence();
#endif

  Achievements::IdleUpdate();

#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (s_socket_multiplexer)
    s_socket_multiplexer->PollEventsWithTimeout(0);
#endif
}

System::State System::GetState()
{
  return s_state;
}

void System::SetState(State new_state)
{
  if (s_state == new_state)
    return;

  Assert(s_state == State::Paused || s_state == State::Running);
  Assert(new_state == State::Paused || new_state == State::Running);
  s_state = new_state;
}

bool System::IsRunning()
{
  return s_state == State::Running;
}

ALWAYS_INLINE bool System::IsExecutionInterrupted()
{
  return s_state != State::Running || s_system_interrupted;
}

ALWAYS_INLINE_RELEASE void System::CheckForAndExitExecution()
{
  if (IsExecutionInterrupted()) [[unlikely]]
  {
    s_system_interrupted = false;

    TimingEvents::CancelRunningEvent();
    CPU::ExitExecution();
  }
}

bool System::IsPaused()
{
  return s_state == State::Paused;
}

bool System::IsShutdown()
{
  return s_state == State::Shutdown;
}

bool System::IsValid()
{
  return s_state == State::Running || s_state == State::Paused;
}

bool System::IsValidOrInitializing()
{
  return s_state == State::Starting || s_state == State::Running || s_state == State::Paused;
}

bool System::IsExecuting()
{
  DebugAssert(s_state != State::Shutdown);
  return s_system_executing;
}

bool System::IsStartupCancelled()
{
  return s_startup_cancelled.load();
}

void System::CancelPendingStartup()
{
  if (s_state == State::Starting)
    s_startup_cancelled.store(true);
}

void System::InterruptExecution()
{
  if (s_system_executing)
    s_system_interrupted = true;
}

ConsoleRegion System::GetRegion()
{
  return s_region;
}

bool System::IsPALVideoTiming()
{
  return s_active_video_timing == Arcade::VideoTimingStandard::PALDerived;
}

bool System::IsArcadeVideoTiming()
{
  return s_active_video_timing_is_arcade;
}

static void ClearActiveVideoTiming()
{
  s_active_video_timing = Arcade::VideoTimingStandard::Invalid;
  s_active_video_timing_is_arcade = false;
}

static void SetActiveVideoTimingForConsoleRegion(ConsoleRegion region)
{
  s_active_video_timing = (region == ConsoleRegion::PAL) ? Arcade::VideoTimingStandard::PALDerived :
                                                          Arcade::VideoTimingStandard::NTSCDerived;
  s_active_video_timing_is_arcade = false;
}

TickCount System::GetMaxSliceTicks()
{
  return s_max_slice_ticks;
}

void System::UpdateOverclock()
{
  // Sony ZN daughterboards which derive time from the global CPU tick counter
  // must settle/capture the old clock domain before g_ticks_per_second changes.
  const bool sony_zn_active = SonyZN::IsActive();
  if (sony_zn_active)
    SonyZN::PrepareForCPUClockChange();

  g_ticks_per_second = ScaleTicksToOverclock(MASTER_CLOCK);
  s_max_slice_ticks = ScaleTicksToOverclock(MASTER_CLOCK / 10);

  if (sony_zn_active)
    SonyZN::CompleteCPUClockChange();

  // CPUClockChanged() retires pending SPU sample ticks using the conversion
  // state under which they were scheduled, then installs the new ratio.
  SPU::CPUClockChanged();
  CDROM::CPUClockChanged();
  g_gpu->CPUClockChanged();
  Timers::CPUClocksChanged();
  UpdateThrottlePeriod();
}

GlobalTicks System::GetGlobalTickCounter()
{
  // When running events, the counter actually goes backwards, because the pending ticks are added in chunks.
  // So, we need to return the counter with all pending ticks added in such cases.
  return TimingEvents::IsRunningEvents() ? TimingEvents::GetEventRunTickCounter() :
                                           (TimingEvents::GetGlobalTickCounter() + CPU::GetPendingTicks());
}

u32 System::GetFrameNumber()
{
  return s_frame_number;
}

u32 System::GetInternalFrameNumber()
{
  return s_internal_frame_number;
}

const std::string& System::GetDiscPath()
{
  return s_running_game_path;
}
const std::string& System::GetGameSerial()
{
  return s_running_game_serial;
}

const std::string& System::GetGameTitle()
{
  return s_running_game_title;
}

const GameDatabase::Entry* System::GetGameDatabaseEntry()
{
  return s_running_game_entry;
}

System::GameHash System::GetGameHash()
{
  return s_running_game_hash;
}

bool System::IsRunningUnknownGame()
{
  return !s_running_game_entry;
}

bool System::SupportsSaveStates()
{
  return s_save_states_supported;
}

const char* System::GetSaveStateUnsupportedMessage()
{
  return SAVE_STATE_UNSUPPORTED_MESSAGE;
}

bool System::CheckSaveStateSupport(Error* error)
{
  if (SupportsSaveStates())
    return true;

  Error::SetStringView(error, SAVE_STATE_UNSUPPORTED_MESSAGE);
  return false;
}

const BIOS::ImageInfo* System::GetBIOSImageInfo()
{
  return s_bios_image_info;
}

float System::GetFPS()
{
  return s_fps;
}
float System::GetVPS()
{
  return s_vps;
}
float System::GetEmulationSpeed()
{
  return s_speed;
}
float System::GetAverageFrameTime()
{
  return s_average_frame_time;
}
float System::GetMinimumFrameTime()
{
  return s_minimum_frame_time;
}
float System::GetMaximumFrameTime()
{
  return s_maximum_frame_time;
}
float System::GetThrottleFrequency()
{
  return s_throttle_frequency;
}
float System::GetCPUThreadUsage()
{
  return s_cpu_thread_usage;
}
float System::GetCPUThreadAverageTime()
{
  return s_cpu_thread_time;
}
float System::GetSWThreadUsage()
{
  return s_sw_thread_usage;
}
float System::GetSWThreadAverageTime()
{
  return s_sw_thread_time;
}
float System::GetGPUUsage()
{
  return s_gpu_usage;
}
float System::GetGPUAverageTime()
{
  return s_average_gpu_time;
}
const System::FrameTimeHistory& System::GetFrameTimeHistory()
{
  return s_frame_time_history;
}
u32 System::GetFrameTimeHistoryPos()
{
  return s_frame_time_history_pos;
}

bool System::IsSaveStateFilename(std::string_view path)
{
  return StringUtil::EndsWithNoCase(path, ".sav");
}

ConsoleRegion System::GetConsoleRegionForDiscRegion(DiscRegion region)
{
  switch (region)
  {
    case DiscRegion::NTSC_J:
      return ConsoleRegion::NTSC_J;

    case DiscRegion::NTSC_U:
    case DiscRegion::Other:
    case DiscRegion::NonPS1:
    default:
      return ConsoleRegion::NTSC_U;

    case DiscRegion::PAL:
      return ConsoleRegion::PAL;
  }
}

std::string System::GetGameHashId(GameHash hash)
{
  return fmt::format("HASH-{:X}", hash);
}

bool System::GetGameDetailsFromImage(CDImage* cdi, std::string* out_id, GameHash* out_hash)
{
  IsoReader iso;
  if (!iso.Open(cdi, 1))
  {
    if (out_id)
      out_id->clear();
    if (out_hash)
      *out_hash = 0;
    return false;
  }

  std::string id;
  std::string exe_name;
  std::vector<u8> exe_buffer;
  if (!ReadExecutableFromImage(iso, &exe_name, &exe_buffer))
  {
    if (out_id)
      out_id->clear();
    if (out_hash)
      *out_hash = 0;
    return false;
  }

  // Always compute the hash.
  const GameHash hash = GetGameHashFromBuffer(exe_name, exe_buffer, iso.GetPVD(), cdi->GetTrackLength(1));
  DEV_LOG("Hash for '{}' - {:016X}", exe_name, hash);

  if (exe_name != FALLBACK_EXE_NAME)
  {
    // Strip off any subdirectories.
    const std::string::size_type slash = exe_name.rfind('\\');
    if (slash != std::string::npos)
      id = std::string_view(exe_name).substr(slash + 1);
    else
      id = exe_name;

    // SCES_123.45 -> SCES-12345
    for (std::string::size_type pos = 0; pos < id.size();)
    {
      if (id[pos] == '.')
      {
        id.erase(pos, 1);
        continue;
      }

      if (id[pos] == '_')
        id[pos] = '-';
      else
        id[pos] = static_cast<char>(std::toupper(id[pos]));

      pos++;
    }
  }

  if (out_id)
  {
    if (id.empty())
      *out_id = GetGameHashId(hash);
    else
      *out_id = std::move(id);
  }

  if (out_hash)
    *out_hash = hash;

  return true;
}

std::string System::GetExecutableNameForImage(IsoReader& iso, bool strip_subdirectories)
{
  // Read SYSTEM.CNF
  std::vector<u8> system_cnf_data;
  if (!iso.ReadFile("SYSTEM.CNF", &system_cnf_data))
    return FALLBACK_EXE_NAME;

  // Parse lines
  std::vector<std::pair<std::string, std::string>> lines;
  std::pair<std::string, std::string> current_line;
  bool reading_value = false;
  for (size_t pos = 0; pos < system_cnf_data.size(); pos++)
  {
    const char ch = static_cast<char>(system_cnf_data[pos]);
    if (ch == '\r' || ch == '\n')
    {
      if (!current_line.first.empty())
      {
        lines.push_back(std::move(current_line));
        current_line = {};
        reading_value = false;
      }
    }
    else if (ch == ' ' || (ch >= 0x09 && ch <= 0x0D))
    {
      continue;
    }
    else if (ch == '=' && !reading_value)
    {
      reading_value = true;
    }
    else
    {
      if (reading_value)
        current_line.second.push_back(ch);
      else
        current_line.first.push_back(ch);
    }
  }

  if (!current_line.first.empty())
    lines.push_back(std::move(current_line));

  // Find the BOOT line
  auto iter = std::find_if(lines.begin(), lines.end(),
                           [](const auto& it) { return StringUtil::Strcasecmp(it.first.c_str(), "boot") == 0; });
  if (iter == lines.end())
  {
    // Fallback to PSX.EXE
    return FALLBACK_EXE_NAME;
  }

  std::string code = iter->second;
  std::string::size_type pos;
  if (strip_subdirectories)
  {
    // cdrom:\SCES_123.45;1
    pos = code.rfind('\\');
    if (pos != std::string::npos)
    {
      code.erase(0, pos + 1);
    }
    else
    {
      // cdrom:SCES_123.45;1
      pos = code.rfind(':');
      if (pos != std::string::npos)
        code.erase(0, pos + 1);
    }
  }
  else
  {
    if (code.compare(0, 6, "cdrom:") == 0)
      code.erase(0, 6);
    else
      WARNING_LOG("Unknown prefix in executable path: '{}'", code);

    // remove leading slashes
    while (code[0] == '/' || code[0] == '\\')
      code.erase(0, 1);
  }

  // strip off ; or version number
  pos = code.rfind(';');
  if (pos != std::string::npos)
    code.erase(pos);

  return code;
}

std::string System::GetExecutableNameForImage(CDImage* cdi, bool strip_subdirectories)
{
  IsoReader iso;
  if (!iso.Open(cdi, 1))
    return {};

  return GetExecutableNameForImage(iso, strip_subdirectories);
}

bool System::ReadExecutableFromImage(CDImage* cdi, std::string* out_executable_name,
                                     std::vector<u8>* out_executable_data)
{
  IsoReader iso;
  if (!iso.Open(cdi, 1))
    return false;

  return ReadExecutableFromImage(iso, out_executable_name, out_executable_data);
}

bool System::ReadExecutableFromImage(IsoReader& iso, std::string* out_executable_name,
                                     std::vector<u8>* out_executable_data)
{
  std::string executable_path = GetExecutableNameForImage(iso, false);
  DEV_LOG("Executable path: '{}'", executable_path);
  if (!executable_path.empty() && out_executable_data)
  {
    if (!iso.ReadFile(executable_path, out_executable_data))
    {
      ERROR_LOG("Failed to read executable '{}' from disc", executable_path);
      return false;
    }
  }

  if (out_executable_name)
    *out_executable_name = std::move(executable_path);

  return true;
}

System::GameHash System::GetGameHashFromBuffer(std::string_view exe_name, std::span<const u8> exe_buffer,
                                               const IsoReader::ISOPrimaryVolumeDescriptor& iso_pvd, u32 track_1_length)
{
  XXH64_state_t* state = XXH64_createState();
  XXH64_reset(state, 0x4242D00C);
  XXH64_update(state, exe_name.data(), exe_name.size());
  XXH64_update(state, exe_buffer.data(), exe_buffer.size());
  XXH64_update(state, &iso_pvd, sizeof(IsoReader::ISOPrimaryVolumeDescriptor));
  XXH64_update(state, &track_1_length, sizeof(track_1_length));
  const GameHash hash = XXH64_digest(state);
  XXH64_freeState(state);
  return hash;
}

DiscRegion System::GetRegionForSerial(const std::string_view serial)
{
  static constexpr const std::pair<const char*, DiscRegion> region_prefixes[] = {
    {"sces", DiscRegion::PAL},    {"sced", DiscRegion::PAL},    {"sles", DiscRegion::PAL},
    {"sled", DiscRegion::PAL},

    {"scps", DiscRegion::NTSC_J}, {"slps", DiscRegion::NTSC_J}, {"slpm", DiscRegion::NTSC_J},
    {"sczs", DiscRegion::NTSC_J}, {"papx", DiscRegion::NTSC_J},

    {"scus", DiscRegion::NTSC_U}, {"slus", DiscRegion::NTSC_U},
  };

  for (const auto& [prefix, region] : region_prefixes)
  {
    if (StringUtil::StartsWithNoCase(serial, prefix))
      return region;
  }

  return DiscRegion::Other;
}

DiscRegion System::GetRegionFromSystemArea(CDImage* cdi)
{
  // The license code is on sector 4 of the disc.
  u8 sector[CDImage::DATA_SECTOR_SIZE];
  if (!cdi->Seek(1, 4) || cdi->Read(CDImage::ReadMode::DataOnly, 1, sector) != 1)
    return DiscRegion::Other;

  static constexpr char ntsc_u_string[] = "          Licensed  by          Sony Computer Entertainment Amer  ica ";
  static constexpr char ntsc_j_string[] = "          Licensed  by          Sony Computer Entertainment Inc.";
  static constexpr char pal_string[] = "          Licensed  by          Sony Computer Entertainment Euro pe";

  // subtract one for the terminating null
  if (std::equal(ntsc_u_string, ntsc_u_string + countof(ntsc_u_string) - 1, sector))
    return DiscRegion::NTSC_U;
  else if (std::equal(ntsc_j_string, ntsc_j_string + countof(ntsc_j_string) - 1, sector))
    return DiscRegion::NTSC_J;
  else if (std::equal(pal_string, pal_string + countof(pal_string) - 1, sector))
    return DiscRegion::PAL;
  else
    return DiscRegion::Other;
}

DiscRegion System::GetRegionForImage(CDImage* cdi)
{
  const DiscRegion system_area_region = GetRegionFromSystemArea(cdi);
  if (system_area_region != DiscRegion::Other)
    return system_area_region;

  IsoReader iso;
  if (!iso.Open(cdi, 1))
    return DiscRegion::NonPS1;

  // The executable must exist, because this just returns PSX.EXE if it doesn't.
  const std::string exename = GetExecutableNameForImage(iso, false);
  if (exename.empty() || !iso.FileExists(exename.c_str()))
    return DiscRegion::NonPS1;

  // Strip off any subdirectories.
  const std::string::size_type slash = exename.rfind('\\');
  if (slash != std::string::npos)
    return GetRegionForSerial(std::string_view(exename).substr(slash + 1));
  else
    return GetRegionForSerial(exename);
}

std::string System::GetGameSettingsPath(std::string_view game_serial)
{
  return Path::Combine(EmuFolders::GameSettings, fmt::format("{}.ini", Path::SanitizeFileName(game_serial)));
}

std::string System::GetInputProfilePath(std::string_view name)
{
  return Path::Combine(EmuFolders::InputProfiles, fmt::format("{}.ini", name));
}

bool System::RecreateGPU(GPURenderer renderer, bool force_recreate_device, bool update_display /* = true*/)
{
  ClearMemorySaveStates();
  g_gpu->RestoreDeviceContext();

  // save current state
  DynamicHeapArray<u8> state_data(GetMaxSaveStateSize());
  {
    StateWrapper sw(state_data.span(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
    if (!g_gpu->DoState(sw, nullptr, false) || !TimingEvents::DoState(sw))
    {
      ERROR_LOG("Failed to save old GPU state when switching renderers");
      state_data.deallocate();
    }
  }

  // create new renderer
  g_gpu.reset();
  if (force_recreate_device)
  {
    PostProcessing::Shutdown();
    Host::ReleaseGPUDevice();
  }

  Error error;
  if (!CreateGPU(renderer, true, &error))
  {
    if (!IsStartupCancelled())
      Host::ReportErrorAsync("Error", error.GetDescription());

    DestroySystem();
    return false;
  }

  if (!state_data.empty())
  {
    StateWrapper sw(state_data.span(), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
    g_gpu->RestoreDeviceContext();
    g_gpu->DoState(sw, nullptr, update_display);
    TimingEvents::DoState(sw);
  }

  // fix up vsync etc
  UpdateSpeedLimiterState();
  return true;
}

void System::LoadSettings(bool display_osd_messages)
{
  std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
  SettingsInterface& si = *Host::GetSettingsInterface();
  LayeredSettingsInterface controller_si = GetControllerSettingsLayers(lock);
  LayeredSettingsInterface hotkey_si = GetHotkeySettingsLayer(lock);
  g_settings.Load(si, controller_si);

  const Arcade::Database::GameDefinition* const arcade_game = Arcade::Database::GetGame(s_running_game_serial);
  if (arcade_game)
  {
    SettingsInterface* const game_settings = Host::Internal::GetGameSettingsLayer();
    if (!game_settings || !game_settings->ContainsValue("Display", "Rotation"))
    {
      if (arcade_game->display.rotation.has_value())
      {
        g_settings.display_rotation = arcade_game->display.rotation.value();
        INFO_LOG("Arcade display rotation game='{}' database='{}'.", arcade_game->id,
                 Settings::GetDisplayRotationName(g_settings.display_rotation));
      }
      else if (arcade_game->display.orientation == Arcade::Database::Orientation::Vertical)
      {
        g_settings.display_rotation = DisplayRotation::Rotate90;
        INFO_LOG("Arcade display orientation game='{}' orientation='vertical' rotation='Rotate90'.", arcade_game->id);
      }
    }

    if (!game_settings || !game_settings->ContainsValue("Display", "ArcadeMonitorCropTop"))
      g_settings.display_arcade_monitor_crop_top = arcade_game->display.monitor_crop_top;
    if (!game_settings || !game_settings->ContainsValue("Display", "ArcadeMonitorCropBottom"))
      g_settings.display_arcade_monitor_crop_bottom = arcade_game->display.monitor_crop_bottom;
    if (!game_settings || !game_settings->ContainsValue("Display", "ArcadeMonitorOffsetX"))
      g_settings.display_arcade_monitor_offset_x = arcade_game->display.monitor_offset_x;

    if (g_settings.display_arcade_monitor_crop_top != 0 || g_settings.display_arcade_monitor_crop_bottom != 0 ||
        g_settings.display_arcade_monitor_offset_x != 0)
    {
      INFO_LOG("Arcade monitor presentation game='{}' crop_top={} crop_bottom={} offset_x={}.", arcade_game->id,
               g_settings.display_arcade_monitor_crop_top, g_settings.display_arcade_monitor_crop_bottom,
               g_settings.display_arcade_monitor_offset_x);
    }
  }

  g_settings.UpdateLogSettings();

  Host::LoadSettings(si, lock);
  InputManager::ReloadSources(controller_si, lock);
  InputManager::ReloadBindings(controller_si, hotkey_si);
  WarnAboutUnsafeSettings();

  if (arcade_game && arcade_game->cpu_clock_percent != 100)
  {
    SettingsInterface* const game_settings = Host::Internal::GetGameSettingsLayer();
    const bool has_game_cpu_clock_override =
      game_settings &&
      (game_settings->ContainsValue("CPU", "OverclockEnable") ||
       game_settings->ContainsValue("CPU", "OverclockNumerator") ||
       game_settings->ContainsValue("CPU", "OverclockDenominator"));
    if (!has_game_cpu_clock_override)
    {
      g_settings.cpu_overclock_enable = true;
      g_settings.SetCPUOverclockPercent(arcade_game->cpu_clock_percent);
      g_settings.UpdateOverclockActive();
      INFO_LOG("Arcade CPU clock game='{}' default={}%.", arcade_game->id, arcade_game->cpu_clock_percent);
    }
  }

  // apply compatibility settings
  if (g_settings.apply_compatibility_settings && !s_running_game_serial.empty())
  {
    const GameDatabase::Entry* entry = GameDatabase::GetEntryForSerial(s_running_game_serial);
    if (entry)
      entry->ApplySettings(g_settings, display_osd_messages);
  }

  g_settings.FixIncompatibleSettings(display_osd_messages);
}

void System::ReloadInputSources()
{
  std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
  LayeredSettingsInterface controller_si = GetControllerSettingsLayers(lock);
  InputManager::ReloadSources(controller_si, lock);

  // skip loading bindings if we're not running, since it'll get done on startup anyway
  if (IsValid())
  {
    LayeredSettingsInterface hotkey_si = GetHotkeySettingsLayer(lock);
    InputManager::ReloadBindings(controller_si, hotkey_si);
  }
}

void System::ReloadInputBindings()
{
  // skip loading bindings if we're not running, since it'll get done on startup anyway
  if (!IsValid())
    return;

  std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
  LayeredSettingsInterface controller_si = GetControllerSettingsLayers(lock);
  LayeredSettingsInterface hotkey_si = GetHotkeySettingsLayer(lock);
  InputManager::ReloadBindings(controller_si, hotkey_si);
}

LayeredSettingsInterface System::GetControllerSettingsLayers(std::unique_lock<std::mutex>& lock)
{
  LayeredSettingsInterface ret;
  ret.SetLayer(LayeredSettingsInterface::Layer::LAYER_BASE, Host::Internal::GetBaseSettingsLayer());

  // Select input profile _or_ game settings, not both.
  if (SettingsInterface* isi = Host::Internal::GetInputSettingsLayer())
  {
    ret.SetLayer(LayeredSettingsInterface::Layer::LAYER_INPUT, isi);
  }
  else if (SettingsInterface* gsi = Host::Internal::GetGameSettingsLayer();
           gsi && gsi->GetBoolValue("ArcadeProfiles", "UseGameSettingsForArcadeInput", false))
  {
    ret.SetLayer(LayeredSettingsInterface::Layer::LAYER_GAME, gsi);
  }

  return ret;
}

LayeredSettingsInterface System::GetHotkeySettingsLayer(std::unique_lock<std::mutex>& lock)
{
  LayeredSettingsInterface ret;
  ret.SetLayer(LayeredSettingsInterface::Layer::LAYER_BASE, Host::Internal::GetBaseSettingsLayer());

  // Only add input profile layer if the option is enabled.
  if (SettingsInterface* isi = Host::Internal::GetInputSettingsLayer();
      isi && isi->GetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", false))
  {
    ret.SetLayer(LayeredSettingsInterface::Layer::LAYER_INPUT, Host::Internal::GetInputSettingsLayer());
  }

  return ret;
}

void System::SetDefaultSettings(SettingsInterface& si)
{
  Settings temp;

  // we don't want to reset some things (e.g. OSD)
  temp.display_show_osd_messages = g_settings.display_show_osd_messages;
  temp.display_show_fps = g_settings.display_show_fps;
  temp.display_show_speed = g_settings.display_show_speed;
  temp.display_show_gpu_stats = g_settings.display_show_gpu_stats;
  temp.display_show_resolution = g_settings.display_show_resolution;
  temp.display_show_cpu_usage = g_settings.display_show_cpu_usage;
  temp.display_show_gpu_usage = g_settings.display_show_gpu_usage;
  temp.display_show_frame_times = g_settings.display_show_frame_times;

  temp.Save(si, false);

#ifndef __ANDROID__
  si.SetStringValue("MediaCapture", "Backend", MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND));
  si.SetStringValue("MediaCapture", "Container", Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
  si.SetBoolValue("MediaCapture", "VideoCapture", true);
  si.SetUIntValue("MediaCapture", "VideoWidth", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH);
  si.SetUIntValue("MediaCapture", "VideoHeight", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT);
  si.SetBoolValue("MediaCapture", "VideoAutoSize", false);
  si.SetUIntValue("MediaCapture", "VideoBitrate", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE);
  si.SetStringValue("MediaCapture", "VideoCodec", "");
  si.SetBoolValue("MediaCapture", "VideoCodecUseArgs", false);
  si.SetStringValue("MediaCapture", "AudioCodecArgs", "");
  si.SetBoolValue("MediaCapture", "AudioCapture", true);
  si.SetUIntValue("MediaCapture", "AudioBitrate", Settings::DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE);
  si.SetStringValue("MediaCapture", "AudioCodec", "");
  si.SetBoolValue("MediaCapture", "AudioCodecUseArgs", false);
  si.SetStringValue("MediaCapture", "AudioCodecArgs", "");
#endif
}

void System::ApplySettings(bool display_osd_messages)
{
  DEV_LOG("Applying settings...");

  const Settings old_config(std::move(g_settings));
  g_settings = Settings();
  LoadSettings(display_osd_messages);

  // If we've disabled/enabled game settings, we need to reload without it.
  if (g_settings.apply_game_settings != old_config.apply_game_settings)
  {
    UpdateGameSettingsLayer();
    LoadSettings(display_osd_messages);
  }

  CheckForSettingsChanges(old_config);
  Host::CheckForSettingsChanges(old_config);

  if (IsValid())
  {
    ResetPerformanceCounters();
    InterruptExecution();
  }
}

bool System::ReloadGameSettings(bool display_osd_messages)
{
  if (!IsValid() || !UpdateGameSettingsLayer())
    return false;

  ApplySettings(display_osd_messages);
  return true;
}

bool System::UpdateGameSettingsLayer()
{
  std::unique_ptr<INISettingsInterface> new_interface;
  if (g_settings.apply_game_settings && !s_running_game_serial.empty())
  {
    std::string filename(GetGameSettingsPath(s_running_game_serial));
    if (FileSystem::FileExists(filename.c_str()))
    {
      INFO_LOG("Loading game settings from '{}'...", Path::GetFileName(filename));
      new_interface = std::make_unique<INISettingsInterface>(std::move(filename));
      if (!new_interface->Load())
      {
        ERROR_LOG("Failed to parse game settings ini '{}'", new_interface->GetFileName());
        new_interface.reset();
      }
    }
    else
    {
      INFO_LOG("No game settings found (tried '{}')", Path::GetFileName(filename));
    }
  }

  std::string input_profile_name;
  if (new_interface)
  {
    if (!new_interface->GetBoolValue("ArcadeProfiles", "UseGameSettingsForArcadeInput", false))
      new_interface->GetStringValue("ArcadeProfiles", "InputProfileName", &input_profile_name);
  }

  if (!s_game_settings_interface && !new_interface && s_input_profile_name == input_profile_name)
    return false;

  auto lock = Host::GetSettingsLock();
  Host::Internal::SetGameSettingsLayer(new_interface.get(), lock);
  s_game_settings_interface = std::move(new_interface);

  std::unique_ptr<INISettingsInterface> input_interface;
  if (!input_profile_name.empty())
  {
    std::string filename = GetInputProfilePath(input_profile_name);
    if (FileSystem::FileExists(filename.c_str()))
    {
      INFO_LOG("Loading input profile from '{}'...", Path::GetFileName(filename));
      input_interface = std::make_unique<INISettingsInterface>(std::move(filename));
      if (!input_interface->Load())
      {
        ERROR_LOG("Failed to parse input profile ini '{}'", Path::GetFileName(input_interface->GetFileName()));
        input_interface.reset();
        input_profile_name = {};
      }
    }
    else
    {
      WARNING_LOG("No input profile found (tried '{}')", Path::GetFileName(filename));
      input_profile_name = {};
    }
  }

  Host::Internal::SetInputSettingsLayer(input_interface.get(), lock);
  s_input_settings_interface = std::move(input_interface);
  s_input_profile_name = std::move(input_profile_name);
  return true;
}

void System::ResetSystem()
{
  if (!IsValid())
    return;

  if (!Achievements::ConfirmSystemReset())
    return;

  if (Konami::IsGQActive())
    Konami::ResetGQ();
  if (Konami::IsGVActive())
    Konami::ResetGV();

  if (Achievements::ResetHardcoreMode(false))
  {
    // Make sure a pre-existing cheat file hasn't been loaded when resetting
    // after enabling HC mode.
    s_cheat_list.reset();
    ApplySettings(false);
  }

  if (SonyZN::IsActive())
    SonyZN::PrepareForTimingEpochReset();

  InternalReset();

  // These machine resets schedule or timestamp work against the global timing
  // epoch, so restart them only after InternalReset() has reset that epoch.
  if (NamcoSystem11::IsActive())
    NamcoSystem11::Reset();
  if (SonyZN::IsActive())
    SonyZN::Reset();

  // Reset boot mode/reload BIOS if needed.
  const BootMode new_boot_mode =
    g_settings.bios_patch_fast_boot ? BootMode::FastBoot : BootMode::FullBoot;
  if (Error error; !SetBootMode(new_boot_mode, &error))
    ERROR_LOG("Failed to reload BIOS on boot mode change, the system may be unstable: {}", error.GetDescription());

  ResetPerformanceCounters();
  ResetThrottler();
  Host::AddIconOSDMessage("system_reset", ICON_FA_POWER_OFF, TRANSLATE_STR("OSDMessage", "System reset."),
                          Host::OSD_QUICK_DURATION);

  InterruptExecution();
}

void System::PauseSystem(bool paused)
{
  if (paused == IsPaused() || !IsValid())
    return;

  SetState(paused ? State::Paused : State::Running);
  SPU::GetOutputStream()->SetPaused(paused);

  if (paused)
  {
    // Make sure the GPU is flushed, otherwise the VB might still be mapped.
    g_gpu->FlushRender();

    FullscreenUI::OnSystemPaused();

    InputManager::UpdateHostMouseMode();

    Achievements::OnSystemPaused(true);

    if (g_settings.inhibit_screensaver)
      PlatformMisc::ResumeScreensaver();

#ifdef ENABLE_GDB_SERVER
    GDBServer::OnSystemPaused();
#endif

    Host::OnSystemPaused();
    Host::OnIdleStateChanged();
    UpdateDisplayVSync();
    InvalidateDisplay();
  }
  else
  {
    FullscreenUI::OnSystemResumed();

    InputManager::UpdateHostMouseMode();

    Achievements::OnSystemPaused(false);

    if (g_settings.inhibit_screensaver)
      PlatformMisc::SuspendScreensaver();

#ifdef ENABLE_GDB_SERVER
    GDBServer::OnSystemResumed();
#endif

    Host::OnSystemResumed();
    Host::OnIdleStateChanged();

    UpdateDisplayVSync();
    ResetPerformanceCounters();
    ResetThrottler();
  }
}

bool System::SaveResumeState(Error* error)
{
  if (!CheckSaveStateSupport(error))
    return false;

  if (s_running_game_serial.empty())
  {
    Error::SetStringView(error, "Cannot save resume state without serial.");
    return false;
  }

  const std::string path(GetGameSaveStateFileName(s_running_game_serial, -1));
  return SaveState(path.c_str(), error, false);
}

bool System::BootSystem(SystemBootParameters parameters, Error* error)
{
  Assert(s_state == State::Shutdown);
  ClearActiveVideoTiming();
  Arcade::Output::Reset();

  std::optional<Arcade::BootContext> arcade_boot_context;
  std::unique_ptr<Arcade::MachineHandler> arcade_machine_handler;
  Arcade::VideoTimingStandard arcade_video_timing = Arcade::VideoTimingStandard::Invalid;
  if (!parameters.save_state.empty() && parameters.boot_intent == SystemBootIntent::InternalCompatibility)
  {
    // loading a state, so pull the media path from the save state to avoid a double change
    std::string state_media(GetMediaPathFromSaveState(parameters.save_state.c_str()));
    if (FileSystem::FileExists(state_media.c_str()))
      parameters.filename = std::move(state_media);
  }

  if (parameters.boot_intent == SystemBootIntent::PublicArcade)
  {
    Path::ToNativePath(&parameters.filename);
    Path::Canonicalize(&parameters.filename);
  }

  if (parameters.filename.empty())
    INFO_LOG("Boot Filename: <BIOS/Shell>");
  else
    INFO_LOG("Boot Filename: {}", parameters.filename);

  const Arcade::Database::GameDefinition* arcade_game = nullptr;
  if (parameters.boot_intent == SystemBootIntent::PublicArcade)
  {
    if (parameters.filename.empty())
    {
      Error::SetStringView(error, "An ArcadeDuck arcade set archive is required for a public launch.");
      return false;
    }
    if (!FileSystem::FileExists(parameters.filename.c_str()))
    {
      Error::SetStringFmt(error, "Arcade launch path '{}' does not exist.", parameters.filename);
      return false;
    }
    if (!Arcade::Database::IsArchivePath(parameters.filename))
    {
      Error::SetStringFmt(error, "File '{}' is not a supported ArcadeDuck arcade archive.", parameters.filename);
      return false;
    }

    const std::string real_path(Path::RealPath(parameters.filename));
    if (!real_path.empty())
      parameters.filename = real_path;

    arcade_game = Arcade::Database::IdentifyArchive(parameters.filename);
    if (!arcade_game)
    {
      Error::SetStringFmt(error, "Archive '{}' is not a recognized ArcadeDuck set.",
                          Path::GetFileName(parameters.filename));
      return false;
    }
  }
  else if (Arcade::Database::IsArchivePath(parameters.filename))
  {
    arcade_game = Arcade::Database::IdentifyArchive(parameters.filename);
    if (!arcade_game)
    {
      Error::SetStringFmt(error, "Arcade archive '{}' is not present in arcadedb.yaml.",
                          Path::GetFileName(parameters.filename));
      return false;
    }
  }

  if (arcade_game)
  {
    const Arcade::Database::SystemDefinition* const arcade_system = Arcade::Database::GetSystem(arcade_game->system_id);
    if (!arcade_system)
    {
      Error::SetStringFmt(error, "Arcade game '{}' references unknown system '{}'.", arcade_game->id,
                          arcade_game->system_id);
      return false;
    }

    parameters.boot_classification = SystemBootClassification::Arcade;
    arcade_boot_context.emplace(
      Arcade::BuildBootContext(parameters.filename, *arcade_game, *arcade_system, EmuFolders::DataRoot));
    Arcade::Output::SetContext(arcade_boot_context->canonical_game_id, arcade_boot_context->system_id);

    INFO_LOG("ArcadeDatabase.Loader candidate_archive='{}' canonical_set='{}' title='{}' system='{}' handler='{}'",
             parameters.filename, arcade_game->id, arcade_game->title, arcade_system->name,
             arcade_system->machine_handler);

    arcade_machine_handler = Arcade::CreateMachineHandler(arcade_boot_context->machine_handler_id);
    if (!arcade_machine_handler)
    {
      Error::SetStringFmt(error, "Arcade machine handler '{}' is not registered for game '{}'.",
                          arcade_system->machine_handler, arcade_game->id);
      return false;
    }

    arcade_video_timing = arcade_machine_handler->GetVideoTiming();
    if (arcade_video_timing == Arcade::VideoTimingStandard::Invalid)
    {
      Error::SetStringFmt(error,
                          "Arcade machine handler '{}' returned invalid video timing for game '{}' on system '{}'.",
                          arcade_system->machine_handler, arcade_game->id, arcade_system->id);
      return false;
    }

    if (!arcade_machine_handler->Preflight(*arcade_boot_context, error))
      return false;
  }

  const bool supports_save_states =
    !arcade_machine_handler || arcade_machine_handler->SupportsSaveStates();
  if (!parameters.save_state.empty() && !supports_save_states)
  {
    Error::SetStringView(error, SAVE_STATE_UNSUPPORTED_MESSAGE);
    return false;
  }

  s_state = State::Starting;
  s_startup_cancelled.store(false);
  s_keep_gpu_device_on_shutdown = static_cast<bool>(g_gpu_device);
  s_region = g_settings.region;
  Host::OnSystemStarting();

  // Load CD image up and detect region.
  std::unique_ptr<CDImage> disc;
  DiscRegion disc_region = DiscRegion::NonPS1;
  BootMode boot_mode = BootMode::FullBoot;
  if (!parameters.filename.empty() && parameters.boot_classification == SystemBootClassification::Standard)
  {
    INFO_LOG("Loading CD image '{}'...", Path::GetFileName(parameters.filename));
    disc = CDImage::Open(parameters.filename.c_str(), g_settings.cdrom_load_image_patches, error);
    if (!disc)
    {
      Error::AddPrefixFmt(error, "Failed to open CD image '{}':\n", Path::GetFileName(parameters.filename));
      s_state = State::Shutdown;
      Host::OnSystemDestroyed();
      Host::OnIdleStateChanged();
      return false;
    }

    disc_region = GetRegionForImage(disc.get());
    if (s_region == ConsoleRegion::Auto)
    {
      if (disc_region != DiscRegion::Other)
      {
        s_region = GetConsoleRegionForDiscRegion(disc_region);
        INFO_LOG("Auto-detected console {} region for '{}' (region {})", Settings::GetConsoleRegionName(s_region),
                 parameters.filename, Settings::GetDiscRegionName(disc_region));
      }
      else
      {
        s_region = ConsoleRegion::NTSC_U;
        WARNING_LOG("Could not determine console region for disc region {}. Defaulting to {}.",
                    Settings::GetDiscRegionName(disc_region), Settings::GetConsoleRegionName(s_region));
      }
    }

    if (g_settings.bios_patch_fast_boot)
    {
      if (disc_region == DiscRegion::NonPS1)
        ERROR_LOG("Not fast booting non-PS1 disc.");
      else
        boot_mode = BootMode::FastBoot;
    }
  }
  else if (parameters.boot_classification == SystemBootClassification::Standard)
  {
    // Default to NTSC for BIOS boot.
    if (s_region == ConsoleRegion::Auto)
      s_region = ConsoleRegion::NTSC_U;
  }

  if (parameters.boot_classification == SystemBootClassification::Standard)
    INFO_LOG("Compatibility Region: {}", Settings::GetConsoleRegionName(s_region));

  // Update running game, this will apply settings as well.
  UpdateRunningGame(disc ? disc->GetFileName().c_str() : parameters.filename.c_str(), disc.get(), true);

  // Check for SBI.
  if (!CheckForSBIFile(disc.get(), error))
  {
    s_state = State::Shutdown;
    ClearRunningGame();
    Host::OnSystemDestroyed();
    Host::OnIdleStateChanged();
    return false;
  }

  // Check for resuming with hardcore mode.
  if (parameters.disable_achievements_hardcore_mode)
    Achievements::DisableHardcoreMode();
  if (!parameters.save_state.empty() && Achievements::IsHardcoreModeActive())
  {
    bool cancelled;
    if (FullscreenUI::IsInitialized())
    {
      Achievements::ConfirmHardcoreModeDisableAsync(TRANSLATE("Achievements", "Resuming state"),
                                                    [parameters = std::move(parameters)](bool approved) mutable {
                                                      if (approved)
                                                      {
                                                        parameters.disable_achievements_hardcore_mode = true;
                                                        BootSystem(std::move(parameters), nullptr);
                                                      }
                                                    });
      cancelled = true;
    }
    else
    {
      cancelled = !Achievements::ConfirmHardcoreModeDisable(TRANSLATE("Achievements", "Resuming state"));
    }

    if (cancelled)
    {
      s_state = State::Shutdown;
      ClearRunningGame();
      Host::OnSystemDestroyed();
      Host::OnIdleStateChanged();

      // Technically a failure, but user-initiated. Returning false here would try to display a non-existent error.
      return true;
    }
  }

  // Arcade boards supply and install their own validated BIOS after Bus initialization.
  if (parameters.boot_classification == SystemBootClassification::Standard && !SetBootMode(boot_mode, error))
  {
    s_state = State::Shutdown;
    ClearRunningGame();
    Host::OnSystemDestroyed();
    Host::OnIdleStateChanged();
    return false;
  }

  const u32 configured_ram_size = g_settings.enable_8mb_ram ? Bus::RAM_8MB_SIZE : Bus::RAM_2MB_SIZE;
  const u32 ram_size = arcade_machine_handler ?
                         arcade_machine_handler->GetRAMSizeOverride().value_or(configured_ram_size) :
                         configured_ram_size;
  s_save_states_supported = supports_save_states;

  if (arcade_machine_handler)
  {
    DebugAssert(arcade_boot_context.has_value());
    s_active_video_timing = arcade_video_timing;
    s_active_video_timing_is_arcade = true;
    INFO_LOG("Arcade video timing game='{}' system='{}' handler='{}' standard='{}'.",
             arcade_boot_context->canonical_game_id, arcade_boot_context->system_id,
             arcade_boot_context->machine_handler_id,
             IsPALVideoTiming() ? "PAL-derived" : "NTSC-derived");
  }
  else
  {
    SetActiveVideoTimingForConsoleRegion(s_region);
  }

  // GPU construction occurs inside Initialize(), so select the board-specific
  // VRAM and register layout before renderer resources or shaders are created.
  if (arcade_machine_handler)
    arcade_machine_handler->PrepareSharedHardware();
  else
    GPU::SetGQMode(false);

  // Component setup.
  if (!Initialize(parameters.force_software_renderer, ram_size, error))
  {
    ClearActiveVideoTiming();
    s_boot_mode = System::BootMode::None;
    s_state = State::Shutdown;
    ClearRunningGame();
    Host::OnSystemDestroyed();
    Host::OnIdleStateChanged();
    return false;
  }

  if (arcade_machine_handler)
  {
    DebugAssert(arcade_boot_context.has_value());
    if (!arcade_machine_handler->Initialize(*arcade_boot_context, error))
    {
      DestroySystem();
      return false;
    }

    if (const BIOS::Image* const bios_identity = arcade_machine_handler->GetBIOSImageForSystemIdentity())
    {
      s_bios_hash = bios_identity->hash;
      s_bios_image_info = bios_identity->info;
    }

    s_boot_mode = BootMode::FullBoot;
    s_running_game_path = arcade_boot_context->archive_path;
    s_running_game_serial = arcade_boot_context->canonical_game_id;
    s_running_game_title = arcade_boot_context->game_definition->title;
  }

  // Attach boot media.
  if (disc)
    CDROM::InsertMedia(std::move(disc), disc_region);

  // Publish the final running identity after every boot classification has completed.
  // Platform loaders may replace the preliminary path, serial, or title during startup.
  UpdateRichPresence(false);
  Host::OnGameChanged(s_running_game_path, s_running_game_serial, s_running_game_title);

  InternalReset();

  // Texture replacement preloading.
  // TODO: Move this and everything else below OnSystemStarted().
  TextureReplacements::SetGameID(s_running_game_serial);

  // Good to go.
  s_state = State::Running;
  SPU::GetOutputStream()->SetPaused(false);

  FullscreenUI::OnSystemStarted();

  InputManager::UpdateHostMouseMode();

  if (g_settings.inhibit_screensaver)
    PlatformMisc::SuspendScreensaver();

#ifdef ENABLE_GDB_SERVER
  if (g_settings.debugging.enable_gdb_server)
    GDBServer::Initialize(g_settings.debugging.gdb_server_port);
#endif

  Host::OnSystemStarted();
  Host::OnIdleStateChanged();

  // try to load the state, if it fails, bail out
  if (!parameters.save_state.empty() && !LoadState(parameters.save_state.c_str(), error, false))
  {
    Error::AddPrefixFmt(error, "Failed to load save state file '{}' for booting:\n",
                        Path::GetFileName(parameters.save_state));
    DestroySystem();
    return false;
  }

  if (g_settings.cdrom_load_image_to_ram)
    CDROM::PrecacheMedia();

  if (parameters.start_media_capture)
    StartMediaCapture({});

  if (g_settings.start_paused || parameters.override_start_paused.value_or(false))
    PauseSystem(true);

  UpdateSpeedLimiterState();
  ResetPerformanceCounters();
  return true;
}

bool System::Initialize(bool force_software_renderer, u32 ram_size, Error* error)
{
  g_ticks_per_second = ScaleTicksToOverclock(MASTER_CLOCK);
  s_max_slice_ticks = ScaleTicksToOverclock(MASTER_CLOCK / 10);
  s_frame_number = 1;
  s_internal_frame_number = 1;

  s_target_speed = g_settings.emulation_speed;
  s_throttle_frequency = 60.0f;
  s_frame_period = 0;
  s_next_frame_time = 0;
  s_turbo_enabled = false;
  s_fast_forward_enabled = false;

  s_rewind_load_frequency = -1;
  s_rewind_load_counter = -1;
  s_rewinding_first_save = true;

  s_average_frame_time_accumulator = 0.0f;
  s_minimum_frame_time_accumulator = 0.0f;
  s_maximum_frame_time_accumulator = 0.0f;

  s_vps = 0.0f;
  s_fps = 0.0f;
  s_speed = 0.0f;
  s_minimum_frame_time = 0.0f;
  s_maximum_frame_time = 0.0f;
  s_average_frame_time = 0.0f;
  s_cpu_thread_usage = 0.0f;
  s_cpu_thread_time = 0.0f;
  s_sw_thread_usage = 0.0f;
  s_sw_thread_time = 0.0f;
  s_average_gpu_time = 0.0f;
  s_accumulated_gpu_time = 0.0f;
  s_gpu_usage = 0.0f;
  s_last_frame_number = 0;
  s_last_internal_frame_number = 0;
  s_last_global_tick_counter = 0;
  s_presents_since_last_update = 0;
  s_last_cpu_time = 0;
  s_fps_timer.Reset();
  s_frame_timer.Reset();
  s_frame_time_history.fill(0.0f);
  s_frame_time_history_pos = 0;

  TimingEvents::Initialize();

  CPU::Initialize();

  if (!Bus::Initialize(ram_size))
  {
    CPU::Shutdown();
    return false;
  }

  CPU::CodeCache::Initialize();

  if (!CreateGPU(force_software_renderer ? GPURenderer::Software : g_settings.gpu_renderer, false, error))
  {
    Bus::Shutdown();
    CPU::Shutdown();
    return false;
  }

  GTE::UpdateAspectRatio();

  if (g_settings.gpu_pgxp_enable)
    CPU::PGXP::Initialize();

  // Was startup cancelled? (e.g. shading compilers took too long and the user closed the application)
  if (IsStartupCancelled())
  {
    g_gpu.reset();
    if (!s_keep_gpu_device_on_shutdown)
    {
      Host::ReleaseGPUDevice();
      Host::ReleaseRenderWindow();
    }
    if (g_settings.gpu_pgxp_enable)
      CPU::PGXP::Shutdown();
    CPU::Shutdown();
    Bus::Shutdown();
    return false;
  }

  DMA::Initialize();
  CDROM::Initialize();
  Timers::Initialize();
  SPU::Initialize();
  MDEC::Initialize();
  SIO::Initialize();
  PCDrv::Initialize();
  PostProcessing::Initialize();

  s_cpu_thread_handle = Threading::ThreadHandle::GetForCallingThread();

  UpdateThrottlePeriod();
  UpdateMemorySaveStateSettings();
  return true;
}

void System::DestroySystem()
{
  DebugAssert(!s_system_executing);
  if (s_state == State::Shutdown)
    return;

  if (s_media_capture)
    StopMediaCapture();

  s_undo_load_state.reset();

#ifdef ENABLE_GDB_SERVER
  GDBServer::Shutdown();
#endif

  Host::ClearOSDMessages();

  PostProcessing::Shutdown();

  SaveStateSelectorUI::Clear();
  FullscreenUI::OnSystemDestroyed();

  InputManager::UpdateHostMouseMode();

  if (g_settings.inhibit_screensaver)
    PlatformMisc::ResumeScreensaver();

  SetTimerResolutionIncreased(false);

  s_cpu_thread_usage = {};

  ClearMemorySaveStates();

  TextureReplacements::Shutdown();

  // Publish an inactive state before board/device teardown. This is also the
  // safety behavior future external-output transports will inherit.
  Arcade::Output::ClearValues();
  Arcade::Output::Shutdown();

  PCDrv::Shutdown();
  SIO::Shutdown();
  MDEC::Shutdown();
  SPU::Shutdown();
  Timers::Shutdown();
  CDROM::Shutdown();
  g_gpu.reset();
  Konami::ShutdownGQ();
  NamcoSystem11::Shutdown();
  SonyZN::Shutdown();
  DMA::Shutdown();
  CPU::PGXP::Shutdown();
  CPU::CodeCache::Shutdown();
  Konami::ShutdownGV();
  Bus::Shutdown();
  CPU::Shutdown();
  TimingEvents::Shutdown();
  ClearRunningGame();

  // Restore present-all-frames behavior.
  if (s_keep_gpu_device_on_shutdown && g_gpu_device)
  {
    UpdateDisplayVSync();
  }
  else
  {
    Host::ReleaseGPUDevice();
    Host::ReleaseRenderWindow();
  }

  s_bios_hash = {};
  s_bios_image_info = nullptr;
  s_boot_mode = BootMode::None;
  s_cheat_list.reset();
  ClearActiveVideoTiming();

  s_state = State::Shutdown;

  Host::OnSystemDestroyed();
  Host::OnIdleStateChanged();
}

void System::ClearRunningGame()
{
  UpdateSessionTime(s_running_game_serial);

  s_running_game_serial.clear();
  s_running_game_path.clear();
  s_running_game_title.clear();
  s_running_game_entry = nullptr;
  s_running_game_hash = 0;
  s_save_states_supported = true;

  Host::OnGameChanged(s_running_game_path, s_running_game_serial, s_running_game_title);

  Achievements::GameChanged(s_running_game_path, nullptr);

  UpdateRichPresence(true);
}

void System::Execute()
{
  for (;;)
  {
    switch (s_state)
    {
      case State::Running:
      {
        s_system_executing = true;

        // TODO: Purge reset/restore
        g_gpu->RestoreDeviceContext();
        TimingEvents::CommitLeftoverTicks();

        if (s_rewind_load_counter >= 0)
          DoRewind();
        else
          CPU::Execute();

        s_system_executing = false;
        continue;
      }

      case State::Stopping:
      {
        DestroySystem();
        return;
      }

      case State::Paused:
      default:
        return;
    }
  }
}

void System::FrameDone()
{
  s_frame_number++;

  // Vertex buffer is shared, need to flush what we have.
  g_gpu->FlushRender();

  // Generate any pending samples from the SPU before sleeping, this way we reduce the chances of underruns.
  // TODO: when running ahead, we can skip this (and the flush above)
  SPU::GeneratePendingSamples();

  if (s_cheat_list)
    s_cheat_list->Apply();

  if (Achievements::IsActive())
    Achievements::FrameUpdate();

  if (NamcoSystem11::BeginMainBoardReset())
  {
    INFO_LOG("Namco System 11 full board reset requested by C76.");
    InternalReset();
    NamcoSystem11::EndMainBoardReset();
    ResetPerformanceCounters();
    ResetThrottler();
    InterruptExecution();
    return;
  }

  if (SonyZN::BeginMainBoardReset())
  {
    INFO_LOG("Sony ZN full board reset requested by Taito MB3773.");
    SonyZN::PrepareForTimingEpochReset();
    InternalReset();
    SonyZN::EndMainBoardReset();
    ResetPerformanceCounters();
    ResetThrottler();
    InterruptExecution();
    return;
  }

  if (SonyZN::IsActive())
    SonyZN::ProcessFrame();

  if (Konami::IsGQActive())
    Konami::ProcessGQFrame();

  if (Konami::IsGVActive() && Konami::ProcessGVFrame())
  {
    ResetSystem();
    return;
  }

#ifdef ENABLE_DISCORD_PRESENCE
  PollDiscordPresence();
#endif

#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (s_socket_multiplexer)
    s_socket_multiplexer->PollEventsWithTimeout(0);
#endif

  Host::FrameDone();

  if (s_frame_step_request)
  {
    s_frame_step_request = false;
    PauseSystem(true);
  }

  // Save states for rewind and runahead.
  if (s_rewind_save_counter >= 0)
  {
    if (s_rewind_save_counter == 0)
    {
      SaveRewindState();
      s_rewind_save_counter = s_rewind_save_frequency;
    }
    else
    {
      s_rewind_save_counter--;
    }
  }
  else if (s_runahead_frames > 0)
  {
    // We don't want to poll during replay, because otherwise we'll lose frames.
    if (s_runahead_replay_frames == 0)
    {
      // For runahead, poll input early, that way we can use the remainder of this frame to replay.
      // *technically* this means higher input latency (by less than a frame), but runahead itself
      // counter-acts that.
      Host::PumpMessagesOnCPUThread();
      InputManager::PollSources();
      g_gpu->RestoreDeviceContext();
      CheckForAndExitExecution();
    }

    if (DoRunahead())
    {
      // running ahead, get it done as soon as possible
      return;
    }

    SaveRunaheadState();
  }

  // Kick off media capture early, might take a while.
  if (s_media_capture && s_media_capture->IsCapturingVideo()) [[unlikely]]
  {
    if (s_media_capture->GetVideoFPS() != GetThrottleFrequency()) [[unlikely]]
    {
      const std::string next_capture_path = s_media_capture->GetNextCapturePath();
      INFO_LOG("Video frame rate changed, switching to new capture file {}", Path::GetFileName(next_capture_path));

      const bool was_capturing_audio = s_media_capture->IsCapturingAudio();
      StopMediaCapture();
      if (StartMediaCapture(std::move(next_capture_path), true, was_capturing_audio) &&
          !g_gpu->SendDisplayToMediaCapture(s_media_capture.get())) [[unlikely]]
      {
        StopMediaCapture();
      }
    }
    else
    {
      if (!g_gpu->SendDisplayToMediaCapture(s_media_capture.get())) [[unlikely]]
        StopMediaCapture();
    }
  }

  Common::Timer::Value current_time = Common::Timer::GetCurrentValue();

  // pre-frame sleep accounting (input lag reduction)
  const Common::Timer::Value pre_frame_sleep_until = s_next_frame_time + s_pre_frame_sleep_time;
  s_last_active_frame_time = current_time - s_frame_start_time;
  if (s_pre_frame_sleep)
    AccumulatePreFrameSleepTime();

  // explicit present (frame pacing)
  const bool is_unique_frame = (s_last_presented_internal_frame_number != s_internal_frame_number);
  s_last_presented_internal_frame_number = s_internal_frame_number;

  const bool skip_this_frame = (((s_skip_presenting_duplicate_frames && !is_unique_frame &&
                                  s_skipped_frame_count < MAX_SKIPPED_DUPLICATE_FRAME_COUNT) ||
                                 (!s_optimal_frame_pacing && current_time > s_next_frame_time &&
                                  s_skipped_frame_count < MAX_SKIPPED_TIMEOUT_FRAME_COUNT) ||
                                 g_gpu_device->ShouldSkipPresentingFrame()) &&
                                !s_syncing_to_host_with_vsync && !IsExecutionInterrupted());
  if (!skip_this_frame)
  {
    s_skipped_frame_count = 0;

    const bool throttle_before_present = (s_optimal_frame_pacing && s_throttler_enabled && !IsExecutionInterrupted());
    const bool explicit_present = (throttle_before_present && g_gpu_device->GetFeatures().explicit_present);
    if (explicit_present)
    {
      const bool do_present = PresentDisplay(false, true);
      Throttle(current_time);
      if (do_present)
        g_gpu_device->SubmitPresent();
    }
    else
    {
      if (throttle_before_present)
        Throttle(current_time);

      PresentDisplay(false, false);

      if (!throttle_before_present && s_throttler_enabled && !IsExecutionInterrupted())
        Throttle(current_time);
    }
  }
  else
  {
    DEBUG_LOG("Skipping displaying frame");
    s_skipped_frame_count++;
    if (s_throttler_enabled)
      Throttle(current_time);
  }

  // pre-frame sleep (input lag reduction)
  current_time = Common::Timer::GetCurrentValue();
  if (s_pre_frame_sleep)
  {
    // don't sleep if it's under 1ms, because we're just going to overshoot (or spin).
    if (pre_frame_sleep_until > current_time &&
        Common::Timer::ConvertValueToMilliseconds(pre_frame_sleep_until - current_time) >= 1)
    {
      Common::Timer::SleepUntil(pre_frame_sleep_until, true);
      current_time = Common::Timer::GetCurrentValue();
    }
  }

  s_frame_start_time = current_time;

  // Input poll already done above
  if (s_runahead_frames == 0)
  {
    Host::PumpMessagesOnCPUThread();
    InputManager::PollSources();
    CheckForAndExitExecution();
  }

  g_gpu->RestoreDeviceContext();

  // Update perf counters *after* throttling, we want to measure from start-of-frame
  // to start-of-frame, not end-of-frame to end-of-frame (will be noisy due to different
  // amounts of computation happening in each frame).
  System::UpdatePerformanceCounters();
}

void System::SetThrottleFrequency(float frequency)
{
  if (s_throttle_frequency == frequency)
    return;

  s_throttle_frequency = frequency;
  UpdateThrottlePeriod();
}

void System::UpdateThrottlePeriod()
{
  if (s_target_speed > std::numeric_limits<double>::epsilon())
  {
    const double target_speed = std::max(static_cast<double>(s_target_speed), std::numeric_limits<double>::epsilon());
    s_frame_period =
      Common::Timer::ConvertSecondsToValue(1.0 / (static_cast<double>(s_throttle_frequency) * target_speed));
  }
  else
  {
    s_frame_period = 1;
  }

  ResetThrottler();
}

void System::ResetThrottler()
{
  s_next_frame_time = Common::Timer::GetCurrentValue() + s_frame_period;
  s_pre_frame_sleep_time = 0;
}

void System::Throttle(Common::Timer::Value current_time)
{
  // If we're running too slow, advance the next frame time based on the time we lost. Effectively skips
  // running those frames at the intended time, because otherwise if we pause in the debugger, we'll run
  // hundreds of frames when we resume.
  if (current_time > s_next_frame_time)
  {
    const Common::Timer::Value diff = static_cast<s64>(current_time) - static_cast<s64>(s_next_frame_time);
    s_next_frame_time += (diff / s_frame_period) * s_frame_period + s_frame_period;
    return;
  }

#ifdef ENABLE_SOCKET_MULTIPLEXER
  // If we are using the socket multiplier, and have clients, then use it to sleep instead.
  // That way in a query->response->query->response chain, we don't process only one message per frame.
  if (s_socket_multiplexer && s_socket_multiplexer->HasAnyClientSockets())
  {
    Common::Timer::Value poll_start_time = current_time;
    for (;;)
    {
      const u32 sleep_ms =
        static_cast<u32>(Common::Timer::ConvertValueToMilliseconds(s_next_frame_time - poll_start_time));
      s_socket_multiplexer->PollEventsWithTimeout(sleep_ms);
      poll_start_time = Common::Timer::GetCurrentValue();
      if (poll_start_time >= s_next_frame_time || (!g_settings.display_optimal_frame_pacing && sleep_ms == 0))
        break;
    }
  }
  else
  {
    // Use a spinwait if we undersleep for all platforms except android.. don't want to burn battery.
    // Linux also seems to do a much better job of waking up at the requested time.
#if !defined(__linux__)
    Common::Timer::SleepUntil(s_next_frame_time, g_settings.display_optimal_frame_pacing);
#else
    Common::Timer::SleepUntil(s_next_frame_time, false);
#endif
  }
#else
  // No spinwait on Android, see above.
  Common::Timer::SleepUntil(s_next_frame_time, false);
#endif

#if 0
  DEV_LOG("Asked for {:.2f} ms, slept for {:.2f} ms, {:.2f} ms late",
          Common::Timer::ConvertValueToMilliseconds(s_next_frame_time - current_time),
          Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - current_time),
          Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - s_next_frame_time));
#endif

  s_next_frame_time += s_frame_period;
}

void System::SingleStepCPU()
{
  CPU::SetSingleStepFlag();

  // If this gets called when the system is executing, we're not going to end up here..
  if (IsPaused())
    PauseSystem(false);
}

void System::IncrementInternalFrameNumber()
{
  s_internal_frame_number++;
}

bool System::CreateGPU(GPURenderer renderer, bool is_switching, Error* error)
{
  const RenderAPI api = Settings::GetRenderAPIForRenderer(renderer);

  if (!g_gpu_device ||
      (renderer != GPURenderer::Software && !GPUDevice::IsSameRenderAPI(g_gpu_device->GetRenderAPI(), api)))
  {
    if (g_gpu_device)
    {
      WARNING_LOG("Recreating GPU device, expecting {} got {}", GPUDevice::RenderAPIToString(api),
                  GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()));
      PostProcessing::Shutdown();
    }

    Host::ReleaseGPUDevice();
    if (!Host::CreateGPUDevice(api, error))
    {
      Host::ReleaseRenderWindow();
      return false;
    }

    if (is_switching)
      PostProcessing::Initialize();
  }

  if (renderer == GPURenderer::Software)
    g_gpu = GPU::CreateSoftwareRenderer();
  else
    g_gpu = GPU::CreateHardwareRenderer();

  if (!g_gpu)
  {
    ERROR_LOG("Failed to initialize {} renderer, falling back to software renderer",
              Settings::GetRendererName(renderer));
    Host::AddOSDMessage(
      fmt::format(TRANSLATE_FS("System", "Failed to initialize {} renderer, falling back to software renderer."),
                  Settings::GetRendererName(renderer)),
      Host::OSD_CRITICAL_ERROR_DURATION);
    g_gpu.reset();
    g_gpu = GPU::CreateSoftwareRenderer();
    if (!g_gpu)
    {
      ERROR_LOG("Failed to create fallback software renderer.");
      if (!s_keep_gpu_device_on_shutdown)
      {
        PostProcessing::Shutdown();
        Host::ReleaseGPUDevice();
        Host::ReleaseRenderWindow();
      }
      return false;
    }
  }

  return true;
}

bool System::DoState(StateWrapper& sw, GPUTexture** host_texture, bool update_display, bool is_memory_state)
{
  if (!sw.DoMarker("System"))
    return false;

  sw.Do(&s_region);
  if (sw.IsReading() && !s_active_video_timing_is_arcade)
    SetActiveVideoTimingForConsoleRegion(s_region);
  sw.Do(&s_frame_number);
  sw.Do(&s_internal_frame_number);

  // Don't bother checking this at all for memory states, since they won't have a different BIOS...
  if (!is_memory_state)
  {
    BIOS::ImageInfo::Hash bios_hash = s_bios_hash;
    sw.DoBytesEx(bios_hash.data(), BIOS::ImageInfo::HASH_SIZE, 58, s_bios_hash.data());
    if (bios_hash != s_bios_hash)
    {
      WARNING_LOG("BIOS hash mismatch: System: {} | State: {}", BIOS::ImageInfo::GetHashString(s_bios_hash),
                  BIOS::ImageInfo::GetHashString(bios_hash));
      Host::AddIconOSDMessage(
        "StateBIOSMismatch", ICON_FA_EXCLAMATION_TRIANGLE,
        TRANSLATE_STR("System", "This save state was created with a different BIOS. This may cause stability issues."),
        Host::OSD_WARNING_DURATION);
    }
  }

  if (!sw.DoMarker("CPU") || !CPU::DoState(sw))
    return false;

  if (sw.IsReading())
  {
    if (is_memory_state)
      CPU::CodeCache::InvalidateAllRAMBlocks();
    else
      CPU::CodeCache::Reset();
  }

  // only reset pgxp if we're not runahead-rollbacking. the value checks will save us from broken rendering, and it
  // saves using imprecise values for a frame in 30fps games.
  if (sw.IsReading() && g_settings.gpu_pgxp_enable && !is_memory_state)
    CPU::PGXP::Reset();

  if (!sw.DoMarker("Bus") || !Bus::DoState(sw))
    return false;

  if (!sw.DoMarker("DMA") || !DMA::DoState(sw))
    return false;

  if (!sw.DoMarker("InterruptController") || !InterruptController::DoState(sw))
    return false;

  g_gpu->RestoreDeviceContext();
  if (!sw.DoMarker("GPU") || !g_gpu->DoState(sw, host_texture, update_display))
    return false;

  if (!sw.DoMarker("CDROM") || !CDROM::DoState(sw))
    return false;

  if (!sw.DoMarker("Pad") || !LegacyPadState::DoState(sw))
    return false;

  if (!sw.DoMarker("Timers") || !Timers::DoState(sw))
    return false;

  if (!sw.DoMarker("SPU") || !SPU::DoState(sw))
    return false;

  if (!sw.DoMarker("MDEC") || !MDEC::DoState(sw))
    return false;

  if (!sw.DoMarker("SIO") || !SIO::DoState(sw))
    return false;

  if (!sw.DoMarker("Events") || !TimingEvents::DoState(sw))
    return false;

  if (!sw.DoMarker("Overclock"))
    return false;

  bool cpu_overclock_active = g_settings.cpu_overclock_active;
  u32 cpu_overclock_numerator = g_settings.cpu_overclock_numerator;
  u32 cpu_overclock_denominator = g_settings.cpu_overclock_denominator;
  sw.Do(&cpu_overclock_active);
  sw.Do(&cpu_overclock_numerator);
  sw.Do(&cpu_overclock_denominator);

  if (sw.IsReading() && sw.GetVersion() < 72)
  {
    // v71 and older predate the cached SPU clock-domain fields. Their pending
    // TimingEvent timestamps belong to the ratio stored in this Overclock block,
    // so reconstruct that saved domain before any transition to current settings.
    SPU::RestoreLegacyClockDomainFromState(cpu_overclock_active, cpu_overclock_numerator,
                                           cpu_overclock_denominator);
  }

  if (sw.IsReading() && (cpu_overclock_active != g_settings.cpu_overclock_active ||
                         (cpu_overclock_active && (g_settings.cpu_overclock_numerator != cpu_overclock_numerator ||
                                                   g_settings.cpu_overclock_denominator != cpu_overclock_denominator))))
  {
    Host::AddIconOSDMessage(
      "state_overclock_difference", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("System", "WARNING: CPU overclock ({}%) was different in save state ({}%)."),
                  g_settings.cpu_overclock_enable ? g_settings.GetCPUOverclockPercent() : 100u,
                  cpu_overclock_active ?
                    Settings::CPUOverclockFractionToPercent(cpu_overclock_numerator, cpu_overclock_denominator) :
                    100u),
      Host::OSD_WARNING_DURATION);
    UpdateOverclock();
  }

  if (!is_memory_state)
  {
    if (sw.GetVersion() >= 56) [[unlikely]]
    {
      if (!sw.DoMarker("Cheevos"))
        return false;

      if (!Achievements::DoState(sw))
        return false;
    }
    else
    {
      // loading an old state without cheevos, so reset the runtime
      Achievements::ResetClient();
    }
  }

  return !sw.HasError();
}

bool System::LoadBIOS(Error* error)
{
  std::optional<BIOS::Image> bios_image = BIOS::GetBIOSImage(s_region, error);
  if (!bios_image.has_value())
    return false;

  s_bios_image_info = bios_image->info;
  s_bios_hash = bios_image->hash;
  if (s_bios_image_info)
    INFO_LOG("Using BIOS: {}", s_bios_image_info->description);
  else
    WARNING_LOG("Using an unknown BIOS: {}", BIOS::ImageInfo::GetHashString(s_bios_hash));

  std::memcpy(Bus::g_bios, bios_image->data.data(), Bus::BIOS_SIZE);
  return true;
}

void System::InternalReset()
{
  if (IsShutdown())
    return;

  // Cabinet outputs are host-visible actuator/lamp state, not persistent machine state.
  // Reset them before the emulated board reinitializes and republishes its outputs.
  Arcade::Output::ClearValues();

  TimingEvents::Reset();
  CPU::Reset();
  CPU::CodeCache::Reset();
  if (g_settings.gpu_pgxp_enable)
    CPU::PGXP::Initialize();

  Bus::Reset();
  DMA::Reset();
  InterruptController::Reset();
  g_gpu->Reset(true);
  CDROM::Reset();
  Timers::Reset();
  SPU::Reset();
  MDEC::Reset();
  SIO::Reset();
  PCDrv::Reset();
  Achievements::ResetClient();
  s_frame_number = 1;
  s_internal_frame_number = 0;
}

bool System::SetBootMode(BootMode new_boot_mode, Error* error)
{
  // Can we actually fast boot? If starting, s_bios_image_info won't be valid.
  const bool can_fast_boot =
    (CDROM::IsMediaPS1Disc() &&
     (s_state == State::Starting || (s_bios_image_info && s_bios_image_info->SupportsFastBoot())));
  const System::BootMode actual_new_boot_mode =
    (new_boot_mode == BootMode::FastBoot) ? (can_fast_boot ? BootMode::FastBoot : BootMode::FullBoot) : new_boot_mode;
  if (actual_new_boot_mode == s_boot_mode)
    return true;

  // Need to reload the BIOS to wipe out the patching.
  if (!LoadBIOS(error))
    return false;

  s_boot_mode = actual_new_boot_mode;
  if (s_boot_mode == BootMode::FastBoot)
  {
    if (s_bios_image_info && s_bios_image_info->SupportsFastBoot())
    {
      // Patch BIOS, this sucks.
      INFO_LOG("Patching BIOS for fast boot.");
      if (!BIOS::PatchBIOSFastBoot(Bus::g_bios, Bus::BIOS_SIZE, s_bios_image_info->fastboot_patch))
        s_boot_mode = BootMode::FullBoot;
    }
    else
    {
      ERROR_LOG("Cannot fast boot, BIOS is incompatible.");
      s_boot_mode = BootMode::FullBoot;
    }
  }

  return true;
}

size_t System::GetMaxSaveStateSize()
{
  // 5 megabytes is sufficient for now, at the moment they're around 4.3MB, or 10.3MB with 8MB RAM enabled.
  static constexpr u32 MAX_2MB_SAVE_STATE_SIZE = 5 * 1024 * 1024;
  static constexpr u32 MAX_8MB_SAVE_STATE_SIZE = 11 * 1024 * 1024;
  const bool is_8mb_ram = (System::IsValid() ? (Bus::g_ram_size > Bus::RAM_2MB_SIZE) : g_settings.enable_8mb_ram);
  return is_8mb_ram ? MAX_8MB_SAVE_STATE_SIZE : MAX_2MB_SAVE_STATE_SIZE;
}

std::string System::GetMediaPathFromSaveState(const char* path)
{
  SaveStateBuffer buffer;
  auto fp = FileSystem::OpenManagedCFile(path, "rb", nullptr);
  if (fp)
    LoadStateBufferFromFile(&buffer, fp.get(), nullptr, false, true, false, false);

  return std::move(buffer.media_path);
}

bool System::LoadState(const char* path, Error* error, bool save_undo_state)
{
  if (!CheckSaveStateSupport(error))
    return false;

  if (!IsValid())
  {
    Error::SetStringView(error, "System is not booted.");
    return false;
  }

  if (Achievements::IsHardcoreModeActive())
  {
    Achievements::ConfirmHardcoreModeDisableAsync(TRANSLATE("Achievements", "Loading state"),
                                                  [path = std::string(path), save_undo_state](bool approved) {
                                                    if (approved)
                                                      LoadState(path.c_str(), nullptr, save_undo_state);
                                                  });
    return true;
  }

  Common::Timer load_timer;

  auto fp = FileSystem::OpenManagedCFile(path, "rb", error);
  if (!fp)
  {
    Error::AddPrefixFmt(error, "Failed to open '{}': ", Path::GetFileName(path));
    return false;
  }

  INFO_LOG("Loading state from '{}'...", path);

  Host::AddIconOSDMessage(
    "load_state", ICON_EMOJI_OPEN_THE_FOLDER,
    fmt::format(TRANSLATE_FS("OSDMessage", "Loading state from '{}'..."), Path::GetFileName(path)),
    Host::OSD_INFO_DURATION);

  if (save_undo_state)
    SaveUndoLoadState();

  SaveStateBuffer buffer;
  if (!LoadStateBufferFromFile(&buffer, fp.get(), error, false, true, false, true) ||
      !LoadStateFromBuffer(buffer, error, true))
  {
    if (save_undo_state)
      UndoLoadState();

    return false;
  }

  ResetPerformanceCounters();
  ResetThrottler();

  if (IsPaused())
    InvalidateDisplay();

  VERBOSE_LOG("Loading state took {:.2f} msec", load_timer.GetTimeMilliseconds());
  return true;
}

bool System::LoadStateFromBuffer(const SaveStateBuffer& buffer, Error* error, bool update_display)
{
  if (!CheckSaveStateSupport(error))
    return false;

  Assert(IsValid());

  std::unique_ptr<CDImage> media;
  std::unique_ptr<CDImage> old_media = CDROM::RemoveMedia(false);
  std::string_view media_path = buffer.media_path;
  u32 media_subimage_index = buffer.media_subimage_index;
  if (old_media && old_media->GetFileName() == buffer.media_path)
  {
    INFO_LOG("Re-using same media '{}'", buffer.media_path);
    media = std::move(old_media);
  }
  else if (!buffer.media_path.empty())
  {
    Error local_error;
    media = CDImage::Open(buffer.media_path.c_str(), g_settings.cdrom_load_image_patches, error ? error : &local_error);
    if (!media)
    {
      if (old_media)
      {
        Host::AddOSDMessage(
          fmt::format(TRANSLATE_FS("OSDMessage", "Failed to open CD image from save state '{}': {}.\nUsing "
                                                 "existing image '{}', this may result in instability."),
                      buffer.media_path, error ? error->GetDescription() : local_error.GetDescription(),
                      old_media->GetFileName()),
          Host::OSD_CRITICAL_ERROR_DURATION);
        media = std::move(old_media);
        media_path = media->GetFileName();
        media_subimage_index = media->GetCurrentSubImage();
      }
      else
      {
        Error::AddPrefixFmt(error, TRANSLATE_FS("System", "Failed to open CD image '{}' used by save state:\n"),
                            Path::GetFileName(buffer.media_path));
        return false;
      }
    }
  }

  if (media && buffer.version >= 51)
  {
    const u32 num_subimages = media->HasSubImages() ? media->GetSubImageCount() : 1;
    if (media_subimage_index >= num_subimages ||
        (media->HasSubImages() && media->GetCurrentSubImage() != media_subimage_index &&
         !media->SwitchSubImage(media_subimage_index, error)))
    {
      Error::AddPrefixFmt(
        error, TRANSLATE_FS("System", "Failed to switch to subimage {} in CD image '{}' used by save state:\n"),
        media_subimage_index + 1u, Path::GetFileName(media_path));
      return false;
    }
    else
    {
      INFO_LOG("Switched to subimage {} in '{}'", media_subimage_index, buffer.media_path.c_str());
    }
  }

  // Skip updating media if there is none, and none in the state. That way we don't wipe out EXE boot.
  if (media)
    UpdateRunningGame(media_path, media.get(), false);

  CDROM::Reset();
  if (media)
  {
    const DiscRegion region = GetRegionForImage(media.get());
    CDROM::InsertMedia(std::move(media), region);
    if (g_settings.cdrom_load_image_to_ram)
      CDROM::PrecacheMedia();
  }

  ClearMemorySaveStates();

  // Updating game/loading settings can turn on hardcore mode. Catch this.
  Achievements::DisableHardcoreMode();

  StateWrapper sw(buffer.state_data.cspan(0, buffer.state_size), StateWrapper::Mode::Read, buffer.version);
  if (!DoState(sw, nullptr, update_display, false))
  {
    Error::SetStringView(error, "Save state stream is corrupted.");
    return false;
  }

  InterruptExecution();
  ResetPerformanceCounters();
  ResetThrottler();
  return true;
}

bool System::LoadStateBufferFromFile(SaveStateBuffer* buffer, std::FILE* fp, Error* error, bool read_title,
                                     bool read_media_path, bool read_screenshot, bool read_data)
{
  const s64 file_size = FileSystem::FSize64(fp, error);
  if (file_size < 0)
    return false;

  DebugAssert(FileSystem::FTell64(fp) == 0);

  SAVE_STATE_HEADER header;
  if (std::fread(&header, sizeof(header), 1, fp) != 1 || header.magic != SAVE_STATE_MAGIC) [[unlikely]]
  {
    Error::SetErrno(error, "fread() for header failed: ", errno);
    return false;
  }

  if (header.version < SAVE_STATE_MINIMUM_VERSION)
  {
    Error::SetStringFmt(
      error, TRANSLATE_FS("System", "Save state is incompatible: minimum version is {0} but state is version {1}."),
      SAVE_STATE_MINIMUM_VERSION, header.version);
    return false;
  }

  if (header.version > SAVE_STATE_VERSION)
  {
    Error::SetStringFmt(
      error, TRANSLATE_FS("System", "Save state is incompatible: maximum version is {0} but state is version {1}."),
      SAVE_STATE_VERSION, header.version);
    return false;
  }

  // Validate offsets.
  if ((static_cast<s64>(header.offset_to_media_path) + header.media_path_length) > file_size ||
      (static_cast<s64>(header.offset_to_screenshot) + header.screenshot_compressed_size) > file_size ||
      header.screenshot_width >= 32768 || header.screenshot_height >= 32768 ||
      (static_cast<s64>(header.offset_to_data) + header.data_compressed_size) > file_size ||
      header.data_uncompressed_size > SAVE_STATE_HEADER::MAX_SAVE_STATE_SIZE) [[unlikely]]
  {
    Error::SetStringView(error, "Save state header is corrupted.");
    return false;
  }

  buffer->version = header.version;

  if (read_title)
  {
    buffer->title.assign(header.title, StringUtil::Strnlen(header.title, std::size(header.title)));
    buffer->serial.assign(header.serial, StringUtil::Strnlen(header.serial, std::size(header.serial)));
  }

  // Read media path.
  if (read_media_path)
  {
    buffer->media_path.resize(header.media_path_length);
    buffer->media_subimage_index = header.media_subimage_index;
    if (header.media_path_length > 0)
    {
      if (!FileSystem::FSeek64(fp, header.offset_to_media_path, SEEK_SET, error)) [[unlikely]]
        return false;

      if (std::fread(buffer->media_path.data(), buffer->media_path.length(), 1, fp) != 1) [[unlikely]]
      {
        Error::SetErrno(error, "fread() for media path failed: ", errno);
        return false;
      }
    }
  }

  // Read screenshot if requested.
  if (read_screenshot)
  {
    buffer->screenshot.SetSize(header.screenshot_width, header.screenshot_height);
    const u32 uncompressed_size = buffer->screenshot.GetPitch() * buffer->screenshot.GetHeight();
    const u32 compressed_size = (header.version >= 69) ? header.screenshot_compressed_size : uncompressed_size;
    const SAVE_STATE_HEADER::CompressionType compression_type =
      (header.version >= 69) ? static_cast<SAVE_STATE_HEADER::CompressionType>(header.screenshot_compression_type) :
                               SAVE_STATE_HEADER::CompressionType::None;
    if (!ReadAndDecompressStateData(
          fp, std::span<u8>(reinterpret_cast<u8*>(buffer->screenshot.GetPixels()), uncompressed_size),
          header.offset_to_screenshot, compressed_size, compression_type, error)) [[unlikely]]
    {
      return false;
    }
  }

  // Decompress state data.
  if (read_data)
  {
    buffer->state_data.resize(header.data_uncompressed_size);
    buffer->state_size = header.data_uncompressed_size;
    if (!ReadAndDecompressStateData(fp, buffer->state_data.span(), header.offset_to_data, header.data_compressed_size,
                                    static_cast<SAVE_STATE_HEADER::CompressionType>(header.data_compression_type),
                                    error)) [[unlikely]]
    {
      return false;
    }
  }

  return true;
}

bool System::ReadAndDecompressStateData(std::FILE* fp, std::span<u8> dst, u32 file_offset, u32 compressed_size,
                                        SAVE_STATE_HEADER::CompressionType method, Error* error)
{
  if (!FileSystem::FSeek64(fp, file_offset, SEEK_SET, error))
    return false;

  if (method == SAVE_STATE_HEADER::CompressionType::None)
  {
    // Feed through.
    if (std::fread(dst.data(), dst.size(), 1, fp) != 1) [[unlikely]]
    {
      Error::SetErrno(error, "fread() failed: ", errno);
      return false;
    }

    return true;
  }

  DynamicHeapArray<u8> compressed_data(compressed_size);
  if (std::fread(compressed_data.data(), compressed_data.size(), 1, fp) != 1)
  {
    Error::SetErrno(error, "fread() failed: ", errno);
    return false;
  }

  if (method == SAVE_STATE_HEADER::CompressionType::Deflate)
  {
    uLong source_len = compressed_size;
    uLong dest_len = static_cast<uLong>(dst.size());
    const int err = uncompress2(dst.data(), &dest_len, compressed_data.data(), &source_len);
    if (err != Z_OK) [[unlikely]]
    {
      Error::SetStringFmt(error, "uncompress2() failed: ", err);
      return false;
    }
    else if (dest_len < dst.size()) [[unlikely]]
    {
      Error::SetStringFmt(error, "Only decompressed {} of {} bytes", dest_len, dst.size());
      return false;
    }

    if (source_len < compressed_size) [[unlikely]]
      WARNING_LOG("Only consumed {} of {} compressed bytes", source_len, compressed_size);

    return true;
  }
  else if (method == SAVE_STATE_HEADER::CompressionType::Zstandard)
  {
    const size_t result = ZSTD_decompress(dst.data(), dst.size(), compressed_data.data(), compressed_size);
    if (ZSTD_isError(result)) [[unlikely]]
    {
      const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(result));
      Error::SetStringFmt(error, "ZSTD_decompress() failed: {}", errstr ? errstr : "<unknown>");
      return false;
    }
    else if (result < dst.size())
    {
      Error::SetStringFmt(error, "Only decompressed {} of {} bytes", result, dst.size());
      return false;
    }

    return true;
  }
  else [[unlikely]]
  {
    Error::SetStringView(error, "Unknown method.");
    return false;
  }
}

bool System::SaveState(const char* path, Error* error, bool backup_existing_save)
{
  if (!CheckSaveStateSupport(error))
    return false;

  Common::Timer save_timer;

  SaveStateBuffer buffer;
  if (!SaveStateToBuffer(&buffer, error, 256))
    return false;

  // TODO: Do this on a thread pool

  if (backup_existing_save && FileSystem::FileExists(path))
  {
    Error backup_error;
    const std::string backup_filename = Path::ReplaceExtension(path, "bak");
    if (!FileSystem::RenamePath(path, backup_filename.c_str(), &backup_error))
    {
      ERROR_LOG("Failed to rename save state backup '{}': {}", Path::GetFileName(backup_filename),
                backup_error.GetDescription());
    }
  }

  auto fp = FileSystem::CreateAtomicRenamedFile(path, "wb", error);
  if (!fp)
  {
    Error::AddPrefixFmt(error, "Cannot open '{}': ", Path::GetFileName(path));
    return false;
  }

  INFO_LOG("Saving state to '{}'...", path);

  if (!SaveStateBufferToFile(buffer, fp.get(), error, g_settings.save_state_compression))
  {
    FileSystem::DiscardAtomicRenamedFile(fp);
    return false;
  }

  Host::AddIconOSDMessage("save_state", ICON_EMOJI_FLOPPY_DISK,
                          fmt::format(TRANSLATE_FS("OSDMessage", "State saved to '{}'."), Path::GetFileName(path)),
                          5.0f);

  VERBOSE_LOG("Saving state took {:.2f} msec", save_timer.GetTimeMilliseconds());
  return true;
}

bool System::SaveStateToBuffer(SaveStateBuffer* buffer, Error* error, u32 screenshot_size /* = 256 */)
{
  if (!CheckSaveStateSupport(error))
    return false;

  if (IsShutdown()) [[unlikely]]
  {
    Error::SetStringView(error, "System is invalid.");
    return 0;
  }

  buffer->title = s_running_game_title;
  buffer->serial = s_running_game_serial;
  buffer->version = SAVE_STATE_VERSION;
  buffer->media_subimage_index = 0;

  if (CDROM::HasMedia())
  {
    buffer->media_path = CDROM::GetMediaFileName();
    buffer->media_subimage_index = CDROM::GetMedia()->HasSubImages() ? CDROM::GetMedia()->GetCurrentSubImage() : 0;
  }

  // save screenshot
  if (screenshot_size > 0)
  {
    // assume this size is the width
    GSVector4i screenshot_display_rect, screenshot_draw_rect;
    g_gpu->CalculateDrawRect(screenshot_size, screenshot_size, true, true, &screenshot_display_rect,
                             &screenshot_draw_rect);

    const u32 screenshot_width = static_cast<u32>(screenshot_display_rect.width());
    const u32 screenshot_height = static_cast<u32>(screenshot_display_rect.height());
    screenshot_draw_rect = screenshot_draw_rect.sub32(screenshot_display_rect.xyxy());
    screenshot_display_rect = screenshot_display_rect.sub32(screenshot_display_rect.xyxy());
    VERBOSE_LOG("Saving {}x{} screenshot for state", screenshot_width, screenshot_height);

    std::vector<u32> screenshot_buffer;
    u32 screenshot_stride;
    GPUTexture::Format screenshot_format;
    if (g_gpu->RenderScreenshotToBuffer(screenshot_width, screenshot_height, screenshot_display_rect,
                                        screenshot_draw_rect, false, &screenshot_buffer, &screenshot_stride,
                                        &screenshot_format) &&
        GPUTexture::ConvertTextureDataToRGBA8(screenshot_width, screenshot_height, screenshot_buffer, screenshot_stride,
                                              screenshot_format))
    {
      if (screenshot_stride != (screenshot_width * sizeof(u32)))
      {
        WARNING_LOG("Failed to save {}x{} screenshot for save state due to incorrect stride({})", screenshot_width,
                    screenshot_height, screenshot_stride);
      }
      else
      {
        if (g_gpu_device->UsesLowerLeftOrigin())
        {
          GPUTexture::FlipTextureDataRGBA8(screenshot_width, screenshot_height,
                                           reinterpret_cast<u8*>(screenshot_buffer.data()), screenshot_stride);
        }

        buffer->screenshot.SetPixels(screenshot_width, screenshot_height, std::move(screenshot_buffer));
      }
    }
    else
    {
      WARNING_LOG("Failed to save {}x{} screenshot for save state due to render/conversion failure", screenshot_width,
                  screenshot_height);
    }
  }

  // write data
  if (buffer->state_data.empty())
    buffer->state_data.resize(GetMaxSaveStateSize());

  g_gpu->RestoreDeviceContext();
  StateWrapper sw(buffer->state_data.span(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  if (!DoState(sw, nullptr, false, false))
  {
    Error::SetStringView(error, "DoState() failed");
    return false;
  }

  buffer->state_size = sw.GetPosition();
  return true;
}

bool System::SaveStateBufferToFile(const SaveStateBuffer& buffer, std::FILE* fp, Error* error,
                                   SaveStateCompressionMode compression)
{
  // Header gets rewritten below.
  SAVE_STATE_HEADER header = {};
  header.magic = SAVE_STATE_MAGIC;
  header.version = SAVE_STATE_VERSION;
  StringUtil::Strlcpy(header.title, s_running_game_title.c_str(), sizeof(header.title));
  StringUtil::Strlcpy(header.serial, s_running_game_serial.c_str(), sizeof(header.serial));

  u32 file_position = 0;
  DebugAssert(FileSystem::FTell64(fp) == static_cast<s64>(file_position));
  if (std::fwrite(&header, sizeof(header), 1, fp) != 1)
  {
    Error::SetErrno(error, "fwrite() for header failed: ", errno);
    return false;
  }
  file_position += sizeof(header);

  if (!buffer.media_path.empty())
  {
    DebugAssert(FileSystem::FTell64(fp) == static_cast<s64>(file_position));
    header.media_path_length = static_cast<u32>(buffer.media_path.length());
    header.offset_to_media_path = file_position;
    if (std::fwrite(buffer.media_path.data(), buffer.media_path.length(), 1, fp) != 1)
    {
      Error::SetErrno(error, "fwrite() for media path failed: ", errno);
      return false;
    }
    file_position += static_cast<u32>(buffer.media_path.length());
  }

  if (buffer.screenshot.IsValid())
  {
    DebugAssert(FileSystem::FTell64(fp) == static_cast<s64>(file_position));
    header.screenshot_width = buffer.screenshot.GetWidth();
    header.screenshot_height = buffer.screenshot.GetHeight();
    header.offset_to_screenshot = file_position;
    header.screenshot_compressed_size =
      CompressAndWriteStateData(fp,
                                std::span<const u8>(reinterpret_cast<const u8*>(buffer.screenshot.GetPixels()),
                                                    buffer.screenshot.GetPitch() * buffer.screenshot.GetHeight()),
                                compression, &header.screenshot_compression_type, error);
    if (header.screenshot_compressed_size == 0)
      return false;
    file_position += header.screenshot_compressed_size;
  }

  DebugAssert(buffer.state_size > 0);
  header.offset_to_data = file_position;
  header.data_uncompressed_size = static_cast<u32>(buffer.state_size);
  header.data_compressed_size = CompressAndWriteStateData(fp, buffer.state_data.cspan(0, buffer.state_size),
                                                          compression, &header.data_compression_type, error);
  if (header.data_compressed_size == 0)
    return false;

  INFO_LOG("Save state compression: screenshot {} => {} bytes, data {} => {} bytes",
           buffer.screenshot.GetPitch() * buffer.screenshot.GetHeight(), header.screenshot_compressed_size,
           buffer.state_size, header.data_compressed_size);

  if (!FileSystem::FSeek64(fp, 0, SEEK_SET, error))
    return false;

  // re-write header
  if (std::fwrite(&header, sizeof(header), 1, fp) != 1 || std::fflush(fp) != 0)
  {
    Error::SetErrno(error, "fwrite()/fflush() to rewrite header failed: {}", errno);
    return false;
  }

  return true;
}

u32 System::CompressAndWriteStateData(std::FILE* fp, std::span<const u8> src, SaveStateCompressionMode method,
                                      u32* header_type, Error* error)
{
  if (method == SaveStateCompressionMode::Uncompressed)
  {
    if (std::fwrite(src.data(), src.size(), 1, fp) != 1) [[unlikely]]
    {
      Error::SetStringFmt(error, "fwrite() failed: {}", errno);
      return 0;
    }

    *header_type = static_cast<u32>(SAVE_STATE_HEADER::CompressionType::None);
    return static_cast<u32>(src.size());
  }

  DynamicHeapArray<u8> buffer;
  u32 write_size;
  if (method >= SaveStateCompressionMode::DeflateLow && method <= SaveStateCompressionMode::DeflateHigh)
  {
    const size_t buffer_size = compressBound(static_cast<uLong>(src.size()));
    buffer.resize(buffer_size);

    uLongf compressed_size = static_cast<uLongf>(buffer_size);
    const int level =
      ((method == SaveStateCompressionMode::DeflateLow) ?
         Z_BEST_SPEED :
         ((method == SaveStateCompressionMode::DeflateHigh) ? Z_BEST_COMPRESSION : Z_DEFAULT_COMPRESSION));
    const int err = compress2(buffer.data(), &compressed_size, src.data(), static_cast<uLong>(src.size()), level);
    if (err != Z_OK) [[unlikely]]
    {
      Error::SetStringFmt(error, "compress2() failed: {}", err);
      return 0;
    }

    *header_type = static_cast<u32>(SAVE_STATE_HEADER::CompressionType::Deflate);
    write_size = static_cast<u32>(compressed_size);
  }
  else if (method >= SaveStateCompressionMode::ZstLow && method <= SaveStateCompressionMode::ZstHigh)
  {
    const size_t buffer_size = ZSTD_compressBound(src.size());
    buffer.resize(buffer_size);

    const int level =
      ((method == SaveStateCompressionMode::ZstLow) ? 1 : ((method == SaveStateCompressionMode::ZstHigh) ? 19 : 0));
    const size_t compressed_size = ZSTD_compress(buffer.data(), buffer_size, src.data(), src.size(), level);
    if (ZSTD_isError(compressed_size)) [[unlikely]]
    {
      const char* errstr = ZSTD_getErrorString(ZSTD_getErrorCode(compressed_size));
      Error::SetStringFmt(error, "ZSTD_compress() failed: {}", errstr ? errstr : "<unknown>");
      return 0;
    }

    *header_type = static_cast<u32>(SAVE_STATE_HEADER::CompressionType::Zstandard);
    write_size = static_cast<u32>(compressed_size);
  }
  else [[unlikely]]
  {
    Error::SetStringView(error, "Unknown method.");
    return 0;
  }

  if (std::fwrite(buffer.data(), write_size, 1, fp) != 1) [[unlikely]]
  {
    Error::SetStringFmt(error, "fwrite() failed: {}", errno);
    return 0;
  }

  return write_size;
}

float System::GetTargetSpeed()
{
  return s_target_speed;
}

float System::GetAudioNominalRate()
{
  return (s_throttler_enabled || s_syncing_to_host_with_vsync) ? s_target_speed : 1.0f;
}

void System::UpdatePerformanceCounters()
{
  const float frame_time = static_cast<float>(s_frame_timer.GetTimeMillisecondsAndReset());
  s_minimum_frame_time_accumulator =
    (s_minimum_frame_time_accumulator == 0.0f) ? frame_time : std::min(s_minimum_frame_time_accumulator, frame_time);
  s_average_frame_time_accumulator += frame_time;
  s_maximum_frame_time_accumulator = std::max(s_maximum_frame_time_accumulator, frame_time);
  s_frame_time_history[s_frame_time_history_pos] = frame_time;
  s_frame_time_history_pos = (s_frame_time_history_pos + 1) % NUM_FRAME_TIME_SAMPLES;

  // update fps counter
  const Common::Timer::Value now_ticks = Common::Timer::GetCurrentValue();
  const Common::Timer::Value ticks_diff = now_ticks - s_fps_timer.GetStartValue();
  const float time = static_cast<float>(Common::Timer::ConvertValueToSeconds(ticks_diff));
  if (time < PERFORMANCE_COUNTER_UPDATE_INTERVAL)
    return;

  const u32 frames_run = s_frame_number - s_last_frame_number;
  const float frames_runf = static_cast<float>(frames_run);
  const GlobalTicks global_tick_counter = GetGlobalTickCounter();

  // TODO: Make the math here less rubbish
  const double pct_divider =
    100.0 * (1.0 / ((static_cast<double>(ticks_diff) * static_cast<double>(Threading::GetThreadTicksPerSecond())) /
                    Common::Timer::GetFrequency() / 1000000000.0));
  const double time_divider = 1000.0 * (1.0 / static_cast<double>(Threading::GetThreadTicksPerSecond())) *
                              (1.0 / static_cast<double>(frames_runf));

  s_minimum_frame_time = std::exchange(s_minimum_frame_time_accumulator, 0.0f);
  s_average_frame_time = std::exchange(s_average_frame_time_accumulator, 0.0f) / frames_runf;
  s_maximum_frame_time = std::exchange(s_maximum_frame_time_accumulator, 0.0f);

  s_vps = static_cast<float>(frames_runf / time);
  s_last_frame_number = s_frame_number;
  s_fps = static_cast<float>(s_internal_frame_number - s_last_internal_frame_number) / time;
  s_last_internal_frame_number = s_internal_frame_number;
  s_speed = static_cast<float>(static_cast<double>(global_tick_counter - s_last_global_tick_counter) /
                               (static_cast<double>(g_ticks_per_second) * time)) *
            100.0f;
  s_last_global_tick_counter = global_tick_counter;

  const Threading::Thread* sw_thread = g_gpu->GetSWThread();
  const u64 cpu_time = s_cpu_thread_handle ? s_cpu_thread_handle.GetCPUTime() : 0;
  const u64 sw_time = sw_thread ? sw_thread->GetCPUTime() : 0;
  const u64 cpu_delta = cpu_time - s_last_cpu_time;
  const u64 sw_delta = sw_time - s_last_sw_time;
  s_last_cpu_time = cpu_time;
  s_last_sw_time = sw_time;

  s_cpu_thread_usage = static_cast<float>(static_cast<double>(cpu_delta) * pct_divider);
  s_cpu_thread_time = static_cast<float>(static_cast<double>(cpu_delta) * time_divider);
  s_sw_thread_usage = static_cast<float>(static_cast<double>(sw_delta) * pct_divider);
  s_sw_thread_time = static_cast<float>(static_cast<double>(sw_delta) * time_divider);

  if (s_media_capture)
    s_media_capture->UpdateCaptureThreadUsage(pct_divider, time_divider);

  s_fps_timer.ResetTo(now_ticks);

  if (g_gpu_device->IsGPUTimingEnabled())
  {
    s_average_gpu_time = s_accumulated_gpu_time / static_cast<float>(std::max(s_presents_since_last_update, 1u));
    s_gpu_usage = s_accumulated_gpu_time / (time * 10.0f);
  }
  s_accumulated_gpu_time = 0.0f;
  s_presents_since_last_update = 0;

  if (g_settings.display_show_gpu_stats)
    g_gpu->UpdateStatistics(frames_run);

  if (s_pre_frame_sleep)
    UpdatePreFrameSleepTime();

  VERBOSE_LOG("FPS: {:.2f} VPS: {:.2f} CPU: {:.2f} GPU: {:.2f} Average: {:.2f}ms Min: {:.2f}ms Max: {:.2f}ms", s_fps,
              s_vps, s_cpu_thread_usage, s_gpu_usage, s_average_frame_time, s_minimum_frame_time, s_maximum_frame_time);

  Host::OnPerformanceCountersUpdated();
}

void System::ResetPerformanceCounters()
{
  s_last_frame_number = s_frame_number;
  s_last_internal_frame_number = s_internal_frame_number;
  s_last_global_tick_counter = GetGlobalTickCounter();
  s_last_cpu_time = s_cpu_thread_handle ? s_cpu_thread_handle.GetCPUTime() : 0;
  if (const Threading::Thread* sw_thread = g_gpu->GetSWThread(); sw_thread)
    s_last_sw_time = sw_thread->GetCPUTime();
  else
    s_last_sw_time = 0;

  s_average_frame_time_accumulator = 0.0f;
  s_minimum_frame_time_accumulator = 0.0f;
  s_maximum_frame_time_accumulator = 0.0f;
  s_frame_timer.Reset();
  s_fps_timer.Reset();
  ResetThrottler();
}

void System::AccumulatePreFrameSleepTime()
{
  DebugAssert(s_pre_frame_sleep);

  s_max_active_frame_time = std::max(s_max_active_frame_time, s_last_active_frame_time);

  // in case one frame runs over, adjust to compensate
  const Common::Timer::Value max_sleep_time_for_this_frame =
    s_frame_period - std::min(s_last_active_frame_time, s_frame_period);
  if (max_sleep_time_for_this_frame < s_pre_frame_sleep_time)
  {
    s_pre_frame_sleep_time = Common::AlignDown(max_sleep_time_for_this_frame,
                                               static_cast<unsigned int>(Common::Timer::ConvertMillisecondsToValue(1)));
    DEV_LOG("Adjust pre-frame time to {} ms due to overrun of {} ms",
            Common::Timer::ConvertValueToMilliseconds(s_pre_frame_sleep_time),
            Common::Timer::ConvertValueToMilliseconds(s_last_active_frame_time));
  }
}

void System::UpdatePreFrameSleepTime()
{
  DebugAssert(s_pre_frame_sleep);

  const Common::Timer::Value expected_frame_time =
    s_max_active_frame_time + Common::Timer::ConvertMillisecondsToValue(g_settings.display_pre_frame_sleep_buffer);
  s_pre_frame_sleep_time = Common::AlignDown(s_frame_period - std::min(expected_frame_time, s_frame_period),
                                             static_cast<unsigned int>(Common::Timer::ConvertMillisecondsToValue(1)));
  DEV_LOG("Set pre-frame time to {} ms (expected frame time of {} ms)",
          Common::Timer::ConvertValueToMilliseconds(s_pre_frame_sleep_time),
          Common::Timer::ConvertValueToMilliseconds(expected_frame_time));

  s_max_active_frame_time = 0;
}

void System::FormatLatencyStats(SmallStringBase& str)
{
  AudioStream* audio_stream = SPU::GetOutputStream();
  const u32 audio_latency =
    AudioStream::GetMSForBufferSize(audio_stream->GetSampleRate(), audio_stream->GetBufferedFramesRelaxed());

  const double active_frame_time = std::ceil(Common::Timer::ConvertValueToMilliseconds(s_last_active_frame_time));
  const double pre_frame_time = std::ceil(Common::Timer::ConvertValueToMilliseconds(s_pre_frame_sleep_time));
  const double input_latency = std::ceil(
    Common::Timer::ConvertValueToMilliseconds(s_frame_period - s_pre_frame_sleep_time) -
    Common::Timer::ConvertValueToMilliseconds(static_cast<Common::Timer::Value>(s_runahead_frames) * s_frame_period));

  str.format("AF: {:.0f}ms | PF: {:.0f}ms | IL: {:.0f}ms | AL: {}ms", active_frame_time, pre_frame_time, input_latency,
             audio_latency);
}

void System::UpdateSpeedLimiterState()
{
  DebugAssert(IsValid());

  s_target_speed = s_turbo_enabled ?
                     g_settings.turbo_speed :
                     (s_fast_forward_enabled ? g_settings.fast_forward_speed : g_settings.emulation_speed);
  s_throttler_enabled = (s_target_speed != 0.0f);
  s_optimal_frame_pacing = (s_throttler_enabled && g_settings.display_optimal_frame_pacing);
  s_skip_presenting_duplicate_frames = s_throttler_enabled && g_settings.display_skip_presenting_duplicate_frames;
  s_pre_frame_sleep = s_optimal_frame_pacing && g_settings.display_pre_frame_sleep;
  s_can_sync_to_host = false;
  s_syncing_to_host = false;
  s_syncing_to_host_with_vsync = false;

  if (g_settings.sync_to_host_refresh_rate)
  {
    const float host_refresh_rate = g_gpu_device->GetWindowInfo().surface_refresh_rate;
    if (host_refresh_rate > 0.0f)
    {
      const float ratio = host_refresh_rate / System::GetThrottleFrequency();
      s_can_sync_to_host = (ratio >= 0.95f && ratio <= 1.05f);
      INFO_LOG("Refresh rate: Host={}hz Guest={}hz Ratio={} - {}", host_refresh_rate, System::GetThrottleFrequency(),
               ratio, s_can_sync_to_host ? "can sync" : "can't sync");

      s_syncing_to_host = (s_can_sync_to_host && g_settings.sync_to_host_refresh_rate && s_target_speed == 1.0f);
      if (s_syncing_to_host)
      {
        s_target_speed = ratio;

        // When syncing to host and using vsync, we don't need to sleep.
        s_syncing_to_host_with_vsync = g_settings.display_vsync;
        if (s_syncing_to_host_with_vsync)
        {
          INFO_LOG("Using host vsync for throttling.");
          s_throttler_enabled = false;
        }
      }
    }
  }

  VERBOSE_LOG("Target speed: {}%", s_target_speed * 100.0f);
  VERBOSE_LOG("Preset timing: {}", s_optimal_frame_pacing ? "consistent" : "immediate");

  // Update audio output.
  AudioStream* stream = SPU::GetOutputStream();
  stream->SetOutputVolume(GetAudioOutputVolume(), GetAudioOutputGainDB());
  stream->SetNominalRate(GetAudioNominalRate());

  UpdateThrottlePeriod();
  ResetThrottler();
  UpdateDisplayVSync();

  if (g_settings.increase_timer_resolution)
    SetTimerResolutionIncreased(s_throttler_enabled);
}

void System::UpdateDisplayVSync()
{
  static constexpr std::array<const char*, static_cast<size_t>(GPUVSyncMode::Count)> vsync_modes = {{
    "Disabled",
    "FIFO",
    "Mailbox",
  }};

  // Avoid flipping vsync on and off by manually throttling when vsync is on.
  const GPUVSyncMode vsync_mode = GetEffectiveVSyncMode();
  const bool allow_present_throttle = ShouldAllowPresentThrottle();
  VERBOSE_LOG("VSync: {}{}{}", vsync_modes[static_cast<size_t>(vsync_mode)],
              s_syncing_to_host_with_vsync ? " (for throttling)" : "",
              allow_present_throttle ? " (present throttle allowed)" : "");

  g_gpu_device->SetVSyncMode(vsync_mode, allow_present_throttle);
}

GPUVSyncMode System::GetEffectiveVSyncMode()
{
  // Vsync off => always disabled.
  if (!g_settings.display_vsync)
    return GPUVSyncMode::Disabled;

  // If there's no VM, or we're using vsync for timing, then we always use double-buffered (blocking).
  // Try to keep the same present mode whether we're running or not, since it'll avoid flicker.
  const bool valid_vm = (s_state != State::Shutdown && s_state != State::Stopping);
  if (s_can_sync_to_host || (!valid_vm && g_settings.sync_to_host_refresh_rate) ||
      g_settings.display_disable_mailbox_presentation)
  {
    return GPUVSyncMode::FIFO;
  }

  // For PAL games, we always want to triple buffer, because otherwise we'll be tearing.
  // Or for when we aren't using sync-to-host-refresh, to avoid dropping frames.
  // Allow present skipping when running outside of normal speed, if mailbox isn't supported.
  return GPUVSyncMode::Mailbox;
}

bool System::ShouldAllowPresentThrottle()
{
  const bool valid_vm = (s_state != State::Shutdown && s_state != State::Stopping);
  return !valid_vm || IsRunningAtNonStandardSpeed();
}

bool System::IsFastForwardEnabled()
{
  return s_fast_forward_enabled;
}

void System::SetFastForwardEnabled(bool enabled)
{
  if (!IsValid())
    return;

  s_fast_forward_enabled = enabled;
  UpdateSpeedLimiterState();
}

bool System::IsTurboEnabled()
{
  return s_turbo_enabled;
}

void System::SetTurboEnabled(bool enabled)
{
  if (!IsValid())
    return;

  s_turbo_enabled = enabled;
  UpdateSpeedLimiterState();
}

void System::SetRewindState(bool enabled)
{
  if (!System::IsValid())
    return;

  if (!SupportsSaveStates())
  {
    SetRewinding(false);
    return;
  }

  if (!g_settings.rewind_enable)
  {
    if (enabled)
      Host::AddKeyedOSDMessage("SetRewindState", TRANSLATE_STR("OSDMessage", "Rewinding is not enabled."), 5.0f);

    return;
  }

  if (Achievements::IsHardcoreModeActive() && enabled)
  {
    Achievements::ConfirmHardcoreModeDisableAsync("Rewinding", [](bool approved) {
      if (approved)
        SetRewindState(true);
    });
    return;
  }

  System::SetRewinding(enabled);
  UpdateSpeedLimiterState();
}

void System::DoFrameStep()
{
  if (!IsValid())
    return;

  if (Achievements::IsHardcoreModeActive())
  {
    Achievements::ConfirmHardcoreModeDisableAsync("Frame stepping", [](bool approved) {
      if (approved)
        DoFrameStep();
    });
    return;
  }

  s_frame_step_request = true;
  PauseSystem(false);
}

void System::DoToggleCheats()
{
  if (!System::IsValid())
    return;

  Host::AddKeyedOSDMessage(
    "CheatsUnderInvestigation",
    TRANSLATE_STR("OSDMessage", "Cheats are still under investigation and are not implemented yet. MAME cheat package compatibility is planned for a future update."),
    8.0f);
}

#if 0
// currently not used until EXP1 is implemented

bool SetExpansionROM(const char* filename)
{
  std::FILE* fp = FileSystem::OpenCFile(filename, "rb");
  if (!fp)
  {
    ERROR_LOG("Failed to open '{}'", Path::GetFileName(filename));
    return false;
  }

  std::fseek(fp, 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  std::vector<u8> data(size);
  if (std::fread(data.data(), size, 1, fp) != 1)
  {
    ERROR_LOG("Failed to read ROM data from '{}'", Path::GetFileName(filename))
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

  INFO_LOG("Loaded expansion ROM from '{}': {} bytes", Path::GetFileName(filename), size);
  Bus::SetExpansionROM(std::move(data));
  return true;
}

#endif

bool System::DumpRAM(const char* filename)
{
  if (!IsValid())
    return false;

  return FileSystem::WriteBinaryFile(filename, Bus::g_unprotected_ram, Bus::g_ram_size);
}

bool System::DumpVRAM(const char* filename)
{
  if (!IsValid())
    return false;

  g_gpu->RestoreDeviceContext();
  return g_gpu->DumpVRAMToFile(filename);
}

bool System::DumpSPURAM(const char* filename)
{
  if (!IsValid())
    return false;

  return FileSystem::WriteBinaryFile(filename, SPU::GetRAM().data(), SPU::RAM_SIZE);
}

void System::UpdateRunningGame(const std::string_view path, CDImage* image, bool booting)
{
  if (!booting && s_running_game_path == path)
    return;

  const std::string prev_serial = std::move(s_running_game_serial);

  s_running_game_path.clear();
  s_running_game_serial = {};
  s_running_game_title.clear();
  s_running_game_entry = nullptr;
  s_running_game_hash = 0;
  s_running_game_custom_title = false;

  if (!path.empty())
  {
    s_running_game_path = path;
    s_running_game_title = GameList::GetCustomTitleForPath(s_running_game_path);
    s_running_game_custom_title = !s_running_game_title.empty();

    // Resolve arcade identity directly from the universal database, then use the game list for custom metadata.
    const bool should_resolve_game_identity =
      (!image || image->GetTrack(1).mode != CDImage::TrackMode::Audio);
    if (should_resolve_game_identity)
    {
      if (const Arcade::Database::GameDefinition* const arcade_game =
            Arcade::Database::IdentifyArchive(s_running_game_path))
      {
        s_running_game_serial = arcade_game->id;
        if (s_running_game_title.empty())
          s_running_game_title = arcade_game->title;
      }

      const auto lock = GameList::GetLock();
      if (const GameList::Entry* const entry = GameList::GetEntryForPath(s_running_game_path))
      {
        if (s_running_game_serial.empty())
          s_running_game_serial = entry->serial;
        if (s_running_game_title.empty())
          s_running_game_title = entry->title;
      }

      if (!s_running_game_serial.empty())
        s_running_game_entry = GameDatabase::GetEntryForSerial(s_running_game_serial);
    }

    // Check for an audio CD. Those shouldn't set any title.
    if (image && image->GetTrack(1).mode != CDImage::TrackMode::Audio)
    {
      std::string id;
      GetGameDetailsFromImage(image, &id, &s_running_game_hash);

      s_running_game_entry = GameDatabase::GetEntryForGameDetails(id, s_running_game_hash);
      if (s_running_game_entry)
      {
        s_running_game_serial = s_running_game_entry->serial;
        if (s_running_game_title.empty())
          s_running_game_title = s_running_game_entry->title;
      }
      else
      {
        s_running_game_serial = std::move(id);
        if (s_running_game_title.empty())
          s_running_game_title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));
      }

      if (image->HasSubImages())
      {
        std::string image_title = image->GetMetadata("title");
        if (!image_title.empty())
        {
          s_running_game_title = std::move(image_title);
          s_running_game_custom_title = false;
        }
      }
    }
    // Direct launches may not have a populated game-list entry yet.
    if (s_running_game_title.empty() && should_resolve_game_identity)
      s_running_game_title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));
  }

  if (!booting)
    TextureReplacements::SetGameID(s_running_game_serial);

  if (booting)
    Achievements::ResetHardcoreMode(true);

  Achievements::GameChanged(s_running_game_path, image);

  UpdateGameSettingsLayer();
  ApplySettings(true);

  s_cheat_list.reset();
  if (g_settings.enable_cheats)
    LoadCheatList();

  if (s_running_game_serial != prev_serial)
    UpdateSessionTime(prev_serial);

  if (SaveStateSelectorUI::IsOpen())
    SaveStateSelectorUI::RefreshList(s_running_game_serial);
  else
    SaveStateSelectorUI::ClearList();

  UpdateRichPresence(booting);

  Host::OnGameChanged(s_running_game_path, s_running_game_serial, s_running_game_title);
}

bool System::CheckForSBIFile(CDImage* image, Error* error)
{
  if (!s_running_game_entry || !s_running_game_entry->HasTrait(GameDatabase::Trait::IsLibCryptProtected) || !image ||
      image->HasNonStandardSubchannel())
  {
    return true;
  }

  WARNING_LOG("SBI file missing but required for {} ({})", s_running_game_serial, s_running_game_title);

  if (Host::GetBoolSettingValue("CDROM", "AllowBootingWithoutSBIFile", false))
  {
    if (Host::ConfirmMessage(
          "Confirm Unsupported Configuration",
          LargeString::from_format(
            TRANSLATE_FS("System", "You are attempting to run a libcrypt protected game without an SBI file:\n\n{0}: "
                                   "{1}\n\nThe game will likely not run properly.\n\nPlease check the README for "
                                   "instructions on how to add an SBI file.\n\nDo you wish to continue?"),
            s_running_game_serial, s_running_game_title)))
    {
      return true;
    }
  }
#ifndef __ANDROID__
  Error::SetStringFmt(
    error,
    TRANSLATE_FS("System", "You are attempting to run a libcrypt protected game without an SBI file:\n\n{0}: "
                           "{1}\n\nYour dump is incomplete, you must add the SBI file to run this game. \n\nThe "
                           "name of the SBI file must match the name of the disc image."),
    s_running_game_serial, s_running_game_title);
#else
  // Shorter because no confirm messages.
  Error::SetStringView(error, "Missing SBI file.", "The selected game requires a SBI file to run properly.");
#endif

  return false;
}

bool System::HasCheatList()
{
  return static_cast<bool>(s_cheat_list);
}

CheatList* System::GetCheatList()
{
  return s_cheat_list.get();
}

void System::ApplyCheatCode(const CheatCode& code)
{
  // ArcadeDuck cheat support is intentionally gated until MAME cheat-package compatibility is implemented.
  return;
}

void System::SetCheatList(std::unique_ptr<CheatList> cheats)
{
  Assert(!IsShutdown());
  s_cheat_list = std::move(cheats);

  if (s_cheat_list && s_cheat_list->GetEnabledCodeCount() > 0)
  {
    Host::AddIconOSDMessage("CheatsLoadWarning", ICON_FA_EXCLAMATION_TRIANGLE,
                            TRANSLATE_PLURAL_STR("System", "%n cheat(s) are enabled. This may crash games.", "",
                                                 s_cheat_list->GetEnabledCodeCount()),
                            Host::OSD_WARNING_DURATION);
  }
  else
  {
    Host::RemoveKeyedOSDMessage("CheatsLoadWarning");
  }
}

void System::CheckForSettingsChanges(const Settings& old_settings)
{
  if (IsValid() &&
      (g_settings.gpu_renderer != old_settings.gpu_renderer ||
       g_settings.gpu_use_debug_device != old_settings.gpu_use_debug_device ||
       g_settings.gpu_threaded_presentation != old_settings.gpu_threaded_presentation ||
       g_settings.gpu_disable_shader_cache != old_settings.gpu_disable_shader_cache ||
       g_settings.gpu_disable_dual_source_blend != old_settings.gpu_disable_dual_source_blend ||
       g_settings.gpu_disable_framebuffer_fetch != old_settings.gpu_disable_framebuffer_fetch ||
       g_settings.gpu_disable_texture_buffers != old_settings.gpu_disable_texture_buffers ||
       g_settings.gpu_disable_texture_copy_to_self != old_settings.gpu_disable_texture_copy_to_self ||
       g_settings.gpu_disable_memory_import != old_settings.gpu_disable_memory_import ||
       g_settings.gpu_disable_raster_order_views != old_settings.gpu_disable_raster_order_views ||
       g_settings.display_exclusive_fullscreen_control != old_settings.display_exclusive_fullscreen_control))
  {
    // if debug device/threaded presentation change, we need to recreate the whole display
    const bool recreate_device =
      (g_settings.gpu_use_debug_device != old_settings.gpu_use_debug_device ||
       g_settings.gpu_threaded_presentation != old_settings.gpu_threaded_presentation ||
       g_settings.gpu_disable_shader_cache != old_settings.gpu_disable_shader_cache ||
       g_settings.gpu_disable_dual_source_blend != old_settings.gpu_disable_dual_source_blend ||
       g_settings.gpu_disable_framebuffer_fetch != old_settings.gpu_disable_framebuffer_fetch ||
       g_settings.gpu_disable_texture_buffers != old_settings.gpu_disable_texture_buffers ||
       g_settings.gpu_disable_texture_copy_to_self != old_settings.gpu_disable_texture_copy_to_self ||
       g_settings.gpu_disable_memory_import != old_settings.gpu_disable_memory_import ||
       g_settings.gpu_disable_raster_order_views != old_settings.gpu_disable_raster_order_views ||
       g_settings.display_exclusive_fullscreen_control != old_settings.display_exclusive_fullscreen_control);

    Host::AddIconOSDMessage("RendererSwitch", ICON_FA_PAINT_ROLLER,
                            fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {}{} GPU renderer."),
                                        Settings::GetRendererName(g_settings.gpu_renderer),
                                        g_settings.gpu_use_debug_device ? " (debug)" : ""),
                            Host::OSD_INFO_DURATION);
    RecreateGPU(g_settings.gpu_renderer, recreate_device);
  }

  if (IsValid())
  {
    ClearMemorySaveStates();

    if (g_settings.cpu_overclock_active != old_settings.cpu_overclock_active ||
        (g_settings.cpu_overclock_active &&
         (g_settings.cpu_overclock_numerator != old_settings.cpu_overclock_numerator ||
          g_settings.cpu_overclock_denominator != old_settings.cpu_overclock_denominator)))
    {
      UpdateOverclock();
    }

    if (g_settings.audio_backend != old_settings.audio_backend ||
        g_settings.audio_driver != old_settings.audio_driver ||
        g_settings.audio_output_device != old_settings.audio_output_device)
    {
      if (g_settings.audio_backend != old_settings.audio_backend)
      {
        Host::AddIconOSDMessage("AudioBackendSwitch", ICON_FA_HEADPHONES,
                                fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {} audio backend."),
                                            AudioStream::GetBackendDisplayName(g_settings.audio_backend)),
                                Host::OSD_INFO_DURATION);
      }

      SPU::RecreateOutputStream();
    }
    if (g_settings.audio_stream_parameters.stretch_mode != old_settings.audio_stream_parameters.stretch_mode)
      SPU::GetOutputStream()->SetStretchMode(g_settings.audio_stream_parameters.stretch_mode);
    if (g_settings.audio_stream_parameters != old_settings.audio_stream_parameters)
    {
      SPU::RecreateOutputStream();
      UpdateSpeedLimiterState();
    }

    if (g_settings.emulation_speed != old_settings.emulation_speed)
      UpdateThrottlePeriod();

    if (g_settings.cpu_execution_mode != old_settings.cpu_execution_mode ||
        g_settings.cpu_fastmem_mode != old_settings.cpu_fastmem_mode)
    {
      Host::AddIconOSDMessage("CPUExecutionModeSwitch", ICON_FA_MICROCHIP,
                              fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {} CPU execution mode."),
                                          TRANSLATE_SV("CPUExecutionMode", Settings::GetCPUExecutionModeDisplayName(
                                                                             g_settings.cpu_execution_mode))),
                              Host::OSD_INFO_DURATION);
      CPU::ExecutionModeChanged();
      if (old_settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
        CPU::CodeCache::Shutdown();
      if (g_settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
        CPU::CodeCache::Initialize();
    }

    if (CPU::CodeCache::IsUsingAnyRecompiler() &&
        (g_settings.cpu_recompiler_memory_exceptions != old_settings.cpu_recompiler_memory_exceptions ||
         g_settings.cpu_recompiler_block_linking != old_settings.cpu_recompiler_block_linking ||
         g_settings.cpu_recompiler_icache != old_settings.cpu_recompiler_icache ||
         g_settings.bios_tty_logging != old_settings.bios_tty_logging))
    {
      Host::AddIconOSDMessage("CPUFlushAllBlocks", ICON_FA_MICROCHIP,
                              TRANSLATE_STR("OSDMessage", "Recompiler options changed, flushing all blocks."),
                              Host::OSD_INFO_DURATION);
      CPU::ExecutionModeChanged();
    }
    else if (g_settings.cpu_execution_mode == CPUExecutionMode::Interpreter &&
             g_settings.bios_tty_logging != old_settings.bios_tty_logging)
    {
      CPU::UpdateDebugDispatcherFlag();
    }

    if (g_settings.enable_cheats != old_settings.enable_cheats)
    {
      if (g_settings.enable_cheats)
        LoadCheatList();
      else
        SetCheatList(nullptr);
    }

    UpdateVolume();

    if (g_settings.gpu_resolution_scale != old_settings.gpu_resolution_scale ||
        g_settings.gpu_multisamples != old_settings.gpu_multisamples ||
        g_settings.gpu_per_sample_shading != old_settings.gpu_per_sample_shading ||
        g_settings.gpu_use_thread != old_settings.gpu_use_thread ||
        g_settings.gpu_use_software_renderer_for_readbacks != old_settings.gpu_use_software_renderer_for_readbacks ||
        g_settings.gpu_fifo_size != old_settings.gpu_fifo_size ||
        g_settings.gpu_max_run_ahead != old_settings.gpu_max_run_ahead ||
        g_settings.gpu_true_color != old_settings.gpu_true_color ||
        g_settings.gpu_debanding != old_settings.gpu_debanding ||
        g_settings.gpu_scaled_dithering != old_settings.gpu_scaled_dithering ||
        g_settings.gpu_force_round_texcoords != old_settings.gpu_force_round_texcoords ||
        g_settings.gpu_accurate_blending != old_settings.gpu_accurate_blending ||
        g_settings.gpu_texture_filter != old_settings.gpu_texture_filter ||
        g_settings.gpu_sprite_texture_filter != old_settings.gpu_sprite_texture_filter ||
        g_settings.gpu_line_detect_mode != old_settings.gpu_line_detect_mode ||
        g_settings.gpu_disable_interlacing != old_settings.gpu_disable_interlacing ||
        g_settings.gpu_force_ntsc_timings != old_settings.gpu_force_ntsc_timings ||
        g_settings.gpu_downsample_mode != old_settings.gpu_downsample_mode ||
        g_settings.gpu_downsample_scale != old_settings.gpu_downsample_scale ||
        g_settings.gpu_wireframe_mode != old_settings.gpu_wireframe_mode ||
        g_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode ||
        g_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing ||
        g_settings.display_crop_mode != old_settings.display_crop_mode ||
        g_settings.display_aspect_ratio != old_settings.display_aspect_ratio ||
        g_settings.display_scaling != old_settings.display_scaling ||
        g_settings.display_show_gpu_usage != old_settings.display_show_gpu_usage ||
        g_settings.gpu_pgxp_enable != old_settings.gpu_pgxp_enable ||
        g_settings.gpu_pgxp_texture_correction != old_settings.gpu_pgxp_texture_correction ||
        g_settings.gpu_pgxp_color_correction != old_settings.gpu_pgxp_color_correction ||
        g_settings.gpu_pgxp_depth_buffer != old_settings.gpu_pgxp_depth_buffer ||
        g_settings.display_active_start_offset != old_settings.display_active_start_offset ||
        g_settings.display_active_end_offset != old_settings.display_active_end_offset ||
        g_settings.display_line_start_offset != old_settings.display_line_start_offset ||
        g_settings.display_line_end_offset != old_settings.display_line_end_offset ||
        g_settings.rewind_enable != old_settings.rewind_enable ||
        g_settings.runahead_frames != old_settings.runahead_frames)
    {
      g_gpu->UpdateSettings(old_settings);
      if (IsPaused())
        InvalidateDisplay();
    }

    if (g_settings.gpu_widescreen_hack != old_settings.gpu_widescreen_hack ||
        g_settings.display_aspect_ratio != old_settings.display_aspect_ratio ||
        (g_settings.display_aspect_ratio == DisplayAspectRatio::Custom &&
         (g_settings.display_aspect_ratio_custom_numerator != old_settings.display_aspect_ratio_custom_numerator ||
          g_settings.display_aspect_ratio_custom_denominator != old_settings.display_aspect_ratio_custom_denominator)))
    {
      GTE::UpdateAspectRatio();
    }

    if (g_settings.gpu_pgxp_enable != old_settings.gpu_pgxp_enable ||
        (g_settings.gpu_pgxp_enable && (g_settings.gpu_pgxp_culling != old_settings.gpu_pgxp_culling ||
                                        g_settings.gpu_pgxp_vertex_cache != old_settings.gpu_pgxp_vertex_cache ||
                                        g_settings.gpu_pgxp_cpu != old_settings.gpu_pgxp_cpu)))
    {
      if (old_settings.gpu_pgxp_enable)
        CPU::PGXP::Shutdown();

      if (g_settings.gpu_pgxp_enable)
        CPU::PGXP::Initialize();

      CPU::CodeCache::Reset();
    }

    if (g_settings.display_show_gpu_stats != old_settings.display_show_gpu_stats)
      g_gpu->ResetStatistics();

    if (g_settings.cdrom_readahead_sectors != old_settings.cdrom_readahead_sectors)
      CDROM::SetReadaheadSectors(g_settings.cdrom_readahead_sectors);

    if (g_settings.rewind_enable != old_settings.rewind_enable ||
        g_settings.rewind_save_frequency != old_settings.rewind_save_frequency ||
        g_settings.rewind_save_slots != old_settings.rewind_save_slots ||
        g_settings.runahead_frames != old_settings.runahead_frames)
    {
      UpdateMemorySaveStateSettings();
    }

    if (g_settings.texture_replacements.enable_vram_write_replacements !=
          old_settings.texture_replacements.enable_vram_write_replacements ||
        g_settings.texture_replacements.preload_textures != old_settings.texture_replacements.preload_textures)
    {
      TextureReplacements::Reload();
    }

    if (g_settings.audio_backend != old_settings.audio_backend ||
        g_settings.increase_timer_resolution != old_settings.increase_timer_resolution ||
        g_settings.emulation_speed != old_settings.emulation_speed ||
        g_settings.fast_forward_speed != old_settings.fast_forward_speed ||
        g_settings.display_optimal_frame_pacing != old_settings.display_optimal_frame_pacing ||
        g_settings.display_skip_presenting_duplicate_frames != old_settings.display_skip_presenting_duplicate_frames ||
        g_settings.display_pre_frame_sleep != old_settings.display_pre_frame_sleep ||
        g_settings.display_pre_frame_sleep_buffer != old_settings.display_pre_frame_sleep_buffer ||
        g_settings.display_vsync != old_settings.display_vsync ||
        g_settings.display_disable_mailbox_presentation != old_settings.display_disable_mailbox_presentation ||
        g_settings.sync_to_host_refresh_rate != old_settings.sync_to_host_refresh_rate)
    {
      UpdateSpeedLimiterState();
    }

    if (g_settings.inhibit_screensaver != old_settings.inhibit_screensaver)
    {
      if (g_settings.inhibit_screensaver)
        PlatformMisc::SuspendScreensaver();
      else
        PlatformMisc::ResumeScreensaver();
    }

    PostProcessing::UpdateSettings();

#ifdef ENABLE_GDB_SERVER
    if (g_settings.debugging.enable_gdb_server != old_settings.debugging.enable_gdb_server ||
        g_settings.debugging.gdb_server_port != old_settings.debugging.gdb_server_port)
    {
      GDBServer::Shutdown();
      if (g_settings.debugging.enable_gdb_server)
        GDBServer::Initialize(g_settings.debugging.gdb_server_port);
    }
#endif
  }
  else
  {
    if (g_gpu_device)
    {
      if (g_settings.display_vsync != old_settings.display_vsync ||
          g_settings.display_disable_mailbox_presentation != old_settings.display_disable_mailbox_presentation)
      {
        UpdateDisplayVSync();
      }
    }
  }

  if (g_gpu_device)
  {
    if (g_settings.display_osd_scale != old_settings.display_osd_scale)
      ImGuiManager::SetGlobalScale(g_settings.display_osd_scale / 100.0f);
    if (g_settings.display_show_osd_messages != old_settings.display_show_osd_messages)
      ImGuiManager::SetShowOSDMessages(g_settings.display_show_osd_messages);
  }

  Achievements::UpdateSettings(old_settings);

  FullscreenUI::CheckForConfigChanges(old_settings);

#ifdef ENABLE_DISCORD_PRESENCE
  if (g_settings.enable_discord_presence != old_settings.enable_discord_presence)
  {
    if (g_settings.enable_discord_presence)
      InitializeDiscordPresence();
    else
      ShutdownDiscordPresence();
  }
#endif

#ifdef ENABLE_PINE_SERVER
  if (g_settings.pine_enable != old_settings.pine_enable || g_settings.pine_slot != old_settings.pine_slot)
  {
    PINEServer::Shutdown();
    if (g_settings.pine_enable)
      PINEServer::Initialize(g_settings.pine_slot);
    else
      ReleaseSocketMultiplexer();
  }
#endif

  if (g_settings.export_shared_memory != old_settings.export_shared_memory) [[unlikely]]
  {
    Error error;
    if (!Bus::ReallocateMemoryMap(g_settings.export_shared_memory, &error)) [[unlikely]]
    {
      ERROR_LOG(error.GetDescription());
      Panic("Failed to reallocate memory map. The log may contain more information.");
    }
  }

  if (g_settings.log_level != old_settings.log_level || g_settings.log_filter != old_settings.log_filter ||
      g_settings.log_timestamps != old_settings.log_timestamps ||
      g_settings.enable_debug_logging != old_settings.enable_debug_logging ||
      g_settings.log_to_console != old_settings.log_to_console ||
      g_settings.log_to_debug != old_settings.log_to_debug || g_settings.log_to_window != old_settings.log_to_window ||
      g_settings.log_to_file != old_settings.log_to_file)
  {
    g_settings.UpdateLogSettings();
  }
}

void System::WarnAboutUnsafeSettings()
{
  LargeString messages;
  auto append = [&messages](const char* icon, std::string_view msg) { messages.append_format("{} {}\n", icon, msg); };

  if (!g_settings.disable_all_enhancements)
  {
    if (g_settings.cpu_overclock_active)
    {
      append(ICON_EMOJI_WARNING,
             SmallString::from_format(
               TRANSLATE_FS("System", "CPU clock speed is set to {}% ({} / {}). This may crash games."),
               g_settings.GetCPUOverclockPercent(), g_settings.cpu_overclock_numerator,
               g_settings.cpu_overclock_denominator));
    }
    if (g_settings.cdrom_read_speedup > 1)
    {
      append(ICON_EMOJI_WARNING,
             SmallString::from_format(
               TRANSLATE_FS("System", "CD-ROM read speedup set to {}x (effective speed {}x). This may crash games."),
               g_settings.cdrom_read_speedup, g_settings.cdrom_read_speedup * 2));
    }
    if (g_settings.cdrom_seek_speedup != 1)
    {
      append(ICON_EMOJI_WARNING,
             SmallString::from_format(TRANSLATE_FS("System", "CD-ROM seek speedup set to {}. This may crash games."),
                                      (g_settings.cdrom_seek_speedup == 0) ?
                                        TinyString(TRANSLATE_SV("System", "Instant")) :
                                        TinyString::from_format("{}x", g_settings.cdrom_seek_speedup)));
    }
    if (g_settings.gpu_force_ntsc_timings)
    {
      append(ICON_FA_TV, TRANSLATE_SV("System", "Force NTSC timings is enabled. Games may run at incorrect speeds."));
    }
    if (!g_settings.IsUsingSoftwareRenderer())
    {
      if (g_settings.gpu_multisamples != 1)
      {
        append(ICON_EMOJI_WARNING,
               TRANSLATE_SV("System", "Multisample anti-aliasing is enabled, some games may not render correctly."));
      }
      if (g_settings.gpu_resolution_scale > 1 && g_settings.gpu_force_round_texcoords)
      {
        append(
          ICON_EMOJI_WARNING,
          TRANSLATE_SV("System", "Round upscaled texture coordinates is enabled. This may cause rendering errors."));
      }
    }
    if (g_settings.enable_8mb_ram)
    {
      append(ICON_EMOJI_WARNING,
        TRANSLATE_SV("System", "8MB RAM is enabled, this may be incompatible with some games."));
    }
  }
  else
  {
    append(ICON_FA_COGS, TRANSLATE_SV("System", "All enhancements are currently disabled."));
  }

  if (!g_settings.apply_compatibility_settings)
  {
    append(ICON_EMOJI_WARNING,
           TRANSLATE_STR("System", "Compatibility settings are not enabled. Some games may not function correctly."));
  }

  if (!messages.empty())
  {
    if (messages.back() == '\n')
      messages.pop_back();

    LogUnsafeSettingsToConsole(messages);
    Host::AddKeyedOSDMessage("performance_settings_warning", std::string(messages.view()), Host::OSD_WARNING_DURATION);
  }
  else
  {
    Host::RemoveKeyedOSDMessage("performance_settings_warning");
  }
}

void System::LogUnsafeSettingsToConsole(const SmallStringBase& messages)
{
  // a not-great way of getting rid of the icons for the console message
  LargeString console_messages = messages;
  for (;;)
  {
    const s32 pos = console_messages.find("\xef");
    if (pos >= 0)
    {
      console_messages.erase(pos, 3);
      console_messages.insert(pos, "[Unsafe Settings]");
    }
    else
    {
      break;
    }
  }
  WARNING_LOG(console_messages);
}

void System::CalculateRewindMemoryUsage(u32 num_saves, u32 resolution_scale, u64* ram_usage, u64* vram_usage)
{
  const u64 real_resolution_scale = std::max<u64>(g_settings.gpu_resolution_scale, 1u);
  *ram_usage = GetMaxSaveStateSize() * static_cast<u64>(num_saves);
  *vram_usage = ((VRAM_WIDTH * real_resolution_scale) * (g_vram_height * real_resolution_scale) * 4) *
                static_cast<u64>(g_settings.gpu_multisamples) * static_cast<u64>(num_saves);
}

void System::ClearMemorySaveStates()
{
  s_rewind_states.clear();
  s_runahead_states.clear();
}

void System::UpdateMemorySaveStateSettings()
{
  ClearMemorySaveStates();

  if (!SupportsSaveStates())
  {
    s_memory_saves_enabled = false;
    s_rewind_save_frequency = -1;
    s_rewind_save_counter = -1;
    s_rewind_load_frequency = -1;
    s_rewind_load_counter = -1;
    s_rewinding_first_save = true;
    s_runahead_frames = 0;
    s_runahead_replay_frames = 0;
    s_runahead_replay_pending = false;
    return;
  }

  s_memory_saves_enabled = g_settings.rewind_enable;

  if (g_settings.rewind_enable)
  {
    s_rewind_save_frequency = static_cast<s32>(std::ceil(g_settings.rewind_save_frequency * s_throttle_frequency));
    s_rewind_save_counter = 0;

    u64 ram_usage, vram_usage;
    CalculateRewindMemoryUsage(g_settings.rewind_save_slots, g_settings.gpu_resolution_scale, &ram_usage, &vram_usage);
    INFO_LOG("Rewind is enabled, saving every {} frames, with {} slots and {}MB RAM and {}MB VRAM usage",
             std::max(s_rewind_save_frequency, 1), g_settings.rewind_save_slots, ram_usage / 1048576,
             vram_usage / 1048576);
  }
  else
  {
    s_rewind_save_frequency = -1;
    s_rewind_save_counter = -1;
  }

  s_rewind_load_frequency = -1;
  s_rewind_load_counter = -1;

  s_runahead_frames = g_settings.runahead_frames;
  s_runahead_replay_pending = false;
  if (s_runahead_frames > 0)
    INFO_LOG("Runahead is active with {} frames", s_runahead_frames);
}

bool System::LoadMemoryState(const MemorySaveState& mss)
{
  if (!SupportsSaveStates())
    return false;

  StateWrapper sw(mss.state_data.cspan(), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  GPUTexture* host_texture = mss.vram_texture.get();
  if (!DoState(sw, &host_texture, true, true)) [[unlikely]]
  {
    Host::ReportErrorAsync("Error", "Failed to load memory save state, resetting.");
    ResetSystem();
    return false;
  }

  return true;
}

bool System::SaveMemoryState(MemorySaveState* mss)
{
  if (!SupportsSaveStates())
    return false;

  if (mss->state_data.empty())
    mss->state_data.resize(GetMaxSaveStateSize());

  GPUTexture* host_texture = mss->vram_texture.release();
  StateWrapper sw(mss->state_data.span(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  if (!DoState(sw, &host_texture, false, true))
  {
    ERROR_LOG("Failed to create rewind state.");
    delete host_texture;
    return false;
  }

#ifdef PROFILE_MEMORY_SAVE_STATES
  mss->state_size = sw.GetPosition();
#endif

  mss->vram_texture.reset(host_texture);
  return true;
}

bool System::SaveRewindState()
{
  if (!SupportsSaveStates())
    return false;

#ifdef PROFILE_MEMORY_SAVE_STATES
  Common::Timer save_timer;
#endif

  // try to reuse the frontmost slot
  const u32 save_slots = g_settings.rewind_save_slots;
  MemorySaveState mss;
  while (s_rewind_states.size() >= save_slots)
  {
    mss = std::move(s_rewind_states.front());
    s_rewind_states.pop_front();
  }

  if (!SaveMemoryState(&mss))
    return false;

  s_rewind_states.push_back(std::move(mss));

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("Saved rewind state ({} bytes, took {:.4f} ms)", s_rewind_states.back().state_size,
          save_timer.GetTimeMilliseconds());
#endif

  return true;
}

bool System::LoadRewindState(u32 skip_saves /*= 0*/, bool consume_state /*=true */)
{
  if (!SupportsSaveStates())
    return false;

  while (skip_saves > 0 && !s_rewind_states.empty())
  {
    g_gpu_device->RecycleTexture(std::move(s_rewind_states.back().vram_texture));
    s_rewind_states.pop_back();
    skip_saves--;
  }

  if (s_rewind_states.empty())
    return false;

#ifdef PROFILE_MEMORY_SAVE_STATES
  Common::Timer load_timer;
#endif

  if (!LoadMemoryState(s_rewind_states.back()))
    return false;

  if (consume_state)
    s_rewind_states.pop_back();

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("Rewind load took {:.4f} ms", load_timer.GetTimeMilliseconds());
#endif

  return true;
}

bool System::IsRewinding()
{
  return (s_rewind_load_frequency >= 0);
}

void System::SetRewinding(bool enabled)
{
  if (enabled && !SupportsSaveStates())
    return;

  if (enabled)
  {
    const bool was_enabled = IsRewinding();

    // Try to rewind at the replay speed, or one per second maximum.
    const float load_frequency = std::min(g_settings.rewind_save_frequency, 1.0f);
    s_rewind_load_frequency = static_cast<s32>(std::ceil(load_frequency * s_throttle_frequency));
    s_rewind_load_counter = 0;

    if (!was_enabled && s_system_executing)
      s_system_interrupted = true;
  }
  else
  {
    s_rewind_load_frequency = -1;
    s_rewind_load_counter = -1;
    s_rewinding_first_save = true;
  }
}

void System::DoRewind()
{
  if (s_rewind_load_counter == 0)
  {
    const u32 skip_saves = BoolToUInt32(!s_rewinding_first_save);
    s_rewinding_first_save = false;
    LoadRewindState(skip_saves, false);
    ResetPerformanceCounters();
    s_rewind_load_counter = s_rewind_load_frequency;
  }
  else
  {
    s_rewind_load_counter--;
  }

  InvalidateDisplay();
  Host::PumpMessagesOnCPUThread();
  Internal::IdlePollUpdate();

  Throttle(Common::Timer::GetCurrentValue());
}

void System::SaveRunaheadState()
{
  if (!SupportsSaveStates())
    return;

  // try to reuse the frontmost slot
  MemorySaveState mss;
  while (s_runahead_states.size() >= s_runahead_frames)
  {
    mss = std::move(s_runahead_states.front());
    s_runahead_states.pop_front();
  }

  if (!SaveMemoryState(&mss))
  {
    ERROR_LOG("Failed to save runahead state.");
    return;
  }

  s_runahead_states.push_back(std::move(mss));
}

bool System::DoRunahead()
{
  if (!SupportsSaveStates())
    return false;

#ifdef PROFILE_MEMORY_SAVE_STATES
  static Common::Timer replay_timer;
#endif

  if (s_runahead_replay_pending)
  {
#ifdef PROFILE_MEMORY_SAVE_STATES
    DEV_LOG("runahead starting at frame {}", s_frame_number);
    replay_timer.Reset();
#endif

    // we need to replay and catch up - load the state,
    s_runahead_replay_pending = false;
    if (s_runahead_states.empty() || !LoadMemoryState(s_runahead_states.front()))
    {
      s_runahead_states.clear();
      return false;
    }

    // figure out how many frames we need to run to catch up
    s_runahead_replay_frames = static_cast<u32>(s_runahead_states.size());

    // and throw away all the states, forcing us to catch up below
    s_runahead_states.clear();

    // run the frames with no audio
    SPU::SetAudioOutputMuted(true);

#ifdef PROFILE_MEMORY_SAVE_STATES
    VERBOSE_LOG("Rewound to frame {}, took {:.2f} ms", s_frame_number, replay_timer.GetTimeMilliseconds());
#endif

    // we don't want to save the frame we just loaded. but we are "one frame ahead", because the frame we just tossed
    // was never saved, so return but don't decrement the counter
    InterruptExecution();
    CheckForAndExitExecution();
    return true;
  }
  else if (s_runahead_replay_frames == 0)
  {
    return false;
  }

  s_runahead_replay_frames--;
  if (s_runahead_replay_frames > 0)
  {
    // keep running ahead
    SaveRunaheadState();
    return true;
  }

#ifdef PROFILE_MEMORY_SAVE_STATES
  VERBOSE_LOG("Running {} frames to catch up took {:.2f} ms", s_runahead_frames, replay_timer.GetTimeMilliseconds());
#endif

  // we're all caught up. this frame gets saved in DoMemoryStates().
  SPU::SetAudioOutputMuted(false);

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("runahead ending at frame {}, took {:.2f} ms", s_frame_number, replay_timer.GetTimeMilliseconds());
#endif

  return false;
}

void System::SetRunaheadReplayFlag()
{
  if (!SupportsSaveStates() || s_runahead_frames == 0 || s_runahead_states.empty())
    return;

#ifdef PROFILE_MEMORY_SAVE_STATES
  DEV_LOG("Runahead rewind pending...");
#endif

  s_runahead_replay_pending = true;
}

void System::ShutdownSystem(bool save_resume_state)
{
  if (!IsValid())
    return;

  if (save_resume_state && SupportsSaveStates())
  {
    Error error;
    if (!SaveResumeState(&error))
    {
      Host::ReportErrorAsync(
        TRANSLATE_SV("System", "Error"),
        fmt::format(TRANSLATE_FS("System", "Failed to save resume state: {}"), error.GetDescription()));
    }
  }

  s_state = State::Stopping;
  if (!s_system_executing)
    DestroySystem();
}

bool System::CanUndoLoadState()
{
  return SupportsSaveStates() && s_undo_load_state.has_value();
}

std::optional<ExtendedSaveStateInfo> System::GetUndoSaveStateInfo()
{
  std::optional<ExtendedSaveStateInfo> ssi;
  if (SupportsSaveStates() && s_undo_load_state.has_value())
  {
    ssi.emplace();
    ssi->title = s_undo_load_state->title;
    ssi->serial = s_undo_load_state->serial;
    ssi->media_path = s_undo_load_state->media_path;
    ssi->screenshot = s_undo_load_state->screenshot;
    ssi->timestamp = 0;
  }

  return ssi;
}

bool System::UndoLoadState()
{
  if (!SupportsSaveStates() || !s_undo_load_state.has_value())
    return false;

  Assert(IsValid());

  Error error;
  if (!LoadStateFromBuffer(s_undo_load_state.value(), &error, true))
  {
    Host::ReportErrorAsync("Error",
                           fmt::format("Failed to load undo state, resetting system:\n", error.GetDescription()));
    s_undo_load_state.reset();
    ResetSystem();
    return false;
  }

  INFO_LOG("Loaded undo save state.");
  s_undo_load_state.reset();
  return true;
}

bool System::SaveUndoLoadState()
{
  if (!SupportsSaveStates())
    return false;

  if (!s_undo_load_state.has_value())
    s_undo_load_state.emplace();

  Error error;
  if (!SaveStateToBuffer(&s_undo_load_state.value(), &error))
  {
    Host::AddOSDMessage(
      fmt::format(TRANSLATE_FS("OSDMessage", "Failed to save undo load state:\n{}"), error.GetDescription()),
      Host::OSD_CRITICAL_ERROR_DURATION);
    s_undo_load_state.reset();
    return false;
  }

  INFO_LOG("Saved undo load state: {} bytes", s_undo_load_state->state_size);
  return true;
}

bool System::IsRunningAtNonStandardSpeed()
{
  if (!IsValid())
    return false;

  return (s_target_speed != 1.0f && !s_syncing_to_host);
}

s32 System::GetAudioOutputVolume()
{
  return g_settings.GetAudioOutputVolume(IsRunningAtNonStandardSpeed());
}

s32 System::GetAudioOutputGainDB()
{
  static constexpr s32 ARCADE_CABINET_BASELINE_GAIN_DB = 12;
  return IsArcadeVideoTiming() ?
           (ARCADE_CABINET_BASELINE_GAIN_DB + g_settings.audio_arcade_gain_adjustment_db) :
           0;
}

void System::UpdateVolume()
{
  if (!IsValid())
    return;

  SPU::GetOutputStream()->SetOutputVolume(GetAudioOutputVolume(), GetAudioOutputGainDB());
}

bool System::SaveScreenshot(const char* filename, DisplayScreenshotMode mode, DisplayScreenshotFormat format,
                            u8 quality, bool compress_on_thread)
{
  if (!System::IsValid())
    return false;

  std::string auto_filename;
  if (!filename)
  {
    const std::string sanitized_name = Path::SanitizeFileName(System::GetGameTitle());
    const char* extension = Settings::GetDisplayScreenshotFormatExtension(format);
    std::string basename;
    if (sanitized_name.empty())
      basename = fmt::format("{}", GetTimestampStringForFileName());
    else
      basename = fmt::format("{} {}", sanitized_name, GetTimestampStringForFileName());

    auto_filename = fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}.{}", EmuFolders::Screenshots, basename, extension);

    // handle quick screenshots to the same filename
    u32 next_suffix = 1;
    while (FileSystem::FileExists(auto_filename.c_str()))
    {
      auto_filename = fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{} ({}).{}", EmuFolders::Screenshots, basename,
                                  next_suffix, extension);
      next_suffix++;
    }

    filename = auto_filename.c_str();
  }

  return g_gpu->RenderScreenshotToFile(filename, mode, quality, compress_on_thread, true);
}

static std::string_view GetCaptureTypeForMessage(bool capture_video, bool capture_audio)
{
  return capture_video ? (capture_audio ? TRANSLATE_SV("System", "capturing audio and video") :
                                          TRANSLATE_SV("System", "capturing video")) :
                         TRANSLATE_SV("System", "capturing audio");
}

MediaCapture* System::GetMediaCapture()
{
  return s_media_capture.get();
}

std::string System::GetNewMediaCapturePath(const std::string_view title, const std::string_view container)
{
  const std::string sanitized_name = Path::SanitizeFileName(title);
  std::string path;
  if (sanitized_name.empty())
  {
    path = Path::Combine(EmuFolders::Videos, fmt::format("{}.{}", GetTimestampStringForFileName(), container));
  }
  else
  {
    path = Path::Combine(EmuFolders::Videos,
                         fmt::format("{} {}.{}", sanitized_name, GetTimestampStringForFileName(), container));
  }

  return path;
}

bool System::StartMediaCapture(std::string path)
{
  const bool capture_video = Host::GetBoolSettingValue("MediaCapture", "VideoCapture", true);
  const bool capture_audio = Host::GetBoolSettingValue("MediaCapture", "AudioCapture", true);
  return StartMediaCapture(std::move(path), capture_video, capture_audio);
}

bool System::StartMediaCapture(std::string path, bool capture_video, bool capture_audio)
{
  if (!IsValid())
    return false;

  if (s_media_capture)
    StopMediaCapture();

  // Need to work out the size.
  u32 capture_width =
    Host::GetUIntSettingValue("MediaCapture", "VideoWidth", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH);
  u32 capture_height =
    Host::GetUIntSettingValue("MediaCapture", "VideoHeight", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT);
  const GPUTexture::Format capture_format =
    g_gpu_device->HasSurface() ? g_gpu_device->GetWindowFormat() : GPUTexture::Format::RGBA8;
  const float fps = System::GetThrottleFrequency();
  if (capture_video)
  {
    // TODO: This will be a mess with GPU thread.
    if (Host::GetBoolSettingValue("MediaCapture", "VideoAutoSize", false))
    {
      GSVector4i unused_display_rect, unused_draw_rect;
      g_gpu->CalculateScreenshotSize(DisplayScreenshotMode::InternalResolution, &capture_width, &capture_height,
                                     &unused_display_rect, &unused_draw_rect);
    }

    MediaCapture::AdjustVideoSize(&capture_width, &capture_height);
  }

  // TODO: Render anamorphic capture instead?
  constexpr float aspect = 1.0f;

  if (path.empty())
  {
    path =
      GetNewMediaCapturePath(GetGameTitle(), Host::GetStringSettingValue("MediaCapture", "Container",
                                                                         Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER));
  }

  const MediaCaptureBackend backend =
    MediaCapture::ParseBackendName(
      Host::GetStringSettingValue("MediaCapture", "Backend",
                                  MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND))
        .c_str())
      .value_or(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);

  Error error;
  s_media_capture = MediaCapture::Create(backend, &error);
  if (!s_media_capture ||
      !s_media_capture->BeginCapture(
        fps, aspect, capture_width, capture_height, capture_format, SPU::SAMPLE_RATE, std::move(path), capture_video,
        Host::GetSmallStringSettingValue("MediaCapture", "VideoCodec"),
        Host::GetUIntSettingValue("MediaCapture", "VideoBitrate", Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE),
        Host::GetBoolSettingValue("MediaCapture", "VideoCodecUseArgs", false) ?
          Host::GetStringSettingValue("MediaCapture", "AudioCodecArgs") :
          std::string(),
        capture_audio, Host::GetSmallStringSettingValue("MediaCapture", "AudioCodec"),
        Host::GetUIntSettingValue("MediaCapture", "AudioBitrate", Settings::DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE),
        Host::GetBoolSettingValue("MediaCapture", "AudioCodecUseArgs", false) ?
          Host::GetStringSettingValue("MediaCapture", "AudioCodecArgs") :
          std::string(),
        &error))
  {
    Host::AddIconOSDMessage(
      "MediaCapture", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("System", "Failed to create media capture: {0}"), error.GetDescription()),
      Host::OSD_ERROR_DURATION);
    s_media_capture.reset();
    Host::OnMediaCaptureStopped();
    return false;
  }

  Host::AddIconOSDMessage(
    "MediaCapture", ICON_FA_CAMERA,
    fmt::format(TRANSLATE_FS("System", "Starting {0} to '{1}'."),
                GetCaptureTypeForMessage(s_media_capture->IsCapturingVideo(), s_media_capture->IsCapturingAudio()),
                Path::GetFileName(s_media_capture->GetPath())),
    Host::OSD_INFO_DURATION);

  Host::OnMediaCaptureStarted();
  return true;
}

void System::StopMediaCapture()
{
  if (!s_media_capture)
    return;

  const bool was_capturing_audio = s_media_capture->IsCapturingAudio();
  const bool was_capturing_video = s_media_capture->IsCapturingVideo();

  Error error;
  if (s_media_capture->EndCapture(&error))
  {
    Host::AddIconOSDMessage("MediaCapture", ICON_FA_CAMERA,
                            fmt::format(TRANSLATE_FS("System", "Stopped {0} to '{1}'."),
                                        GetCaptureTypeForMessage(was_capturing_video, was_capturing_audio),
                                        Path::GetFileName(s_media_capture->GetPath())),
                            Host::OSD_INFO_DURATION);
  }
  else
  {
    Host::AddIconOSDMessage(
      "MediaCapture", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("System", "Stopped {0}: {1}."),
                  GetCaptureTypeForMessage(s_media_capture->IsCapturingVideo(), s_media_capture->IsCapturingAudio()),
                  error.GetDescription()),
      Host::OSD_INFO_DURATION);
  }
  s_media_capture.reset();

  Host::OnMediaCaptureStopped();
}

std::string System::GetGameSaveStateFileName(std::string_view serial, s32 slot)
{
  if (slot < 0)
    return Path::Combine(EmuFolders::SaveStates, fmt::format("{}_resume.sav", serial));
  else
    return Path::Combine(EmuFolders::SaveStates, fmt::format("{}_{}.sav", serial, slot));
}

std::string System::GetGlobalSaveStateFileName(s32 slot)
{
  if (slot < 0)
    return Path::Combine(EmuFolders::SaveStates, "resume.sav");
  else
    return Path::Combine(EmuFolders::SaveStates, fmt::format("savestate_{}.sav", slot));
}

std::vector<SaveStateInfo> System::GetAvailableSaveStates(const char* serial)
{
  std::vector<SaveStateInfo> si;
  std::string path;

  auto add_path = [&si](std::string path, s32 slot, bool global) {
    FILESYSTEM_STAT_DATA sd;
    if (!FileSystem::StatFile(path.c_str(), &sd))
      return;

    si.push_back(SaveStateInfo{std::move(path), sd.ModificationTime, static_cast<s32>(slot), global});
  };

  if (serial && std::strlen(serial) > 0)
  {
    add_path(GetGameSaveStateFileName(serial, -1), -1, false);
    for (s32 i = 1; i <= PER_GAME_SAVE_STATE_SLOTS; i++)
      add_path(GetGameSaveStateFileName(serial, i), i, false);
  }

  for (s32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
    add_path(GetGlobalSaveStateFileName(i), i, true);

  return si;
}

std::optional<SaveStateInfo> System::GetSaveStateInfo(const char* serial, s32 slot)
{
  const bool global = (!serial || serial[0] == 0);
  std::string path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(serial, slot);

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return std::nullopt;

  return SaveStateInfo{std::move(path), sd.ModificationTime, slot, global};
}

std::optional<ExtendedSaveStateInfo> System::GetExtendedSaveStateInfo(const char* path)
{
  if (IsValid() && !SupportsSaveStates())
    return std::nullopt;

  std::optional<ExtendedSaveStateInfo> ssi;

  Error error;
  auto fp = FileSystem::OpenManagedCFile(path, "rb", &error);
  if (fp)
  {
    ssi.emplace();

    SaveStateBuffer buffer;
    if (LoadStateBufferFromFile(&buffer, fp.get(), &error, true, true, true, false)) [[likely]]
    {
      ssi->title = std::move(buffer.title);
      ssi->serial = std::move(buffer.serial);
      ssi->media_path = std::move(buffer.media_path);
      ssi->screenshot = std::move(buffer.screenshot);

      FILESYSTEM_STAT_DATA sd;
      ssi->timestamp = FileSystem::StatFile(fp.get(), &sd) ? sd.ModificationTime : 0;
    }
    else
    {
      ssi->title = error.GetDescription();
      ssi->timestamp = 0;
    }
  }

  return ssi;
}

void System::DeleteSaveStates(const char* serial, bool resume)
{
  const std::vector<SaveStateInfo> states(GetAvailableSaveStates(serial));
  for (const SaveStateInfo& si : states)
  {
    if (si.global || (!resume && si.slot < 0))
      continue;

    INFO_LOG("Removing save state '{}'", Path::GetFileName(si.path));

    Error error;
    if (!FileSystem::DeleteFile(si.path.c_str(), &error)) [[unlikely]]
      ERROR_LOG("Failed to delete save state file '{}': {}", Path::GetFileName(si.path), error.GetDescription());
  }
}

std::string System::GetMostRecentResumeSaveStatePath()
{
  std::vector<FILESYSTEM_FIND_DATA> files;
  if (!FileSystem::FindFiles(EmuFolders::SaveStates.c_str(), "*resume.sav", FILESYSTEM_FIND_FILES, &files) ||
      files.empty())
  {
    return {};
  }

  FILESYSTEM_FIND_DATA* most_recent = &files[0];
  for (FILESYSTEM_FIND_DATA& file : files)
  {
    if (file.ModificationTime > most_recent->ModificationTime)
      most_recent = &file;
  }

  return std::move(most_recent->FileName);
}

std::string System::GetCheatFileName()
{
  std::string ret;

  const std::string& title = System::GetGameTitle();
  if (!title.empty())
    ret = Path::Combine(EmuFolders::Cheats, fmt::format("{}.cht", title.c_str()));

  return ret;
}

bool System::LoadCheatList()
{
  // ArcadeDuck cheat support is intentionally gated until MAME cheat-package compatibility is implemented.
  s_cheat_list.reset();
  return false;
}

bool System::LoadCheatListFromDatabase()
{
  // ArcadeDuck cheat support is intentionally gated until MAME cheat-package compatibility is implemented.
  s_cheat_list.reset();
  return false;
}

bool System::SaveCheatList()
{
  if (!System::IsValid() || !System::HasCheatList())
    return false;

  const std::string filename(GetCheatFileName());
  if (filename.empty())
    return false;

  if (!System::GetCheatList()->SaveToPCSXRFile(filename.c_str()))
  {
    Host::AddIconOSDMessage(
      "cheat_save_error", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("System", "Failed to save cheat list to '{}'."), Path::GetFileName(filename)),
      Host::OSD_ERROR_DURATION);
  }

  return true;
}

bool System::DeleteCheatList()
{
  if (!System::IsValid())
    return false;

  const std::string filename(GetCheatFileName());
  if (!filename.empty())
  {
    if (!FileSystem::DeleteFile(filename.c_str()))
      return false;

    Host::AddIconOSDMessage(
      "cheat_delete", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("System", "Deleted cheat list '{}'."), Path::GetFileName(filename)),
      Host::OSD_INFO_DURATION);
  }

  System::SetCheatList(nullptr);
  return true;
}

void System::ClearCheatList(bool save_to_file)
{
  if (!System::IsValid())
    return;

  CheatList* cl = System::GetCheatList();
  if (!cl)
    return;

  while (cl->GetCodeCount() > 0)
    cl->RemoveCode(cl->GetCodeCount() - 1);

  if (save_to_file)
    SaveCheatList();
}

void System::SetCheatCodeState(u32 index, bool enabled)
{
  Host::AddKeyedOSDMessage(
    "CheatsUnderInvestigation",
    TRANSLATE_STR("OSDMessage", "Cheats are still under investigation and are not implemented yet. MAME cheat package compatibility is planned for a future update."),
    8.0f);
}

void System::ApplyCheatCode(u32 index)
{
  Host::AddKeyedOSDMessage(
    "CheatsUnderInvestigation",
    TRANSLATE_STR("OSDMessage", "Cheats are still under investigation and are not implemented yet. MAME cheat package compatibility is planned for a future update."),
    8.0f);
}

void System::ToggleWidescreen()
{
  g_settings.gpu_widescreen_hack = !g_settings.gpu_widescreen_hack;

  const DisplayAspectRatio user_ratio =
    Settings::ParseDisplayAspectRatio(
      Host::GetStringSettingValue("Display", "AspectRatio",
                                  Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO))
        .c_str())
      .value_or(DisplayAspectRatio::Auto);
  ;

  if (user_ratio == DisplayAspectRatio::Auto || user_ratio == DisplayAspectRatio::PAR1_1 ||
      user_ratio == DisplayAspectRatio::R4_3)
  {
    g_settings.display_aspect_ratio = g_settings.gpu_widescreen_hack ? DisplayAspectRatio::R16_9 : user_ratio;
  }
  else
  {
    g_settings.display_aspect_ratio = g_settings.gpu_widescreen_hack ? user_ratio : DisplayAspectRatio::Auto;
  }

  if (g_settings.gpu_widescreen_hack)
  {
    Host::AddKeyedOSDMessage(
      "WidescreenHack",
      fmt::format(TRANSLATE_FS("OSDMessage", "Widescreen hack is now enabled, and aspect ratio is set to {}."),
                  Settings::GetDisplayAspectRatioDisplayName(g_settings.display_aspect_ratio)),
      5.0f);
  }
  else
  {
    Host::AddKeyedOSDMessage(
      "WidescreenHack",
      fmt::format(TRANSLATE_FS("OSDMessage", "Widescreen hack is now disabled, and aspect ratio is set to {}."),
                  Settings::GetDisplayAspectRatioDisplayName(g_settings.display_aspect_ratio), 5.0f));
  }

  GTE::UpdateAspectRatio();
}

void System::ToggleSoftwareRendering()
{
  if (IsShutdown() || g_settings.gpu_renderer == GPURenderer::Software)
    return;

  const GPURenderer new_renderer = g_gpu->IsHardwareRenderer() ? GPURenderer::Software : g_settings.gpu_renderer;

  Host::AddIconOSDMessage("SoftwareRendering", ICON_FA_PAINT_ROLLER,
                          fmt::format(TRANSLATE_FS("OSDMessage", "Switching to {} renderer..."),
                                      Settings::GetRendererDisplayName(new_renderer)),
                          Host::OSD_QUICK_DURATION);
  RecreateGPU(new_renderer);
  ResetPerformanceCounters();
}

void System::RequestDisplaySize(float scale /*= 0.0f*/)
{
  if (!IsValid())
    return;

  if (scale == 0.0f)
    scale = g_gpu->IsHardwareRenderer() ? static_cast<float>(g_settings.gpu_resolution_scale) : 1.0f;

  const float y_scale =
    (static_cast<float>(g_gpu->GetCRTCDisplayWidth()) / static_cast<float>(g_gpu->GetCRTCDisplayHeight())) /
    g_gpu->ComputeDisplayAspectRatio();

  u32 requested_width =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(g_gpu->GetCRTCDisplayWidth()) * scale)), 1);
  u32 requested_height =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(g_gpu->GetCRTCDisplayHeight()) * y_scale * scale)), 1);

  if (g_settings.display_rotation == DisplayRotation::Rotate90 ||
      g_settings.display_rotation == DisplayRotation::Rotate270)
    std::swap(requested_width, requested_height);

  Host::RequestResizeHostDisplay(static_cast<s32>(requested_width), static_cast<s32>(requested_height));
}

void System::HostDisplayResized()
{
  if (!IsValid())
    return;

  if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio == DisplayAspectRatio::MatchWindow)
    GTE::UpdateAspectRatio();

  g_gpu->UpdateResolutionScale();
}

bool System::PresentDisplay(bool skip_present, bool explicit_present)
{
  // acquire for IO.MousePos.
  std::atomic_thread_fence(std::memory_order_acquire);

  if (!skip_present)
  {
    ImGuiManager::RenderBezelOverlay(g_settings.display_bezel_enabled, g_settings.display_bezel_path);
    FullscreenUI::Render();
    ImGuiManager::RenderTextOverlays();
    ImGuiManager::RenderOSDMessages();

    if (s_state == State::Running)
      ImGuiManager::RenderSoftwareCursors();
  }

  // Debug windows are always rendered, otherwise mouse input breaks on skip.
  ImGuiManager::RenderOverlayWindows();
  ImGuiManager::RenderDebugWindows();

  bool do_present;
  if (g_gpu && !skip_present)
    do_present = g_gpu->PresentDisplay();
  else
    do_present = g_gpu_device->BeginPresent(skip_present);

  if (do_present)
  {
    g_gpu_device->RenderImGui();
    g_gpu_device->EndPresent(explicit_present);

    if (g_gpu_device->IsGPUTimingEnabled())
    {
      s_accumulated_gpu_time += g_gpu_device->GetAndResetAccumulatedGPUTime();
      s_presents_since_last_update++;
    }
  }
  else
  {
    // Still need to kick ImGui or it gets cranky.
    ImGui::Render();
  }

  ImGuiManager::NewFrame();

  return do_present;
}

void System::InvalidateDisplay()
{
  PresentDisplay(false, false);

  if (g_gpu)
    g_gpu->RestoreDeviceContext();
}

void System::SetTimerResolutionIncreased(bool enabled)
{
#if defined(_WIN32)
  static bool current_state = false;
  if (current_state == enabled)
    return;

  current_state = enabled;

  if (enabled)
    timeBeginPeriod(1);
  else
    timeEndPeriod(1);
#endif
}

void System::UpdateSessionTime(const std::string& prev_serial)
{
  const u64 ctime = Common::Timer::GetCurrentValue();
  if (!prev_serial.empty() && GameList::IsGameListLoaded())
  {
    // round up to seconds
    const std::time_t etime =
      static_cast<std::time_t>(std::round(Common::Timer::ConvertValueToSeconds(ctime - s_session_start_time)));
    const std::time_t wtime = std::time(nullptr);
    GameList::AddPlayedTimeForSerial(prev_serial, wtime, etime);
  }

  s_session_start_time = ctime;
}

u64 System::GetSessionPlayedTime()
{
  const u64 ctime = Common::Timer::GetCurrentValue();
  return static_cast<u64>(std::round(Common::Timer::ConvertValueToSeconds(ctime - s_session_start_time)));
}

SocketMultiplexer* System::GetSocketMultiplexer()
{
#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (s_socket_multiplexer)
    return s_socket_multiplexer.get();

  Error error;
  s_socket_multiplexer = SocketMultiplexer::Create(&error);
  if (s_socket_multiplexer)
    INFO_LOG("Created socket multiplexer.");
  else
    ERROR_LOG("Failed to create socket multiplexer: {}", error.GetDescription());

  return s_socket_multiplexer.get();
#else
  ERROR_LOG("This build does not support sockets.");
  return nullptr;
#endif
}

void System::ReleaseSocketMultiplexer()
{
#ifdef ENABLE_SOCKET_MULTIPLEXER
  if (!s_socket_multiplexer || s_socket_multiplexer->HasAnyOpenSockets())
    return;

  INFO_LOG("Destroying socket multiplexer.");
  s_socket_multiplexer.reset();
#endif
}

#ifdef ENABLE_DISCORD_PRESENCE

#include "discord_rpc.h"

#define DISCORD_RPC_FUNCTIONS(X)                                                                                       \
  X(Discord_Initialize)                                                                                                \
  X(Discord_Shutdown)                                                                                                  \
  X(Discord_RunCallbacks)                                                                                              \
  X(Discord_UpdatePresence)                                                                                            \
  X(Discord_ClearPresence)

namespace dyn_libs {
static bool OpenDiscordRPC(Error* error);
static void CloseDiscordRPC();

static DynamicLibrary s_discord_rpc_library;

#define ADD_FUNC(F) static decltype(&::F) F;
DISCORD_RPC_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC
} // namespace dyn_libs

bool dyn_libs::OpenDiscordRPC(Error* error)
{
  if (s_discord_rpc_library.IsOpen())
    return true;

  const std::string libname = DynamicLibrary::GetVersionedFilename("discord-rpc");
  if (!s_discord_rpc_library.Open(libname.c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load discord-rpc: ");
    return false;
  }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_discord_rpc_library.GetSymbol(#F, &F))                                                                        \
  {                                                                                                                    \
    Error::SetStringFmt(error, "Failed to find function {}", #F);                                                      \
    CloseDiscordRPC();                                                                                                 \
    return false;                                                                                                      \
  }
  DISCORD_RPC_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

  return true;
}

void dyn_libs::CloseDiscordRPC()
{
  if (!s_discord_rpc_library.IsOpen())
    return;

#define UNLOAD_FUNC(F) F = nullptr;
  DISCORD_RPC_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_discord_rpc_library.Close();
}

void System::InitializeDiscordPresence()
{
  if (s_discord_presence_active)
    return;

  Error error;
  if (!dyn_libs::OpenDiscordRPC(&error))
  {
    ERROR_LOG("Failed to open discord-rpc: {}", error.GetDescription());
    return;
  }

  DiscordEventHandlers handlers = {};
  dyn_libs::Discord_Initialize("1532729313521172600", &handlers, 0, nullptr);
  s_discord_presence_active = true;

  UpdateRichPresence(true);
}

void System::ShutdownDiscordPresence()
{
  if (!s_discord_presence_active)
    return;

  dyn_libs::Discord_ClearPresence();
  dyn_libs::Discord_Shutdown();
  dyn_libs::CloseDiscordRPC();

  s_discord_presence_active = false;
}

void System::UpdateRichPresence(bool update_session_time)
{
  if (!s_discord_presence_active)
    return;

  if (update_session_time)
    s_discord_presence_time_epoch = std::time(nullptr);

  // https://discord.com/developers/docs/rich-presence/how-to#updating-presence-update-presence-payload-fields
  DiscordRichPresence rp = {};
  rp.largeImageKey = "arcadeduck_logo";
  rp.largeImageText = "ArcadeDuck Arcade Emulator";
  rp.startTimestamp = s_discord_presence_time_epoch;
  rp.details = "No Game Running";
  if (IsValidOrInitializing())
  {
    rp.details = s_running_game_title.empty() ? "Unknown Game" : s_running_game_title.c_str();
  }

  std::string state_string;
  if (Achievements::HasRichPresence())
  {
    const auto lock = Achievements::GetLock();
    state_string = StringUtil::Ellipsise(Achievements::GetRichPresenceString(), 128);
    rp.state = state_string.c_str();
  }

  dyn_libs::Discord_UpdatePresence(&rp);
}

void System::PollDiscordPresence()
{
  if (!s_discord_presence_active)
    return;

  dyn_libs::Discord_RunCallbacks();
}

#else

void System::UpdateRichPresence(bool update_session_time)
{
}

#endif
