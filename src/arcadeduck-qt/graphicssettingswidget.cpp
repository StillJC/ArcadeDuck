// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "graphicssettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/arcade/arcade_database.h"
#include "core/game_database.h"
#include "core/gpu.h"
#include "core/settings.h"

#include "util/media_capture.h"

#include <algorithm>
#include <optional>

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>

static QVariant GetMSAAModeValue(uint multisamples, bool ssaa)
{
  const uint userdata = (multisamples & 0x7FFFFFFFu) | (static_cast<uint>(ssaa) << 31);
  return QVariant(userdata);
}

static void DecodeMSAAModeValue(const QVariant& userdata, uint* multisamples, bool* ssaa)
{
  bool ok;
  const uint value = userdata.toUInt(&ok);
  if (!ok || value == 0)
  {
    *multisamples = 1;
    *ssaa = false;
    return;
  }

  *multisamples = value & 0x7FFFFFFFu;
  *ssaa = (value & (1u << 31)) != 0u;
}

GraphicsSettingsWidget::GraphicsSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);
  setupAdditionalUi();
  setupArcadeMonitorUi();
  removePlatformSpecificUi();

  // Rendering Tab

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.renderer, "GPU", "Renderer", &Settings::ParseRendererName,
                                               &Settings::GetRendererName, Settings::DEFAULT_GPU_RENDERER);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.textureFiltering, "GPU", "TextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.spriteTextureFiltering, "GPU", "SpriteTextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuDownsampleMode, "GPU", "DownsampleMode",
                                               &Settings::ParseDownsampleModeName, &Settings::GetDownsampleModeName,
                                               Settings::DEFAULT_GPU_DOWNSAMPLE_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayAspectRatio, "Display", "AspectRatio",
                                               &Settings::ParseDisplayAspectRatio, &Settings::GetDisplayAspectRatioName,
                                               Settings::DEFAULT_DISPLAY_ASPECT_RATIO);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.customAspectRatioNumerator, "Display",
                                              "CustomAspectRatioNumerator", 1);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.customAspectRatioDenominator, "Display",
                                              "CustomAspectRatioDenominator", 1);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.widescreenHack, "GPU", "WidescreenHack", false);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.displayDeinterlacing, "Display", "DeinterlacingMode", &Settings::ParseDisplayDeinterlacingMode,
    &Settings::GetDisplayDeinterlacingModeName, Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayCropMode, "Display", "CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
                                               Settings::DEFAULT_DISPLAY_CROP_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayScaling, "Display", "Scaling",
                                               &Settings::ParseDisplayScaling, &Settings::GetDisplayScalingName,
                                               Settings::DEFAULT_DISPLAY_SCALING);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.gpuDownsampleScale, "GPU", "DownsampleScale", 1);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.trueColor, "GPU", "TrueColor", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableInterlacing, "GPU", "DisableInterlacing", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpEnable, "GPU", "PGXPEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpDepthBuffer, "GPU", "PGXPDepthBuffer", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.force43For24Bit, "Display", "Force4_3For24Bit", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.chromaSmoothingFor24Bit, "GPU", "ChromaSmoothing24Bit", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.forceNTSCTimings, "GPU", "ForceNTSCTimings", false);

  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::updateRendererDependentOptions);
  connect(m_ui.textureFiltering, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::updateResolutionDependentOptions);
  connect(m_ui.displayAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onAspectRatioChanged);
  connect(m_ui.gpuDownsampleMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onDownsampleModeChanged);
  connect(m_ui.trueColor, &QCheckBox::checkStateChanged, this, &GraphicsSettingsWidget::onTrueColorChanged);
  connect(m_ui.pgxpEnable, &QCheckBox::checkStateChanged, this, &GraphicsSettingsWidget::updatePGXPSettingsEnabled);

  SettingWidgetBinder::SetAvailability(m_ui.renderer,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceSoftwareRenderer));
  SettingWidgetBinder::SetAvailability(m_ui.resolutionScale,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling));
  SettingWidgetBinder::SetAvailability(m_ui.textureFiltering,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTextureFiltering));
  SettingWidgetBinder::SetAvailability(m_ui.trueColor, !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTrueColor));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpEnable, !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXP));
  SettingWidgetBinder::SetAvailability(m_ui.disableInterlacing,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceInterlacing));
  SettingWidgetBinder::SetAvailability(m_ui.widescreenHack,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableWidescreen));
  SettingWidgetBinder::SetAvailability(m_ui.forceNTSCTimings,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableForceNTSCTimings));

  // Advanced Tab

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.exclusiveFullscreenControl, "Display", "ExclusiveFullscreenControl",
    &Settings::ParseDisplayExclusiveFullscreenControl, &Settings::GetDisplayExclusiveFullscreenControlName,
    Settings::DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayAlignment, "Display", "Alignment",
                                               &Settings::ParseDisplayAlignment, &Settings::GetDisplayAlignmentName,
                                               Settings::DEFAULT_DISPLAY_ALIGNMENT);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayRotation, "Display", "Rotation",
                                               &Settings::ParseDisplayRotation, &Settings::GetDisplayRotationName,
                                               Settings::DEFAULT_DISPLAY_ROTATION);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.gpuThread, "GPU", "UseThread", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.threadedPresentation, "GPU", "ThreadedPresentation",
                                               Settings::DEFAULT_THREADED_PRESENTATION);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableMailboxPresentation, "Display",
                                               "DisableMailboxPresentation", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.stretchDisplayVertically, "Display", "StretchVertically",
                                               false);
