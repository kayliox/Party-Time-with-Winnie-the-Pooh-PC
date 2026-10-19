// Textures: every upload into the game's own textures is fingerprinted, whether it is written through the texture
// (LockRect/UnlockRect) or through its top surface (GetSurfaceLevel + surface LockRect, the D3DX file loaders' way,
// used for font sheets). A replacement pack (folder of <hash>.png, usually 4x upscaled) is substituted at SetTexture
// time; missing images can be dumped while playing so a pack grows to full coverage. The game's own textures are
// never modified. Stencil replacements (one colour + soft alpha: font sheets) drawn as alpha-tested cut-outs are
// blended instead for that draw, so their smooth HD edge is not cut back to a hard pixel staircase.
#include "compat.h"

#include <d3d8.h>
#include <wincodec.h>

#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace compat {

using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                     IDirect3DTexture8**);
using TexLockRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture8*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using TexUnlockRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture8*, UINT);
using GetSurfaceLevel_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture8*, UINT, IDirect3DSurface8**);
using SurfLockRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface8*, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfUnlockRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface8*);

static CreateTexture_t o_CreateTexture;
static TexLockRect_t o_TexLockRect;
static TexUnlockRect_t o_TexUnlockRect;
static GetSurfaceLevel_t o_GetSurfaceLevel;
static SurfLockRect_t o_SurfLockRect;
static SurfUnlockRect_t o_SurfUnlockRect;

struct TexInfo {
    IDirect3DTexture8* tex;
    IDirect3DSurface8* top;  // level 0 surface once the game asked for it (it lives as long as the texture)
    UINT w, h;
    D3DFORMAT fmt;
    BYTE* lockBits;
    INT lockPitch;
    bool locked;
};
static std::vector<TexInfo> g_texs;
static std::unordered_set<uint64_t> g_seen;
static uint64_t g_uploads, g_unique, g_dumped;
static std::string g_dumpDir;     // every new image (SPARTA_TEX_DUMP_DIR)
static std::string g_missingDir;  // only images the pack lacks ([Textures] Dump=1 -> <Pack>\dump)
static bool g_dumpMissing;
static CRITICAL_SECTION g_texLock;
static IDirect3DDevice8* g_dev;

// ---- replacement pack ----
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8*);
static SetTexture_t o_SetTexture;
static std::unordered_map<uint64_t, std::wstring> g_pack;  // hash -> file
struct Loaded {
    IDirect3DTexture8* tex;
    int refs;
    bool stencil;  // every visible pixel has one colour and the edge is soft (font glyphs tinted by the game)
};
static std::unordered_map<uint64_t, Loaded> g_loaded;              // replacements in use
static std::unordered_map<IDirect3DTexture8*, uint64_t> g_slotHash;  // game texture -> replacement shown for it
static uint64_t g_replaced, g_missing;
static IWICImagingFactory* g_wic;
static bool g_packEnabled = true;  // F1 overlay: off shows the game's own textures (instant comparison)
static std::string g_packDir;

// ---- soft edges for stencil replacements ----
using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                              D3DFORMAT, const void*, UINT);
static DrawPrimitive_t o_DrawPrimitive;
static DrawIndexedPrimitive_t o_DrawIndexedPrimitive;
static DrawPrimitiveUP_t o_DrawPrimitiveUP;
static DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP;
static bool g_softEdges;      // [Textures] SoftEdges
static bool g_stage0Stencil;  // a stencil replacement is bound on stage 0
static uint64_t g_softDraws, g_stencilBinds;

static bool ParseHash(const wchar_t* name, uint64_t& h) {
    const wchar_t* u = wcschr(name, L'_');  // optional "WxH_" prefix
    const wchar_t* s = u ? u + 1 : name;
    wchar_t* end = nullptr;
    h = _wcstoui64(s, &end, 16);
    return end && end - s == 16 && _wcsicmp(end, L".png") == 0;
}

