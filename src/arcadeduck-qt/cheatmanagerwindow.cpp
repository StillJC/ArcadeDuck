// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "cheatmanagerwindow.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/host.h"
#include "core/system.h"

#include <QtCore/QSignalBlocker>

#include <utility>

namespace {

enum ItemDataRole : int
{
  CheatIndexRole = Qt::UserRole,
  CheatTypeRole,
  CanActivateRole,
  CanPreviousRole,
  CanNextRole,
  CommentRole,
  ErrorRole,
};

QString GetCheatTypeName(System::MAMECheatType type)
{
  switch (type)
  {
    case System::MAMECheatType::TextOnly:
      return {};
    case System::MAMECheatType::OneShot:
      return CheatManagerWindow::tr("One Shot");
    case System::MAMECheatType::OnOff:
      return CheatManagerWindow::tr("On / Off");
    case System::MAMECheatType::ValueParameter:
      return CheatManagerWindow::tr("Value");
    case System::MAMECheatType::ItemListParameter:
      return CheatManagerWindow::tr("Choice");
    case System::MAMECheatType::OneShotParameter:
      return CheatManagerWindow::tr("One Shot Value");
    default:
      return {};
  }
}

} // namespace

CheatManagerWindow::CheatManagerWindow() : QWidget()
{
  m_ui.setupUi(this);
  connectUi();
  updateCheatList();
}

CheatManagerWindow::~CheatManagerWindow() = default;

void CheatManagerWindow::connectUi()
{
  connect(m_ui.cheatList, &QTreeWidget::currentItemChanged, this, &CheatManagerWindow::cheatListCurrentItemChanged);
  connect(m_ui.cheatList, &QTreeWidget::itemSelectionChanged, this, &CheatManagerWindow::updateSelectedCheatUi);
  connect(m_ui.cheatList, &QTreeWidget::itemActivated, this, &CheatManagerWindow::cheatListItemActivated);
  connect(m_ui.cheatListPrevious, &QPushButton::clicked, this, &CheatManagerWindow::previousStateClicked);
  connect(m_ui.cheatListNext, &QPushButton::clicked, this, &CheatManagerWindow::nextStateClicked);
  connect(m_ui.cheatListActivate, &QPushButton::clicked, this, &CheatManagerWindow::activateClicked);
  connect(g_emu_thread, &EmuThread::runningGameChanged, this, &CheatManagerWindow::updateCheatList);
}

void CheatManagerWindow::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  resizeColumns();
}

void CheatManagerWindow::closeEvent(QCloseEvent* event)
{
  QWidget::closeEvent(event);
  emit closed();
}

void CheatManagerWindow::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  resizeColumns();
}

void CheatManagerWindow::resizeColumns()
{
  QtUtils::ResizeColumnsForTreeView(m_ui.cheatList, {-1, 140, 160});
}

int CheatManagerWindow::getSelectedCheatIndex() const
{
  const QList<QTreeWidgetItem*> selected = m_ui.cheatList->selectedItems();
  if (selected.isEmpty())
    return -1;

  const QVariant index = selected.first()->data(0, CheatIndexRole);
  return index.isValid() ? static_cast<int>(index.toUInt()) : -1;
}

