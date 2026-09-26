// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "consolesettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/game_database.h"

#include "util/cd_image.h"


ConsoleSettingsWidget::ConsoleSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  if (dialog->isArcadeGameSettings())
  {
    m_ui.groupBox->setTitle(tr("Machine Overrides"));

    m_ui.groupBox_4->setTitle(tr("Optical Media"));
    m_ui.cdromLoadImagePatches->setVisible(false);
  }

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
  {
    m_ui.cpuExecutionMode->addItem(
      QString::fromUtf8(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i))));
  }

  static constexpr float TIME_PER_SECTOR_DOUBLE_SPEED = 1000.0f / 150.0f;
  m_ui.cdromReadaheadSectors->addItem(tr("Disabled (Synchronous)"));
  for (u32 i = 1; i <= 32; i++)
  {
    m_ui.cdromReadaheadSectors->addItem(tr("%1 sectors (%2 KB / %3 ms)")
                                          .arg(i)

                                          .arg(static_cast<float>(i) * TIME_PER_SECTOR_DOUBLE_SPEED, 0, 'f', 0)
                                          .arg(static_cast<float>(i * CDImage::DATA_SECTOR_SIZE) / 1024.0f));
  }

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableAllEnhancements, "Main", "DisableAllEnhancements",
                                               false);

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.cpuExecutionMode, "CPU", "ExecutionMode",
                                               &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName,
                                               Settings::DEFAULT_CPU_EXECUTION_MODE);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromReadaheadSectors, "CDROM", "ReadaheadSectors",
                                              Settings::DEFAULT_CDROM_READAHEAD_SECTORS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromLoadImageToRAM, "CDROM", "LoadImageToRAM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.cdromLoadImagePatches, "CDROM", "LoadImagePatches", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vsync, "Display", "VSync", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.syncToHostRefreshRate, "Main", "SyncToHostRefreshRate", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.optimalFramePacing, "Display", "OptimalFramePacing", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.preFrameSleep, "Display", "PreFrameSleep", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.skipPresentingDuplicateFrames, "Display",
                                               "SkipPresentingDuplicateFrames", false);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.preFrameSleepBuffer, "Display", "PreFrameSleepBuffer",
                                                Settings::DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER);

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromSeekSpeedup, "CDROM", "SeekSpeedup", 1);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.cdromReadSpeedup, "CDROM", "ReadSpeedup", 1, 1);

  dialog->registerWidgetHelp(m_ui.cpuExecutionMode, tr("Execution Mode"), tr("Recompiler (Fastest)"),
                             tr("Determines how the emulated CPU executes instructions."));

  dialog->registerWidgetHelp(m_ui.disableAllEnhancements, tr("Disable All Enhancements"), tr("Unchecked"),
                             tr("Disables all enhancement options, simulating the system as accurately as possible. "
                                "Use to quickly determine whether an enhancement is responsible for game bugs."));
  dialog->registerWidgetHelp(
    m_ui.cdromLoadImageToRAM, tr("Preload Image to RAM"), tr("Unchecked"),
    tr("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay. In some "
       "cases also eliminates stutter when games initiate audio track playback."));
  dialog->registerWidgetHelp(
    m_ui.cdromReadSpeedup, tr("CD-ROM Read Speedup"), tr("None (Double Speed)"),
    tr("Speeds up CD-ROM reads by the specified factor. Only applies to double-speed reads, and is ignored when audio "
       "is playing. May improve loading speeds in some games, at the cost of breaking others."));
  dialog->registerWidgetHelp(
    m_ui.cdromSeekSpeedup, tr("CD-ROM Seek Speedup"), tr("None (Normal Speed)"),
    tr("Reduces the simulated time for the CD-ROM sled to move to different areas of the disc. Can improve loading "
       "times, but crash games which do not expect the CD-ROM to operate faster."));
  dialog->registerWidgetHelp(m_ui.cdromReadaheadSectors, tr("Asynchronous Readahead"), tr("8 Sectors"),
                             tr("Reduces hitches in emulation by reading/decompressing CD data asynchronously on a "
                                "worker thread. Higher sector numbers can reduce spikes when streaming FMVs or audio "
                                "on slower storage or when using compression formats such as CHD."));
  dialog->registerWidgetHelp(
    m_ui.cdromLoadImageToRAM, tr("Preload Image to RAM"), tr("Unchecked"),
    tr("Loads the game image into RAM. Useful for network paths that may become unreliable during gameplay. In some "
       "cases also eliminates stutter when games initiate audio track playback."));
  dialog->registerWidgetHelp(m_ui.cdromLoadImagePatches, tr("Apply Image Patches"), tr("Unchecked"),
                             tr("Automatically applies patches to disc images when they are present in the same "
                                "directory. Currently only PPF patches are supported with this option."));
  dialog->registerWidgetHelp(
    m_ui.vsync, tr("Vertical Sync (VSync)"), tr("Unchecked"),
    tr("Synchronizes presentation of the console's frames to the host. Enabling may result in smoother animations, at "
       "the cost of increased input lag. <strong>GSync/FreeSync users should enable Optimal Frame Pacing "
       "instead.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.syncToHostRefreshRate, tr("Sync To Host Refresh Rate"), tr("Unchecked"),
    tr(
      "Adjusts the emulation speed so the console's refresh rate matches the host's refresh rate when VSync is "
      "enabled. This results in the smoothest animations possible, at the cost of potentially increasing the emulation "
      "speed by less than 1%. Sync To Host Refresh Rate will not take effect if the console's refresh rate is too far "
      "from the host's refresh rate. Users with variable refresh rate displays should disable this option."));
  dialog->registerWidgetHelp(
    m_ui.optimalFramePacing, tr("Optimal Frame Pacing"), tr("Unchecked"),
    tr("Enabling this option will ensure every frame the console renders is displayed to the screen, at a consistent "
       "rate, for optimal frame pacing. If you have a GSync/FreeSync display, enable this option. If you are having "
       "difficulties maintaining full speed, or are getting audio glitches, try disabling this option."));
  dialog->registerWidgetHelp(
    m_ui.preFrameSleep, tr("Reduce Input Latency"), tr("Unchecked"),
    tr("Reduces input latency by delaying the start of frame until closer to the presentation time. This may cause "
       "dropped frames on slower systems with higher frame time variance, if the buffer size is not sufficient."));
  dialog->registerWidgetHelp(m_ui.preFrameSleepBuffer, tr("Frame Time Buffer"),
                             tr("%1 ms").arg(Settings::DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUFFER),
                             tr("Specifies the amount of buffer time added, which reduces the additional sleep time "
                                "introduced. Higher values increase input latency, but decrease the risk of overrun, "
                                "or missed frames. Lower values require faster hardware."));
  dialog->registerWidgetHelp(
    m_ui.skipPresentingDuplicateFrames, tr("Skip Duplicate Frame Display"), tr("Unchecked"),
    tr("Skips the presentation/display of frames that are not unique. Can be combined with driver-level frame "
       "generation to increase perceptible frame rate. Can result in worse frame pacing, and is not compatible with "
       "syncing to host refresh."));

  connect(m_ui.vsync, &QCheckBox::checkStateChanged, this, &ConsoleSettingsWidget::updateSkipDuplicateFramesEnabled);
  connect(m_ui.syncToHostRefreshRate, &QCheckBox::checkStateChanged, this,
          &ConsoleSettingsWidget::updateSkipDuplicateFramesEnabled);
  connect(m_ui.optimalFramePacing, &QCheckBox::checkStateChanged, this,
          &ConsoleSettingsWidget::onOptimalFramePacingChanged);
  connect(m_ui.preFrameSleep, &QCheckBox::checkStateChanged, this, &ConsoleSettingsWidget::onPreFrameSleepChanged);

  SettingWidgetBinder::SetAvailability(m_ui.cpuExecutionModeLabel,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceInterpreter));
  SettingWidgetBinder::SetAvailability(m_ui.cpuExecutionMode,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceInterpreter));

  onOptimalFramePacingChanged();
  updateSkipDuplicateFramesEnabled();
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;


void ConsoleSettingsWidget::onOptimalFramePacingChanged()
{
  const bool optimal_frame_pacing_enabled = m_dialog->getEffectiveBoolValue("Display", "OptimalFramePacing", false);
  m_ui.preFrameSleep->setEnabled(optimal_frame_pacing_enabled);
  onPreFrameSleepChanged();
}

void ConsoleSettingsWidget::onPreFrameSleepChanged()
{
  const bool pre_frame_sleep_enabled = m_dialog->getEffectiveBoolValue("Display", "PreFrameSleep", false);
  const bool show_buffer_size = (m_ui.preFrameSleep->isEnabled() && pre_frame_sleep_enabled);
  m_ui.preFrameSleepBuffer->setVisible(show_buffer_size);
  m_ui.preFrameSleepBufferLabel->setVisible(show_buffer_size);
}

void ConsoleSettingsWidget::updateSkipDuplicateFramesEnabled()
{
  const bool vsync = m_dialog->getEffectiveBoolValue("Display", "VSync", false);
  const bool sync_to_host = m_dialog->getEffectiveBoolValue("Main", "SyncToHostRefreshRate", false) && vsync;
  m_ui.skipPresentingDuplicateFrames->setEnabled(!sync_to_host);
}
