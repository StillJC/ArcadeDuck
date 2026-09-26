// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "operatorsettingswidget.h"
#include "inputbindingwidgets.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/settings.h"

#include "common/assert.h"

OperatorSettingsWidget::OperatorSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);
  DebugAssert(!dialog->isPerGameSettings());

  m_ui.testBinding->initialize(nullptr, InputBindingInfo::Type::Button, "ArcadeOperator", "Test");
  m_ui.serviceBinding->initialize(nullptr, InputBindingInfo::Type::Button, "ArcadeOperator", "Service");

  dialog->registerWidgetHelp(
    m_ui.testBinding, tr("Test Switch"), tr("Not Bound"),
    tr("Momentary cabinet test input used to enter or operate a game's hardware test mode. Bind any supported "
       "keyboard key, controller button, or mouse button."));
  dialog->registerWidgetHelp(m_ui.serviceBinding, tr("Service Button"), tr("Not Bound"),
                             tr("Momentary cabinet service input used for service credits or operator-menu functions "
                                "where supported. Bind any supported input."));

  SettingWidgetBinder::BindWidgetToBoolSetting(
    nullptr, m_ui.externalOutputsEnabled, "ArcadeOutput", "Enabled", false);

  static const char* output_protocol_names[] = {
    QT_TRANSLATE_NOOP("OperatorSettingsWidget", "MAME Win32"),
    QT_TRANSLATE_NOOP("OperatorSettingsWidget", "MAME TCP (Port 8000)"),
    QT_TRANSLATE_NOOP("OperatorSettingsWidget", "Both"),
    nullptr,
  };
  static const char* output_protocol_values[] = {
    "Win32",
    "TCP",
    "Both",
    nullptr,
  };
  SettingWidgetBinder::BindWidgetToEnumSetting(
    nullptr, m_ui.externalOutputProtocol, "ArcadeOutput", "Protocol", output_protocol_names,
    output_protocol_values, "Win32", "OperatorSettingsWidget");

  m_ui.externalOutputProtocol->setEnabled(m_ui.externalOutputsEnabled->isChecked());
  connect(m_ui.externalOutputsEnabled, &QCheckBox::toggled, m_ui.externalOutputProtocol, &QWidget::setEnabled);

  dialog->registerWidgetHelp(
    m_ui.externalOutputsEnabled, tr("Enable External Cabinet Outputs"), tr("Disabled"),
    tr("Publishes supported cabinet lamps, recoil, and other physical feedback to external software. "
       "The internal Debug -> Show Arcade Outputs monitor remains available when external publishing is disabled. "
       "Changes take effect the next time a game starts."));
  dialog->registerWidgetHelp(
    m_ui.externalOutputProtocol, tr("External Output Protocol"), tr("MAME Win32"),
    tr("MAME Win32 uses the standard Windows MAME output message protocol. MAME TCP uses the standard network "
       "output protocol on TCP port 8000. Both enables both transports. Changes take effect the next time a game starts."));

  SettingWidgetBinder::BindWidgetToFolderSetting(
    nullptr, m_ui.searchDirectory, m_ui.browseSearchDirectory, tr("Select BIOS Directory"), m_ui.openSearchDirectory,
    nullptr, "BIOS", "SearchDirectory", Path::Combine(EmuFolders::DataRoot, "bios"));
}

OperatorSettingsWidget::~OperatorSettingsWidget() = default;
