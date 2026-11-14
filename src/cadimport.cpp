// STEP and IGES, the neutral CAD formats, through Open CASCADE. A model drawn flat (as IGES drawings often are)
// comes in as its edges, seen from above. A solid model comes in as a technical drawing: four pages, one view
// each — isometric, front, top and right — its edges as OCCT's hidden-line removal sees them, the visible ones
// solid and the hidden ones dashed in a group of their own. Curves are followed to a fraction of the view.
#include "prepress.h"
#include <QPainterPath>
#include <QtMath>

#ifdef HAVE_OCCT
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_PolyAlgo.hxx>
#include <HLRBRep_PolyHLRToShape.hxx>
#include <IGESControl_Reader.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#endif

namespace xara {

#ifdef HAVE_OCCT
namespace {

// Every edge of `shape` as polylines in its x/y plane (y flipped to point down the page).
QPainterPath edgesOf(const TopoDS_Shape& shape, double deflection, int* count){
    QPainterPath path;
    if(shape.IsNull()) return path;
    for(TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()){
        if(*count > 2000000) break;
        try {
            const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
            BRepAdaptor_Curve c(e);
            GCPnts_TangentialDeflection d(c, 0.2, deflection, 2);
            if(d.NbPoints() < 2) continue;
            for(int i = 1; i <= d.NbPoints(); ++i){
                const gp_Pnt p = d.Value(i);
                const QPointF q(p.X(), -p.Y());
                if(i == 1) path.moveTo(q); else path.lineTo(q);
            }
            ++*count;
        } catch(const Standard_Failure&){ continue; }
    }
    return path;
}

Shape stroke(const QPainterPath& p, const QString& name, const QColor& c, double w, bool dashed){
    Shape s; s.type = ShapeType::Path; s.name = name; s.path = p; s.fill.type = FillType::None;
    s.stroke.enabled = true; s.stroke.color = c; s.stroke.width = w; s.stroke.cap = Qt::RoundCap; s.stroke.join = Qt::RoundJoin;
    if(dashed) s.stroke.dashes = {4, 3};
    return s;
}

} // namespace

bool cadWriteTestStep(const QString& path){
    try {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 100, 60, 40).Shape();
        TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(50, 30, -1), gp_Dir(0, 0, 1)), 15, 42).Shape();
        TopoDS_Shape part = BRepAlgoAPI_Cut(box, hole).Shape();
        STEPControl_Writer w;
        if(w.Transfer(part, STEPControl_AsIs) != IFSelect_RetDone) return false;
        return w.Write(path.toLocal8Bit().constData()) == IFSelect_RetDone;
    } catch(const Standard_Failure&){ return false; }
}
#endif

#ifndef HAVE_OCCT
bool cadWriteTestStep(const QString&){ return false; }
#endif

