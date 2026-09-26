// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "controllersettingswindow.h"
#include "arcadecontrollerbindingwidget.h"
#include "controllerglobalsettingswidget.h"
#include "hotkeysettingswidget.h"
#include "qthost.h"

#include "core/arcade/arcade_control_registry.h"
#include "core/game_list.h"
#include "core/host.h"

#include "util/ini_settings_interface.h"
#include "util/input_manager.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/memory_settings_interface.h"

#include <QtCore/QCollator>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTextEdit>
#include <algorithm>
#include <array>

namespace {

void ConfigureComboBoxPopup(QComboBox* combo, int max_visible_items = 18)
{
  if (!combo)
    return;

  combo->setMaxVisibleItems(max_visible_items);
  combo->view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

} // namespace

ControllerSettingsWindow::ControllerSettingsWindow(SettingsInterface* game_sif /* = nullptr */,
                                                   QWidget* parent /* = nullptr */)
  : QWidget(parent), m_editing_settings_interface(game_sif)
{
  m_ui.setupUi(this);
  ConfigureComboBoxPopup(m_ui.currentProfile);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_ui.settingsCategory->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this,
          &ControllerSettingsWindow::onCategoryCurrentRowChanged);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &ControllerSettingsWindow::close);

  if (!game_sif)
  {
    refreshProfileList();

    m_ui.editProfileLayout->removeWidget(m_ui.copyGlobalSettings);
    delete m_ui.copyGlobalSettings;
    m_ui.copyGlobalSettings = nullptr;

    connect(m_ui.currentProfile, &QComboBox::currentIndexChanged, this,
            &ControllerSettingsWindow::onCurrentProfileChanged);
    connect(m_ui.newProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onNewProfileClicked);
    connect(m_ui.applyProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onApplyProfileClicked);
    connect(m_ui.deleteProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onDeleteProfileClicked);
    connect(m_ui.saveGameProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onSaveGameProfileClicked);
    connect(m_ui.restoreDefaults, &QPushButton::clicked, this, &ControllerSettingsWindow::onRestoreDefaultsClicked);

    connect(g_emu_thread, &EmuThread::onInputDevicesEnumerated, this,
            &ControllerSettingsWindow::onInputDevicesEnumerated);
    connect(g_emu_thread, &EmuThread::onInputDeviceConnected, this, &ControllerSettingsWindow::onInputDeviceConnected);
    connect(g_emu_thread, &EmuThread::onInputDeviceDisconnected, this,
            &ControllerSettingsWindow::onInputDeviceDisconnected);
    connect(g_emu_thread, &EmuThread::onVibrationMotorsEnumerated, this,
            &ControllerSettingsWindow::onVibrationMotorsEnumerated);

    // trigger a device enumeration to populate the device list
    g_emu_thread->enumerateInputDevices();
    g_emu_thread->enumerateVibrationMotors();
  }
  else
  {
    m_ui.editProfileLayout->removeWidget(m_ui.editProfileLabel);
    delete m_ui.editProfileLabel;
    m_ui.editProfileLabel = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.currentProfile);
    delete m_ui.currentProfile;
    m_ui.currentProfile = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.newProfile);
    delete m_ui.newProfile;
    m_ui.newProfile = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.applyProfile);
    delete m_ui.applyProfile;
    m_ui.applyProfile = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.deleteProfile);
    delete m_ui.deleteProfile;
    m_ui.deleteProfile = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.saveGameProfile);
    delete m_ui.saveGameProfile;
    m_ui.saveGameProfile = nullptr;

    connect(m_ui.copyGlobalSettings, &QPushButton::clicked, this,
            &ControllerSettingsWindow::onCopyGlobalSettingsClicked);
    connect(m_ui.restoreDefaults, &QPushButton::clicked, this,
            &ControllerSettingsWindow::onRestoreDefaultsForGameClicked);

    connect(g_emu_thread, &EmuThread::onInputDeviceConnected, this,
            [this](const std::string&, const std::string&) { refreshPointerDeviceList(); });
    connect(g_emu_thread, &EmuThread::onInputDeviceDisconnected, this,
            [this](const std::string&) { refreshPointerDeviceList(); });
  }

  refreshPointerDeviceList();
  createWidgets();
}

ControllerSettingsWindow::~ControllerSettingsWindow() = default;

