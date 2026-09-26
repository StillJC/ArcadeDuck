// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "gamesummarywidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "settingswindow.h"

#include "core/arcade/arcade_control_registry.h"
#include "core/arcade/arcade_database.h"
#include "core/game_list.h"

#include <QtCore/QDir>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <utility>
#include <vector>

namespace {
QString GetControllerDescription(const Arcade::ArcadePortProfile& port)
{
  QString description;
  switch (port.controller_type)
  {
    case Arcade::ArcadeControllerType::Arcade:
      description = QStringLiteral("Arcade");
      break;
    case Arcade::ArcadeControllerType::Trackball:
      description = QStringLiteral("Trackball");
      break;
    case Arcade::ArcadeControllerType::Lightgun:
      description = QStringLiteral("Lightgun");
      break;
    case Arcade::ArcadeControllerType::Driving:
      description = QStringLiteral("Driving / Racing");
      break;
    case Arcade::ArcadeControllerType::Tokimeki:
      description = QStringLiteral("Tokimeki");
      break;
    case Arcade::ArcadeControllerType::None:
    default:
      return {};
  }

  if (port.controller_type == Arcade::ArcadeControllerType::Arcade)
  {
    if (port.joystick_mode == Arcade::ArcadeJoystickMode::FourWay)
      description += QStringLiteral(" / 4-Way");
    else if (port.joystick_mode == Arcade::ArcadeJoystickMode::EightWay)
      description += QStringLiteral(" / 8-Way");
  }

  return description;
}

QString GetControllerSummary(std::string_view set_name)
{
  const Arcade::ArcadeGameControlProfile* profile = Arcade::GetArcadeGameControlProfile(set_name);
  if (!profile)
    return {};

  std::vector<std::pair<QString, u32>> groups;
  for (const Arcade::ArcadePortProfile& port : profile->ports)
  {
    QString description = GetControllerDescription(port);
    if (description.isEmpty())
      continue;

    const auto it = std::find_if(groups.begin(), groups.end(), [&description](const auto& group) {
      return group.first == description;
    });
    if (it != groups.end())
      it->second++;
    else
      groups.emplace_back(std::move(description), 1u);
  }

  QStringList descriptions;
  descriptions.reserve(static_cast<int>(groups.size()));
  for (const auto& [description, count] : groups)
  {
    descriptions.append(count == 1 ? description : QStringLiteral("%1 x %2").arg(count).arg(description));
  }
  return descriptions.join(QStringLiteral(", "));
}
} // namespace

GameSummaryWidget::GameSummaryWidget(const std::string& path, const std::string& set_name, SettingsWindow* dialog,
                                     QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);
  populateUi(path, set_name);

  connect(m_ui.inputProfile, &QComboBox::currentIndexChanged, this, &GameSummaryWidget::onInputProfileChanged);
  connect(m_ui.editInputProfile, &QAbstractButton::clicked, this, &GameSummaryWidget::onEditInputProfileClicked);
  connect(m_ui.bezelEnabled, &QCheckBox::toggled, this, &GameSummaryWidget::onBezelEnabledChanged);
  connect(m_ui.bezelBrowse, &QAbstractButton::clicked, this, &GameSummaryWidget::onBezelBrowseClicked);
  connect(m_ui.bezelClear, &QAbstractButton::clicked, this, &GameSummaryWidget::onBezelClearClicked);
  connect(m_ui.bezelPath, &QLineEdit::editingFinished, this, &GameSummaryWidget::onBezelPathEditingFinished);
  connect(m_ui.title, &QLineEdit::editingFinished, this, [this]() {
    if (m_ui.title->isModified())
    {
      setCustomTitle(m_ui.title->text().toStdString());
      m_ui.title->setModified(false);
    }
  });
  connect(m_ui.restoreTitle, &QAbstractButton::clicked, this, [this]() { setCustomTitle({}); });
}

