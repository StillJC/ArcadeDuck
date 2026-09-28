// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "mainwindow.h"
#include "aboutdialog.h"
#include "achievementlogindialog.h"
#include "autoupdaterdialog.h"
#include "cheatmanagerwindow.h"
#include "coverdownloaddialog.h"
#include "debuggerwindow.h"
#include "displaywidget.h"
#include "gamelistmodel.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "interfacesettingswidget.h"
#include "logwindow.h"
#include "memoryscannerwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/arcade/arcade_database.h"
#include "core/arcade/arcade_machine_handler.h"
#include "core/game_list.h"
#include "core/fullscreen_ui.h"
#include "core/host.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/gpu_device.h"
#include "util/ini_settings_interface.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QCursor>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStyleFactory>
#include <cmath>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <Dbt.h>
#include <VersionHelpers.h>

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

#ifdef __APPLE__
#include "common/cocoa_tools.h"
#endif

Log_SetChannel(MainWindow);

static constexpr char ARCADE_SET_FILTER[] = QT_TRANSLATE_NOOP("MainWindow", "Arcade Set Archives (*.zip)");

MainWindow* g_main_window = nullptr;

#if defined(_WIN32) || defined(__APPLE__)
static const bool s_use_central_widget = false;
#else
// Qt Wayland is broken. Any sort of stacked widget usage fails to update,
// leading to broken window resizes, no display rendering, etc. So, we mess
// with the central widget instead. Which we can't do on xorg, because it
// breaks window resizing there...
static bool s_use_central_widget = false;
#endif

// UI thread VM validity.
static bool s_system_valid = false;
static bool s_system_paused = false;
static QString s_current_game_title;

static void ShowSaveStateUnsupportedMessage(QWidget* parent)
{
  QMessageBox::information(parent, MainWindow::tr("Under Investigation"),
                           QString::fromUtf8(System::GetSaveStateUnsupportedMessage()), QMessageBox::Ok);
}

static bool CheckSaveStateAvailability(QWidget* parent)
{
  // Save states are currently disabled for ArcadeDuck. In particular, the desktop Load State action can be
  // triggered while no machine is running, where System::SupportsSaveStates() alone would otherwise report the
  // idle default and allow the user to reach the file picker before the core rejects the selected state.
  if (System::IsValid() && System::SupportsSaveStates())
    return true;

  ShowSaveStateUnsupportedMessage(parent);
  return false;
}

static bool ArcadeGameSupportsSaveStates(const Arcade::Database::GameDefinition* game)
{
  if (!game)
    return false;

  const Arcade::Database::SystemDefinition* system = Arcade::Database::GetSystem(game->system_id);
  return system && Arcade::MachineHandlerSupportsSaveStates(system->machine_handler);
}

static const Arcade::Database::GameDefinition* GetArcadeGameForEntry(const GameList::Entry* entry)
{
  if (const Arcade::Database::GameDefinition* game = Arcade::Database::GetGame(entry->serial))
    return game;
  return Arcade::Database::IdentifyArchive(entry->path);
}

static QString s_current_game_serial;
static QString s_current_game_path;
static QIcon s_current_game_icon;

bool QtHost::IsSystemPaused()
{
  return s_system_paused;
}

bool QtHost::IsSystemValid()
{
  return s_system_valid;
}

const QString& QtHost::GetCurrentGameTitle()
{
  return s_current_game_title;
}

const QString& QtHost::GetCurrentGameSerial()
{
  return s_current_game_serial;
}

const QString& QtHost::GetCurrentGamePath()
{
  return s_current_game_path;
}

MainWindow::MainWindow() : QMainWindow(nullptr)
{
  Assert(!g_main_window);
  g_main_window = this;

#if !defined(_WIN32) && !defined(__APPLE__)
  s_use_central_widget = DisplayContainer::isRunningOnWayland();
#endif

  initialize();
}

MainWindow::~MainWindow()
{
  Assert(!m_display_widget);
  Assert(!m_debugger_window);
  cancelGameListRefresh();

  // we compare here, since recreate destroys the window later
  if (g_main_window == this)
    g_main_window = nullptr;

#ifdef _WIN32
  unregisterForDeviceNotifications();
#endif
#ifdef __APPLE__
  CocoaTools::RemoveThemeChangeHandler(this);
#endif
}

void MainWindow::initialize()
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();

  restoreStateFromConfig();
  switchToGameListView();
  updateWindowTitle();


#ifdef _WIN32
  registerForDeviceNotifications();
#endif

#ifdef __APPLE__
  CocoaTools::AddThemeChangeHandler(this,
                                    [](void* ctx) { QtHost::RunOnUIThread([] { g_main_window->updateTheme(); }); });
#endif
}

void MainWindow::reportError(const QString& title, const QString& message)
{
  QMessageBox::critical(this, title, message, QMessageBox::Ok);
}

bool MainWindow::confirmMessage(const QString& title, const QString& message)
{
  SystemLock lock(pauseAndLockSystem());

  return (QMessageBox::question(this, title, message) == QMessageBox::Yes);
}

void MainWindow::onStatusMessage(const QString& message)
{
  m_ui.statusBar->showMessage(message);
}

void MainWindow::registerForDeviceNotifications()
{
#ifdef _WIN32
  // We use these notifications to detect when a controller is connected or disconnected.
  DEV_BROADCAST_DEVICEINTERFACE_W filter = {
    sizeof(DEV_BROADCAST_DEVICEINTERFACE_W), DBT_DEVTYP_DEVICEINTERFACE, 0u, {}, {}};
  m_device_notification_handle = RegisterDeviceNotificationW(
    (HANDLE)winId(), &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
#endif
}

void MainWindow::unregisterForDeviceNotifications()
{
#ifdef _WIN32
  if (!m_device_notification_handle)
    return;

  UnregisterDeviceNotification(static_cast<HDEVNOTIFY>(m_device_notification_handle));
  m_device_notification_handle = nullptr;
#endif
}

#ifdef _WIN32

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
  static constexpr const char win_type[] = "windows_generic_MSG";
  if (eventType == QByteArray(win_type, sizeof(win_type) - 1))
  {
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_SIZING && isBezelAspectRatioLockActive() && isRenderingToMain() && !isFullScreen() &&
        !isMaximized())
    {
      RECT current_outer_rect;
      if (GetWindowRect(msg->hwnd, &current_outer_rect))
      {
        const qreal dpr = m_display_widget->devicePixelRatioF();
        const LONG display_width =
          std::max<LONG>(static_cast<LONG>(std::lround(static_cast<double>(m_display_widget->width()) * dpr)), 1);
        const LONG display_height =
          std::max<LONG>(static_cast<LONG>(std::lround(static_cast<double>(m_display_widget->height()) * dpr)), 1);
        const LONG extra_width =
          std::max<LONG>((current_outer_rect.right - current_outer_rect.left) - display_width, 0);
        const LONG extra_height =
          std::max<LONG>((current_outer_rect.bottom - current_outer_rect.top) - display_height, 0);

        ConstrainWindowSizingRectTo16By9(reinterpret_cast<RECT*>(msg->lParam), msg->wParam, extra_width, extra_height);
        *result = TRUE;
        return true;
      }
    }

    if (msg->message == WM_DEVICECHANGE && msg->wParam == DBT_DEVNODES_CHANGED)
    {
      g_emu_thread->reloadInputDevices();
      *result = 1;
      return true;
    }
  }

  return QMainWindow::nativeEvent(eventType, message, result);
}

#endif

std::optional<WindowInfo> MainWindow::acquireRenderWindow(bool recreate_window, bool fullscreen, bool render_to_main,
                                                          bool surfaceless, bool use_main_window_pos)
{
  DEV_LOG("acquireRenderWindow() recreate={} fullscreen={} render_to_main={} surfaceless={} use_main_window_pos={}",
          recreate_window ? "true" : "false", fullscreen ? "true" : "false", render_to_main ? "true" : "false",
          surfaceless ? "true" : "false", use_main_window_pos ? "true" : "false");

  QWidget* container =
    m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
  const bool is_fullscreen = isRenderingFullscreen();
  const bool is_rendering_to_main = isRenderingToMain();
  const bool changing_surfaceless = (!m_display_widget != surfaceless);
  if (m_display_created && !recreate_window && fullscreen == is_fullscreen && is_rendering_to_main == render_to_main &&
      !changing_surfaceless)
  {
    return m_display_widget ? m_display_widget->getWindowInfo() : WindowInfo();
  }

  // Skip recreating the surface if we're just transitioning between fullscreen and windowed with render-to-main off.
  // .. except on Wayland, where everything tends to break if you don't recreate.
  const bool has_container = (m_display_container != nullptr);
  const bool needs_container = DisplayContainer::isNeeded(fullscreen, render_to_main);
  if (m_display_created && !recreate_window && !is_rendering_to_main && !render_to_main &&
      has_container == needs_container && !needs_container && !changing_surfaceless)
  {
    DEV_LOG("Toggling to {} without recreating surface", (fullscreen ? "fullscreen" : "windowed"));

    // since we don't destroy the display widget, we need to save it here
    if (!is_fullscreen && !is_rendering_to_main)
      saveDisplayWindowGeometryToConfig();

    if (fullscreen)
    {
      container->showFullScreen();
    }
    else
    {
      if (use_main_window_pos)
        container->setGeometry(geometry());
      else
        restoreDisplayWindowGeometryFromConfig();

      container->showNormal();
    }

    updateDisplayWidgetCursor();
    m_display_widget->setFocus();
    updateWindowState();
    if (!fullscreen && m_bezel_aspect_ratio_lock_enabled)
      QTimer::singleShot(0, this, [this]() { applyBezelAspectRatioToCurrentWindow(); });

    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    return m_display_widget->getWindowInfo();
  }

  destroyDisplayWidget(surfaceless);
  m_display_created = true;

  // if we're going to surfaceless, we're done here
  if (surfaceless)
    return WindowInfo();

  createDisplayWidget(fullscreen, render_to_main, use_main_window_pos);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to get window info from widget"));
    destroyDisplayWidget(true);
    return std::nullopt;
  }

  g_emu_thread->connectDisplaySignals(m_display_widget);

  updateWindowTitle();
  updateWindowState();

  updateDisplayWidgetCursor();
  updateDisplayRelatedActions(true, render_to_main, fullscreen);
  QtUtils::ShowOrRaiseWindow(QtUtils::GetRootWidget(m_display_widget));
  m_display_widget->setFocus();

  return wi;
}

void MainWindow::createDisplayWidget(bool fullscreen, bool render_to_main, bool use_main_window_pos)
{
  // If we're rendering to main and were hidden (e.g. coming back from fullscreen),
  // make sure we're visible before trying to add ourselves. Otherwise Wayland breaks.
  if (!fullscreen && render_to_main && !isVisible())
  {
    setVisible(true);
    QGuiApplication::sync();
  }

  QWidget* container;
  if (DisplayContainer::isNeeded(fullscreen, render_to_main))
  {
    m_display_container = new DisplayContainer();
    m_display_widget = new DisplayWidget(m_display_container);
    m_display_container->setDisplayWidget(m_display_widget);
    container = m_display_container;
  }
  else
  {
    m_display_widget = new DisplayWidget((!fullscreen && render_to_main) ? getContentParent() : nullptr);
    container = m_display_widget;
  }

  m_display_widget->setBezelAspectRatioLockEnabled(m_bezel_aspect_ratio_lock_enabled);

  if (fullscreen || !render_to_main)
  {
    container->setWindowTitle(windowTitle());
    container->setWindowIcon(windowIcon());
  }

  if (fullscreen)
  {
    // Don't risk doing this on Wayland, it really doesn't like window state changes,
    // and positioning has no effect anyway.
    if (!s_use_central_widget)
    {
      if (isVisible() && g_emu_thread->shouldRenderToMain())
        container->move(pos());
      else
        restoreDisplayWindowGeometryFromConfig();
    }

    container->showFullScreen();
  }
  else if (!render_to_main)
  {
    // See lameland comment above.
    if (use_main_window_pos && !s_use_central_widget)
      container->setGeometry(geometry());
    else
      restoreDisplayWindowGeometryFromConfig();
    container->showNormal();
  }
  else if (s_use_central_widget)
  {
    m_game_list_widget->setVisible(false);
    takeCentralWidget();
    m_game_list_widget->setParent(this); // takeCentralWidget() removes parent
    setCentralWidget(m_display_widget);
    m_display_widget->setFocus();
    update();
  }
  else
  {
    AssertMsg(m_ui.mainContainer->count() == 1, "Has no display widget");
    m_ui.mainContainer->addWidget(container);
    m_ui.mainContainer->setCurrentIndex(1);
  }

  updateDisplayRelatedActions(true, render_to_main, fullscreen);

  // We need the surface visible.
  QGuiApplication::sync();

  if (m_bezel_aspect_ratio_lock_enabled && !fullscreen)
    QTimer::singleShot(0, this, [this]() { applyBezelAspectRatioToCurrentWindow(); });
}