void ControllerSettingsWindow::editControllerSettingsForGame(QWidget* parent, SettingsInterface* sif)
{
  ControllerSettingsWindow* dlg = new ControllerSettingsWindow(sif, parent);
  dlg->setWindowFlag(Qt::Window);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowModality(Qt::WindowModality::WindowModal);
  dlg->setWindowTitle(parent->windowTitle());
  dlg->setWindowIcon(parent->windowIcon());
  dlg->show();
}

int ControllerSettingsWindow::getHotkeyCategoryIndex() const
{
  return 1 + static_cast<int>(Arcade::NUM_ARCADE_CONTROLLER_PORTS);
}

ControllerSettingsWindow::Category ControllerSettingsWindow::getCurrentCategory() const
{
  const int index = m_ui.settingsCategory->currentRow();
  if (index == 0)
    return Category::GlobalSettings;
  else if (index >= getHotkeyCategoryIndex())
    return Category::HotkeySettings;
  else
    return Category::FirstControllerSettings;
}

void ControllerSettingsWindow::setCategory(Category category)
{
  switch (category)
  {
    case Category::GlobalSettings:
      m_ui.settingsCategory->setCurrentRow(0);
      break;

    case Category::FirstControllerSettings:
      m_ui.settingsCategory->setCurrentRow(1);
      break;

    case Category::HotkeySettings:
      m_ui.settingsCategory->setCurrentRow(getHotkeyCategoryIndex());
      break;

    default:
      break;
  }
}

void ControllerSettingsWindow::onCategoryCurrentRowChanged(int row)
{
  m_ui.settingsContainer->setCurrentIndex(row);
}

void ControllerSettingsWindow::onCurrentProfileChanged(int index)
{
  std::string profile_name;
  if (index > 0)
    profile_name = m_ui.currentProfile->itemText(index).toStdString();

  switchProfile(profile_name);
}

