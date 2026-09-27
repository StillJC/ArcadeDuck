// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QtWidgets/QWidget>

class QCheckBox;
class QLineEdit;
class QSpinBox;
class SettingsWindow;

class SystemLinkSettingsWidget final : public QWidget
{
public:
  explicit SystemLinkSettingsWidget(SettingsWindow* dialog, QWidget* parent);

private:
  void updateEnabledState();

  QCheckBox* m_enabled = nullptr;
  QSpinBox* m_cabinet_id = nullptr;
  QLineEdit* m_server_address = nullptr;
  QSpinBox* m_port = nullptr;
};
