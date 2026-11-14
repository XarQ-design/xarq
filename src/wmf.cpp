// wmf.cpp — WMF → SVG via libwmf's SVG device layer. See wmf.h. libwmf headers/symbols confined here.
#include "wmf.h"

#ifdef HAVE_WMF
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifdef _WIN32
#include <QTemporaryFile>
#endif
extern "C" {
#include <libwmf/api.h>
#include <libwmf/svg.h>
}

namespace xara {
bool wmfAvailable(){ return true; }

// Where libwmf's SVG device writes. open_memstream keeps it in memory; Windows has no open_memstream, so
// there it is a temporary file, read back and removed.
#ifdef _WIN32
struct SvgSink {
    QTemporaryFile tmp; FILE* f=nullptr;
    FILE* open(){
        if(!tmp.open()) return nullptr;
        const QString path=tmp.fileName(); tmp.close();      // the file stays until tmp goes away
        f=_wfopen(reinterpret_cast<const wchar_t*>(path.utf16()), L"w+b"); return f;
    }
    QByteArray finish(){
        if(!f) return {};
        QByteArray out; char chunk[65536]; size_t n;
        fflush(f); rewind(f);
        while((n=fread(chunk,1,sizeof chunk,f))>0) out.append(chunk,(qsizetype)n);
        fclose(f); f=nullptr; return out;
    }
    void discard(){ if(f){ fclose(f); f=nullptr; } }
};
#else
struct SvgSink {
    char* buf=nullptr; size_t size=0; FILE* f=nullptr;
    FILE* open(){ f=open_memstream(&buf,&size); return f; }
    QByteArray finish(){                                     // fclose flushes the stream into buf/size
        if(!f) return {};
        fclose(f); f=nullptr;
        QByteArray out = (buf && size>0) ? QByteArray(buf,(qsizetype)size) : QByteArray();
        std::free(buf); buf=nullptr; return out;
    }
    void discard(){ if(f){ fclose(f); f=nullptr; } std::free(buf); buf=nullptr; }
};
#endif

QByteArray wmfToSvg(const QByteArray& data){
    if(data.isEmpty()) return {};

    SvgSink sink;
    FILE* memf = sink.open();
    if(!memf){ sink.discard(); return {}; }

    wmfAPI* API = nullptr;
    wmfAPI_Options opts; opts.function = wmf_svg_function;                 // route playback through the SVG device
    const unsigned long flags = WMF_OPT_FUNCTION | WMF_OPT_IGNORE_NONFATAL;

    auto bail = [&](wmfAPI* a, bool memOpen)->QByteArray{
        if(a){ if(memOpen) wmf_mem_close(a); wmf_api_destroy(a); }
        sink.discard(); return {};
    };

    if(wmf_api_create(&API, flags, &opts) != wmf_E_None) return bail(API, false);
    if(wmf_mem_open(API, reinterpret_cast<unsigned char*>(const_cast<char*>(data.constData())),
                    (long)data.size()) != wmf_E_None) return bail(API, false);
    wmfD_Rect bbox;
    if(wmf_scan(API, 0, &bbox) != wmf_E_None) return bail(API, true);

    unsigned int dw = 0, dh = 0;
    if(wmf_display_size(API, &dw, &dh, 96.0, 96.0) != wmf_E_None || dw==0 || dh==0){
        dw = (unsigned int)std::ceil((double)(bbox.BR.x - bbox.TL.x));     // fall back to the bounding box
        dh = (unsigned int)std::ceil((double)(bbox.BR.y - bbox.TL.y));
    }
    if(dw==0 || dh==0 || (long long)dw*dh > 400000000LL) return bail(API, true);

    wmf_svg_t* dd = WMF_SVG_GetData(API);
    dd->out    = wmf_stream_create(API, memf);
    dd->bbox   = bbox;
    dd->width  = dw;
    dd->height = dh;

    const wmf_error_t perr = wmf_play(API, 0, &bbox);
    wmf_mem_close(API);
    wmf_api_destroy(API);
    QByteArray out = sink.finish();
    if(perr != wmf_E_None) out.clear();
    return out;
}
}   // namespace xara

#else   // no libwmf at build time

namespace xara {
bool       wmfAvailable(){ return false; }
QByteArray wmfToSvg(const QByteArray&){ return {}; }
}

#endif
