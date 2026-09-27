// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "systemlinksettingswidget.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "common/assert.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace {

QString Tr(const char* text)
{
  return QCoreApplication::translate("SystemLinkSettingsWidget", text);
}

} // namespace

SystemLinkSettingsWidget::SystemLinkSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  DebugAssert(!dialog->isPerGameSettings());

  QVBoxLayout* layout = new QVBoxLayout(this);

  QGroupBox* network_group = new QGroupBox(Tr("Network"), this);
  QFormLayout* network_layout = new QFormLayout(network_group);
  network_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  m_enabled = new QCheckBox(Tr("Enable System Link"), network_group);
  network_layout->addRow(m_enabled);

  m_cabinet_id = new QSpinBox(network_group);
  m_cabinet_id->setRange(1, 4);
  network_layout->addRow(Tr("Cabinet ID:"), m_cabinet_id);

  m_server_address = new QLineEdit(network_group);
  m_server_address->setClearButtonEnabled(true);
  network_layout->addRow(Tr("Host IPv4 Address:"), m_server_address);

  m_port = new QSpinBox(network_group);
  m_port->setRange(1, 65535);
  network_layout->addRow(Tr("Port:"), m_port);

  layout->addWidget(network_group);

  QLabel* automatic_note =
    new QLabel(Tr("System Link provides the network transport used by supported linked arcade hardware. Use "
                  "127.0.0.1 when multiple ArcadeDuck instances run on the same PC. For separate PCs on a LAN, "
                  "enter the IPv4 address of the host machine. All cabinets in the same link group must use the "
                  "same port. Configure cabinet count, cabinet ID, or other link options in the arcade game's "
                  "service menu when required."),
               this);
  automatic_note->setWordWrap(true);
  layout->addWidget(automatic_note);
  layout->addStretch();

  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_enabled, "SystemLink", "Enabled", false);
  SettingWidgetBinder::BindWidgetToIntSetting(nullptr, m_cabinet_id, "SystemLink", "CabinetID", 1);
  SettingWidgetBinder::BindWidgetToStringSetting(nullptr, m_server_address, "SystemLink", "ServerAddress",
                                                 "127.0.0.1");
  SettingWidgetBinder::BindWidgetToIntSetting(nullptr, m_port, "SystemLink", "Port", 19702);

  dialog->registerWidgetHelp(
    m_enabled, Tr("Enable System Link"), Tr("Disabled"),
    Tr("Enables the network transport used by supported linked arcade hardware. Leave this disabled for normal "
       "single-cabinet operation."));
  dialog->registerWidgetHelp(
    m_cabinet_id, Tr("Cabinet ID"), Tr("1"),
    Tr("Physical cabinet/node ID for linked arcade hardware which uses an external ID switch. Taito G-Net "
       "Communication PCB uses IDs 1 through 4. Use a unique ID for every linked instance."));
  dialog->registerWidgetHelp(
    m_server_address, Tr("Host IPv4 Address"), Tr("127.0.0.1"),
    Tr("IPv4 address of the host machine for the system-link session. Use 127.0.0.1 when multiple ArcadeDuck "
       "instances are running on the same PC. For separate PCs on a LAN, enter the host machine's LAN IPv4 "
       "address."));
  dialog->registerWidgetHelp(
    m_port, Tr("System Link Port"), Tr("19702"),
    Tr("TCP port shared by all ArcadeDuck instances participating in the same system-link group. All cabinets must "
       "use the same port."));

  connect(m_enabled, &QCheckBox::toggled, this, [this](bool) { updateEnabledState(); });
  updateEnabledState();
}

void SystemLinkSettingsWidget::updateEnabledState()
{
  const bool enabled = m_enabled->isChecked();
  m_cabinet_id->setEnabled(enabled);
  m_server_address->setEnabled(enabled);
  m_port->setEnabled(enabled);
}
