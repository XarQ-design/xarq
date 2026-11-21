// galleries.cpp — the gallery docks. Batch B: the Colour Gallery (named/theme/
// tint/shade/linked + PANTONE).
#include "galleries.h"
#include "theme.h"
#include "dialogs.h"
#include "canvas.h"
#include <QtWidgets>

namespace xara {

// A clickable colour swatch: left = fill, right = line, double-click = edit.
class Swatch : public QWidget {
public:
    std::function<void(Qt::MouseButton)> onClick;
    std::function<void()> onEdit;
    Swatch(const QColor& c,const QString& tip,bool sel):m_c(c){ setFixedSize(26,26); setToolTip(tip); m_sel=sel; setCursor(Qt::PointingHandCursor); }
protected:
    void paintEvent(QPaintEvent*) override { QPainter g(this); g.setRenderHint(QPainter::Antialiasing); QRectF r=rect().adjusted(1,1,-1,-1);
        g.setPen(Qt::NoPen); for(int y=0;y<r.height();y+=5)for(int x=0;x<r.width();x+=5){ g.setBrush(((x/5+y/5)&1)?QColor(90,90,96):QColor(64,64,70)); g.drawRect(QRectF(r.x()+x,r.y()+y,5,5)); }
        g.setBrush(m_c); g.setPen(QPen(m_sel?QColor(90,170,255):QColor(0,0,0,110),m_sel?2:1)); g.drawRoundedRect(r,4,4); }
    void mousePressEvent(QMouseEvent* e) override { if(onClick) onClick(e->button()); }
    void mouseDoubleClickEvent(QMouseEvent*) override { if(onEdit) onEdit(); }
    QColor m_c; bool m_sel=false;
};

// A compact PANTONE-ish spot set (approximate sRGB) so the gallery shows real names.
static QVector<QPair<QString,QColor>> pantone(){
    return { {"PANTONE 185 C",QColor(228,0,43)}, {"PANTONE 286 C",QColor(0,51,160)},
             {"PANTONE 348 C",QColor(0,132,61)}, {"PANTONE 116 C",QColor(255,205,0)},
             {"PANTONE 165 C",QColor(255,103,31)}, {"PANTONE 2685 C",QColor(86,16,156)},
             {"PANTONE 3262 C",QColor(0,178,169)}, {"PANTONE 219 C",QColor(218,24,132)},
             {"PANTONE Cool Gray 8",QColor(136,139,141)}, {"PANTONE Black 6 C",QColor(16,24,32)},
             {"PANTONE 871 C (Gold)",QColor(133,109,59)}, {"PANTONE 877 C (Silver)",QColor(138,141,143)} };
}

ColorGallery::ColorGallery(Canvas* c, QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(8,8,8,8); V->setSpacing(6);
    m_host=new QWidget; V->addWidget(m_host);
    auto* btns=new QHBoxLayout; V->addLayout(btns);
    auto mkBtn=[&](const QString& t,const QString& tip,std::function<void()> fn){ auto* b=new QToolButton; b->setText(t); b->setToolTip(tip); b->setAutoRaise(true); connect(b,&QToolButton::clicked,this,[fn]{fn();}); btns->addWidget(b); return b; };
    mkBtn("＋ New…","Create a new named colour",[this]{
        QColor col=ColorEditor::pick(this,Qt::white,m_c); if(!col.isValid()) return;
        bool ok; QString name=QInputDialog::getText(this,"New named colour","Name:",QLineEdit::Normal,QString("Colour %1").arg(m_c->doc().namedColors.size()+1),&ok);
        if(!ok||name.isEmpty()) return;
        m_c->pushUndo("New named colour"); NamedColor n; n.name=name; n.color=col; m_c->doc().namedColors.append(n); m_c->refreshAll(); });
    mkBtn("Tint","Add a lighter tint of the selected colour",[this]{ if(m_sel<0) return; m_c->pushUndo("Tint colour");
        NamedColor n; n.kind=1; n.parent=m_sel; n.amount=0.4; n.name=m_c->doc().namedColors[m_sel].name+" tint"; m_c->doc().namedColors.append(n); m_sel=m_c->doc().namedColors.size()-1; m_c->refreshAll(); });
    mkBtn("Shade","Add a darker shade of the selected colour",[this]{ if(m_sel<0) return; m_c->pushUndo("Shade colour");
        NamedColor n; n.kind=2; n.parent=m_sel; n.amount=0.4; n.name=m_c->doc().namedColors[m_sel].name+" shade"; m_c->doc().namedColors.append(n); m_sel=m_c->doc().namedColors.size()-1; m_c->refreshAll(); });
    mkBtn("✕","Delete the selected named colour",[this]{ if(m_sel<0||m_sel>=m_c->doc().namedColors.size()) return; m_c->pushUndo("Delete colour");
        const int del=m_sel; m_c->doc().namedColors.remove(del);
        m_c->doc().reindexNamedColorsAfterDelete(del);   // shift Fill/Stroke.colorRef + tint/shade parent chains past the hole
        m_sel=-1; m_c->refreshAll(); });
    btns->addStretch();

    // theme scheme presets
    auto* themeRow=new QHBoxLayout; V->addLayout(themeRow);
    themeRow->addWidget(new QLabel("Theme:"));
    auto* themeCombo=new QComboBox; themeCombo->addItems({"Choose…","Ocean","Sunset","Forest","Mono","Candy"});
    connect(themeCombo,QOverload<int>::of(&QComboBox::activated),this,[this,themeCombo](int i){ if(i<=0) return;
        struct S{const char* n; QColor c;}; static const QVector<QVector<S>> sch={
            {{"Deep",QColor(18,52,86)},{"Sea",QColor(0,121,150)},{"Sky",QColor(120,200,230)},{"Foam",QColor(220,245,250)},{"Coral",QColor(255,111,97)}},
            {{"Plum",QColor(60,20,60)},{"Berry",QColor(180,40,90)},{"Flame",QColor(255,100,60)},{"Amber",QColor(255,180,70)},{"Cream",QColor(255,235,200)}},
            {{"Bark",QColor(54,40,28)},{"Moss",QColor(80,120,60)},{"Leaf",QColor(120,170,90)},{"Sage",QColor(190,210,160)},{"Bloom",QColor(230,120,150)}},
            {{"Ink",QColor(20,20,24)},{"Slate",QColor(80,84,92)},{"Steel",QColor(140,146,156)},{"Mist",QColor(200,204,210)},{"Paper",QColor(245,246,248)}},
            {{"Bubblegum",QColor(255,110,180)},{"Lemon",QColor(255,225,90)},{"Mint",QColor(120,230,180)},{"Sky",QColor(120,190,255)},{"Grape",QColor(170,110,230)}} };
        const auto& s=sch[i-1]; m_c->pushUndo("Apply theme"); for(auto& e: s){ NamedColor n; n.name=e.n; n.color=e.c; n.theme=true; m_c->doc().namedColors.append(n); }
        themeCombo->setCurrentIndex(0); m_c->refreshAll(); });
    themeRow->addWidget(themeCombo,1);

    connect(m_c,&Canvas::documentChangedDebounced,this,&ColorGallery::refresh);   // debounced: no per-frame rebuild during a drag
    rebuild();
}

void ColorGallery::refresh(){ rebuild(); }

void ColorGallery::rebuild(){
    // tear down and rebuild the swatch host
    if(m_host->layout()) delete m_host->layout();
    qDeleteAll(m_host->findChildren<QWidget*>("",Qt::FindDirectChildrenOnly));
    auto* L=new QVBoxLayout(m_host); L->setContentsMargins(0,0,0,0); L->setSpacing(4);

    auto section=[&](const QString& title){ auto* l=new QLabel(title); QFont f=l->font(); f.setBold(true); f.setPointSizeF(f.pointSizeF()*0.9); l->setFont(f); l->setStyleSheet("color:#9aa;"); L->addWidget(l); };
    auto gridFor=[&](){ auto* w=new QWidget; auto* g=new QGridLayout(w); g->setContentsMargins(0,0,0,0); g->setSpacing(3); g->setAlignment(Qt::AlignLeft); L->addWidget(w); return g; };

    // document named colours
    const auto& nc=m_c->doc().namedColors;
    section(QString("Document colours (%1)").arg(nc.size()));
    if(nc.isEmpty()){ auto* hint=new QLabel("No named colours yet.\nClick ＋ New… to add one."); hint->setStyleSheet("color:#778;"); L->addWidget(hint); }
    else { auto* g=gridFor(); int col=0,row=0;
        for(int i=0;i<nc.size();++i){ QColor rc=m_c->doc().resolveNamed(i);
            QString kindTag = nc[i].kind==1?" (tint)":nc[i].kind==2?" (shade)":nc[i].kind==3?" (linked)":nc[i].theme?" (theme)":"";
            auto* sw=new Swatch(rc, nc[i].name+kindTag, i==m_sel);
            sw->onClick=[this,i,rc](Qt::MouseButton b){ m_sel=i;
                // Bind the object to this named colour (colorRef) so later edits to the palette ripple to it.
                if(b==Qt::RightButton) m_c->applyToSelection([rc,i](Shape& s){ s.stroke.enabled=true; s.stroke.color=rc; s.stroke.colorRef=i; },"Apply line colour");
                else m_c->applyToSelection([rc,i](Shape& s){ s.fill.type=FillType::Solid; s.fill.color=rc; s.fill.colorRef=i; },"Apply fill colour");
                rebuild(); };
            sw->onEdit=[this,i]{ QColor col=ColorEditor::pick(this,m_c->doc().resolveNamed(i),m_c); if(!col.isValid())return;
                m_c->pushUndo("Edit named colour"); m_c->doc().namedColors[i].color=col; m_c->doc().namedColors[i].kind=0; m_c->doc().namedColors[i].parent=-1;
                m_c->doc().resyncNamedColorBindings();   // ripple the edit to every object bound to this colour (& its tints/shades)
                m_c->refreshAll(); };
            g->addWidget(sw,row,col); if(++col>=8){col=0;row++;} } }

    // PANTONE
    section("PANTONE® spot colours");
    { auto* g=gridFor(); int col=0,row=0; for(auto& p: pantone()){ auto* sw=new Swatch(p.second,p.first,false);
        QColor pc=p.second; sw->onClick=[this,pc](Qt::MouseButton b){ if(b==Qt::RightButton) m_c->applyToSelection([pc](Shape& s){ s.stroke.enabled=true; s.stroke.color=pc; },"Apply line colour"); else m_c->applyToSelection([pc](Shape& s){ s.fill.type=FillType::Solid; s.fill.color=pc; },"Apply fill colour"); };
        sw->onEdit=[this,p]{ m_c->pushUndo("Add PANTONE"); NamedColor n; n.name=p.first; n.color=p.second; m_c->doc().namedColors.append(n); m_c->refreshAll(); };
        g->addWidget(sw,row,col); if(++col>=8){col=0;row++;} } }
    L->addStretch();
}

// ============================================================================
// FillGallery — preset flat/gradient fills
// ============================================================================
class FillSwatch : public QWidget {
public:
    std::function<void()> onClick;
    FillSwatch(const Fill& f,const QString& tip):m_f(f){ setFixedSize(54,38); setToolTip(tip); setCursor(Qt::PointingHandCursor); }
protected:
    void paintEvent(QPaintEvent*) override { QPainter g(this); g.setRenderHint(QPainter::Antialiasing); QRectF r=rect().adjusted(2,2,-2,-2);
        QBrush br;
        if(m_f.type==FillType::Linear){ QLinearGradient lg(r.topLeft(),r.bottomRight()); for(auto&s:m_f.stops) lg.setColorAt(qBound(0.0,s.pos,1.0),s.color); br=lg; }
        else if(m_f.type==FillType::Radial){ QRadialGradient rg(r.center(),r.width()*0.6); for(auto&s:m_f.stops) rg.setColorAt(qBound(0.0,s.pos,1.0),s.color); br=rg; }
        else br=QBrush(m_f.color);
        g.setBrush(br); g.setPen(QPen(QColor(0,0,0,90),1)); g.drawRoundedRect(r,4,4); }
    void mousePressEvent(QMouseEvent*) override { if(onClick) onClick(); }
    Fill m_f;
};

FillGallery::FillGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* outer=new QVBoxLayout(this); outer->setContentsMargins(8,8,8,8); outer->setSpacing(6);
    auto mkFill=[](FillType t,QColor a,QColor b)->Fill{ Fill f; f.type=t; if(t==FillType::Solid) f.color=a; else { f.stops={{0,a},{1,b}}; f.p0=QPointF(0,0); f.p1=QPointF(100,80);} return f; };
    struct P{const char* n; Fill f;};
    QVector<QPair<QString,Fill>> presets={
        {"Sunset", mkFill(FillType::Linear,QColor(255,170,60),QColor(225,60,120))},
        {"Ocean",  mkFill(FillType::Linear,QColor(80,200,255),QColor(20,70,160))},
        {"Forest", mkFill(FillType::Linear,QColor(140,210,90),QColor(20,110,70))},
        {"Grape",  mkFill(FillType::Linear,QColor(190,120,255),QColor(80,30,140))},
        {"Steel",  mkFill(FillType::Linear,QColor(220,224,230),QColor(110,120,135))},
        {"Gold",   mkFill(FillType::Linear,QColor(255,232,150),QColor(180,130,40))},
        {"Fire",   mkFill(FillType::Radial,QColor(255,240,140),QColor(200,40,20))},
        {"Orb",    mkFill(FillType::Radial,QColor(255,255,255),QColor(60,90,200))},
        {"Spotlight",mkFill(FillType::Radial,QColor(255,255,255),QColor(20,20,30))},
        {"Mint",   mkFill(FillType::Solid,QColor(120,230,180),Qt::white)},
        {"Coral",  mkFill(FillType::Solid,QColor(255,120,110),Qt::white)},
        {"Sky",    mkFill(FillType::Solid,QColor(120,190,255),Qt::white)},
    };
    auto* grid=new QGridLayout; grid->setSpacing(5); grid->setAlignment(Qt::AlignLeft); outer->addLayout(grid);
    int col=0,row=0; for(auto& p: presets){ auto* sw=new FillSwatch(p.second,p.first); Fill f=p.second;
        sw->onClick=[this,f]{ m_c->applyToSelection([f](Shape& s){ Fill nf=f; nf.p0=QPointF(0,0); QRectF bb=Document::docPath(s).boundingRect();
            QTransform inv=s.transform.inverted(); QRectF lb=inv.mapRect(bb); nf.p0=lb.topLeft(); nf.p1=(f.type==FillType::Radial)?lb.bottomRight():lb.bottomRight(); s.fill=nf; },"Apply fill preset");
            m_c->setCurrentFill(f); };
        grid->addWidget(sw,row,col); auto* lbl=new QLabel(p.first); lbl->setStyleSheet("color:#9aa;font-size:9px;"); grid->addWidget(lbl,row+1,col,Qt::AlignHCenter);
        if(++col>=4){col=0;row+=2;} }
    outer->addStretch();
}

