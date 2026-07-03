#include "TableLayout.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <new>

#include "Page.h"

TableLayout::TableLayout(const GfxRenderer& renderer, const int fontId, const float lineCompression,
                         const int16_t originX, const uint16_t availableWidth)
    : renderer(renderer),
      fontId(fontId),
      lineHeight(static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f)),
      originX(originX),
      availableWidth(availableWidth) {
  bufferedRows.reserve(SAMPLE_ROW_LIMIT);
}

BlockStyle TableLayout::cellBlockStyle(const bool isHeaderCell, const CssStyle& cssStyle) {
  BlockStyle style;
  style.textAlignDefined = true;
  style.alignment =
      cssStyle.hasTextAlign() ? cssStyle.textAlign : (isHeaderCell ? CssTextAlign::Center : CssTextAlign::Left);
  // Suppress the paragraph first-line indent inside cells.
  style.textIndentDefined = true;
  style.textIndent = 0;
  if (cssStyle.hasDirection()) {
    style.directionDefined = true;
    style.isRtl = cssStyle.direction == CssTextDirection::Rtl;
  }
  return style;
}

void TableLayout::startRow() {
  rowOpen = true;
  cellOpen = false;
  currentRowCols = 0;
  currentRow.clear();
  currentRow.reserve(MAX_COLS);
}

bool TableLayout::startCell(const uint8_t colSpan) {
  if (!rowOpen) {
    // Malformed HTML (<td> without <tr>): open an implicit row instead of losing content.
    startRow();
  }
  if (cellOpen || currentRowCols >= MAX_COLS) {
    LOG_DBG("TBL", "Dropping table cell beyond column cap (%u)", static_cast<unsigned>(MAX_COLS));
    return false;
  }
  if (layoutDecided && useColumns && currentRowCols >= columnWidths.size()) {
    // Row is wider than the sampled column count; extra cells are dropped.
    LOG_DBG("TBL", "Dropping table cell beyond sampled column count (%u)", static_cast<unsigned>(columnWidths.size()));
    return false;
  }
  cellOpen = true;
  pendingColSpan = static_cast<uint8_t>(std::clamp<size_t>(colSpan, 1, MAX_COLS - currentRowCols));
  return true;
}

void TableLayout::endCell(std::unique_ptr<ParsedText> text) {
  if (!cellOpen) {
    return;  // cell was dropped at startCell; discard its content
  }
  cellOpen = false;
  if (!layoutDecided && text) {
    sampledWords += text->size();
  }
  Cell cell;
  cell.colSpan = pendingColSpan;
  cell.text = std::move(text);
  currentRowCols += pendingColSpan;
  currentRow.push_back(std::move(cell));
}

void TableLayout::endRow(TablePageSink& sink) {
  cellOpen = false;
  rowOpen = false;
  if (currentRow.empty()) {
    return;
  }

  if (layoutDecided) {
    emitRow(currentRow, sink);
    currentRow.clear();
    return;
  }

  bufferedRows.push_back(std::move(currentRow));
  currentRow.clear();
  if (bufferedRows.size() >= SAMPLE_ROW_LIMIT || sampledWords >= SAMPLE_WORD_LIMIT) {
    decideLayout();
    flushBufferedRows(sink);
  }
}

void TableLayout::finish(TablePageSink& sink) {
  // Salvage a row whose </tr> never arrived.
  endRow(sink);
  if (!layoutDecided) {
    decideLayout();
  }
  flushBufferedRows(sink);
  if (emittedAnything) {
    // Breathing room below the table, mirroring the gap added above it.
    if (sink.currentY() > 0 && sink.currentY() + lineHeight / 2 <= sink.pageHeight()) {
      sink.advanceY(lineHeight / 2);
    }
  }
}