static void IndexPack(const std::string& dir) {
    std::wstring w(dir.begin(), dir.end());
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((w + L"\\*.png").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        uint64_t h;
        if (ParseHash(fd.cFileName, h)) g_pack[h] = w + L"\\" + fd.cFileName;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// Loads a PNG into a managed A8R8G8B8 texture with a full mip chain (alpha-weighted box filter: no dark fringes).
static IDirect3DTexture8* LoadReplacement(const std::wstring& file, bool* stencil) {
    if (!g_wic) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // the game's own CoInitialize later returns S_FALSE
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                    reinterpret_cast<void**>(&g_wic))))
            return nullptr;
    }
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    std::vector<uint32_t> px;
    UINT w = 0, h = 0;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h && w <= 4096 && h <= 4096) {
        px.resize(size_t(w) * h);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data())))) px.clear();
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (px.empty()) return nullptr;
    uint32_t colour = 0;
    bool any = false, soft = false, single = true;
    for (uint32_t v : px) {
        uint32_t a = v >> 24;
        if (!a) continue;
        soft = soft || a < 255;
        if (!any) {
            colour = v & 0xFFFFFF;
            any = true;
        } else if ((v & 0xFFFFFF) != colour) {
            single = false;
            break;
        }
    }
    *stencil = any && soft && single;
    IDirect3DTexture8* tex = nullptr;
    if (FAILED(o_CreateTexture(g_dev, w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) || !tex) return nullptr;
    DWORD levels = tex->GetLevelCount();
    std::vector<uint32_t> cur = px, next;
    UINT lw = w, lh = h;
    for (DWORD l = 0; l < levels; l++) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tex->LockRect(l, &lr, nullptr, 0))) {
            for (UINT y = 0; y < lh; y++) memcpy(static_cast<BYTE*>(lr.pBits) + size_t(y) * lr.Pitch, &cur[size_t(y) * lw], lw * 4);
            tex->UnlockRect(l);
        }
        if (l + 1 == levels) break;
        UINT nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
        next.assign(size_t(nw) * nh, 0);
        for (UINT y = 0; y < nh; y++)
            for (UINT x = 0; x < nw; x++) {
                uint32_t a = 0, r = 0, g = 0, b = 0;
                for (UINT dy = 0; dy < 2; dy++)
                    for (UINT dx = 0; dx < 2; dx++) {
                        uint32_t s = cur[size_t(std::min(y * 2 + dy, lh - 1)) * lw + std::min(x * 2 + dx, lw - 1)];
                        uint32_t sa = s >> 24;
                        a += sa; r += ((s >> 16) & 255) * sa; g += ((s >> 8) & 255) * sa; b += (s & 255) * sa;
                    }
                next[size_t(y) * nw + x] = a ? ((a / 4) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a)) : 0;
            }
        cur.swap(next);
        lw = nw;
        lh = nh;
    }
    return tex;
}

static void Unmap(IDirect3DTexture8* slot) {
    auto it = g_slotHash.find(slot);
    if (it == g_slotHash.end()) return;
    auto l = g_loaded.find(it->second);
    if (l != g_loaded.end() && --l->second.refs <= 0) {
        l->second.tex->Release();
        g_loaded.erase(l);
    }
    g_slotHash.erase(it);
}

static void MapReplacement(IDirect3DTexture8* slot, uint64_t h) {
    Unmap(slot);
    auto p = g_pack.find(h);
    if (p == g_pack.end()) return;
    auto l = g_loaded.find(h);
    if (l == g_loaded.end()) {
        bool stencil = false;
        IDirect3DTexture8* tex = LoadReplacement(p->second, &stencil);
        if (!tex) {
            Log("textures: cannot load %ls; using the original", p->second.c_str());
            g_pack.erase(p);
            return;
        }
        l = g_loaded.emplace(h, Loaded{tex, 0, stencil}).first;
    }
    l->second.refs++;
    g_slotHash[slot] = h;
    g_replaced++;
}

static HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice8* dev, DWORD stage, IDirect3DBaseTexture8* tex) {
    if (g_layerCalls) return o_SetTexture(dev, stage, tex);
    bool stencil = false;
    if (tex && g_packEnabled && !g_slotHash.empty()) {
        auto it = g_slotHash.find(static_cast<IDirect3DTexture8*>(tex));
        if (it != g_slotHash.end()) {
            auto l = g_loaded.find(it->second);
            if (l != g_loaded.end()) {
                tex = l->second.tex;
                stencil = l->second.stencil;
            }
        }
    }
    if (stage == 0) {
        g_stage0Stencil = stencil;
        if (stencil) g_stencilBinds++;
    }
    return o_SetTexture(dev, stage, tex);
}