GameSummaryWidget::~GameSummaryWidget() = default;

void GameSummaryWidget::populateUi(const std::string& path, const std::string& set_name)
{
  m_path = path;
  if (const Arcade::Database::GameDefinition* game = Arcade::Database::GetGame(set_name))
    m_set_name = game->id;
  else if (const Arcade::Database::GameDefinition* identified_game = Arcade::Database::IdentifyArchive(path))
    m_set_name = identified_game->id;
  else
    m_set_name = set_name;

  populateArcadeUi();
  populateInputProfileUi();
  populateBezelUi();
  updateWindowTitle();
}

void GameSummaryWidget::populateArcadeUi()
{
  const Arcade::Database::GameDefinition* game = Arcade::Database::GetGame(m_set_name);
  m_ui.path->setText(QString::fromStdString(m_path));
  m_ui.serial->setText(QString::fromStdString(m_set_name));

  const bool has_region = game && !game->region.empty();
  m_ui.label_3->setVisible(has_region);
  m_ui.region->setVisible(has_region);
  if (has_region)
    m_ui.region->setText(QString::fromStdString(game->region));

  const bool has_genre = game && !game->genre.empty();
  m_ui.label_11->setVisible(has_genre);
  m_ui.genre->setVisible(has_genre);
  if (has_genre)
    m_ui.genre->setText(QString::fromStdString(game->genre));

  const bool has_manufacturer = game && !game->manufacturer.empty();
  m_ui.label_12->setVisible(has_manufacturer);
  m_ui.developer->setVisible(has_manufacturer);
  if (has_manufacturer)
    m_ui.developer->setText(QString::fromStdString(game->manufacturer));

  QStringList release_info;
  if (game)
  {
    if (game->year != 0)
      release_info.append(QString::number(game->year));
    if (game->max_players != 0)
    {
      if (game->min_players != game->max_players)
        release_info.append(tr("%1-%2 players").arg(game->min_players).arg(game->max_players));
      else if (game->max_players == 1)
        release_info.append(tr("1 player"));
      else
        release_info.append(tr("%1 players").arg(game->max_players));
    }
  }
  const bool has_release_info = !release_info.isEmpty();
  m_ui.label_14->setVisible(has_release_info);
  m_ui.releaseInfo->setVisible(has_release_info);
  if (has_release_info)
    m_ui.releaseInfo->setText(release_info.join(QStringLiteral(", ")));

  const QString controller_summary = GetControllerSummary(m_set_name);
  const bool has_controllers = !controller_summary.isEmpty();
  m_ui.label_15->setVisible(has_controllers);
  m_ui.controllers->setVisible(has_controllers);
  if (has_controllers)
    m_ui.controllers->setText(controller_summary);

  populateCustomAttributes();
}

void GameSummaryWidget::populateInputProfileUi()
{
  m_ui.inputProfile->clear();
  m_ui.inputProfile->addItem(QIcon::fromTheme(QStringLiteral("global-line")), tr("Use Global Settings"));
  m_ui.inputProfile->addItem(QIcon::fromTheme(QStringLiteral("controller-digital-line")),
                             tr("Game Specific Configuration"));
  for (const std::string& name : InputManager::GetInputProfileNames())
    m_ui.inputProfile->addItem(QString::fromStdString(name));

  if (m_dialog->getBoolValue("ArcadeProfiles", "UseGameSettingsForArcadeInput", std::nullopt).value_or(false))
  {
    m_ui.inputProfile->setCurrentIndex(1);
  }
  else if (const std::optional<std::string> profile_name =
             m_dialog->getStringValue("ArcadeProfiles", "InputProfileName", std::nullopt);
           profile_name.has_value() && !profile_name->empty())
  {
    m_ui.inputProfile->setCurrentIndex(m_ui.inputProfile->findText(QString::fromStdString(profile_name.value())));
  }
  else
  {
    m_ui.inputProfile->setCurrentIndex(0);
  }
  m_ui.editInputProfile->setEnabled(m_ui.inputProfile->currentIndex() >= 1);
}

