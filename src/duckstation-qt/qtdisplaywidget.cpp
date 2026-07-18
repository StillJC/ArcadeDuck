#include "qtdisplaywidget.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "qthostinterface.h"
#include "qtutils.h"
#if defined(_WIN32)
#include "common/windows_headers.h"
#endif
#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/QWindowStateChangeEvent>
#include <algorithm>
#include <cmath>
#if defined(_WIN32)
#include <vector>
#endif

#if !defined(_WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif

Log_SetChannel(QtDisplayWidget);

QtDisplayWidget::QtDisplayWidget(QWidget* parent) : QWidget(parent)
{
  // We want a native window for both D3D and OpenGL.
  setAutoFillBackground(false);
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_PaintOnScreen, true);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}

QtDisplayWidget::~QtDisplayWidget() = default;

#if defined(_WIN32)
void QtDisplayWidget::registerRawInputMouse()
{
  RAWINPUTDEVICE rid = {};
  rid.usUsagePage = 0x01;
  rid.usUsage = 0x02;
  rid.dwFlags = RIDEV_INPUTSINK;
  rid.hwndTarget = reinterpret_cast<HWND>(winId());

  if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
  {
    Log_WarningPrintf("Raw Input mouse registration failed: error=%lu", GetLastError());
    return;
  }
}

void QtDisplayWidget::logRawInputDevices()
{
  UINT device_count = 0;
  if (GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST)) != 0 || device_count == 0)
  {
    Log_DevPrintf("Raw Input mouse device list is empty or unavailable: count=%u error=%lu", device_count,
                  GetLastError());
    return;
  }

  std::vector<RAWINPUTDEVICELIST> devices(device_count);
  const UINT result = GetRawInputDeviceList(devices.data(), &device_count, sizeof(RAWINPUTDEVICELIST));
  if (result == static_cast<UINT>(-1))
  {
    Log_WarningPrintf("Failed to enumerate Raw Input mouse devices: error=%lu", GetLastError());
    return;
  }

  Log_DevPrintf("Raw Input device count: %u", device_count);

  for (UINT i = 0; i < device_count; i++)
  {
    if (devices[i].dwType != RIM_TYPEMOUSE)
      continue;

    UINT name_size = 0;
    GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, nullptr, &name_size);

    std::vector<char> name(name_size + 1);
    if (name_size > 0)
      GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, name.data(), &name_size);

    RID_DEVICE_INFO info = {};
    info.cbSize = sizeof(info);
    UINT info_size = sizeof(info);
    if (GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICEINFO, &info, &info_size) == static_cast<UINT>(-1))
    {
      Log_DevPrintf("Failed to query Raw Input mouse %u (%s): error=%lu", i,
                    name.empty() ? "" : name.data(), GetLastError());
      continue;
    }

    Log_DevPrintf("Raw Input mouse %u: id=%lu buttons=%lu rate=%lu horizontal_wheel=%lu name=%s", i,
                  info.mouse.dwId, info.mouse.dwNumberOfButtons, info.mouse.dwSampleRate,
                  info.mouse.fHasHorizontalWheel, name.empty() ? "" : name.data());
  }
}

QString QtDisplayWidget::getRawMouseSettingValue(void* device_handle)
{
  UINT name_size = 0;
  GetRawInputDeviceInfoA(device_handle, RIDI_DEVICENAME, nullptr, &name_size);
  if (name_size == 0)
    return QString();

  std::vector<char> name(name_size + 1);
  if (GetRawInputDeviceInfoA(device_handle, RIDI_DEVICENAME, name.data(), &name_size) == static_cast<UINT>(-1))
    return QString();

  const QString setting_value = QStringLiteral("RawMouse:%1").arg(QString::fromLocal8Bit(name.data()));

  for (const auto& [existing_setting_value, position] : m_raw_mouse_positions)
  {
    if (existing_setting_value == setting_value)
      return setting_value;
  }

  m_raw_mouse_positions.emplace_back(setting_value, QPoint(scaledWindowWidth() / 2, scaledWindowHeight() / 2));

  return setting_value;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool QtDisplayWidget::nativeEvent(const QByteArray& event_type, void* message, qintptr* result)
#else
bool QtDisplayWidget::nativeEvent(const QByteArray& event_type, void* message, long* result)
#endif
{
  Q_UNUSED(event_type);
  Q_UNUSED(result);

  MSG* msg = static_cast<MSG*>(message);
  if (!msg || msg->message != WM_INPUT)
    return false;

  UINT data_size = 0;
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, nullptr, &data_size,
                      sizeof(RAWINPUTHEADER)) != 0 ||
      data_size == 0)
  {
    return false;
  }

  std::vector<BYTE> data(data_size);
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, data.data(), &data_size,
                      sizeof(RAWINPUTHEADER)) != data_size)
  {
    Log_WarningPrintf("Failed to read Raw Input mouse data: error=%lu", GetLastError());
    return false;
  }

  const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(data.data());
  if (raw->header.dwType == RIM_TYPEMOUSE)
  {
    const QString setting_value = getRawMouseSettingValue(raw->header.hDevice);
    if (setting_value.isEmpty())
      return false;

    for (auto& [existing_setting_value, position] : m_raw_mouse_positions)
    {
      if (existing_setting_value != setting_value)
        continue;

      const int max_x = std::max(0, scaledWindowWidth() - 1);
      const int max_y = std::max(0, scaledWindowHeight() - 1);

      int new_x = position.x() + static_cast<int>(raw->data.mouse.lLastX);
      int new_y = position.y() + static_cast<int>(raw->data.mouse.lLastY);

      if (new_x < 0)
        new_x = 0;
      else if (new_x > max_x)
        new_x = max_x;

      if (new_y < 0)
        new_y = 0;
      else if (new_y > max_y)
        new_y = max_y;

      position = QPoint(new_x, new_y);
      emit windowRawMouseMoveEvent(setting_value, new_x, new_y);
      break;
    }

    const USHORT button_flags = raw->data.mouse.usButtonFlags;
    const auto emit_raw_mouse_button = [this, &setting_value, button_flags](USHORT down_flag, USHORT up_flag,
                                                                            int button) {
      if (button_flags & down_flag)
        emit windowRawMouseButtonEvent(setting_value, button, true);

      if (button_flags & up_flag)
        emit windowRawMouseButtonEvent(setting_value, button, false);
    };

    emit_raw_mouse_button(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1);
    emit_raw_mouse_button(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2);
    emit_raw_mouse_button(RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 3);
    emit_raw_mouse_button(RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4);
    emit_raw_mouse_button(RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5);
  }

  return false;
}
#endif

