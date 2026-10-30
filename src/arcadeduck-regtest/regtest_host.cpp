// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "core/achievements.h"
#include "core/arcade/arcade_input.h"
#include "core/arcade/arcade_save_state.h"
#include "core/arcade/devices/jvs/namco_cyberlead.h"
#include "core/cpu_code_cache.h"
#include "core/fullscreen_ui.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/save_state_version.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "scmversion/arcadeduck_version.h"
#include "scmversion/scmversion.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/platform_misc.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/memory_settings_interface.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

Log_SetChannel(RegTestHost);

namespace RegTestHost {
static bool ParseCommandLineParameters(int argc, char* argv[], std::optional<SystemBootParameters>& autoboot);
static void PrintCommandLineVersion();
static void PrintCommandLineHelp(const char* progname);
static bool InitializeConfig();
static void InitializeEarlyConsole();
static void HookSignals();
static bool SetFolders();
static bool SetNewDataRoot(const std::string& filename);
static std::string GetFrameDumpFilename(u32 frame);
} // namespace RegTestHost

static std::unique_ptr<MemorySettingsInterface> s_base_settings_interface;

static u32 s_frames_to_run = 60 * 60;
static u32 s_frames_remaining = 0;
static u32 s_frame_dump_interval = 0;
static std::string s_dump_base_directory;

// Arcade save-state check script: one "<frame> <command> [args...]" per line, run between frames.
namespace RegTestScript {
struct Command
{
  u32 frame;
  std::vector<std::string> args;
};

static bool Load(const std::string& path);
static void RunFrame();
static void Check(bool condition, std::string_view what);

static std::vector<Command> s_commands;
static size_t s_next_command = 0;
static u32 s_frame = 0;
static u32 s_failures = 0;
static std::map<std::string, Arcade::SaveState::Hash> s_hashes;
static std::map<std::string, std::vector<u8>> s_file_backups;
} // namespace RegTestScript

static bool s_arcade_boot = false;
static std::string s_data_root_override;

bool RegTestHost::SetFolders()
{
  std::string program_path(FileSystem::GetProgramPath());
  INFO_LOG("Program Path: {}", program_path);

  EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(program_path));
  EmuFolders::DataRoot = EmuFolders::AppRoot;

#ifdef __APPLE__
  static constexpr char MAC_DATA_DIR[] = "Library/Application Support/ArcadeDuck";
  const char* home_dir = getenv("HOME");
  if (home_dir)
    EmuFolders::DataRoot = Path::Combine(home_dir, MAC_DATA_DIR);
#endif

  // On Windows/Linux, these are in the binary directory.
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");

  DEV_LOG("AppRoot Directory: {}", EmuFolders::AppRoot);
  DEV_LOG("DataRoot Directory: {}", EmuFolders::DataRoot);
  DEV_LOG("Resources Directory: {}", EmuFolders::Resources);

  // Write crash dumps to the data directory, since that'll be accessible for certain.
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

  // the resources directory should exist, bail out if not
  if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
  {
    ERROR_LOG("Resources directory is missing, your installation is incomplete.");
    return false;
  }

  return true;
}

bool RegTestHost::InitializeConfig()
{
  SetFolders();

  s_base_settings_interface = std::make_unique<MemorySettingsInterface>();
  Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

  // default settings for runner
  SettingsInterface& si = *s_base_settings_interface.get();
  g_settings.Save(si, false);
  si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(GPURenderer::Software));
  si.SetBoolValue("GPU", "DisableShaderCache", true);
  si.SetStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioBackend::Null));
  si.SetBoolValue("Logging", "LogToConsole", false);
  si.SetBoolValue("Logging", "LogToFile", false);
  si.SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(LOGLEVEL_INFO));
  si.SetBoolValue("Main", "ApplyGameSettings", false); // don't want game settings interfering
  si.SetBoolValue("BIOS", "PatchFastBoot", true);      // no point validating the bios intro..
  si.SetFloatValue("Main", "EmulationSpeed", 0.0f);

  // disable all sources
  for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
    si.SetBoolValue("InputSources", InputManager::InputSourceToString(static_cast<InputSourceType>(i)), false);

  EmuFolders::LoadConfig(*s_base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();

  // imgui setup, make sure it doesn't bug out
  ImGuiManager::SetFontPathAndRange(std::string(), {0x0020, 0x00FF, 0, 0});

  return true;
}

void Host::ReportFatalError(std::string_view title, std::string_view message)
{
  ERROR_LOG("ReportFatalError: {}", message);
  abort();
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ReportErrorAsync: {}", message);
}

bool Host::ConfirmMessage(std::string_view title, std::string_view message)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ConfirmMessage: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ConfirmMessage: {}", message);

  return true;
}