// ============================================================================
// LineGallery — stroke width / dash / arrow presets
// ============================================================================
LineGallery::LineGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(8,8,8,8); V->setSpacing(8);
    auto head=[&](const QString& t){ auto* l=new QLabel(t); QFont f=l->font(); f.setBold(true); l->setFont(f); l->setStyleSheet("color:#9aa;"); V->addWidget(l); };
    auto rowBox=[&](){ auto* w=new QWidget; auto* h=new QHBoxLayout(w); h->setContentsMargins(0,0,0,0); h->setSpacing(4); h->setAlignment(Qt::AlignLeft); V->addWidget(w); return h; };

    head("Line width");
    { auto* h=rowBox(); const double ws[]={0,0.25,0.5,1,2,4,8,16};
      for(double w: ws){ auto* b=new QToolButton; b->setText(w==0?"None":QString::number(w)); b->setAutoRaise(true);
        connect(b,&QToolButton::clicked,this,[this,w]{ m_c->applyToSelection([w](Shape& s){ if(w<=0){ s.stroke.enabled=false; } else { s.stroke.enabled=true; s.stroke.width=w; } },"Line width"); });
        h->addWidget(b); } }

    head("Dash pattern");
    { auto* h=rowBox(); struct D{const char* n; QVector<qreal> p;}; QVector<D> ds={{"Solid",{}},{"Dot",{1,3}},{"Dash",{6,4}},{"Dash-dot",{6,3,1,3}},{"Long",{12,5}}};
      for(auto& d: ds){ auto* b=new QToolButton; b->setText(d.n); b->setAutoRaise(true); QVector<qreal> pat=d.p;
        connect(b,&QToolButton::clicked,this,[this,pat]{ m_c->applyToSelection([pat](Shape& s){ s.stroke.enabled=true; s.stroke.dashes=pat; },"Dash pattern"); });
        h->addWidget(b); } }

    head("Arrowheads");
    { auto* h=rowBox(); struct A{const char* n; int id;}; QVector<A> as={{"None",0},{"▸ Straight",-2},{"▸ Angled",-3},{"● Spot",-5},{"◆ Diamond",-6},{"❮ Feather",-7}};
      for(auto& a: as){ auto* b=new QToolButton; b->setText(a.n); b->setAutoRaise(true); int id=a.id;
        connect(b,&QToolButton::clicked,this,[this,id]{ m_c->applyToSelection([id](Shape& s){ s.stroke.enabled=true; s.stroke.endArrow=id; if(s.stroke.endArrowW<=0){s.stroke.endArrowW=s.stroke.endArrowH=1;} },"Arrowhead"); });
        h->addWidget(b); } }

    head("Caps & joins");
    { auto* h=rowBox();
      auto cap=[&](const QString& n,Qt::PenCapStyle cs){ auto* b=new QToolButton; b->setText(n); b->setAutoRaise(true); connect(b,&QToolButton::clicked,this,[this,cs]{ m_c->applyToSelection([cs](Shape& s){ s.stroke.cap=cs; },"Cap"); }); h->addWidget(b); };
      cap("Butt",Qt::FlatCap); cap("Round",Qt::RoundCap); cap("Square",Qt::SquareCap);
      auto* sp=new QLabel("  "); h->addWidget(sp);
      auto join=[&](const QString& n,Qt::PenJoinStyle js){ auto* b=new QToolButton; b->setText(n); b->setAutoRaise(true); connect(b,&QToolButton::clicked,this,[this,js]{ m_c->applyToSelection([js](Shape& s){ s.stroke.join=js; },"Join"); }); h->addWidget(b); };
      join("Mitre",Qt::MiterJoin); join("Round",Qt::RoundJoin); join("Bevel",Qt::BevelJoin); }

    head("Start arrowhead");
    { auto* h=rowBox(); struct A{const char* n; int id;}; QVector<A> as={{"None",0},{"◂ Straight",-2},{"◄ Angled",-3},{"● Spot",-5},{"◆ Diamond",-6}};
      for(auto& a: as){ auto* b=new QToolButton; b->setText(a.n); b->setAutoRaise(true); int id=a.id;
        connect(b,&QToolButton::clicked,this,[this,id]{ m_c->applyToSelection([id](Shape& s){ s.stroke.enabled=true; s.stroke.startArrow=id; if(s.stroke.startArrowW<=0){s.stroke.startArrowW=s.stroke.startArrowH=1;} },"Start arrow"); });
        h->addWidget(b); } }

    head("Width profile (variable width)");
    { auto* h=rowBox(); struct P{const char* n; QVector<QPointF> prof;};
      QVector<P> ps={{"Uniform",{}},{"Taper",{{0,1},{1,0}}},{"Reverse",{{0,0},{1,1}}},{"Spindle",{{0,0.1},{0.5,1},{1,0.1}}},{"Pressure",{{0,0.2},{0.25,1},{0.75,1},{1,0.2}}}};
      for(auto& p: ps){ auto* b=new QToolButton; b->setText(p.n); b->setAutoRaise(true); QVector<QPointF> prof=p.prof;
        connect(b,&QToolButton::clicked,this,[this,prof]{ m_c->applyToSelection([prof](Shape& s){ s.stroke.enabled=true; if(s.stroke.width<1)s.stroke.width=6; s.stroke.varWidth=!prof.isEmpty(); s.stroke.widthProfile=prof; },"Width profile"); });
        h->addWidget(b); } }

    head("Art brush");
    { auto* h=rowBox();
      auto* none=new QToolButton; none->setText("None"); none->setAutoRaise(true); connect(none,&QToolButton::clicked,this,[this]{ m_c->applyToSelection([](Shape& s){ s.stroke.brushInk=QImage(); },"No brush"); }); h->addWidget(none);
      auto mkBrush=[&](const QString& n, std::function<QImage()> gen){ auto* b=new QToolButton; b->setText(n); b->setAutoRaise(true);
        connect(b,&QToolButton::clicked,this,[this,gen]{ QImage ink=gen(); m_c->applyToSelection([ink](Shape& s){ s.stroke.enabled=true; if(s.stroke.width<2)s.stroke.width=10; s.stroke.brushInk=ink; s.stroke.brushScale=0; s.stroke.brushSpacing=0.35; },"Art brush"); }); h->addWidget(b); };
      mkBrush("Soft", []{ QImage im(40,40,QImage::Format_ARGB32); im.fill(Qt::transparent); QPainter g(&im); g.setRenderHint(QPainter::Antialiasing); QRadialGradient rg(20,20,20); rg.setColorAt(0,QColor(255,255,255,255)); rg.setColorAt(1,QColor(255,255,255,0)); g.setBrush(rg); g.setPen(Qt::NoPen); g.drawEllipse(0,0,40,40); return im; });
      mkBrush("Chalk", []{ QImage im(40,40,QImage::Format_ARGB32); im.fill(Qt::transparent); QPainter g(&im); g.setPen(Qt::NoPen); for(int i=0;i<160;++i){ unsigned u=i*2654435761u; int x=(u>>3)%40,y=(u>>11)%40,a=80+int((u>>17)%150); g.setBrush(QColor(255,255,255,a)); g.drawEllipse(QPoint(x,y),2,2);} return im; });
      mkBrush("Block", []{ QImage im(24,24,QImage::Format_ARGB32); im.fill(QColor(255,255,255,255)); return im; }); }

    head("Custom dash");
    { auto* h=rowBox(); auto* b=new QToolButton; b->setText("Custom dash…"); b->setAutoRaise(true);
      connect(b,&QToolButton::clicked,this,[this]{ bool ok; QString t=QInputDialog::getText(this,"Custom dash","Dash/gap lengths (e.g. 8 4 2 4):",QLineEdit::Normal,"8 4",&ok);
        if(!ok)return; QVector<qreal> pat; for(const QString& n: t.split(' ',Qt::SkipEmptyParts)){ bool o; double v=n.toDouble(&o); if(o&&v>0) pat<<v; }
        if(!pat.isEmpty()) m_c->applyToSelection([pat](Shape& s){ s.stroke.enabled=true; s.stroke.dashes=pat; },"Custom dash"); }); h->addWidget(b); }
    V->addStretch();
}

