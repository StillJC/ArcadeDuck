// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "common/types.h"
#include "util/input_manager.h"

#include <QtWidgets/QWidget>

#include <string>

class ControllerSettingsWindow;
class QComboBox;
class QFormLayout;
class QTabWidget;
class QVBoxLayout;

namespace Arcade {
enum class ArcadeControllerType : u8;
}

class ArcadeControllerBindingWidget final : public QWidget
{
public:
  ArcadeControllerBindingWidget(QWidget* parent, ControllerSettingsWindow* dialog, u32 port);

  u32 getPortNumber() const { return m_port_number; }
  QString getDisplayName() const;
  void queueRebuild();

private:
  std::string getConfigSection() const;
  void rebuildPage();
  void rebuildLayoutList();
  void addBinding(QFormLayout* layout, const char* key, const QString& label, InputBindingInfo::Type type);
  void addBindingsPage(QVBoxLayout* layout);
  QWidget* createArtworkWidget();
  void addSettingsPage(QVBoxLayout* layout);
  void setStringValue(const char* key, const char* value);
  void setBoolValue(const char* key, bool value);
  void setIntValue(const char* key, s32 value);
  void clearAllBindingValues();
  void clearControllerSpecificValues();
  void clearBindings();
  Arcade::ArcadeControllerType getType() const;
  bool usesNativeDevice() const;

  ControllerSettingsWindow* m_dialog;
  u32 m_port_number;
  QComboBox* m_type_combo = nullptr;
  QComboBox* m_layout_combo = nullptr;
  QTabWidget* m_tabs = nullptr;
  bool m_rebuild_queued = false;
};