void Host::ReportDebuggerMessage(std::string_view message)
{
  ERROR_LOG("ReportDebuggerMessage: {}", message);
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  return {};
}

bool Host::ChangeLanguage(const char* new_language)
{
  return false;
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view context, std::string_view msg, char* tbuf,
                                            size_t tbuf_space)
{
  if (msg.size() > tbuf_space)
    return -1;
  else if (msg.empty())
    return 0;

  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
  TinyString count_str = TinyString::from_format("{}", count);

  std::string ret(msg);
  for (;;)
  {
    std::string::size_type pos = ret.find("%n");
    if (pos == std::string::npos)
      break;

    ret.replace(pos, pos + 2, count_str.view());
  }

  return ret;
}

SmallString Host::TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation,
                                               int count)
{
  SmallString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Settings& old_settings)
{
}

void Host::CommitBaseSettingChanges()
{
  // noop, in memory
}

bool Host::ResourceFileExists(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  return FileSystem::FileExists(path.c_str());
}

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  std::optional<DynamicHeapArray<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
  if (!ret.has_value())
    ERROR_LOG("Failed to read resource file '{}'", filename);
  return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
  if (!ret.has_value())
    ERROR_LOG("Failed to read resource file to string '{}'", filename);
  return ret;
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
  {
    ERROR_LOG("Failed to stat resource file '{}'", filename);
    return std::nullopt;
  }

  return sd.ModificationTime;
}

void Host::OnSystemStarting()
{
  //
}

void Host::OnSystemStarted()
{
  //
}

void Host::OnSystemDestroyed()
{
  //
}

void Host::OnSystemPaused()
{
  //
}

void Host::OnSystemResumed()
{
  //
}

void Host::OnIdleStateChanged()
{
  //
}

void Host::OnPerformanceCountersUpdated()
{
  //
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name)
{
  INFO_LOG("Disc Path: {}", disc_path);
  INFO_LOG("Game Serial: {}", game_serial);
  INFO_LOG("Game Name: {}", game_name);
}

void Host::OnMediaCaptureStarted()
{
  //
}

void Host::OnMediaCaptureStopped()
{
  //
}

void Host::PumpMessagesOnCPUThread()
{
  RegTestScript::RunFrame();
  s_frames_remaining--;
  if (s_frames_remaining == 0)
    System::ShutdownSystem(false);
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
  // only one thread in this version...
  function();
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
  //
}

void Host::RequestExitApplication(bool save_state_if_running)
{
  //
}

void Host::RequestExitBigPicture()
{
  //
}

void Host::RequestSystemShutdown(bool allow_confirm, bool save_state)
{
  //
}

bool Host::IsFullscreen()
{
  return false;
}

void Host::SetFullscreen(bool enabled)
{
  //
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
  WindowInfo wi;
  wi.SetSurfaceless();
  return wi;
}

void Host::ReleaseRenderWindow()
{
  //
}

void Host::FrameDone()
{
  const u32 frame = System::GetFrameNumber();
  if (s_frame_dump_interval > 0 && (s_frame_dump_interval == 1 || (frame % s_frame_dump_interval) == 0))
  {
    std::string dump_filename(RegTestHost::GetFrameDumpFilename(frame));
    g_gpu->WriteDisplayTextureToFile(std::move(dump_filename));
  }
}

void Host::OpenURL(std::string_view url)
{
  //
}

bool Host::CopyTextToClipboard(std::string_view text)
{
  return false;
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
  //
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  // noop
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
  // noop
}

void Host::OnAchievementsRefreshed()
{
  // noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
  // noop
}

void Host::OnCoverDownloaderOpenRequested()
{
  // noop
}

bool Host::ShouldPreferHostFileSelector()
{
  return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                     FileSelectorFilters filters /* = FileSelectorFilters() */,
                                     std::string_view initial_directory /* = std::string_view() */)
{
  callback(std::string());
}

// The runner has no keyboard; scripts press named keys so ordinary bindings can be exercised.
static constexpr std::array<std::string_view, 24> s_regtest_key_names = {
  {"Up", "Down", "Left", "Right", "Enter", "Space", "F1", "F2", "F3", "F4", "F5", "F6",
   "1",  "2",    "3",    "4",     "5",     "6",     "A",  "B",  "C",  "D",  "E",  "F"}};

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(std::string_view str)
{
  for (size_t i = 0; i < s_regtest_key_names.size(); i++)
  {
    if (s_regtest_key_names[i] == str)
      return static_cast<u32>(i + 1);
  }

  return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
  if (code == 0 || code > s_regtest_key_names.size())
    return std::nullopt;

  return std::string(s_regtest_key_names[code - 1]);
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
  return nullptr;
}