void MainWindow::displayResizeRequested(qint32 width, qint32 height)
{
  if (!m_display_widget)
    return;

  // unapply the pixel scaling factor for hidpi
  const float dpr = devicePixelRatioF();
  width = static_cast<qint32>(std::max(static_cast<int>(std::lroundf(static_cast<float>(width) / dpr)), 1));
  height = static_cast<qint32>(std::max(static_cast<int>(std::lroundf(static_cast<float>(height) / dpr)), 1));

  if (m_bezel_aspect_ratio_lock_enabled)
    width = std::max<qint32>(static_cast<qint32>(std::lround(static_cast<double>(height) * 16.0 / 9.0)), 1);

  if (m_display_container || !m_display_widget->parent())
  {
    // no parent - rendering to separate window. easy.
    QtUtils::ResizePotentiallyFixedSizeWindow(getDisplayContainer(), width, height);
    return;
  }

  // we are rendering to the main window. we have to add in the extra height from the toolbar/status bar.
  const s32 extra_height = this->height() - m_display_widget->height();
  QtUtils::ResizePotentiallyFixedSizeWindow(this, width, height + extra_height);
}

void MainWindow::releaseRenderWindow()
{
  // Now we can safely destroy the display window.
  destroyDisplayWidget(true);
  m_display_created = false;

  updateDisplayRelatedActions(false, false, false);

  m_ui.actionViewSystemDisplay->setEnabled(false);
  m_ui.actionFullscreen->setEnabled(false);
}

void MainWindow::destroyDisplayWidget(bool show_game_list)
{
  if (!m_display_widget)
    return;

  if (!isRenderingFullscreen() && !isRenderingToMain())
    saveDisplayWindowGeometryToConfig();

  if (m_display_container)
    m_display_container->removeDisplayWidget();

  if (isRenderingToMain())
  {
    if (s_use_central_widget)
    {
      AssertMsg(centralWidget() == m_display_widget, "Display widget is currently central");
      takeCentralWidget();
      if (show_game_list)
      {
        m_game_list_widget->setVisible(true);
        setCentralWidget(m_game_list_widget);
        m_game_list_widget->resizeTableViewColumnsToFit();
      }
    }
    else
    {
      AssertMsg(m_ui.mainContainer->indexOf(m_display_widget) == 1, "Display widget in stack");
      m_ui.mainContainer->removeWidget(m_display_widget);
      if (show_game_list)
      {
        m_ui.mainContainer->setCurrentIndex(0);
        m_game_list_widget->resizeTableViewColumnsToFit();
      }
    }
  }

  if (m_display_widget)
  {
    m_display_widget->destroy();
    m_display_widget = nullptr;
  }

  if (m_display_container)
  {
    m_display_container->deleteLater();
    m_display_container = nullptr;
  }
}

void MainWindow::updateDisplayWidgetCursor()
{
  m_display_widget->updateRelativeMode(s_system_valid && !s_system_paused && m_relative_mouse_mode);
  m_display_widget->updateCursor(s_system_valid && !s_system_paused && shouldHideMouseCursor());
}

void MainWindow::updateDisplayRelatedActions(bool has_surface, bool render_to_main, bool fullscreen)
{
  // rendering to main, or switched to gamelist/grid
  m_ui.actionViewSystemDisplay->setEnabled((has_surface && render_to_main) || (!has_surface && g_gpu_device));
  m_ui.menuWindowSize->setEnabled(has_surface && !fullscreen);
  m_ui.actionFullscreen->setEnabled(has_surface);

  {
    QSignalBlocker blocker(m_ui.actionFullscreen);
    m_ui.actionFullscreen->setChecked(fullscreen);
  }
}

void MainWindow::focusDisplayWidget()
{
  if (!m_display_widget || centralWidget() != m_display_widget)
    return;

  m_display_widget->setFocus();
}

bool MainWindow::isBezelAspectRatioLockActive() const
{
  return m_bezel_aspect_ratio_lock_enabled && m_display_widget;
}

void MainWindow::setBezelAspectRatioLockEnabled(bool enabled)
{
  m_bezel_aspect_ratio_lock_enabled = enabled;
  if (m_display_widget)
    m_display_widget->setBezelAspectRatioLockEnabled(enabled);

  if (enabled && m_display_widget && !isRenderingFullscreen())
    QTimer::singleShot(0, this, [this]() { applyBezelAspectRatioToCurrentWindow(); });
}

void MainWindow::setBezelAspectRatioLockForGame(std::string_view game_serial, bool enabled)
{
  if (!s_system_valid || s_current_game_serial.toStdString() != game_serial)
    return;

  setBezelAspectRatioLockEnabled(enabled);
}

void MainWindow::applyBezelAspectRatioToCurrentWindow()
{
  if (!isBezelAspectRatioLockActive() || isRenderingFullscreen())
    return;

  if (isRenderingToMain())
  {
    if (isFullScreen() || isMaximized() || m_display_widget->height() <= 0)
      return;

    const int extra_width = width() - m_display_widget->width();
    const int target_display_width =
      std::max(static_cast<int>(std::lround(static_cast<double>(m_display_widget->height()) * 16.0 / 9.0)), 1);
    const int target_width = target_display_width + extra_width;
    if (std::abs(width() - target_width) > 1)
      QtUtils::ResizePotentiallyFixedSizeWindow(this, target_width, height());
  }
  else
  {
    QWidget* container = getDisplayContainer();
    if (!container || container->isFullScreen() || container->isMaximized() || container->height() <= 0)
      return;

    const int target_width =
      std::max(static_cast<int>(std::lround(static_cast<double>(container->height()) * 16.0 / 9.0)), 1);
    if (std::abs(container->width() - target_width) > 1)
      QtUtils::ResizePotentiallyFixedSizeWindow(container, target_width, container->height());
  }
}

QWidget* MainWindow::getContentParent()
{
  return s_use_central_widget ? static_cast<QWidget*>(this) : static_cast<QWidget*>(m_ui.mainContainer);
}

QWidget* MainWindow::getDisplayContainer() const
{
  return (m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget));
}

void MainWindow::onMouseModeRequested(bool relative_mode, bool hide_cursor)
{
  m_relative_mouse_mode = relative_mode;
  m_hide_mouse_cursor = hide_cursor;
  if (m_display_widget)
    updateDisplayWidgetCursor();
}

void MainWindow::onSystemStarting()
{
  s_system_valid = false;
  s_system_paused = false;

  updateEmulationActions(true, false, Achievements::IsHardcoreModeActive());
}

void MainWindow::onSystemStarted()
{
  s_system_valid = true;
  updateEmulationActions(false, true, Achievements::IsHardcoreModeActive());
  updateWindowTitle();
  updateStatusBarWidgetVisibility();
  updateDisplayWidgetCursor();
  setBezelAspectRatioLockEnabled(g_settings.display_bezel_enabled && !g_settings.display_bezel_path.empty());
}

void MainWindow::onSystemPaused()
{
  // update UI
  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(true);
  }

  s_system_paused = true;
  updateStatusBarWidgetVisibility();
  m_ui.statusBar->showMessage(tr("Paused"));
  if (m_display_widget)
    updateDisplayWidgetCursor();
}

void MainWindow::onSystemResumed()
{
  // update UI
  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(false);
  }

  s_system_paused = false;
  m_ui.statusBar->clearMessage();
  updateStatusBarWidgetVisibility();
  if (m_display_widget)
  {
    updateDisplayWidgetCursor();
    m_display_widget->setFocus();
  }
}

void MainWindow::onSystemDestroyed()
{
  // update UI
  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(false);
  }

  s_system_valid = false;
  s_system_paused = false;
  setBezelAspectRatioLockEnabled(false);

  // If we're closing or in batch mode, quit the whole application now.
  if (m_is_closing || QtHost::InBatchMode())
  {
    destroySubWindows();
    quit();
    return;
  }

  updateEmulationActions(false, false, Achievements::IsHardcoreModeActive());
  if (m_display_widget)
    updateDisplayWidgetCursor();
  else
    switchToGameListView();

  // reload played time
  if (m_game_list_widget->isShowingGameList())
    m_game_list_widget->refresh(false);
}

void MainWindow::onRunningGameChanged(const QString& filename, const QString& game_serial, const QString& game_title)
{
  s_current_game_path = filename;
  s_current_game_title = game_title;
  s_current_game_serial = game_serial;
  s_current_game_icon = m_game_list_widget->getModel()->getIconForGame(filename);

  updateWindowTitle();
  if (s_system_valid)
    setBezelAspectRatioLockEnabled(g_settings.display_bezel_enabled && !g_settings.display_bezel_path.empty());
}

void MainWindow::onMediaCaptureStarted()
{
  QSignalBlocker sb(m_ui.actionMediaCapture);
  m_ui.actionMediaCapture->setChecked(true);
}

void MainWindow::onMediaCaptureStopped()
{
  QSignalBlocker sb(m_ui.actionMediaCapture);
  m_ui.actionMediaCapture->setChecked(false);
}

void MainWindow::onApplicationStateChanged(Qt::ApplicationState state)
{
  if (!s_system_valid)
    return;

  const bool focus_loss = (state != Qt::ApplicationActive);
  if (focus_loss)
  {
    if (g_settings.pause_on_focus_loss && !m_was_paused_by_focus_loss && !s_system_paused)
    {
      g_emu_thread->setSystemPaused(true);
      m_was_paused_by_focus_loss = true;
    }

    // Clear the state of all keyboard binds.
    // That way, if we had a key held down, and lost focus, the bind won't be stuck enabled because we never
    // got the key release message, because it happened in another window which "stole" the event.
    g_emu_thread->clearInputBindStateFromSource(InputManager::MakeHostKeyboardKey(0));
  }
  else
  {
    if (m_was_paused_by_focus_loss)
    {
      if (s_system_paused)
        g_emu_thread->setSystemPaused(false);
      m_was_paused_by_focus_loss = false;
    }
  }
}

void MainWindow::onStartFileActionTriggered()
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Arcade Set"), QString(), tr(ARCADE_SET_FILTER), nullptr));
  if (filename.isEmpty())
    return;

  startArcadeSet(filename);
}

void MainWindow::quit()
{
  // Make sure VM is gone. It really should be if we're here.
  if (s_system_valid)
  {
    g_emu_thread->shutdownSystem(false);
    while (s_system_valid)
      QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
  }

  // Big picture might still be active.
  if (m_display_created)
    g_emu_thread->stopFullscreenUI();

  // Ensure subwindows are removed before quitting. That way the log window cancelling
  // the close event won't cancel the quit process.
  destroySubWindows();
  QGuiApplication::quit();
}