void TableLayout::decideLayout() {
  layoutDecided = true;

  size_t numCols = 0;
  for (const auto& row : bufferedRows) {
    size_t cols = 0;
    for (const auto& cell : row) {
      cols += cell.colSpan;
    }
    numCols = std::max(numCols, cols);
  }
  numCols = std::min(numCols, MAX_COLS);

  // Narrowest legible column: roughly two characters of the current font.
  const int minColWidth = std::max(8, renderer.getFontAscenderSize(fontId) * 2);
  const int slotOverhead = 2 * CELL_PAD_X + BORDER;
  const size_t maxFit = availableWidth > static_cast<uint16_t>(BORDER + minColWidth + slotOverhead)
                            ? static_cast<size_t>((availableWidth - BORDER) / (minColWidth + slotOverhead))
                            : 1;
  // A 1-column grid is just a boxed paragraph; render it (and anything too wide) stacked.
  useColumns = numCols >= 2 && numCols <= maxFit;
  if (!useColumns) {
    if (numCols > maxFit) {
      LOG_DBG("TBL", "Table with %u columns too wide for viewport (%u fit), using stacked layout",
              static_cast<unsigned>(numCols), static_cast<unsigned>(maxFit));
    }
    return;
  }

  // Natural (single-line) width of each column, sampled from the buffered rows.
  // Spanning cells contribute an even share to each spanned column.
  std::vector<int32_t> natural(numCols, 0);
  for (const auto& row : bufferedRows) {
    size_t col = 0;
    for (const auto& cell : row) {
      if (col >= numCols) break;
      const size_t span = std::min<size_t>(cell.colSpan, numCols - col);
      if (cell.text && !cell.text->isEmpty()) {
        int naturalWidth = 0;
        int maxWordWidth = 0;
        cell.text->measureIntrinsicWidths(renderer, fontId, naturalWidth, maxWordWidth);
        const auto perCol = static_cast<int32_t>(naturalWidth / static_cast<int>(span));
        for (size_t c = col; c < col + span; ++c) {
          natural[c] = std::max(natural[c], perCol);
        }
      }
      col += span;
    }
  }

  // Distribute the available content width: every column first gets
  // min(natural, fair share); leftover space goes proportionally to columns
  // whose content wants more. Fully satisfied content leaves the table
  // narrower than the viewport (content-fit).
  const auto avail = static_cast<int32_t>(availableWidth) - static_cast<int32_t>(numCols + 1) * BORDER -
                     static_cast<int32_t>(numCols) * 2 * CELL_PAD_X;
  const int32_t fair = avail / static_cast<int32_t>(numCols);
  columnWidths.assign(numCols, 0);
  int32_t unmetTotal = 0;
  for (size_t c = 0; c < numCols; ++c) {
    const int32_t want = std::max<int32_t>(natural[c], minColWidth);
    const int32_t base = std::min(want, fair);
    columnWidths[c] = static_cast<uint16_t>(std::max<int32_t>(base, 1));
    if (want > base) {
      unmetTotal += want - base;
    }
  }
  int32_t leftover = avail;
  for (const auto w : columnWidths) leftover -= w;
  if (leftover > 0 && unmetTotal > 0) {
    for (size_t c = 0; c < numCols; ++c) {
      const int32_t want = std::max<int32_t>(natural[c], minColWidth);
      const int32_t unmet = want - columnWidths[c];
      if (unmet > 0) {
        const auto extra = static_cast<int32_t>(static_cast<int64_t>(leftover) * unmet / unmetTotal);
        columnWidths[c] = static_cast<uint16_t>(columnWidths[c] + std::min(unmet, extra));
      }
    }
  }

  totalWidth = static_cast<uint16_t>((numCols + 1) * BORDER);
  for (const auto w : columnWidths) {
    totalWidth = static_cast<uint16_t>(totalWidth + w + 2 * CELL_PAD_X);
  }
  tableX = static_cast<int16_t>(originX + std::max(0, (availableWidth - totalWidth) / 2));

  boundaryXs.clear();
  boundaryXs.reserve(numCols + 1);
  int16_t x = tableX;
  for (size_t b = 0; b <= numCols; ++b) {
    boundaryXs.push_back(x);
    if (b < numCols) {
      x = static_cast<int16_t>(x + BORDER + columnWidths[b] + 2 * CELL_PAD_X);
    }
  }
}

void TableLayout::flushBufferedRows(TablePageSink& sink) {
  for (auto& row : bufferedRows) {
    emitRow(row, sink);
  }
  bufferedRows.clear();
}

void TableLayout::emitRow(Row& row, TablePageSink& sink) {
  if (useColumns) {
    emitRowColumns(row, sink);
  } else {
    emitRowStacked(row, sink);
  }
}

void TableLayout::addRect(TablePageSink& sink, const int16_t x, const int16_t y, const uint16_t w,
                          const uint16_t h) const {
  if (w == 0 || h == 0) {
    return;
  }
  auto rect = std::shared_ptr<PageRect>(new (std::nothrow) PageRect(w, h, x, y));
  if (!rect) {
    LOG_ERR("TBL", "OOM: PageRect");
    return;
  }
  sink.addElement(std::move(rect));
}