void ControllerSettingsWindow::onNewProfileClicked()
{
  const std::string profile_name =
    QInputDialog::getText(this, tr("Create Input Profile"), tr("Enter the name for the new input profile:"))
      .toStdString();
  if (profile_name.empty())
    return;

  std::string profile_path = System::GetInputProfilePath(profile_name);
  if (FileSystem::FileExists(profile_path.c_str()))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("A profile with the name '%1' already exists.").arg(QString::fromStdString(profile_name)));
    return;
  }

  const int res = QMessageBox::question(this, tr("Create Input Profile"),
                                        tr("Do you want to copy all bindings from the currently-selected profile to "
                                           "the new profile? Selecting No will create a completely empty profile."),
                                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (res == QMessageBox::Cancel)
    return;

  INISettingsInterface temp_si(std::move(profile_path));
  if (res == QMessageBox::Yes)
  {
    // copy from global or the current profile
    if (!m_editing_settings_interface)
    {
      const int hkres = QMessageBox::question(
        this, tr("Create Input Profile"),
        tr("Do you want to copy the current hotkey bindings from global settings to the new input profile?"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
      if (hkres == QMessageBox::Cancel)
        return;

      const bool copy_hotkey_bindings = (hkres == QMessageBox::Yes);
      if (copy_hotkey_bindings)
        temp_si.SetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", true);

      // from global
      auto lock = Host::GetSettingsLock();
      InputManager::CopyConfiguration(&temp_si, *Host::Internal::GetBaseSettingsLayer(), true, true,
                                      copy_hotkey_bindings);
      copyArcadeControllerConfiguration(&temp_si, *Host::Internal::GetBaseSettingsLayer());
    }
    else
    {
      // from profile
      const bool copy_hotkey_bindings =
        m_editing_settings_interface->GetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", false);
      temp_si.SetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", copy_hotkey_bindings);
      InputManager::CopyConfiguration(&temp_si, *m_editing_settings_interface, true, true, copy_hotkey_bindings);
      copyArcadeControllerConfiguration(&temp_si, *m_editing_settings_interface);
    }
  }

  if (!temp_si.Save())
  {
    QMessageBox::critical(
      this, tr("Error"),
      tr("Failed to save the new profile to '%1'.").arg(QString::fromStdString(temp_si.GetFileName())));
    return;
  }

  refreshProfileList();
  switchProfile(profile_name);
}

void ControllerSettingsWindow::onApplyProfileClicked()
{
  if (QMessageBox::question(this, tr("Load Input Profile"),
                            tr("Are you sure you want to load the input profile named '%1'?\n\n"
                               "All current global bindings will be removed, and the profile bindings loaded.\n\n"
                               "You cannot undo this action.")
                              .arg(m_profile_name)) != QMessageBox::Yes)
  {
    return;
  }

  {
    const bool copy_hotkey_bindings =
      m_editing_settings_interface->GetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", false);
    auto lock = Host::GetSettingsLock();
    InputManager::CopyConfiguration(Host::Internal::GetBaseSettingsLayer(), *m_editing_settings_interface, true, true,
                                    copy_hotkey_bindings);
    copyArcadeControllerConfiguration(Host::Internal::GetBaseSettingsLayer(), *m_editing_settings_interface);
    QtHost::QueueSettingsSave();
  }
  g_emu_thread->applySettings();

  // make it visible
  switchProfile({});
}

void ControllerSettingsWindow::onDeleteProfileClicked()
{
  if (QMessageBox::question(this, tr("Delete Input Profile"),
                            tr("Are you sure you want to delete the input profile named '%1'?\n\n"
                               "You cannot undo this action.")
                              .arg(m_profile_name)) != QMessageBox::Yes)
  {
    return;
  }

  std::string profile_path(System::GetInputProfilePath(m_profile_name.toStdString()));
  if (!FileSystem::DeleteFile(profile_path.c_str()))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to delete '%1'.").arg(QString::fromStdString(profile_path)));
    return;
  }

  // switch back to global
  refreshProfileList();
  switchProfile({});
}

void ControllerSettingsWindow::onSaveGameProfileClicked()
{
  struct GameProfileChoice
  {
    QString label;
    QString game_id;
  };

  std::vector<GameProfileChoice> choices;
  {
    const auto lock = GameList::GetLock();
    const u32 count = GameList::GetEntryCount();
    choices.reserve(count);

    for (u32 i = 0; i < count; i++)
    {
      const GameList::Entry* const entry = GameList::GetEntryByIndex(i);
      if (!entry || entry->serial.empty())
        continue;

      const Arcade::ArcadeGameControlProfile* const profile = Arcade::GetArcadeGameControlProfile(entry->serial);
      if (!profile)
        continue;

      const bool has_controls =
        std::any_of(profile->ports.begin(), profile->ports.end(), [](const Arcade::ArcadePortProfile& port) {
          return port.controller_type != Arcade::ArcadeControllerType::None;
        });
      if (!has_controls)
        continue;

      const QString game_id = QString::fromStdString(entry->serial);
      if (std::any_of(choices.begin(), choices.end(), [&game_id](const GameProfileChoice& choice) {
            return choice.game_id == game_id;
          }))
      {
        continue;
      }

      const QString title = QString::fromStdString(entry->title);
      choices.push_back({tr("%1 [%2]").arg(title, game_id), game_id});
    }
  }

  QCollator collator;
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setNumericMode(true);
  std::sort(choices.begin(), choices.end(), [&collator](const GameProfileChoice& lhs, const GameProfileChoice& rhs) {
    const int label_result = collator.compare(lhs.label, rhs.label);
    return label_result != 0 ? (label_result < 0) : (lhs.game_id < rhs.game_id);
  });
  if (choices.empty())
  {
    QMessageBox::information(this, tr("Save Game Profile"),
                             tr("No games in your game list have supported controller layouts."));
    return;
  }

  QStringList labels;
  labels.reserve(static_cast<qsizetype>(choices.size()));
  for (const GameProfileChoice& choice : choices)
    labels.append(choice.label);

  QInputDialog selector(this);
  selector.setWindowTitle(tr("Save Game Profile"));
  selector.setLabelText(tr("Select the game this controller mapping applies to:"));
  selector.setComboBoxEditable(false);
  selector.setComboBoxItems(labels);
  if (QComboBox* const combo = selector.findChild<QComboBox*>())
    ConfigureComboBoxPopup(combo);

  if (selector.exec() != QDialog::Accepted)
    return;

  const int selected_index = labels.indexOf(selector.textValue());
  if (selected_index < 0)
    return;

  const QString selected_set = choices[static_cast<size_t>(selected_index)].game_id;
  const QString selected_label = choices[static_cast<size_t>(selected_index)].label;

  MemorySettingsInterface replacement_settings;
  if (m_editing_settings_interface)
  {
    InputManager::CopyConfiguration(&replacement_settings, *m_editing_settings_interface, true, true, false);
    copyArcadeControllerConfiguration(&replacement_settings, *m_editing_settings_interface);
  }
  else
  {
    const auto lock = Host::GetSettingsLock();
    InputManager::CopyConfiguration(&replacement_settings, *Host::Internal::GetBaseSettingsLayer(), true, true, false);
    copyArcadeControllerConfiguration(&replacement_settings, *Host::Internal::GetBaseSettingsLayer());
  }
  replacement_settings.SetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", false);

  const std::string profile_path = System::GetInputProfilePath(selected_set.toStdString());
  const bool profile_exists = FileSystem::FileExists(profile_path.c_str());
  if (profile_exists &&
      QMessageBox::question(this, tr("Save Game Profile"),
                            tr("A game profile for '%1' already exists. Replace it?").arg(selected_label),
                            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
  {
    return;
  }

  if (profile_exists && !FileSystem::DeleteFile(profile_path.c_str()))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to replace '%1'.").arg(QString::fromStdString(profile_path)));
    return;
  }

  INISettingsInterface output_si(profile_path);
  InputManager::CopyConfiguration(&output_si, replacement_settings, true, true, false);
  copyArcadeControllerConfiguration(&output_si, replacement_settings);
  output_si.SetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", false);

  if (!output_si.Save())
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save the game profile to '%1'.").arg(QString::fromStdString(profile_path)));
    return;
  }

  const std::string game_settings_path = System::GetGameSettingsPath(selected_set.toStdString());
  if (game_settings_path.empty())
  {
    QMessageBox::warning(this, tr("Save Game Profile"),
                         tr("Game profile '%1' was saved, but ArcadeDuck could not assign it to the game.")
                           .arg(selected_label));
    return;
  }

  INISettingsInterface game_settings_si(game_settings_path);
  if (FileSystem::FileExists(game_settings_path.c_str()) && !game_settings_si.Load())
  {
    QMessageBox::warning(this, tr("Save Game Profile"),
                         tr("Game profile '%1' was saved, but ArcadeDuck could not assign it in '%2'.")
                           .arg(selected_label, QString::fromStdString(game_settings_path)));
    return;
  }

  game_settings_si.SetStringValue("ArcadeProfiles", "InputProfileName", selected_set.toUtf8().constData());
  if (!game_settings_si.Save())
  {
    QMessageBox::warning(this, tr("Save Game Profile"),
                         tr("Game profile '%1' was saved, but ArcadeDuck could not assign it in '%2'.")
                           .arg(selected_label, QString::fromStdString(game_settings_path)));
    return;
  }

  refreshProfileList();
  QMessageBox::information(this, tr("Save Game Profile"),
                           tr("Game profile '%1' was saved and assigned to the game.").arg(selected_label));
}

void ControllerSettingsWindow::onRestoreDefaultsClicked()
{
  if (QMessageBox::question(
        this, tr("Restore Defaults"),
        tr("Are you sure you want to restore the default controller configuration?\n\n"
           "All shared bindings and configuration will be lost, but your input profiles will remain.\n\n"
           "You cannot undo this action.")) != QMessageBox::Yes)
  {
    return;
  }

  // actually restore it
  g_emu_thread->setDefaultSettings(false, true);

  // reload all settings
  switchProfile({});
}

void ControllerSettingsWindow::onCopyGlobalSettingsClicked()
{
  DebugAssert(isEditingGameSettings());

  {
    const auto lock = Host::GetSettingsLock();
    InputManager::CopyConfiguration(m_editing_settings_interface, *Host::Internal::GetBaseSettingsLayer(), true, true,
                                    false);
    copyArcadeControllerConfiguration(m_editing_settings_interface, *Host::Internal::GetBaseSettingsLayer());
  }

  m_editing_settings_interface->Save();
  g_emu_thread->reloadGameSettings();
  createWidgets();

  QMessageBox::information(QtUtils::GetRootWidget(this), tr("ArcadeDuck Controller Settings"),
                           tr("Per-game controller configuration reset to global settings."));
}

void ControllerSettingsWindow::onRestoreDefaultsForGameClicked()
{
  DebugAssert(isEditingGameSettings());
  Settings::SetDefaultControllerConfig(*m_editing_settings_interface);
  m_editing_settings_interface->Save();
  g_emu_thread->reloadGameSettings();
  createWidgets();

  QMessageBox::information(QtUtils::GetRootWidget(this), tr("ArcadeDuck Controller Settings"),
                           tr("Per-game controller configuration reset to default settings."));
}

void ControllerSettingsWindow::onInputDevicesEnumerated(const std::vector<std::pair<std::string, std::string>>& devices)
{
  m_device_list = devices;
  for (const auto& [device_name, display_name] : m_device_list)
    m_global_settings->addDeviceToList(QString::fromStdString(device_name), QString::fromStdString(display_name));
  refreshPointerDeviceList();
}

void ControllerSettingsWindow::onInputDeviceConnected(const std::string& identifier, const std::string& device_name)
{
  m_device_list.emplace_back(identifier, device_name);
  m_global_settings->addDeviceToList(QString::fromStdString(identifier), QString::fromStdString(device_name));
  refreshPointerDeviceList();
  g_emu_thread->enumerateVibrationMotors();
}

void ControllerSettingsWindow::onInputDeviceDisconnected(const std::string& identifier)
{
  for (auto iter = m_device_list.begin(); iter != m_device_list.end(); ++iter)
  {
    if (iter->first == identifier)
    {
      m_device_list.erase(iter);
      break;
    }
  }

  m_global_settings->removeDeviceFromList(QString::fromStdString(identifier));
  refreshPointerDeviceList();
  g_emu_thread->enumerateVibrationMotors();
}

void ControllerSettingsWindow::refreshPointerDeviceList()
{
  m_pointer_device_list = InputManager::EnumeratePointerDevices();
  for (ArcadeControllerBindingWidget* widget : m_arcade_port_bindings)
  {
    if (widget)
      widget->queueRebuild();
  }
}

void ControllerSettingsWindow::onVibrationMotorsEnumerated(const QList<InputBindingKey>& motors)
{
  m_vibration_motors.clear();
  m_vibration_motors.reserve(motors.size());

  for (const InputBindingKey key : motors)
  {
    const std::string key_str(InputManager::ConvertInputBindingKeyToString(InputBindingInfo::Type::Motor, key));
    if (!key_str.empty())
      m_vibration_motors.push_back(QString::fromStdString(key_str));
  }
}

bool ControllerSettingsWindow::getBoolValue(const char* section, const char* key, bool default_value) const
{
  if (m_editing_settings_interface)
    return m_editing_settings_interface->GetBoolValue(section, key, default_value);
  else
    return Host::GetBaseBoolSettingValue(section, key, default_value);
}

s32 ControllerSettingsWindow::getIntValue(const char* section, const char* key, s32 default_value) const
{
  if (m_editing_settings_interface)
    return m_editing_settings_interface->GetIntValue(section, key, default_value);
  else
    return Host::GetBaseIntSettingValue(section, key, default_value);
}

std::string ControllerSettingsWindow::getStringValue(const char* section, const char* key,
                                                     const char* default_value) const
{
  std::string value;
  if (m_editing_settings_interface)
    value = m_editing_settings_interface->GetStringValue(section, key, default_value);
  else
    value = Host::GetBaseStringSettingValue(section, key, default_value);
  return value;
}

void ControllerSettingsWindow::setBoolValue(const char* section, const char* key, bool value)
{
  if (getBoolValue(section, key, !value) == value)
    return;

  if (m_editing_settings_interface)
    m_editing_settings_interface->SetBoolValue(section, key, value);
  else
    Host::SetBaseBoolSettingValue(section, key, value);

  markSettingsChanged();
}

void ControllerSettingsWindow::setIntValue(const char* section, const char* key, s32 value)
{
  if (getIntValue(section, key, value - 1) == value)
    return;

  if (m_editing_settings_interface)
    m_editing_settings_interface->SetIntValue(section, key, value);
  else
    Host::SetBaseIntSettingValue(section, key, value);

  markSettingsChanged();
}

void ControllerSettingsWindow::setStringValue(const char* section, const char* key, const char* value)
{
  if (getStringValue(section, key, "") == value)
    return;

  if (m_editing_settings_interface)
    m_editing_settings_interface->SetStringValue(section, key, value);
  else
    Host::SetBaseStringSettingValue(section, key, value);

  markSettingsChanged();
}

void ControllerSettingsWindow::beginSettingsBatch()
{
  m_settings_batch_depth++;
}

void ControllerSettingsWindow::endSettingsBatch()
{
  DebugAssert(m_settings_batch_depth > 0);
  if (--m_settings_batch_depth == 0)
    flushPendingSettingsChanges();
}

void ControllerSettingsWindow::markSettingsChanged()
{
  m_settings_change_pending = true;
  if (m_settings_batch_depth == 0)
    flushPendingSettingsChanges();
}

void ControllerSettingsWindow::flushPendingSettingsChanges()
{
  if (!m_settings_change_pending)
    return;

  m_settings_change_pending = false;
  if (m_editing_settings_interface)
  {
    saveAndReloadGameSettings();
  }
  else
  {
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
  }
}

void ControllerSettingsWindow::saveAndReloadGameSettings()
{
  DebugAssert(m_editing_settings_interface);
  QtHost::SaveGameSettings(m_editing_settings_interface, false);
  g_emu_thread->reloadGameSettings(false);
}

void ControllerSettingsWindow::clearSettingValue(const char* section, const char* key)
{
  if (m_editing_settings_interface)
    m_editing_settings_interface->DeleteValue(section, key);
  else
    Host::DeleteBaseSettingValue(section, key);

  markSettingsChanged();
}

void ControllerSettingsWindow::createWidgets()
{
  QSignalBlocker sb(m_ui.settingsContainer);
  QSignalBlocker sb2(m_ui.settingsCategory);

  while (m_ui.settingsContainer->count() > 0)
  {
    QWidget* widget = m_ui.settingsContainer->widget(m_ui.settingsContainer->count() - 1);
    m_ui.settingsContainer->removeWidget(widget);
    widget->deleteLater();
  }

  m_ui.settingsCategory->clear();

  m_global_settings = nullptr;
  m_hotkey_settings = nullptr;

  {
    // global settings
    QListWidgetItem* item = new QListWidgetItem();
    item->setText(tr("Global Settings"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("settings-3-line")));
    m_ui.settingsCategory->addItem(item);
    m_ui.settingsCategory->setCurrentRow(0);
    m_global_settings = new ControllerGlobalSettingsWidget(m_ui.settingsContainer, this);
    m_ui.settingsContainer->addWidget(m_global_settings);
    connect(m_global_settings, &ControllerGlobalSettingsWidget::bindingSetupChanged, this,
            &ControllerSettingsWindow::createWidgets);
    for (const auto& [identifier, device_name] : m_device_list)
      m_global_settings->addDeviceToList(QString::fromStdString(identifier), QString::fromStdString(device_name));
  }

  // ArcadeDuck always presents four independent arcade ports. They are host-binding profiles,
  // Arcade controls are independent machine inputs, so legacy port state does not affect this list.
  for (u32 port = 0; port < Arcade::NUM_ARCADE_CONTROLLER_PORTS; port++)
  {
    m_arcade_port_bindings[port] = new ArcadeControllerBindingWidget(m_ui.settingsContainer, this, port);
    m_ui.settingsContainer->addWidget(m_arcade_port_bindings[port]);

    QListWidgetItem* item = new QListWidgetItem();
    item->setText(tr("Controller Port %1\n%2").arg(port + 1).arg(m_arcade_port_bindings[port]->getDisplayName()));
    item->setIcon(QIcon::fromTheme(QStringLiteral("controller-line")));
    item->setData(Qt::UserRole, QVariant(port));
    m_ui.settingsCategory->addItem(item);
  }

  // only add hotkeys if we're editing global settings
  if (!m_editing_settings_interface ||
      m_editing_settings_interface->GetBoolValue("ArcadeProfiles", "UseProfileHotkeyBindings", false))
  {
    QListWidgetItem* item = new QListWidgetItem();
    item->setText(tr("Hotkeys"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("keyboard-line")));
    m_ui.settingsCategory->addItem(item);
    m_hotkey_settings = new HotkeySettingsWidget(m_ui.settingsContainer, this);
    m_ui.settingsContainer->addWidget(m_hotkey_settings);
  }

  if (!isEditingGameSettings())
  {
    m_ui.applyProfile->setEnabled(isEditingProfile());
    m_ui.deleteProfile->setEnabled(isEditingProfile());
    m_ui.restoreDefaults->setEnabled(isEditingGlobalSettings());
  }
}

void ControllerSettingsWindow::copyArcadeControllerConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si)
{
  static constexpr std::array<const char*, 38> binding_keys = {{
    "Up", "Down", "Left", "Right", "Button1", "Button2", "Button3", "Button4", "Button5", "Button6",
    "Coin", "Start", "TrackballX", "TrackballY", "GunX", "GunY", "Trigger", "Reload", "GSRIncrease",
    "GSRDecrease", "Steering", "Accelerator", "Brake", "Clutch", "GearUp", "GearDown", "Handbrake",
    "View", "Horn", "MusicNext", "MusicPrevious", "Select", "SelectUp", "SelectDown", "SelectLeft",
    "SelectRight", "Enter", "Sensor"}};
  static constexpr std::array<const char*, 17> value_keys = {{"Type", "Layout", "InputMode", "PhysicalDevice", "PhysicalDeviceName", "Screen", "XSensitivity",
    "YSensitivity", "InvertX", "InvertY", "OffscreenReload", "CrosshairEnabled", "CrosshairImagePath", "CrosshairSize",
    "CrosshairScale", "SindenBorder", "SindenBorderWidth"}};

  for (u32 port = 0; port < Arcade::NUM_ARCADE_CONTROLLER_PORTS; port++)
  {
    const std::string section = std::string("ArcadeControllerPort") + std::to_string(port + 1);
    for (const char* key : binding_keys)
      dest_si->DeleteValue(section.c_str(), key);
    for (const char* key : value_keys)
      dest_si->DeleteValue(section.c_str(), key);

    const std::string type_name = src_si.GetStringValue(section.c_str(), "Type", "none");
    Arcade::ArcadeControllerType type = Arcade::ArcadeControllerType::None;
    const Arcade::ArcadeControllerTypeInfo* type_info = nullptr;
    for (const Arcade::ArcadeControllerTypeInfo& info : Arcade::GetArcadeControllerTypeInfos())
    {
      if (info.name == type_name)
      {
        type = info.type;
        type_info = &info;
        break;
      }
    }

    if (type == Arcade::ArcadeControllerType::None || !type_info)
    {
      dest_si->SetStringValue(section.c_str(), "Type", "none");
      continue;
    }

    const auto copy_binding = [&src_si, dest_si, &section](const char* key) {
      dest_si->CopyStringListValue(src_si, section.c_str(), key);
    };
    const auto copy_value = [&src_si, dest_si, &section](const char* key) {
      dest_si->CopyStringValue(src_si, section.c_str(), key);
    };
    const auto copy_layout_bindings = [&]() {
      const std::string layout = src_si.GetStringValue(section.c_str(), "Layout", "");
      const Arcade::ArcadeControlLayoutInfo* layout_info = Arcade::GetArcadeControlLayoutInfo(layout);
      if (!layout_info || layout_info->controller_type != type)
        return false;

      const bool uses_native_pointer =
        src_si.GetStringValue(section.c_str(), "InputMode", "") == "NativeRawInputDevice";
      for (const Arcade::ArcadeControlLayoutBindingInfo& binding : layout_info->bindings)
      {
        const bool native_pointer_axis =
          uses_native_pointer &&
          ((type == Arcade::ArcadeControllerType::Trackball &&
            (binding.binding_key == "TrackballX" || binding.binding_key == "TrackballY")) ||
           (type == Arcade::ArcadeControllerType::Lightgun &&
            (binding.binding_key == "GunX" || binding.binding_key == "GunY")));
        if (!native_pointer_axis)
          copy_binding(binding.binding_key.data());
      }
      return true;
    };

    dest_si->SetStringValue(section.c_str(), "Type", type_info->name.data());
    copy_value("Layout");
    copy_binding("Coin");
    copy_binding("Start");

    switch (type)
    {
      case Arcade::ArcadeControllerType::Arcade:
      case Arcade::ArcadeControllerType::Driving:
        copy_layout_bindings();
        break;

      case Arcade::ArcadeControllerType::Trackball:
      {
        if (!copy_layout_bindings())
        {
          for (const char* key : {"Button1", "Button2"})
            copy_binding(key);
        }

        for (const char* key : {"InputMode", "PhysicalDevice", "PhysicalDeviceName", "Screen", "XSensitivity",
                                "YSensitivity", "InvertX", "InvertY"})
        {
          copy_value(key);
        }

        if (src_si.GetStringValue(section.c_str(), "InputMode", "") != "NativeRawInputDevice")
        {
          copy_binding("TrackballX");
          copy_binding("TrackballY");
        }
        break;
      }

      case Arcade::ArcadeControllerType::Lightgun:
      {
        if (!copy_layout_bindings())
        {
          copy_binding("Trigger");
          copy_binding("Reload");
        }

        for (const char* key : {"InputMode", "PhysicalDevice", "PhysicalDeviceName", "Screen", "XSensitivity",
                                "YSensitivity", "InvertX", "InvertY", "OffscreenReload", "CrosshairEnabled",
                                "CrosshairImagePath", "CrosshairScale"})
        {
          copy_value(key);
        }

        if (src_si.GetStringValue(section.c_str(), "InputMode", "") != "NativeRawInputDevice")
        {
          copy_binding("GunX");
          copy_binding("GunY");
        }

        if (port == 0)
        {
          copy_value("SindenBorder");
          copy_value("SindenBorderWidth");
        }
        break;
      }

      case Arcade::ArcadeControllerType::Tokimeki:
        if (!copy_layout_bindings())
        {
          for (const char* key : {"Up", "Down", "Left", "Right", "Button1", "GSRIncrease", "GSRDecrease"})
            copy_binding(key);
        }
        break;

      case Arcade::ArcadeControllerType::None:
      default:
        break;
    }
  }
}

