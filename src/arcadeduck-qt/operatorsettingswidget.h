// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "ui_operatorsettingswidget.h"

#include <QtWidgets/QWidget>

class SettingsWindow;

class OperatorSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit OperatorSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~OperatorSettingsWidget();

private:
  Ui::OperatorSettingsWidget m_ui;
};
