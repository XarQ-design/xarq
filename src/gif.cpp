// gif.cpp — minimal animated GIF89a writer (exact palette when the frames have ≤255 colours, else the
// 216-colour web cube; 1-bit transparency) and an APNG writer.
#include "gif.h"
#include <QFile>
#include <QByteArray>
#include <QHash>
#include <QBuffer>

namespace xara {

// nearest web-216 index for an (r,g,b): 6 levels per channel (0,51,…,255)
static inline int webIndex(int r,int g,int b){
    auto q=[](int c){ return (c*5+127)/255; };   // 0..5
    return q(r)*36 + q(g)*6 + q(b);
}

// GIF variable-width LZW for one frame's index buffer (min code size = 8).
static QByteArray lzwCompress(const QVector<quint8>& px){
    const int minCode=8;
    const int clearCode=1<<minCode;          // 256
    const int eoiCode=clearCode+1;           // 257
    QByteArray out;
    // bit packer → packed into sub-blocks of ≤255 bytes
    QByteArray block; int bitBuf=0, bitCnt=0;
    auto flushBlocks=[&](bool force){
        while(block.size()>=255){ out.append((char)255); out.append(block.left(255)); block.remove(0,255); }
        if(force && !block.isEmpty()){ out.append((char)block.size()); out.append(block); block.clear(); }
    };
    auto emitCode=[&](int code,int width){ bitBuf |= (code<<bitCnt); bitCnt+=width;
        while(bitCnt>=8){ block.append((char)(bitBuf&0xFF)); bitBuf>>=8; bitCnt-=8; flushBlocks(false); } };

    QHash<QByteArray,int> table; table.reserve(4096);
    auto reset=[&]{ table.clear(); for(int i=0;i<clearCode;++i) table.insert(QByteArray(1,(char)i), i); };
    int codeWidth=minCode+1, next=eoiCode+1;
    reset(); emitCode(clearCode,codeWidth);
    if(px.isEmpty()){ emitCode(eoiCode,codeWidth); }
    else {
        QByteArray cur; cur.append((char)px[0]);
        for(int i=1;i<px.size();++i){ char k=(char)px[i]; QByteArray nk=cur; nk.append(k);
            if(table.contains(nk)) cur=nk;
            else { emitCode(table.value(cur),codeWidth); table.insert(nk,next++);
                   if(next == (1<<codeWidth) && codeWidth<12) codeWidth++;
                   if(next>=4096){ emitCode(clearCode,codeWidth); reset(); codeWidth=minCode+1; next=eoiCode+1; }
                   cur=QByteArray(1,k); } }
        emitCode(table.value(cur),codeWidth);
        emitCode(eoiCode,codeWidth);
    }
    if(bitCnt>0){ block.append((char)(bitBuf&0xFF)); }     // flush remaining bits
    flushBlocks(true);
    out.append((char)0);                                    // block terminator
    return out;
}

static void put16(QByteArray& b, int v){ b.append((char)(v&0xFF)); b.append((char)((v>>8)&0xFF)); }

bool writeAnimatedGif(const QString& path, const QVector<QImage>& frames, int delayMs,
                      int loops, const QVector<int>& perFrameMs){
    if(frames.isEmpty()) return false;
    const int W=frames[0].width(), H=frames[0].height();
    QVector<QImage> fs; fs.reserve(frames.size());
    for(const QImage& f0 : frames){ QImage f=f0.convertToFormat(QImage::Format_ARGB32); if(f.width()!=W||f.height()!=H) f=f.scaled(W,H); fs << f; }
    // The palette: the animation's own colours when they fit (pixel art, flat colours), else the web-216 cube.
    // Pixels less than half opaque become the transparent index.
    bool transparent=false, exact=true; QHash<QRgb,int> pal;
    for(const QImage& f : fs) for(int y=0;y<H;++y){ const QRgb* r=(const QRgb*)f.constScanLine(y);
        for(int x=0;x<W;++x){ if(qAlpha(r[x])<128){ transparent=true; continue; }
            if(!exact) continue; const QRgb c=r[x]|0xff000000u;
            if(!pal.contains(c)){ if(pal.size()>=255){ exact=false; continue; } const int k=int(pal.size()); pal.insert(c,k); } } }
    const int clear = exact ? int(pal.size()) : 216;          // transparent index (unused when nothing is transparent)
    QByteArray g;
    g.append("GIF89a");
    put16(g,W); put16(g,H);
    g.append((char)0xF7);   // global colour table, 256 entries (2^(7+1)), 8-bit colour
    g.append((char)(transparent ? clear : 0));   // background colour index
    g.append((char)0);      // pixel aspect ratio
    QVector<QRgb> table(256, qRgb(0,0,0));
    if(exact){ for(auto it=pal.constBegin(); it!=pal.constEnd(); ++it) table[it.value()]=it.key(); }
    else for(int i=0;i<216;++i){ int r=i/36, gg=(i/6)%6, bb=i%6; table[i]=qRgb(r*51,gg*51,bb*51); }
    for(int i=0;i<256;++i){ g.append((char)qRed(table[i])); g.append((char)qGreen(table[i])); g.append((char)qBlue(table[i])); }
    // Netscape looping extension (loops = repeat count; 0 = forever)
    g.append((char)0x21); g.append((char)0xFF); g.append((char)0x0B); g.append("NETSCAPE2.0");
    g.append((char)0x03); g.append((char)0x01); put16(g, qMax(0,loops)); g.append((char)0);
    const bool perFrame = perFrameMs.size()==frames.size();
    for(int fi=0; fi<fs.size(); ++fi){
        const QImage& f = fs[fi];
        const int delayCs = qMax(2, (perFrame ? perFrameMs[fi] : delayMs)/10);   // centiseconds
        // graphic control extension: delay; with transparency each frame clears to the background first
        g.append((char)0x21); g.append((char)0xF9); g.append((char)0x04); g.append((char)(transparent ? 0x09 : 0x04));
        put16(g,delayCs); g.append((char)(transparent ? clear : 0)); g.append((char)0);
        // image descriptor
        g.append((char)0x2C); put16(g,0); put16(g,0); put16(g,W); put16(g,H); g.append((char)0);   // no local table
        QVector<quint8> px; px.resize(W*H);
        for(int y=0;y<H;++y){ const QRgb* r=(const QRgb*)f.constScanLine(y);
            for(int x=0;x<W;++x){ const QRgb p=r[x];
                if(qAlpha(p)<128){ px[y*W+x]=(quint8)clear; continue; }
                px[y*W+x] = exact ? (quint8)pal.value(p|0xff000000u) : (quint8)webIndex(qRed(p),qGreen(p),qBlue(p)); } }
        g.append((char)0x08);                 // LZW minimum code size
        g.append(lzwCompress(px));
    }
    g.append((char)0x3B);                     // trailer
    QFile out(path); if(!out.open(QIODevice::WriteOnly)) return false; out.write(g); out.close();
    return true;
}

// APNG: an ordinary PNG of frame 0 whose chunks are followed by each further frame's image data as fdAT,
// every frame introduced by an fcTL (size, offset, delay, disposal), after an acTL (frame count, plays).
static quint32 crcPng(const QByteArray& b){
    static quint32 t[256]; static bool init=false;
    if(!init){ for(quint32 n=0;n<256;++n){ quint32 c=n; for(int k=0;k<8;++k) c = (c&1) ? 0xEDB88320u^(c>>1) : c>>1; t[n]=c; } init=true; }
    quint32 c=0xFFFFFFFFu; for(uchar ch : b) c=t[(c^ch)&0xff]^(c>>8); return c^0xFFFFFFFFu;
}
static void be32(QByteArray& b, quint32 v){ b.append((char)(v>>24)); b.append((char)(v>>16)); b.append((char)(v>>8)); b.append((char)v); }
static void chunk(QByteArray& out, const char* type, const QByteArray& data){
    be32(out, quint32(data.size())); QByteArray td(type,4); td+=data; out+=td; be32(out, crcPng(td));
}
// The concatenated IDAT payload of a PNG encoding of img, plus its IHDR data.
static bool pngParts(const QImage& img, QByteArray* ihdr, QByteArray* idat){
    QByteArray png; { QBuffer b(&png); b.open(QIODevice::WriteOnly); if(!img.save(&b,"PNG")) return false; }
    int o=8;
    while(o+12<=png.size()){
        const quint32 len = quint32(uchar(png[o]))<<24 | quint32(uchar(png[o+1]))<<16 | quint32(uchar(png[o+2]))<<8 | uchar(png[o+3]);
        if(len > quint32(png.size()-o-12)) return false;
        const QByteArray type=png.mid(o+4,4), data=png.mid(o+8,int(len));
        if(type=="IHDR") *ihdr=data; else if(type=="IDAT") *idat+=data;
        o+=12+int(len);
    }
    return !ihdr->isEmpty() && !idat->isEmpty();
}

bool writeApng(const QString& path, const QVector<QImage>& frames, const QVector<int>& delaysMs, int loops){
    if(frames.isEmpty()) return false;
    const int W=frames[0].width(), H=frames[0].height();
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    quint32 seq=0;
    for(int i=0;i<frames.size();++i){
        QImage f=frames[i].convertToFormat(QImage::Format_ARGB32); if(f.width()!=W||f.height()!=H) f=f.scaled(W,H);
        QByteArray ihdr, idat; if(!pngParts(f,&ihdr,&idat)) return false;
        if(i==0){
            chunk(out,"IHDR",ihdr);
            QByteArray actl; be32(actl,quint32(frames.size())); be32(actl,quint32(qMax(0,loops))); chunk(out,"acTL",actl);
        }
        const int ms = qBound(1, i<delaysMs.size() ? delaysMs[i] : 100, 65535);
        QByteArray fctl; be32(fctl,seq++); be32(fctl,W); be32(fctl,H); be32(fctl,0); be32(fctl,0);
        fctl.append((char)(ms>>8)); fctl.append((char)ms); fctl.append((char)0x03); fctl.append((char)0xE8);   // delay ms/1000
        fctl.append((char)1); fctl.append((char)0);                                                           // clear to transparent; replace
        chunk(out,"fcTL",fctl);
        if(i==0) chunk(out,"IDAT",idat);
        else { QByteArray fd; be32(fd,seq++); fd+=idat; chunk(out,"fdAT",fd); }
    }
    chunk(out,"IEND",QByteArray());
    QFile f(path); if(!f.open(QIODevice::WriteOnly)) return false;
    return f.write(out)==out.size();
}

} // namespace xara
