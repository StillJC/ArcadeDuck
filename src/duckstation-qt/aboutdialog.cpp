#include "aboutdialog.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include <QtCore/QString>
#include <QtGui/QPixmap>
#include <QtWidgets/QDialog>

AboutDialog::AboutDialog(QWidget* parent /* = nullptr */) : QDialog(parent)
{
  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  resize(760, 420);
  setMinimumSize(760, 420);

  m_ui.icon->setMinimumSize(320, 200);
  m_ui.icon->setMaximumSize(320, 200);
  m_ui.icon->setPixmap(QPixmap(QStringLiteral(":/icons/duck.png"))
                         .scaled(m_ui.icon->maximumSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

  m_ui.scmversion->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_ui.scmversion->setText(tr("%1 (%2)").arg(QString(g_scm_tag_str)).arg(QString(g_scm_branch_str)));

  m_ui.description->setTextInteractionFlags(Qt::TextBrowserInteraction);
  m_ui.description->setOpenExternalLinks(true);
  m_ui.description->setText(
    QStringLiteral(R"(
<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.0//EN" "http://www.w3.org/TR/REC-html40/strict.dtd">
<html><head><meta name="qrichtext" content="1" /><style type="text/css">
p, li { white-space: pre-wrap; }
</style></head><body style=" font-size:10pt; font-weight:400; font-style:normal;">
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%1</p>
<p style=" margin-top:12px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%2</p>
<p style=" margin-top:12px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><span style=" font-weight:600;">%3</span>:</p>
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">  Connor McLaughlin &lt;stenzek@gmail.com&gt;</p>
<p style=" margin-top:0px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">  and other <a href="https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md"><span style=" text-decoration: underline; color:#0057ae;">DuckStation contributors</span></a></p>
<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%4</p>
<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><a href="https://github.com/StillJC/ArcadeDuck/blob/main/LICENSE"><span style=" text-decoration: underline; color:#0057ae;">%5</span></a> | <a href="https://github.com/StillJC/ArcadeDuck"><span style=" text-decoration: underline; color:#0057ae;">ArcadeDuck GitHub</span></a> | <a href="https://github.com/stenzek/duckstation"><span style=" text-decoration: underline; color:#0057ae;">DuckStation GitHub</span></a></p></body></html>
)")
      .arg(tr(
        "ArcadeDuck is a PlayStation-based arcade hardware emulator built from a heavily modified DuckStation base."))
      .arg(tr("ArcadeDuck focuses on PS1-based arcade hardware support, arcade-oriented loading, input handling, and "
              "cabinet use."))
      .arg(tr("Original DuckStation Credits"))
      .arg(tr("ArcadeDuck logo and icon created by the ArcadeDuck project."))
      .arg(tr("ArcadeDuck License")));
}

AboutDialog::~AboutDialog() = default;