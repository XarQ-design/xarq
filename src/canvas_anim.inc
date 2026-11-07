// Animation: frames are the document's layers, except those marked as frame backgrounds, which show behind
// every frame. Included at the bottom of canvas.cpp.

int Canvas::currentFrame() const {
    const QVector<int> fr = m_doc.frameLayers();
    const int at = fr.indexOf(m_activeLayer);
    if(at >= 0) return at;
    for(int k = 0; k < fr.size(); ++k) if(m_doc.layers[fr[k]].visible) return k;
    return fr.isEmpty() ? -1 : 0;
}

void Canvas::showFrame(int f, bool quiet){
    const QVector<int> fr = m_doc.frameLayers();
    if(f < 0 || f >= fr.size()) return;
    for(int i = 0; i < m_doc.layers.size(); ++i) if(!m_doc.layers[i].frameBackground) m_doc.layers[i].visible = (i == fr[f]);
    m_activeLayer = fr[f];
    if(!quiet){
        QVector<int> keep; for(int s : m_sel) if(s >= 0 && s < m_doc.shapes.size() && m_doc.shapes[s].layer == fr[f]) keep << s;
        if(keep.size() != m_sel.size()){ m_sel = keep; emit selectionChanged(); }
        emit documentChanged();
    }
    m_cacheDirty = true; update();
}

// Reorder the layers: `order` lists the old layer indices in their new order.
void Canvas::permuteLayers(const QVector<int>& order){
    const int n = m_doc.layers.size();
    if(order.size() != n) return;
    QVector<int> inv(n, -1);
    for(int k = 0; k < n; ++k){ if(order[k] < 0 || order[k] >= n || inv[order[k]] >= 0) return; inv[order[k]] = k; }
    QVector<Layer> ls; for(int k : order) ls << m_doc.layers[k];
    m_doc.layers = ls;
    for(Shape& s : m_doc.shapes) s.layer = inv[qBound(0, s.layer, n - 1)];
    m_activeLayer = inv[qBound(0, m_activeLayer, n - 1)];
}

int Canvas::insertFrame(int after, bool duplicate){
    const QVector<int> fr = m_doc.frameLayers();
    const int src = (after >= 0 && after < fr.size()) ? fr[after] : (fr.isEmpty() ? -1 : fr.last());
    pushUndo(duplicate ? "Duplicate frame" : "New frame");
    const int at = src + 1 > 0 ? src + 1 : m_doc.layers.size();
    Layer L;
    if(src >= 0){ L.frameDelayMs = m_doc.layers[src].frameDelayMs; if(duplicate){ L = m_doc.layers[src]; L.name += QStringLiteral(" copy"); } }
    if(!duplicate) L.name = QStringLiteral("Frame %1").arg(fr.size() + 1);
    L.frameBackground = false; L.visible = true;
    for(Shape& s : m_doc.shapes) if(s.layer >= at) ++s.layer;
    m_doc.layers.insert(at, L);
    if(duplicate && src >= 0){
        QVector<Shape> copies;
        for(const Shape& s : m_doc.shapes) if(s.layer == src){ Shape c = s; c.layer = at; c.nextFrame = -1; c.anchorTextIndex = -1; copies << c; }
        m_doc.shapes += copies;
    }
    const int f = m_doc.frameLayers().indexOf(at);
    m_sel.clear(); emit selectionChanged();
    showFrame(f);
    return f;
}

void Canvas::deleteFrames(QVector<int> frames){
    const QVector<int> fr = m_doc.frameLayers();
    QVector<int> layers;
    for(int f : frames) if(f >= 0 && f < fr.size() && !layers.contains(fr[f])) layers << fr[f];
    if(layers.isEmpty() || layers.size() >= m_doc.layers.size()) return;     // a document keeps at least one layer
    std::sort(layers.begin(), layers.end(), std::greater<int>());
    invalidateMould(); resetTextEdit(); pushUndo(layers.size() > 1 ? "Delete frames" : "Delete frame");
    for(int L : layers){
        for(int k = m_doc.shapes.size() - 1; k >= 0; --k){ if(m_doc.shapes[k].layer == L) m_doc.shapes.remove(k); else if(m_doc.shapes[k].layer > L) m_doc.shapes[k].layer--; }
        m_doc.layers.remove(L);
    }
    m_sel.clear(); emit selectionChanged();
    const int left = m_doc.frameLayers().size();
    m_activeLayer = qBound(0, m_activeLayer, m_doc.layers.size() - 1);
    if(left > 0) showFrame(qBound(0, frames.isEmpty() ? 0 : *std::min_element(frames.begin(), frames.end()), left - 1));
    else { emit documentChanged(); update(); }
}

