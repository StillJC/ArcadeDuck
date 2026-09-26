// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include <QtWidgets/QWidget>

#include "ui_consolesettingswidget.h"

class SettingsWindow;

class ConsoleSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ConsoleSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~ConsoleSettingsWidget();

private Q_SLOTS:
  void onOptimalFramePacingChanged();
  void onPreFrameSleepChanged();
  void updateSkipDuplicateFramesEnabled();

private:

  Ui::ConsoleSettingsWidget m_ui;

  SettingsWindow* m_dialog;
};
