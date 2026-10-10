// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "aboutdialog.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/settings.h"

#include "common/file_system.h"
#include "common/path.h"

#include "scmversion/arcadeduck_version.h"
#include "scmversion/scmversion.h"

#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTextBrowser>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <mmsystem.h>
#endif

AboutDialog::AboutDialog(QWidget* parent /* = nullptr */) : QDialog(parent)
{
  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  setFixedSize(geometry().width(), geometry().height());

  m_normal_title = m_ui.title->text();
  m_ui.icon->installEventFilter(this);

  if (const QString& exo2_family = QtHost::GetExo2FontFamily(); !exo2_family.isEmpty())
  {
    QFont title_font(m_ui.title->font());
    title_font.setFamily(exo2_family);
    title_font.setWeight(QFont::Bold);
    m_ui.title->setFont(title_font);

    QFont version_font(m_ui.scmversion->font());
    version_font.setFamily(exo2_family);
    version_font.setWeight(QFont::DemiBold);
    m_ui.scmversion->setFont(version_font);
  }

  m_ui.scmversion->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_ui.scmversion->setText(QStringLiteral("%1\nCommit: %2\nBranch: %3\nCommit date: %4")
                              .arg(QString::fromLatin1(g_scm_version_str))
                              .arg(QLatin1StringView(g_scm_hash_str))
                              .arg(QLatin1StringView(g_scm_branch_str))
                              .arg(QLatin1StringView(g_scm_date_str)));  m_ui.description->setTextInteractionFlags(Qt::TextBrowserInteraction);
  m_ui.description->setOpenExternalLinks(true);
  m_ui.description->setText(QStringLiteral(R"(
<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.0//EN" "http://www.w3.org/TR/REC-html40/strict.dtd">
<html><head><meta name="qrichtext" content="1" /><style type="text/css">
p, li { white-space: pre-wrap; }
</style></head><body style=" font-size:10pt; font-weight:400; font-style:normal;">
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%1</p>
<p style=" margin-top:12px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><span style=" font-weight:600;">%2</span>:</p>
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">  ArcadeDuck &lt;<a href="mailto:ArcadeDuck1@gmail.com">ArcadeDuck1@gmail.com</a>&gt;</p>

<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%3 <a href="https://icons8.com/icon/74847/platforms.undefined.short-title"><span style=" text-decoration: underline; color:#0057ae;">icons8</span></a></p>
<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><a href="https://github.com/StillJC/ArcadeDuck/blob/main/LICENSE"><span style=" text-decoration: underline; color:#0057ae;">%4</span></a> | <a href="https://github.com/StillJC/ArcadeDuck"><span style=" text-decoration: underline; color:#0057ae;">GitHub</span></a> | <a href="https://discord.gg/fQ9HvgKCg"><span style=" text-decoration: underline; color:#0057ae;">Support</span></a></p></body></html>
)")
                              .arg(tr("ArcadeDuck is a free and open-source emulator focused on PlayStation 1-based arcade hardware."))
                              .arg(tr("Project contact"))
                              .arg(tr("Icon by"))
                              .arg(tr("License")));
}

AboutDialog::~AboutDialog()
{
  stopLogoAudio();
}

bool AboutDialog::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == m_ui.icon && event->type() == QEvent::MouseButtonRelease)
  {
    const QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->button() == Qt::LeftButton && !m_logo_sequence_active)
    {
      if (!m_logo_click_timer.isValid() || m_logo_click_timer.elapsed() > 4000)
        m_logo_click_count = 0;

      m_logo_click_timer.restart();
      if (++m_logo_click_count >= 7)
      {
        m_logo_click_count = 0;
        startLogoSequence();
      }
    }
  }

  return QDialog::eventFilter(watched, event);
}

void AboutDialog::startLogoSequence()
{
  static constexpr const char* frame_paths[] = {
    ":/aboutfx/seq0.png",
    ":/aboutfx/seq1.png",
    ":/aboutfx/seq2.png",
    ":/aboutfx/seq3.png",
  };

  for (int i = 0; i < 4; i++)
  {
    const QPixmap frame(QString::fromLatin1(frame_paths[i]));
    if (frame.isNull())
      return;

    m_logo_frames[i] = frame.scaledToHeight(100, Qt::FastTransformation);
  }

  m_logo_sequence_active = true;
  m_logo_frame = 0;
  static constexpr char mode_suffix[] = {81, 85, 65, 67, 75, 32, 77, 79, 68, 69, 0};
  m_ui.title->setText(QStringLiteral("%1 — %2").arg(m_normal_title, QString::fromLatin1(mode_suffix)));

  m_logo_animation = new QLabel(this);
  m_logo_animation->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_logo_animation->setAlignment(Qt::AlignCenter);
  m_logo_animation->setFixedSize(120, 110);
  m_logo_animation->setPixmap(m_logo_frames[0]);
  m_logo_animation->move(-m_logo_animation->width(), height() - m_logo_animation->height() - 6);
  m_logo_animation->show();
  m_logo_animation->raise();

  m_logo_timer = new QTimer(this);
  m_logo_timer->setTimerType(Qt::PreciseTimer);
  m_logo_timer->setInterval(16);
  connect(m_logo_timer, &QTimer::timeout, this, &AboutDialog::updateLogoSequence);

  startLogoAudio();
  m_logo_animation_timer.restart();
  m_logo_timer->start();
}

