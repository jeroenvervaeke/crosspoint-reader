#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ParsedText.h"

class GfxRenderer;
class PageElement;

// Narrow interface the chapter parser implements so table layout can place
// elements on pages (and break pages) without seeing parser internals.
// TableLayout only ever holds a sink by reference for the duration of a call,
// never owns one, so the destructor is protected and non-virtual.
class TablePageSink {
 public:
  virtual int16_t currentY() const = 0;
  virtual int16_t pageHeight() const = 0;
  // Flush the current page (if it has content) and start a fresh one at y = 0.
  virtual void completePage() = 0;
  virtual void addElement(std::shared_ptr<PageElement> element) = 0;
  virtual void advanceY(int16_t dy) = 0;

 protected:
  ~TablePageSink() = default;
};

// Lays out <table> content as a real grid with drawn cell borders.
//
// Memory strategy (the ~380KB heap is the hard constraint): only a small
// sample of leading rows is buffered to derive column widths from content;
// after that, every row is laid out and flushed to pages as soon as its
// </tr> closes, so peak RAM stays bounded no matter how long the table is.
// Per-cell word count is capped by the parser via MAX_WORDS_PER_CELL, and
// grid rows are additionally budgeted to ROW_WORD_BUDGET words in total so a
// full row's laid-out lines (which must be materialized together to compute
// the row height) stay bounded as well.
//
// When the table has more columns than the viewport can legibly hold (or is
// single-column), it degrades to a stacked layout: each row's cells rendered
// as consecutive full-width paragraphs with a thin separator between rows.
class TableLayout {
 public:
  static constexpr size_t MAX_COLS = 8;
  static constexpr size_t MAX_WORDS_PER_CELL = 100;

  TableLayout(const GfxRenderer& renderer, int fontId, float lineCompression, int16_t originX, uint16_t availableWidth);

  // Presentation policy for a cell's text block: th centered+indent-free,
  // td left-aligned, CSS text-align/dir honored when present.
  static BlockStyle cellBlockStyle(bool isHeaderCell, const CssStyle& cssStyle);

  void startRow();
  // Returns false when the cell must be dropped (row column cap reached);
  // the parser then discards the cell's content.
  bool startCell(uint8_t colSpan);
  // Hands ownership of the finished cell's words to the table.
  void endCell(std::unique_ptr<ParsedText> text);
  void endRow(TablePageSink& sink);
  // Called on </table>: flushes anything still buffered and closes the grid.
  void finish(TablePageSink& sink);

 private:
  static constexpr int16_t BORDER = 1;
  static constexpr int16_t CELL_PAD_X = 4;
  static constexpr int16_t CELL_PAD_Y = 3;
  // Column widths are derived from the first few rows only, so an arbitrarily
  // long table never buffers more than this sample.
  static constexpr size_t SAMPLE_ROW_LIMIT = 6;
  static constexpr size_t SAMPLE_WORD_LIMIT = 600;
  // Cap on words per grid row across all cells. A row's lines exist together
  // in RAM during emitRowColumns (each line is a TextBlock of ~200 bytes), so
  // this bounds the per-row transient to roughly 60KB worst case.
  static constexpr size_t ROW_WORD_BUDGET = 320;

  struct Cell {
    std::unique_ptr<ParsedText> text;
    uint8_t colSpan = 1;
  };
  using Row = std::vector<Cell>;

  const GfxRenderer& renderer;
  const int fontId;
  const int16_t lineHeight;
  const int16_t originX;
  const uint16_t availableWidth;

  std::vector<Row> bufferedRows;
  Row currentRow;
  bool rowOpen = false;
  bool cellOpen = false;
  uint8_t pendingColSpan = 1;
  size_t currentRowCols = 0;
  size_t sampledWords = 0;

  bool layoutDecided = false;
  bool useColumns = false;
  std::vector<uint16_t> columnWidths;
  std::vector<int16_t> boundaryXs;  // x of each vertical border line, size numCols + 1
  int16_t tableX = 0;
  uint16_t totalWidth = 0;
  bool topBorderPending = true;
  bool emittedAnything = false;

  // Per-row scratch, reused across rows to avoid per-row heap churn
  // (capacity is retained by clear()).
  std::vector<std::vector<std::shared_ptr<TextBlock>>> rowCellLines;
  std::vector<int16_t> rowCellX;
  std::vector<bool> rowBoundarySkipped;
  std::vector<int16_t> rowBoundaries;

  void decideLayout();
  void flushBufferedRows(TablePageSink& sink);
  void emitRow(Row& row, TablePageSink& sink);
  // Lays every cell of the row out into rowCellLines/rowCellX and fills
  // rowBoundaries; returns the row's line count (>= 1).
  size_t layoutRowCells(Row& row);
  void emitRowColumns(Row& row, TablePageSink& sink);
  void emitRowStacked(Row& row, TablePageSink& sink);
  void addRect(TablePageSink& sink, int16_t x, int16_t y, uint16_t w, uint16_t h) const;
};