Document importCadDocument(const QString& path, CadImportStats* st){
    Document doc;
#ifndef HAVE_OCCT
    Q_UNUSED(path); if(st) st->unavailable = true;
    return doc;
#else
    const QString lower = path.toLower();
    const bool iges = lower.endsWith(QLatin1String(".igs")) || lower.endsWith(QLatin1String(".iges"));
    TopoDS_Shape shape;
    try {
        if(iges){
            IGESControl_Reader r;
            if(r.ReadFile(path.toLocal8Bit().constData()) != IFSelect_RetDone) return doc;
            r.TransferRoots(); shape = r.OneShape();
        } else {
            STEPControl_Reader r;
            if(r.ReadFile(path.toLocal8Bit().constData()) != IFSelect_RetDone) return doc;
            r.TransferRoots(); shape = r.OneShape();
        }
    } catch(const Standard_Failure&){ return doc; }
    if(shape.IsNull()) return doc;
    Bnd_Box box; BRepBndLib::Add(shape, box);
    if(box.IsVoid()) return doc;
    double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
    const double diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
    if(!(diag > 0) || !std::isfinite(diag)) return doc;
    const double deflection = diag * 0.0005;
    const QSizeF page(1123, 794);                                               // A4 landscape
    const double margin = 48;
    int edges = 0;
    struct View { QString name; QPainterPath visible, hidden; };
    QVector<View> views;
    const bool flat = (z1 - z0) <= diag * 1e-6;
    if(flat){
        views << View{QStringLiteral("Drawing"), edgesOf(shape, deflection, &edges), QPainterPath()};
    } else {
        try { BRepMesh_IncrementalMesh mesh(shape, deflection * 2, false, 0.2, true); } catch(const Standard_Failure&){}   // this hidden-line algorithm sees curves as fine as their mesh
        const gp_Pnt c((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
        struct Dir { const char* name; gp_Dir look, up; };
        const Dir dirs[] = { {"Isometric", gp_Dir(1, -1, 1), gp_Dir(0, 0, 1)}, {"Front", gp_Dir(0, -1, 0), gp_Dir(0, 0, 1)},
                             {"Top", gp_Dir(0, 0, 1), gp_Dir(0, 1, 0)}, {"Right", gp_Dir(1, 0, 0), gp_Dir(0, 0, 1)} };
        for(const Dir& d : dirs){
            try {
                // The view's frame: x to the right, y up the page, z towards the eye.
                const gp_Dir xDir = d.up.Crossed(d.look);
                const gp_Ax2 frame(c, d.look, xDir);
                Handle(HLRBRep_PolyAlgo) algo = new HLRBRep_PolyAlgo();
                algo->Load(shape);
                algo->Projector(HLRAlgo_Projector(frame));
                algo->Update();
                HLRBRep_PolyHLRToShape out; out.Update(algo);
                View v; v.name = QString::fromLatin1(d.name);
                v.visible.addPath(edgesOf(out.VCompound(), deflection, &edges));
                v.visible.addPath(edgesOf(out.OutLineVCompound(), deflection, &edges));
                v.hidden.addPath(edgesOf(out.HCompound(), deflection, &edges));
                v.hidden.addPath(edgesOf(out.OutLineHCompound(), deflection, &edges));
                views << v;
            } catch(const Standard_Failure&){ continue; }
        }
    }
    QVector<QVector<Shape>> pages;
    for(const View& v : std::as_const(views)){
        QRectF b = v.visible.boundingRect().united(v.hidden.boundingRect());
        if(b.isEmpty() && b.width() <= 0 && b.height() <= 0) continue;
        const double sc = qMin((page.width() - 2 * margin) / qMax(1e-9, b.width()), (page.height() - 2 * margin) / qMax(1e-9, b.height()));
        const QTransform fit = QTransform::fromTranslate(-b.center().x(), -b.center().y()) * QTransform::fromScale(sc, sc) * QTransform::fromTranslate(page.width() / 2, page.height() / 2);
        QVector<Shape> shapes;
        if(!v.hidden.isEmpty()){ Shape g; g.type = ShapeType::Group; g.name = QStringLiteral("Hidden lines"); g.children << stroke(fit.map(v.hidden), QStringLiteral("Hidden"), QColor(150, 150, 150), 0.6, true); shapes << g; }
        if(!v.visible.isEmpty()) shapes << stroke(fit.map(v.visible), QStringLiteral("Visible"), QColor(20, 20, 20), 1.2, false);
        Shape label; label.type = ShapeType::Text; label.text = v.name; label.name = v.name; label.font = QFont(QStringLiteral("DejaVu Sans")); label.font.setPixelSize(16);
        label.fill.type = FillType::Solid; label.fill.color = QColor(60, 60, 60); label.stroke.enabled = false; label.transform = QTransform::fromTranslate(margin / 2, margin / 4);
        if(!flat) shapes << label;
        Shape g; g.type = ShapeType::Group; g.name = v.name; g.children = shapes;
        pages << QVector<Shape>{g};
    }
    if(pages.isEmpty()) return doc;
    doc.pageSize = page; doc.pageCount = pages.size();
    for(int i = 0; i < pages.size(); ++i){
        Shape g = pages[i].first(); g.transform = QTransform::fromTranslate(0, doc.pageRect(i).top());
        doc.shapes << g;
    }
    if(st){ st->views = pages.size(); st->edges = edges; st->solid = !flat; }
    return doc;
#endif
}

} // namespace xara