void MainWindow::recreate()
{
  std::optional<QPoint> settings_window_pos;
  int settings_window_row = 0;
  std::optional<QPoint> controller_settings_window_pos;
  ControllerSettingsWindow::Category controller_settings_window_row =
    ControllerSettingsWindow::Category::GlobalSettings;
  if (m_settings_window && m_settings_window->isVisible())
  {
    settings_window_pos = m_settings_window->pos();
    settings_window_row = m_settings_window->getCategoryRow();
  }
  if (m_controller_settings_window && m_controller_settings_window->isVisible())
  {
    controller_settings_window_pos = m_controller_settings_window->pos();
    controller_settings_window_row = m_controller_settings_window->getCurrentCategory();
  }

  // Remove subwindows before switching to surfaceless, because otherwise e.g. the debugger can cause funkyness.
  destroySubWindows();

  const bool was_display_created = m_display_created;
  if (was_display_created)
  {
    g_emu_thread->setSurfaceless(true);
    while (m_display_widget || !g_emu_thread->isSurfaceless())
      QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);

    m_display_created = false;
  }

  // We need to close input sources, because e.g. DInput uses our window handle.
  g_emu_thread->closeInputSources();

  close();
  g_main_window = nullptr;

  MainWindow* new_main_window = new MainWindow();
  DebugAssert(g_main_window == new_main_window);
  new_main_window->show();
  deleteLater();

  // Recreate log window as well. Then make sure we're still on top.
  LogWindow::updateSettings();
  new_main_window->raise();
  new_main_window->activateWindow();

  // Reload the sources we just closed.
  g_emu_thread->reloadInputSources();

  if (was_display_created)
  {
    g_emu_thread->setSurfaceless(false);
    g_main_window->updateEmulationActions(false, System::IsValid(), Achievements::IsHardcoreModeActive());
    g_main_window->onFullscreenUIStateChange(g_emu_thread->isRunningFullscreenUI());
  }

  if (settings_window_pos.has_value())
  {
    SettingsWindow* dlg = g_main_window->getSettingsWindow();
    dlg->move(settings_window_pos.value());
    dlg->setCategoryRow(settings_window_row);
    QtUtils::ShowOrRaiseWindow(dlg);
  }
  if (controller_settings_window_pos.has_value())
  {
    ControllerSettingsWindow* dlg = g_main_window->getControllerSettingsWindow();
    dlg->move(controller_settings_window_pos.value());
    dlg->setCategory(controller_settings_window_row);
    QtUtils::ShowOrRaiseWindow(dlg);
  }
}

void MainWindow::destroySubWindows()
{
  QtUtils::CloseAndDeleteWindow(m_memory_scanner_window);
  QtUtils::CloseAndDeleteWindow(m_debugger_window);
  QtUtils::CloseAndDeleteWindow(m_cheat_manager_window);
  QtUtils::CloseAndDeleteWindow(m_controller_settings_window);
  QtUtils::CloseAndDeleteWindow(m_settings_window);

  SettingsWindow::closeGamePropertiesDialogs();

  LogWindow::destroy();
}

void MainWindow::populateGameListContextMenu(const GameList::Entry* entry, QWidget* parent_window, QMenu* menu)
{
  const bool supports_save_states = ArcadeGameSupportsSaveStates(GetArcadeGameForEntry(entry));
  if (!supports_save_states)
  {
    QAction* resume_action = menu->addAction(tr("Resume"));
    connect(resume_action, &QAction::triggered,
            [parent_window]() { ShowSaveStateUnsupportedMessage(parent_window); });

    QAction* load_state_action = menu->addAction(tr("Load State"));
    connect(load_state_action, &QAction::triggered,
            [parent_window]() { ShowSaveStateUnsupportedMessage(parent_window); });

    QAction* delete_save_states_action = menu->addAction(tr("Delete Save States..."));
    delete_save_states_action->setEnabled(false);
    return;
  }

  QAction* resume_action = nullptr;
  QMenu* load_state_menu = nullptr;

  {
    resume_action = menu->addAction(tr("Resume"));
    resume_action->setEnabled(false);

    load_state_menu = menu->addMenu(tr("Load State"));
    load_state_menu->setEnabled(false);

    if (!entry->serial.empty())
    {
      std::vector<SaveStateInfo> available_states(System::GetAvailableSaveStates(entry->serial.c_str()));
      const QString timestamp_format = QLocale::system().dateTimeFormat(QLocale::ShortFormat);
      const bool challenge_mode = Achievements::IsHardcoreModeActive();
      for (SaveStateInfo& ssi : available_states)
      {
        if (ssi.global)
          continue;

        const s32 slot = ssi.slot;
        const QDateTime timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ssi.timestamp)));
        const QString timestamp_str(timestamp.toString(timestamp_format));

        QAction* action;
        if (slot < 0)
        {
          resume_action->setText(tr("Resume (%1)").arg(timestamp_str));
          resume_action->setEnabled(!challenge_mode);
          action = resume_action;
        }
        else
        {
          load_state_menu->setEnabled(true);
          action = load_state_menu->addAction(tr("Game Save %1 (%2)").arg(slot).arg(timestamp_str));
        }

        action->setDisabled(challenge_mode);
        connect(action, &QAction::triggered, [this, entry, parent_window, supports_save_states,
                                              path = std::move(ssi.path)]() mutable {
          if (!supports_save_states)
            ShowSaveStateUnsupportedMessage(parent_window);
          else
            startFile(entry->path, std::move(path));
        });
      }
    }
  }

  {
    const bool has_any_states = resume_action->isEnabled() || load_state_menu->isEnabled();
    QAction* delete_save_states_action = menu->addAction(tr("Delete Save States..."));
    delete_save_states_action->setEnabled(has_any_states);
    if (has_any_states)
    {
      connect(delete_save_states_action, &QAction::triggered, [parent_window, entry] {
        if (QMessageBox::warning(
              parent_window, tr("Confirm Save State Deletion"),
              tr("Are you sure you want to delete all save states for %1?\n\nThe saves will not be recoverable.")
                .arg(QString::fromStdString(entry->serial)),
              QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
        {
          return;
        }

        System::DeleteSaveStates(entry->serial.c_str(), true);
      });
    }
  }
}

static QString FormatTimestampForSaveStateMenu(u64 timestamp)
{
  const QDateTime qtime(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp)));
  return qtime.toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat));
}

void MainWindow::populateLoadStateMenu(const char* game_serial, QMenu* menu)
{
  auto add_slot = [this, game_serial, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(global ? nullptr : game_serial, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

    QAction* load_action = menu->addAction(menu_title);
    load_action->setEnabled(ssi.has_value());
    if (ssi.has_value())
    {
      const QString path(QString::fromStdString(ssi->path));
      connect(load_action, &QAction::triggered, this, [path]() { g_emu_thread->loadState(path); });
    }
  };

  menu->clear();

  connect(menu->addAction(tr("Load From File...")), &QAction::triggered, []() {
    if (!CheckSaveStateAvailability(g_main_window))
      return;

    const QString path = QDir::toNativeSeparators(
      QFileDialog::getOpenFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_emu_thread->loadState(path);
  });
  QAction* load_from_state = menu->addAction(tr("Undo Load State"));
  load_from_state->setEnabled(System::CanUndoLoadState());
  connect(load_from_state, &QAction::triggered, g_emu_thread, &EmuThread::undoLoadState);
  menu->addSeparator();

  if (game_serial && std::strlen(game_serial) > 0)
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void MainWindow::populateSaveStateMenu(const char* game_serial, QMenu* menu)
{
  auto add_slot = [game_serial, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(global ? nullptr : game_serial, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

    QAction* save_action = menu->addAction(menu_title);
    connect(save_action, &QAction::triggered, [global, slot]() { g_emu_thread->saveState(global, slot); });
  };

  menu->clear();

  connect(menu->addAction(tr("Save To File...")), &QAction::triggered, []() {
    if (!System::IsValid() || !CheckSaveStateAvailability(g_main_window))
      return;

    const QString path = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_emu_thread->saveState(QDir::toNativeSeparators(path));
  });
  menu->addSeparator();

  if (game_serial && std::strlen(game_serial) > 0)
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void MainWindow::updateCheatActionsVisibility()
{
  // ArcadeDuck cheat support is intentionally gated until MAME cheat-package compatibility is implemented.
  m_ui.actionCheats->setVisible(true);
  m_ui.actionCheatsToolbar->setVisible(true);
  m_ui.menuCheats->menuAction()->setVisible(false);
}
void MainWindow::onCheatsActionTriggered()
{
  QMessageBox::information(
    this, tr("Cheats Under Investigation"),
    tr("ArcadeDuck cheat support is still under investigation and is not implemented yet.\n\n"
       "MAME cheat package compatibility is planned for a future update."));
}

void MainWindow::onCheatsMenuAboutToShow()
{
  m_ui.menuCheats->clear();
  connect(m_ui.menuCheats->addAction(tr("Cheat Manager")), &QAction::triggered, this,
          &MainWindow::onCheatsActionTriggered);
  m_ui.menuCheats->addSeparator();
  populateCheatsMenu(m_ui.menuCheats);
}

void MainWindow::populateCheatsMenu(QMenu* menu)
{
  QAction* placeholder_action = menu->addAction(tr("Cheats Under Investigation"));
  connect(placeholder_action, &QAction::triggered, this, &MainWindow::onCheatsActionTriggered);
}

std::shared_ptr<SystemBootParameters> MainWindow::getSystemBootParameters(std::string file)
{
  std::shared_ptr<SystemBootParameters> ret = std::make_shared<SystemBootParameters>(std::move(file));
  ret->boot_intent = SystemBootIntent::PublicArcade;
  ret->start_media_capture = m_ui.actionMediaCapture->isChecked();
  return ret;
}

std::optional<bool> MainWindow::promptForResumeState(const std::string& save_state_path)
{
  FILESYSTEM_STAT_DATA sd;
  if (save_state_path.empty() || !FileSystem::StatFile(save_state_path.c_str(), &sd))
    return false;

  QMessageBox msgbox(this);
  msgbox.setIcon(QMessageBox::Question);
  msgbox.setWindowTitle(tr("Load Resume State"));
  msgbox.setWindowModality(Qt::WindowModal);
  msgbox.setText(tr("A resume save state was found for this game, saved at:\n\n%1.\n\nDo you want to load this state, "
                    "or start from a fresh boot?")
                   .arg(QDateTime::fromSecsSinceEpoch(sd.ModificationTime, Qt::UTC).toLocalTime().toString()));

  QPushButton* load = msgbox.addButton(tr("Load State"), QMessageBox::AcceptRole);
  QPushButton* boot = msgbox.addButton(tr("Fresh Boot"), QMessageBox::RejectRole);
  QPushButton* delboot = msgbox.addButton(tr("Delete And Boot"), QMessageBox::RejectRole);
  msgbox.addButton(QMessageBox::Cancel);
  msgbox.setDefaultButton(load);
  msgbox.exec();

  QAbstractButton* clicked = msgbox.clickedButton();
  if (load == clicked)
  {
    return true;
  }
  else if (boot == clicked)
  {
    return false;
  }
  else if (delboot == clicked)
  {
    if (!FileSystem::DeleteFile(save_state_path.c_str()))
    {
      QMessageBox::critical(this, tr("Error"),
                            tr("Failed to delete save state file '%1'.").arg(QString::fromStdString(save_state_path)));
    }

    return false;
  }

  return std::nullopt;
}

void MainWindow::startFile(std::string path, std::optional<std::string> save_path)
{
  std::shared_ptr<SystemBootParameters> params = getSystemBootParameters(std::move(path));
  if (save_path.has_value())
    params->save_state = std::move(save_path.value());

  g_emu_thread->bootSystem(std::move(params));
}

void MainWindow::startArcadeSet(const QString& path)
{
  const std::string supplied_path(Path::Canonicalize(Path::ToNativePath(path.toStdString())));
  if (!FileSystem::FileExists(supplied_path.c_str()))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Arcade launch path '%1' does not exist.").arg(QString::fromStdString(supplied_path)));
    return;
  }
  if (!Arcade::Database::IsArchivePath(supplied_path))
  {
    QMessageBox::critical(
      this, tr("Error"),
      tr("File '%1' is not a supported ArcadeDuck arcade archive.").arg(QString::fromStdString(supplied_path)));
    return;
  }

  std::string path_str(Path::RealPath(supplied_path));
  if (path_str.empty())
    path_str = supplied_path;

  const Arcade::Database::GameDefinition* const game = Arcade::Database::IdentifyArchive(path_str);
  if (!game)
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Archive '%1' is not a recognized ArcadeDuck set.")
                            .arg(QtUtils::StringViewToQString(Path::GetFileName(path_str))));
    return;
  }

  std::optional<std::string> save_path;
  if (!game->id.empty() && ArcadeGameSupportsSaveStates(game))
  {
    std::string resume_path(System::GetGameSaveStateFileName(game->id.c_str(), -1));
    std::optional<bool> resume = promptForResumeState(resume_path);
    if (!resume.has_value())
    {
      // cancelled
      return;
    }
    else if (resume.value())
      save_path = std::move(resume_path);
  }

  // only resume if the option is enabled, and we have one for this game
  startFile(std::move(path_str), std::move(save_path));
}

void MainWindow::onLoadStateMenuAboutToShow()
{
  populateLoadStateMenu(s_current_game_serial.toUtf8().constData(), m_ui.menuLoadState);
}

void MainWindow::onSaveStateMenuAboutToShow()
{
  populateSaveStateMenu(s_current_game_serial.toUtf8().constData(), m_ui.menuSaveState);
}

void MainWindow::onStartFullscreenUITriggered()
{
  if (m_display_widget)
    g_emu_thread->stopFullscreenUI();
  else
    g_emu_thread->startFullscreenUI();
}

void MainWindow::onFullscreenUIStateChange(bool running)
{
  m_ui.actionStartFullscreenUI->setText(running ? tr("Stop Big Picture Mode") : tr("Start Big Picture Mode"));
  m_ui.actionStartFullscreenUI2->setText(running ? tr("Exit Big Picture") : tr("Big Picture"));
}

void MainWindow::onViewToolbarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ShowToolbar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.toolBar->setVisible(checked);
  if (isBezelAspectRatioLockActive() && isRenderingToMain())
    QTimer::singleShot(0, this, [this]() { applyBezelAspectRatioToCurrentWindow(); });
}

void MainWindow::onViewLockToolbarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "LockToolbar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.toolBar->setMovable(!checked);
}