void GameSummaryWidget::populateBezelUi()
{
  const bool enabled = m_dialog->getBoolValue("Display", "BezelEnabled", std::nullopt).value_or(false);
  const std::optional<std::string> path = m_dialog->getStringValue("Display", "BezelPath", std::nullopt);

  const QSignalBlocker enabled_blocker(m_ui.bezelEnabled);
  const QSignalBlocker path_blocker(m_ui.bezelPath);
  m_ui.bezelEnabled->setChecked(enabled);
  m_ui.bezelPath->setText(path.has_value() ? QString::fromStdString(path.value()) : QString());
  m_ui.bezelPath->setModified(false);
}

void GameSummaryWidget::populateCustomAttributes()
{
  const Arcade::Database::GameDefinition* game = Arcade::Database::GetGame(m_set_name);
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(m_path);

  QSignalBlocker title_blocker(m_ui.title);
  if (entry && entry->has_custom_title)
    m_ui.title->setText(QString::fromStdString(entry->title));
  else if (game)
    m_ui.title->setText(QString::fromStdString(game->title));
  else
    m_ui.title->setText(QString::fromStdString(m_set_name));
  m_ui.restoreTitle->setEnabled(entry && entry->has_custom_title);
}

void GameSummaryWidget::updateWindowTitle()
{
  m_dialog->setWindowTitle(tr("%1 [%2]").arg(m_ui.title->text()).arg(m_ui.serial->text()));
}

void GameSummaryWidget::setCustomTitle(const std::string& text)
{
  m_ui.restoreTitle->setEnabled(!text.empty());
  GameList::SaveCustomTitleForPath(m_path, text);
  populateCustomAttributes();
  updateWindowTitle();
  g_main_window->refreshGameListModel();
}

void GameSummaryWidget::onInputProfileChanged(int index)
{
  SettingsInterface* sif = m_dialog->getSettingsInterface();
  if (index == 0)
  {
    sif->DeleteValue("ArcadeProfiles", "InputProfileName");
    sif->DeleteValue("ArcadeProfiles", "UseGameSettingsForArcadeInput");
  }
  else if (index == 1)
  {
    sif->DeleteValue("ArcadeProfiles", "InputProfileName");
    sif->SetBoolValue("ArcadeProfiles", "UseGameSettingsForArcadeInput", true);

    if (!sif->GetBoolValue("ArcadeProfiles", "GameSettingsInitialized", false))
    {
      sif->SetBoolValue("ArcadeProfiles", "GameSettingsInitialized", true);
      {
        const auto lock = Host::GetSettingsLock();
        SettingsInterface* base_sif = Host::Internal::GetBaseSettingsLayer();
        InputManager::CopyConfiguration(sif, *base_sif, true, true, false);

        QWidget* dlg_parent = QtUtils::GetRootWidget(this);
        QMessageBox::information(dlg_parent, dlg_parent->windowTitle(),
                                 tr("Per-game controller configuration initialized with global settings."));
      }
    }
  }
  else
  {
    sif->SetStringValue("ArcadeProfiles", "InputProfileName", m_ui.inputProfile->itemText(index).toUtf8());
    sif->DeleteValue("ArcadeProfiles", "UseGameSettingsForArcadeInput");
  }

  m_dialog->saveAndReloadGameSettings();
  m_ui.editInputProfile->setEnabled(index > 0);
}