void Host::AddFixedInputBindings(SettingsInterface& si)
{
  ArcadeInput::RegisterBindings(si, *s_base_settings_interface, System::GetGameSerial());
}

void Host::OnInputDeviceConnected(std::string_view identifier, std::string_view device_name)
{
  // noop
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
{
  // noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
  return std::nullopt;
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
  // noop
}

void Host::CancelGameListRefresh()
{
  // noop
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void SignalHandler(int signal)
{
  std::signal(signal, SIG_DFL);

  // MacOS is missing std::quick_exit() despite it being C++11...
#ifndef __APPLE__
  std::quick_exit(1);
#else
  _Exit(1);
#endif
}

void RegTestHost::HookSignals()
{
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
}

void RegTestHost::InitializeEarlyConsole()
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);
}

void RegTestHost::PrintCommandLineVersion()
{
  InitializeEarlyConsole();
  std::fprintf(stderr, "ArcadeDuck Regression Test Runner - %s\n", g_scm_version_str);
  std::fprintf(stderr, "Commit: %s\n", g_scm_hash_str);
  std::fprintf(stderr, "Branch: %s\n", g_scm_branch_str);
  std::fprintf(stderr, "Commit date: %s\n", g_scm_date_str);
  std::fprintf(stderr, "https://github.com/StillJC/ArcadeDuck\n");
  std::fprintf(stderr, "\n");
}void RegTestHost::PrintCommandLineHelp(const char* progname)
{
  InitializeEarlyConsole();
  PrintCommandLineVersion();
  std::fprintf(stderr, "Usage: %s [parameters] [--] [boot filename]\n", progname);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "  -help: Displays this information and exits.\n");
  std::fprintf(stderr, "  -version: Displays version information and exits.\n");
  std::fprintf(stderr, "  -dumpdir: Set frame dump base directory (will be dumped to basedir/gametitle).\n");
  std::fprintf(stderr, "  -dumpinterval: Dumps every N frames.\n");
  std::fprintf(stderr, "  -frames: Sets the number of frames to execute.\n");
  std::fprintf(stderr, "  -log <level>: Sets the log level. Defaults to verbose.\n");
  std::fprintf(stderr, "  -renderer <renderer>: Sets the graphics renderer. Default to software.\n");
  std::fprintf(stderr, "  -arcade: Boots the filename as an arcade ROM archive.\n");
  std::fprintf(stderr, "  -dataroot <dir>: Uses <dir> for settings-relative folders, NVRAM and save states.\n");
  std::fprintf(stderr, "  -set <Section/Key=Value>: Overrides one setting before boot.\n");
  std::fprintf(stderr, "  -bind <Section/Key=Binding>: Adds one input binding, e.g. Keyboard/C.\n");
  std::fprintf(stderr, "  -script <file>: Runs frame-scheduled save-state checks; failures set exit code 2.\n");
  std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
                       "    parameters make up the filename. Use when the filename contains\n"
                       "    spaces or starts with a dash.\n");
  std::fprintf(stderr, "\n");
}

static std::optional<SystemBootParameters>& AutoBoot(std::optional<SystemBootParameters>& autoboot)
{
  if (!autoboot)
    autoboot.emplace();

  return autoboot;
}