// ============================================================================
// FontsGallery — installed fonts with preview
// ============================================================================
FontsGallery::FontsGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(6,6,6,6); V->setSpacing(6);
    m_filter=new QLineEdit; m_filter->setPlaceholderText("Filter fonts…"); m_filter->setClearButtonEnabled(true); V->addWidget(m_filter);
    m_list=new QListWidget; m_list->setUniformItemSizes(false); V->addWidget(m_list,1);
    connect(m_filter,&QLineEdit::textChanged,this,[this](const QString& s){ populate(s); });
    connect(m_list,&QListWidget::itemClicked,this,[this](QListWidgetItem* it){ QString fam=it->data(Qt::UserRole).toString();
        m_c->applyToSelection([fam](Shape& s){ if(s.type==ShapeType::Text){ QFont f=s.font; f.setFamily(fam); s.font=f; } },"Apply font"); });
    populate(QString());
}
void FontsGallery::populate(const QString& filter){
    m_list->clear();
    const QStringList fams=QFontDatabase::families();
    int n=0; for(const QString& fam: fams){ if(!filter.isEmpty() && !fam.contains(filter,Qt::CaseInsensitive)) continue;
        auto* it=new QListWidgetItem(fam); QFont f(fam); f.setPointSize(13); it->setFont(f); it->setData(Qt::UserRole,fam); m_list->addItem(it);
        if(++n>800) break; }   // safety cap
}

