// Frame sequences from formats that are animations or picture sets: Windows animated cursors (.ani) and
// multi-picture JPEG (.mpo).
//
// ANI is a RIFF "ACON": an "anih" header (frame and step counts, default rate in 1/60 s "jiffies", flags),
// optional "rate" (jiffies per step) and "seq " (frame per step) tables, and a LIST "fram" of "icon" chunks,
// each a whole .ico or .cur file. Some writers miscount chunk sizes (the RIFF's own, or a list's by a byte or
// two), so the file is read to its end and a chunk that doesn't start where expected is looked for nearby.
//
// MPO is a JPEG followed by more JPEGs; the first one's APP2 "MPF" segment (a little TIFF) indexes them, each
// entry giving a type, size and offset from that segment's TIFF header. Large-thumbnail entries are previews,
// not pictures of their own.
#include "prepress.h"
#include <QBuffer>
#include <QImageReader>
#include <cstring>
#include <functional>

namespace xara {
namespace {

quint32 le32(const uchar* p){ return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }
bool fourcc(const uchar* p){ for(int k = 0; k < 4; ++k) if(p[k] < 0x20 || p[k] > 0x7e) return false; return true; }

// The largest picture in an .ico / .cur file.
QImage largestIcon(const QByteArray& data){
    QBuffer buf; buf.setData(data); buf.open(QIODevice::ReadOnly);
    QImageReader r(&buf, "ico");
    QImage best;
    for(int k = 0, n = qMax(1, r.imageCount()); k < n && k < 64; ++k){
        if(k > 0 && !r.jumpToImage(k)) break;
        const QImage img = r.read();
        if(!img.isNull() && qint64(img.width()) * img.height() > qint64(best.width()) * best.height()) best = img;
    }
    if(best.isNull()) best.loadFromData(data);                // a plain bitmap stored without the icon wrapper
    return best.isNull() ? best : best.convertToFormat(QImage::Format_ARGB32);
}

struct Chunk { QByteArray id, list; qint64 at = 0, size = 0; };

// The chunks between o and end, tolerating sizes that are off by a little.
QVector<Chunk> chunks(const uchar* p, qint64 o, qint64 end){
    QVector<Chunk> out;
    while(o + 8 <= end && out.size() < 100000){
        if(!fourcc(p + o)){
            bool found = false;
            for(int d : {-2, 2, -1, 1, -3, 3, -4, 4}) if(o + d >= 0 && o + d + 8 <= end && fourcc(p + o + d)){ o += d; found = true; break; }
            if(!found) break;
        }
        Chunk c; c.id = QByteArray(reinterpret_cast<const char*>(p + o), 4); c.size = le32(p + o + 4); c.at = o + 8;
        if(c.size > end - c.at) c.size = end - c.at;
        if((c.id == "LIST" || c.id == "RIFF") && c.size >= 4) c.list = QByteArray(reinterpret_cast<const char*>(p + c.at), 4);
        out << c;
        o = c.at + c.size + (c.size & 1);
    }
    return out;
}

} // namespace

bool decodeAni(const QByteArray& data, FrameSet* out){
    const uchar* p = reinterpret_cast<const uchar*>(data.constData());
    const qint64 n = data.size();
    if(n < 12 || std::memcmp(p, "RIFF", 4) || std::memcmp(p + 8, "ACON", 4)) return false;
    quint32 steps = 0, rate = 10, flags = 0;
    QVector<quint32> rates, seq; QVector<QImage> icons;
    std::function<void(qint64, qint64, int)> walk = [&](qint64 o, qint64 end, int depth){
        for(const Chunk& c : chunks(p, o, end)){
            if(c.id == "anih" && c.size >= 36){ steps = le32(p + c.at + 8); rate = le32(p + c.at + 28); flags = le32(p + c.at + 32); if(!steps) steps = le32(p + c.at + 4); }
            else if(c.id == "rate") for(qint64 k = 0; k + 4 <= c.size && rates.size() < 100000; k += 4) rates << le32(p + c.at + k);
            else if(c.id == "seq ") for(qint64 k = 0; k + 4 <= c.size && seq.size() < 100000; k += 4) seq << le32(p + c.at + k);
            else if(c.id == "icon" && icons.size() < 4096) icons << largestIcon(QByteArray(reinterpret_cast<const char*>(p + c.at), int(c.size)));
            else if(c.id == "LIST" && depth < 4){
                if(c.list == "INFO") for(const Chunk& i : chunks(p, c.at + 4, c.at + c.size)){
                    const QString s = QString::fromLatin1(reinterpret_cast<const char*>(p + i.at), int(qstrnlen(reinterpret_cast<const char*>(p + i.at), size_t(i.size))));
                    if(i.id == "INAM") out->title = s; else if(i.id == "IART") out->author = s;
                }
                else walk(c.at + 4, c.at + c.size, depth + 1);
            }
        }
    };
    walk(12, n, 0);                                   // to the end of the file: some writers understate the RIFF size
    if(icons.isEmpty()) return false;
    Q_UNUSED(flags);
    if(steps == 0 || steps > 100000) steps = seq.isEmpty() ? quint32(icons.size()) : quint32(seq.size());
    for(quint32 s = 0; s < steps; ++s){
        const int f = s < quint32(seq.size()) ? int(seq[s]) : int(s);
        if(f < 0 || f >= icons.size() || icons[f].isNull()) continue;
        const quint32 j = s < quint32(rates.size()) ? rates[s] : rate;
        out->frames << icons[f];
        out->delays << qBound(10, int(qMax<quint32>(j, 1) * 1000 / 60), 600000);   // jiffies are 1/60 s
        out->names << QStringLiteral("Frame %1").arg(f + 1);
    }
    return !out->frames.isEmpty();
}

bool decodeMpo(const QByteArray& data, FrameSet* out){
    const uchar* p = reinterpret_cast<const uchar*>(data.constData());
    const qint64 n = data.size();
    if(n < 4 || p[0] != 0xff || p[1] != 0xd8) return false;
    // Find the first picture's APP2 "MPF" segment.
    qint64 o = 2, mpf = -1, mpfLen = 0;
    while(o + 4 <= n && p[o] == 0xff){
        const int m = p[o + 1];
        if(m == 0xd9 || m == 0xda) break;
        const qint64 len = qint64(p[o + 2]) << 8 | p[o + 3];
        if(len < 2 || o + 2 + len > n) break;
        if(m == 0xe2 && len >= 8 && !std::memcmp(p + o + 4, "MPF\0", 4)){ mpf = o + 8; mpfLen = len - 6; break; }
        o += 2 + len;
    }
    struct Entry { quint32 type, size, offset; };
    QVector<Entry> entries;
    if(mpf >= 0 && mpfLen >= 8){
        const uchar* t = p + mpf; const qint64 tn = mpfLen;
        const bool be = t[0] == 'M';
        auto u16 = [&](qint64 a)->quint32 { if(a < 0 || a + 2 > tn) return 0; return be ? quint32(t[a]) << 8 | t[a + 1] : quint32(t[a + 1]) << 8 | t[a]; };
        auto u32 = [&](qint64 a)->quint32 { if(a < 0 || a + 4 > tn) return 0; return be ? quint32(t[a]) << 24 | quint32(t[a + 1]) << 16 | quint32(t[a + 2]) << 8 | t[a + 3] : le32(t + a); };
        const qint64 ifd = u32(4);
        const int count = int(u16(ifd));
        quint32 entryCount = 0, entryAt = 0;
        for(int k = 0; k < count && k < 64; ++k){
            const qint64 e = ifd + 2 + k * 12;
            const quint32 tag = u16(e);
            if(tag == 0xb001) entryCount = u32(e + 8);
            else if(tag == 0xb002) entryAt = u32(e + 8);
        }
        for(quint32 k = 0; k < entryCount && k < 256; ++k){
            const qint64 e = qint64(entryAt) + k * 16;
            if(e + 16 > tn) break;
            Entry en{ u32(e) & 0x00ffffff, u32(e + 4), u32(e + 8) };
            entries << en;
        }
    }
    QVector<QByteArray> pictures;
    for(int k = 0; k < entries.size(); ++k){
        const Entry& e = entries[k];
        if(e.type == 0x010001 || e.type == 0x010002) continue;      // a large thumbnail, not a picture of its own
        const qint64 at = k == 0 ? 0 : mpf + qint64(e.offset);
        if(at < 0 || at + 2 > n || p[at] != 0xff || p[at + 1] != 0xd8) continue;
        const qint64 size = (k == 0 || e.size == 0) ? n - at : qMin<qint64>(e.size, n - at);
        pictures << data.mid(int(at), int(size));
    }
    if(pictures.size() < 2){                                        // no usable index: each further JPEG start marker
        pictures.clear();
        qint64 start = 0;
        for(qint64 k = 2; k + 3 < n; ++k)
            if(p[k] == 0xff && p[k + 1] == 0xd8 && p[k + 2] == 0xff && (p[k + 3] & 0xf0) == 0xe0 && p[k - 1] != 0xff){
                pictures << data.mid(int(start), int(k - start)); start = k;
                if(pictures.size() >= 64) break;
            }
        pictures << data.mid(int(start));
    }
    for(int k = 0; k < pictures.size(); ++k){
        QImage img; img.loadFromData(pictures[k], "JPEG");
        if(img.isNull()) continue;
        out->frames << img.convertToFormat(QImage::Format_ARGB32);
        out->delays << 150;
        out->names << (pictures.size() == 2 ? QString(k == 0 ? QStringLiteral("Left") : QStringLiteral("Right")) : QStringLiteral("Picture %1").arg(k + 1));
    }
    return out->frames.size() >= 2;
}

} // namespace xara