void AboutDialog::updateLogoSequence()
{
  static constexpr qint64 animation_duration_ms = 8000;
  static constexpr qint64 frame_duration_ms = 120;

  const qint64 elapsed = m_logo_animation_timer.elapsed();
  if (elapsed >= animation_duration_ms)
  {
    stopLogoSequence();
    return;
  }

  const int next_frame = static_cast<int>((elapsed / frame_duration_ms) % 4);
  if (next_frame != m_logo_frame)
  {
    m_logo_frame = next_frame;
    m_logo_animation->setPixmap(m_logo_frames[m_logo_frame]);
  }

  const int travel_distance = width() + m_logo_animation->width();
  const int x = -m_logo_animation->width() + static_cast<int>((travel_distance * elapsed) / animation_duration_ms);
  m_logo_animation->move(x, height() - m_logo_animation->height() - 6);
}

void AboutDialog::stopLogoSequence()
{
  if (m_logo_timer)
  {
    m_logo_timer->stop();
    m_logo_timer->deleteLater();
    m_logo_timer = nullptr;
  }

  if (m_logo_animation)
  {
    m_logo_animation->deleteLater();
    m_logo_animation = nullptr;
  }

  stopLogoAudio();
  m_ui.title->setText(m_normal_title);
  m_logo_sequence_active = false;
}

void AboutDialog::startLogoAudio()
{
#ifdef _WIN32
  using MciSendStringW = MCIERROR(WINAPI*)(LPCWSTR, LPWSTR, UINT, HWND);

  const HMODULE winmm = LoadLibraryW(L"winmm.dll");
  if (!winmm)
    return;

  const auto mci_send_string =
    reinterpret_cast<MciSendStringW>(GetProcAddress(winmm, "mciSendStringW"));
  if (!mci_send_string)
  {
    FreeLibrary(winmm);
    return;
  }

  const QString temp_directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  if (temp_directory.isEmpty())
  {
    FreeLibrary(winmm);
    return;
  }

  m_logo_audio_path = QDir(temp_directory).filePath(QStringLiteral("ArcadeDuck_aboutfx.mp3"));
  if (!QFile::exists(m_logo_audio_path) &&
      !QFile::copy(QStringLiteral(":/aboutfx/seq.mp3"), m_logo_audio_path))
  {
    m_logo_audio_path.clear();
    FreeLibrary(winmm);
    return;
  }

  mci_send_string(L"close arcadeduck_aboutfx", nullptr, 0, nullptr);
  const QString open_command =
    QStringLiteral("open \"%1\" type mpegvideo alias arcadeduck_aboutfx").arg(QDir::toNativeSeparators(m_logo_audio_path));
  if (mci_send_string(reinterpret_cast<LPCWSTR>(open_command.utf16()), nullptr, 0, nullptr) == 0)
    mci_send_string(L"play arcadeduck_aboutfx from 0", nullptr, 0, nullptr);

  FreeLibrary(winmm);
#endif
}

void AboutDialog::stopLogoAudio()
{
#ifdef _WIN32
  using MciSendStringW = MCIERROR(WINAPI*)(LPCWSTR, LPWSTR, UINT, HWND);

  if (const HMODULE winmm = LoadLibraryW(L"winmm.dll"))
  {
    if (const auto mci_send_string =
          reinterpret_cast<MciSendStringW>(GetProcAddress(winmm, "mciSendStringW")))
    {
      mci_send_string(L"stop arcadeduck_aboutfx", nullptr, 0, nullptr);
      mci_send_string(L"close arcadeduck_aboutfx", nullptr, 0, nullptr);
    }
    FreeLibrary(winmm);
  }

  if (!m_logo_audio_path.isEmpty())
  {
    QFile::remove(m_logo_audio_path);
    m_logo_audio_path.clear();
  }
#endif
}

void AboutDialog::showThirdPartyNotices(QWidget* parent)
{
  QDialog dialog(parent);
  dialog.setMinimumSize(700, 400);
  dialog.setWindowTitle(tr("ArcadeDuck Third-Party Notices"));

  QIcon icon;
  icon.addFile(QString::fromUtf8(":/icons/duck.png"), QSize(), QIcon::Normal, QIcon::Off);
  dialog.setWindowIcon(icon);

  QVBoxLayout* layout = new QVBoxLayout(&dialog);

  QTextBrowser* tb = new QTextBrowser(&dialog);
  tb->setAcceptRichText(true);
  tb->setReadOnly(true);
  tb->setOpenExternalLinks(true);
  if (std::optional<std::string> notice =
        FileSystem::ReadFileToString(Path::Combine(EmuFolders::Resources, "thirdparty.html").c_str());
      notice.has_value())
  {
    tb->setText(QString::fromStdString(notice.value()));
  }
  else
  {
    tb->setText(tr("Missing thirdparty.html file. You should request it from where-ever you obtained ArcadeDuck."));
  }
  layout->addWidget(tb, 1);

  QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  connect(bb->button(QDialogButtonBox::Close), &QPushButton::clicked, &dialog, &QDialog::done);
  layout->addWidget(bb, 0);

  dialog.exec();
}
