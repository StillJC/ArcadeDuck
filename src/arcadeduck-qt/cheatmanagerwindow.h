// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "ui_cheatmanagerwindow.h"

#include "core/types.h"

#include <QtWidgets/QWidget>

class CheatManagerWindow : public QWidget
{
  Q_OBJECT

public:
  CheatManagerWindow();
  ~CheatManagerWindow();

Q_SIGNALS:
  void closed();

protected:
  void showEvent(QShowEvent* event);
  void closeEvent(QCloseEvent* event);
  void resizeEvent(QResizeEvent* event);

private Q_SLOTS:
  void updateCheatList();
  void cheatListCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);
  void cheatListItemActivated(QTreeWidgetItem* item);
  void previousStateClicked();
  void nextStateClicked();
  void activateClicked();

private:
  void connectUi();
  void resizeColumns();
  void updateSelectedCheatUi();
  int getSelectedCheatIndex() const;

  Ui::CheatManagerWindow m_ui;
};
