// textures.h — GPU texture containers (PKM, .astc, KTX 2.0, Basis Universal, Crunch) and the block formats KTX 1 / DDS name.
#pragma once
#include <QImage>
#include <QString>
#include <QVector>

namespace xara {
namespace gputex { enum class Format; }
namespace textures {

QImage decodePkm(const uchar* p, qint64 n);                       // ETC1/ETC2/EAC .pkm (etcpack)
QImage decodeAstcFile(const uchar* p, qint64 n);                  // .astc (ARM): one 2D ASTC image
// KTX 2.0: the first image of the biggest level, uncompressed, block-compressed, zstd or zlib supercompressed.
// note says why a file couldn't be read (Basis Universal data, a build without Zstandard).
QImage decodeKtx2(const uchar* p, qint64 n, QString* note = nullptr);
bool glBlockFormat(quint32 glInternalFormat, gputex::Format* f, int* blockW, int* blockH);   // KTX 1
bool dxgiBlockFormat(quint32 dxgiFormat, gputex::Format* f);                              // DDS DX10
// Basis Universal .basis: the biggest level of every image (a video's frames), ETC1S or UASTC; frameUs is a
// video's frame time. note says why a file couldn't be read (HDR or XUASTC data).
QVector<QImage> decodeBasis(const uchar* p, qint64 n, QString* note = nullptr, int* frameUs = nullptr);
// One image of a KTX 2.0 level supercompressed with BasisLZ, from the file's global data.
QImage decodeBasisLz(const uchar* globalData, qint64 globalSize, int image, const uchar* level, qint64 levelSize, int width, int height);
// Crunch .crn (DXT1/DXT5/DXN/DXT5A and the swizzled DXT5 variants): the biggest level of the first face.
QImage decodeCrn(const uchar* p, qint64 n, QString* note = nullptr);

} // namespace textures
} // namespace xara
