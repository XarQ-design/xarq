// UASTC HDR 6x6 intermediate (Basis Universal's "GPU Photo 6x6"), after its published specification: a bit stream,
// least significant bit first, of commands that each give one or more ASTC HDR 6x6 blocks in raster order: runs of
// the previous block, solid half-float colours, blocks reusing a nearby block's configuration and endpoints with
// new weights, and blocks of their own (one of 75 configurations, endpoints raw or taken from the left or upper
// block, optionally with small deltas). Coded values can use ranges and weight grids standard ASTC doesn't allow,
// so they're requantized (keeping the endpoint bits that choose the HDR sub-mode) and 2x2 grids upsampled before
// the block is evaluated as ASTC. Included into gputex.cpp after astc.inc.

struct Hdr6Mode { int dual, cem, parts, gx, gy, endRange, weightRange, outEndRange, outWeightRange, channel; };
// The 75 block configurations: dual plane, endpoint mode, subsets, weight grid, coded endpoint and weight ranges,
// the ranges a standard block needs, and the dual-plane channel (from the UASTC HDR 6x6i specification, Apache 2.0).
const Hdr6Mode kHdr6Modes[75] = {
    {0,11,1,6,6,20,1,20,1,0}, {0,11,1,6,6,15,2,15,2,0}, {0,11,1,6,5,16,3,16,3,0}, {0,11,1,5,6,16,3,16,3,0},
    {0,11,1,6,4,15,5,15,5,0}, {0,11,1,4,6,15,5,15,5,0}, {0,11,1,6,3,15,8,15,8,0}, {0,11,1,3,6,15,8,15,8,0},
    {0,11,1,5,5,14,5,14,5,0}, {0,11,1,4,4,19,8,19,8,0}, {0,11,1,3,3,20,8,20,8,0}, {0,7,1,6,6,16,3,16,3,0},
    {0,7,1,6,6,20,1,20,1,0}, {0,7,1,6,6,20,2,20,2,0}, {0,7,1,5,6,20,4,20,4,0}, {0,7,1,6,5,20,4,20,4,0},
    {0,7,1,3,6,20,9,20,9,0}, {0,7,1,6,3,20,9,20,9,0}, {0,11,2,6,6,11,0,11,0,0}, {0,11,2,6,3,13,1,13,1,0},
    {0,11,2,3,6,13,1,13,1,0}, {0,11,2,3,6,11,2,11,2,0}, {0,11,2,6,3,11,2,11,2,0}, {0,11,2,4,6,11,1,11,1,0},
    {0,11,2,6,4,11,1,11,1,0}, {0,7,2,5,6,15,1,15,1,0}, {0,7,2,6,5,15,1,15,1,0}, {0,7,2,4,6,15,2,15,2,0},
    {0,7,2,6,4,15,2,15,2,0}, {0,7,2,6,6,11,1,11,1,0}, {0,7,2,6,6,19,0,19,0,0}, {0,7,2,5,5,14,2,14,2,0},
    {0,7,2,3,6,13,5,13,5,0}, {0,7,2,6,3,13,5,13,5,0}, {0,7,2,3,6,15,4,15,4,0}, {0,7,2,6,3,15,4,15,4,0},
    {1,11,1,3,6,14,2,14,2,0}, {1,11,1,3,6,14,2,14,2,1}, {1,11,1,3,6,14,2,14,2,2}, {1,11,1,6,3,14,2,14,2,0},
    {1,11,1,6,3,14,2,14,2,1}, {1,11,1,6,3,14,2,14,2,2}, {1,11,1,3,3,14,8,14,8,0}, {1,11,1,3,3,14,8,14,8,1},
    {1,11,1,3,3,14,8,14,8,2}, {1,11,1,4,4,13,3,13,3,0}, {1,11,1,4,4,13,3,13,3,1}, {1,11,1,4,4,13,3,13,3,2},
    {1,11,1,5,5,20,0,20,0,0}, {1,11,1,5,5,20,0,20,0,1}, {1,11,1,5,5,20,0,20,0,2}, {1,11,1,2,2,14,2,20,5,0},
    {1,11,1,2,2,14,2,20,5,1}, {1,11,1,2,2,14,2,20,5,2}, {0,11,1,2,2,17,0,20,1,0}, {0,7,3,6,6,11,0,11,0,0},
    {0,7,3,5,5,14,0,14,0,0}, {0,7,3,4,4,14,1,14,1,0}, {0,7,3,4,4,12,2,12,2,0}, {0,7,3,4,4,11,3,11,3,0},
    {0,7,3,3,3,14,5,14,5,0}, {0,7,3,6,4,14,0,14,0,0}, {0,7,3,4,6,14,0,14,0,0}, {0,7,3,6,4,11,1,11,1,0},
    {0,7,3,4,6,11,1,11,1,0}, {0,7,3,6,5,13,0,13,0,0}, {0,7,3,5,6,13,0,13,0,0}, {0,7,3,6,3,13,1,13,1,0},
    {0,7,3,3,6,13,1,13,1,0}, {0,7,3,6,3,11,2,11,2,0}, {0,7,3,3,6,11,2,11,2,0}, {0,7,3,6,3,10,3,10,3,0},
    {0,7,3,3,6,10,3,10,3,0}, {0,7,3,5,4,12,1,12,1,0}, {0,7,3,4,5,12,1,12,1,0},
};
const quint16 kHdr6Part2[521] = {
    86, 959, 936, 476, 1007, 672, 447, 423, 488, 422, 273, 65, 267, 786, 585, 195, 108, 731, 878, 812, 264, 125, 868, 581,
    258, 390, 549, 872, 661, 352, 645, 543, 988, 906, 903, 616, 482, 529, 3, 286, 272, 303, 151, 504, 498, 260, 79, 66,
    608, 769, 305, 610, 1014, 967, 835, 789, 7, 951, 691, 15, 763, 976, 438, 314, 601, 673, 177, 252, 615, 436, 220, 899,
    623, 433, 674, 278, 797, 107, 847, 114, 470, 760, 821, 490, 329, 945, 387, 471, 225, 172, 83, 418, 966, 439, 316, 247,
    43, 343, 625, 798, 1, 61, 73, 307, 136, 474, 42, 664, 1013, 249, 389, 227, 374, 121, 48, 538, 226, 309, 554, 802,
    834, 335, 495, 10, 955, 461, 293, 508, 153, 101, 63, 139, 31, 687, 132, 174, 324, 545, 289, 39, 178, 594, 963, 854,
    222, 323, 998, 964, 598, 475, 720, 1019, 983, 91, 703, 614, 394, 612, 281, 207, 930, 758, 586, 128, 517, 426, 306, 168,
    713, 36, 458, 876, 368, 780, 5, 9, 214, 109, 553, 726, 175, 103, 753, 684, 44, 665, 53, 500, 367, 611, 119, 732,
    639, 326, 203, 156, 686, 910, 255, 62, 392, 591, 112, 88, 213, 19, 1022, 478, 90, 486, 799, 702, 730, 414, 99, 1008,
    142, 886, 373, 216, 69, 393, 299, 648, 415, 822, 912, 110, 567, 550, 693, 2, 138, 59, 271, 562, 295, 714, 719, 199,
    893, 831, 1006, 662, 235, 262, 78, 51, 902, 298, 190, 169, 583, 347, 890, 958, 909, 49, 987, 696, 633, 480, 50, 764,
    826, 1023, 1016, 437, 891, 774, 257, 724, 791, 526, 593, 690, 638, 858, 895, 794, 995, 130, 87, 877, 819, 318, 649, 376,
    211, 284, 937, 370, 688, 229, 994, 115, 842, 60, 521, 95, 694, 804, 146, 754, 487, 55, 17, 770, 450, 223, 4, 137,
    911, 236, 683, 523, 47, 181, 24, 270, 602, 736, 11, 355, 148, 351, 762, 1009, 16, 210, 619, 805, 874, 807, 887, 403,
    999, 810, 27, 402, 551, 135, 778, 33, 409, 993, 71, 363, 159, 183, 77, 596, 670, 380, 968, 811, 404, 348, 539, 158,
    578, 196, 621, 68, 530, 193, 100, 167, 919, 353, 366, 327, 643, 948, 518, 756, 801, 558, 28, 705, 116, 94, 898, 453,
    622, 647, 231, 445, 652, 230, 191, 277, 292, 254, 198, 766, 386, 232, 29, 70, 942, 740, 291, 607, 411, 496, 839, 8,
    675, 319, 742, 21, 547, 627, 716, 663, 23, 914, 631, 595, 499, 685, 950, 510, 54, 587, 432, 45, 646, 25, 122, 947,
    171, 862, 441, 808, 722, 14, 74, 658, 129, 266, 1001, 534, 395, 527, 250, 206, 237, 67, 897, 634, 572, 569, 533, 37,
    341, 89, 463, 419, 75, 134, 283, 943, 519, 362, 144, 681, 407, 954, 131, 455, 934, 46, 513, 339, 194, 361, 606, 852,
    546, 655, 1015, 147, 506, 240, 56, 836, 76, 98, 600, 430, 388, 980, 695, 817, 279, 58, 215, 149, 170, 531, 870, 18,
    727, 154, 26, 938, 929, 302, 697, 452, 218, 700, 524, 828, 751, 869, 217, 440, 354,
};
const quint16 kHdr6Part3[333] = {
    0, 8, 11, 14, 15, 17, 18, 19, 26, 31, 34, 35, 36, 38, 44, 47, 48, 49, 51, 56, 59, 61, 70, 74,
    76, 82, 88, 90, 96, 100, 103, 104, 108, 110, 111, 117, 122, 123, 126, 127, 132, 133, 135, 139, 147, 150, 151, 152,
    156, 157, 163, 166, 168, 171, 175, 176, 179, 181, 182, 183, 186, 189, 192, 199, 203, 205, 207, 210, 214, 216, 222, 247,
    249, 250, 252, 254, 260, 261, 262, 263, 266, 272, 273, 275, 276, 288, 291, 292, 293, 294, 297, 302, 309, 310, 313, 314,
    318, 327, 328, 331, 335, 337, 346, 356, 357, 358, 363, 365, 368, 378, 381, 384, 386, 390, 391, 392, 396, 397, 398, 399,
    401, 410, 411, 419, 427, 430, 431, 437, 439, 440, 451, 455, 457, 458, 459, 460, 462, 468, 470, 471, 472, 474, 475, 477,
    479, 482, 483, 488, 493, 495, 496, 502, 503, 504, 507, 510, 511, 512, 515, 516, 518, 519, 522, 523, 525, 526, 527, 538,
    543, 544, 546, 547, 549, 550, 552, 553, 554, 562, 570, 578, 579, 581, 582, 588, 589, 590, 593, 595, 600, 606, 611, 613,
    618, 623, 625, 632, 637, 638, 645, 646, 650, 651, 658, 659, 662, 666, 667, 669, 670, 678, 679, 685, 686, 687, 688, 691,
    694, 696, 698, 699, 700, 701, 703, 704, 707, 713, 714, 715, 717, 719, 722, 724, 727, 730, 731, 734, 738, 739, 743, 747,
    748, 750, 751, 753, 758, 760, 764, 766, 769, 775, 776, 783, 784, 785, 787, 791, 793, 798, 799, 802, 804, 805, 806, 807,
    808, 809, 810, 813, 822, 823, 825, 831, 835, 837, 838, 839, 840, 842, 845, 846, 848, 853, 854, 858, 859, 860, 866, 874,
    882, 884, 887, 888, 892, 894, 898, 902, 907, 914, 915, 918, 919, 922, 923, 925, 927, 931, 932, 937, 938, 940, 943, 944,
    945, 953, 955, 958, 959, 963, 966, 971, 974, 979, 990, 991, 998, 999, 1007, 1010, 1011, 1012, 1015, 1020, 1023,
};
const qint8 kHdr6Reuse[32][2] = {{-1,0}, {-2,0}, {-3,0}, {-4,0}, {3,-1}, {2,-1}, {1,-1}, {0,-1}, {-1,-1}, {-2,-1}, {-3,-1}, {-4,-1}, {3,-2}, {2,-2}, {1,-2}, {0,-2}, {-1,-2}, {-2,-2}, {-3,-2}, {-4,-2}, {3,-3}, {2,-3}, {1,-3}, {0,-3}, {-1,-3}, {-2,-3}, {-3,-3}, {-4,-3}, {3,-4}, {2,-4}, {1,-4}, {0,-4}};