void GameSummaryWidget::onEditInputProfileClicked()
{
  if (m_dialog->getBoolValue("ArcadeProfiles", "UseGameSettingsForArcadeInput", std::nullopt).value_or(false))
  {
    ControllerSettingsWindow::editControllerSettingsForGame(QtUtils::GetRootWidget(this),
                                                            m_dialog->getSettingsInterface());
  }
  else if (const std::optional<std::string> profile_name =
             m_dialog->getStringValue("ArcadeProfiles", "InputProfileName", std::nullopt);
           profile_name.has_value() && !profile_name->empty())
  {
    g_main_window->openInputProfileEditor(profile_name.value());
  }
}
void GameSummaryWidget::onBezelEnabledChanged(bool checked)
{
  SettingsInterface* sif = m_dialog->getSettingsInterface();
  sif->SetBoolValue("Display", "BezelEnabled", checked);
  g_main_window->setBezelAspectRatioLockForGame(m_set_name, checked && !m_ui.bezelPath->text().trimmed().isEmpty());
  m_dialog->saveAndReloadGameSettings();
}

void GameSummaryWidget::onBezelBrowseClicked()
{
  const QString filename = QDir::toNativeSeparators(QFileDialog::getOpenFileName(
    this, tr("Select Bezel Image"), m_ui.bezelPath->text(), tr("All Bezel Image Types (*.png *.webp *.jpg *.jpeg *.bmp)")));
  if (filename.isEmpty())
    return;

  const std::optional<QString> imported_path =
    g_main_window->importGameBezelImage(QtUtils::GetRootWidget(this), m_set_name, filename);
  if (!imported_path.has_value())
    return;

  SettingsInterface* sif = m_dialog->getSettingsInterface();
  const QByteArray path_utf8 = imported_path->toUtf8();
  sif->SetStringValue("Display", "BezelPath", path_utf8.constData());
  sif->SetBoolValue("Display", "BezelEnabled", true);

  {
    const QSignalBlocker enabled_blocker(m_ui.bezelEnabled);
    const QSignalBlocker path_blocker(m_ui.bezelPath);
    m_ui.bezelEnabled->setChecked(true);
    m_ui.bezelPath->setText(imported_path.value());
    m_ui.bezelPath->setModified(false);
  }

  g_main_window->setBezelAspectRatioLockForGame(m_set_name, true);
  m_dialog->saveAndReloadGameSettings();
}

void GameSummaryWidget::onBezelClearClicked()
{
  SettingsInterface* sif = m_dialog->getSettingsInterface();
  sif->DeleteValue("Display", "BezelEnabled");
  sif->DeleteValue("Display", "BezelPath");

  {
    const QSignalBlocker enabled_blocker(m_ui.bezelEnabled);
    const QSignalBlocker path_blocker(m_ui.bezelPath);
    m_ui.bezelEnabled->setChecked(false);
    m_ui.bezelPath->clear();
    m_ui.bezelPath->setModified(false);
  }

  g_main_window->setBezelAspectRatioLockForGame(m_set_name, false);
  m_dialog->saveAndReloadGameSettings();
}

void GameSummaryWidget::onBezelPathEditingFinished()
{
  if (!m_ui.bezelPath->isModified())
    return;

  const QString source_path = m_ui.bezelPath->text().trimmed();
  if (source_path.isEmpty())
  {
    onBezelClearClicked();
    return;
  }

  const std::optional<QString> imported_path =
    g_main_window->importGameBezelImage(QtUtils::GetRootWidget(this), m_set_name, source_path);
  if (!imported_path.has_value())
  {
    populateBezelUi();
    return;
  }

  SettingsInterface* sif = m_dialog->getSettingsInterface();
  const QByteArray path_utf8 = imported_path->toUtf8();
  sif->SetStringValue("Display", "BezelPath", path_utf8.constData());

  {
    const QSignalBlocker path_blocker(m_ui.bezelPath);
    m_ui.bezelPath->setText(imported_path.value());
    m_ui.bezelPath->setModified(false);
  }

  g_main_window->setBezelAspectRatioLockForGame(
    m_set_name, m_ui.bezelEnabled->isChecked() && !m_ui.bezelPath->text().trimmed().isEmpty());
  m_dialog->saveAndReloadGameSettings();
}
