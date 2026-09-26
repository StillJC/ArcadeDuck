// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "memoryviewwidget.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QScrollBar>

#include <algorithm>

MemoryViewWidget::MemoryViewWidget(QWidget* parent, size_t address_offset, const void* data_ptr, size_t data_size)
  : QAbstractScrollArea(parent)
{
  updateMetrics();

  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &MemoryViewWidget::adjustContent);
  connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { viewport()->update(); });

  setData(address_offset, data_ptr, data_size);
}

MemoryViewWidget::~MemoryViewWidget() = default;

int MemoryViewWidget::addressWidth() const
{
  return 10 * m_char_width;
}

int MemoryViewWidget::hexWidth() const
{
  return static_cast<int>((BYTES_PER_LINE * 3) + 1) * m_char_width;
}

int MemoryViewWidget::asciiWidth() const
{
  return static_cast<int>(BYTES_PER_LINE + 2) * m_char_width;
}

int MemoryViewWidget::contentWidth() const
{
  return addressWidth() + hexWidth() + asciiWidth();
}

void MemoryViewWidget::updateMetrics()
{
  const QFontMetrics metrics(font());
  m_char_width = std::max(1, metrics.horizontalAdvance(QLatin1Char('0')));
  m_char_height = std::max(1, metrics.height());
}

void MemoryViewWidget::setData(size_t address_offset, const void* data_ptr, size_t data_size)
{
  m_address_offset = address_offset;
  m_data = static_cast<const unsigned char*>(data_ptr);
  m_data_size = data_size;

  if (verticalScrollBar()->value() != 0)
    verticalScrollBar()->setValue(0);

  adjustContent();
}

void MemoryViewWidget::setHighlightRange(size_t start, size_t end)
{
  m_highlight_start = std::min(start, m_data_size);
  m_highlight_end = std::min(std::max(start, end), m_data_size);
  viewport()->update();
}

void MemoryViewWidget::clearHighlightRange()
{
  m_highlight_start = 0;
  m_highlight_end = 0;
  viewport()->update();
}

void MemoryViewWidget::scrolltoOffset(size_t offset)
{
  if (m_data_size == 0)
    return;

  const size_t clamped_offset = std::min(offset, m_data_size - 1);
  verticalScrollBar()->setValue(static_cast<int>(clamped_offset / BYTES_PER_LINE));
  horizontalScrollBar()->setValue(0);
}

void MemoryViewWidget::scrollToAddress(size_t address)
{
  const size_t offset = (address > m_address_offset) ? (address - m_address_offset) : 0;
  scrolltoOffset(offset);
}

void MemoryViewWidget::setFont(const QFont& font)
{
  QAbstractScrollArea::setFont(font);
  updateMetrics();
  adjustContent();
}

void MemoryViewWidget::resizeEvent(QResizeEvent* event)
{
  QAbstractScrollArea::resizeEvent(event);
  adjustContent();
}

void MemoryViewWidget::paintEvent(QPaintEvent*)
{
  QPainter painter(viewport());
  painter.setFont(font());
  painter.fillRect(viewport()->rect(), viewport()->palette().color(QPalette::Base));

  if (!m_data || m_data_size == 0)
    return;

  const int horizontal_offset = horizontalScrollBar()->value();
  const int address_x = m_char_width - horizontal_offset;
  const int hex_x = addressWidth() - horizontal_offset;
  const int ascii_x = addressWidth() + hexWidth() - horizontal_offset;
  const QColor text_color = viewport()->palette().color(QPalette::Text);
  const QColor highlight_color = viewport()->palette().color(QPalette::Highlight);
  const QColor highlight_text_color = viewport()->palette().color(QPalette::HighlightedText);

  painter.setPen(text_color);

  const int header_y = m_char_height;
  for (unsigned col = 0; col < BYTES_PER_LINE; col++)
  {
    painter.drawText(hex_x + static_cast<int>(col * 3 + 1) * m_char_width, header_y,
                     QString::asprintf("%02X", col));
  }

  painter.drawText(ascii_x + m_char_width, header_y, QStringLiteral("ASCII"));
  painter.drawLine(0, header_y + 2, contentWidth(), header_y + 2);

  const size_t first_row = static_cast<size_t>(verticalScrollBar()->value());
  const size_t total_rows = (m_data_size + BYTES_PER_LINE - 1) / BYTES_PER_LINE;
  const size_t last_row = std::min(total_rows, first_row + static_cast<size_t>(m_rows_visible));

  for (size_t row = first_row; row < last_row; row++)
  {
    const size_t row_offset = row * BYTES_PER_LINE;
    const int y = static_cast<int>(row - first_row + 2) * m_char_height;

    const size_t row_address = m_address_offset + row_offset;
    painter.setPen(text_color);
    painter.drawText(address_x, y,
                     QStringLiteral("%1").arg(static_cast<qulonglong>(row_address), 8, 16, QLatin1Char('0')).toUpper());

    for (unsigned col = 0; col < BYTES_PER_LINE; col++)
    {
      const size_t offset = row_offset + col;
      if (offset >= m_data_size)
        break;

      const unsigned char value = m_data[offset];
      const bool highlighted = (offset >= m_highlight_start && offset < m_highlight_end);
      const int cell_hex_x = hex_x + static_cast<int>(col * 3) * m_char_width;
      const int cell_ascii_x = ascii_x + static_cast<int>(col) * m_char_width;
      const QRect hex_rect(cell_hex_x, y - m_char_height + 2, 3 * m_char_width, m_char_height);
      const QRect ascii_rect(cell_ascii_x, y - m_char_height + 2, m_char_width, m_char_height);

      if (highlighted)
      {
        painter.fillRect(hex_rect, highlight_color);
        painter.fillRect(ascii_rect, highlight_color);
        painter.setPen(highlight_text_color);
      }
      else
      {
        painter.setPen(text_color);
      }

      painter.drawText(cell_hex_x, y, QString::asprintf("%02X", static_cast<unsigned>(value)));

      const QChar ascii = (value >= 0x20 && value <= 0x7e) ? QChar::fromLatin1(static_cast<char>(value))
                                                           : QLatin1Char('.');
      painter.drawText(cell_ascii_x, y, ascii);
    }
  }
}

void MemoryViewWidget::adjustContent()
{
  if (!m_data || m_data_size == 0)
  {
    setEnabled(false);
    verticalScrollBar()->setRange(0, 0);
    horizontalScrollBar()->setRange(0, 0);
    viewport()->update();
    return;
  }

  setEnabled(true);

  m_rows_visible = std::max(1, (viewport()->height() / m_char_height) - 1);

  const size_t total_rows = (m_data_size + BYTES_PER_LINE - 1) / BYTES_PER_LINE;
  const int maximum_row =
    (total_rows > static_cast<size_t>(m_rows_visible)) ? static_cast<int>(total_rows - m_rows_visible) : 0;

  verticalScrollBar()->setRange(0, maximum_row);
  verticalScrollBar()->setPageStep(m_rows_visible);
  verticalScrollBar()->setSingleStep(1);

  const int horizontal_maximum = std::max(0, contentWidth() - viewport()->width());
  horizontalScrollBar()->setRange(0, horizontal_maximum);
  horizontalScrollBar()->setPageStep(viewport()->width());

  viewport()->update();
}