qreal QtDisplayWidget::devicePixelRatioFromScreen() const
{
  QScreen* screen_for_ratio;
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
  screen_for_ratio = windowHandle()->screen();
#else
  screen_for_ratio = screen();
#endif
  if (!screen_for_ratio)
    screen_for_ratio = QGuiApplication::primaryScreen();

  return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

int QtDisplayWidget::scaledWindowWidth() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(width()) * devicePixelRatioFromScreen()));
}

int QtDisplayWidget::scaledWindowHeight() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(height()) * devicePixelRatioFromScreen()));
}

std::optional<WindowInfo> QtDisplayWidget::getWindowInfo() const
{
  WindowInfo wi;

  // Windows and Apple are easy here since there's no display connection.
#if defined(_WIN32)
  wi.type = WindowInfo::Type::Win32;
  wi.window_handle = reinterpret_cast<void*>(winId());
#elif defined(__APPLE__)
  wi.type = WindowInfo::Type::MacOS;
  wi.window_handle = reinterpret_cast<void*>(winId());
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  const QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("xcb"))
  {
    wi.type = WindowInfo::Type::X11;
    wi.display_connection = pni->nativeResourceForWindow("display", windowHandle());
    wi.window_handle = reinterpret_cast<void*>(winId());
  }
  else if (platform_name == QStringLiteral("wayland"))
  {
    wi.type = WindowInfo::Type::Wayland;
    wi.display_connection = pni->nativeResourceForWindow("display", windowHandle());
    wi.window_handle = pni->nativeResourceForWindow("surface", windowHandle());
  }
  else
  {
    qCritical() << "Unknown PNI platform " << platform_name;
    return std::nullopt;
  }
#endif

  wi.surface_width = scaledWindowWidth();
  wi.surface_height = scaledWindowHeight();
  wi.surface_scale = devicePixelRatioFromScreen();
  wi.surface_format = WindowInfo::SurfaceFormat::RGB8;

  return wi;
}

void QtDisplayWidget::setRelativeMode(bool enabled)
{
  if (m_relative_mouse_enabled == enabled)
    return;

  m_has_last_mouse_global_position = false;

  if (enabled)
  {
    m_relative_mouse_start_position = QCursor::pos();

    const QPoint center_pos = mapToGlobal(QPoint(width() / 2, height() / 2));
    QCursor::setPos(center_pos);

    m_relative_mouse_last_position = center_pos;
    m_last_mouse_global_position = center_pos;
    m_has_last_mouse_global_position = true;

    grabMouse();
  }
  else
  {
    QCursor::setPos(m_relative_mouse_start_position);
    releaseMouse();
  }

  m_relative_mouse_enabled = enabled;
}

QPaintEngine* QtDisplayWidget::paintEngine() const
{
  return nullptr;
}

