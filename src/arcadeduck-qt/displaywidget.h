// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "util/window_info.h"

#include "common/types.h"

#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QWidget>
#include <optional>

class QByteArray;
class QCloseEvent;

class DisplayWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit DisplayWidget(QWidget* parent);
  ~DisplayWidget();

  QPaintEngine* paintEngine() const override;

  int scaledWindowWidth() const;
  int scaledWindowHeight() const;

  std::optional<WindowInfo> getWindowInfo();

  void updateRelativeMode(bool enabled);
  void updateCursor(bool hidden);
  void setBezelAspectRatioLockEnabled(bool enabled);

  void handleCloseEvent(QCloseEvent* event);
  void destroy();

Q_SIGNALS:
  void windowResizedEvent(int width, int height, float scale);
  void windowRestoredEvent();
  void windowKeyEvent(int key_code, bool pressed);
  void windowTextEntered(const QString& text);
  void windowMouseButtonEvent(int button, bool pressed);
  void windowMouseWheelEvent(const QPoint& angle_delta);
  void windowPointerResyncRequested(float x, float y);

protected:
  bool event(QEvent* event) override;
#ifdef _WIN32
  bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private:
  bool isActuallyFullscreen() const;
  void updateCenterPos();
  void updatePointerAbsoluteDisplayRect();
  void requestWindowedPointerResync();

  QPoint m_relative_mouse_start_pos{};
  QPoint m_relative_mouse_center_pos{};
  bool m_relative_mouse_enabled = false;
#ifdef _WIN32
  bool m_clip_mouse_enabled = false;
#endif
  bool m_cursor_hidden = false;
  bool m_destroying = false;
  bool m_bezel_aspect_ratio_lock_enabled = false;
#ifndef _WIN32
  bool m_bezel_resize_correction_pending = false;
#endif

  std::vector<u32> m_keys_pressed_with_modifiers;

  u32 m_last_window_width = 0;
  u32 m_last_window_height = 0;
  float m_last_window_scale = 1.0f;
};

class DisplayContainer final : public QStackedWidget
{
  Q_OBJECT

public:
  DisplayContainer();
  ~DisplayContainer();

  // Wayland is broken in lots of ways, so we need to check for it.
  static bool isRunningOnWayland();

  static bool isNeeded(bool fullscreen, bool render_to_main);

  void setDisplayWidget(DisplayWidget* widget);
  DisplayWidget* removeDisplayWidget();

protected:
  bool event(QEvent* event) override;

private:
  DisplayWidget* m_display_widget = nullptr;
};
