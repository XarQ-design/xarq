// textures.h — GPU texture containers (PKM, .astc, KTX 2.0) and the block formats KTX 1 / DDS name.
#pragma once
#include <QImage>
#include <QString>

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

} // namespace textures
} // namespace xara