#ifdef _WIN32
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.blitSwapChain, "Display", "UseBlitSwapChain", false);
#endif
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuLineDetectMode, "GPU", "LineDetectMode",
                                               &Settings::ParseLineDetectModeName, &Settings::GetLineDetectModeName,
                                               Settings::DEFAULT_GPU_LINE_DETECT_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuWireframeMode, "GPU", "WireframeMode",
                                               Settings::ParseGPUWireframeMode, Settings::GetGPUWireframeModeName,
                                               Settings::DEFAULT_GPU_WIREFRAME_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.debanding, "GPU", "Debanding", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.scaledDithering, "GPU", "ScaledDithering", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useSoftwareRendererForReadbacks, "GPU",
                                               "UseSoftwareRendererForReadbacks", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.forceRoundedTexcoords, "GPU", "ForceRoundTextureCoordinates",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.accurateBlending, "GPU", "AccurateBlending", false);

  SettingWidgetBinder::SetAvailability(m_ui.scaledDithering,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableScaledDithering));

  // PGXP Tab

  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.pgxpGeometryTolerance, "GPU", "PGXPTolerance", -1.0f);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.pgxpDepthClearThreshold, "GPU", "PGXPDepthClearThreshold",
                                                Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpTextureCorrection, "GPU", "PGXPTextureCorrection", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpColorCorrection, "GPU", "PGXPColorCorrection", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpCulling, "GPU", "PGXPCulling", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpPreserveProjPrecision, "GPU", "PGXPPreserveProjFP", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpCPU, "GPU", "PGXPCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpVertexCache, "GPU", "PGXPVertexCache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpDisableOn2DPolygons, "GPU", "PGXPDisableOn2DPolygons",
                                               false);

  connect(m_ui.pgxpTextureCorrection, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::updatePGXPSettingsEnabled);
  connect(m_ui.pgxpDepthBuffer, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::updatePGXPSettingsEnabled);

  SettingWidgetBinder::SetAvailability(m_ui.pgxpTextureCorrection,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPTextureCorrection));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpColorCorrection,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPColorCorrection));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpCulling,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPCulling));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpPreserveProjPrecision,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPPreserveProjFP));

  // OSD Tab

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.osdScale, "Display", "OSDScale", 100);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showOSDMessages, "Display", "ShowOSDMessages", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFPS, "Display", "ShowFPS", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSpeed, "Display", "ShowSpeed", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showResolution, "Display", "ShowResolution", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showCPU, "Display", "ShowCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPU, "Display", "ShowGPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showInput, "Display", "ShowInputs", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPUStatistics, "Display", "ShowGPUStatistics", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showLatencyStatistics, "Display", "ShowLatencyStatistics",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showStatusIndicators, "Display", "ShowStatusIndicators", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFrameTimes, "Display", "ShowFrameTimes", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSettings, "Display", "ShowEnhancements", false);

  // Capture Tab

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.screenshotSize, "Display", "ScreenshotMode", &Settings::ParseDisplayScreenshotMode,
    &Settings::GetDisplayScreenshotModeName, Settings::DEFAULT_DISPLAY_SCREENSHOT_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.screenshotFormat, "Display", "ScreenshotFormat", &Settings::ParseDisplayScreenshotFormat,
    &Settings::GetDisplayScreenshotFormatName, Settings::DEFAULT_DISPLAY_SCREENSHOT_FORMAT);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.screenshotQuality, "Display", "ScreenshotQuality",
                                              Settings::DEFAULT_DISPLAY_SCREENSHOT_QUALITY);

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.mediaCaptureBackend, "MediaCapture", "Backend",
                                               &MediaCapture::ParseBackendName, &MediaCapture::GetBackendName,
                                               Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVideoCapture, "MediaCapture", "VideoCapture", true);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.videoCaptureWidth, "MediaCapture", "VideoWidth",
                                              Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.videoCaptureHeight, "MediaCapture", "VideoHeight",
                                              Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.videoCaptureResolutionAuto, "MediaCapture", "VideoAutoSize",
                                               false);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.videoCaptureBitrate, "MediaCapture", "VideoBitrate",
                                              Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVideoCaptureArguments, "MediaCapture",
                                               "VideoCodecUseArgs", false);
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.videoCaptureArguments, "MediaCapture", "AudioCodecArgs");
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableAudioCapture, "MediaCapture", "AudioCapture", true);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.audioCaptureBitrate, "MediaCapture", "AudioBitrate",
                                              Settings::DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVideoCaptureArguments, "MediaCapture",
                                               "VideoCodecUseArgs", false);
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.audioCaptureArguments, "MediaCapture", "AudioCodecArgs");

  connect(m_ui.mediaCaptureBackend, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onMediaCaptureBackendChanged);
  connect(m_ui.enableVideoCapture, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onMediaCaptureVideoEnabledChanged);
  connect(m_ui.videoCaptureResolutionAuto, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onMediaCaptureVideoAutoResolutionChanged);
  connect(m_ui.enableAudioCapture, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onMediaCaptureAudioEnabledChanged);

  // Texture Replacements Tab

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vramWriteReplacement, "TextureReplacements",
                                               "EnableVRAMWriteReplacements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.preloadTextureReplacements, "TextureReplacements",
                                               "PreloadTextures", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useOldMDECRoutines, "Hacks", "UseOldMDECRoutines", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vramWriteDumping, "TextureReplacements", "DumpVRAMWrites",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.setVRAMWriteAlphaChannel, "TextureReplacements",
                                               "DumpVRAMWriteForceAlphaChannel", true);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.minDumpedVRAMWriteWidth, "TextureReplacements",
                                              "DumpVRAMWriteWidthThreshold",
                                              Settings::DEFAULT_VRAM_WRITE_DUMP_WIDTH_THRESHOLD);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.minDumpedVRAMWriteHeight, "TextureReplacements",
                                              "DumpVRAMWriteHeightThreshold",
                                              Settings::DEFAULT_VRAM_WRITE_DUMP_HEIGHT_THRESHOLD);

  connect(m_ui.vramWriteReplacement, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableAnyTextureReplacementsChanged);
  connect(m_ui.vramWriteDumping, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableVRAMWriteDumpingChanged);

  // Debugging Tab

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useDebugDevice, "GPU", "UseDebugDevice", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableShaderCache, "GPU", "DisableShaderCache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableDualSource, "GPU", "DisableDualSourceBlend", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableFramebufferFetch, "GPU", "DisableFramebufferFetch",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableTextureBuffers, "GPU", "DisableTextureBuffers", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableTextureCopyToSelf, "GPU", "DisableTextureCopyToSelf",
                                               false);

  // Init all dependent options.
  updateRendererDependentOptions();
  onAspectRatioChanged();
  onDownsampleModeChanged();
  updateResolutionDependentOptions();
  onMediaCaptureBackendChanged();
  onMediaCaptureAudioEnabledChanged();
  onMediaCaptureVideoEnabledChanged();
  onEnableAnyTextureReplacementsChanged();
  onEnableVRAMWriteDumpingChanged();
  onShowDebugSettingsChanged(QtHost::ShouldShowDebugOptions());

  // Rendering Tab

  dialog->registerWidgetHelp(
    m_ui.renderer, tr("Renderer"), QString::fromUtf8(Settings::GetRendererDisplayName(Settings::DEFAULT_GPU_RENDERER)),
    tr("Selects the graphics API used by the host PC. Automatic is recommended and lets ArcadeDuck choose a supported "
       "renderer. Select a specific API only for driver compatibility or troubleshooting."));
  dialog->registerWidgetHelp(
    m_ui.adapter, tr("Adapter"), tr("(Default)"),
    tr("Selects which host GPU ArcadeDuck uses for hardware rendering. Default is recommended. Choose another adapter "
       "only on a multi-GPU system or for troubleshooting."));
  dialog->registerWidgetHelp(
    m_ui.resolutionScale, tr("Internal Resolution"), tr("1x Native"),
    tr("Controls the resolution used for 3D rendering. 1x Native reproduces the original arcade hardware output and is "
       "the ArcadeDuck default. Higher values sharpen 3D graphics but can expose rendering errors or change the "
       "original appearance."));
  dialog->registerWidgetHelp(
    m_ui.gpuDownsampleMode, tr("Down-Sampling"), tr("Disabled"),
    tr("Reduces an internally upscaled image before presentation. It has no benefit at 1x Native and is disabled by "
       "default."));
  dialog->registerWidgetHelp(m_ui.gpuDownsampleScale, tr("Down-Sampling Display Scale"), tr("1x"),
                             tr("Controls the final scale used by Box downsampling. 1x returns the upscaled image to "
                                "the original hardware resolution."));
  dialog->registerWidgetHelp(
    m_ui.textureFiltering, tr("Texture Filtering"),
    QString::fromUtf8(Settings::GetTextureFilterDisplayName(Settings::DEFAULT_GPU_TEXTURE_FILTER)),
    tr("Controls filtering for 3D textures. Nearest-Neighbor preserves the original texture sampling and is the "
       "ArcadeDuck default. Smoother filters can blur artwork or create texture seams."));
  dialog->registerWidgetHelp(
    m_ui.spriteTextureFiltering, tr("Sprite Texture Filtering"),
    QString::fromUtf8(Settings::GetTextureFilterDisplayName(Settings::DEFAULT_GPU_TEXTURE_FILTER)),
    tr("Controls filtering for sprites, HUD elements, and other 2D artwork. Nearest-Neighbor is recommended to keep "
       "text and overlays sharp."));
  dialog->registerWidgetHelp(
    m_ui.displayAspectRatio, tr("Aspect Ratio"),
    QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO)),
    tr("Controls the physical shape of the displayed image. Auto uses the game or arcade hardware's native presentation "
       "rules and is recommended. ArcadeDuck may apply hardware-specific aspect handling when raw pixel dimensions are "
       "not the intended display shape."));
  dialog->registerWidgetHelp(
    m_ui.displayDeinterlacing, tr("Deinterlacing"),
    QString::fromUtf8(Settings::GetDisplayDeinterlacingModeName(Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE)),
    tr("Controls how interlaced video modes are presented. Disabled preserves native behavior and is the default. "
       "Enable a deinterlacing method per game when visible combing or flicker requires it."));
  dialog->registerWidgetHelp(
    m_ui.displayCropMode, tr("Crop"),
    QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(Settings::DEFAULT_DISPLAY_CROP_MODE)),
    tr("Controls which programmed border areas are shown. All Borders removes inactive borders and is the ArcadeDuck "
       "default. Only Overscan Area applies a television-safe crop and may remove boot or game content. "
       "None preserves the complete programmed display canvas."));
  dialog->registerWidgetHelp(
    m_ui.displayScaling, tr("Scaling"), tr("Bilinear (Smooth)"),
    tr("Controls how the final image is resized to the host display. Bilinear (Smooth) is recommended for aspect-correct "
       "arcade output and non-square pixels. Nearest-Neighbor is sharper but can produce uneven pixels at non-integer "
       "scales."));
  dialog->registerWidgetHelp(
    m_ui.trueColor, tr("True Color Rendering"), tr("Unchecked"),
    tr("Replaces the original 15-bit color and dithering with higher-precision color. Leave disabled for "
       "hardware-accurate arcade presentation."));
  dialog->registerWidgetHelp(
    m_ui.widescreenHack, tr("Widescreen Rendering"), tr("Unchecked"),
    tr("Expands supported 3D scenes beyond their original aspect ratio. It is disabled by default because it changes "
       "the intended presentation and can reveal missing or incorrect geometry."));
  dialog->registerWidgetHelp(m_ui.pgxpEnable, tr("PGXP Geometry Correction"), tr("Unchecked"),
                             tr("Optional enhancement for PlayStation-derived 3D graphics. It changes original polygon "
                                "and texture behavior and may cause compatibility problems. It is disabled by default "
                                "in ArcadeDuck."));
  dialog->registerWidgetHelp(
    m_ui.pgxpDepthBuffer, tr("PGXP Depth Buffer"), tr("Unchecked"),
    tr("Optional PGXP enhancement that uses depth data to improve polygon ordering. It only matters while PGXP Geometry "
       "Correction is enabled. Compatibility is limited, so leave it disabled unless a specific game has been "
       "verified."));
  dialog->registerWidgetHelp(
    m_ui.force43For24Bit, tr("Force 4:3 For FMVs"), tr("Unchecked"),
    tr("Forces 24-bit video playback to 4:3. Leave disabled by default because Auto aspect and hardware-specific rules "
       "normally determine the correct presentation."));
  dialog->registerWidgetHelp(m_ui.chromaSmoothingFor24Bit, tr("FMV Chroma Smoothing"), tr("Unchecked"),
                             tr("Smooths blocky color transitions in full-motion video. This is an optional visual "
                                "enhancement and is disabled by default."));
  dialog->registerWidgetHelp(
    m_ui.disableInterlacing, tr("Disable Interlacing"), tr("Unchecked"),
    tr("Forces compatible interlaced modes into progressive output. It can break games that rely on interlacing and is "
       "disabled by default."));
  dialog->registerWidgetHelp(
    m_ui.forceNTSCTimings, tr("Force NTSC Timings"), tr("Unchecked"),
    tr("Forces PAL software to use NTSC timing. This changes game speed and video timing and must remain disabled unless "
       "explicitly required."));

  // Advanced Tab

  dialog->registerWidgetHelp(
    m_ui.fullscreenMode, tr("Fullscreen Mode"), tr("Borderless Fullscreen"),
    tr("Selects borderless fullscreen or a specific exclusive resolution and refresh rate. Borderless Fullscreen is "
       "the ArcadeDuck default and uses the desktop display mode. Choose an exclusive mode only when required by a "
       "display or driver."));
  dialog->registerWidgetHelp(
    m_ui.exclusiveFullscreenControl, tr("Exclusive Fullscreen Control"), tr("Automatic"),
    tr("Controls whether supported renderers may use exclusive fullscreen. Automatic is recommended and lets "
       "ArcadeDuck follow renderer and driver support."));
  dialog->registerWidgetHelp(
    m_ui.displayAlignment, tr("Screen Position"),
    QString::fromUtf8(Settings::GetDisplayAlignmentDisplayName(Settings::DEFAULT_DISPLAY_ALIGNMENT)),
    tr("Controls where the arcade image is placed when aspect-correct borders are required. Center is the ArcadeDuck "
       "default."));
  dialog->registerWidgetHelp(
    m_ui.displayRotation, tr("Rotation"),
    QString::fromUtf8(Settings::GetDisplayRotationDisplayName(Settings::DEFAULT_DISPLAY_ROTATION)),
    tr("Rotates the displayed arcade image. No Rotation is the default; choose another orientation only for games or "
       "cabinets mounted that way."));
  dialog->registerWidgetHelp(m_ui.gpuThread, tr("Threaded Rendering"), tr("Checked"),
                             tr("Processes rendering work on a separate thread. It is recommended and enabled by "
                                "default because it generally improves performance without changing emulation "
                                "behavior."));
  dialog->registerWidgetHelp(m_ui.threadedPresentation, tr("Threaded Presentation"), tr("Checked"),
                             tr("Presents frames on a background thread when fast forwarding or vsync is disabled. "
                                "This can measurably improve performance in the Vulkan renderer."));
  dialog->registerWidgetHelp(
    m_ui.disableMailboxPresentation, tr("Disable Mailbox Presentation"), tr("Unchecked"),
    tr("Disables low-latency mailbox presentation and uses a fallback presentation path. This can increase latency or "
       "worsen frame pacing. Leave disabled unless required by a specific GPU driver or display."));
  dialog->registerWidgetHelp(
    m_ui.stretchDisplayVertically, tr("Stretch Vertically"), tr("Unchecked"),
    tr("Stretches the image to fill more of the host window vertically. This overrides the calculated presentation "
       "shape and is disabled by default."));
