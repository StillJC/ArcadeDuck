// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "aboutdialog.h"
#include "qtutils.h"

#include "core/settings.h"

#include "common/file_system.h"
#include "common/path.h"

#include "scmversion/arcadeduck_version.h"
#include "scmversion/scmversion.h"

#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTextBrowser>

AboutDialog::AboutDialog(QWidget* parent /* = nullptr */) : QDialog(parent)
{
  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  setFixedSize(geometry().width(), geometry().height());

  m_ui.scmversion->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_ui.scmversion->setText(QStringLiteral("%1\nBuild %2\nSCM revision: %3 (%4, %5)\nBuild date: %6\nGPL baseline: %7")
                             .arg(QStringLiteral(ARCADEDUCK_PRODUCT_NAME " " ARCADEDUCK_SEMANTIC_VERSION))
                             .arg(QStringLiteral(ARCADEDUCK_BUILD_STRING))
                             .arg(QLatin1StringView(g_scm_hash_str))
                             .arg(QLatin1StringView(g_scm_branch_str))
                             .arg(QLatin1StringView(g_scm_tag_str))
                             .arg(QLatin1StringView(g_scm_date_str))
                             .arg(QStringLiteral(ARCADEDUCK_GPL_BASELINE_HASH)));

  m_ui.description->setTextInteractionFlags(Qt::TextBrowserInteraction);
  m_ui.description->setOpenExternalLinks(true);
  m_ui.description->setText(QStringLiteral(R"(
<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.0//EN" "http://www.w3.org/TR/REC-html40/strict.dtd">
<html><head><meta name="qrichtext" content="1" /><style type="text/css">
p, li { white-space: pre-wrap; }
</style></head><body style=" font-size:10pt; font-weight:400; font-style:normal;">
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%1</p>
<p style=" margin-top:12px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><span style=" font-weight:600;">%2</span>:</p>
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">  Connor McLaughlin &lt;stenzek@gmail.com&gt;</p>
<p style=" margin-top:0px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">  and other <a href="https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md">contributors</a></p>
<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%3 <a href="https://icons8.com/icon/74847/platforms.undefined.short-title"><span style=" text-decoration: underline; color:#0057ae;">icons8</span></a></p>
<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><a href="https://github.com/StillJC/ArcadeDuck/blob/main/LICENSE"><span style=" text-decoration: underline; color:#0057ae;">%4</span></a> | <a href="https://github.com/StillJC/ArcadeDuck"><span style=" text-decoration: underline; color:#0057ae;">GitHub</span></a> | <a href="https://discord.gg/fQ9HvgKCg"><span style=" text-decoration: underline; color:#0057ae;">Support</span></a></p></body></html>
)")
                              .arg(tr("ArcadeDuck is a free and open-source PS1-based arcade emulator built from the "
                                      "final GPL release of DuckStation."))
                              .arg(tr("Authors"))
                              .arg(tr("Icon by"))
                              .arg(tr("License")));
}

AboutDialog::~AboutDialog() = default;

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