// ============================================================================
// NameGallery — object names
// ============================================================================
NameGallery::NameGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(6,6,6,6); V->setSpacing(4);
    V->addWidget(new QLabel("Object names — double-click to rename, click to select"));
    m_table=new QTableWidget(0,2); m_table->setHorizontalHeaderLabels({"Name","Type"});
    m_table->horizontalHeader()->setStretchLastSection(true); m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setEditTriggers(QAbstractItemView::DoubleClicked);
    V->addWidget(m_table,1);
    connect(m_table,&QTableWidget::cellClicked,this,[this](int r,int){ if(m_block)return; m_c->select({r}); });
    connect(m_table,&QTableWidget::cellChanged,this,[this](int r,int col){ if(m_block||col!=0||r>=m_c->doc().shapes.size())return;
        m_c->pushUndo("Rename object"); m_c->doc().shapes[r].name=m_table->item(r,0)->text(); m_c->refreshAll(); });
    connect(m_c,&Canvas::documentChangedDebounced,this,&NameGallery::refresh);   // debounced: iterates named colours + shapes
    connect(m_c,&Canvas::selectionChanged,this,&NameGallery::refresh);
    refresh();
}
void NameGallery::refresh(){
    m_block=true; const auto& sh=m_c->doc().shapes;
    // A per-object table is unusable — and pathologically slow to build/select — for thousands of objects
    // (e.g. a big imported .cdr). Cap it: list the first N and skip the O(n^2) per-row select storm.
    const int CAP=2000; const int n=qMin(sh.size(), CAP);
    m_table->setRowCount(n);
    // One label per ShapeType enumerator, IN ORDER — must stay in sync with model.h's
    // `enum class ShapeType`. int(type) indexes this, so a missing entry over-reads the array.
    static const char* tn[]={"Rectangle","Ellipse","Polygon","Path","Text","Group","Shape","Table"};
    constexpr int TN=int(sizeof(tn)/sizeof(tn[0]));
    for(int i=0;i<n;++i){ auto* ni=new QTableWidgetItem(sh[i].name.isEmpty()?QString("Object %1").arg(i+1):sh[i].name);
        m_table->setItem(i,0,ni); const int t=int(sh[i].type);
        auto* ti=new QTableWidgetItem((t>=0&&t<TN)?tn[t]:"Object"); ti->setFlags(ti->flags()&~Qt::ItemIsEditable); m_table->setItem(i,1,ti); }
    auto sel=m_c->selection(); m_table->clearSelection();
    if(sel.size()<=CAP) for(int i: sel) if(i<n) m_table->selectRow(i);   // skip the selection storm on a huge (whole-doc) selection
    m_block=false;
}

// ============================================================================
// ClipartGallery — built-in insertable shapes
// ============================================================================
static QPainterPath clipStar(int pts,double inner){ QPainterPath p; for(int i=0;i<pts*2;++i){ double a=-M_PI/2+i*M_PI/pts; double r=(i&1)?inner:1.0; QPointF v(50+r*45*std::cos(a),50+r*45*std::sin(a)); if(i==0)p.moveTo(v); else p.lineTo(v);} p.closeSubpath(); return p; }
static QPainterPath clipHeart(){ QPainterPath p; p.moveTo(50,88); p.cubicTo(8,55,12,18,38,18); p.cubicTo(48,18,50,28,50,34); p.cubicTo(50,28,52,18,62,18); p.cubicTo(88,18,92,55,50,88); p.closeSubpath(); return p; }
static QPainterPath clipArrow(){ QPainterPath p; p.moveTo(10,38); p.lineTo(60,38); p.lineTo(60,22); p.lineTo(92,50); p.lineTo(60,78); p.lineTo(60,62); p.lineTo(10,62); p.closeSubpath(); return p; }
static QPainterPath clipSpeech(){ QPainterPath p; p.addRoundedRect(QRectF(10,12,80,52),12,12); QPainterPath tail; tail.moveTo(30,62); tail.lineTo(26,86); tail.lineTo(50,62); tail.closeSubpath(); return p.united(tail); }
static QPainterPath clipGear(){ QPainterPath p; const int teeth=10; for(int i=0;i<teeth*2;++i){ double a=i*M_PI/teeth; double r=(i&1)?34:46; QPointF v(50+r*std::cos(a),50+r*std::sin(a)); if(i==0)p.moveTo(v); else p.lineTo(v);} p.closeSubpath(); QPainterPath hole; hole.addEllipse(QPointF(50,50),16,16); return p.subtracted(hole); }
static QPainterPath clipLightning(){ QPainterPath p; p.moveTo(54,8); p.lineTo(28,52); p.lineTo(46,52); p.lineTo(40,92); p.lineTo(72,40); p.lineTo(52,40); p.closeSubpath(); return p; }
static QPainterPath clipCross(){ QPainterPath p; p.addRect(40,12,20,76); p.addRect(12,40,76,20); return p.simplified(); }
static QPainterPath clipBadge(){ return clipStar(12,0.82); }

ClipartGallery::ClipartGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(8,8,8,8); V->setSpacing(6);
    V->addWidget(new QLabel("Click to insert clipart"));
    struct Cl{const char* n; QPainterPath p; QColor col;};
    QVector<Cl> items={{"Star",clipStar(5,0.42),QColor(255,200,70)},{"Heart",clipHeart(),QColor(235,70,110)},
        {"Arrow",clipArrow(),QColor(90,160,255)},{"Speech",clipSpeech(),QColor(120,210,150)},
        {"Gear",clipGear(),QColor(150,156,168)},{"Lightning",clipLightning(),QColor(255,210,60)},
        {"Cross",clipCross(),QColor(230,90,90)},{"Badge",clipBadge(),QColor(180,130,240)}};
    auto* grid=new QGridLayout; grid->setSpacing(6); grid->setAlignment(Qt::AlignLeft); V->addLayout(grid);
    int col=0,row=0; for(auto& it: items){
        auto* b=new QToolButton; b->setFixedSize(58,58); b->setToolTip(it.n); b->setCursor(Qt::PointingHandCursor);
        QPixmap pm(52,52); pm.fill(Qt::transparent); { QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing); g.translate(1,1); g.scale(0.5,0.5); g.setBrush(it.col); g.setPen(QPen(it.col.darker(140),2)); g.drawPath(it.p); }
        b->setIcon(QIcon(pm)); b->setIconSize(QSize(52,52));
        QPainterPath path=it.p; QColor col2=it.col;
        connect(b,&QToolButton::clicked,this,[this,path,col2]{ Shape s; s.type=ShapeType::Path; s.path=path; s.name="Clipart";
            s.fill.type=FillType::Solid; s.fill.color=col2; s.stroke.enabled=false;
            s.transform=QTransform::fromScale(2.2,2.2)*QTransform::fromTranslate(m_c->pageSize().width()/2-110, m_c->pageSize().height()/2-110);
            m_c->addShapes({s},"Insert clipart"); });
        grid->addWidget(b,row,col); if(++col>=4){col=0;row++;} }
    V->addStretch();
}

// ============================================================================
// PageLayerGallery — a hierarchical TREE (layers ▸ objects ▸ group children)
// ============================================================================
// Item roles: UserRole = kind (0 layer, 1 top-level object, 2 group child/display-only);
// UserRole+1 = index (layer index for kind 0, shape index for kind 1, -1 for kind 2).
namespace { enum { KLayer=0, KObject=1, KChild=2 }; const int RKind=Qt::UserRole, RIdx=Qt::UserRole+1;
// A readable label for an object row (falls back to the shape type when unnamed).
static QString objLabel(const Shape& s){
    if(!s.name.trimmed().isEmpty()) return s.name;
    switch(s.type){
        case ShapeType::Rect:    return QStringLiteral("Rectangle");
        case ShapeType::Ellipse: return QStringLiteral("Ellipse");
        case ShapeType::Polygon: return s.polyStar?QStringLiteral("Star"):QStringLiteral("Polygon");
        case ShapeType::Path:    return QStringLiteral("Path");
        case ShapeType::Text:  { QString t=s.text.simplified(); if(t.size()>22)t=t.left(22)+QStringLiteral("…"); return t.isEmpty()?QStringLiteral("Text"):QStringLiteral("“%1”").arg(t); }
        case ShapeType::Group:   return QStringLiteral("Group");
        case ShapeType::Smart:   return QStringLiteral("Shape");
        case ShapeType::Table:   return QStringLiteral("Table");
    }
    return QStringLiteral("Object");
}
// A tiny colour-swatch icon so rows are visually identifiable without coupling to the renderer.
static QIcon swatch(const Shape& s){
    QPixmap pm(14,14); pm.fill(Qt::transparent); QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing);
    QColor c = s.fill.color.isValid()? s.fill.color : QColor(120,128,140);
    if(s.type==ShapeType::Group){ g.setPen(QPen(theme::ink(QColor(150,160,175)),1)); g.setBrush(Qt::NoBrush); g.drawRect(1,3,11,9); g.drawRect(3,1,7,3); }
    else if(s.type==ShapeType::Text){ g.setPen(QPen(theme::ink(QColor(200,205,215)),1)); g.setFont(QFont("Sans",9,QFont::Bold)); g.drawText(pm.rect(),Qt::AlignCenter,"T"); }
    else { g.setPen(QPen(c.darker(150),1)); g.setBrush(c);
        if(s.type==ShapeType::Ellipse) g.drawEllipse(1,1,12,12); else g.drawRoundedRect(1,1,12,12,2,2); }
    return QIcon(pm);
}}

void LayerTree::dropEvent(QDropEvent* e){ QTreeWidget::dropEvent(e); emit reordered(); }