bool QtDisplayWidget::event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      const QKeyEvent* key_event = static_cast<QKeyEvent*>(event);
      if (!key_event->isAutoRepeat())
      {
        emit windowKeyEvent(key_event->key(), static_cast<int>(key_event->modifiers()),
                            event->type() == QEvent::KeyPress);
      }

      return true;
    }

    case QEvent::MouseMove:
    {
      const QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);

      if (!m_relative_mouse_enabled)
      {
        const qreal dpr = devicePixelRatioFromScreen();
        const QPoint mouse_pos = mouse_event->pos();
        const QPoint global_pos = mapToGlobal(mouse_pos);

        const int scaled_x = static_cast<int>(static_cast<qreal>(mouse_pos.x()) * dpr);
        const int scaled_y = static_cast<int>(static_cast<qreal>(mouse_pos.y()) * dpr);

        windowMouseMoveEvent(scaled_x, scaled_y);

        if (m_has_last_mouse_global_position)
        {
          const int dx = global_pos.x() - m_last_mouse_global_position.x();
          const int dy = global_pos.y() - m_last_mouse_global_position.y();

          if (dx != 0 || dy != 0)
            emit windowMouseRelativeEvent(dx, dy);
        }

        m_last_mouse_global_position = global_pos;
        m_has_last_mouse_global_position = true;
      }
      else
      {
        const QPoint center_pos = mapToGlobal(QPoint((width() + 1) / 2, (height() + 1) / 2));
        const QPoint mouse_pos = mapToGlobal(mouse_event->pos());

        const int dx = mouse_pos.x() - center_pos.x();
        const int dy = mouse_pos.y() - center_pos.y();

        if (dx != 0 || dy != 0)
          emit windowMouseRelativeEvent(dx, dy);

        m_relative_mouse_last_position.setX(m_relative_mouse_last_position.x() + dx);
        m_relative_mouse_last_position.setY(m_relative_mouse_last_position.y() + dy);

        windowMouseMoveEvent(m_relative_mouse_last_position.x(), m_relative_mouse_last_position.y());

        QCursor::setPos(center_pos);

        m_last_mouse_global_position = center_pos;
        m_has_last_mouse_global_position = true;

#if 0
    qCritical() << "center" << center_pos.x() << "," << center_pos.y();
    qCritical() << "mouse" << mouse_pos.x() << "," << mouse_pos.y();
    qCritical() << "dxdy" << dx << "," << dy;
#endif
      }

      return true;
    }
        
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    {
      const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
      emit windowMouseButtonEvent(static_cast<int>(button_index + 1u), event->type() != QEvent::MouseButtonRelease);
      return true;
    }

    case QEvent::Wheel:
    {
      const QWheelEvent* wheel_event = static_cast<QWheelEvent*>(event);
      emit windowMouseWheelEvent(wheel_event->angleDelta());
      return true;
    }

    case QEvent::Resize:
    {
      QWidget::event(event);

      emit windowResizedEvent(scaledWindowWidth(), scaledWindowHeight());
      return true;
    }

    case QEvent::Close:
    {
      emit windowClosedEvent();
      QWidget::event(event);
      return true;
    }

    case QEvent::WindowStateChange:
    {
      QWidget::event(event);

      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit windowRestoredEvent();

      return true;
    }

    case QEvent::FocusIn:
    {
      QWidget::event(event);
      emit windowFocusEvent();
      return true;
    }

    case QEvent::ActivationChange:
    {
      QWidget::event(event);
      if (isActiveWindow())
        emit windowFocusEvent();

      return true;
    }

    default:
      return QWidget::event(event);
  }
}

QtDisplayContainer::QtDisplayContainer() : QStackedWidget(nullptr) {}

QtDisplayContainer::~QtDisplayContainer() = default;

bool QtDisplayContainer::IsNeeded(bool fullscreen, bool render_to_main)
{
#if defined(_WIN32) || defined(__APPLE__)
  return false;
#else
  if (fullscreen || render_to_main)
    return false;

  // We only need this on Wayland because of client-side decorations...
  const QString platform_name = QGuiApplication::platformName();
  return (platform_name == QStringLiteral("wayland"));
#endif
}

void QtDisplayContainer::setDisplayWidget(QtDisplayWidget* widget)
{
  Assert(!m_display_widget);
  m_display_widget = widget;
  addWidget(widget);
}

QtDisplayWidget* QtDisplayContainer::removeDisplayWidget()
{
  QtDisplayWidget* widget = m_display_widget;
  Assert(widget);
  m_display_widget = nullptr;
  removeWidget(widget);
  return widget;
}

bool QtDisplayContainer::event(QEvent* event)
{
  const bool res = QStackedWidget::event(event);
  if (!m_display_widget)
    return res;

  switch (event->type())
  {
    case QEvent::Close:
    {
      emit m_display_widget->windowClosedEvent();
    }
    break;

    case QEvent::WindowStateChange:
    {
      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit m_display_widget->windowRestoredEvent();
    }
    break;

    case QEvent::FocusIn:
    {
      emit m_display_widget->windowFocusEvent();
    }
    break;

    case QEvent::ActivationChange:
    {
      if (isActiveWindow())
        emit m_display_widget->windowFocusEvent();
    }
    break;

    default:
      break;
  }

  return res;
}