// For one draw: a stencil replacement cut out by the alpha test (blending off) is blended with its own soft alpha
// instead. The states are read from the device and put back afterwards, so the game never sees a difference.
struct SoftEdgeDraw {
    IDirect3DDevice8* dev;
    bool on = false;
    DWORD src = 0, dst = 0, ref = 0, func = 0;
    explicit SoftEdgeDraw(IDirect3DDevice8* d) : dev(d) {
        if (!g_softEdges || !g_stage0Stencil || g_layerCalls) return;
        DWORD test = 0, blend = 0;
        if (FAILED(dev->GetRenderState(D3DRS_ALPHATESTENABLE, &test)) || FAILED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) ||
            !test || blend)
            return;
        dev->GetRenderState(D3DRS_SRCBLEND, &src);
        dev->GetRenderState(D3DRS_DESTBLEND, &dst);
        dev->GetRenderState(D3DRS_ALPHAREF, &ref);
        dev->GetRenderState(D3DRS_ALPHAFUNC, &func);
        if (!g_softDraws)
            Log("textures: soft edges for stencil replacements (the game cuts them out: alpha test ref %lu func %lu, no blending)",
                ref, func);
        g_softDraws++;
        on = true;
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_ALPHAREF, 1);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    }
    ~SoftEdgeDraw() {
        if (!on) return;
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_SRCBLEND, src);
        dev->SetRenderState(D3DRS_DESTBLEND, dst);
        dev->SetRenderState(D3DRS_ALPHAREF, ref);
        dev->SetRenderState(D3DRS_ALPHAFUNC, func);
    }
};

static HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice8* dev, D3DPRIMITIVETYPE t, UINT start, UINT count) {
    SoftEdgeDraw soft(dev);
    return o_DrawPrimitive(dev, t, start, count);
}

static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitive(IDirect3DDevice8* dev, D3DPRIMITIVETYPE t, UINT minIndex, UINT num,
                                                        UINT startIndex, UINT count) {
    SoftEdgeDraw soft(dev);
    return o_DrawIndexedPrimitive(dev, t, minIndex, num, startIndex, count);
}

static HRESULT STDMETHODCALLTYPE H_DrawPrimitiveUP(IDirect3DDevice8* dev, D3DPRIMITIVETYPE t, UINT count, const void* data,
                                                   UINT stride) {
    SoftEdgeDraw soft(dev);
    return o_DrawPrimitiveUP(dev, t, count, data, stride);
}

static HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitiveUP(IDirect3DDevice8* dev, D3DPRIMITIVETYPE t, UINT minIndex, UINT num,
                                                          UINT count, const void* idx, D3DFORMAT fmt, const void* data,
                                                          UINT stride) {
    SoftEdgeDraw soft(dev);
    return o_DrawIndexedPrimitiveUP(dev, t, minIndex, num, count, idx, fmt, data, stride);
}

static TexInfo* FindTex(IDirect3DTexture8* t) {
    for (auto& i : g_texs)
        if (i.tex == t) return &i;
    return nullptr;
}

static TexInfo* FindTop(IDirect3DSurface8* s) {
    for (auto& i : g_texs)
        if (i.top == s) return &i;
    return nullptr;
}

