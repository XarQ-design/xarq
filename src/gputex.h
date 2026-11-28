// gputex.h — GPU block-compressed texture decoding (BC1–BC5, BC7, ETC1, ETC2, EAC, ASTC, UASTC) to 8-bit RGBA, shared by
// the texture containers (KTX, KTX2, DDS, PKM, .astc, Basis, Crunch).
#pragma once
#include <QImage>

namespace xara {
namespace gputex {

enum class Format {
    BC1, BC1A, BC2, BC3, BC4, BC4S, BC5, BC5S, BC7,
    BC6H, BC6HS,                                   // half-float RGB (unsigned, signed): shown clamped, in sRGB
    ETC1, ETC2_RGB, ETC2_RGBA1, ETC2_RGBA8, EAC_R11, EAC_R11S, EAC_RG11, EAC_RG11S,
    ASTC,
    UASTC                                          // Basis Universal's UASTC LDR 4x4
};

// Bytes in one block, and the block size in pixels (ASTC's is given; the others are 4×4).
int blockBytes(Format f);
// The data a width × height image needs.
qint64 dataSize(Format f, int width, int height, int blockW = 4, int blockH = 4);
// The image, or a null image if the data is too short or the size too big. ASTC needs its block size;
// srgb only matters to ASTC's decoding of out-of-range values.
QImage decode(Format f, const uchar* data, qint64 size, int width, int height, int blockW = 4, int blockH = 4);

// One block into 16 (or blockW × blockH for ASTC) ARGB pixels, row by row. False for an invalid block,
// which leaves the pixels at the format's error colour.
bool decodeBlock(Format f, const uchar* block, QRgb* out, int blockW = 4, int blockH = 4);
// A BC6H block's 16 texels as half-float RGB bit patterns; false for a reserved mode (zeros).
bool decodeBc6Halves(const uchar* block, bool sgn, quint16 (*out)[3]);
// Basis Universal's UASTC HDR 6x6 intermediate stream; width and height (0 for any) must match its header.
QImage decodeUastcHdr6x6i(const uchar* data, qint64 size, int width = 0, int height = 0);

} // namespace gputex
} // namespace xara