void CheatManagerWindow::updateCheatList()
{
  const int selected_index = getSelectedCheatIndex();

  m_ui.cheatList->setEnabled(false);
  m_ui.cheatListPrevious->setEnabled(false);
  m_ui.cheatListNext->setEnabled(false);
  m_ui.cheatListActivate->setEnabled(false);
  m_ui.cheatComment->clear();

  Host::RunOnCPUThread([selected_index]() {
    std::vector<System::MAMECheatInfo> cheats = System::GetMAMECheatInfo();

    QtHost::RunOnUIThread([cheats = std::move(cheats), selected_index]() {
      if (!QtHost::IsSystemValid())
        return;

      CheatManagerWindow* const cm = g_main_window->getCheatManagerWindow();
      if (!cm)
        return;

      QSignalBlocker blocker(cm->m_ui.cheatList);
      cm->m_ui.cheatList->clear();

      QTreeWidgetItem* selected_item = nullptr;
      for (const System::MAMECheatInfo& cheat : cheats)
      {
        QTreeWidgetItem* const item = new QTreeWidgetItem(cm->m_ui.cheatList);
        item->setData(0, CheatIndexRole, QVariant(static_cast<uint>(cheat.index)));
        item->setData(0, CheatTypeRole, QVariant(static_cast<uint>(cheat.type)));
        item->setData(0, CanActivateRole, cheat.can_activate);
        item->setData(0, CanPreviousRole, cheat.can_previous);
        item->setData(0, CanNextRole, cheat.can_next);
        item->setData(0, CommentRole, QString::fromStdString(cheat.comment));
        item->setData(0, ErrorRole, QString::fromStdString(cheat.error));
        item->setText(0, QString::fromStdString(cheat.description));
        item->setText(1, GetCheatTypeName(cheat.type));
        item->setText(2, QString::fromStdString(cheat.value));

        if (cheat.type == System::MAMECheatType::TextOnly)
        {
          // MAME text-only entries are headings/separators, not cheat controls.
          item->setFirstColumnSpanned(true);
        }
        else if (!cheat.valid)
        {
          item->setDisabled(true);
        }
        if (selected_index >= 0 && cheat.index == static_cast<u32>(selected_index))
          selected_item = item;
      }

      cm->m_ui.cheatList->setEnabled(!cheats.empty());
      if (!cheats.empty())
        cm->m_ui.cheatList->setCurrentItem(selected_item ? selected_item : cm->m_ui.cheatList->topLevelItem(0));
      else
        cm->m_ui.cheatComment->setText(cm->tr("No MAME cheats are available for this arcade set."));

      cm->resizeColumns();
      cm->updateSelectedCheatUi();
    });
  });
}

void CheatManagerWindow::updateSelectedCheatUi()
{
  const QList<QTreeWidgetItem*> selected = m_ui.cheatList->selectedItems();
  QTreeWidgetItem* const item = selected.isEmpty() ? nullptr : selected.first();

  const bool can_activate = item && item->data(0, CanActivateRole).toBool();
  const bool can_previous = item && item->data(0, CanPreviousRole).toBool();
  const bool can_next = item && item->data(0, CanNextRole).toBool();
  m_ui.cheatListActivate->setEnabled(can_activate);
  m_ui.cheatListPrevious->setEnabled(can_previous);
  m_ui.cheatListNext->setEnabled(can_next);
  m_ui.cheatListPrevious->setText(tr("Previous"));
  m_ui.cheatListNext->setText(tr("Next"));
  m_ui.cheatListActivate->setText(tr("Activate"));

  if (!item)
    return;

  const System::MAMECheatType type =
    static_cast<System::MAMECheatType>(item->data(0, CheatTypeRole).toUInt());
  if (type == System::MAMECheatType::OnOff)
  {
    m_ui.cheatListPrevious->setText(tr("Off"));
    m_ui.cheatListNext->setText(tr("On"));
  }
  else if (type == System::MAMECheatType::OneShotParameter)
  {
    m_ui.cheatListActivate->setText(tr("Apply"));
  }

  const QString error = item->data(0, ErrorRole).toString();
  const QString comment = item->data(0, CommentRole).toString();
  if (!error.isEmpty())
    m_ui.cheatComment->setText(tr("Unsupported: %1").arg(error));
  else
    m_ui.cheatComment->setText(comment);
}

void CheatManagerWindow::cheatListCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous)
{
  Q_UNUSED(current);
  Q_UNUSED(previous);
  updateSelectedCheatUi();
}

void CheatManagerWindow::cheatListItemActivated(QTreeWidgetItem* item)
{
  if (!item)
    return;

  const System::MAMECheatType type =
    static_cast<System::MAMECheatType>(item->data(0, CheatTypeRole).toUInt());
  if (type == System::MAMECheatType::OneShot || type == System::MAMECheatType::OneShotParameter ||
      item->data(0, CanActivateRole).toBool())
  {
    activateClicked();
  }
  else if (item->data(0, CanNextRole).toBool())
  {
    nextStateClicked();
  }
  else if (type == System::MAMECheatType::OnOff && item->data(0, CanPreviousRole).toBool())
  {
    previousStateClicked();
  }
}

void CheatManagerWindow::previousStateClicked()
{
  const int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  Host::RunOnCPUThread([index]() { System::SelectPreviousMAMECheatState(static_cast<u32>(index)); }, true);
  updateCheatList();
}

void CheatManagerWindow::nextStateClicked()
{
  const int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  Host::RunOnCPUThread([index]() { System::SelectNextMAMECheatState(static_cast<u32>(index)); }, true);
  updateCheatList();
}

void CheatManagerWindow::activateClicked()
{
  const int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  Host::RunOnCPUThread([index]() { System::ActivateMAMECheat(static_cast<u32>(index)); }, true);
  updateCheatList();
}