// UTF-8 for U+2026 HORIZONTAL ELLIPSIS, appended where cell content is truncated.
constexpr const char* TRUNCATION_ELLIPSIS = "\xE2\x80\xA6";

size_t TableLayout::layoutRowCells(Row& row) {
  const size_t numCols = columnWidths.size();

  rowCellLines.clear();
  rowCellX.clear();
  rowBoundaries.clear();
  rowCellLines.reserve(row.size());
  rowCellX.reserve(row.size());
  // Border lines running through a colspan cell are suppressed.
  rowBoundarySkipped.assign(numCols + 1, false);

  size_t col = 0;
  size_t rowLines = 1;  // empty rows still get one line of height
  for (auto& cell : row) {
    if (col >= numCols) break;
    const size_t span = std::min<size_t>(std::max<uint8_t>(cell.colSpan, 1), numCols - col);
    int contentWidth = -2 * CELL_PAD_X;
    for (size_t c = col; c < col + span; ++c) {
      contentWidth += columnWidths[c] + 2 * CELL_PAD_X;
      if (c > col) contentWidth += BORDER;
    }
    if (contentWidth < 1) contentWidth = 1;
    for (size_t b = col + 1; b < col + span; ++b) {
      rowBoundarySkipped[b] = true;
    }

    rowCellX.push_back(static_cast<int16_t>(boundaryXs[col] + BORDER + CELL_PAD_X));
    rowCellLines.emplace_back();
    auto& lines = rowCellLines.back();
    if (cell.text && !cell.text->isEmpty()) {
      // All of a row's lines are alive at once below, so bound the row's total
      // words; spanning cells get a proportionally larger share.
      const size_t cellBudget = std::max<size_t>(1, ROW_WORD_BUDGET * span / numCols);
      if (cell.text->size() > cellBudget) {
        LOG_DBG("TBL", "Truncating table cell to %u words", static_cast<unsigned>(cellBudget));
        cell.text->truncateWords(cellBudget);
        cell.text->addWord(TRUNCATION_ELLIPSIS, EpdFontFamily::REGULAR);
      }
      lines.reserve(cell.text->size());
      cell.text->layoutAndExtractLines(renderer, fontId, static_cast<uint16_t>(contentWidth),
                                       [&lines](const std::shared_ptr<TextBlock>& line) { lines.push_back(line); });
    }
    cell.text.reset();  // words are now held by the lines; free the source early
    rowLines = std::max(rowLines, lines.size());
    col += span;
  }

  // Vertical border x positions relevant for this row.
  rowBoundaries.reserve(numCols + 1);
  for (size_t b = 0; b <= numCols; ++b) {
    if (!rowBoundarySkipped[b]) {
      rowBoundaries.push_back(boundaryXs[b]);
    }
  }

  return rowLines;
}