#ifdef _WIN32
  dialog->registerWidgetHelp(m_ui.blitSwapChain, tr("Use Blit Swap Chain"), tr("Unchecked"),
                             tr("Uses a compatibility-oriented presentation path that is generally slower. Leave "
                                "disabled unless the normal swap-chain path fails on a specific system."));
#endif

  dialog->registerWidgetHelp(m_ui.gpuLineDetectMode, tr("Line Detection"),
                             QString::fromUtf8(Settings::GetLineDetectModeName(Settings::DEFAULT_GPU_LINE_DETECT_MODE)),
                             tr("Attempts to preserve thin polygon lines when rendering above native resolution. It is "
                                "unnecessary at 1x Native and is disabled by default."));
  dialog->registerWidgetHelp(
    m_ui.msaaMode, tr("Multi-Sampling"), tr("Disabled"),
    tr("Applies anti-aliasing to internally rendered polygons. It can introduce rendering errors and is disabled by "
       "default."));
  dialog->registerWidgetHelp(
    m_ui.debanding, tr("True Color Debanding"), tr("Unchecked"),
    tr("Reduces visible color banding when True Color Rendering is enabled. It has no effect on the default "
       "hardware-accurate color path."));
  dialog->registerWidgetHelp(
    m_ui.scaledDithering, tr("Scaled Dithering"), tr("Checked"),
    tr("Scales the original dithering pattern with higher internal resolutions. It remains enabled so dithering behaves "
       "consistently when a user raises resolution."));
  dialog->registerWidgetHelp(
    m_ui.useSoftwareRendererForReadbacks, tr("Software Renderer Readbacks"), tr("Unchecked"),
    tr("Uses the software renderer for GPU readbacks to improve compatibility in specific games. It can reduce "
       "performance and is disabled globally."));
  dialog->registerWidgetHelp(
    m_ui.forceRoundedTexcoords, tr("Round Upscaled Texture Coordinates"), tr("Unchecked"),
    tr("Adjusts texture coordinates to reduce seams at higher internal resolutions. It is disabled by default because "
       "it can break effects in some games."));
  dialog->registerWidgetHelp(
    m_ui.accurateBlending, tr("Accurate Blending"), tr("Unchecked"),
    tr("Uses a more accurate but potentially slower blending path. Leave disabled globally and enable it through a "
       "verified per-game compatibility override when necessary."));

  // PGXP Tab

  dialog->registerWidgetHelp(
    m_ui.pgxpGeometryTolerance, tr("Geometry Tolerance"), tr("-1.00px (Disabled)"),
    tr("Optional PGXP compatibility workaround that discards corrected geometry beyond the selected offset. It only "
       "matters while PGXP Geometry Correction is enabled; leave it disabled unless a game has verified geometry "
       "issues."));
  dialog->registerWidgetHelp(m_ui.pgxpDepthClearThreshold, tr("Depth Clear Threshold"),
                             QStringLiteral("%1").arg(Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD),
                             tr("Optional PGXP depth-buffer compatibility threshold. It only matters while PGXP "
                                "Geometry Correction and PGXP Depth Buffer are enabled; adjust it only for a verified "
                                "game."));
  dialog->registerWidgetHelp(m_ui.pgxpTextureCorrection, tr("Perspective Correct Textures"), tr("Checked"),
                             tr("Optional PGXP enhancement that applies perspective-correct texture coordinates. It "
                                "only matters while PGXP Geometry Correction is enabled and may cause compatibility "
                                "problems."));
  dialog->registerWidgetHelp(
    m_ui.pgxpColorCorrection, tr("Perspective Correct Colors"), tr("Unchecked"),
    tr("Optional PGXP enhancement that applies perspective-correct vertex colors. It only matters while PGXP Geometry "
       "Correction and Perspective Correct Textures are enabled, and can cause rendering errors."));
  dialog->registerWidgetHelp(m_ui.pgxpCulling, tr("Culling Correction"), tr("Checked"),
                             tr("Optional PGXP enhancement that increases polygon-culling precision. It only matters "
                                "while PGXP Geometry Correction is enabled and can reduce holes in geometry."));
  dialog->registerWidgetHelp(
    m_ui.pgxpPreserveProjPrecision, tr("Preserve Projection Precision"), tr("Unchecked"),
    tr("Optional PGXP enhancement that preserves additional post-projection precision. It only matters while PGXP "
       "Geometry Correction is enabled and is game-dependent."));
  dialog->registerWidgetHelp(m_ui.pgxpCPU, tr("CPU Mode"), tr("Unchecked"),
                             tr("Optional PGXP compatibility mode that applies correction to all CPU instructions. It "
                                "only matters while PGXP Geometry Correction is enabled and has a high performance "
                                "cost."));
  dialog->registerWidgetHelp(
    m_ui.pgxpVertexCache, tr("Vertex Cache"), tr("Unchecked"),
    tr("Optional PGXP compatibility workaround that resolves data from screen-space vertex positions. It only matters "
       "while PGXP Geometry Correction is enabled and generally provides no benefit."));
  dialog->registerWidgetHelp(m_ui.pgxpDisableOn2DPolygons, tr("Disable on 2D Polygons"), tr("Unchecked"),
                             tr("Optional PGXP compatibility workaround that uses native coordinates for 2D polygons. "
                                "It only matters while PGXP Geometry Correction is enabled and should remain disabled "
                                "unless a game's 2D elements are misaligned."));

  // OSD Tab

  dialog->registerWidgetHelp(
    m_ui.osdScale, tr("OSD Scale"), tr("100%"),
    tr("Changes the size at which on-screen elements, including status and messages are displayed."));
  dialog->registerWidgetHelp(m_ui.showOSDMessages, tr("Show OSD Messages"), tr("Checked"),
                             tr("Shows on-screen-display messages when events occur such as save states being "
                                "created/loaded, screenshots being taken, etc."));
  dialog->registerWidgetHelp(m_ui.showResolution, tr("Show Resolution"), tr("Unchecked"),
                             tr("Shows the resolution of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showSpeed, tr("Show Emulation Speed"), tr("Unchecked"),
    tr("Shows the current emulation speed of the system in the top-right corner of the display as a percentage."));
  dialog->registerWidgetHelp(m_ui.showFPS, tr("Show FPS"), tr("Unchecked"),
                             tr("Shows the internal frame rate of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showCPU, tr("Show CPU Usage"), tr("Unchecked"),
    tr("Shows the host's CPU usage based on threads in the top-right corner of the display. This does not display the "
       "emulated system CPU's usage. If a value close to 100% is being displayed, this means your host's CPU is likely "
       "the bottleneck. In this case, you should reduce enhancement-related settings such as overclocking."));
  dialog->registerWidgetHelp(m_ui.showGPU, tr("Show GPU Usage"), tr("Unchecked"),
                             tr("Shows the host's GPU usage in the top-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showGPUStatistics, tr("Show GPU Statistics"), tr("Unchecked"),
                             tr("Shows information about the emulated GPU in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showLatencyStatistics, tr("Show Latency Statistics"), tr("Unchecked"),
    tr("Shows information about input and audio latency in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showFrameTimes, tr("Show Frame Times"), tr("Unchecked"),
    tr("Shows the history of frame rendering times as a graph in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showInput, tr("Show Controller Input"), tr("Unchecked"),
    tr("Shows the current controller state of the system in the bottom-left corner of the display."));
  dialog->registerWidgetHelp(m_ui.showSettings, tr("Show Settings"), tr("Unchecked"),
                             tr("Shows a summary of current settings in the bottom-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showStatusIndicators, tr("Show Status Indicators"), tr("Checked"),
                             tr("Shows indicators on screen when the system is not running in its \"normal\" state. "
                                "For example, fast forwarding, or being paused."));

  // Capture Tab

  dialog->registerWidgetHelp(m_ui.screenshotSize, tr("Screenshot Size"), tr("Screen Resolution"),
                             tr("Determines the resolution at which screenshots will be saved. Internal resolutions "
                                "preserve more detail at the cost of file size."));
  dialog->registerWidgetHelp(
    m_ui.screenshotFormat, tr("Screenshot Format"), tr("PNG"),
    tr("Selects the format which will be used to save screenshots. JPEG produces smaller files, but loses detail."));
  dialog->registerWidgetHelp(m_ui.screenshotQuality, tr("Screenshot Quality"),
                             QStringLiteral("%1%").arg(Settings::DEFAULT_DISPLAY_SCREENSHOT_QUALITY),
                             tr("Selects the quality at which screenshots will be compressed. Higher values preserve "
                                "more detail for JPEG, and reduce file size for PNG."));
  dialog->registerWidgetHelp(
    m_ui.mediaCaptureBackend, tr("Backend"),
    QString::fromUtf8(MediaCapture::GetBackendDisplayName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND)),
    tr("Selects the framework that is used to encode video/audio."));
  dialog->registerWidgetHelp(m_ui.captureContainer, tr("Container"), tr("MP4"),
                             tr("Determines the file format used to contain the captured audio/video"));
  dialog->registerWidgetHelp(
    m_ui.videoCaptureCodec, tr("Video Codec"), tr("Default"),
    tr("Selects which Video Codec to be used for Video Capture. <b>If unsure, leave it on default.<b>"));
  dialog->registerWidgetHelp(m_ui.videoCaptureBitrate, tr("Video Bitrate"), tr("6000 kbps"),
                             tr("Sets the video bitrate to be used. Larger bitrate generally yields better video "
                                "quality at the cost of larger resulting file size."));
  dialog->registerWidgetHelp(
    m_ui.videoCaptureResolutionAuto, tr("Automatic Resolution"), tr("Unchecked"),
    tr("When checked, the video capture resolution will follows the internal resolution of the running "
       "game. <b>Be careful when using this setting especially when you are upscaling, as higher internal "
       "resolutions (above 4x) can cause system slowdown.</b>"));
  dialog->registerWidgetHelp(m_ui.enableVideoCaptureArguments, tr("Enable Extra Video Arguments"), tr("Unchecked"),
                             tr("Allows you to pass arguments to the selected video codec."));
  dialog->registerWidgetHelp(
    m_ui.videoCaptureArguments, tr("Extra Video Arguments"), tr("Empty"),
    tr("Parameters passed to the selected video codec.<br><b>You must use '=' to separate key from value and ':' to "
       "separate two pairs from each other.</b><br>For example: \"crf = 21 : preset = veryfast\""));
  dialog->registerWidgetHelp(
    m_ui.audioCaptureCodec, tr("Audio Codec"), tr("Default"),
    tr("Selects which Audio Codec to be used for Video Capture. <b>If unsure, leave it on default.<b>"));
  dialog->registerWidgetHelp(m_ui.audioCaptureBitrate, tr("Audio Bitrate"), tr("160 kbps"),
                             tr("Sets the audio bitrate to be used."));
  dialog->registerWidgetHelp(m_ui.enableAudioCaptureArguments, tr("Enable Extra Audio Arguments"), tr("Unchecked"),
                             tr("Allows you to pass arguments to the selected audio codec."));
  dialog->registerWidgetHelp(
    m_ui.audioCaptureArguments, tr("Extra Audio Arguments"), tr("Empty"),
    tr("Parameters passed to the selected audio codec.<br><b>You must use '=' to separate key from value and ':' to "
       "separate two pairs from each other.</b><br>For example: \"compression_level = 4 : joint_stereo = 1\""));

  // Texture Replacements Tab

  dialog->registerWidgetHelp(m_ui.vramWriteReplacement, tr("Enable VRAM Write Replacement"), tr("Unchecked"),
                             tr("Enables the replacement of background textures in supported games. <strong>This is "
                                "not general texture replacement.</strong>"));
  dialog->registerWidgetHelp(m_ui.preloadTextureReplacements, tr("Preload Texture Replacements"), tr("Unchecked"),
                             tr("Loads all replacement texture to RAM, reducing stuttering at runtime."));
  dialog->registerWidgetHelp(m_ui.useOldMDECRoutines, tr("Use Old MDEC Routines"), tr("Unchecked"),
                             tr("Enables the older, less accurate MDEC decoding routines. May be required for old "
                                "replacement backgrounds to match/load."));
  dialog->registerWidgetHelp(m_ui.setVRAMWriteAlphaChannel, tr("Set Alpha Channel"), tr("Checked"),
                             tr("Clears the mask/transparency bit in VRAM write dumps."));
  dialog->registerWidgetHelp(m_ui.vramWriteDumping, tr("Enable VRAM Write Dumping"), tr("Unchecked"),
                             tr("Writes backgrounds that can be replaced to the dump directory."));
  dialog->registerWidgetHelp(m_ui.minDumpedVRAMWriteWidth, tr("Dump Size Threshold"), tr("128px"),
                             tr("Determines the threshold that triggers a VRAM write to be dumped."));
  dialog->registerWidgetHelp(m_ui.minDumpedVRAMWriteHeight, tr("Dump Size Threshold"), tr("128px"),
                             tr("Determines the threshold that triggers a VRAM write to be dumped."));

  // Debugging Tab

  dialog->registerWidgetHelp(m_ui.gpuWireframeMode, tr("Wireframe Mode"), tr("Disabled"),
                             tr("Draws a wireframe outline of the triangles rendered by the console's GPU, either as a "
                                "replacement or an overlay."));

  dialog->registerWidgetHelp(
    m_ui.useDebugDevice, tr("Use Debug Device"), tr("Unchecked"),
    tr("Enable debugging when supported by the host's renderer API. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.disableShaderCache, tr("Disable Shader Cache"), tr("Unchecked"),
    tr("Forces shaders to be compiled for every run of the program. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableDualSource, tr("Disable Dual-Source Blending"), tr("Unchecked"),
                             tr("Prevents dual-source blending from being used. Useful for testing broken graphics "
                                "drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableFramebufferFetch, tr("Disable Framebuffer Fetch"), tr("Unchecked"),
                             tr("Prevents the framebuffer fetch extensions from being used. Useful for testing broken "
                                "graphics drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.disableTextureBuffers, tr("Disable Texture Buffers"), tr("Unchecked"),
    tr("Forces VRAM updates through texture updates, instead of texture buffers and draws. Useful for testing broken "
       "graphics drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableTextureCopyToSelf, tr("Disable Texture Copies To Self"), tr("Unchecked"),
                             tr("Disables the use of self-copy updates for the VRAM texture. Useful for testing broken "
                                "graphics drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableMemoryImport, tr("Disable Memory Import"), tr("Unchecked"),
                             tr("Disables the use of host memory importing. Useful for testing broken graphics "
                                "drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableRasterOrderViews, tr("Disable Rasterizer Order Views"), tr("Unchecked"),
                             tr("Disables the use of rasterizer order views. Useful for testing broken graphics "
                                "drivers. <strong>Only for developer use.</strong>"));
}

GraphicsSettingsWidget::~GraphicsSettingsWidget() = default;

void GraphicsSettingsWidget::setupArcadeMonitorUi()
{
  if (!m_dialog->isArcadeGameSettings())
    return;

  const Arcade::Database::GameDefinition* const game = Arcade::Database::GetGame(m_dialog->getGameSerial());
  if (!game)
    return;

  const int database_crop_top = game->display.monitor_crop_top;
  const int database_crop_bottom = game->display.monitor_crop_bottom;
  const int database_offset_x = game->display.monitor_offset_x;

  QGroupBox* const group = new QGroupBox(tr("Arcade Monitor Adjustment"), m_ui.basicTab);
  QVBoxLayout* const group_layout = new QVBoxLayout(group);
  QLabel* const description = new QLabel(
    tr("Fine-tune the visible arcade monitor area for this game. Values from ArcadeDB are shown unless a per-game "
       "override exists. Negative horizontal values move the image left; positive values move it right."),
    group);
  description->setWordWrap(true);
  group_layout->addWidget(description);

  QFormLayout* const form = new QFormLayout();
  group_layout->addLayout(form);

  QPushButton* const restore_button = new QPushButton(tr("Restore ArcadeDB Defaults"), group);
  const auto has_overrides = [this]() {
    return m_dialog->containsSettingValue("Display", "ArcadeMonitorCropTop") ||
           m_dialog->containsSettingValue("Display", "ArcadeMonitorCropBottom") ||
           m_dialog->containsSettingValue("Display", "ArcadeMonitorOffsetX");
  };
  restore_button->setEnabled(has_overrides());

  const auto add_adjustment =
    [this, group, form, restore_button, has_overrides](const QString& label, const char* key, int database_default,
                                                       int minimum, int maximum, const QString& help_text) {
      QSpinBox* const spin = new QSpinBox(group);
      spin->setRange(minimum, maximum);
      const std::optional<int> override_value = m_dialog->getIntValue("Display", key, std::nullopt);
      spin->setValue(std::clamp(override_value.value_or(database_default), minimum, maximum));

      connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
              [this, key, database_default, restore_button, has_overrides](int value) {
                m_dialog->setIntSettingValue("Display", key,
                                             value == database_default ? std::nullopt : std::optional<int>(value));
                restore_button->setEnabled(has_overrides());
              });

      form->addRow(label, spin);
      m_dialog->registerWidgetHelp(spin, label, tr("ArcadeDB: %1").arg(database_default), help_text);
      return spin;
    };

  QSpinBox* const crop_top =
    add_adjustment(tr("Top Crop:"), "ArcadeMonitorCropTop", database_crop_top, 0, 64,
                   tr("Crops scanlines from the top edge of the arcade image for this game."));
  QSpinBox* const crop_bottom =
    add_adjustment(tr("Bottom Crop:"), "ArcadeMonitorCropBottom", database_crop_bottom, 0, 64,
                   tr("Crops scanlines from the bottom edge of the arcade image for this game."));
  QSpinBox* const offset_x =
    add_adjustment(tr("Horizontal Position:"), "ArcadeMonitorOffsetX", database_offset_x, -64, 64,
                   tr("Moves the arcade image horizontally. Negative values move left; positive values move right."));

  connect(restore_button, &QPushButton::clicked, this,
          [this, crop_top, crop_bottom, offset_x, database_crop_top, database_crop_bottom, database_offset_x,
           restore_button]() {
            INISettingsInterface* const sif = m_dialog->getSettingsInterface();
            if (!sif)
              return;

            sif->DeleteValue("Display", "ArcadeMonitorCropTop");
            sif->DeleteValue("Display", "ArcadeMonitorCropBottom");
            sif->DeleteValue("Display", "ArcadeMonitorOffsetX");
            m_dialog->saveAndReloadGameSettings();

            const QSignalBlocker block_top(crop_top);
            const QSignalBlocker block_bottom(crop_bottom);
            const QSignalBlocker block_offset(offset_x);
            crop_top->setValue(database_crop_top);
            crop_bottom->setValue(database_crop_bottom);
            offset_x->setValue(database_offset_x);
            restore_button->setEnabled(false);
          });

  group_layout->addWidget(restore_button, 0, Qt::AlignRight);
  m_dialog->registerWidgetHelp(
    restore_button, tr("Restore ArcadeDB Defaults"), tr("Recommended"),
    tr("Removes the three per-game arcade monitor overrides so this game follows ArcadeDB again."));

  m_ui.verticalLayout_4->insertWidget(m_ui.verticalLayout_4->count() - 1, group);
}

void GraphicsSettingsWidget::setupAdditionalUi()
{
  // Rendering Tab

  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    m_ui.renderer->addItem(QString::fromUtf8(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPUTextureFilter::Count); i++)
  {
    m_ui.textureFiltering->addItem(
      QString::fromUtf8(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
    m_ui.spriteTextureFiltering->addItem(
      QString::fromUtf8(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPUDownsampleMode::Count); i++)
  {
    m_ui.gpuDownsampleMode->addItem(
      QString::fromUtf8(Settings::GetDownsampleModeDisplayName(static_cast<GPUDownsampleMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.displayAspectRatio->addItem(
      QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(static_cast<DisplayAspectRatio>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayDeinterlacingMode::Count); i++)
  {
    m_ui.displayDeinterlacing->addItem(
      QString::fromUtf8(Settings::GetDisplayDeinterlacingModeDisplayName(static_cast<DisplayDeinterlacingMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    m_ui.displayCropMode->addItem(
      QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayScalingMode::Count); i++)
  {
    m_ui.displayScaling->addItem(
      QString::fromUtf8(Settings::GetDisplayScalingDisplayName(static_cast<DisplayScalingMode>(i))));
  }

  // Advanced Tab

  for (u32 i = 0; i < static_cast<u32>(DisplayExclusiveFullscreenControl::Count); i++)
  {
    m_ui.exclusiveFullscreenControl->addItem(QString::fromUtf8(
      Settings::GetDisplayExclusiveFullscreenControlDisplayName(static_cast<DisplayExclusiveFullscreenControl>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayAlignment::Count); i++)
  {
    m_ui.displayAlignment->addItem(
      QString::fromUtf8(Settings::GetDisplayAlignmentDisplayName(static_cast<DisplayAlignment>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayRotation::Count); i++)
  {
    m_ui.displayRotation->addItem(
      QString::fromUtf8(Settings::GetDisplayRotationDisplayName(static_cast<DisplayRotation>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPULineDetectMode::Count); i++)
  {
    m_ui.gpuLineDetectMode->addItem(
      QString::fromUtf8(Settings::GetLineDetectModeDisplayName(static_cast<GPULineDetectMode>(i))));
  }

  // Capture Tab

  for (u32 i = 0; i < static_cast<u32>(DisplayScreenshotMode::Count); i++)
  {
    m_ui.screenshotSize->addItem(
      QString::fromUtf8(Settings::GetDisplayScreenshotModeDisplayName(static_cast<DisplayScreenshotMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayScreenshotFormat::Count); i++)
  {
    m_ui.screenshotFormat->addItem(
      QString::fromUtf8(Settings::GetDisplayScreenshotFormatDisplayName(static_cast<DisplayScreenshotFormat>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(MediaCaptureBackend::MaxCount); i++)
  {
    m_ui.mediaCaptureBackend->addItem(
      QString::fromUtf8(MediaCapture::GetBackendDisplayName(static_cast<MediaCaptureBackend>(i))));
  }

  // Debugging Tab

  for (u32 i = 0; i < static_cast<u32>(GPUWireframeMode::Count); i++)
  {
    m_ui.gpuWireframeMode->addItem(
      QString::fromUtf8(Settings::GetGPUWireframeModeDisplayName(static_cast<GPUWireframeMode>(i))));
  }
}

void GraphicsSettingsWidget::removePlatformSpecificUi()
{
#ifndef _WIN32
  m_ui.advancedDisplayOptionsLayout->removeWidget(m_ui.blitSwapChain);
  delete m_ui.blitSwapChain;
  m_ui.blitSwapChain = nullptr;
#endif
}

GPURenderer GraphicsSettingsWidget::getEffectiveRenderer() const
{
  return Settings::ParseRendererName(
           m_dialog
             ->getEffectiveStringValue("GPU", "Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER))
             .c_str())
    .value_or(Settings::DEFAULT_GPU_RENDERER);
}

bool GraphicsSettingsWidget::effectiveRendererIsHardware() const
{
  return (getEffectiveRenderer() != GPURenderer::Software);
}

void GraphicsSettingsWidget::onShowDebugSettingsChanged(bool enabled)
{
  m_ui.tabs->setTabVisible(TAB_INDEX_DEBUGGING, enabled);
}

void GraphicsSettingsWidget::updateRendererDependentOptions()
{
  const GPURenderer renderer = getEffectiveRenderer();
  const RenderAPI render_api = Settings::GetRenderAPIForRenderer(renderer);
  const bool is_hardware = (renderer != GPURenderer::Software);

  m_ui.resolutionScale->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling));
  m_ui.resolutionScaleLabel->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling));
  m_ui.msaaMode->setEnabled(is_hardware);
  m_ui.msaaModeLabel->setEnabled(is_hardware);
  m_ui.textureFiltering->setEnabled(is_hardware &&
                                    !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTextureFiltering));
  m_ui.textureFilteringLabel->setEnabled(is_hardware &&
                                         !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTextureFiltering));
  m_ui.gpuDownsampleLabel->setEnabled(is_hardware);
  m_ui.gpuDownsampleMode->setEnabled(is_hardware);
  m_ui.gpuDownsampleScale->setEnabled(is_hardware);
  m_ui.trueColor->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTrueColor));
  m_ui.pgxpEnable->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXP));

  m_ui.gpuLineDetectMode->setEnabled(is_hardware);
  m_ui.gpuLineDetectModeLabel->setEnabled(is_hardware);
  m_ui.gpuWireframeMode->setEnabled(is_hardware);
  m_ui.gpuWireframeModeLabel->setEnabled(is_hardware);
  m_ui.debanding->setEnabled(is_hardware);
  m_ui.scaledDithering->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableScaledDithering));
  m_ui.useSoftwareRendererForReadbacks->setEnabled(is_hardware);
  m_ui.forceRoundedTexcoords->setEnabled(is_hardware);
  m_ui.accurateBlending->setEnabled(is_hardware);

  m_ui.tabs->setTabEnabled(TAB_INDEX_TEXTURE_REPLACEMENTS, is_hardware);

#ifdef _WIN32
  m_ui.blitSwapChain->setEnabled(render_api == RenderAPI::D3D11);
#endif

  m_ui.gpuThread->setEnabled(!is_hardware);
  m_ui.threadedPresentation->setEnabled(render_api == RenderAPI::Vulkan);

  m_ui.exclusiveFullscreenLabel->setEnabled(render_api == RenderAPI::D3D11 || render_api == RenderAPI::D3D12 ||
                                            render_api == RenderAPI::Vulkan);
  m_ui.exclusiveFullscreenControl->setEnabled(render_api == RenderAPI::Vulkan);

  populateGPUAdaptersAndResolutions(render_api);
  updatePGXPSettingsEnabled();
}

void GraphicsSettingsWidget::populateGPUAdaptersAndResolutions(RenderAPI render_api)
{
  // Don't re-query, it's expensive.
  if (m_adapters_render_api != render_api)
  {
    m_adapters_render_api = render_api;
    m_adapters = GPUDevice::GetAdapterListForAPI(render_api);
  }

  const GPUDevice::AdapterInfo* current_adapter = nullptr;
  SettingsInterface* const sif = m_dialog->getSettingsInterface();

  {
    m_ui.adapter->disconnect();
    m_ui.adapter->clear();
    m_ui.adapter->addItem(tr("(Default)"), QVariant(QString()));

    const std::string current_adapter_name = m_dialog->getEffectiveStringValue("GPU", "Adapter", "");
    for (const GPUDevice::AdapterInfo& adapter : m_adapters)
    {
      const QString qadaptername = QString::fromStdString(adapter.name);
      m_ui.adapter->addItem(qadaptername, QVariant(qadaptername));
      if (adapter.name == current_adapter_name)
        current_adapter = &adapter;
    }

    // default adapter
    if (!m_adapters.empty() && current_adapter_name.empty())
      current_adapter = &m_adapters.front();

    // disable it if we don't have a choice
    m_ui.adapter->setEnabled(!m_adapters.empty());
    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.adapter, "GPU", "Adapter");
    connect(m_ui.adapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &GraphicsSettingsWidget::updateRendererDependentOptions);
  }

  {
    m_ui.fullscreenMode->disconnect();
    m_ui.fullscreenMode->clear();

    m_ui.fullscreenMode->addItem(tr("Borderless Fullscreen"), QVariant(QString()));
    if (current_adapter)
    {
      for (const std::string& mode_name : current_adapter->fullscreen_modes)
      {
        const QString qmodename = QString::fromStdString(mode_name);
        m_ui.fullscreenMode->addItem(qmodename, QVariant(qmodename));
      }
    }

    // disable it if we don't have a choice
    m_ui.fullscreenMode->setEnabled(current_adapter && !current_adapter->fullscreen_modes.empty());
    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.fullscreenMode, "GPU", "FullscreenMode");
  }

  if (!m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling))
  {
    m_ui.resolutionScale->disconnect();
    m_ui.resolutionScale->clear();

    static constexpr const std::pair<int, const char*> templates[] = {
      {0, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "Automatic (Based on Window Size)")},
      {1, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "1x Native (Default)")},
      {3, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "3x Native (for 720p)")},
      {5, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "5x Native (for 1080p)")},
      {6, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "6x Native (for 1440p)")},
      {9, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "9x Native (for 4K)")},
    };

    const int max_scale =
      static_cast<int>(current_adapter ? std::max<u32>(current_adapter->max_texture_size / 1024, 1) : 16);
    for (int scale = 0; scale <= max_scale; scale++)
    {
      const auto it = std::find_if(std::begin(templates), std::end(templates),
                                   [&scale](const std::pair<int, const char*>& it) { return scale == it.first; });
      m_ui.resolutionScale->addItem((it != std::end(templates)) ?
                                      qApp->translate("GraphicsSettingsWidget", it->second) :
                                      qApp->translate("GraphicsSettingsWidget", "%1x Native").arg(scale));
    }

    SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.resolutionScale, "GPU", "ResolutionScale", 1);
    connect(m_ui.resolutionScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &GraphicsSettingsWidget::updateResolutionDependentOptions);
  }

  {
    m_ui.msaaMode->disconnect();
    m_ui.msaaMode->clear();

    if (m_dialog->isPerGameSettings())
      m_ui.msaaMode->addItem(tr("Use Global Setting"));

    const u32 max_multisamples = current_adapter ? current_adapter->max_multisamples : 8;
    m_ui.msaaMode->addItem(tr("Disabled"), GetMSAAModeValue(1, false));
    for (uint i = 2; i <= max_multisamples; i *= 2)
      m_ui.msaaMode->addItem(tr("%1x MSAA").arg(i), GetMSAAModeValue(i, false));
    for (uint i = 2; i <= max_multisamples; i *= 2)
      m_ui.msaaMode->addItem(tr("%1x SSAA").arg(i), GetMSAAModeValue(i, true));

    if (!m_dialog->isPerGameSettings() || (m_dialog->containsSettingValue("GPU", "Multisamples") ||
                                           m_dialog->containsSettingValue("GPU", "PerSampleShading")))
    {
      const QVariant current_msaa_mode(
        GetMSAAModeValue(static_cast<uint>(m_dialog->getEffectiveIntValue("GPU", "Multisamples", 1)),
                         m_dialog->getEffectiveBoolValue("GPU", "PerSampleShading", false)));
      const int current_msaa_index = m_ui.msaaMode->findData(current_msaa_mode);
      if (current_msaa_index >= 0)
        m_ui.msaaMode->setCurrentIndex(current_msaa_index);
    }
    else
    {
      m_ui.msaaMode->setCurrentIndex(0);
    }
    connect(m_ui.msaaMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
      const int index = m_ui.msaaMode->currentIndex();
      if (m_dialog->isPerGameSettings() && index == 0)
      {
        m_dialog->removeSettingValue("GPU", "Multisamples");
        m_dialog->removeSettingValue("GPU", "PerSampleShading");
      }
      else
      {
        uint multisamples;
        bool ssaa;
        DecodeMSAAModeValue(m_ui.msaaMode->itemData(index), &multisamples, &ssaa);
        m_dialog->setIntSettingValue("GPU", "Multisamples", static_cast<int>(multisamples));
        m_dialog->setBoolSettingValue("GPU", "PerSampleShading", ssaa);
      }
    });
  }
}

void GraphicsSettingsWidget::updatePGXPSettingsEnabled()
{
  const bool enabled = (effectiveRendererIsHardware() && m_dialog->getEffectiveBoolValue("GPU", "PGXPEnable", false) &&
                        !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXP));
  const bool tc_enabled = (enabled && m_dialog->getEffectiveBoolValue("GPU", "PGXPTextureCorrection", true));
  const bool depth_enabled = (enabled && m_dialog->getEffectiveBoolValue("GPU", "PGXPDepthBuffer", false));
  m_ui.tabs->setTabEnabled(TAB_INDEX_PGXP, enabled);
  m_ui.pgxpTab->setEnabled(enabled);
  m_ui.pgxpCulling->setEnabled(enabled && !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPCulling));
  m_ui.pgxpTextureCorrection->setEnabled(enabled &&
                                         !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPTextureCorrection));
  m_ui.pgxpColorCorrection->setEnabled(tc_enabled &&
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPColorCorrection));
  m_ui.pgxpDepthBuffer->setEnabled(enabled && !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPDepthBuffer));
  m_ui.pgxpPreserveProjPrecision->setEnabled(enabled &&
                                             !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPPreserveProjFP));
  m_ui.pgxpCPU->setEnabled(enabled);
  m_ui.pgxpVertexCache->setEnabled(enabled);
  m_ui.pgxpGeometryTolerance->setEnabled(enabled);
  m_ui.pgxpGeometryToleranceLabel->setEnabled(enabled);
  m_ui.pgxpDepthClearThreshold->setEnabled(depth_enabled);
  m_ui.pgxpDepthClearThresholdLabel->setEnabled(depth_enabled);
}

void GraphicsSettingsWidget::onAspectRatioChanged()
{
  const DisplayAspectRatio ratio =
    Settings::ParseDisplayAspectRatio(
      m_dialog
        ->getEffectiveStringValue("Display", "AspectRatio",
                                  Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO))
        .c_str())
      .value_or(Settings::DEFAULT_DISPLAY_ASPECT_RATIO);

  const bool is_custom = (ratio == DisplayAspectRatio::Custom);

  m_ui.customAspectRatioNumerator->setVisible(is_custom);
  m_ui.customAspectRatioDenominator->setVisible(is_custom);
  m_ui.customAspectRatioSeparator->setVisible(is_custom);
}

void GraphicsSettingsWidget::updateResolutionDependentOptions()
{
  const int scale = m_dialog->getEffectiveIntValue("GPU", "ResolutionScale", 1);
  const GPUTextureFilter texture_filtering =
    Settings::ParseTextureFilterName(
      m_dialog
        ->getEffectiveStringValue("GPU", "TextureFilter",
                                  Settings::GetTextureFilterName(Settings::DEFAULT_GPU_TEXTURE_FILTER))
        .c_str())
      .value_or(Settings::DEFAULT_GPU_TEXTURE_FILTER);
  m_ui.forceRoundedTexcoords->setEnabled(scale > 1 && texture_filtering == GPUTextureFilter::Nearest);
  onTrueColorChanged();
}

void GraphicsSettingsWidget::onTrueColorChanged()
{
  const int resolution_scale = m_dialog->getEffectiveIntValue("GPU", "ResolutionScale", 1);
  const bool true_color = m_dialog->getEffectiveBoolValue("GPU", "TrueColor", false);
  const bool allow_scaled_dithering =
    (resolution_scale != 1 && !true_color && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableScaledDithering));
  m_ui.scaledDithering->setEnabled(allow_scaled_dithering);
  m_ui.debanding->setEnabled(true_color);
  m_ui.accurateBlending->setEnabled(!true_color);
}

void GraphicsSettingsWidget::onDownsampleModeChanged()
{
  const GPUDownsampleMode mode =
    Settings::ParseDownsampleModeName(
      m_dialog
        ->getEffectiveStringValue("GPU", "DownsampleMode",
                                  Settings::GetDownsampleModeName(Settings::DEFAULT_GPU_DOWNSAMPLE_MODE))
        .c_str())
      .value_or(Settings::DEFAULT_GPU_DOWNSAMPLE_MODE);

  const bool visible = (mode == GPUDownsampleMode::Box);
  if (visible && m_ui.gpuDownsampleLayout->indexOf(m_ui.gpuDownsampleScale) < 0)
  {
    m_ui.gpuDownsampleScale->setVisible(true);
    m_ui.gpuDownsampleLayout->addWidget(m_ui.gpuDownsampleScale, 0);
  }
  else if (!visible && m_ui.gpuDownsampleLayout->indexOf(m_ui.gpuDownsampleScale) >= 0)
  {
    m_ui.gpuDownsampleScale->setVisible(false);
    m_ui.gpuDownsampleLayout->removeWidget(m_ui.gpuDownsampleScale);
  }
}

void GraphicsSettingsWidget::onMediaCaptureBackendChanged()
{
  SettingsInterface* const sif = m_dialog->getSettingsInterface();
  const MediaCaptureBackend backend =
    MediaCapture::ParseBackendName(
      m_dialog
        ->getEffectiveStringValue("MediaCapture", "Backend",
                                  MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND))
        .c_str())
      .value_or(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);

  {
    m_ui.captureContainer->disconnect();
    m_ui.captureContainer->clear();

    for (const auto& [name, display_name] : MediaCapture::GetContainerList(backend))
    {
      const QString qname = QString::fromStdString(name);
      m_ui.captureContainer->addItem(tr("%1 (%2)").arg(QString::fromStdString(display_name)).arg(qname), qname);
    }

    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.captureContainer, "MediaCapture", "Container",
                                                   Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
    connect(m_ui.captureContainer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &GraphicsSettingsWidget::onMediaCaptureContainerChanged);
  }

  onMediaCaptureContainerChanged();
}

void GraphicsSettingsWidget::onMediaCaptureContainerChanged()
{
  SettingsInterface* const sif = m_dialog->getSettingsInterface();
  const MediaCaptureBackend backend =
    MediaCapture::ParseBackendName(
      m_dialog
        ->getEffectiveStringValue("MediaCapture", "Backend",
                                  MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND))
        .c_str())
      .value_or(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);
  const std::string container = m_dialog->getEffectiveStringValue("MediaCapture", "Container", "mp4");

  {
    m_ui.videoCaptureCodec->disconnect();
    m_ui.videoCaptureCodec->clear();
    m_ui.videoCaptureCodec->addItem(tr("Default"), QVariant(QString()));

    for (const auto& [name, display_name] : MediaCapture::GetVideoCodecList(backend, container.c_str()))
    {
      const QString qname = QString::fromStdString(name);
      m_ui.videoCaptureCodec->addItem(tr("%1 (%2)").arg(QString::fromStdString(display_name)).arg(qname), qname);
    }

    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.videoCaptureCodec, "MediaCapture", "VideoCodec");
  }

  {
    m_ui.audioCaptureCodec->disconnect();
    m_ui.audioCaptureCodec->clear();
    m_ui.audioCaptureCodec->addItem(tr("Default"), QVariant(QString()));

    for (const auto& [name, display_name] : MediaCapture::GetAudioCodecList(backend, container.c_str()))
    {
      const QString qname = QString::fromStdString(name);
      m_ui.audioCaptureCodec->addItem(tr("%1 (%2)").arg(QString::fromStdString(display_name)).arg(qname), qname);
    }

    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.audioCaptureCodec, "MediaCapture", "AudioCodec");
  }
}