void MainWindow::onViewStatusBarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ShowStatusBar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.statusBar->setVisible(checked);
  if (isBezelAspectRatioLockActive() && isRenderingToMain())
    QTimer::singleShot(0, this, [this]() { applyBezelAspectRatioToCurrentWindow(); });
}

void MainWindow::onViewGameListActionTriggered()
{
  switchToGameListView();
  m_game_list_widget->showGameList();
}

void MainWindow::onViewGameGridActionTriggered()
{
  switchToGameListView();
  m_game_list_widget->showGameGrid();
}

void MainWindow::onViewSystemDisplayTriggered()
{
  if (m_display_created)
    switchToEmulationView();
}

void MainWindow::onViewGamePropertiesActionTriggered()
{
  if (!s_system_valid)
    return;

  Host::RunOnCPUThread([]() {
    const std::string& path = System::GetDiscPath();
    const std::string& serial = System::GetGameSerial();
    if (path.empty() || serial.empty())
      return;

    QtHost::RunOnUIThread([path = path, serial = serial]() {
      SettingsWindow::openGamePropertiesDialog(path, serial);
    });
  });
}

void MainWindow::onGitHubRepositoryActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/StillJC/ArcadeDuck");
}

void MainWindow::onIssueTrackerActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/StillJC/ArcadeDuck/issues");
}

void MainWindow::onDiscordServerActionTriggered()
{
  QtUtils::OpenURL(this, "https://discord.gg/fQ9HvgKCg");
}

void MainWindow::onAboutActionTriggered()
{
  AboutDialog about(this);
  about.exec();
}

void MainWindow::onGameListRefreshProgress(const QString& status, int current, int total)
{
  m_ui.statusBar->showMessage(status);
  setProgressBar(current, total);
}

void MainWindow::onGameListRefreshComplete()
{
  m_ui.statusBar->clearMessage();
  clearProgressBar();
}

void MainWindow::onGameListSelectionChanged()
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = m_game_list_widget->getSelectedEntry();
  if (!entry)
    return;

  m_ui.statusBar->showMessage(QString::fromStdString(entry->path));
}

void MainWindow::onGameListEntryActivated()
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = m_game_list_widget->getSelectedEntry();
  if (!entry || s_system_valid)
    return;

  std::optional<std::string> save_path;
  if (!entry->serial.empty() && ArcadeGameSupportsSaveStates(GetArcadeGameForEntry(entry)))
  {
    std::string resume_path(System::GetGameSaveStateFileName(entry->serial.c_str(), -1));
    std::optional<bool> resume = promptForResumeState(resume_path);
    if (!resume.has_value())
    {
      // cancelled
      return;
    }
    else if (resume.value())
      save_path = std::move(resume_path);
  }

  // only resume if the option is enabled, and we have one for this game
  startFile(entry->path, std::move(save_path));
}

void MainWindow::onGameListEntryContextMenuRequested(const QPoint& point)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = m_game_list_widget->getSelectedEntry();

  QMenu menu;

  // Hopefully this pointer doesn't disappear... it shouldn't.
  if (entry)
  {
    connect(menu.addAction(tr("Properties...")), &QAction::triggered, [entry]() {
      SettingsWindow::openGamePropertiesDialog(entry->path, entry->serial);
    });

    connect(menu.addAction(tr("Open Containing Directory...")), &QAction::triggered, [this, entry]() {
      const QFileInfo fi(QString::fromStdString(entry->path));
      QtUtils::OpenURL(this, QUrl::fromLocalFile(fi.absolutePath()));
    });

    connect(menu.addAction(tr("Set Cover Image...")), &QAction::triggered,
            [this, entry]() { setGameListEntryCoverImage(entry); });
    connect(menu.addAction(tr("Set Bezel Image...")), &QAction::triggered,
            [this, entry]() { setGameListEntryBezelImage(entry); });
    connect(menu.addAction(tr("Clear Bezel Image")), &QAction::triggered,
            [this, entry]() { clearGameListEntryBezelImage(entry); });

    menu.addSeparator();

    if (!s_system_valid)
    {
      populateGameListContextMenu(entry, this, &menu);
      menu.addSeparator();

      connect(menu.addAction(tr("Start")), &QAction::triggered,
              [this, entry]() { g_emu_thread->bootSystem(getSystemBootParameters(entry->path)); });

      if (m_ui.menuDebug->menuAction()->isVisible() && !Achievements::IsHardcoreModeActive())
      {
        connect(menu.addAction(tr("Start and Debug")), &QAction::triggered, [this, entry]() {
          m_open_debugger_on_start = true;

          std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(entry->path);
          boot_params->override_start_paused = true;
          g_emu_thread->bootSystem(std::move(boot_params));
        });
      }
    }

    menu.addSeparator();

    connect(menu.addAction(tr("Exclude From List")), &QAction::triggered,
            [this, entry]() { getSettingsWindow()->getGameListSettingsWidget()->addExcludedPath(entry->path); });

    connect(menu.addAction(tr("Reset Play Time")), &QAction::triggered,
            [this, entry]() { clearGameListEntryPlayTime(entry); });
  }

  menu.addSeparator();

  connect(menu.addAction(tr("Add Search Directory...")), &QAction::triggered,
          [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });

  menu.exec(point);
}