void Canvas::moveFrame(int from, int to){
    const QVector<int> fr = m_doc.frameLayers();
    if(from < 0 || from >= fr.size() || to < 0 || to >= fr.size() || from == to) return;
    pushUndo("Move frame");
    QVector<int> order; for(int i = 0; i < m_doc.layers.size(); ++i) order << i;
    const int a = fr[from], b = fr[to];
    order.removeOne(a);
    const int at = order.indexOf(b);
    order.insert(to > from ? at + 1 : at, a);
    permuteLayers(order);
    showFrame(to);
}

void Canvas::setFrameDelay(const QVector<int>& frames, int ms){
    const QVector<int> fr = m_doc.frameLayers();
    bool any = false;
    for(int f : frames) if(f >= 0 && f < fr.size() && m_doc.layers[fr[f]].frameDelayMs != ms){
        if(!any){ pushUndo("Frame duration"); any = true; }
        m_doc.layers[fr[f]].frameDelayMs = qBound(0, ms, 600000);
    }
    if(any) emit documentChanged();
}

void Canvas::setFrameBackground(int layer, bool on){
    if(layer < 0 || layer >= m_doc.layers.size() || m_doc.layers[layer].frameBackground == on) return;
    if(on && m_doc.frameLayers().size() <= 1) return;                       // keep at least one frame
    pushUndo(on ? "Frame background" : "Make a frame");
    m_doc.layers[layer].frameBackground = on;
    if(on) m_doc.layers[layer].visible = true;
    emit documentChanged(); m_cacheDirty = true; update();
}

void Canvas::setFrameTags(const QVector<FrameTag>& tags){ pushUndo("Frame tags"); m_doc.frameTags = tags; emit documentChanged(); }
void Canvas::setAnimationLoops(int loops){ if(m_doc.animLoops == loops) return; pushUndo("Animation loops"); m_doc.animLoops = qMax(0, loops); emit documentChanged(); }

// The document as frame f shows it: that frame plus the (visible) backgrounds.
static Document frameDocument(const Document& src, int layer){
    Document d = src;
    for(int i = 0; i < d.layers.size(); ++i) d.layers[i].visible = d.layers[i].frameBackground ? src.layers[i].visible : (i == layer);
    return d;
}

QImage Canvas::renderFrame(int f, const QSize& box, bool white) const {
    const QVector<int> fr = m_doc.frameLayers();
    if(f < 0 || f >= fr.size()) return QImage();
    const Document base = (hasTextFields() || hasTextChains()) ? resolvedForRender() : m_doc;
    const Document d = frameDocument(base, fr[f]);
    const QRectF page = m_doc.pageRect(0);
    double sc = 1.0;
    if(box.width() > 0 && box.height() > 0) sc = qMin(box.width() / page.width(), box.height() / page.height());
    const QSize sz(qMax(1, qRound(page.width() * sc)), qMax(1, qRound(page.height() * sc)));
    if(qint64(sz.width()) * sz.height() > (qint64(1) << 28)) return QImage();
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(white ? QColor(Qt::white) : QColor(Qt::transparent));
    QPainter g(&img);
    g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    g.scale(sc, sc); g.translate(-page.topLeft());
    paintDocument(g, d);
    g.end();
    return img;
}

void Canvas::setOnionSkin(int before, int after, double opacity){
    m_onionBefore = qBound(0, before, 5); m_onionAfter = qBound(0, after, 5); m_onionOpacity = qBound(0.05, opacity, 1.0);
    m_cacheDirty = true; update();
}

