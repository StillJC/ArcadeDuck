// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "displaywidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/fullscreen_ui.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/QWindowStateChangeEvent>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include "common/windows_headers.h"

static void ConstrainWindowSizingRectTo16By9(RECT* rect, WPARAM edge, LONG extra_width, LONG extra_height)
{
  const LONG outer_width = std::max<LONG>(rect->right - rect->left, extra_width + 16);
  const LONG outer_height = std::max<LONG>(rect->bottom - rect->top, extra_height + 9);
  const LONG client_width = std::max<LONG>(outer_width - extra_width, 1);
  const LONG client_height = std::max<LONG>(outer_height - extra_height, 1);

  LONG target_client_width = client_width;
  LONG target_client_height = client_height;

  if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT)
  {
    target_client_height = std::max<LONG>((client_width * 9 + 8) / 16, 1);
  }
  else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM)
  {
    target_client_width = std::max<LONG>((client_height * 16 + 4) / 9, 1);
  }
  else
  {
    const LONG height_from_width = std::max<LONG>((client_width * 9 + 8) / 16, 1);
    const LONG width_from_height = std::max<LONG>((client_height * 16 + 4) / 9, 1);
    if (std::abs(height_from_width - client_height) <= std::abs(width_from_height - client_width))
      target_client_height = height_from_width;
    else
      target_client_width = width_from_height;
  }

  const LONG target_outer_width = target_client_width + extra_width;
  const LONG target_outer_height = target_client_height + extra_height;
  const bool dragging_left = (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT);
  const bool dragging_top = (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT);

  if (dragging_left)
    rect->left = rect->right - target_outer_width;
  else
    rect->right = rect->left + target_outer_width;

  if (dragging_top)
    rect->top = rect->bottom - target_outer_height;
  else
    rect->bottom = rect->top + target_outer_height;
}
#endif

#if !defined(_WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif

Log_SetChannel(DisplayWidget);

DisplayWidget::DisplayWidget(QWidget* parent) : QWidget(parent)
{
  // We want a native window for both D3D and OpenGL.
  setAutoFillBackground(false);
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_PaintOnScreen, true);
  setAttribute(Qt::WA_KeyCompression, false);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}

DisplayWidget::~DisplayWidget() = default;

int DisplayWidget::scaledWindowWidth() const
{
  return std::max(
    static_cast<int>(std::ceil(static_cast<qreal>(width()) * QtUtils::GetDevicePixelRatioForWidget(this))), 1);
}

int DisplayWidget::scaledWindowHeight() const
{
  return std::max(
    static_cast<int>(std::ceil(static_cast<qreal>(height()) * QtUtils::GetDevicePixelRatioForWidget(this))), 1);
}

std::optional<WindowInfo> DisplayWidget::getWindowInfo()
{
  std::optional<WindowInfo> ret(QtUtils::GetWindowInfoForWidget(this));
  if (ret.has_value())
  {
    m_last_window_width = ret->surface_width;
    m_last_window_height = ret->surface_height;
    m_last_window_scale = ret->surface_scale;
  }
  updatePointerAbsoluteDisplayRect();
  return ret;
}

void DisplayWidget::updatePointerAbsoluteDisplayRect()
{
#ifdef _WIN32
  RECT rect;
  if (!GetWindowRect(reinterpret_cast<HWND>(winId()), &rect))
    return;
  const float left = static_cast<float>(GetSystemMetrics(SM_XVIRTUALSCREEN));
  const float top = static_cast<float>(GetSystemMetrics(SM_YVIRTUALSCREEN));
  const float width = static_cast<float>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
  const float height = static_cast<float>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
  if (width > 0.0f && height > 0.0f)
    InputManager::SetPointerAbsoluteDisplayRect((rect.left - left) / width, (rect.top - top) / height,
                                                (rect.right - left) / width, (rect.bottom - top) / height);
#endif
}

void DisplayWidget::requestWindowedPointerResync()
{
  const QPoint local_pos = mapFromGlobal(QCursor::pos());
  if (!rect().contains(local_pos))
    return;

  const float widget_width = static_cast<float>(std::max(width(), 1));
  const float widget_height = static_cast<float>(std::max(height(), 1));
  const float x = std::clamp((static_cast<float>(local_pos.x()) + 0.5f) / widget_width, 0.0f, 1.0f);
  const float y = std::clamp((static_cast<float>(local_pos.y()) + 0.5f) / widget_height, 0.0f, 1.0f);
  emit windowPointerResyncRequested(x, y);
}