void MainWindow::setGameListEntryCoverImage(const GameList::Entry* entry)
{
  const QString filename = QDir::toNativeSeparators(QFileDialog::getOpenFileName(
    this, tr("Select Cover Image"), QString(), tr("All Cover Image Types (*.jpg *.jpeg *.png *.webp)")));
  if (filename.isEmpty())
    return;

  const QString old_filename = QString::fromStdString(GameList::GetManualCoverImagePathForEntry(entry));
  const QString new_filename =
    QString::fromStdString(GameList::GetNewCoverImagePathForEntry(entry, filename.toUtf8().constData(), false));
  if (new_filename.isEmpty())
    return;

  if (!old_filename.isEmpty())
  {
    if (QFileInfo(old_filename) == QFileInfo(filename))
    {
      QMessageBox::critical(this, tr("Copy Error"), tr("You must select a different file to the current cover image."));
      return;
    }

    if (QMessageBox::question(this, tr("Cover Already Exists"),
                              tr("A cover image for this game already exists, do you wish to replace it?"),
                              QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
    {
      return;
    }
  }

  if (QFile::exists(new_filename) && !QFile::remove(new_filename))
  {
    QMessageBox::critical(this, tr("Copy Error"), tr("Failed to remove existing cover '%1'").arg(new_filename));
    return;
  }
  if (!QFile::copy(filename, new_filename))
  {
    QMessageBox::critical(this, tr("Copy Error"), tr("Failed to copy '%1' to '%2'").arg(filename).arg(new_filename));
    return;
  }
  if (!old_filename.isEmpty() && old_filename != new_filename && !QFile::remove(old_filename))
  {
    QMessageBox::critical(this, tr("Copy Error"), tr("Failed to remove '%1'").arg(old_filename));
    return;
  }
  m_game_list_widget->refreshGridCovers();
  FullscreenUI::InvalidateCoverCache();
}

std::optional<QString> MainWindow::importGameBezelImage(QWidget* parent, std::string_view game_serial,
                                                        const QString& source_filename)
{
  if (game_serial.empty() || source_filename.isEmpty())
    return std::nullopt;

  const QFileInfo source_info(source_filename);
  if (!source_info.exists() || !source_info.isFile())
  {
    QMessageBox::critical(parent, tr("Bezel Image Error"), tr("The selected bezel image does not exist."));
    return std::nullopt;
  }

  const QString suffix = source_info.suffix().toLower();
  if (suffix != QStringLiteral("png") && suffix != QStringLiteral("webp") && suffix != QStringLiteral("jpg") &&
      suffix != QStringLiteral("jpeg") && suffix != QStringLiteral("bmp"))
  {
    QMessageBox::critical(parent, tr("Bezel Image Error"), tr("The selected file is not a supported bezel image."));
    return std::nullopt;
  }

  const QString bezel_directory = QString::fromStdString(EmuFolders::Bezels);
  if (!QDir().mkpath(bezel_directory))
  {
    QMessageBox::critical(parent, tr("Bezel Image Error"),
                          tr("Failed to create the bezel directory '%1'.").arg(QDir::toNativeSeparators(bezel_directory)));
    return std::nullopt;
  }

  const QString destination =
    QDir(bezel_directory)
      .filePath(QString::fromStdString(Path::SanitizeFileName(game_serial)) + QLatin1Char('.') + suffix);

  if (QFileInfo(source_filename) == QFileInfo(destination))
    return QDir::toNativeSeparators(destination);

  QFile input(source_filename);
  if (!input.open(QIODevice::ReadOnly))
  {
    QMessageBox::critical(parent, tr("Bezel Image Error"),
                          tr("Failed to open bezel image '%1'.").arg(QDir::toNativeSeparators(source_filename)));
    return std::nullopt;
  }

  QSaveFile output(destination);
  if (!output.open(QIODevice::WriteOnly))
  {
    QMessageBox::critical(parent, tr("Bezel Image Error"),
                          tr("Failed to create bezel image '%1'.").arg(QDir::toNativeSeparators(destination)));
    return std::nullopt;
  }

  while (!input.atEnd())
  {
    const QByteArray chunk = input.read(1024 * 1024);
    if (chunk.isEmpty() && input.error() != QFileDevice::NoError)
    {
      output.cancelWriting();
      QMessageBox::critical(parent, tr("Bezel Image Error"),
                            tr("Failed while reading bezel image '%1'.").arg(QDir::toNativeSeparators(source_filename)));
      return std::nullopt;
    }

    if (!chunk.isEmpty() && output.write(chunk) != chunk.size())
    {
      output.cancelWriting();
      QMessageBox::critical(parent, tr("Bezel Image Error"),
                            tr("Failed while copying bezel image to '%1'.").arg(QDir::toNativeSeparators(destination)));
      return std::nullopt;
    }
  }

  if (!output.commit())
  {
    QMessageBox::critical(parent, tr("Bezel Image Error"),
                          tr("Failed to save bezel image '%1'.").arg(QDir::toNativeSeparators(destination)));
    return std::nullopt;
  }

  return QDir::toNativeSeparators(destination);
}

void MainWindow::setGameListEntryBezelImage(const GameList::Entry* entry)
{
  if (!entry || entry->serial.empty())
    return;

  const QString filename = QDir::toNativeSeparators(QFileDialog::getOpenFileName(
    this, tr("Select Bezel Image"), QString(), tr("All Bezel Image Types (*.png *.webp *.jpg *.jpeg *.bmp)")));
  if (filename.isEmpty())
    return;

  const std::string settings_path = System::GetGameSettingsPath(entry->serial);
  INISettingsInterface si(settings_path);
  if (FileSystem::FileExists(settings_path.c_str()) && !si.Load())
  {
    QMessageBox::critical(this, tr("Bezel Settings Error"),
                          tr("Failed to load the existing game settings for '%1'. No settings were changed.")
                            .arg(QString::fromStdString(entry->title)));
    return;
  }

  const std::optional<QString> imported_path = importGameBezelImage(this, entry->serial, filename);
  if (!imported_path.has_value())
    return;

  const QByteArray path_utf8 = imported_path->toUtf8();
  si.SetBoolValue("Display", "BezelEnabled", true);
  si.SetStringValue("Display", "BezelPath", path_utf8.constData());

  Error error;
  if (!si.Save(&error))
  {
    QMessageBox::critical(this, tr("Bezel Settings Error"), tr("Failed to save the bezel settings file."));
    return;
  }

  if (s_system_valid && s_current_game_serial.toStdString() == entry->serial)
  {
    setBezelAspectRatioLockForGame(entry->serial, true);
    g_emu_thread->reloadGameSettings(false);
  }
}

void MainWindow::clearGameListEntryBezelImage(const GameList::Entry* entry)
{
  if (!entry || entry->serial.empty())
    return;

  const std::string settings_path = System::GetGameSettingsPath(entry->serial);
  if (!FileSystem::FileExists(settings_path.c_str()))
    return;

  INISettingsInterface si(settings_path);
  if (!si.Load())
  {
    QMessageBox::critical(this, tr("Bezel Settings Error"),
                          tr("Failed to load the existing game settings for '%1'. No settings were changed.")
                            .arg(QString::fromStdString(entry->title)));
    return;
  }

  si.DeleteValue("Display", "BezelEnabled");
  si.DeleteValue("Display", "BezelPath");
  si.RemoveEmptySections();

  Error error;
  if (!si.Save(&error))
  {
    QMessageBox::critical(this, tr("Bezel Settings Error"), tr("Failed to save the bezel settings file."));
    return;
  }

  if (s_system_valid && s_current_game_serial.toStdString() == entry->serial)
  {
    setBezelAspectRatioLockForGame(entry->serial, false);
    g_emu_thread->reloadGameSettings(false);
  }
}

void MainWindow::clearGameListEntryPlayTime(const GameList::Entry* entry)
{
  if (QMessageBox::question(
        this, tr("Confirm Reset"),
        tr("Are you sure you want to reset the play time for '%1'?\n\nThis action cannot be undone.")
          .arg(QString::fromStdString(entry->title))) != QMessageBox::Yes)
  {
    return;
  }

  GameList::ClearPlayedTimeForSerial(entry->serial);
  m_game_list_widget->refresh(false);
}

void MainWindow::setupAdditionalUi()
{
  const bool status_bar_visible = Host::GetBaseBoolSettingValue("UI", "ShowStatusBar", true);
  m_ui.actionViewStatusBar->setChecked(status_bar_visible);
  m_ui.statusBar->setVisible(status_bar_visible);

  const bool toolbar_visible = Host::GetBaseBoolSettingValue("UI", "ShowToolbar", false);
  m_ui.actionViewToolbar->setChecked(toolbar_visible);
  m_ui.toolBar->setVisible(toolbar_visible);

  const bool toolbars_locked = Host::GetBaseBoolSettingValue("UI", "LockToolbar", false);
  m_ui.actionViewLockToolbar->setChecked(toolbars_locked);
  m_ui.toolBar->setMovable(!toolbars_locked);
  m_ui.toolBar->setContextMenuPolicy(Qt::PreventContextMenu);

  m_game_list_widget = new GameListWidget(getContentParent());
  m_game_list_widget->initialize();
  m_ui.actionGridViewShowTitles->setChecked(m_game_list_widget->isShowingGridCoverTitles());
  m_ui.actionShowGameIcons->setChecked(m_game_list_widget->isShowingGameIcons());
  if (s_use_central_widget)
  {
    m_ui.mainContainer = nullptr; // setCentralWidget() will delete this
    setCentralWidget(m_game_list_widget);
  }
  else
  {
    m_ui.mainContainer->addWidget(m_game_list_widget);
  }

  m_status_progress_widget = new QProgressBar(m_ui.statusBar);
  m_status_progress_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_progress_widget->setFixedSize(140, 16);
  m_status_progress_widget->setMinimum(0);
  m_status_progress_widget->setMaximum(100);
  m_status_progress_widget->hide();

  m_status_renderer_widget = new QLabel(m_ui.statusBar);
  m_status_renderer_widget->setFixedHeight(16);
  m_status_renderer_widget->setFixedSize(65, 16);
  m_status_renderer_widget->hide();

  m_status_resolution_widget = new QLabel(m_ui.statusBar);
  m_status_resolution_widget->setFixedHeight(16);
  m_status_resolution_widget->setFixedSize(70, 16);
  m_status_resolution_widget->hide();

  m_status_fps_widget = new QLabel(m_ui.statusBar);
  m_status_fps_widget->setFixedSize(85, 16);
  m_status_fps_widget->hide();

  m_status_vps_widget = new QLabel(m_ui.statusBar);
  m_status_vps_widget->setFixedSize(125, 16);
  m_status_vps_widget->hide();

  m_settings_toolbar_menu = new QMenu(m_ui.toolBar);
  m_settings_toolbar_menu->addAction(m_ui.actionSettings);
  m_settings_toolbar_menu->addAction(m_ui.actionViewGameProperties);

  m_ui.actionGridViewShowTitles->setChecked(m_game_list_widget->isShowingGridCoverTitles());

  updateDebugMenuVisibility();
  updateCheatActionsVisibility();

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
  {
    const CPUExecutionMode mode = static_cast<CPUExecutionMode>(i);
    QAction* action =
      m_ui.menuCPUExecutionMode->addAction(QString::fromUtf8(Settings::GetCPUExecutionModeDisplayName(mode)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, mode]() {
      Host::SetBaseStringSettingValue("CPU", "ExecutionMode", Settings::GetCPUExecutionModeName(mode));
      Host::CommitBaseSettingChanges();
      g_emu_thread->applySettings();
      updateDebugMenuCPUExecutionMode();
    });
  }
  updateDebugMenuCPUExecutionMode();

  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    const GPURenderer renderer = static_cast<GPURenderer>(i);
    QAction* action = m_ui.menuRenderer->addAction(QString::fromUtf8(Settings::GetRendererDisplayName(renderer)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, renderer]() {
      Host::SetBaseStringSettingValue("GPU", "Renderer", Settings::GetRendererName(renderer));
      Host::CommitBaseSettingChanges();
      g_emu_thread->applySettings();
      updateDebugMenuGPURenderer();
    });
  }
  updateDebugMenuGPURenderer();

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    const DisplayCropMode crop_mode = static_cast<DisplayCropMode>(i);
    QAction* action =
      m_ui.menuCropMode->addAction(QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(crop_mode)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, crop_mode]() {
      Host::SetBaseStringSettingValue("Display", "CropMode", Settings::GetDisplayCropModeName(crop_mode));
      Host::CommitBaseSettingChanges();
      g_emu_thread->applySettings();
      updateDebugMenuCropMode();
    });
  }
  updateDebugMenuCropMode();

  for (u32 scale = 1; scale <= 10; scale++)
  {
    QAction* action = m_ui.menuWindowSize->addAction(tr("%1x Scale").arg(scale));
    connect(action, &QAction::triggered, [scale]() { g_emu_thread->requestDisplaySize(scale); });
  }

}

void MainWindow::updateEmulationActions(bool starting, bool running, bool cheevos_challenge_mode)
{
  m_ui.actionStartFile->setDisabled(starting || running);
  m_ui.actionResumeLastState->setDisabled(starting || running || cheevos_challenge_mode);
  m_ui.actionStartFullscreenUI->setDisabled(starting || running);
  m_ui.actionStartFullscreenUI2->setDisabled(starting || running);

  m_ui.actionPowerOff->setDisabled(starting || !running);
  m_ui.actionPowerOffWithoutSaving->setDisabled(starting || !running);
  m_ui.actionReset->setDisabled(starting || !running);
  m_ui.actionPause->setDisabled(starting || !running);
  m_ui.actionCheats->setDisabled(cheevos_challenge_mode);
  m_ui.actionCheatsToolbar->setDisabled(cheevos_challenge_mode);
  m_ui.actionScreenshot->setDisabled(starting || !running);
  m_ui.menuCheats->setDisabled(cheevos_challenge_mode);
  m_ui.actionCPUDebugger->setDisabled(cheevos_challenge_mode);
  m_ui.actionMemoryScanner->setDisabled(cheevos_challenge_mode);
  m_ui.actionDumpRAM->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionDumpVRAM->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionDumpSPURAM->setDisabled(starting || !running || cheevos_challenge_mode);

  m_ui.actionSaveState->setDisabled(starting || !running);
  m_ui.menuSaveState->setDisabled(starting || !running);
  m_ui.menuWindowSize->setDisabled(starting || !running);

  m_ui.actionViewGameProperties->setDisabled(starting || !running);

  if (starting || running)
  {
    if (!m_ui.toolBar->actions().contains(m_ui.actionPowerOff))
    {
      m_ui.toolBar->insertAction(m_ui.actionResumeLastState, m_ui.actionPowerOff);
      m_ui.toolBar->removeAction(m_ui.actionResumeLastState);
    }
  }
  else
  {
    if (!m_ui.toolBar->actions().contains(m_ui.actionResumeLastState))
    {
      m_ui.toolBar->insertAction(m_ui.actionPowerOff, m_ui.actionResumeLastState);
      m_ui.toolBar->removeAction(m_ui.actionPowerOff);
    }

    m_ui.actionViewGameProperties->setEnabled(false);
  }

  if (m_open_debugger_on_start && running)
    openCPUDebugger();
  if ((!starting && !running) || running)
    m_open_debugger_on_start = false;

  m_ui.statusBar->clearMessage();
}

void MainWindow::updateStatusBarWidgetVisibility()
{
  auto Update = [this](QWidget* widget, bool visible, int stretch) {
    if (widget->isVisible())
    {
      m_ui.statusBar->removeWidget(widget);
      widget->hide();
    }

    if (visible)
    {
      m_ui.statusBar->addPermanentWidget(widget, stretch);
      widget->show();
    }
  };

  Update(m_status_renderer_widget, s_system_valid && !s_system_paused, 0);
  Update(m_status_resolution_widget, s_system_valid && !s_system_paused, 0);
  Update(m_status_fps_widget, s_system_valid && !s_system_paused, 0);
  Update(m_status_vps_widget, s_system_valid && !s_system_paused, 0);
}

void MainWindow::updateWindowTitle()
{
  const QString suffix(QtHost::GetAppConfigSuffix());
  const QString app_title(QtHost::GetAppNameAndVersion() + suffix);
  const QString display_title =
    (s_system_valid && !s_current_game_title.isEmpty()) ?
      QStringLiteral("%1 - %2").arg(s_current_game_title, app_title) :
      app_title;

  if (windowTitle() != display_title)
    setWindowTitle(display_title);
  setWindowIcon(s_current_game_icon.isNull() ? QtHost::GetAppIcon() : s_current_game_icon);

  if (m_display_widget && !isRenderingToMain())
  {
    QWidget* container =
      m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
    if (container->windowTitle() != display_title)
      container->setWindowTitle(display_title);
    container->setWindowIcon(s_current_game_icon.isNull() ? QtHost::GetAppIcon() : s_current_game_icon);
  }

  if (g_log_window)
    g_log_window->updateWindowTitle();
}
void MainWindow::updateWindowState(bool force_visible)
{
  // Skip all of this when we're closing, since we don't want to make ourselves visible and cancel it.
  if (m_is_closing)
    return;

  const bool hide_window = !isRenderingToMain() && shouldHideMainWindow();
  const bool disable_resize = Host::GetBoolSettingValue("Main", "DisableWindowResize", false);
  const bool has_window = s_system_valid || m_display_widget;

  // Need to test both valid and display widget because of startup (vm invalid while window is created).
  const bool visible = force_visible || !hide_window || !has_window;
  if (isVisible() != visible)
    setVisible(visible);

  // No point changing realizability if we're not visible.
  const bool resizeable = force_visible || !disable_resize || !has_window;
  if (visible)
    QtUtils::SetWindowResizeable(this, resizeable);

  // Update the display widget too if rendering separately.
  if (m_display_widget && !isRenderingToMain())
    QtUtils::SetWindowResizeable(getDisplayContainer(), resizeable);
}

void MainWindow::setProgressBar(int current, int total)
{
  const int value = (total != 0) ? ((current * 100) / total) : 0;
  if (m_status_progress_widget->value() != value)
    m_status_progress_widget->setValue(value);

  if (m_status_progress_widget->isVisible())
    return;

  m_status_progress_widget->show();
  m_ui.statusBar->addPermanentWidget(m_status_progress_widget);
}