struct Hdr6Bits {
    const uchar* p; qint64 n; qint64 at = 0; bool over = false;
    quint32 get(int k){ quint32 v = 0; for(int i = 0; i < k; ++i, ++at){ if(at >= n * 8){ over = true; continue; } v |= quint32(p[at >> 3] >> (at & 7) & 1) << i; } return v; }
    quint32 truncated(quint32 count){                                           // truncated binary
        int k = 0; while((2u << k) <= count) ++k;
        const quint32 u = (2u << k) - count; quint32 r = get(k);
        if(r >= u) r = ((r << 1) | get(1)) - u;
        return r;
    }
    quint32 vlc(int chunk){
        quint32 v = 0; int ofs = 0;
        for(;;){ const quint32 s = get(chunk + 1); v |= (s & ((1u << chunk) - 1)) << ofs; ofs += chunk; if(!(s & (1u << chunk)) || ofs >= 32) break; }
        return v;
    }
};

int hdr6Levels(int range){ return kIse[range].levels; }
int hdr6Endpoint(int ise, int range){ return unquantColour(ise, kIse[range]); }
int hdr6Weight(int ise, int range){ return unquantWeight(ise, kIse[range]); }
// The nearest coded value to v (the first on a tie); keep is a mask of top bits that mustn't change (-1 if none fits).
int hdr6NearestEndpoint(int v, int range, int keep = 0){
    int best = -1, bestErr = 1 << 30;
    for(int i = 0; i < hdr6Levels(range); ++i){ const int q = hdr6Endpoint(i, range); if((q & keep) != (v & keep)) continue; const int e = (q - v) * (q - v); if(e < bestErr){ bestErr = e; best = i; if(!e && !keep) break; } }
    return best;
}
int hdr6NearestWeight(int v, int range){
    int best = 0, bestErr = 1 << 30;
    for(int i = 0; i < hdr6Levels(range); ++i){ const int e = qAbs(v - hdr6Weight(i, range)); if(e < bestErr){ bestErr = e; best = i; if(!e) break; } }
    return best;
}
// Coded endpoint values in order of the values they stand for (deltas step through this order).
struct Hdr6Ranks { quint8 rank[21][256], ise[21][256]; };
const Hdr6Ranks& hdr6Ranks(){
    static const Hdr6Ranks t = []{
        Hdr6Ranks r{};
        for(int range = 0; range < 21; ++range) for(int i = 0; i < hdr6Levels(range); ++i){
            int k = 0; for(int j = 0; j < hdr6Levels(range); ++j) if(hdr6Endpoint(j, range) < hdr6Endpoint(i, range)) ++k;
            r.rank[range][i] = quint8(k); r.ise[range][k] = quint8(i);
        }
        return r;
    }();
    return t;
}
int hdr6Rank(int ise, int range){ return hdr6Ranks().rank[range][ise & 255]; }
int hdr6FromRank(int rank, int range){ return hdr6Ranks().ise[range][rank & 255]; }