void DisplayWidget::updateRelativeMode(bool enabled)
{
#ifdef _WIN32
  // prefer ClipCursor() over warping movement when we're using raw input
  bool clip_cursor = enabled && InputManager::IsUsingRawInput();
  if (m_relative_mouse_enabled == enabled && m_clip_mouse_enabled == clip_cursor)
    return;

  INFO_LOG("updateRelativeMode(): relative={}, clip={}", enabled ? "yes" : "no", clip_cursor ? "yes" : "no");

  if (!clip_cursor && m_clip_mouse_enabled)
  {
    m_clip_mouse_enabled = false;
    ClipCursor(nullptr);
  }
#else
  if (m_relative_mouse_enabled == enabled)
    return;

  INFO_LOG("updateRelativeMode(): relative={}", enabled ? "yes" : "no");
#endif

  if (enabled)
  {
    m_relative_mouse_enabled = true;
#ifdef _WIN32
    m_clip_mouse_enabled = clip_cursor;
#endif
    m_relative_mouse_start_pos = QCursor::pos();
    updateCenterPos();
    grabMouse();
  }
  else if (m_relative_mouse_enabled)
  {
    m_relative_mouse_enabled = false;
    QCursor::setPos(m_relative_mouse_start_pos);
    releaseMouse();
  }
}

void DisplayWidget::updateCursor(bool hidden)
{
  if (m_cursor_hidden == hidden)
    return;

  m_cursor_hidden = hidden;
  if (hidden)
  {
    DEV_LOG("updateCursor(): Cursor is now hidden");
    setCursor(Qt::BlankCursor);
  }
  else
  {
    DEV_LOG("updateCursor(): Cursor is now shown");
    unsetCursor();
  }
}

void DisplayWidget::setBezelAspectRatioLockEnabled(bool enabled)
{
  m_bezel_aspect_ratio_lock_enabled = enabled;
}

#ifdef _WIN32
bool DisplayWidget::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
  static constexpr const char win_type[] = "windows_generic_MSG";
  if (eventType == QByteArray(win_type, sizeof(win_type) - 1))
  {
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_SIZING && m_bezel_aspect_ratio_lock_enabled && isWindow() && !isFullScreen() &&
        !isMaximized())
    {
      RECT window_rect;
      RECT client_rect;
      if (GetWindowRect(msg->hwnd, &window_rect) && GetClientRect(msg->hwnd, &client_rect))
      {
        const LONG extra_width = std::max<LONG>((window_rect.right - window_rect.left) -
                                                  (client_rect.right - client_rect.left),
                                                0);
        const LONG extra_height = std::max<LONG>((window_rect.bottom - window_rect.top) -
                                                   (client_rect.bottom - client_rect.top),
                                                 0);
        ConstrainWindowSizingRectTo16By9(reinterpret_cast<RECT*>(msg->lParam), msg->wParam, extra_width,
                                         extra_height);
        *result = TRUE;
        return true;
      }
    }
  }

  return QWidget::nativeEvent(eventType, message, result);
}
#endif

void DisplayWidget::handleCloseEvent(QCloseEvent* event)
{
  event->ignore();

  // Closing the separate widget will either cancel the close, or trigger shutdown.
  // In the latter case, it's going to destroy us, so don't let Qt do it first.
  // Treat a close event while fullscreen as an exit, that way ALT+F4 closes ArcadeDuck,
  // rather than just the game.
  if (QtHost::IsSystemValid() && !isActuallyFullscreen())
  {
    QMetaObject::invokeMethod(g_main_window, "requestShutdown", Qt::QueuedConnection, Q_ARG(bool, true),
                              Q_ARG(bool, true), Q_ARG(bool, false));
  }
  else
  {
    QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection);
  }
}

void DisplayWidget::destroy()
{
  m_destroying = true;

#ifdef _WIN32
  if (m_clip_mouse_enabled)
    ClipCursor(nullptr);
#endif

#ifdef __APPLE__
  // See Qt documentation, entire application is in full screen state, and the main
  // window will get reopened fullscreen instead of windowed if we don't close the
  // fullscreen window first.
  if (isActuallyFullscreen())
    close();
#endif
  deleteLater();
}

bool DisplayWidget::isActuallyFullscreen() const
{
  // I hate you QtWayland... have to check the parent, not ourselves.
  QWidget* container = qobject_cast<QWidget*>(parent());
  return container ? container->isFullScreen() : isFullScreen();
}

void DisplayWidget::updateCenterPos()
{
#ifdef _WIN32
  if (m_clip_mouse_enabled)
  {
    RECT rc;
    if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
      ClipCursor(&rc);
  }
  else if (m_relative_mouse_enabled)
  {
    RECT rc;
    if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
    {
      m_relative_mouse_center_pos.setX(((rc.right - rc.left) / 2) + rc.left);
      m_relative_mouse_center_pos.setY(((rc.bottom - rc.top) / 2) + rc.top);
      SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
    }
  }
#else
  if (m_relative_mouse_enabled)
  {
    // we do a round trip here because these coordinates are dpi-unscaled
    m_relative_mouse_center_pos = mapToGlobal(QPoint((width() + 1) / 2, (height() + 1) / 2));
    QCursor::setPos(m_relative_mouse_center_pos);
    m_relative_mouse_center_pos = QCursor::pos();
  }
#endif
}

