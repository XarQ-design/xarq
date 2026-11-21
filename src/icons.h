// icons.h — crisp vector tool icons drawn at runtime with QPainter (no asset
// files). One clean line-art glyph per tool, in the app's monochrome style.
#pragma once
#include <QIcon>
#include <functional>
#include "canvas.h"
namespace xara {
QIcon toolIcon(Tool t, int size=22, const QColor& col=QColor(220,223,230));
QIcon actionIcon(const QString& name, int size=20, const QColor& col=QColor(210,214,222));
// actionIcon in `col` as given, in either look (an icon on a piece whose colour the light look sets by hand).
QIcon fixedActionIcon(const QString& name, int size, const QColor& col);
// An icon drawn by `draw` in `col` (a dark-look colour), redrawn in the look current when it is painted.
QIcon themedIcon(std::function<QIcon(const QColor&)> draw, const QColor& col);
} // namespace xara