// Values in the UASTC way of bounded integer sequences: all the trit or quint packets first, then each value's bits.
bool hdr6Values(Hdr6Bits& b, int count, int range, int* out){
    const Ise& q = kIse[range];
    const int bundle = q.trits ? 5 : q.quints ? 3 : 0, mul = q.trits ? 3 : 5, packets = bundle ? (count + bundle - 1) / bundle : 0;
    quint32 tq[32];
    if(packets > 32) return false;
    for(int i = 0; i < packets; ++i){
        int bits = q.trits ? 8 : 7;
        if(i == packets - 1){
            const int left = count - (packets - 1) * bundle;
            if(q.trits){ static const int tb[6] = {0, 2, 4, 5, 7, 8}; bits = tb[left]; } else { static const int qb[4] = {0, 3, 5, 7}; bits = qb[left]; }
        }
        tq[i] = b.get(bits);
    }
    quint32 acc = 0; int accLeft = 0, next = 0;
    for(int i = 0; i < count; ++i){
        int v = int(b.get(q.bits));
        if(packets){ if(!accLeft){ acc = tq[next++]; accLeft = bundle; } v |= int(acc % mul) << q.bits; acc /= mul; --accLeft; }
        if(v >= q.levels) return false;
        out[i] = v;
    }
    return !b.over;
}