QPaintEngine* DisplayWidget::paintEngine() const
{
  return nullptr;
}

bool DisplayWidget::event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      const QKeyEvent* key_event = static_cast<QKeyEvent*>(event);

      if (ImGuiManager::WantsTextInput() && key_event->type() == QEvent::KeyPress)
      {
        // Don't forward backspace characters. We send the backspace as a normal key event,
        // so if we send the character too, it double-deletes.
        QString text(key_event->text());
        text.remove(QChar('\b'));
        if (!text.isEmpty())
          emit windowTextEntered(text);
      }

      if (key_event->isAutoRepeat())
        return true;

      // For some reason, Windows sends "fake" key events.
      // Scenario: Press shift, press F1, release shift, release F1.
      // Events: Shift=Pressed, F1=Pressed, Shift=Released, **F1=Pressed**, F1=Released.
      // To work around this, we keep track of keys pressed with modifiers in a list, and
      // discard the press event when it's been previously activated. It's pretty gross,
      // but I can't think of a better way of handling it, and there doesn't appear to be
      // any window flag which changes this behavior that I can see.

      const u32 key = QtUtils::KeyEventToCode(key_event);
      const Qt::KeyboardModifiers modifiers = key_event->modifiers();
      const bool pressed = (key_event->type() == QEvent::KeyPress);
      const auto it = std::find(m_keys_pressed_with_modifiers.begin(), m_keys_pressed_with_modifiers.end(), key);
      if (it != m_keys_pressed_with_modifiers.end())
      {
        if (pressed)
          return true;
        else
          m_keys_pressed_with_modifiers.erase(it);
      }
      else if (modifiers != Qt::NoModifier && modifiers != Qt::KeypadModifier && pressed)
      {
        m_keys_pressed_with_modifiers.push_back(key);
      }

      emit windowKeyEvent(key, pressed);
      return true;
    }

    case QEvent::FocusIn:
    {
      QWidget::event(event);
      requestWindowedPointerResync();
      return true;
    }

    case QEvent::MouseMove:
    {
      if (!m_relative_mouse_enabled)
      {
        const qreal dpr = QtUtils::GetDevicePixelRatioForWidget(this);
        const QPoint mouse_pos = static_cast<QMouseEvent*>(event)->pos();

        const float scaled_x = static_cast<float>(static_cast<qreal>(mouse_pos.x()) * dpr);
        const float scaled_y = static_cast<float>(static_cast<qreal>(mouse_pos.y()) * dpr);
        InputManager::UpdatePointerAbsolutePosition(0, scaled_x, scaled_y);
      }
      else
      {
        // On windows, we use winapi here. The reason being that the coordinates in QCursor
        // are un-dpi-scaled, so we lose precision at higher desktop scalings.
        float dx = 0.0f, dy = 0.0f;

#ifndef _WIN32
        const QPoint mouse_pos = QCursor::pos();
        if (mouse_pos != m_relative_mouse_center_pos)
        {
          dx = static_cast<float>(mouse_pos.x() - m_relative_mouse_center_pos.x());
          dy = static_cast<float>(mouse_pos.y() - m_relative_mouse_center_pos.y());
          QCursor::setPos(m_relative_mouse_center_pos);
        }
#else
        POINT mouse_pos;
        if (GetCursorPos(&mouse_pos))
        {
          dx = static_cast<float>(mouse_pos.x - m_relative_mouse_center_pos.x());
          dy = static_cast<float>(mouse_pos.y - m_relative_mouse_center_pos.y());
          SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
        }
#endif

        if (!InputManager::IsUsingRawInput())
        {
          if (dx != 0.0f)
            InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::X, dx);
          if (dy != 0.0f)
            InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::Y, dy);
        }
      }

      return true;
    }

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    {
      if (event->type() == QEvent::MouseButtonPress)
        requestWindowedPointerResync();

      if (!m_relative_mouse_enabled || !InputManager::IsUsingRawInput())
      {
        const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
        emit windowMouseButtonEvent(static_cast<int>(button_index), event->type() != QEvent::MouseButtonRelease);
      }

      // don't toggle fullscreen when we're bound.. that wouldn't end well.
      if (event->type() == QEvent::MouseButtonDblClick &&
          static_cast<const QMouseEvent*>(event)->button() == Qt::LeftButton && QtHost::IsSystemValid() &&
          !FullscreenUI::HasActiveWindow() &&
          ((!QtHost::IsSystemPaused() &&
            !InputManager::HasAnyBindingsForKey(InputManager::MakePointerButtonKey(0, 0))) ||
           (QtHost::IsSystemPaused() && !ImGuiManager::WantsMouseInput())) &&
          Host::GetBoolSettingValue("Main", "DoubleClickTogglesFullscreen", true))
      {
        g_emu_thread->toggleFullscreen();
      }

      return true;
    }

    case QEvent::Wheel:
    {
      const QWheelEvent* wheel_event = static_cast<QWheelEvent*>(event);
      emit windowMouseWheelEvent(wheel_event->angleDelta());
      return true;
    }

      // According to https://bugreports.qt.io/browse/QTBUG-95925 the recommended practice for handling DPI change is
      // responding to paint events
    case QEvent::Paint:
    case QEvent::Resize:
    {
      QWidget::event(event);
#ifndef _WIN32
      if (event->type() == QEvent::Resize && m_bezel_aspect_ratio_lock_enabled && isWindow() && !isFullScreen() &&
          !isMaximized() && !m_bezel_resize_correction_pending)
      {
        m_bezel_resize_correction_pending = true;
        QTimer::singleShot(0, this, [this]() {
          m_bezel_resize_correction_pending = false;
          if (!m_bezel_aspect_ratio_lock_enabled || !isWindow() || isFullScreen() || isMaximized() || height() <= 0)
            return;

          const int target_width =
            std::max(static_cast<int>(std::lround(static_cast<double>(height()) * 16.0 / 9.0)), 1);
          if (std::abs(width() - target_width) > 1)
            resize(target_width, height());
        });
      }
#endif
      updatePointerAbsoluteDisplayRect();

      const float dpr = QtUtils::GetDevicePixelRatioForWidget(this);
      const u32 scaled_width =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(width()) * dpr)), 1));
      const u32 scaled_height =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(height()) * dpr)), 1));

      // avoid spamming resize events for paint events (sent on move on windows)
      if (m_last_window_width != scaled_width || m_last_window_height != scaled_height || m_last_window_scale != dpr)
      {
        m_last_window_width = scaled_width;
        m_last_window_height = scaled_height;
        m_last_window_scale = dpr;
        emit windowResizedEvent(scaled_width, scaled_height, dpr);
      }

      updateCenterPos();
      return true;
    }

    case QEvent::Move:
    {
      updateCenterPos();
      return true;
    }

    case QEvent::Close:
    {
      if (m_destroying)
        return QWidget::event(event);

      handleCloseEvent(static_cast<QCloseEvent*>(event));
      return true;
    }

    case QEvent::WindowStateChange:
    {
      QWidget::event(event);

      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit windowRestoredEvent();

      // Fullscreen/windowed transitions change the display geometry underneath the
      // Raw Input virtual pointer. Resynchronize on the next event-loop iteration,
      // after Qt/Windows has committed the new widget geometry.
      QTimer::singleShot(0, this, [this]() { requestWindowedPointerResync(); });

      return true;
    }

    default:
      return QWidget::event(event);
  }
}