void GraphicsSettingsWidget::onMediaCaptureVideoEnabledChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoCapture", true);
  m_ui.videoCaptureCodecLabel->setEnabled(enabled);
  m_ui.videoCaptureCodec->setEnabled(enabled);
  m_ui.videoCaptureBitrateLabel->setEnabled(enabled);
  m_ui.videoCaptureBitrate->setEnabled(enabled);
  m_ui.videoCaptureResolutionLabel->setEnabled(enabled);
  m_ui.videoCaptureResolutionAuto->setEnabled(enabled);
  m_ui.enableVideoCaptureArguments->setEnabled(enabled);
  m_ui.videoCaptureArguments->setEnabled(enabled);
  onMediaCaptureVideoAutoResolutionChanged();
}

void GraphicsSettingsWidget::onMediaCaptureVideoAutoResolutionChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoCapture", true);
  const bool auto_enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoAutoSize", false);
  m_ui.videoCaptureWidth->setEnabled(enabled && !auto_enabled);
  m_ui.xLabel->setEnabled(enabled && !auto_enabled);
  m_ui.videoCaptureHeight->setEnabled(enabled && !auto_enabled);
}

void GraphicsSettingsWidget::onMediaCaptureAudioEnabledChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "AudioCapture", true);
  m_ui.audioCaptureCodecLabel->setEnabled(enabled);
  m_ui.audioCaptureCodec->setEnabled(enabled);
  m_ui.audioCaptureBitrateLabel->setEnabled(enabled);
  m_ui.audioCaptureBitrate->setEnabled(enabled);
  m_ui.enableAudioCaptureArguments->setEnabled(enabled);
  m_ui.audioCaptureArguments->setEnabled(enabled);
}

void GraphicsSettingsWidget::onEnableAnyTextureReplacementsChanged()
{
  const bool any_replacements_enabled =
    m_dialog->getEffectiveBoolValue("TextureReplacements", "EnableVRAMWriteReplacements", false);
  m_ui.preloadTextureReplacements->setEnabled(any_replacements_enabled);
}

void GraphicsSettingsWidget::onEnableVRAMWriteDumpingChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("TextureReplacements", "DumpVRAMWrites", false);
  m_ui.setVRAMWriteAlphaChannel->setEnabled(enabled);
  m_ui.minDumpedVRAMWriteWidth->setEnabled(enabled);
  m_ui.minDumpedVRAMWriteHeight->setEnabled(enabled);
  m_ui.vramWriteDumpThresholdLabel->setEnabled(enabled);
  m_ui.vramWriteDumpThresholdSeparator->setEnabled(enabled);
}
