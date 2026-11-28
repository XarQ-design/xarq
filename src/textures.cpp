// GPU texture containers: PKM (ETC from etcpack), .astc (ARM's ASTC files), KTX 2.0 (Khronos), and the block
// formats KTX 1 and DDS name by OpenGL internal format or DXGI format. The pixels go through gputex.
#include "textures.h"
#include "gputex.h"
#include <QByteArray>
#include <cmath>
#include <cstring>
#include <zlib.h>
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

namespace xara {
namespace textures {
namespace {

quint32 le32(const uchar* p){ return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }
quint64 le64(const uchar* p){ return quint64(le32(p)) | quint64(le32(p + 4)) << 32; }
quint32 be16(const uchar* p){ return quint32(p[0]) << 8 | p[1]; }

// Half and full floats, linear light, to sRGB bytes.
int encodeSrgb(float v){
    if(!(v > 0.0f)) return 0;
    if(v >= 1.0f) return 255;
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return int(s * 255.0f + 0.5f);
}
float half(quint32 h){
    const int e = (h >> 10) & 31, m = h & 1023; const float sgn = (h & 0x8000) ? -1.0f : 1.0f;
    if(e == 0) return sgn * std::ldexp(float(m), -24);
    if(e == 31) return m ? 0.0f : sgn * 65504.0f;
    return sgn * std::ldexp(float(m | 1024), e - 25);
}

// A Vulkan format as a block format (with its block size), or an uncompressed layout.
bool blockFormat(quint32 vk, gputex::Format* f, int* bw, int* bh){
    using F = gputex::Format;
    *bw = *bh = 4;
    switch(vk){
    case 131: case 132: *f = F::BC1; return true;
    case 133: case 134: *f = F::BC1A; return true;
    case 135: case 136: *f = F::BC2; return true;
    case 137: case 138: *f = F::BC3; return true;
    case 139: *f = F::BC4; return true;  case 140: *f = F::BC4S; return true;
    case 141: *f = F::BC5; return true;  case 142: *f = F::BC5S; return true;
    case 145: case 146: *f = F::BC7; return true;
    case 143: *f = F::BC6H; return true;
    case 144: *f = F::BC6HS; return true;
    case 147: case 148: *f = F::ETC2_RGB; return true;
    case 149: case 150: *f = F::ETC2_RGBA1; return true;
    case 151: case 152: *f = F::ETC2_RGBA8; return true;
    case 153: *f = F::EAC_R11; return true;  case 154: *f = F::EAC_R11S; return true;
    case 155: *f = F::EAC_RG11; return true; case 156: *f = F::EAC_RG11S; return true;
    }
    if(vk >= 1000066000 && vk <= 1000066013){                         // ASTC with half-float (HDR) texels, the same size order
        static const int dims[14][2] = { {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12} };
        const int k = int(vk - 1000066000); *f = F::ASTC; *bw = dims[k][0]; *bh = dims[k][1]; return true;
    }
    if(vk >= 157 && vk <= 184){                                       // ASTC, unorm/srgb pairs in a fixed size order
        static const int dims[14][2] = { {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12} };
        const int k = int(vk - 157) / 2; *f = F::ASTC; *bw = dims[k][0]; *bh = dims[k][1]; return true;
    }
    return false;
}

QImage uncompressed(quint32 vk, const uchar* s, qint64 n, int w, int h){
    int bpp = 0;
    switch(vk){ case 9: case 15: bpp = 1; break; case 16: case 22: case 4: case 5: case 2: bpp = 2; break; case 23: case 29: case 30: case 36: bpp = 3; break;
                case 37: case 43: case 44: case 50: case 64: case 122: bpp = 4; break; case 90: bpp = 6; break; case 91: case 97: bpp = 8; break; case 109: bpp = 16; break; default: return QImage(); }
    if(qint64(w) * h * bpp > n) return QImage();
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return img;
    for(int y = 0; y < h; ++y){
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
        const uchar* q = s + qint64(y) * w * bpp;
        for(int x = 0; x < w; ++x, q += bpp){
            switch(vk){
            case 9: case 15: d[x] = qRgb(q[0], q[0], q[0]); break;
            case 16: case 22: d[x] = qRgb(q[0], q[1], 0); break;
            case 4: { const quint32 v = q[0] | q[1] << 8; d[x] = qRgb(int((v >> 11) * 255 / 31), int(((v >> 5) & 63) * 255 / 63), int((v & 31) * 255 / 31)); } break;
            case 5: { const quint32 v = q[0] | q[1] << 8; d[x] = qRgb(int((v & 31) * 255 / 31), int(((v >> 5) & 63) * 255 / 63), int((v >> 11) * 255 / 31)); } break;
            case 90: d[x] = qRgb(encodeSrgb(half(q[0] | q[1] << 8)), encodeSrgb(half(q[2] | q[3] << 8)), encodeSrgb(half(q[4] | q[5] << 8))); break;
            case 122: { const quint32 v = le32(q);                        // 11-, 11- and 10-bit unsigned floats
                auto uf = [](quint32 bits, int mant){ const int e = int(bits >> mant) & 31; const float m = float(bits & ((1u << mant) - 1)) / float(1 << mant);
                                                      return e == 0 ? std::ldexp(m, -14) : e == 31 ? 65504.0f : std::ldexp(1.0f + m, e - 15); };
                d[x] = qRgb(encodeSrgb(uf(v & 2047, 6)), encodeSrgb(uf((v >> 11) & 2047, 6)), encodeSrgb(uf(v >> 22, 5))); } break;
            case 2: { const quint32 v = q[0] | q[1] << 8; d[x] = qRgba(int((v >> 12) * 17), int(((v >> 8) & 15) * 17), int(((v >> 4) & 15) * 17), int((v & 15) * 17)); } break;
            case 23: case 29: d[x] = qRgb(q[0], q[1], q[2]); break;
            case 30: case 36: d[x] = qRgb(q[2], q[1], q[0]); break;
            case 37: case 43: d[x] = qRgba(q[0], q[1], q[2], q[3]); break;
            case 44: case 50: d[x] = qRgba(q[2], q[1], q[0], q[3]); break;
            case 64: { const quint32 v = le32(q); d[x] = qRgba(int((v & 1023) >> 2), int(((v >> 10) & 1023) >> 2), int(((v >> 20) & 1023) >> 2), int((v >> 30) * 85)); } break;
            case 91: d[x] = qRgba(q[1], q[3], q[5], q[7]); break;
            case 97: d[x] = qRgba(encodeSrgb(half(q[0] | q[1] << 8)), encodeSrgb(half(q[2] | q[3] << 8)), encodeSrgb(half(q[4] | q[5] << 8)), qBound(0, int(half(q[6] | q[7] << 8) * 255 + 0.5f), 255)); break;
            case 109: { float f[4]; std::memcpy(f, q, 16); d[x] = qRgba(encodeSrgb(f[0]), encodeSrgb(f[1]), encodeSrgb(f[2]), qBound(0, int(f[3] * 255 + 0.5f), 255)); } break;
            }
        }
    }
    return img;
}

} // namespace

QImage decodePkm(const uchar* p, qint64 n){
    if(n < 16 || std::memcmp(p, "PKM ", 4)) return QImage();
    using F = gputex::Format;
    static const F kinds[12] = { F::ETC1, F::ETC2_RGB, F::ETC2_RGBA8, F::ETC2_RGBA8, F::ETC2_RGBA1, F::EAC_R11, F::EAC_RG11, F::EAC_R11S, F::EAC_RG11S,
                                 F::ETC2_RGB, F::ETC2_RGBA8, F::ETC2_RGBA1 };
    const quint32 type = be16(p + 6);
    if(type >= 12) return QImage();
    const int w = int(be16(p + 12)), h = int(be16(p + 14));
    return gputex::decode(kinds[type], p + 16, n - 16, w, h);
}

QImage decodeAstcFile(const uchar* p, qint64 n){
    if(n < 16 || le32(p) != 0x5CA1AB13u) return QImage();
    const int bw = p[4], bh = p[5], bd = p[6];
    const int w = int(p[7] | p[8] << 8 | p[9] << 16), h = int(p[10] | p[11] << 8 | p[12] << 16);
    if(bd != 1) return QImage();                                       // 3D blocks
    return gputex::decode(gputex::Format::ASTC, p + 16, n - 16, w, h, bw, bh);
}

QImage decodeKtx2(const uchar* p, qint64 n, QString* note){
    static const uchar id[12] = {0xAB,'K','T','X',' ','2','0',0xBB,0x0D,0x0A,0x1A,0x0A};
    if(n < 80 || std::memcmp(p, id, 12)) return QImage();
    const quint32 vk = le32(p + 12), w = le32(p + 20), h = qMax<quint32>(1, le32(p + 24)), levels = qMax<quint32>(1, le32(p + 40)), scheme = le32(p + 44);
    if(w < 1 || w > 65536 || h > 65536 || qint64(w) * h > (qint64(1) << 28)) return QImage();
    if(80 + 24 > n) return QImage();
    Q_UNUSED(levels);
    const quint64 off = le64(p + 80), len = le64(p + 88), full = le64(p + 96);   // level 0, the biggest
    if(off > quint64(n) || len > quint64(n) - off) return QImage();
    QByteArray unpacked; const uchar* data = p + off; qint64 size = qint64(len);
    if(scheme == 2){
#ifdef HAVE_ZSTD
        if(full == 0 || full > (quint64(1) << 31)) return QImage();
        unpacked.resize(int(full));
        const size_t got = ZSTD_decompress(unpacked.data(), size_t(full), data, size_t(len));
        if(ZSTD_isError(got)) return QImage();
        data = reinterpret_cast<const uchar*>(unpacked.constData()); size = qint64(got);
#else
        if(note) *note = QStringLiteral("this KTX2 file is Zstandard-compressed, which this build can't unpack");
        return QImage();
#endif
    } else if(scheme == 3){
        if(full == 0 || full > (quint64(1) << 31) || len > 0x7fffffff) return QImage();
        unpacked.resize(int(full)); uLongf out = uLongf(full);
        if(uncompress(reinterpret_cast<Bytef*>(unpacked.data()), &out, data, uLong(len)) != Z_OK) return QImage();
        data = reinterpret_cast<const uchar*>(unpacked.constData()); size = qint64(out);
    } else if(scheme == 1){                                                    // BasisLZ: ETC1S against the global codebooks
        const quint64 gOff = le64(p + 64), gLen = le64(p + 72);
        if(gOff > quint64(n) || gLen > quint64(n) - gOff) return QImage();
        return decodeBasisLz(p + gOff, qint64(gLen), 0, data, size, int(w), int(h));
    } else if(scheme == 4){                                                    // UASTC HDR 6x6 intermediate: a stream of its own
        return gputex::decodeUastcHdr6x6i(data, size, int(w), int(h));
    } else if(scheme != 0){
        if(note) *note = QStringLiteral("an unknown supercompression");
        return QImage();
    }
    gputex::Format f; int bw, bh;
    if(blockFormat(vk, &f, &bw, &bh)) return gputex::decode(f, data, size, int(w), int(h), bw, bh);
    if(vk == 0){                                                               // UASTC: its colour model in the data format descriptor
        const quint32 dfd = le32(p + 48);
        if(quint64(dfd) + 13 <= quint64(n) && p[dfd + 12] == 166) return gputex::decode(gputex::Format::UASTC, data, size, int(w), int(h));
    }
    return uncompressed(vk, data, size, int(w), int(h));
}

bool glBlockFormat(quint32 gl, gputex::Format* f, int* bw, int* bh){
    using F = gputex::Format;
    *bw = *bh = 4;
    switch(gl){
    case 0x83F0: *f = F::BC1; return true; case 0x83F1: *f = F::BC1A; return true; case 0x83F2: *f = F::BC2; return true; case 0x83F3: *f = F::BC3; return true;
    case 0x8C4C: *f = F::BC1; return true; case 0x8C4D: *f = F::BC1A; return true; case 0x8C4E: *f = F::BC2; return true; case 0x8C4F: *f = F::BC3; return true;
    case 0x8DBB: *f = F::BC4; return true; case 0x8DBC: *f = F::BC4S; return true; case 0x8DBD: *f = F::BC5; return true; case 0x8DBE: *f = F::BC5S; return true;
    case 0x8E8C: case 0x8E8D: *f = F::BC7; return true;
    case 0x8E8F: *f = F::BC6H; return true;                         // GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT
    case 0x8E8E: *f = F::BC6HS; return true;
    case 0x8D64: *f = F::ETC1; return true;
    case 0x9274: case 0x9275: *f = F::ETC2_RGB; return true;
    case 0x9276: case 0x9277: *f = F::ETC2_RGBA1; return true;
    case 0x9278: case 0x9279: *f = F::ETC2_RGBA8; return true;
    case 0x9270: *f = F::EAC_R11; return true; case 0x9271: *f = F::EAC_R11S; return true;
    case 0x9272: *f = F::EAC_RG11; return true; case 0x9273: *f = F::EAC_RG11S; return true;
    }
    static const int dims[14][2] = { {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12} };
    if((gl >= 0x93B0 && gl <= 0x93BD) || (gl >= 0x93D0 && gl <= 0x93DD)){ const int k = int(gl & 15); *f = F::ASTC; *bw = dims[k][0]; *bh = dims[k][1]; return true; }
    return false;
}

bool dxgiBlockFormat(quint32 dxgi, gputex::Format* f){
    using F = gputex::Format;
    switch(dxgi){
    case 70: case 71: case 72: *f = F::BC1A; return true;
    case 73: case 74: case 75: *f = F::BC2; return true;
    case 76: case 77: case 78: *f = F::BC3; return true;
    case 79: case 80: *f = F::BC4; return true; case 81: *f = F::BC4S; return true;
    case 82: case 83: *f = F::BC5; return true; case 84: *f = F::BC5S; return true;
    case 97: case 98: case 99: *f = F::BC7; return true;
    case 94: case 95: *f = F::BC6H; return true;
    case 96: *f = F::BC6HS; return true;
    }
    return false;
}

} // namespace textures
} // namespace xara