DisplayContainer::DisplayContainer() : QStackedWidget(nullptr)
{
}

DisplayContainer::~DisplayContainer() = default;

bool DisplayContainer::isNeeded(bool fullscreen, bool render_to_main)
{
#if defined(_WIN32) || defined(__APPLE__)
  return false;
#else
  if (!isRunningOnWayland())
    return false;

  // We only need this on Wayland because of client-side decorations...
  return (fullscreen || !render_to_main);
#endif
}

bool DisplayContainer::isRunningOnWayland()
{
#if defined(_WIN32) || defined(__APPLE__)
  return false;
#else
  const QString platform_name = QGuiApplication::platformName();
  return (platform_name == QStringLiteral("wayland"));
#endif
}

void DisplayContainer::setDisplayWidget(DisplayWidget* widget)
{
  Assert(!m_display_widget);
  m_display_widget = widget;
  addWidget(widget);
}

DisplayWidget* DisplayContainer::removeDisplayWidget()
{
  DisplayWidget* widget = m_display_widget;
  Assert(widget);
  m_display_widget = nullptr;
  removeWidget(widget);
  return widget;
}

bool DisplayContainer::event(QEvent* event)
{
  if (event->type() == QEvent::Close && m_display_widget)
  {
    m_display_widget->handleCloseEvent(static_cast<QCloseEvent*>(event));
    return true;
  }

  const bool res = QStackedWidget::event(event);
  if (!m_display_widget)
    return res;

  switch (event->type())
  {
    case QEvent::WindowStateChange:
    {
      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit m_display_widget->windowRestoredEvent();
    }
    break;

    default:
      break;
  }

  return res;
}