static uint64_t Fnv1a(const BYTE* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// 32-bit TGA (BGRA, bottom-up), alpha from the A1 bit: 0 = transparent (the PS1 colour key)
static void WriteTga(const char* path, const TexInfo& t) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint8_t hdr[18] = {0, 0, 2};
    hdr[12] = uint8_t(t.w); hdr[13] = uint8_t(t.w >> 8);
    hdr[14] = uint8_t(t.h); hdr[15] = uint8_t(t.h >> 8);
    hdr[16] = 32; hdr[17] = 8;
    fwrite(hdr, 1, 18, f);
    std::vector<uint8_t> row(size_t(t.w) * 4);
    for (int y = int(t.h) - 1; y >= 0; y--) {
        const BYTE* src = t.lockBits + size_t(y) * t.lockPitch;
        for (UINT x = 0; x < t.w; x++) {
            uint8_t r, g, b, a;
            if (t.fmt == D3DFMT_A1R5G5B5 || t.fmt == D3DFMT_X1R5G5B5) {
                uint16_t v = reinterpret_cast<const uint16_t*>(src)[x];
                r = uint8_t(((v >> 10) & 31) * 255 / 31); g = uint8_t(((v >> 5) & 31) * 255 / 31); b = uint8_t((v & 31) * 255 / 31);
                a = (t.fmt == D3DFMT_X1R5G5B5 || (v & 0x8000)) ? 255 : 0;
            } else if (t.fmt == D3DFMT_R5G6B5) {  // D3DX loads JPEGs this way (help screens)
                uint16_t v = reinterpret_cast<const uint16_t*>(src)[x];
                r = uint8_t(((v >> 11) & 31) * 255 / 31); g = uint8_t(((v >> 5) & 63) * 255 / 63); b = uint8_t((v & 31) * 255 / 31);
                a = 255;
            } else if (t.fmt == D3DFMT_A4R4G4B4) {
                uint16_t v = reinterpret_cast<const uint16_t*>(src)[x];
                a = uint8_t(((v >> 12) & 15) * 17); r = uint8_t(((v >> 8) & 15) * 17); g = uint8_t(((v >> 4) & 15) * 17); b = uint8_t((v & 15) * 17);
            } else {  // 32-bit ARGB/XRGB
                const BYTE* p = src + x * 4;
                b = p[0]; g = p[1]; r = p[2]; a = t.fmt == D3DFMT_X8R8G8B8 ? 255 : p[3];
            }
            row[x * 4] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = a;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

static UINT BytesPerPixel(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: case D3DFMT_R5G6B5: case D3DFMT_A4R4G4B4: return 2;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
        default: return 0;
    }
}

// Fingerprint of the visible image: the colour of fully transparent pixels is ignored, because the game leaves stale
// data there (Pooh: a palette row from its emulated VRAM) that differs from run to run.
static uint64_t Fingerprint(const TexInfo& t, UINT bpp) {
    uint64_t h = 1469598103934665603ull;
    std::vector<BYTE> row(size_t(t.w) * bpp);
    for (UINT y = 0; y < t.h; y++) {
        memcpy(row.data(), t.lockBits + size_t(y) * t.lockPitch, row.size());
        if (t.fmt == D3DFMT_A1R5G5B5 || t.fmt == D3DFMT_A4R4G4B4) {
            uint16_t* p = reinterpret_cast<uint16_t*>(row.data());
            uint16_t amask = t.fmt == D3DFMT_A1R5G5B5 ? 0x8000 : 0xF000;
            for (UINT x = 0; x < t.w; x++)
                if (!(p[x] & amask)) p[x] = 0;
        } else if (t.fmt == D3DFMT_A8R8G8B8) {
            uint32_t* p = reinterpret_cast<uint32_t*>(row.data());
            for (UINT x = 0; x < t.w; x++)
                if (!(p[x] >> 24)) p[x] = 0;
        }
        h = Fnv1a(row.data(), row.size(), h);
    }
    return h ^ uint64_t(t.w) << 48 ^ uint64_t(t.h) << 32 ^ uint64_t(t.fmt);
}

// A whole-image write into level 0 just finished (caller holds g_texLock): fingerprint, map, dump.
static void OnUpload(TexInfo* i) {
    i->locked = false;
    UINT bpp = BytesPerPixel(i->fmt);
    if (!bpp) return;
    uint64_t h = Fingerprint(*i, bpp);
    g_uploads++;
    bool inPack = g_pack.count(h) != 0;
    if (!g_pack.empty()) MapReplacement(i->tex, h);
    if (!g_seen.insert(h).second) return;
    g_unique++;
    if (!inPack) g_missing++;
    const std::string& dir = !g_dumpDir.empty() ? g_dumpDir : (!inPack && g_dumpMissing ? g_missingDir : std::string());
    if (dir.empty()) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%ux%u_%016llx.tga", dir.c_str(), i->w, i->h, (unsigned long long)h);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        WriteTga(path, *i);
        g_dumped++;
    }
}

static bool WholeImage(const TexInfo& i, const RECT* rc) {
    return !rc || (rc->left == 0 && rc->top == 0 && UINT(rc->right) == i.w && UINT(rc->bottom) == i.h);
}

// The entry must describe the memory just locked: an address can come back for another object (a released texture's
// surface reused by an image surface), and copying with a stale entry's size reads past the lock (O-121).
static bool Matches(const TexInfo& i, const D3DSURFACE_DESC& d) { return d.Width == i.w && d.Height == i.h && d.Format == i.fmt; }

static HRESULT STDMETHODCALLTYPE H_TexLockRect(IDirect3DTexture8* t, UINT level, D3DLOCKED_RECT* lr, const RECT* rc, DWORD flags) {
    HRESULT hr = o_TexLockRect(t, level, lr, rc, flags);
    if (SUCCEEDED(hr) && level == 0 && !(flags & D3DLOCK_READONLY) && !g_layerCalls) {
        EnterCriticalSection(&g_texLock);
        TexInfo* i = FindTex(t);
        D3DSURFACE_DESC d;
        if (i && WholeImage(*i, rc) && SUCCEEDED(t->GetLevelDesc(0, &d)) && Matches(*i, d)) {
            i->lockBits = static_cast<BYTE*>(lr->pBits);
            i->lockPitch = lr->Pitch;
            i->locked = true;
        }
        LeaveCriticalSection(&g_texLock);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_TexUnlockRect(IDirect3DTexture8* t, UINT level) {
    if (level == 0 && !g_layerCalls) {
        EnterCriticalSection(&g_texLock);
        TexInfo* i = FindTex(t);
        if (i && i->locked) OnUpload(i);
        LeaveCriticalSection(&g_texLock);
    }
    return o_TexUnlockRect(t, level);
}

// Hooked on the level-0 surfaces of tracked textures; anything else (render targets) is passed straight through.
static HRESULT STDMETHODCALLTYPE H_SurfLockRect(IDirect3DSurface8* s, D3DLOCKED_RECT* lr, const RECT* rc, DWORD flags) {
    HRESULT hr = o_SurfLockRect(s, lr, rc, flags);
    if (SUCCEEDED(hr) && !(flags & D3DLOCK_READONLY) && !g_layerCalls) {
        EnterCriticalSection(&g_texLock);
        TexInfo* i = FindTop(s);
        D3DSURFACE_DESC d;
        if (i && WholeImage(*i, rc) && SUCCEEDED(s->GetDesc(&d)) && Matches(*i, d)) {
            i->lockBits = static_cast<BYTE*>(lr->pBits);
            i->lockPitch = lr->Pitch;
            i->locked = true;
        }
        LeaveCriticalSection(&g_texLock);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SurfUnlockRect(IDirect3DSurface8* s) {
    if (g_layerCalls) return o_SurfUnlockRect(s);
    EnterCriticalSection(&g_texLock);
    TexInfo* i = FindTop(s);
    if (i && i->locked) OnUpload(i);
    LeaveCriticalSection(&g_texLock);
    return o_SurfUnlockRect(s);
}

static HRESULT STDMETHODCALLTYPE H_GetSurfaceLevel(IDirect3DTexture8* t, UINT level, IDirect3DSurface8** out) {
    HRESULT hr = o_GetSurfaceLevel(t, level, out);
    if (SUCCEEDED(hr) && level == 0 && out && *out) {
        EnterCriticalSection(&g_texLock);
        for (auto& o : g_texs)  // a released texture's surface address comes back for new textures (fonts reload)
            if (o.top == *out) o.top = nullptr;
        if (TexInfo* i = FindTex(t)) i->top = *out;
        LeaveCriticalSection(&g_texLock);
        void* p;
        if ((p = HookVtable(*out, 9, reinterpret_cast<void*>(H_SurfLockRect)))) o_SurfLockRect = reinterpret_cast<SurfLockRect_t>(p);
        if ((p = HookVtable(*out, 10, reinterpret_cast<void*>(H_SurfUnlockRect)))) o_SurfUnlockRect = reinterpret_cast<SurfUnlockRect_t>(p);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice8* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool, IDirect3DTexture8** out) {
    HRESULT hr = o_CreateTexture(dev, w, h, levels, usage, fmt, pool, out);
    if (g_layerCalls) return hr;  // the scene's targets and the overlay's font atlas are not game textures
    if (SUCCEEDED(hr) && out && *out) {
        EnterCriticalSection(&g_texLock);
        // a released texture's address can come back for a new one: forget what was known about the old one
        TexInfo info{*out, nullptr, w, h, fmt, nullptr, 0, false};
        if (TexInfo* old = FindTex(*out)) *old = info;
        else g_texs.push_back(info);
        Unmap(*out);
        LeaveCriticalSection(&g_texLock);
        void* p;
        if ((p = HookVtable(*out, 15, reinterpret_cast<void*>(H_GetSurfaceLevel)))) o_GetSurfaceLevel = reinterpret_cast<GetSurfaceLevel_t>(p);
        if ((p = HookVtable(*out, 16, reinterpret_cast<void*>(H_TexLockRect)))) o_TexLockRect = reinterpret_cast<TexLockRect_t>(p);
        if ((p = HookVtable(*out, 17, reinterpret_cast<void*>(H_TexUnlockRect)))) o_TexUnlockRect = reinterpret_cast<TexUnlockRect_t>(p);
    }
    return hr;
}

bool TexturePackActive() { return o_SetTexture != nullptr; }

void SetTexturePackEnabled(bool on) { g_packEnabled = on; }  // takes effect at the game's next SetTexture
bool TexturePackEnabled() { return g_packEnabled && !g_pack.empty(); }
void SetSoftEdges(bool on) { g_softEdges = on && o_DrawPrimitive != nullptr; }
bool SoftEdgesEnabled() { return g_softEdges; }
void SetTextureDump(bool on) {
    if (on && g_missingDir.empty() && !g_packDir.empty()) {
        g_missingDir = g_packDir + "\\dump";
        CreateDirectoryA(g_packDir.c_str(), nullptr);
        CreateDirectoryA(g_missingDir.c_str(), nullptr);
        Log("textures: images without a replacement are saved to %s", g_missingDir.c_str());
    }
    g_dumpMissing = on && !g_missingDir.empty();
}
bool TextureDumpEnabled() { return g_dumpMissing; }
TextureInfo GetTextureInfo() {
    return {unsigned(g_texs.size()), unsigned(g_loaded.size()), unsigned(g_pack.size()), g_unique, g_missing, g_dumped};
}

void LogTextureStats() {
    if (!o_CreateTexture) return;
    Log("[textures] %u created, %llu uploads, %llu distinct images (%llu without a replacement), %llu replacements mapped, "
        "%u loaded, %llu dumped; %llu stencil binds, %llu soft-edge draws",
        unsigned(g_texs.size()), (unsigned long long)g_uploads, (unsigned long long)g_unique, (unsigned long long)g_missing,
        (unsigned long long)g_replaced, unsigned(g_loaded.size()), (unsigned long long)g_dumped,
        (unsigned long long)g_stencilBinds, (unsigned long long)g_softDraws);
}

void InstallTextureHooks(void* device) {
    auto* dev = static_cast<IDirect3DDevice8*>(device);
    char buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("SPARTA_TEX_DUMP_DIR", buf, sizeof buf);
    if (n && n < sizeof buf) g_dumpDir = buf;
    std::string pack = IniString("Textures", "Pack", "");
    if (!pack.empty()) {
        if (!(pack.size() > 1 && (pack[1] == ':' || pack[0] == '\\'))) pack = g_cfg.exeDir + pack;
        IndexPack(pack);
        g_packDir = pack;
        g_packEnabled = IniInt("Textures", "Enable", 1) != 0;
        Log("textures: pack %s: %u replacement(s)%s", pack.c_str(), unsigned(g_pack.size()), g_packEnabled ? "" : " (switched off)");
        if (IniInt("Textures", "Dump", 0)) SetTextureDump(true);
    }
    bool census = GetEnvironmentVariableA("SPARTA_TEX_CENSUS", buf, sizeof buf) != 0;
    if (g_dumpDir.empty() && g_pack.empty() && g_missingDir.empty() && !census) return;  // nothing to do
    g_dev = dev;
    InitializeCriticalSection(&g_texLock);
    if (!g_dumpDir.empty()) Log("textures: dumping every new image to %s", g_dumpDir.c_str());
    HookDevice(dev, 20, reinterpret_cast<void*>(H_CreateTexture), reinterpret_cast<void**>(&o_CreateTexture));
    if (g_pack.empty()) return;
    HookDevice(dev, 61, reinterpret_cast<void*>(H_SetTexture), reinterpret_cast<void**>(&o_SetTexture));
    // the draw hooks stay in place either way, so the overlay can switch soft edges on and off while playing
    HookDevice(dev, 70, reinterpret_cast<void*>(H_DrawPrimitive), reinterpret_cast<void**>(&o_DrawPrimitive));
    HookDevice(dev, 71, reinterpret_cast<void*>(H_DrawIndexedPrimitive), reinterpret_cast<void**>(&o_DrawIndexedPrimitive));
    HookDevice(dev, 72, reinterpret_cast<void*>(H_DrawPrimitiveUP), reinterpret_cast<void**>(&o_DrawPrimitiveUP));
    HookDevice(dev, 73, reinterpret_cast<void*>(H_DrawIndexedPrimitiveUP), reinterpret_cast<void**>(&o_DrawIndexedPrimitiveUP));
    g_softEdges = IniInt("Textures", "SoftEdges", 1) != 0;
}

}  // namespace compat