PageLayerGallery::PageLayerGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(8,8,8,8); V->setSpacing(6);
    m_tree=new LayerTree; m_tree->setColumnCount(3); m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true); m_tree->setUniformRowHeights(true); m_tree->setIndentation(14);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1,QHeaderView::Fixed);
    m_tree->header()->setSectionResizeMode(2,QHeaderView::Fixed);
    m_tree->setColumnWidth(1,26); m_tree->setColumnWidth(2,26);
    V->addWidget(m_tree,1);
    auto* btns=new QHBoxLayout; V->addLayout(btns);
    auto mk=[&](const QString& t,const QString& tip,std::function<void()> fn){ auto* b=new QToolButton; b->setText(t); b->setToolTip(tip); b->setAutoRaise(true); connect(b,&QToolButton::clicked,this,[fn]{fn();}); btns->addWidget(b); return b; };
    // ▲/▼ act on whatever the tree row selects: a layer row raises/lowers that LAYER; otherwise the
    // selected OBJECTS are raised/lowered. (⤒/⤓ always move the active layer.)
    auto layerRowActive=[this]()->bool{ auto* it=m_tree?m_tree->currentItem():nullptr; return it && it->data(0,RKind).toInt()==KLayer; };
    mk("＋","New layer",[this]{ m_c->addLayer(); });
    mk("✕","Delete active layer",[this]{ m_c->removeLayer(m_c->activeLayer()); });
    mk("▲","Raise (the active layer if a layer row is selected, else the selected objects)",[this,layerRowActive]{ if(layerRowActive()) m_c->moveLayer(m_c->activeLayer(),+1); else m_c->raiseSelection(); });
    mk("▼","Lower (the active layer if a layer row is selected, else the selected objects)",[this,layerRowActive]{ if(layerRowActive()) m_c->moveLayer(m_c->activeLayer(),-1); else m_c->lowerSelection(); });
    mk("⤒","Layer up",[this]{ m_c->moveLayer(m_c->activeLayer(),+1); });
    mk("⤓","Layer down",[this]{ m_c->moveLayer(m_c->activeLayer(),-1); });
    btns->addStretch();

    connect(m_c,&Canvas::documentChangedDebounced,this,&PageLayerGallery::refresh);   // debounced: rebuilds the page/layer/object tree
    connect(m_c,&Canvas::selectionChanged,this,[this]{ if(m_block) return; m_block=true;
        // Highlight the tree rows matching the canvas selection without a full rebuild.
        const auto sel=m_c->selection(); QSet<int> ss(sel.begin(),sel.end());
        std::function<void(QTreeWidgetItem*)> walk=[&](QTreeWidgetItem* it){
            if(it->data(0,RKind).toInt()==KObject) it->setSelected(ss.contains(it->data(0,RIdx).toInt()));
            for(int k=0;k<it->childCount();++k) walk(it->child(k)); };
        for(int k=0;k<m_tree->topLevelItemCount();++k) walk(m_tree->topLevelItem(k));
        m_block=false; });

    connect(m_tree,&QTreeWidget::itemClicked,this,[this](QTreeWidgetItem* it,int col){
        int kind=it->data(0,RKind).toInt(), idx=it->data(0,RIdx).toInt();
        if(col==1){ // visibility toggle
            m_block=true;
            if(kind==KLayer) m_c->setLayerVisible(idx,!m_c->doc().layers[idx].visible);
            else if(kind==KObject) m_c->setShapeVisible(idx,!m_c->doc().shapes[idx].visible);
            m_block=false; rebuild();
        } else if(col==2){ // lock toggle
            m_block=true;
            if(kind==KLayer) m_c->setLayerLocked(idx,!m_c->doc().layers[idx].locked);
            else if(kind==KObject) m_c->setShapeLocked(idx,!m_c->doc().shapes[idx].locked);
            m_block=false; rebuild();
        } });
    connect(m_tree,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem* it,int col){
        if(col==0 && it->data(0,RKind).toInt()!=KChild) m_tree->editItem(it,0); });
    connect(m_tree,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem* it,int col){
        if(m_block||col!=0) return; int kind=it->data(0,RKind).toInt(), idx=it->data(0,RIdx).toInt();
        m_block=true;
        if(kind==KLayer) m_c->renameLayer(idx,it->text(0));
        else if(kind==KObject) m_c->renameShape(idx,it->text(0));
        m_block=false; });
    connect(m_tree,&QTreeWidget::itemSelectionChanged,this,&PageLayerGallery::syncSelectionToCanvas);
    connect(m_tree,&LayerTree::reordered,this,&PageLayerGallery::applyTreeOrder);
    rebuild();
}
void PageLayerGallery::refresh(){ if(!m_block) rebuild(); }

void PageLayerGallery::rebuild(){
    m_block=true;
    // Preserve which layers are expanded across rebuilds (default: all expanded).
    QSet<int> collapsed;
    for(int k=0;k<m_tree->topLevelItemCount();++k){ auto* it=m_tree->topLevelItem(k);
        if(it->data(0,RKind).toInt()==KLayer && !it->isExpanded()) collapsed.insert(it->data(0,RIdx).toInt()); }
    m_tree->clear();
    const Document& d=m_c->doc(); const auto& ly=d.layers; const auto& sh=d.shapes;
    const auto sel=m_c->selection(); QSet<int> ss(sel.begin(),sel.end());
    auto styleObj=[&](QTreeWidgetItem* it,const Shape& s){
        it->setText(1, s.visible?QStringLiteral("👁"):QStringLiteral("·"));
        it->setText(2, s.locked?QStringLiteral("🔒"):QString());
        it->setTextAlignment(1,Qt::AlignCenter); it->setTextAlignment(2,Qt::AlignCenter);
        if(s.locked) it->setForeground(0,theme::ink(QColor(130,138,150))); };
    // Recursively add a group's children as display-only rows (nested groups included).
    std::function<void(QTreeWidgetItem*,const Shape&)> addKids=[&](QTreeWidgetItem* parent,const Shape& g){
        for(const Shape& ch: g.children){ auto* ci=new QTreeWidgetItem(parent);
            ci->setData(0,RKind,KChild); ci->setData(0,RIdx,-1); ci->setText(0,objLabel(ch)); ci->setIcon(0,swatch(ch));
            ci->setFlags(Qt::ItemIsEnabled); ci->setForeground(0,theme::ink(QColor(150,158,170))); styleObj(ci,ch);
            if(ch.type==ShapeType::Group && !ch.children.isEmpty()) addKids(ci,ch); } };
    for(int i=ly.size()-1;i>=0;--i){                     // top layer at the top
        auto* li=new QTreeWidgetItem(m_tree);
        li->setData(0,RKind,KLayer); li->setData(0,RIdx,i);
        QString nm=ly[i].name; if(ly[i].master) nm+=QStringLiteral("  ⟨master⟩");
        li->setText(0,nm);
        li->setText(1, ly[i].visible?QStringLiteral("👁"):QStringLiteral("·"));
        li->setText(2, ly[i].locked?QStringLiteral("🔒"):QString());
        li->setTextAlignment(1,Qt::AlignCenter); li->setTextAlignment(2,Qt::AlignCenter);
        QFont lf=li->font(0); lf.setBold(i==m_c->activeLayer()); li->setFont(0,lf);
        li->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsDropEnabled);
        // objects on this layer, top-of-stack first (reverse of paint/list order)
        for(int k=sh.size()-1;k>=0;--k){ if(qBound(0,sh[k].layer,ly.size()-1)!=i) continue;
            auto* oi=new QTreeWidgetItem(li);
            oi->setData(0,RKind,KObject); oi->setData(0,RIdx,k); oi->setText(0,objLabel(sh[k])); oi->setIcon(0,swatch(sh[k]));
            oi->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsEditable|Qt::ItemIsDragEnabled);
            styleObj(oi,sh[k]);
            if(sh[k].type==ShapeType::Group && !sh[k].children.isEmpty()) addKids(oi,sh[k]);
            if(ss.contains(k)) oi->setSelected(true); }
        li->setExpanded(!collapsed.contains(i));
    }
    m_block=false;
}

// Tree selection → canvas: select the chosen top-level objects (and set the active layer).
void PageLayerGallery::syncSelectionToCanvas(){
    if(m_block) return; m_block=true;
    QVector<int> objs; int lastLayer=-1;
    for(QTreeWidgetItem* it: m_tree->selectedItems()){
        int kind=it->data(0,RKind).toInt(), idx=it->data(0,RIdx).toInt();
        if(kind==KObject){ objs.push_back(idx); lastLayer=qBound(0,m_c->doc().shapes[idx].layer,m_c->doc().layers.size()-1); }
        else if(kind==KLayer){ lastLayer=idx; }
    }
    if(lastLayer>=0) m_c->setActiveLayer(lastLayer);
    m_c->select(objs);               // empty => clears selection (matches clicking a layer row)
    m_block=false;
}

