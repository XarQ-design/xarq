// gif.h — a small self-contained animated GIF89a encoder (web-216 palette + LZW),
// used to export Xara-style frame animations (frames = layers).
#pragma once
#include <QImage>
#include <QVector>
#include <QString>

namespace xara {
// Write an animated GIF from `frames`: with their own colours when the whole animation uses 255 or fewer,
// else mapped to the 216-colour web palette; pixels less than half opaque become transparent.
// delayMs = default time per frame; loops = repeat count (0 = forever).
// perFrameMs (if sized to frames) overrides delayMs per frame. Returns false on I/O failure.
bool writeAnimatedGif(const QString& path, const QVector<QImage>& frames, int delayMs,
                      int loops = 0, const QVector<int>& perFrameMs = QVector<int>());
// Write an animated PNG, keeping full colour and alpha. delaysMs per frame (100 where missing); loops 0 = forever.
bool writeApng(const QString& path, const QVector<QImage>& frames, const QVector<int>& delaysMs, int loops = 0);
}
