// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>.
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "common/types.h"

#include "ui_setupwizarddialog.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <array>
#include <string>
#include <vector>

class SetupWizardDialog final : public QDialog
{
  Q_OBJECT

public:
  SetupWizardDialog();
  ~SetupWizardDialog();

private Q_SLOTS:
  bool canShowNextPage();
  void previousPage();
  void nextPage();
  void confirmCancel();

  void themeChanged();
  void languageChanged();

  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onSearchDirectoryListSelectionChanged();
  void refreshDirectoryList();
  void resizeDirectoryListColumns();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  enum Page : u32
  {
    Page_Language,
    Page_BIOS,
    Page_GameList,
    Page_Complete,
    Page_Count,
  };

  void setupUi();
  void setupLanguagePage();
  void setupBIOSPage();
  void setupGameListPage();
  void updateStylesheets();

  void pageChangedTo(int page);
  void updatePageLabels(int prev_page);
  void updatePageButtons();

  void addPathToTable(const std::string& path, bool recursive);

  Ui::SetupWizardDialog m_ui;

  std::array<QLabel*, Page_Count> m_page_labels;
};
