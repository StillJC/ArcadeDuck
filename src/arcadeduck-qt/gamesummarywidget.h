// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "common/types.h"

#include <QtWidgets/QWidget>

#include "ui_gamesummarywidget.h"

#include <string>

class SettingsWindow;

class GameSummaryWidget : public QWidget
{
  Q_OBJECT

public:
  GameSummaryWidget(const std::string& path, const std::string& set_name, SettingsWindow* dialog, QWidget* parent);
  ~GameSummaryWidget();

private Q_SLOTS:
  void onInputProfileChanged(int index);
  void onEditInputProfileClicked();
  void onBezelEnabledChanged(bool checked);
  void onBezelBrowseClicked();
  void onBezelClearClicked();
  void onBezelPathEditingFinished();

private:
  void populateUi(const std::string& path, const std::string& set_name);
  void populateArcadeUi();
  void populateInputProfileUi();
  void populateBezelUi();
  void populateCustomAttributes();
  void updateWindowTitle();
  void setCustomTitle(const std::string& text);

  Ui::GameSummaryWidget m_ui;
  SettingsWindow* m_dialog;

  std::string m_path;
  std::string m_set_name;
};