// Neighbouring frames, faded and tinted (red before, blue after), under the frame being edited.
void Canvas::paintOnionSkin(QPainter& lp, const QSize& devSize, qreal dpr){
    if(m_onionBefore <= 0 && m_onionAfter <= 0) return;
    const QVector<int> fr = m_doc.frameLayers();
    const int cur = currentFrame();
    if(cur < 0 || fr.size() < 2) return;
    for(int dir : {-1, 1}) for(int k = 1; k <= (dir < 0 ? m_onionBefore : m_onionAfter); ++k){
        const int f = cur + dir * k;
        if(f < 0 || f >= fr.size()) continue;
        Document d = m_doc;
        for(int i = 0; i < d.layers.size(); ++i) d.layers[i].visible = (i == fr[f]);
        QImage tmp(devSize, QImage::Format_ARGB32_Premultiplied); tmp.setDevicePixelRatio(dpr); tmp.fill(Qt::transparent);
        { QPainter tp(&tmp); tp.setRenderHint(QPainter::Antialiasing); tp.setRenderHint(QPainter::SmoothPixmapTransform);
          tp.setTransform(lp.transform()); paintDocument(tp, d);
          tp.resetTransform(); tp.setCompositionMode(QPainter::CompositionMode_SourceAtop);
          tp.fillRect(QRectF(QPointF(0, 0), QSizeF(devSize) / dpr), dir < 0 ? QColor(230, 60, 60, 110) : QColor(60, 110, 230, 110)); }
        lp.save(); lp.resetTransform(); lp.setOpacity(m_onionOpacity * std::pow(0.6, k - 1)); lp.drawImage(QPointF(0, 0), tmp); lp.restore();
    }
}

bool Canvas::exportAnimation(const QString& path, int format, double scale, int from, int to, bool transparent){
    const QVector<int> fr = m_doc.frameLayers();
    if(fr.isEmpty()) return false;
    from = qBound(0, from, fr.size() - 1); to = to < 0 ? fr.size() - 1 : qBound(from, to, fr.size() - 1);
    const QRectF page = m_doc.pageRect(0);
    scale = qBound(0.05, scale, 64.0);
    const QSize box(qMax(1, qRound(page.width() * scale)), qMax(1, qRound(page.height() * scale)));
    QVector<QImage> frames; QVector<int> delays;
    for(int f = from; f <= to; ++f){
        QImage img = renderFrame(f, box, !transparent);
        if(img.isNull()) return false;
        frames << img.convertToFormat(QImage::Format_ARGB32);
        const int d = m_doc.layers[fr[f]].frameDelayMs;
        delays << (d > 0 ? d : 100);
    }
    switch(format){
    case 0: return writeAnimatedGif(path, frames, 100, m_doc.animLoops, delays);
    case 1: return writeApng(path, frames, delays, m_doc.animLoops);
    case 2: {                                                              // name.png → name_001.png, name_002.png, …
        const QFileInfo fi(path); const QString stem = fi.path() + QLatin1Char('/') + fi.completeBaseName();
        const int digits = qMax(3, int(QString::number(frames.size()).size()));
        for(int k = 0; k < frames.size(); ++k)
            if(!frames[k].save(QStringLiteral("%1_%2.png").arg(stem).arg(k + 1, digits, 10, QLatin1Char('0')), "PNG")) return false;
        return true;
    }
    case 3: {                                                              // sprite sheet + a JSON frame list beside it
        const int n = frames.size(), cols = qMax(1, int(std::ceil(std::sqrt(double(n))))), rows = (n + cols - 1) / cols;
        const int fw = frames[0].width(), fh = frames[0].height();
        if(qint64(fw) * cols * fh * rows > (qint64(1) << 28)) return false;
        QImage sheet(fw * cols, fh * rows, QImage::Format_ARGB32); sheet.fill(Qt::transparent);
        QJsonArray list;
        { QPainter g(&sheet);
          for(int k = 0; k < n; ++k){
              const int x = (k % cols) * fw, y = (k / cols) * fh;
              g.drawImage(x, y, frames[k]);
              list.append(QJsonObject{{"frame", QJsonObject{{"x",x},{"y",y},{"w",fw},{"h",fh}}}, {"duration", delays[k]}});
          } }
        if(!sheet.save(path, "PNG")) return false;
        QJsonArray tags;
        for(const FrameTag& t : m_doc.frameTags)
            tags.append(QJsonObject{{"name",t.name},{"from",t.from},{"to",t.to},{"direction", t.direction == 1 ? "reverse" : t.direction == 2 ? "pingpong" : "forward"}});
        const QJsonObject meta{{"image", QFileInfo(path).fileName()}, {"size", QJsonObject{{"w",sheet.width()},{"h",sheet.height()}}}, {"frameTags", tags}};
        QFile jf(QFileInfo(path).path() + QLatin1Char('/') + QFileInfo(path).completeBaseName() + QStringLiteral(".json"));
        if(!jf.open(QIODevice::WriteOnly)) return false;
        jf.write(QJsonDocument(QJsonObject{{"frames", list}, {"meta", meta}}).toJson());
        return true;
    }
    }
    return false;
}

