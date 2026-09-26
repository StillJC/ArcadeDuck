// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QtWidgets/QAbstractScrollArea>

#include <cstddef>

class MemoryViewWidget final : public QAbstractScrollArea
{
  Q_OBJECT

public:
  MemoryViewWidget(QWidget* parent = nullptr, size_t address_offset = 0, const void* data_ptr = nullptr,
                   size_t data_size = 0);
  ~MemoryViewWidget();

  size_t addressOffset() const { return m_address_offset; }

  void setData(size_t address_offset, const void* data_ptr, size_t data_size);
  void setHighlightRange(size_t start, size_t end);
  void clearHighlightRange();
  void scrolltoOffset(size_t offset);
  void scrollToAddress(size_t address);
  void setFont(const QFont& font);

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
  void adjustContent();

private:
  static constexpr unsigned BYTES_PER_LINE = 16;

  int addressWidth() const;
  int hexWidth() const;
  int asciiWidth() const;
  int contentWidth() const;
  void updateMetrics();

  const unsigned char* m_data = nullptr;
  size_t m_data_size = 0;
  size_t m_address_offset = 0;
  size_t m_highlight_start = 0;
  size_t m_highlight_end = 0;

  int m_char_width = 1;
  int m_char_height = 1;
  int m_rows_visible = 1;
};