// Endpoints from one range to another, keeping the bits that pick the HDR sub-mode.
bool hdr6Requantize(int cem, int from, const int* src, int to, int* dst){
    const int n = cem == 11 ? 6 : 4;
    if(from == to){ for(int i = 0; i < n; ++i) dst[i] = src[i]; return true; }
    int v[6]; for(int i = 0; i < n; ++i) v[i] = from == 20 ? src[i] : hdr6Endpoint(src[i], from);
    if(to == 20){ for(int i = 0; i < n; ++i) dst[i] = v[i]; return true; }
    if(to < 7) return false;
    int keep[6] = {};
    if(cem == 11){ const int major = (v[4] >> 7 & 1) | (v[5] >> 7 & 1) << 1; if(major != 3){ keep[1] = keep[2] = keep[3] = 0xC0; keep[4] = keep[5] = 0xE0; } }
    else { keep[0] = 0xC0; keep[1] = keep[2] = keep[3] = 0xE0; }
    for(int i = 0; i < n; ++i){ dst[i] = hdr6NearestEndpoint(v[i], to, keep[i]); if(dst[i] < 0) return false; }
    return true;
}

// A logical block's weights as a standard block's: requantized, and a 2x2 grid upsampled to 4x4.
void hdr6Weights(const AstcLogical& logical, AstcLogical& out, bool oldUpsample){
    const int planes = logical.dual ? 2 : 1, n = logical.W * logical.H * planes;
    int tw[72];
    for(int i = 0; i < n; ++i) tw[i] = logical.weightRange == out.weightRange ? logical.weights[i] : hdr6NearestWeight(hdr6Weight(logical.weights[i], logical.weightRange), out.weightRange);
    if(!logical.dual && logical.W == 2 && logical.H == 2){
        out.W = out.H = 4;
        const int scale = (1024 + 2) / 3;
        for(int y = 0; y < 4; ++y) for(int x = 0; x < 4; ++x){
            const int gx = (scale * x + 32) >> 6, gy = (scale * y + 32) >> 6, jx = gx >> 4, jy = gy >> 4, fx = gx & 15, fy = gy & 15;
            const int w11 = (fx * fy + 8) >> 4, wt[2][2] = { {16 - fx - fy + w11, fx - w11}, {fy - w11, w11} };
            int total = 8;
            for(int yo = 0; yo < 2; ++yo) for(int xo = 0; xo < 2; ++xo){
                if(!wt[yo][xo]) continue;
                // Files from Basis Universal 1.6 sampled the first or second weight here; later ones the right one.
                const int idx = oldUpsample ? int((x + xo) + (y + yo) * 2 < 4) : qMin(3, (jx + xo) + (jy + yo) * 2);
                total += hdr6Weight(tw[idx], out.weightRange) * wt[yo][xo];
            }
            out.weights[x + y * 4] = hdr6NearestWeight(qMin(64, total >> 4), out.weightRange);
        }
    } else { out.W = logical.W; out.H = logical.H; for(int i = 0; i < n; ++i) out.weights[i] = tw[i]; }
}