bool Canvas::importAni(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    FrameSet fs;
    if(!decodeAni(f.readAll(), &fs)){ emit statusMessage("No readable frames found in the animated cursor."); return false; }
    adoptFrames(fs.frames, fs.delays, fs.names, true, "Import animated cursor");
    emit statusMessage(QStringLiteral("Imported %1: %2 frame(s)%3.").arg(fs.title.isEmpty() ? QFileInfo(path).fileName() : fs.title).arg(fs.frames.size())
                       .arg(fs.author.isEmpty() ? QString() : QStringLiteral(" by ") + fs.author));
    return true;
}
bool Canvas::importMpo(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    FrameSet fs;
    if(!decodeMpo(f.readAll(), &fs)){ f.close(); return importImage(path); }      // a single picture: an ordinary JPEG
    adoptFrames(fs.frames, fs.delays, fs.names, false, "Import multi-picture JPEG");
    emit statusMessage(QStringLiteral("Imported %1 pictures, one per frame; play them for a stereo wiggle.").arg(fs.frames.size()));
    return true;
}

// An imported animation replaces the document: one frame (layer) per image, on a page the size of the
// frames, placed 1:1. Pixel art keeps hard pixels when zoomed.
void Canvas::adoptFrames(const QVector<QImage>& frames, const QVector<int>& delays, const QStringList& names, bool pixelated, const QString& undoLabel){
    if(frames.isEmpty()) return;
    pushUndo(undoLabel);
    const double W = frames[0].width(), H = frames[0].height();
    m_doc.layers.clear(); m_doc.shapes.clear(); m_doc.frameTags.clear();
    m_doc.pageSize = QSizeF(W, H); m_doc.pageCount = 1;
    for(int i = 0; i < frames.size(); ++i){
        Layer L; L.name = i < names.size() ? names[i] : QStringLiteral("Frame %1").arg(i + 1); L.visible = (i == 0); L.frameDelayMs = delays.value(i, 100);
        m_doc.layers.push_back(L);
        const QImage& f = frames[i];
        Shape s; s.type = ShapeType::Rect; s.rect = QRectF(0, 0, f.width(), f.height()); s.name = L.name; s.layer = i;
        s.fill.type = FillType::Bitmap; s.fill.image = f; s.fill.origImage = f; s.fill.tiled = false; s.fill.pixelated = pixelated;
        s.fill.p0 = QPointF(0, f.height()); s.fill.p1 = QPointF(f.width(), f.height()); s.fill.p2 = QPointF(0, 0);
        s.stroke.enabled = false; m_doc.shapes.push_back(s);
    }
    m_activeLayer = 0;
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); zoomFit(); update();
}

void Canvas::adoptAnimation(const Document& anim, const QString& undoLabel){
    pushUndo(undoLabel);
    m_doc.layers = anim.layers.isEmpty() ? QVector<Layer>{ Layer{} } : anim.layers;
    m_doc.shapes = anim.shapes; m_doc.pageSize = anim.pageSize; m_doc.pageCount = 1; m_doc.frameTags = anim.frameTags;
    m_activeLayer = 0;
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); zoomFit(); update();
}

bool Canvas::importAse(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    AseImportStats st;
    const Document pd = importAseDocument(f.readAll(), &st);
    if(pd.layers.isEmpty() || pd.shapes.isEmpty()){ emit statusMessage("No readable frames found in the Aseprite file."); return false; }
    adoptAnimation(pd, "Import Aseprite");
    QStringList notes;
    if(st.badCels) notes << QStringLiteral("%1 cel(s) couldn't be read").arg(st.badCels);
    if(st.unknownModes) notes << QStringLiteral("%1 blend mode(s) drawn as Normal").arg(st.unknownModes);
    if(st.referenceLayers) notes << QStringLiteral("%1 reference layer(s) left out").arg(st.referenceLayers);
    emit statusMessage(QStringLiteral("Imported Aseprite (%1): %2 frame(s), %3 layer(s)%4.").arg(st.colorMode).arg(st.frames).arg(st.layers)
                       .arg(notes.isEmpty() ? QString() : QStringLiteral(" — ") + notes.join(QStringLiteral(", "))));
    return true;
}