// After an internal drag-drop: re-derive object stacking + layer membership from the tree.
void PageLayerGallery::applyTreeOrder(){
    const int nL=m_c->doc().layers.size(), nS=m_c->doc().shapes.size();
    // Collect, per layer index, the object shape-indices now under that layer node (top-first).
    QVector<QVector<int>> perLayer(nL);
    int seen=0; bool ok=true;
    for(int k=0;k<m_tree->topLevelItemCount() && ok;++k){ auto* li=m_tree->topLevelItem(k);
        if(li->data(0,RKind).toInt()!=KLayer){ ok=false; break; }   // an object escaped to top level → bail
        int L=qBound(0,li->data(0,RIdx).toInt(),nL-1);
        for(int j=0;j<li->childCount();++j){ auto* oi=li->child(j);
            if(oi->data(0,RKind).toInt()!=KObject) continue;         // group children are display-only
            perLayer[L].push_back(oi->data(0,RIdx).toInt()); ++seen; }
    }
    if(!ok || seen!=nS){ rebuild(); return; }                        // malformed drop → revert view, no change
    // Build the new flat paint order (bottom layer first; within a layer bottom-of-stack first).
    QVector<int> order, newLayer; order.reserve(nS); newLayer.reserve(nS);
    for(int L=0;L<nL;++L) for(int j=perLayer[L].size()-1;j>=0;--j){ order.push_back(perLayer[L][j]); newLayer.push_back(L); }
    m_block=true; m_c->setObjectOrder(order,newLayer); m_block=false;
    rebuild();
}

// ============================================================================
// SmartShapeGallery — parametric shapes (the `param` slider drives count/teeth/…)
// ============================================================================
// Generators now live in Shape::smartPath() (model.cpp) so inserted SmartShapes stay parametric.

SmartShapeGallery::SmartShapeGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(8,8,8,8); V->setSpacing(6);
    V->addWidget(new QLabel("SmartShapes — set the parameter, then click to insert"));
    auto* prow=new QHBoxLayout; prow->addWidget(new QLabel("Count / teeth / points"));
    auto* sp=new QSpinBox; sp->setRange(2,24); sp->setValue(m_param); prow->addWidget(sp); prow->addStretch(); V->addLayout(prow);
    connect(sp,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ m_param=v; });
    struct Sm{const char* n; int kind; QColor col;};
    QVector<Sm> items={{"Chevron",0,QColor(90,160,255)},{"Gear",1,QColor(150,156,168)},{"Cog",2,QColor(120,130,150)},
        {"Banner",3,QColor(235,120,90)},{"Arrow",4,QColor(90,200,140)},{"Star",5,QColor(255,200,70)},
        {"Callout",6,QColor(120,210,150)}};
    auto* grid=new QGridLayout; grid->setSpacing(6); grid->setAlignment(Qt::AlignLeft); V->addLayout(grid);
    int col=0,row=0; for(auto& it: items){
        auto* b=new QToolButton; b->setFixedSize(60,60); b->setToolTip(it.n); b->setCursor(Qt::PointingHandCursor);
        QPixmap pm(54,54); pm.fill(Qt::transparent); { QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing); g.translate(2,2); g.scale(0.5,0.5); g.setBrush(it.col); g.setPen(QPen(it.col.darker(140),2)); g.drawPath(Shape::smartPath(it.kind,6)); }
        b->setIcon(QIcon(pm)); b->setIconSize(QSize(54,54));
        int kind=it.kind; QColor cc=it.col; QString nm=it.n;
        connect(b,&QToolButton::clicked,this,[this,kind,cc,nm]{ Shape s; s.type=ShapeType::Smart; s.smartKind=kind; s.smartParam=m_param; s.name=nm;
            s.rect=QRectF(0,0,100,100);   // generator's nominal box; localPath fits this rect
            s.fill.type=FillType::Solid; s.fill.color=cc; s.stroke.enabled=false;
            s.transform=QTransform::fromScale(2.2,2.2)*QTransform::fromTranslate(m_c->pageSize().width()/2-110,m_c->pageSize().height()/2-110);
            m_c->addShapes({s},"Insert SmartShape"); });
        grid->addWidget(b,row,col); if(++col>=4){col=0;row++;} }
    V->addStretch();
}

// ============================================================================
// BitmapGallery — thumbnails of embedded bitmaps; click to select the object
// ============================================================================
BitmapGallery::BitmapGallery(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(8,8,8,8); V->setSpacing(6);
    V->addWidget(new QLabel("Embedded bitmaps — click to select"));
    m_host=new QWidget; V->addWidget(m_host); V->addStretch();
    connect(m_c,&Canvas::documentChangedDebounced,this,&BitmapGallery::refresh);   // debounced: scans the bitmap list
    refresh();
}
void BitmapGallery::refresh(){
    if(m_host->layout()) delete m_host->layout();
    qDeleteAll(m_host->findChildren<QWidget*>("",Qt::FindDirectChildrenOnly));
    auto* g=new QGridLayout(m_host); g->setContentsMargins(0,0,0,0); g->setSpacing(5); g->setAlignment(Qt::AlignLeft|Qt::AlignTop);
    int col=0,row=0, found=0;
    const auto& sh=m_c->doc().shapes;
    for(int i=0;i<sh.size();++i){ if(sh[i].fill.type!=FillType::Bitmap || sh[i].fill.image.isNull()) continue; found++;
        const QImage& im=sh[i].fill.image;
        auto* b=new QToolButton; b->setFixedSize(64,64); b->setToolTip(QString("%1×%2  (%3)").arg(im.width()).arg(im.height()).arg(sh[i].name.isEmpty()?"bitmap":sh[i].name)); b->setCursor(Qt::PointingHandCursor);
        b->setIcon(QIcon(QPixmap::fromImage(im.scaled(58,58,Qt::KeepAspectRatio,Qt::SmoothTransformation)))); b->setIconSize(QSize(58,58));
        int idx=i; connect(b,&QToolButton::clicked,this,[this,idx]{ m_c->select({idx}); });
        g->addWidget(b,row,col); if(++col>=3){col=0;row++;} }
    if(found==0){ auto* l=new QLabel("No bitmaps yet.\nFile ▸ Import Image…"); l->setStyleSheet("color:#778;"); g->addWidget(l,0,0); }
}