QImage decodeHdr6x6i(const uchar* data, qint64 size, int width, int height){
    Hdr6Bits b{ data, size };
    const quint32 prefix = b.get(16);
    if(prefix != 0xABCD && prefix != 0xABCE) return QImage();
    const int w = int(b.get(16)), h = int(b.get(16));
    if(w < 1 || h < 1 || (width && w != width) || (height && h != height) || qint64(w) * h > (qint64(1) << 28)) return QImage();
    const int bx = (w + 5) / 6, by = (h + 5) / 6;
    QImage img(bx * 6, by * 6, QImage::Format_ARGB32);
    if(img.isNull()) return img;
    // The last five rows of blocks, as coded and as standard blocks.
    struct Slot { AstcLogical logical, physical; bool solid = false; };
    QVector<Slot> rows(qsizetype(bx) * 5);
    auto at = [&](int x, int y) -> Slot& { return rows[qsizetype(y % 5) * bx + x]; };
    QRgb px[36];
    auto place = [&](int x, int y){
        astcEvaluate(at(x, y).physical, px, 6, 6);
        for(int t = 0; t < 6; ++t){ QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y * 6 + t)) + x * 6; for(int s = 0; s < 6; ++s) line[s] = px[t * 6 + s]; }
    };
    int cx = 0, cy = 0;
    auto advance = [&]{ place(cx, cy); if(++cx == bx){ cx = 0; ++cy; } };
    while(cy < by){
        if(b.over) return QImage();
        int kind = 3;                                                           // run 0, solid 1, reuse 2, block 3
        if(!b.get(1)){ if(b.get(1)) kind = 2; else kind = b.get(1) ? 1 : 0; }
        if(kind == 0){
            if(!cx && !cy) return QImage();
            const quint32 run = b.vlc(5) + 1;
            if(run > quint32(qint64(bx) * by - (qint64(cy) * bx + cx))) return QImage();
            const Slot prev = at(cx ? cx - 1 : bx - 1, cx ? cy : cy - 1);
            for(quint32 i = 0; i < run; ++i){ at(cx, cy) = prev; advance(); }
        } else if(kind == 1){
            Slot& s = at(cx, cy); s = Slot(); s.solid = true;
            s.physical.solid = true; for(int c = 0; c < 3; ++c) s.physical.solidHalf[c] = quint16(b.get(15)); s.physical.solidHalf[3] = 0x3C00;
            s.logical = s.physical; s.logical.cem[0] = 0;
            advance();
        } else if(kind == 2){
            if(!cx && !cy) return QImage();
            const int d = int(b.get(5)), px2 = cx + kHdr6Reuse[d][0], py2 = cy + kHdr6Reuse[d][1];
            if(px2 < 0 || px2 >= bx || py2 < 0) return QImage();
            const Slot prev = at(px2, py2);
            if(prev.solid) return QImage();
            Slot& s = at(cx, cy); s = prev;
            const int n = s.logical.W * s.logical.H * (s.logical.dual ? 2 : 1);
            if(!hdr6Values(b, n, s.logical.weightRange, s.logical.weights)) return QImage();
            hdr6Weights(s.logical, s.physical, prefix == 0xABCD);
            advance();
        } else {
            const int mi = int(b.truncated(75)), em = int(b.truncated(5));
            if(mi >= 75 || em >= 5) return QImage();
            const Hdr6Mode& m = kHdr6Modes[mi];
            const int nv = m.cem == 11 ? 6 : 4;
            Slot s;
            AstcLogical& L = s.logical; AstcLogical& P = s.physical;
            L.parts = P.parts = m.parts; for(int i = 0; i < m.parts; ++i) L.cem[i] = P.cem[i] = m.cem;
            L.weightRange = m.weightRange; L.W = m.gx; L.H = m.gy; L.dual = P.dual = m.dual; L.ccs = P.ccs = m.dual ? m.channel : -1;
            P.endRange = m.outEndRange; P.weightRange = m.outWeightRange;
            if(em == 0){
                L.endRange = m.endRange;
                if(m.parts == 2) L.seed = P.seed = kHdr6Part2[b.truncated(521)];
                else if(m.parts == 3) L.seed = P.seed = kHdr6Part3[b.truncated(333)];
                if(!hdr6Values(b, nv * m.parts, m.endRange, L.values)) return QImage();
            } else {
                if(m.parts != 1) return QImage();
                const int nx = cx - (em == 1 || em == 3 ? 1 : 0), ny = cy - (em == 2 || em == 4 ? 1 : 0);
                if(nx < 0 || ny < 0) return QImage();
                const AstcLogical& nb = at(nx, ny).logical;
                if(!nb.cem[0] || nb.cem[0] != m.cem) return QImage();
                if(em <= 2){ L.endRange = nb.endRange; for(int i = 0; i < nv; ++i) L.values[i] = nb.values[i]; }
                else {
                    L.endRange = m.endRange;
                    if(!hdr6Requantize(m.cem, nb.endRange, nb.values, m.endRange, L.values)) return QImage();
                    for(int i = 0; i < nv; ++i){
                        const int r = hdr6Rank(L.values[i], L.endRange) + int(b.get(5)) - 16;
                        if(r < 0 || r >= hdr6Levels(L.endRange)) return QImage();
                        L.values[i] = hdr6FromRank(r, L.endRange);
                    }
                }
            }
            if(!hdr6Values(b, m.gx * m.gy * (m.dual ? 2 : 1), m.weightRange, L.weights)) return QImage();
            for(int p = 0; p < m.parts; ++p) if(!hdr6Requantize(m.cem, L.endRange, L.values + nv * p, P.endRange, P.values + nv * p)) return QImage();
            hdr6Weights(L, P, prefix == 0xABCD);
            at(cx, cy) = s;
            advance();
        }
    }
    if(b.get(16) != 0xA742 || b.over) return QImage();
    return img.copy(0, 0, w, h);
}
