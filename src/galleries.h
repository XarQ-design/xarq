// galleries.h — Xara's signature dockable "galleries". Each observes the Canvas
// document/selection and applies presets back through it.
#pragma once
#include <QWidget>
#include <QTreeWidget>

class QLineEdit; class QListWidget; class QTableWidget; class QLabel; class QTimer;
class QSpinBox; class QCheckBox; class QComboBox; class QToolButton; class QSlider; class QDoubleSpinBox;

namespace xara {
class Canvas;

// Named/theme/PANTONE colours: swatches you click to fill (right-click = line),
// create named colours, derive tints/shades, and apply theme schemes.
class ColorGallery : public QWidget {
    Q_OBJECT
public:
    explicit ColorGallery(Canvas* c, QWidget* parent=nullptr);
public slots:
    void refresh();
private:
    void rebuild();
    Canvas* m_c;
    QWidget* m_host=nullptr;
    int m_sel=-1;
};

// Preset gradient/flat fills — click a swatch to apply to the selection.
class FillGallery : public QWidget {
    Q_OBJECT
public:
    explicit FillGallery(Canvas* c, QWidget* parent=nullptr);
private:
    Canvas* m_c;
};

// Stroke presets: widths, dash patterns, arrowheads, simple brush styles.
class LineGallery : public QWidget {
    Q_OBJECT
public:
    explicit LineGallery(Canvas* c, QWidget* parent=nullptr);
private:
    Canvas* m_c;
};

// Installed fonts with a live preview — applies to selected text.
class FontsGallery : public QWidget {
    Q_OBJECT
public:
    explicit FontsGallery(Canvas* c, QWidget* parent=nullptr);
private:
    Canvas* m_c; QLineEdit* m_filter=nullptr; QListWidget* m_list=nullptr;
    void populate(const QString& filter);
};

// Object names — list/rename every object, click to select.
class NameGallery : public QWidget {
    Q_OBJECT
public:
    explicit NameGallery(Canvas* c, QWidget* parent=nullptr);
public slots:
    void refresh();
private:
    Canvas* m_c; QTableWidget* m_table=nullptr; bool m_block=false;
};

// Built-in clipart — click to insert a ready-made shape (star, heart, arrow, …).
class ClipartGallery : public QWidget {
    Q_OBJECT
public:
    explicit ClipartGallery(Canvas* c, QWidget* parent=nullptr);
private:
    Canvas* m_c;
};

// A QTreeWidget that reports internal drag-drops so the gallery can re-derive object
// stacking/layer membership from the post-drop tree. (Defined here so moc sees it.)
class LayerTree : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;
signals:
    void reordered();
protected:
    void dropEvent(QDropEvent* e) override;
};

// Page & Layer gallery — a hierarchical TREE: layers ▸ objects ▸ group children, each row
// with visibility / lock toggles, inline rename, click-to-select, and drag-to-restack
// (within a layer or across layers). Layer visibility / lock / solo / master + new/delete/
// reorder buttons as before (manual p177-188).
class PageLayerGallery : public QWidget {
    Q_OBJECT
public:
    explicit PageLayerGallery(Canvas* c, QWidget* parent=nullptr);
public slots:
    void refresh();
private:
    void rebuild();
    void applyTreeOrder();                 // read the tree after a drop → Canvas::setObjectOrder
    void syncSelectionToCanvas();          // tree selection → Canvas::select / setActiveLayer
    Canvas* m_c; LayerTree* m_tree=nullptr; bool m_block=false;
};

// SmartShapes — parametric shapes (chevron, gear, banner, callout …) inserted with a
// live "complexity"/count parameter (manual p154-161).
class SmartShapeGallery : public QWidget {
    Q_OBJECT
public:
    explicit SmartShapeGallery(Canvas* c, QWidget* parent=nullptr);
private:
    Canvas* m_c; int m_param=6;
};

// Bitmap gallery — thumbnails of every embedded bitmap in the document (manual p331).
class BitmapGallery : public QWidget {
    Q_OBJECT
public:
    explicit BitmapGallery(Canvas* c, QWidget* parent=nullptr);
public slots:
    void refresh();
private:
    Canvas* m_c; QWidget* m_host=nullptr;
};

// Animation — frames are layers; play preview + animated-GIF export (manual p519-541).
// The Animation panel: frames are layers. A strip of frames to pick, reorder, add, duplicate and delete, their
// durations, playback (whole animation or a tagged range, forward / reverse / ping-pong, looping), onion skin,
// layers shown behind every frame, named frame tags, and export (GIF, APNG, numbered PNGs, sprite sheet).
class AnimationPanel : public QWidget {
    Q_OBJECT
public:
    explicit AnimationPanel(Canvas* c, QWidget* parent=nullptr);
private:
    void rebuild();                 // the frame, background and tag lists from the document
    void renderThumbs();
    void play(); void stop(); void step();
    void syncControls();            // duration and background box for the selected frames
    QVector<int> selectedFrames() const;
    QVector<int> playSequence() const;
    Canvas* m_c;
    QListWidget* m_frames=nullptr; QListWidget* m_backs=nullptr; QListWidget* m_tags=nullptr;
    QSpinBox* m_delay=nullptr; QSpinBox* m_loops=nullptr; QSpinBox* m_onionBefore=nullptr; QSpinBox* m_onionAfter=nullptr;
    QSlider* m_onionOpacity=nullptr; QCheckBox* m_onion=nullptr; QCheckBox* m_transparent=nullptr;
    QComboBox* m_range=nullptr; QComboBox* m_format=nullptr; QComboBox* m_tagDir=nullptr; QDoubleSpinBox* m_scale=nullptr;
    QToolButton* m_playBtn=nullptr; QToolButton* m_loopBtn=nullptr; QLabel* m_info=nullptr;
    QTimer* m_timer=nullptr; QTimer* m_thumbTimer=nullptr;
    QVector<int> m_seq; int m_seqPos=0, m_restore=-1; bool m_playing=false, m_building=false;
};

} // namespace xara
