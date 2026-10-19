// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "ui_aboutdialog.h"
#include <QtCore/QElapsedTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QDialog>

class QLabel;
class QTimer;

class AboutDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit AboutDialog(QWidget* parent = nullptr);
  ~AboutDialog();

  static void showThirdPartyNotices(QWidget* parent);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  void startLogoSequence();
  void updateLogoSequence();
  void stopLogoSequence();
  void startLogoAudio();
  void stopLogoAudio();

  Ui::AboutDialog m_ui;

  QElapsedTimer m_logo_click_timer;
  QElapsedTimer m_logo_animation_timer;
  QPixmap m_logo_frames[4];
  QTimer* m_logo_timer = nullptr;
  QLabel* m_logo_animation = nullptr;
  QString m_normal_title;
  QString m_logo_audio_path;
  int m_logo_click_count = 0;
  int m_logo_frame = 0;
  bool m_logo_sequence_active = false;
};