bool RegTestHost::ParseCommandLineParameters(int argc, char* argv[], std::optional<SystemBootParameters>& autoboot)
{
  bool no_more_args = false;
  for (int i = 1; i < argc; i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

      if (CHECK_ARG("-help"))
      {
        PrintCommandLineHelp(argv[0]);
        return false;
      }
      else if (CHECK_ARG("-version"))
      {
        PrintCommandLineVersion();
        return false;
      }
      else if (CHECK_ARG_PARAM("-dumpdir"))
      {
        s_dump_base_directory = argv[++i];
        if (s_dump_base_directory.empty())
        {
          ERROR_LOG("Invalid dump directory specified.");
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-dumpinterval"))
      {
        s_frame_dump_interval = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (s_frame_dump_interval <= 0)
        {
          ERROR_LOG("Invalid dump interval specified: {}", argv[i]);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-frames"))
      {
        s_frames_to_run = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (s_frames_to_run == 0)
        {
          ERROR_LOG("Invalid frame count specified: {}", argv[i]);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-log"))
      {
        std::optional<LOGLEVEL> level = Settings::ParseLogLevelName(argv[++i]);
        if (!level.has_value())
        {
          ERROR_LOG("Invalid log level specified.");
          return false;
        }

        Log::SetLogLevel(level.value());
        s_base_settings_interface->SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(level.value()));
        continue;
      }
      else if (CHECK_ARG_PARAM("-console"))
      {
        Log::SetConsoleOutputParams(true);
        s_base_settings_interface->SetBoolValue("Logging", "LogToConsole", true);
        continue;
      }
      else if (CHECK_ARG_PARAM("-renderer"))
      {
        std::optional<GPURenderer> renderer = Settings::ParseRendererName(argv[++i]);
        if (!renderer.has_value())
        {
          ERROR_LOG("Invalid renderer specified.");
          return false;
        }

        s_base_settings_interface->SetStringValue("GPU", "Renderer", Settings::GetRendererName(renderer.value()));
        continue;
      }
      else if (CHECK_ARG_PARAM("-upscale"))
      {
        const u32 upscale = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (upscale == 0)
        {
          ERROR_LOG("Invalid upscale value.");
          return false;
        }

        INFO_LOG("Setting upscale to {}.", upscale);
        s_base_settings_interface->SetIntValue("GPU", "ResolutionScale", static_cast<s32>(upscale));
        continue;
      }
      else if (CHECK_ARG_PARAM("-cpu"))
      {
        const std::optional<CPUExecutionMode> cpu = Settings::ParseCPUExecutionMode(argv[++i]);
        if (!cpu.has_value())
        {
          ERROR_LOG("Invalid CPU execution mode.");
          return false;
        }

        INFO_LOG("Setting CPU execution mode to {}.", Settings::GetCPUExecutionModeName(cpu.value()));
        s_base_settings_interface->SetStringValue("CPU", "ExecutionMode",
                                                  Settings::GetCPUExecutionModeName(cpu.value()));
        continue;
      }
      else if (CHECK_ARG("-pgxp"))
      {
        INFO_LOG("Enabling PGXP.");
        s_base_settings_interface->SetBoolValue("GPU", "PGXPEnable", true);
        continue;
      }
      else if (CHECK_ARG("-pgxp-cpu"))
      {
        INFO_LOG("Enabling PGXP CPU mode.");
        s_base_settings_interface->SetBoolValue("GPU", "PGXPEnable", true);
        s_base_settings_interface->SetBoolValue("GPU", "PGXPCPU", true);
        continue;
      }
      else if (CHECK_ARG("-arcade"))
      {
        s_arcade_boot = true;
        continue;
      }
      else if (CHECK_ARG_PARAM("-dataroot"))
      {
        s_data_root_override = Path::Canonicalize(argv[++i]);
        continue;
      }
      else if (CHECK_ARG_PARAM("-set") || CHECK_ARG_PARAM("-bind"))
      {
        const bool bind = CHECK_ARG("-bind");
        const std::string_view arg = argv[++i];
        const std::string_view::size_type slash = arg.find('/');
        const std::string_view::size_type equals = arg.find('=');
        if (slash == std::string_view::npos || equals == std::string_view::npos || equals < slash)
        {
          ERROR_LOG("Invalid -set/-bind argument: '{}'", arg);
          return false;
        }

        const std::string section(arg.substr(0, slash));
        const std::string key(arg.substr(slash + 1, equals - slash - 1));
        const std::string value(arg.substr(equals + 1));
        if (bind)
          s_base_settings_interface->AddToStringList(section.c_str(), key.c_str(), value.c_str());
        else
          s_base_settings_interface->SetStringValue(section.c_str(), key.c_str(), value.c_str());
        continue;
      }
      else if (CHECK_ARG_PARAM("-script"))
      {
        if (!RegTestScript::Load(argv[++i]))
          return false;
        continue;
      }
      else if (CHECK_ARG("--"))
      {
        no_more_args = true;
        continue;
      }
      else if (argv[i][0] == '-')
      {
        ERROR_LOG("Unknown parameter: '{}'", argv[i]);
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (autoboot && !autoboot->filename.empty())
      autoboot->filename += ' ';
    AutoBoot(autoboot)->filename += argv[i];
  }

  return true;
}

bool RegTestHost::SetNewDataRoot(const std::string& filename)
{
  const std::string_view game_name = Path::GetFileTitle(filename);
  INFO_LOG("Game name from filename: {}", game_name);

  if (!s_dump_base_directory.empty())
  {
    std::string dump_directory = Path::Combine(s_dump_base_directory, game_name);
    if (!FileSystem::DirectoryExists(dump_directory.c_str()))
    {
      INFO_LOG("Creating directory '{}'...", dump_directory);
      if (!FileSystem::CreateDirectory(dump_directory.c_str(), false))
        Panic("Failed to create dump directory.");
    }

    // Switch to file logging.
    INFO_LOG("Dumping frames to '{}'...", dump_directory);
    EmuFolders::DataRoot = std::move(dump_directory);
    s_base_settings_interface->SetBoolValue("Logging", "LogToConsole", false);
    s_base_settings_interface->SetBoolValue("Logging", "LogToFile", true);
    s_base_settings_interface->SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(LOGLEVEL_DEV));
    System::ApplySettings(false);
  }

  return true;
}

bool RegTestScript::Load(const std::string& path)
{
  const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
  if (!text.has_value())
  {
    ERROR_LOG("Failed to read script '{}'.", path);
    return false;
  }

  for (const std::string_view line : StringUtil::SplitString(text.value(), '\n'))
  {
    const std::string_view trimmed = StringUtil::StripWhitespace(line);
    if (trimmed.empty() || trimmed.front() == '#')
      continue;

    Command cmd;
    for (const std::string_view token : StringUtil::SplitString(trimmed, ' '))
      cmd.args.emplace_back(token);

    const std::optional<u32> frame = StringUtil::FromChars<u32>(cmd.args.front());
    if (!frame.has_value() || cmd.args.size() < 2)
    {
      ERROR_LOG("Invalid script line: '{}'", trimmed);
      return false;
    }

    cmd.frame = frame.value();
    cmd.args.erase(cmd.args.begin());
    s_commands.push_back(std::move(cmd));
  }

  std::stable_sort(s_commands.begin(), s_commands.end(),
                   [](const Command& a, const Command& b) { return a.frame < b.frame; });
  return true;
}

void RegTestScript::Check(bool condition, std::string_view what)
{
  if (condition)
  {
    INFO_LOG("REGTEST PASS frame={} {}", s_frame, what);
  }
  else
  {
    ERROR_LOG("REGTEST FAIL frame={} {}", s_frame, what);
    s_failures++;
  }
}

static std::string GetScriptPath(std::string_view name)
{
  return Path::IsAbsolute(name) ? std::string(name) : Path::Combine(EmuFolders::SaveStates, name);
}

static std::string GetNVRAMPath(std::string_view name)
{
  return Path::Combine(Path::Combine(Path::Combine(EmuFolders::DataRoot, "nvram"), System::GetGameSerial()), name);
}

// Hashes the machine state as written by an uncompressed save, excluding header, metadata and screenshot.
static std::optional<Arcade::SaveState::Hash> HashMachineState(const std::string& tag, bool keep_file)
{
  const std::string path = Path::Combine(EmuFolders::Cache, fmt::format("regtest_hash_{}.sav", tag));
  // Hashes observe the machine at any point, including while saving is refused for readiness.
  Error error;
  const bool saved = System::IsArcadeSaveStateSession() ? System::SaveArcadeDiagnosticState(path.c_str(), &error) :
                                                          System::SaveState(path.c_str(), &error, false);
  if (!saved)
  {
    ERROR_LOG("hash save failed: {}", error.GetDescription());
    return std::nullopt;
  }

  const std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
  if (!keep_file)
    FileSystem::DeleteFile(path.c_str());
  if (!data.has_value() || data->size() < sizeof(SAVE_STATE_HEADER))
    return std::nullopt;

  SAVE_STATE_HEADER header;
  std::memcpy(&header, data->data(), sizeof(header));
  if (header.data_compression_type != static_cast<u32>(SAVE_STATE_HEADER::CompressionType::None) ||
      static_cast<size_t>(header.offset_to_data) + header.data_compressed_size > data->size())
  {
    ERROR_LOG("hash requires Main/SaveStateCompression=Uncompressed");
    return std::nullopt;
  }

  return Arcade::SaveState::HashBytes(
    std::span<const u8>(data->data() + header.offset_to_data, header.data_compressed_size));
}

void RegTestScript::RunFrame()
{
  for (; s_next_command < s_commands.size() && s_commands[s_next_command].frame <= s_frame; s_next_command++)
  {
    const std::vector<std::string>& a = s_commands[s_next_command].args;
    const std::string& op = a[0];
    const auto arg = [&a](size_t i) -> std::string_view { return (i < a.size()) ? std::string_view(a[i]) : ""; };
    INFO_LOG("REGTEST frame={} {}", s_frame, StringUtil::JoinString(a.begin(), a.end(), ' '));

    if (op == "await-save-ready")
    {
      // Delays this and every later command until the machine reports it can be saved (device/protocol state),
      // failing after the given number of frames (default 3000).
      static u32 s_waited = 0;
      Error error;
      if (!System::IsArcadeSaveReady(&error))
      {
        const u32 limit = StringUtil::FromChars<u32>(arg(1)).value_or(3000);
        if (++s_waited <= limit)
        {
          for (size_t i = s_next_command; i < s_commands.size(); i++)
            s_commands[i].frame++;
          break;
        }
        Check(false, fmt::format("await-save-ready ({})", error.GetDescription()));
      }
      else
      {
        INFO_LOG("REGTEST await-save-ready waited {} frames", s_waited);
      }
      s_waited = 0;
    }
    else if (op == "quit")
    {
      // Ends the session after this frame (persistence is written by the normal shutdown).
      s_frames_remaining = 1;
    }
    else if (op == "press" || op == "release")
    {
      const std::optional<u32> code = InputManager::ConvertHostKeyboardStringToCode(arg(1));
      Check(code.has_value(), fmt::format("key {}", arg(1)));
      if (code.has_value())
        InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(code.value()), (op == "press") ? 1.0f : 0.0f);
    }
    else if (op == "save" || op == "save-fail")
    {
      Error error;
      const bool ok = System::SaveState(GetScriptPath(arg(1)).c_str(), &error, false);
      Check(ok == (op == "save"), fmt::format("{} {} ({})", op, arg(1), ok ? "saved" : error.GetDescription()));
    }
    else if (op == "saveslot")
    {
      Error error;
      const s32 slot = StringUtil::FromChars<s32>(arg(1)).value_or(0);
      const bool ok =
        System::SaveState(System::GetGameSaveStateFileName(System::GetGameSerial(), slot).c_str(), &error, false);
      Check(ok == (arg(2) != "fail"), fmt::format("saveslot {} ({})", slot, ok ? "saved" : error.GetDescription()));
    }
    else if (op == "saveslot-wait")
    {
      // Interactive slot save: waits for the board when it is only briefly busy. Logs whether it was queued.
      Error error;
      const s32 slot = StringUtil::FromChars<s32>(arg(1)).value_or(0);
      const bool ok = System::SaveStateWhenReady(
        System::GetGameSaveStateFileName(System::GetGameSerial(), slot).c_str(), &error, false);
      Check(ok, fmt::format("saveslot-wait {} ({})", slot,
                            ok ? (System::HasPendingArcadeSave() ? "queued" : "saved") : error.GetDescription()));
    }
    else if (op == "pending-save")
    {
      INFO_LOG("REGTEST pending-save {} pending={}", arg(1), System::HasPendingArcadeSave());
    }
    else if (op == "savenamed" || op == "savenamed-fail")
    {
      Error error;
      const bool ok = System::SaveNamedState(arg(1), &error);
      Check(ok == (op == "savenamed"), fmt::format("{} {} ({})", op, arg(1), ok ? "saved" : error.GetDescription()));
    }
    else if (op == "saveresume" || op == "saveresume-fail")
    {
      Error error;
      const bool ok = System::SaveResumeState(&error);
      Check(ok == (op == "saveresume"), fmt::format("{} ({})", op, ok ? "saved" : error.GetDescription()));
    }
    else if (op == "load" || op == "load-reject")
    {
      Error error;
      const std::string path = (arg(1).starts_with("slot:")) ?
                                 System::GetGameSaveStateFileName(
                                   System::GetGameSerial(), StringUtil::FromChars<s32>(arg(1).substr(5)).value_or(0)) :
                                 GetScriptPath(arg(1));
      const bool ok = System::LoadState(path.c_str(), &error, false);
      const bool expected = (op == "load");
      Check(ok == expected && (ok || System::WasLoadRejectionReported()),
            fmt::format("{} {} ({})", op, arg(1), ok ? "loaded" : error.GetDescription()));
    }
    else if (op == "hash" || op == "hash-keep")
    {
      std::optional<Arcade::SaveState::Hash> hash = HashMachineState(std::string(arg(1)), op == "hash-keep");
      Check(hash.has_value(), fmt::format("hash {}", arg(1)));
      if (hash.has_value())
      {
        INFO_LOG("REGTEST hash {} = {}", arg(1), Arcade::SaveState::FormatHash(hash.value()));
        s_hashes[std::string(arg(1))] = hash.value();
      }
    }
    else if (op == "expect-equal" || op == "expect-differ")
    {
      const auto ha = s_hashes.find(std::string(arg(1)));
      const auto hb = s_hashes.find(std::string(arg(2)));
      const bool both = (ha != s_hashes.end() && hb != s_hashes.end());
      Check(both && ((ha->second == hb->second) == (op == "expect-equal")),
            fmt::format("{} {} {}", op, arg(1), arg(2)));
    }
    else if (op == "nvram-backup" || op == "file-backup")
    {
      const std::string path = (op == "nvram-backup") ? GetNVRAMPath(arg(1)) : GetScriptPath(arg(1));
      std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
      Check(data.has_value(), fmt::format("{} {}", op, path));
      if (data.has_value())
        s_file_backups[path].assign(data->begin(), data->end());
    }
    else if (op == "nvram-restore" || op == "file-restore")
    {
      const std::string path = (op == "nvram-restore") ? GetNVRAMPath(arg(1)) : GetScriptPath(arg(1));
      const auto it = s_file_backups.find(path);
      Check(it != s_file_backups.end() &&
              FileSystem::WriteBinaryFile(path.c_str(), it->second.data(), it->second.size()),
            fmt::format("{} {}", op, path));
    }
    else if (op == "nvram-corrupt" || op == "file-corrupt")
    {
      // XORs one byte in place.
      const std::string path = (op == "nvram-corrupt") ? GetNVRAMPath(arg(1)) : GetScriptPath(arg(1));
      std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
      const size_t offset = StringUtil::FromChars<size_t>(arg(2)).value_or(0);
      const bool ok = data.has_value() && offset < data->size();
      if (ok)
        (*data)[offset] ^= 0xFF;
      Check(ok && FileSystem::WriteBinaryFile(path.c_str(), data->data(), data->size()),
            fmt::format("{} {} @{}", op, path, offset));
    }
    else if (op == "nvram-delete" || op == "file-delete")
    {
      const std::string path = (op == "nvram-delete") ? GetNVRAMPath(arg(1)) : GetScriptPath(arg(1));
      Check(FileSystem::DeleteFile(path.c_str()), fmt::format("{} {}", op, path));
    }
    else if (op == "file-truncate")
    {
      const std::string src = GetScriptPath(arg(1));
      std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(src.c_str());
      const size_t size = StringUtil::FromChars<size_t>(arg(3)).value_or(0);
      Check(data.has_value() && size <= data->size() &&
              FileSystem::WriteBinaryFile(GetScriptPath(arg(2)).c_str(), data->data(), size),
            fmt::format("file-truncate {} -> {} ({} bytes)", arg(1), arg(2), size));
    }
    else if (op == "file-copy")
    {
      Check(FileSystem::CopyFilePath(GetScriptPath(arg(1)).c_str(), GetScriptPath(arg(2)).c_str(), true),
            fmt::format("file-copy {} -> {}", arg(1), arg(2)));
    }
    else if (op == "rewrite-slot")
    {
      Error error;
      const s32 slot = StringUtil::FromChars<s32>(arg(2)).value_or(0);
      const bool ok = Arcade::SaveState::RewriteSlot(GetScriptPath(arg(1)).c_str(), slot, arg(3), &error);
      Check(ok == (arg(4) != "fail"), fmt::format("rewrite-slot {} {} {}", arg(1), slot, error.GetDescription()));
    }
    else if (op == "expect-slot-path")
    {
      // The path a slot resolves to, by file name.
      const s32 slot = StringUtil::FromChars<s32>(arg(1)).value_or(0);
      const std::string path = System::GetGameSaveStateFileName(System::GetGameSerial(), slot);
      Check(StringUtil::EqualNoCase(Path::GetFileName(path), arg(2)),
            fmt::format("expect-slot-path {} {} (got {})", slot, arg(2), Path::GetFileName(path)));
    }
    else if (op == "list")
    {
      for (const Arcade::SaveState::Entry& e : Arcade::SaveState::FindSaves(System::GetGameSerial()))
      {
        INFO_LOG("REGTEST save '{}' slot={} label='{}' created={} duplicate={} nvram={}", Path::GetFileName(e.path),
                 e.metadata.slot, e.metadata.label, e.metadata.created_time, e.duplicate_slot, e.metadata.nvram.size());
      }
    }
    else if (op == "expect-count")
    {
      const size_t expected = StringUtil::FromChars<size_t>(arg(1)).value_or(0);
      const size_t actual = Arcade::SaveState::FindSaves(System::GetGameSerial()).size();
      Check(actual == expected, fmt::format("expect-count {} (got {})", expected, actual));
    }
    else if (op == "reset")
    {
      System::ResetSystem();
    }
    else if (op == "flush-code-cache")
    {
      // Discards compiled code exactly as a state load does, without changing machine state.
      CPU::CodeCache::Reset();
      System::InterruptExecution();
    }
    else if (op == "position")
    {
      INFO_LOG("REGTEST position {} frame={} internal_frame={} ticks={} event_run={} running_events={}", arg(1),
               System::GetFrameNumber(), System::GetInternalFrameNumber(), System::GetGlobalTickCounter(),
               TimingEvents::GetEventRunTickCounter(), TimingEvents::IsRunningEvents());
    }
    else if (op == "nvram-dump")
    {
      // Live NVRAM contents written to the cache folder as <tag>.<component>.bin.
      Check(System::DumpArcadeNVRAM(EmuFolders::Cache, arg(1)), fmt::format("nvram-dump {}", arg(1)));
      if (const Arcade::JVS::NamcoCyberLead* cl = Arcade::JVS::NamcoCyberLead::GetActive(); cl && cl->IsLEDPresent())
      {
        const auto& led = cl->GetLEDDevice();
        const auto& st = led.GetStats();
        INFO_LOG(
          "REGTEST cyberlead {} link_state={} flash_mode={} page={} ready={} programmed={} changed={} erases={} {}",
          arg(1), cl->GetLinkStateCode(), led.GetFlashMode(), led.GetFlashPageCount(), cl->IsLEDSaveReady(),
          st.flash_program_bytes, st.flash_changed_bytes, st.flash_erases, cl->DescribeLinkDiagnostics());
      }
    }
    else if (op == "nvram-fp")
    {
      // Live NVRAM contents; these are excluded from the state data and so from "hash".
      const std::vector<std::string> fps = System::GetArcadeNVRAMFingerprints();
      const std::string joined = StringUtil::JoinString(fps.begin(), fps.end(), ' ');
      INFO_LOG("REGTEST nvram {} {}", arg(1), joined);
      s_hashes[fmt::format("nvram:{}", arg(1))] =
        Arcade::SaveState::HashBytes(std::span<const u8>(reinterpret_cast<const u8*>(joined.data()), joined.size()));
    }
    else
    {
      Check(false, fmt::format("unknown command '{}'", op));
    }
  }

  s_frame++;
}

std::string RegTestHost::GetFrameDumpFilename(u32 frame)
{
  return Path::Combine(EmuFolders::DataRoot, fmt::format("frame_{:05d}.png", frame));
}

int main(int argc, char* argv[])
{
  RegTestHost::InitializeEarlyConsole();

  if (!RegTestHost::InitializeConfig())
    return EXIT_FAILURE;

  std::optional<SystemBootParameters> autoboot;
  if (!RegTestHost::ParseCommandLineParameters(argc, argv, autoboot))
    return EXIT_FAILURE;

  if (!autoboot || autoboot->filename.empty())
  {
    ERROR_LOG("No boot path specified.");
    return EXIT_FAILURE;
  }

  if (!RegTestHost::SetNewDataRoot(autoboot->filename))
    return EXIT_FAILURE;

  if (!s_data_root_override.empty())
  {
    // Every folder, including nvram/ and savestates/, resolves under the override.
    EmuFolders::DataRoot = s_data_root_override;
    EmuFolders::LoadConfig(*s_base_settings_interface.get());
    EmuFolders::EnsureFoldersExist();
  }

  INFO_LOG("Resolved DataRoot='{}' SaveStates='{}' Bios='{}' NVRAM='{}'", EmuFolders::DataRoot, EmuFolders::SaveStates,
           EmuFolders::Bios, Path::Combine(EmuFolders::DataRoot, "nvram"));

  if (s_arcade_boot)
    autoboot->boot_intent = SystemBootIntent::PublicArcade;

  {
    Error startup_error;
    if (!System::Internal::PerformEarlyHardwareChecks(&startup_error) ||
        !System::Internal::ProcessStartup(&startup_error) || !System::Internal::CPUThreadInitialize(&startup_error))
    {
      ERROR_LOG("CPUThreadInitialize() failed: {}", startup_error.GetDescription());
      return EXIT_FAILURE;
    }
  }

  RegTestHost::HookSignals();

  Error error;
  int result = -1;
  INFO_LOG("Trying to boot '{}'...", autoboot->filename);
  if (!System::BootSystem(std::move(autoboot.value()), &error))
  {
    ERROR_LOG("Failed to boot system: {}", error.GetDescription());
    goto cleanup;
  }

  if (s_frame_dump_interval > 0)
  {
    if (s_dump_base_directory.empty())
    {
      ERROR_LOG("Dump directory not specified.");
      goto cleanup;
    }

    INFO_LOG("Dumping every {}th frame to '{}'.", s_frame_dump_interval, s_dump_base_directory);
  }

  INFO_LOG("Running for {} frames...", s_frames_to_run);
  s_frames_remaining = s_frames_to_run;

  {
    const Common::Timer::Value start_time = Common::Timer::GetCurrentValue();

    System::Execute();

    const Common::Timer::Value elapsed_time = Common::Timer::GetCurrentValue() - start_time;
    const double elapsed_time_ms = Common::Timer::ConvertValueToMilliseconds(elapsed_time);
    INFO_LOG("Total execution time: {:.2f}ms, average frame time {:.2f}ms, {:.2f} FPS", elapsed_time_ms,
             elapsed_time_ms / static_cast<double>(s_frames_to_run),
             static_cast<double>(s_frames_to_run) / elapsed_time_ms * 1000.0);
  }

  if (RegTestScript::s_failures > 0)
  {
    ERROR_LOG("REGTEST {} check(s) failed.", RegTestScript::s_failures);
    result = 2;
    goto cleanup;
  }

  INFO_LOG("Exiting with success.");
  result = 0;

cleanup:
  System::Internal::CPUThreadShutdown();
  System::Internal::ProcessShutdown();
  return result;
}