void TableLayout::emitRowColumns(Row& row, TablePageSink& sink) {
  const int16_t pageH = sink.pageHeight();
  const size_t rowLines = layoutRowCells(row);

  // Pre-flight: make sure the row can open on this page with at least one text
  // line plus its closing padding and border; otherwise start a fresh page.
  if (topBorderPending && sink.currentY() > 0 && sink.currentY() + lineHeight / 2 <= pageH) {
    sink.advanceY(lineHeight / 2);  // breathing room above the table
  }
  {
    const int16_t y = sink.currentY();
    const auto opening =
        static_cast<int16_t>((topBorderPending ? BORDER : 0) + CELL_PAD_Y + lineHeight + CELL_PAD_Y + BORDER);
    if (y > 0 && y + opening > pageH) {
      sink.completePage();
    }
  }

  size_t emitted = 0;
  bool firstSegment = true;
  while (true) {
    if (topBorderPending) {
      addRect(sink, tableX, sink.currentY(), totalWidth, BORDER);
      sink.advanceY(BORDER);
      topBorderPending = false;
    }
    const int16_t segTop = sink.currentY();
    const int16_t contentY = static_cast<int16_t>(segTop + (firstSegment ? CELL_PAD_Y : 0));
    const size_t remaining = rowLines - emitted;
    const int spaceNoClose = pageH - contentY;
    const int spaceWithClose = spaceNoClose - CELL_PAD_Y - BORDER;
    const size_t fitWithClose = spaceWithClose > 0 ? static_cast<size_t>(spaceWithClose / lineHeight) : 0;

    size_t fit;
    bool closing;
    if (remaining <= fitWithClose) {
      fit = remaining;
      closing = true;
    } else {
      fit = spaceNoClose > 0 ? static_cast<size_t>(spaceNoClose / lineHeight) : 0;
      if (fit >= remaining) fit = remaining - 1;  // keep at least one line for the closing segment
      closing = false;
    }
    if (fit == 0) {
      // Degenerate page (shorter than one text line): emit a single line and
      // drop the rest of the row rather than looping or overflowing the
      // int16 geometry below with an unbounded forced fit.
      fit = 1;
      closing = true;
    }

    for (size_t i = 0; i < rowCellLines.size(); ++i) {
      const auto& lines = rowCellLines[i];
      const size_t end = std::min(lines.size(), emitted + fit);
      for (size_t k = emitted; k < end; ++k) {
        auto line = std::shared_ptr<PageLine>(new (std::nothrow) PageLine(
            lines[k], rowCellX[i], static_cast<int16_t>(contentY + (k - emitted) * lineHeight)));
        if (!line) {
          LOG_ERR("TBL", "OOM: PageLine");
          continue;
        }
        sink.addElement(std::move(line));
      }
    }
    emittedAnything = true;

    const auto contentBottom = static_cast<int16_t>(contentY + fit * lineHeight);
    if (closing) {
      const auto rowBottom = static_cast<int16_t>(contentBottom + CELL_PAD_Y);
      for (const auto bx : rowBoundaries) {
        addRect(sink, bx, segTop, BORDER, static_cast<uint16_t>(rowBottom + BORDER - segTop));
      }
      addRect(sink, tableX, rowBottom, totalWidth, BORDER);
      sink.advanceY(static_cast<int16_t>(rowBottom + BORDER - sink.currentY()));
      break;
    }

    // Row continues on the next page: extend this segment's verticals to the
    // page edge so the grid reads as continuing.
    for (const auto bx : rowBoundaries) {
      addRect(sink, bx, segTop, BORDER, static_cast<uint16_t>(pageH - segTop));
    }
    sink.completePage();
    emitted += fit;
    firstSegment = false;
  }

  // Release this row's line references promptly (the emitted PageLines hold
  // their own); capacity is retained for the next row.
  rowCellLines.clear();
}

void TableLayout::emitRowStacked(Row& row, TablePageSink& sink) {
  const int16_t pageH = sink.pageHeight();
  const uint16_t contentWidth =
      availableWidth > 2 * CELL_PAD_X ? static_cast<uint16_t>(availableWidth - 2 * CELL_PAD_X) : availableWidth;
  const auto contentX = static_cast<int16_t>(originX + CELL_PAD_X);

  bool emittedCell = false;
  for (auto& cell : row) {
    if (!cell.text || cell.text->isEmpty()) {
      cell.text.reset();
      continue;
    }

    if (topBorderPending) {
      // Open the table block with a gap and a full-width separator.
      if (sink.currentY() > 0) {
        if (sink.currentY() + lineHeight / 2 + BORDER + lineHeight > pageH) {
          sink.completePage();
        } else {
          sink.advanceY(lineHeight / 2);
        }
      }
      addRect(sink, originX, sink.currentY(), availableWidth, BORDER);
      sink.advanceY(static_cast<int16_t>(BORDER + lineHeight / 4));
      topBorderPending = false;
    }

    cell.text->layoutAndExtractLines(renderer, fontId, contentWidth, [&](const std::shared_ptr<TextBlock>& line) {
      if (sink.currentY() + lineHeight > pageH) {
        sink.completePage();
      }
      auto pageLine = std::shared_ptr<PageLine>(new (std::nothrow) PageLine(line, contentX, sink.currentY()));
      if (!pageLine) {
        LOG_ERR("TBL", "OOM: PageLine");
        return;
      }
      sink.addElement(std::move(pageLine));
      sink.advanceY(lineHeight);
      emittedCell = true;
    });
    cell.text.reset();
  }

  if (!emittedCell) {
    return;
  }
  emittedAnything = true;

  // Thin separator under the row (skipped when the page break already separates).
  const auto sepGap = static_cast<int16_t>(lineHeight / 4);
  if (sink.currentY() > 0 && sink.currentY() + 2 * sepGap + BORDER <= pageH) {
    sink.advanceY(sepGap);
    addRect(sink, originX, sink.currentY(), availableWidth, BORDER);
    sink.advanceY(static_cast<int16_t>(BORDER + sepGap));
  }
}