void ControllerSettingsWindow::closeEvent(QCloseEvent* event)
{
  QWidget::closeEvent(event);
  emit windowClosed();
}

void ControllerSettingsWindow::updateArcadeListDescription(u32 port, const QString& display_name)
{
  for (int i = 0; i < m_ui.settingsCategory->count(); i++)
  {
    QListWidgetItem* item = m_ui.settingsCategory->item(i);
    bool is_ok = false;
    if (item->data(Qt::UserRole).toUInt(&is_ok) == port && is_ok)
    {
      item->setText(tr("Controller Port %1\n%2").arg(port + 1).arg(display_name));
      break;
    }
  }
}
void ControllerSettingsWindow::refreshProfileList()
{
  QStringList names;
  for (const std::string& name : InputManager::GetInputProfileNames())
    names.append(QString::fromStdString(name));

  QCollator collator;
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setNumericMode(true);
  std::sort(names.begin(), names.end(), [&collator](const QString& lhs, const QString& rhs) {
    return collator.compare(lhs, rhs) < 0;
  });

  QSignalBlocker sb(m_ui.currentProfile);
  m_ui.currentProfile->clear();
  m_ui.currentProfile->addItem(tr("Shared"));
  if (isEditingGlobalSettings())
    m_ui.currentProfile->setCurrentIndex(0);

  for (const QString& qname : names)
  {
    m_ui.currentProfile->addItem(qname);
    if (qname == m_profile_name)
      m_ui.currentProfile->setCurrentIndex(m_ui.currentProfile->count() - 1);
  }
}

