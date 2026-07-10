#pragma once
#include "common/types.h"
#include "common/window_info.h"
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QtGlobal>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QWidget>
#include <optional>
#include <utility>
#include <vector>

class QtDisplayWidget final : public QWidget
{
  Q_OBJECT

public:
  QtDisplayWidget(QWidget* parent);
  ~QtDisplayWidget();

  QPaintEngine* paintEngine() const override;

  int scaledWindowWidth() const;
  int scaledWindowHeight() const;
  qreal devicePixelRatioFromScreen() const;

  std::optional<WindowInfo> getWindowInfo() const;

  void setRelativeMode(bool enabled);

Q_SIGNALS:
  void windowFocusEvent();
  void windowResizedEvent(int width, int height);
  void windowRestoredEvent();
  void windowClosedEvent();
  void windowKeyEvent(int key_code, int mods, bool pressed);
  void windowMouseMoveEvent(int x, int y);
  void windowRawMouseMoveEvent(const QString& device_name, int x, int y);
  void windowRawMouseButtonEvent(const QString& device_name, int button, bool pressed);
  void windowMouseRelativeEvent(int dx, int dy);
  void windowMouseButtonEvent(int button, bool pressed);
  void windowMouseWheelEvent(const QPoint& angle_delta);

protected:
  bool event(QEvent* event) override;

#if defined(_WIN32)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override;
#else
  bool nativeEvent(const QByteArray& event_type, void* message, long* result) override;
#endif
#endif

private:
#if defined(_WIN32)
  void registerRawInputMouse();
  void logRawInputDevices();
  QString getRawMouseSettingValue(void* device_handle);

  std::vector<std::pair<QString, QPoint>> m_raw_mouse_positions;
#endif

  QPoint m_relative_mouse_start_position{};
  QPoint m_relative_mouse_last_position{};
  QPoint m_last_mouse_global_position{};
  bool m_has_last_mouse_global_position = false;
  bool m_relative_mouse_enabled = false;
};

class QtDisplayContainer final : public QStackedWidget
{
  Q_OBJECT

public:
  QtDisplayContainer();
  ~QtDisplayContainer();

  static bool IsNeeded(bool fullscreen, bool render_to_main);

  void setDisplayWidget(QtDisplayWidget* widget);
  QtDisplayWidget* removeDisplayWidget();

protected:
  bool event(QEvent* event) override;

private:
  QtDisplayWidget* m_display_widget = nullptr;
};