void MainWindow::clearProgressBar()
{
  if (!m_status_progress_widget->isVisible())
    return;

  m_status_progress_widget->hide();
  m_ui.statusBar->removeWidget(m_status_progress_widget);
}

bool MainWindow::isShowingGameList() const
{
  if (s_use_central_widget)
    return (centralWidget() == m_game_list_widget);
  else
    return (m_ui.mainContainer->currentIndex() == 0);
}

bool MainWindow::isRenderingFullscreen() const
{
  if (!g_gpu_device || !m_display_widget)
    return false;

  return getDisplayContainer()->isFullScreen();
}

bool MainWindow::isRenderingToMain() const
{
  if (s_use_central_widget)
    return (m_display_widget && centralWidget() == m_display_widget);
  else
    return (m_display_widget && m_ui.mainContainer->indexOf(m_display_widget) == 1);
}

bool MainWindow::shouldHideMouseCursor() const
{
  return isRenderingFullscreen() &&
         Host::GetBoolSettingValue("Main", "HideCursorInFullscreen", true);
}

bool MainWindow::shouldHideMainWindow() const
{
  return Host::GetBoolSettingValue("Main", "HideMainWindowWhenRunning", false) ||
         (g_emu_thread->shouldRenderToMain() && !isRenderingToMain()) || QtHost::InNoGUIMode();
}

void MainWindow::switchToGameListView()
{
  if (!isShowingGameList())
  {
    if (m_display_created)
    {
      m_was_paused_on_surface_loss = s_system_paused;
      if (!s_system_paused)
        g_emu_thread->setSystemPaused(true);

      // switch to surfaceless. we have to wait until the display widget is gone before we swap over.
      g_emu_thread->setSurfaceless(true);
      while (m_display_widget)
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
    }
  }

  m_game_list_widget->setFocus();
}

void MainWindow::switchToEmulationView()
{
  if (!m_display_created || !isShowingGameList())
    return;

  // we're no longer surfaceless! this will call back to UpdateDisplay(), which will swap the widget out.
  g_emu_thread->setSurfaceless(false);

  // resume if we weren't paused at switch time
  if (s_system_paused && !m_was_paused_on_surface_loss)
    g_emu_thread->setSystemPaused(false);

  if (m_display_widget)
    m_display_widget->setFocus();
}

void MainWindow::connectSignals()
{
  updateEmulationActions(false, false, Achievements::IsHardcoreModeActive());

  connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::onApplicationStateChanged);

  connect(m_ui.actionStartFile, &QAction::triggered, this, &MainWindow::onStartFileActionTriggered);
  connect(m_ui.actionResumeLastState, &QAction::triggered, this, [this]() {
    if (!CheckSaveStateAvailability(this))
      return;

    g_emu_thread->resumeSystemFromMostRecentState();
  });
  connect(m_ui.menuLoadState, &QMenu::aboutToShow, this, &MainWindow::onLoadStateMenuAboutToShow);
  connect(m_ui.menuSaveState, &QMenu::aboutToShow, this, &MainWindow::onSaveStateMenuAboutToShow);
  connect(m_ui.menuCheats, &QMenu::aboutToShow, this, &MainWindow::onCheatsMenuAboutToShow);
  connect(m_ui.actionCheats, &QAction::triggered, this, &MainWindow::onCheatsActionTriggered);
  connect(m_ui.actionCheatsToolbar, &QAction::triggered, this, &MainWindow::onCheatsActionTriggered);
  connect(m_ui.actionStartFullscreenUI, &QAction::triggered, this, &MainWindow::onStartFullscreenUITriggered);
  connect(m_ui.actionStartFullscreenUI2, &QAction::triggered, this, &MainWindow::onStartFullscreenUITriggered);
  connect(m_ui.actionAddGameDirectory, &QAction::triggered,
          [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });
  connect(m_ui.actionPowerOff, &QAction::triggered, this,
          [this]() { requestShutdown(true, true, g_settings.save_state_on_exit); });
  connect(m_ui.actionPowerOffWithoutSaving, &QAction::triggered, this,
          [this]() { requestShutdown(false, false, false); });
  connect(m_ui.actionReset, &QAction::triggered, this, []() { g_emu_thread->resetSystem(); });
  connect(m_ui.actionPause, &QAction::toggled, this, [](bool active) { g_emu_thread->setSystemPaused(active); });
  connect(m_ui.actionScreenshot, &QAction::triggered, g_emu_thread, &EmuThread::saveScreenshot);
  connect(m_ui.actionScanForNewGames, &QAction::triggered, this, [this]() { refreshGameList(false); });
  connect(m_ui.actionRescanAllGames, &QAction::triggered, this, [this]() { refreshGameList(true); });
  connect(m_ui.actionLoadState, &QAction::triggered, this, [this]() {
    if (!CheckSaveStateAvailability(this))
      return;

    m_ui.menuLoadState->exec(QCursor::pos());
  });
  connect(m_ui.actionSaveState, &QAction::triggered, this, [this]() {
    if (!CheckSaveStateAvailability(this))
      return;

    m_ui.menuSaveState->exec(QCursor::pos());
  });
  connect(m_ui.actionExit, &QAction::triggered, this, &MainWindow::close);
  connect(m_ui.actionFullscreen, &QAction::triggered, g_emu_thread, &EmuThread::toggleFullscreen);
  connect(m_ui.actionSettings, &QAction::triggered, [this]() { doSettings(); });
  connect(m_ui.actionSettings2, &QAction::triggered, this, &MainWindow::onSettingsTriggeredFromToolbar);
  connect(m_ui.actionInterfaceSettings, &QAction::triggered, [this]() { doSettings("Interface"); });
  connect(m_ui.actionOperatorSettings, &QAction::triggered, [this]() { doSettings("Operator"); });
  connect(m_ui.actionSystemLinkSettings, &QAction::triggered, [this]() { doSettings("System Link"); });
  connect(m_ui.actionConsoleSettings, &QAction::triggered, [this]() { doSettings("Machine"); });
  connect(m_ui.actionEmulationSettings, &QAction::triggered, [this]() { doSettings("Emulation"); });
  connect(m_ui.actionGameListSettings, &QAction::triggered, [this]() { doSettings("Game List"); });
  connect(m_ui.actionHotkeySettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsWindow::Category::HotkeySettings); });
  connect(m_ui.actionControllerSettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsWindow::Category::GlobalSettings); });
  connect(m_ui.actionGraphicsSettings, &QAction::triggered, [this]() { doSettings("Graphics"); });
  connect(m_ui.actionPostProcessingSettings, &QAction::triggered, [this]() { doSettings("Post-Processing"); });
  connect(m_ui.actionAudioSettings, &QAction::triggered, [this]() { doSettings("Audio"); });
  connect(m_ui.actionAchievementSettings, &QAction::triggered, [this]() { doSettings("Achievements"); });
  connect(m_ui.actionFolderSettings, &QAction::triggered, [this]() { doSettings("Folders"); });
  connect(m_ui.actionAdvancedSettings, &QAction::triggered, [this]() { doSettings("Advanced"); });
  connect(m_ui.actionViewToolbar, &QAction::toggled, this, &MainWindow::onViewToolbarActionToggled);
  connect(m_ui.actionViewLockToolbar, &QAction::toggled, this, &MainWindow::onViewLockToolbarActionToggled);
  connect(m_ui.actionViewStatusBar, &QAction::toggled, this, &MainWindow::onViewStatusBarActionToggled);
  connect(m_ui.actionViewGameList, &QAction::triggered, this, &MainWindow::onViewGameListActionTriggered);
  connect(m_ui.actionViewGameGrid, &QAction::triggered, this, &MainWindow::onViewGameGridActionTriggered);
  connect(m_ui.actionViewSystemDisplay, &QAction::triggered, this, &MainWindow::onViewSystemDisplayTriggered);
  connect(m_ui.actionViewGameProperties, &QAction::triggered, this, &MainWindow::onViewGamePropertiesActionTriggered);
  connect(m_ui.actionGitHubRepository, &QAction::triggered, this, &MainWindow::onGitHubRepositoryActionTriggered);
  connect(m_ui.actionIssueTracker, &QAction::triggered, this, &MainWindow::onIssueTrackerActionTriggered);
  connect(m_ui.actionDiscordServer, &QAction::triggered, this, &MainWindow::onDiscordServerActionTriggered);
  connect(m_ui.actionViewThirdPartyNotices, &QAction::triggered, this,
          [this]() { AboutDialog::showThirdPartyNotices(this); });
  connect(m_ui.actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
  connect(m_ui.actionAbout, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);
  connect(m_ui.actionCheckForUpdates, &QAction::triggered, this, &MainWindow::onCheckForUpdatesActionTriggered);
  connect(m_ui.actionMemoryScanner, &QAction::triggered, this, &MainWindow::onToolsMemoryScannerTriggered);
  connect(m_ui.actionCoverDownloader, &QAction::triggered, this, &MainWindow::onToolsCoverDownloaderTriggered);
  connect(m_ui.actionMediaCapture, &QAction::toggled, this, &MainWindow::onToolsMediaCaptureToggled);
  connect(m_ui.actionCPUDebugger, &QAction::triggered, this, &MainWindow::openCPUDebugger);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionEnableGDBServer, "Debug", "EnableGDBServer", false);
  connect(m_ui.actionOpenDataDirectory, &QAction::triggered, this, &MainWindow::onToolsOpenDataDirectoryTriggered);
  connect(m_ui.actionShowGameIcons, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowGameIcons);
  connect(m_ui.actionGridViewShowTitles, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowCoverTitles);
  connect(m_ui.actionGridViewZoomIn, &QAction::triggered, m_game_list_widget, [this]() {
    if (isShowingGameList())
      m_game_list_widget->gridZoomIn();
  });
  connect(m_ui.actionGridViewZoomOut, &QAction::triggered, m_game_list_widget, [this]() {
    if (isShowingGameList())
      m_game_list_widget->gridZoomOut();
  });
  connect(m_ui.actionGridViewRefreshCovers, &QAction::triggered, m_game_list_widget,
          &GameListWidget::refreshGridCovers);

  connect(g_emu_thread, &EmuThread::settingsResetToDefault, this, &MainWindow::onSettingsResetToDefault,
          Qt::QueuedConnection);
  connect(g_emu_thread, &EmuThread::errorReported, this, &MainWindow::reportError, Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::messageConfirmed, this, &MainWindow::confirmMessage, Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::statusMessage, this, &MainWindow::onStatusMessage);
  connect(g_emu_thread, &EmuThread::onAcquireRenderWindowRequested, this, &MainWindow::acquireRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::onReleaseRenderWindowRequested, this, &MainWindow::releaseRenderWindow);
  connect(g_emu_thread, &EmuThread::onResizeRenderWindowRequested, this, &MainWindow::displayResizeRequested,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::focusDisplayWidgetRequested, this, &MainWindow::focusDisplayWidget);
  connect(g_emu_thread, &EmuThread::systemStarting, this, &MainWindow::onSystemStarting);
  connect(g_emu_thread, &EmuThread::systemStarted, this, &MainWindow::onSystemStarted);
  connect(g_emu_thread, &EmuThread::systemDestroyed, this, &MainWindow::onSystemDestroyed);
  connect(g_emu_thread, &EmuThread::systemPaused, this, &MainWindow::onSystemPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, this, &MainWindow::onSystemResumed);
  connect(g_emu_thread, &EmuThread::runningGameChanged, this, &MainWindow::onRunningGameChanged);
  connect(g_emu_thread, &EmuThread::mediaCaptureStarted, this, &MainWindow::onMediaCaptureStarted);
  connect(g_emu_thread, &EmuThread::mediaCaptureStopped, this, &MainWindow::onMediaCaptureStopped);
  connect(g_emu_thread, &EmuThread::mouseModeRequested, this, &MainWindow::onMouseModeRequested);
  connect(g_emu_thread, &EmuThread::fullscreenUIStateChange, this, &MainWindow::onFullscreenUIStateChange);
  connect(g_emu_thread, &EmuThread::achievementsLoginRequested, this, &MainWindow::onAchievementsLoginRequested);
  connect(g_emu_thread, &EmuThread::achievementsChallengeModeChanged, this,
          &MainWindow::onAchievementsChallengeModeChanged);
  connect(g_emu_thread, &EmuThread::onCoverDownloaderOpenRequested, this, &MainWindow::onToolsCoverDownloaderTriggered);

  // These need to be queued connections to stop crashing due to menus opening/closing and switching focus.
  connect(m_game_list_widget, &GameListWidget::refreshProgress, this, &MainWindow::onGameListRefreshProgress);
  connect(m_game_list_widget, &GameListWidget::refreshComplete, this, &MainWindow::onGameListRefreshComplete);
  connect(m_game_list_widget, &GameListWidget::selectionChanged, this, &MainWindow::onGameListSelectionChanged,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryActivated, this, &MainWindow::onGameListEntryActivated,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryContextMenuRequested, this,
          &MainWindow::onGameListEntryContextMenuRequested, Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::addGameDirectoryRequested, this,
          [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });

  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDisableAllEnhancements, "Main",
                                               "DisableAllEnhancements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugDumpCPUtoVRAMCopies, "Debug",
                                               "DumpCPUToVRAMCopies", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugDumpVRAMtoCPUCopies, "Debug",
                                               "DumpVRAMToCPUCopies", false);
  connect(m_ui.actionDumpRAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)")));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpRAM(filename);
  });
  connect(m_ui.actionDumpVRAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(QFileDialog::getSaveFileName(
      this, tr("Destination File"), QString(), tr("Binary Files (*.bin);;PNG Images (*.png)")));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpVRAM(filename);
  });
  connect(m_ui.actionDumpSPURAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)")));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpSPURAM(filename);
  });
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowVRAM, "Debug", "ShowVRAM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowGPUState, "Debug", "ShowGPUState", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowCDROMState, "Debug", "ShowCDROMState",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowSPUState, "Debug", "ShowSPUState", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowTimersState, "Debug", "ShowTimersState",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowMDECState, "Debug", "ShowMDECState", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowDMAState, "Debug", "ShowDMAState", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowArcadeOutputs, "Debug", "ShowArcadeOutputs",
                                               false);
}