// ============================================================================
// AnimationPanel — frames are layers (those not marked as backgrounds shown behind every frame)
// ============================================================================
AnimationPanel::AnimationPanel(Canvas* c,QWidget* parent):QWidget(parent),m_c(c){
    auto* V=new QVBoxLayout(this); V->setContentsMargins(8,8,8,8); V->setSpacing(6);
    auto tool=[this](const QString& text,const QString& tip){ auto* b=new QToolButton; b->setText(text); b->setToolTip(tip); b->setAutoRaise(true); return b; };
    auto section=[V](const QString& title){ auto* l=new QLabel(title); l->setStyleSheet("font-weight:600; margin-top:6px;"); V->addWidget(l); };

    // Transport
    auto* tr=new QHBoxLayout; tr->setSpacing(2);
    auto* first=tool("«","First frame"); auto* prev=tool("‹","Previous frame"); m_playBtn=tool("▶","Play"); auto* next=tool("›","Next frame"); auto* last=tool("»","Last frame");
    m_loopBtn=tool("Loop","Play over and over"); m_loopBtn->setCheckable(true); m_loopBtn->setChecked(true);
    for(auto* b:{first,prev,m_playBtn,next,last}){ b->setMinimumSize(30,26); b->setStyleSheet("font-size:17px;"); tr->addWidget(b); }
    tr->addStretch(); tr->addWidget(m_loopBtn); V->addLayout(tr);
    m_range=new QComboBox; m_range->setToolTip("What Play plays: every frame, or the frames of one tag"); V->addWidget(m_range);
    m_info=new QLabel; m_info->setStyleSheet("color:#889;"); V->addWidget(m_info);

    // Frames
    m_frames=new QListWidget; m_frames->setIconSize(QSize(48,48)); m_frames->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_frames->setDragDropMode(QAbstractItemView::InternalMove); m_frames->setDefaultDropAction(Qt::MoveAction); m_frames->setMinimumHeight(180);
    m_frames->setToolTip("Click to show a frame; drag to reorder; Shift/Ctrl-click to pick several");
    V->addWidget(m_frames);
    auto* fr=new QHBoxLayout; fr->setSpacing(2);
    auto* addB=tool("New","Add an empty frame after this one"); auto* dupB=tool("Duplicate","Copy this frame"); auto* delB=tool("Delete","Delete the selected frames");
    auto* upB=tool("↑","Move earlier"); auto* downB=tool("↓","Move later");
    for(auto* b:{addB,dupB,delB}) fr->addWidget(b); fr->addStretch(); fr->addWidget(upB); fr->addWidget(downB); V->addLayout(fr);
    auto* dr=new QHBoxLayout; m_delay=new QSpinBox; m_delay->setRange(10,60000); m_delay->setSingleStep(10); m_delay->setSuffix(" ms"); m_delay->setValue(100);
    auto* allB=tool("All frames","Give every frame this duration");
    dr->addWidget(new QLabel("Duration")); dr->addWidget(m_delay); dr->addWidget(allB); dr->addStretch(); V->addLayout(dr);
    auto* bgB=tool("Show in every frame","Turn the selected frame into a background shown behind every frame");

    // Backgrounds
    section("Backgrounds");
    m_backs=new QListWidget; m_backs->setMaximumHeight(70); m_backs->setToolTip("Layers shown behind every frame"); V->addWidget(m_backs);
    auto* br=new QHBoxLayout; auto* unBgB=tool("Make a frame","Turn the selected background back into a frame of its own");
    br->addWidget(bgB); br->addWidget(unBgB); br->addStretch(); V->addLayout(br);

    // Onion skin
    section("Onion skin");
    auto* on=new QHBoxLayout; m_onion=new QCheckBox("Show"); m_onionBefore=new QSpinBox; m_onionAfter=new QSpinBox;
    for(auto* sp:{m_onionBefore,m_onionAfter}){ sp->setRange(0,5); sp->setValue(1); }
    m_onionBefore->setToolTip("Frames before (shown red)"); m_onionAfter->setToolTip("Frames after (shown blue)");
    on->addWidget(m_onion); on->addWidget(new QLabel("before")); on->addWidget(m_onionBefore); on->addWidget(new QLabel("after")); on->addWidget(m_onionAfter); on->addStretch(); V->addLayout(on);
    m_onionOpacity=new QSlider(Qt::Horizontal); m_onionOpacity->setRange(5,100); m_onionOpacity->setValue(35); m_onionOpacity->setToolTip("Onion skin strength");
    V->addWidget(m_onionOpacity);

    // Tags
    section("Tags");
    m_tags=new QListWidget; m_tags->setMaximumHeight(90); m_tags->setToolTip("Named frame ranges; double-click to rename"); V->addWidget(m_tags);
    auto* tg=new QHBoxLayout; auto* tagB=tool("Tag selection","Name the selected frames as a range"); auto* untagB=tool("Delete","Delete the selected tag");
    m_tagDir=new QComboBox; m_tagDir->addItems({"Forward","Reverse","Ping-pong"}); m_tagDir->setToolTip("How the selected tag plays");
    tg->addWidget(tagB); tg->addWidget(untagB); tg->addWidget(m_tagDir); tg->addStretch(); V->addLayout(tg);

    // Export
    section("Export");
    auto* ex1=new QHBoxLayout; m_format=new QComboBox; m_format->addItems({"Animated GIF","Animated PNG","PNG for each frame","Sprite sheet + JSON"});
    m_scale=new QDoubleSpinBox; m_scale->setRange(0.1,32); m_scale->setDecimals(2); m_scale->setValue(1.0); m_scale->setSuffix(" ×"); m_scale->setToolTip("Size, as a multiple of the page");
    ex1->addWidget(m_format); ex1->addWidget(m_scale); V->addLayout(ex1);
    auto* ex2=new QHBoxLayout; m_transparent=new QCheckBox("Transparent"); m_transparent->setChecked(true); m_transparent->setToolTip("Leave the background clear; off puts the frames on white");
    m_loops=new QSpinBox; m_loops->setRange(0,1000); m_loops->setSpecialValueText("forever"); m_loops->setToolTip("Times the exported animation plays");
    auto* expB=new QPushButton("Export…");
    ex2->addWidget(m_transparent); ex2->addWidget(new QLabel("Plays")); ex2->addWidget(m_loops); ex2->addStretch(); ex2->addWidget(expB); V->addLayout(ex2);
    V->addStretch();

    m_timer=new QTimer(this); m_timer->setSingleShot(true); connect(m_timer,&QTimer::timeout,this,[this]{ step(); });
    m_thumbTimer=new QTimer(this); m_thumbTimer->setSingleShot(true); m_thumbTimer->setInterval(250); connect(m_thumbTimer,&QTimer::timeout,this,[this]{ renderThumbs(); });

    auto goTo=[this](int f){ if(m_playing) stop(); const int n=m_c->doc().frameLayers().size(); if(n<1) return; m_c->showFrame(qBound(0,f,n-1)); };
    connect(first,&QToolButton::clicked,this,[goTo]{ goTo(0); });
    connect(last,&QToolButton::clicked,this,[this,goTo]{ goTo(m_c->doc().frameLayers().size()-1); });
    connect(prev,&QToolButton::clicked,this,[this,goTo]{ const int n=m_c->doc().frameLayers().size(); if(n) goTo((m_c->currentFrame()-1+n)%n); });
    connect(next,&QToolButton::clicked,this,[this,goTo]{ const int n=m_c->doc().frameLayers().size(); if(n) goTo((m_c->currentFrame()+1)%n); });
    connect(m_playBtn,&QToolButton::clicked,this,[this]{ if(m_playing) stop(); else play(); });

    connect(m_frames,&QListWidget::itemSelectionChanged,this,[this]{ if(m_building) return;
        const QVector<int> sel=selectedFrames(); if(sel.size()==1 && !m_playing && sel[0]!=m_c->currentFrame()) m_c->showFrame(sel[0]); syncControls(); });
    connect(m_frames->model(),&QAbstractItemModel::rowsMoved,this,[this](const QModelIndex&,int start,int,const QModelIndex&,int row){
        if(m_building) return; const int to = row > start ? row-1 : row; m_c->moveFrame(start,to); });
    connect(addB,&QToolButton::clicked,this,[this]{ m_c->insertFrame(m_c->currentFrame(),false); });
    connect(dupB,&QToolButton::clicked,this,[this]{ m_c->insertFrame(m_c->currentFrame(),true); });
    connect(delB,&QToolButton::clicked,this,[this]{ QVector<int> s=selectedFrames(); if(s.isEmpty() && m_c->currentFrame()>=0) s<<m_c->currentFrame(); m_c->deleteFrames(s); });
    connect(upB,&QToolButton::clicked,this,[this]{ const int f=m_c->currentFrame(); if(f>0) m_c->moveFrame(f,f-1); });
    connect(downB,&QToolButton::clicked,this,[this]{ const int f=m_c->currentFrame(); if(f>=0 && f+1<m_c->doc().frameLayers().size()) m_c->moveFrame(f,f+1); });
    connect(m_delay,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ if(m_building) return;
        QVector<int> s=selectedFrames(); if(s.isEmpty() && m_c->currentFrame()>=0) s<<m_c->currentFrame(); m_c->setFrameDelay(s,v); });
    connect(allB,&QToolButton::clicked,this,[this]{ QVector<int> all; for(int k=0;k<m_c->doc().frameLayers().size();++k) all<<k; m_c->setFrameDelay(all,m_delay->value()); });
    connect(bgB,&QToolButton::clicked,this,[this]{ const QVector<int> fr=m_c->doc().frameLayers(); const int f=m_c->currentFrame();
        if(fr.size()<2){ m_info->setText("An animation needs at least one frame."); return; } if(f>=0) m_c->setFrameBackground(fr[f],true); });
    connect(unBgB,&QToolButton::clicked,this,[this]{ auto* it=m_backs->currentItem(); if(it) m_c->setFrameBackground(it->data(Qt::UserRole).toInt(),false); });

    auto onion=[this]{ if(m_onion->isChecked()) m_c->setOnionSkin(m_onionBefore->value(),m_onionAfter->value(),m_onionOpacity->value()/100.0); else m_c->setOnionSkin(0,0,0.35); };
    connect(m_onion,&QCheckBox::toggled,this,onion);
    connect(m_onionBefore,QOverload<int>::of(&QSpinBox::valueChanged),this,onion);
    connect(m_onionAfter,QOverload<int>::of(&QSpinBox::valueChanged),this,onion);
    connect(m_onionOpacity,&QSlider::valueChanged,this,onion);

    connect(tagB,&QToolButton::clicked,this,[this]{ QVector<int> s=selectedFrames(); if(s.isEmpty()){ m_info->setText("Select the frames to tag first."); return; }
        bool ok=false; const QString name=QInputDialog::getText(this,"Tag frames","Name for these frames:",QLineEdit::Normal,QString("Tag %1").arg(m_c->doc().frameTags.size()+1),&ok);
        if(!ok || name.trimmed().isEmpty()) return;
        FrameTag t; t.name=name.trimmed(); t.from=*std::min_element(s.begin(),s.end()); t.to=*std::max_element(s.begin(),s.end());
        QVector<FrameTag> tags=m_c->doc().frameTags; tags<<t; m_c->setFrameTags(tags); });
    connect(untagB,&QToolButton::clicked,this,[this]{ const int r=m_tags->currentRow(); if(r<0) return; QVector<FrameTag> tags=m_c->doc().frameTags; if(r<tags.size()){ tags.remove(r); m_c->setFrameTags(tags); } });
    connect(m_tags,&QListWidget::currentRowChanged,this,[this](int r){ if(m_building) return; const auto& tags=m_c->doc().frameTags;
        if(r>=0 && r<tags.size()){ const QSignalBlocker b(m_tagDir); m_tagDir->setCurrentIndex(tags[r].direction); } });
    connect(m_tagDir,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int d){ if(m_building) return; const int r=m_tags->currentRow();
        QVector<FrameTag> tags=m_c->doc().frameTags; if(r>=0 && r<tags.size() && tags[r].direction!=d){ tags[r].direction=d; m_c->setFrameTags(tags); } });
    connect(m_tags,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem*){ const int r=m_tags->currentRow(); QVector<FrameTag> tags=m_c->doc().frameTags; if(r<0||r>=tags.size()) return;
        bool ok=false; const QString n=QInputDialog::getText(this,"Rename tag","Name:",QLineEdit::Normal,tags[r].name,&ok); if(ok && !n.trimmed().isEmpty()){ tags[r].name=n.trimmed(); m_c->setFrameTags(tags); } });

    connect(m_loops,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){ if(!m_building) m_c->setAnimationLoops(v); });
    connect(expB,&QPushButton::clicked,this,[this]{
        static const char* filters[]={"GIF (*.gif)","Animated PNG (*.png *.apng)","PNG (*.png)","PNG (*.png)"};
        static const char* names[]={"animation.gif","animation.png","frame.png","sprites.png"};
        const int fmt=m_format->currentIndex();
        QString p=QFileDialog::getSaveFileName(this,"Export Animation",names[fmt],filters[fmt],nullptr,QFileDialog::DontUseNativeDialog);
        if(p.isEmpty()) return;
        if(QFileInfo(p).suffix().isEmpty()) p+= fmt==0 ? ".gif" : ".png";
        int from=0, to=-1; const int r=m_range->currentIndex()-1; const auto& tags=m_c->doc().frameTags;
        if(r>=0 && r<tags.size()){ from=tags[r].from; to=tags[r].to; }
        const bool ok=m_c->exportAnimation(p,fmt,m_scale->value(),from,to,m_transparent->isChecked());
        m_info->setText(ok ? QString("Exported %1").arg(QFileInfo(p).fileName()) : QString("Couldn't export %1").arg(QFileInfo(p).fileName())); });

    connect(m_c,&Canvas::documentChanged,this,[this]{ if(!m_playing) rebuild(); });
    rebuild();
}

