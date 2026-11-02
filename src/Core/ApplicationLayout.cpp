// Editor group layouts: a grid of cols x rows panes (Sublime's Columns, Rows and Grid layouts).
#include "Core/Application.h"
#include <algorithm>

#ifdef None
#undef None
#endif

// Groups fill the grid row by row. A group count that no longer matches the chosen grid (a group was added or closed)
// falls back to one row of columns.
void Application::layoutGrid(size_t groupCount, size_t& cols, size_t& rows) const {
    if (layoutCols_ > 0 && layoutCols_ * layoutRows_ == groupCount) { cols = layoutCols_; rows = layoutRows_; return; }
    cols = std::max<size_t>(1, groupCount);
    rows = 1;
}

Application::PaneRect Application::paneRectFor(size_t group, size_t groupCount, float areaLeft, float areaRight, float areaTop, float areaBottom) const {
    size_t cols = 1, rows = 1;
    layoutGrid(groupCount, cols, rows);
    size_t col = group % cols, row = group / cols;
    float w = (areaRight - areaLeft) / static_cast<float>(cols), h = (areaBottom - areaTop) / static_cast<float>(rows);
    PaneRect r;
    r.l = areaLeft + w * static_cast<float>(col);
    r.r = col + 1 == cols ? areaRight : r.l + w;
    r.t = areaTop + h * static_cast<float>(row);
    r.b = row + 1 == rows ? areaBottom : r.t + h;
    return r;
}

void Application::setLayoutGrid(size_t cols, size_t rows) {
    cols = std::clamp<size_t>(cols, 1, 4);
    rows = std::clamp<size_t>(rows, 1, 4);
    setLayoutColumns(cols * rows);
    layoutCols_ = cols * rows > 1 ? cols : 0;
    layoutRows_ = rows;
}