void MainWindow::updateTheme()
{
  QtHost::UpdateApplicationTheme();
  reloadThemeSpecificImages();
}

void MainWindow::reloadThemeSpecificImages()
{
  m_game_list_widget->reloadThemeSpecificImages();
}

void MainWindow::onSettingsThemeChanged()
{
#ifdef _WIN32
  const QString old_style_name = qApp->style()->name();
#endif

  updateTheme();

#ifdef _WIN32
  // Work around a bug where the background colour of menus is broken when changing to/from the windowsvista theme.
  const QString new_style_name = qApp->style()->name();
  if ((old_style_name == QStringLiteral("windowsvista")) != (new_style_name == QStringLiteral("windowsvista")))
    recreate();
#endif
}

void MainWindow::onSettingsResetToDefault(bool system, bool controller)
{
  if (system && m_settings_window)
  {
    const bool had_settings_window = m_settings_window->isVisible();
    m_settings_window->close();
    m_settings_window->deleteLater();
    m_settings_window = nullptr;

    if (had_settings_window)
      doSettings();
  }

  if (controller && m_controller_settings_window)
  {
    const bool had_controller_settings_window = m_controller_settings_window->isVisible();
    m_controller_settings_window->close();
    m_controller_settings_window->deleteLater();
    m_controller_settings_window = nullptr;

    if (had_controller_settings_window)
      doControllerSettings(ControllerSettingsWindow::Category::GlobalSettings);
  }

  updateDebugMenuCPUExecutionMode();
  updateDebugMenuGPURenderer();
  updateDebugMenuCropMode();
  updateDebugMenuVisibility();
}

void MainWindow::saveStateToConfig()
{
  if (!isVisible() || ((windowState() & Qt::WindowFullScreen) != Qt::WindowNoState))
    return;

  bool changed = false;

  const QByteArray geometry(saveGeometry());
  const QByteArray geometry_b64(geometry.toBase64());
  const std::string old_geometry_b64(Host::GetBaseStringSettingValue("UI", "MainWindowGeometry"));
  if (old_geometry_b64 != geometry_b64.constData())
  {
    Host::SetBaseStringSettingValue("UI", "MainWindowGeometry", geometry_b64.constData());
    changed = true;
  }

  const QByteArray state(saveState());
  const QByteArray state_b64(state.toBase64());
  const std::string old_state_b64(Host::GetBaseStringSettingValue("UI", "MainWindowState"));
  if (old_state_b64 != state_b64.constData())
  {
    Host::SetBaseStringSettingValue("UI", "MainWindowState", state_b64.constData());
    changed = true;
  }

  if (changed)
    Host::CommitBaseSettingChanges();
}

void MainWindow::restoreStateFromConfig()
{
  {
    const std::string geometry_b64 = Host::GetBaseStringSettingValue("UI", "MainWindowGeometry");
    const QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(geometry_b64));
    if (!geometry.isEmpty())
      restoreGeometry(geometry);
  }

  {
    const std::string state_b64 = Host::GetBaseStringSettingValue("UI", "MainWindowState");
    const QByteArray state = QByteArray::fromBase64(QByteArray::fromStdString(state_b64));
    if (!state.isEmpty())
    {
      restoreState(state);

      // make sure we're not loading a dodgy config which had fullscreen set...
      setWindowState(windowState() & ~(Qt::WindowFullScreen | Qt::WindowActive));
    }

    {
      QSignalBlocker sb(m_ui.actionViewToolbar);
      m_ui.actionViewToolbar->setChecked(!m_ui.toolBar->isHidden());
    }
    {
      QSignalBlocker sb(m_ui.actionViewStatusBar);
      m_ui.actionViewStatusBar->setChecked(!m_ui.statusBar->isHidden());
    }
  }
}

void MainWindow::saveDisplayWindowGeometryToConfig()
{
  QWidget* container = getDisplayContainer();
  if (container->windowState() & Qt::WindowFullScreen)
  {
    // if we somehow ended up here, don't save the fullscreen state to the config
    return;
  }

  const QByteArray geometry = container->saveGeometry();
  const QByteArray geometry_b64 = geometry.toBase64();
  const std::string old_geometry_b64 = Host::GetBaseStringSettingValue("UI", "DisplayWindowGeometry");
  if (old_geometry_b64 != geometry_b64.constData())
  {
    Host::SetBaseStringSettingValue("UI", "DisplayWindowGeometry", geometry_b64.constData());
    Host::CommitBaseSettingChanges();
  }
}

void MainWindow::restoreDisplayWindowGeometryFromConfig()
{
  const std::string geometry_b64 = Host::GetBaseStringSettingValue("UI", "DisplayWindowGeometry");
  const QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(geometry_b64));
  QWidget* container = getDisplayContainer();
  if (!geometry.isEmpty())
  {
    container->restoreGeometry(geometry);

    // make sure we're not loading a dodgy config which had fullscreen set...
    container->setWindowState(container->windowState() & ~(Qt::WindowFullScreen | Qt::WindowActive));
  }
  else
  {
    // default size
    container->resize(640, 480);
  }
}

SettingsWindow* MainWindow::getSettingsWindow()
{
  if (!m_settings_window)
  {
    m_settings_window = new SettingsWindow();
    connect(m_settings_window->getInterfaceSettingsWidget(), &InterfaceSettingsWidget::themeChanged, this,
            &MainWindow::onSettingsThemeChanged);
  }

  return m_settings_window;
}

void MainWindow::doSettings(const char* category /* = nullptr */)
{
  SettingsWindow* dlg = getSettingsWindow();
  QtUtils::ShowOrRaiseWindow(dlg);
  if (category)
    dlg->setCategory(category);
}

ControllerSettingsWindow* MainWindow::getControllerSettingsWindow()
{
  if (!m_controller_settings_window)
    m_controller_settings_window = new ControllerSettingsWindow();

  return m_controller_settings_window;
}

void MainWindow::doControllerSettings(
  ControllerSettingsWindow::Category category /*= ControllerSettingsDialog::Category::Count*/)
{
  ControllerSettingsWindow* dlg = getControllerSettingsWindow();
  QtUtils::ShowOrRaiseWindow(dlg);
  if (category != ControllerSettingsWindow::Category::Count)
    dlg->setCategory(category);
}

void MainWindow::openInputProfileEditor(const std::string_view name)
{
  ControllerSettingsWindow* dlg = getControllerSettingsWindow();
  QtUtils::ShowOrRaiseWindow(dlg);
  dlg->switchProfile(name);
}

void MainWindow::updateDebugMenuCPUExecutionMode()
{
  std::optional<CPUExecutionMode> current_mode =
    Settings::ParseCPUExecutionMode(Host::GetBaseStringSettingValue("CPU", "ExecutionMode").c_str());
  if (!current_mode.has_value())
    return;

  const QString current_mode_display_name =
    QString::fromUtf8(Settings::GetCPUExecutionModeDisplayName(current_mode.value()));
  for (QObject* obj : m_ui.menuCPUExecutionMode->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_mode_display_name);
  }
}

void MainWindow::updateDebugMenuGPURenderer()
{
  // update the menu with the new selected renderer
  std::optional<GPURenderer> current_renderer =
    Settings::ParseRendererName(Host::GetBaseStringSettingValue("GPU", "Renderer").c_str());
  if (!current_renderer.has_value())
    return;

  const QString current_renderer_display_name =
    QString::fromUtf8(Settings::GetRendererDisplayName(current_renderer.value()));
  for (QObject* obj : m_ui.menuRenderer->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_renderer_display_name);
  }
}

void MainWindow::updateDebugMenuCropMode()
{
  std::optional<DisplayCropMode> current_crop_mode =
    Settings::ParseDisplayCropMode(Host::GetBaseStringSettingValue("Display", "CropMode").c_str());
  if (!current_crop_mode.has_value())
    return;

  const QString current_crop_mode_display_name =
    QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(current_crop_mode.value()));
  for (QObject* obj : m_ui.menuCropMode->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_crop_mode_display_name);
  }
}

void MainWindow::showEvent(QShowEvent* event)
{
  QMainWindow::showEvent(event);

  // This is a bit silly, but for some reason resizing *before* the window is shown
  // gives the incorrect sizes for columns, if you set the style before setting up
  // the rest of the window... so, instead, let's just force it to be resized on show.
  if (isShowingGameList())
    m_game_list_widget->resizeTableViewColumnsToFit();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  // If there's no VM, we can just exit as normal.
  if (!s_system_valid || !m_display_created)
  {
    saveStateToConfig();
    if (m_display_created)
      g_emu_thread->stopFullscreenUI();
    destroySubWindows();
    QMainWindow::closeEvent(event);
    return;
  }

  // But if there is, we have to cancel the action, regardless of whether we ended exiting
  // or not. The window still needs to be visible while GS is shutting down.
  event->ignore();

  // Exit cancelled?
  if (!requestShutdown(true, true, g_settings.save_state_on_exit))
    return;

  // Application will be exited in VM stopped handler.
  saveStateToConfig();
  m_is_closing = true;
}

void MainWindow::changeEvent(QEvent* event)
{
  if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
  {
    // TODO: This should check the render-to-main option.
    if (m_display_widget)
      g_emu_thread->redrawDisplayWindow();
  }

  if (event->type() == QEvent::StyleChange)
  {
    QtHost::SetIconThemeFromStyle();
    reloadThemeSpecificImages();
  }

  QMainWindow::changeEvent(event);
}

static QString getFilenameFromMimeData(const QMimeData* md)
{
  QString filename;
  if (md->hasUrls())
  {
    // only one url accepted
    const QList<QUrl> urls(md->urls());
    if (urls.size() == 1)
      filename = QDir::toNativeSeparators(urls.front().toLocalFile());
  }

  return filename;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const std::string filename(getFilenameFromMimeData(event->mimeData()).toStdString());
  if (!System::IsSaveStateFilename(filename) && !Arcade::Database::IdentifyArchive(filename))
    return;

  event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QString qfilename(getFilenameFromMimeData(event->mimeData()));
  const std::string filename(qfilename.toStdString());
  const bool is_save_state = System::IsSaveStateFilename(filename);
  if (!is_save_state && !Arcade::Database::IdentifyArchive(filename))
    return;

  event->acceptProposedAction();

  if (is_save_state)
    g_emu_thread->loadState(qfilename);
  else
    startArcadeSet(qfilename);
}