QVector<int> AnimationPanel::selectedFrames() const {
    QVector<int> s; for(auto* it : m_frames->selectedItems()) s << m_frames->row(it);
    std::sort(s.begin(),s.end()); return s;
}

void AnimationPanel::rebuild(){
    m_building=true;
    const Document& d=m_c->doc();
    const QVector<int> fr=d.frameLayers();
    const int cur=m_c->currentFrame();
    if(m_frames->count()!=fr.size()){ m_frames->clear(); for(int k=0;k<fr.size();++k) m_frames->addItem(new QListWidgetItem); }
    for(int k=0;k<fr.size();++k){
        const Layer& L=d.layers[fr[k]];
        m_frames->item(k)->setText(QString("%1  %2\n%3 ms").arg(k+1).arg(L.name).arg(L.frameDelayMs>0?L.frameDelayMs:100));
    }
    if(cur>=0 && cur<m_frames->count() && !m_frames->item(cur)->isSelected()){ m_frames->clearSelection(); m_frames->item(cur)->setSelected(true); m_frames->setCurrentRow(cur, QItemSelectionModel::NoUpdate); }
    m_backs->clear();
    for(int i=0;i<d.layers.size();++i) if(d.layers[i].frameBackground){ auto* it=new QListWidgetItem(d.layers[i].name); it->setData(Qt::UserRole,i); m_backs->addItem(it); }
    const int rangeWas=m_range->currentIndex();
    m_range->clear(); m_range->addItem(QString("All frames (%1)").arg(fr.size()));
    m_tags->clear();
    static const char* dirs[]={"","  ◀","  ◀▶"};
    for(const FrameTag& t : d.frameTags){
        m_range->addItem(QString("%1 (%2–%3)").arg(t.name).arg(t.from+1).arg(t.to+1));
        auto* it=new QListWidgetItem(QString("%1   %2–%3%4").arg(t.name).arg(t.from+1).arg(t.to+1).arg(dirs[qBound(0,t.direction,2)]));
        QPixmap sw(12,12); sw.fill(t.color); it->setIcon(QIcon(sw)); m_tags->addItem(it);
    }
    m_range->setCurrentIndex(qBound(0,rangeWas,m_range->count()-1));
    m_loops->setValue(d.animLoops);
    m_info->setText(fr.size()==1 ? QString("1 frame") : QString("%1 frames%2").arg(fr.size()).arg(cur>=0 ? QString(", showing %1").arg(cur+1) : QString()));
    syncControls();
    m_building=false;
    m_thumbTimer->start();
}

void AnimationPanel::syncControls(){
    const bool was=m_building; m_building=true;
    const QVector<int> fr=m_c->doc().frameLayers();
    QVector<int> s=selectedFrames(); if(s.isEmpty() && m_c->currentFrame()>=0) s<<m_c->currentFrame();
    if(!s.isEmpty() && s[0]<fr.size()){ const int d=m_c->doc().layers[fr[s[0]]].frameDelayMs; m_delay->setValue(d>0?d:100); }
    m_building=was;
}

void AnimationPanel::renderThumbs(){
    if(!isVisible() || m_playing) return;
    const int n=qMin(m_frames->count(), 400);                 // a long animation gets text-only rows past this
    for(int k=0;k<n;++k){
        const QImage img=m_c->renderFrame(k,QSize(48,48),true);
        if(!img.isNull()) m_frames->item(k)->setIcon(QIcon(QPixmap::fromImage(img)));
    }
}

QVector<int> AnimationPanel::playSequence() const {
    const int n=m_c->doc().frameLayers().size();
    int from=0, to=n-1, dir=0;
    const int r=m_range->currentIndex()-1; const auto& tags=m_c->doc().frameTags;
    if(r>=0 && r<tags.size()){ from=qBound(0,tags[r].from,n-1); to=qBound(from,tags[r].to,n-1); dir=tags[r].direction; }
    QVector<int> seq; for(int f=from;f<=to;++f) seq<<f;
    if(dir==1) std::reverse(seq.begin(),seq.end());
    else if(dir==2) for(int f=to-1;f>from;--f) seq<<f;          // there and back, without repeating the ends
    return seq;
}

void AnimationPanel::play(){
    m_seq=playSequence();
    if(m_seq.size()<2){ m_info->setText("Add frames to play."); return; }
    m_restore=m_c->currentFrame(); m_playing=true; m_seqPos=0; m_playBtn->setText("■"); m_playBtn->setToolTip("Stop");
    step();
}

void AnimationPanel::stop(){
    m_playing=false; m_timer->stop(); m_playBtn->setText("▶"); m_playBtn->setToolTip("Play");
    const int n=m_c->doc().frameLayers().size();
    if(m_restore>=0 && m_restore<n) m_c->showFrame(m_restore); else rebuild();
}

void AnimationPanel::step(){
    if(!m_playing) return;
    const QVector<int> fr=m_c->doc().frameLayers();
    if(m_seqPos>=m_seq.size()){
        if(!m_loopBtn->isChecked()){ stop(); return; }
        m_seqPos=0;
    }
    const int f=m_seq[m_seqPos++];
    if(f<0 || f>=fr.size()){ stop(); return; }
    m_c->showFrame(f,true);
    m_info->setText(QString("Playing frame %1 of %2").arg(f+1).arg(fr.size()));
    const int d=m_c->doc().layers[fr[f]].frameDelayMs;
    m_timer->start(d>0?d:100);
}

} // namespace xara