void ControllerSettingsWindow::switchProfile(const std::string_view name)
{
  QSignalBlocker sb(m_ui.currentProfile);

  if (!name.empty())
  {
    const QString name_qstr = QtUtils::StringViewToQString(name);

    std::string path = System::GetInputProfilePath(name);
    if (!FileSystem::FileExists(path.c_str()))
    {
      QMessageBox::critical(this, tr("Error"), tr("The input profile named '%1' cannot be found.").arg(name_qstr));
      return;
    }

    std::unique_ptr<INISettingsInterface> sif = std::make_unique<INISettingsInterface>(std::move(path));
    if (!sif->Load())
    {
      QMessageBox::critical(this, tr("Error"), tr("Failed to load the input profile named '%1'.").arg(name_qstr));
      return;
    }
    Settings::RemoveStandardControllerConfig(*sif);
    sif->RemoveEmptySections();
    if (!sif->Save())
    {
      QMessageBox::critical(this, tr("Error"), tr("Failed to save the input profile named '%1'.").arg(name_qstr));
      return;
    }

    m_profile_settings_interface = std::move(sif);
    m_editing_settings_interface = m_profile_settings_interface.get();
    m_ui.currentProfile->setCurrentIndex(m_ui.currentProfile->findText(name_qstr));
    m_profile_name = name_qstr;
  }
  else
  {
    m_profile_settings_interface.reset();
    m_editing_settings_interface = nullptr;
    m_ui.currentProfile->setCurrentIndex(0);
    m_profile_name = QString();
  }

  createWidgets();
}