void MainWindow::moveEvent(QMoveEvent* event)
{
  QMainWindow::moveEvent(event);

  if (g_log_window && g_log_window->isAttachedToMainWindow())
    g_log_window->reattachToMainWindow();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
  QMainWindow::resizeEvent(event);

#ifndef _WIN32
  if (isBezelAspectRatioLockActive() && isRenderingToMain() && !isFullScreen() && !isMaximized() &&
      !m_bezel_resize_correction_pending)
  {
    m_bezel_resize_correction_pending = true;
    QTimer::singleShot(0, this, [this]() {
      m_bezel_resize_correction_pending = false;
      applyBezelAspectRatioToCurrentWindow();
    });
  }
#endif

  if (g_log_window && g_log_window->isAttachedToMainWindow())
    g_log_window->reattachToMainWindow();
}

void MainWindow::startupUpdateCheck()
{
  if (!Host::GetBaseBoolSettingValue("AutoUpdater", "CheckAtStartup", true))
    return;

  checkForUpdates(false);
}

void MainWindow::updateDebugMenuVisibility()
{
  const bool visible = QtHost::ShouldShowDebugOptions();
  m_ui.menuDebug->menuAction()->setVisible(visible);
}

void MainWindow::refreshGameList(bool invalidate_cache)
{
  m_game_list_widget->refresh(invalidate_cache);
}

void MainWindow::refreshGameListModel()
{
  m_game_list_widget->refreshModel();
}

void MainWindow::cancelGameListRefresh()
{
  m_game_list_widget->cancelRefresh();
}

void MainWindow::runOnUIThread(const std::function<void()>& func)
{
  func();
}

bool MainWindow::requestShutdown(bool allow_confirm /* = true */, bool allow_save_to_state /* = true */,
                                 bool save_state /* = true */)
{
  if (!s_system_valid)
    return true;

  // If we don't have a serial, we can't save state.
  allow_save_to_state &= !s_current_game_serial.isEmpty() && System::SupportsSaveStates();
  save_state &= allow_save_to_state;

  // Only confirm on UI thread because we need to display a msgbox.
  if (!m_is_closing && allow_confirm && Host::GetBoolSettingValue("Main", "ConfirmPowerOff", true))
  {
    SystemLock lock(pauseAndLockSystem());

    QMessageBox msgbox(lock.getDialogParent());
    msgbox.setIcon(QMessageBox::Question);
    msgbox.setWindowTitle(tr("Confirm Shutdown"));
    msgbox.setWindowModality(Qt::WindowModal);
    msgbox.setText(tr("Are you sure you want to shut down the virtual machine?"));

    QCheckBox* save_cb = new QCheckBox(tr("Save State For Resume"), &msgbox);
    save_cb->setChecked(allow_save_to_state && save_state);
    save_cb->setEnabled(allow_save_to_state);
    msgbox.setCheckBox(save_cb);
    msgbox.addButton(QMessageBox::Yes);
    msgbox.addButton(QMessageBox::No);
    msgbox.setDefaultButton(QMessageBox::Yes);
    if (msgbox.exec() != QMessageBox::Yes)
      return false;

    save_state = save_cb->isChecked();

    // Don't switch back to fullscreen when we're shutting down anyway.
    lock.cancelResume();
  }

  // This is a little bit annoying. Qt will close everything down if we don't have at least one window visible,
  // but we might not be visible because the user is using render-to-separate and hide. We don't want to always
  // reshow the main window during display updates, because otherwise fullscreen transitions and renderer switches
  // would briefly show and then hide the main window. So instead, we do it on shutdown, here. Except if we're in
  // batch mode, when we're going to exit anyway.
  if (!isRenderingToMain() && isHidden() && !QtHost::InBatchMode() && !g_emu_thread->isRunningFullscreenUI())
    updateWindowState(true);

  // Now we can actually shut down the VM.
  g_emu_thread->shutdownSystem(save_state);
  return true;
}

void MainWindow::requestExit(bool allow_confirm /* = true */)
{
  // this is block, because otherwise closeEvent() will also prompt
  if (!requestShutdown(allow_confirm, true, g_settings.save_state_on_exit))
    return;

  // VM stopped signal won't have fired yet, so queue an exit if we still have one.
  // Otherwise, immediately exit, because there's no VM to exit us later.
  if (s_system_valid)
    m_is_closing = true;
  else
    quit();
}

void MainWindow::checkForSettingChanges()
{
  LogWindow::updateSettings();
  updateWindowState();
  updateCheatActionsVisibility();
  if (s_system_valid)
    setBezelAspectRatioLockEnabled(g_settings.display_bezel_enabled && !g_settings.display_bezel_path.empty());
}

std::optional<WindowInfo> MainWindow::getWindowInfo()
{
  if (!m_display_widget || isRenderingToMain())
    return QtUtils::GetWindowInfoForWidget(this);
  else if (QWidget* widget = getDisplayContainer())
    return QtUtils::GetWindowInfoForWidget(widget);
  else
    return std::nullopt;
}

void MainWindow::onCheckForUpdatesActionTriggered()
{
  // Wipe out the last version, that way it displays the update if we've previously skipped it.
  Host::DeleteBaseSettingValue("AutoUpdater", "LastVersion");
  Host::CommitBaseSettingChanges();
  checkForUpdates(true);
}

void MainWindow::onAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  const auto lock = pauseAndLockSystem();

  AchievementLoginDialog dlg(lock.getDialogParent(), reason);
  dlg.exec();
}

void MainWindow::onAchievementsChallengeModeChanged(bool enabled)
{
  if (enabled)
  {
    QtUtils::CloseAndDeleteWindow(m_cheat_manager_window);
    QtUtils::CloseAndDeleteWindow(m_debugger_window);
    QtUtils::CloseAndDeleteWindow(m_memory_scanner_window);
  }

  updateEmulationActions(false, System::IsValid(), enabled);
}

void MainWindow::onToolsCoverDownloaderTriggered()
{
  // This can be invoked via big picture, so exit fullscreen.
  SystemLock lock(pauseAndLockSystem());
  CoverDownloadDialog dlg(lock.getDialogParent());
  connect(&dlg, &CoverDownloadDialog::coverRefreshRequested, this, [this]() {
    m_game_list_widget->refreshGridCovers();
    FullscreenUI::InvalidateCoverCache();
  });
  dlg.exec();
}

void MainWindow::onToolsMediaCaptureToggled(bool checked)
{
  if (!QtHost::IsSystemValid())
  {
    // leave it for later, we'll fill in the boot params
    return;
  }

  if (!checked)
  {
    Host::RunOnCPUThread(&System::StopMediaCapture);
    return;
  }

  const std::string container =
    Host::GetStringSettingValue("MediaCapture", "Container", Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
  const QString qcontainer = QString::fromStdString(container);
  const QString filter(tr("%1 Files (*.%2)").arg(qcontainer.toUpper()).arg(qcontainer));

  QString path =
    QString::fromStdString(System::GetNewMediaCapturePath(QtHost::GetCurrentGameTitle().toStdString(), container));
  path = QDir::toNativeSeparators(QFileDialog::getSaveFileName(this, tr("Media Capture"), path, filter));
  if (path.isEmpty())
  {
    // uncheck it again
    const QSignalBlocker sb(m_ui.actionMediaCapture);
    m_ui.actionMediaCapture->setChecked(false);
    return;
  }

  Host::RunOnCPUThread([path = path.toStdString()]() { System::StartMediaCapture(path); });
}

void MainWindow::onToolsMemoryScannerTriggered()
{
  if (Achievements::IsHardcoreModeActive())
    return;

  if (!m_memory_scanner_window)
  {
    m_memory_scanner_window = new MemoryScannerWindow();
    connect(m_memory_scanner_window, &MemoryScannerWindow::closed, this, [this]() {
      m_memory_scanner_window->deleteLater();
      m_memory_scanner_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_memory_scanner_window);
}

void MainWindow::openCheatManager()
{
  onCheatsActionTriggered();
}

void MainWindow::openCPUDebugger()
{
  if (!m_debugger_window)
  {
    m_debugger_window = new DebuggerWindow();
    connect(m_debugger_window, &DebuggerWindow::closed, this, [this]() {
      m_debugger_window->deleteLater();
      m_debugger_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_debugger_window);
}

void MainWindow::onToolsOpenDataDirectoryTriggered()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(EmuFolders::DataRoot)));
}

void MainWindow::onSettingsTriggeredFromToolbar()
{
  if (s_system_valid)
    m_settings_toolbar_menu->exec(QCursor::pos());
  else
    doSettings();
}

void MainWindow::checkForUpdates(bool display_message)
{
  if (!AutoUpdaterDialog::isSupported())
  {
    if (display_message)
    {
      QMessageBox mbox(this);
      mbox.setWindowTitle(tr("ArcadeDuck Updater"));
      mbox.setWindowModality(Qt::WindowModal);
      mbox.setTextFormat(Qt::RichText);

      mbox.setText(tr("<p>Automatic updates are not available in this build.</p>"
                      "<p>Please download the latest ArcadeDuck release from the <a "
                      "href=\"https://github.com/StillJC/ArcadeDuck\">GitHub repository</a>.</p>"));
      mbox.setIcon(QMessageBox::Information);
      mbox.exec();
    }

    return;
  }

  if (m_auto_updater_dialog)
    return;

  m_auto_updater_dialog = new AutoUpdaterDialog(this);
  connect(m_auto_updater_dialog, &AutoUpdaterDialog::updateCheckCompleted, this, &MainWindow::onUpdateCheckComplete);
  m_auto_updater_dialog->queueUpdateCheck(display_message);
}

void* MainWindow::getNativeWindowId()
{
  return (void*)winId();
}

void MainWindow::onUpdateCheckComplete()
{
  if (!m_auto_updater_dialog)
    return;

  m_auto_updater_dialog->deleteLater();
  m_auto_updater_dialog = nullptr;
}

MainWindow::SystemLock MainWindow::pauseAndLockSystem()
{
  // To switch out of fullscreen when displaying a popup, or not to?
  // For Windows, with driver's direct scanout, what renders behind tends to be hit and miss.
  // We can't draw anything over exclusive fullscreen, so get out of it in that case.
  // Wayland's a pain as usual, we need to recreate the window, which means there'll be a brief
  // period when there's no window, and Qt might shut us down. So avoid it there.
  // On MacOS, it forces a workspace switch, which is kinda jarring.

#ifndef __APPLE__
  const bool was_fullscreen = g_emu_thread->isFullscreen() && !s_use_central_widget;
#else
  const bool was_fullscreen = false;
#endif
  const bool was_paused = !s_system_valid || s_system_paused;

  // We need to switch out of exclusive fullscreen before we can display our popup.
  // However, we do not want to switch back to render-to-main, the window might have generated this event.
  if (was_fullscreen)
  {
    g_emu_thread->setFullscreen(false, false);

    // Container could change... thanks Wayland.
    QWidget* container;
    while (s_system_valid &&
           (g_emu_thread->isFullscreen() || !(container = getDisplayContainer()) || container->isFullScreen()))
    {
      QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
  }

  if (!was_paused)
  {
    g_emu_thread->setSystemPaused(true);

    // Need to wait for the pause to go through, and make the main window visible if needed.
    while (!s_system_paused)
      QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);

    // Ensure it's visible before we try to create any dialogs parented to us.
    QApplication::sync();
  }

  // Now we'll either have a borderless window, or a regular window (if we were exclusive fullscreen).
  QWidget* dialog_parent = getDisplayContainer();

  return SystemLock(dialog_parent, was_paused, was_fullscreen);
}

MainWindow::SystemLock::SystemLock(QWidget* dialog_parent, bool was_paused, bool was_fullscreen)
  : m_dialog_parent(dialog_parent), m_was_paused(was_paused), m_was_fullscreen(was_fullscreen)
{
}

MainWindow::SystemLock::SystemLock(SystemLock&& lock)
  : m_dialog_parent(lock.m_dialog_parent), m_was_paused(lock.m_was_paused), m_was_fullscreen(lock.m_was_fullscreen)
{
  lock.m_dialog_parent = nullptr;
  lock.m_was_paused = true;
  lock.m_was_fullscreen = false;
}

MainWindow::SystemLock::~SystemLock()
{
  if (m_was_fullscreen)
    g_emu_thread->setFullscreen(true, true);
  if (!m_was_paused)
    g_emu_thread->setSystemPaused(false);
}

void MainWindow::SystemLock::cancelResume()
{
  m_was_paused = true;
  m_was_fullscreen = false;
}
