// Display side of the layer: the Direct3DCreate8 export, the game window (borderless/windowed at any size), the
// device parameters (resolution, MSAA, depth), the offscreen scene (supersampling, hor+ side fill), frame pacing that
// replaces the game's own cap, and frame dumps.
#include "compat.h"

#include <d3d8.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace compat {

// ---------------------------------------------------------------- window

using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
using SetForegroundWindow_t = BOOL(WINAPI*)(HWND);

static CreateWindowExA_t o_CreateWindowExA;
static SetWindowPos_t o_SetWindowPos;
static ShowWindow_t o_ShowWindow;
static SetForegroundWindow_t o_SetForegroundWindow;

static HWND g_gameWnd;
static int g_reqW, g_reqH;        // size the game asked for (its own render size)
static int g_outW, g_outH;        // output resolution (the picture's canvas)
static int g_winW, g_winH;        // window client size (borderless: the monitor; the canvas is scaled into it)
static int g_renderW, g_renderH;  // the game's internal render size (its 512x240 space maps onto it)
static int g_bbW, g_bbH;          // back buffer size (= render size, except hor+: full window width)
static float g_xOffset;           // hor+: pre-transformed vertices move right by this much (centred 4:3 space)
static RECT g_dest;               // where the back buffer lands in the client area (pillarbox)
static bool g_pillarbox;
static bool g_horPlus;
static uint64_t g_fadeTris, g_shiftedTris, g_hudGroups;  // hor+ remap statistics
// Offscreen scene (Supersample=2 or SideFill=ambient): the game draws into a render-target texture of g_bbW x g_bbH,
// which is the real back buffer (g_presW x g_presH) times g_ss; every size above is in that scene space. Present
// averages the scene down into the back buffer. Without it the game draws into the back buffer (g_ss = 1).
static int g_ss = 1, g_ssWanted = 1;
static int g_presW, g_presH;  // the real back buffer
static float g_presXOffset;   // hor+ offset in window pixels (child windows such as movies)
static bool g_scene;
static bool g_renderVarsOk;          // the game's render size variables were verified at device creation
static unsigned g_refreshHz = 60;    // desktop refresh rate (FpsLimit=-1)
static IDirect3DSurface8* g_autoDepth;  // the device's own depth surface (direct drawing after a live switch)

static void SceneRelease(IDirect3DDevice8* dev);
static bool SceneCreate(IDirect3DDevice8* dev);
static IDirect3DTexture8* g_sceneTex;

static bool Managed() { return g_cfg.mode == "borderless" || g_cfg.mode == "windowed" || g_cfg.mode == "fullscreen"; }
static bool Fullscreen() { return g_cfg.mode == "fullscreen"; }

static bool IsGameWindowClass(LPCSTR cls) {
    std::string want = IniString("Display", "WindowClass", "");
    if (want.empty()) return true;  // first top-level window
    if (!cls || IS_INTRESOURCE(cls)) return false;
    return _stricmp(cls, want.c_str()) == 0;
}

static void ComputeSizes() {
    g_ssWanted = g_cfg.supersample;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int monW = mi.rcMonitor.right - mi.rcMonitor.left, monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    g_outW = g_cfg.width > 0 ? g_cfg.width : monW;
    g_outH = g_cfg.height > 0 ? g_cfg.height : monH;
    // borderless covers the monitor and scales the chosen resolution into it; fullscreen switches the display to it
    g_winW = g_cfg.mode == "borderless" ? monW : g_outW;
    g_winH = g_cfg.mode == "borderless" ? monH : g_outH;
    if (Fullscreen() && g_cfg.aspect == "4:3") {
        g_cfg.aspect = "hor+";  // a fullscreen back buffer must cover the display mode
        Log("display: exclusive fullscreen uses the whole screen: aspect hor+");
    }
    if (g_cfg.aspect == "stretch") {
        g_renderW = g_outW;
        g_renderH = g_outH;
    } else {  // keep the game's 4:3 picture, as large as fits
        g_renderH = g_outH;
        g_renderW = (g_outH * 4 + 1) / 3;
        if (g_renderW > g_outW) {
            g_renderW = g_outW;
            g_renderH = g_outW * 3 / 4;
        }
    }
    g_bbW = g_renderW;
    g_bbH = g_renderH;
    // hor+: the back buffer fills the window; the game's 4:3 space sits in the middle and the game's own 3D (with
    // its culling widened by the game profile) fills the sides. 2D stays centred and undistorted.
    g_horPlus = g_cfg.aspect == "hor+" && g_renderW < g_outW;
    if (g_horPlus) {
        g_bbW = g_outW;
        g_bbH = g_renderH;
        g_xOffset = float(g_outW - g_renderW) / 2.0f;
        int units = IniInt("Game", "ScreenUnitsX", 0);  // the game's 2D width in its own units (PS1: 512)
        if (units > 0) SetWideExtension(int(std::ceil(g_xOffset * units / g_renderW)) + 8);
    }
    // the back buffer sits centred on the output canvas, which is scaled (aspect kept) into the window
    double k = std::min(double(g_winW) / g_outW, double(g_winH) / g_outH);
    double cx = (g_winW - g_outW * k) / 2.0, cy = (g_winH - g_outH * k) / 2.0;
    g_dest.left = LONG(cx + (g_outW - g_bbW) / 2.0 * k + 0.5);
    g_dest.top = LONG(cy + (g_outH - g_bbH) / 2.0 * k + 0.5);
    g_dest.right = g_dest.left + LONG(g_bbW * k + 0.5);
    g_dest.bottom = g_dest.top + LONG(g_bbH * k + 0.5);
    g_pillarbox = !Fullscreen() && (g_dest.left != 0 || g_dest.top != 0 || g_dest.right != g_winW || g_dest.bottom != g_winH ||
                                    g_bbW != g_winW || g_bbH != g_winH);
    Log("display: monitor %dx%d, window %dx%d (%s), output %dx%d, back buffer %dx%d, game render %dx%d (%s, x offset %.1f)",
        monW, monH, g_winW, g_winH, g_cfg.mode.c_str(), g_outW, g_outH, g_bbW, g_bbH, g_renderW, g_renderH,
        g_cfg.aspect.c_str(), g_xOffset);
    g_presW = g_bbW;
    g_presH = g_bbH;
    g_presXOffset = g_xOffset;
    g_ss = g_ssWanted;
    g_scene = g_ss > 1 || (g_horPlus && g_cfg.sideFill == "ambient");
    if (g_ss > 1) {
        g_renderW *= g_ss;
        g_renderH *= g_ss;
        g_bbW *= g_ss;
        g_bbH *= g_ss;
        g_xOffset *= float(g_ss);
    }
    if (g_scene)
        Log("display: offscreen scene %dx%d (supersampling %dx%d, side fill %s)", g_bbW, g_bbH, g_ss, g_ss,
            g_horPlus ? g_cfg.sideFill.c_str() : "none");
}

// The game window's procedure, wrapped: the overlay takes mouse and keyboard messages while it is open, and old
// games pause on WM_ACTIVATE/WM_ACTIVATEAPP "inactive" (this one blocks in WaitMessage), so hidden test runs or
// RunInBackground=1 keep it running by always reporting "active".
static WNDPROC g_gameProc;

static LRESULT CALLBACK GameWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT r = 0;
    if (OverlayWndProc(w, msg, wp, lp, &r)) return r;
    if (msg == WM_CLOSE || msg == WM_QUERYENDSESSION) AutosaveFlush("window closed");  // Alt+F4 keeps the progress
    if (msg == WM_DESTROY) UnmuteTestAudio();  // a test run the game ended itself must not stay muted (O-122)
    if (g_cfg.windowHidden || g_cfg.runInBackground) {
        if (msg == WM_ACTIVATEAPP) wp = TRUE;
        else if (msg == WM_ACTIVATE) wp = MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
        else if (msg == WM_KILLFOCUS) return 0;
    }
    return CallWindowProcA(g_gameProc, w, msg, wp, lp);
}

static void WrapGameWindow(HWND hwnd) {
    if (!hwnd || g_gameProc) return;
    g_gameProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameWndProc)));
}

static HWND WINAPI H_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    bool game = !g_gameWnd && !parent && !(style & WS_CHILD) && IsGameWindowClass(cls);
    if (game && Managed()) {
        g_reqW = w;
        g_reqH = h;
        ComputeSizes();
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        if (g_cfg.mode == "borderless" || Fullscreen()) {
            ex = WS_EX_APPWINDOW;
            style = WS_POPUP | WS_VISIBLE;
            x = mi.rcMonitor.left;
            y = mi.rcMonitor.top;
            w = g_winW;
            h = g_winH;
        } else {
            ex = WS_EX_APPWINDOW;
            style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
            RECT r{0, 0, g_outW, g_outH};
            AdjustWindowRectEx(&r, style, FALSE, ex);
            w = r.right - r.left;
            h = r.bottom - r.top;
            x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
            y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
        }
        if (g_cfg.windowHidden) style &= ~WS_VISIBLE;
    }
    HWND hwnd = o_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (game) {
        g_gameWnd = hwnd;
        Log("window: game window %p class=%s requested %dx%d", hwnd,
            (cls && !IS_INTRESOURCE(cls)) ? cls : "#atom", g_reqW, g_reqH);
        WrapGameWindow(hwnd);
        if (g_cfg.windowHidden || g_cfg.runInBackground) Log("window: keeps running while inactive");
        if (Managed()) {
            SetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
            if (g_cfg.hideCursor && g_cfg.mode != "windowed") ShowCursor(FALSE);
        }
    }
    return hwnd;
}

static BOOL WINAPI H_SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags) {
    if (hwnd == g_gameWnd && Managed()) return TRUE;  // the layer owns the window geometry
    return o_SetWindowPos(hwnd, after, x, y, w, h, flags);
}

static BOOL WINAPI H_ShowWindow(HWND hwnd, int cmd) {
    if (hwnd == g_gameWnd && g_cfg.windowHidden) cmd = SW_HIDE;
    return o_ShowWindow(hwnd, cmd);
}

static BOOL WINAPI H_SetForegroundWindow(HWND hwnd) {
    if (hwnd == g_gameWnd && g_cfg.windowHidden) return TRUE;  // unattended runs never steal focus
    return o_SetForegroundWindow(hwnd);
}

static void PaintBars() {
    if (!g_pillarbox || !g_gameWnd || g_cfg.windowHidden) return;
    HDC dc = GetDC(g_gameWnd);
    if (!dc) return;
    auto black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT r;
    if (g_dest.left > 0) { r = {0, 0, g_dest.left, g_winH}; FillRect(dc, &r, black); }
    if (g_dest.right < g_winW) { r = {g_dest.right, 0, g_winW, g_winH}; FillRect(dc, &r, black); }
    if (g_dest.top > 0) { r = {0, 0, g_winW, g_dest.top}; FillRect(dc, &r, black); }
    if (g_dest.bottom < g_winH) { r = {0, g_dest.bottom, g_winW, g_winH}; FillRect(dc, &r, black); }
    ReleaseDC(g_gameWnd, dc);
}

// Unattended runs must never leave a box on the user's screen (O-81): the game's own message boxes and its exit dialog
// are logged and answered (OK / "keep playing"), and Windows' crash and file-error boxes are switched off.
static int WINAPI H_MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type) {
    Log("messagebox (answered automatically): [%s] %s", caption ? caption : "", text ? text : "");
    return IDOK;
}

static INT_PTR WINAPI H_DialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
    Log("dialog %p (answered automatically with IDNO/7)", tmpl);
    return 7;
}

void InstallWindowHooks(HMODULE exe) {
    if (g_cfg.windowHidden) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        HookImport(exe, "user32.dll", "MessageBoxA", reinterpret_cast<void*>(H_MessageBoxA));
        HookImport(exe, "user32.dll", "DialogBoxParamA", reinterpret_cast<void*>(H_DialogBoxParamA));
    }
#define HOOK(name) o_##name = reinterpret_cast<name##_t>(HookImport(exe, "user32.dll", #name, reinterpret_cast<void*>(H_##name))); \
    if (!o_##name) o_##name = reinterpret_cast<name##_t>(GetProcAddress(GetModuleHandleA("user32.dll"), #name));
    HOOK(CreateWindowExA)
    HOOK(SetWindowPos)
    HOOK(ShowWindow)
    HOOK(SetForegroundWindow)
#undef HOOK
}

// ---------------------------------------------------------------- pacing and statistics

static int g_fpsTarget = 60;
static double g_nextFrame;
static HANDLE g_timer;
static uint64_t g_frames;
static double g_statStart, g_statMax, g_lastPresent;
static uint64_t g_statFrames;

uint64_t FramesPresented() { return g_frames; }

int RenderScale() { return g_ss; }

void MovieRect(long* l, long* t, long* w, long* h) {
    if (!Managed() || !g_presW || !g_presH) return;
    // game render coordinates -> back buffer (supersampling, hor+ offset) -> window (the back buffer's place in it)
    double kx = double(g_dest.right - g_dest.left) / g_presW, ky = double(g_dest.bottom - g_dest.top) / g_presH;
    double x = double(*l) / g_ss + (g_horPlus ? g_presXOffset : 0.0), y = double(*t) / g_ss;
    *l = long(g_dest.left + x * kx + 0.5);
    *t = long(g_dest.top + y * ky + 0.5);
    *w = long(double(*w) / g_ss * kx + 0.5);
    *h = long(double(*h) / g_ss * ky + 0.5);
}
static uint32_t g_firstFrameMs;
uint32_t FirstFrameMs() { return g_firstFrameMs; }

static double g_lastFrameDur;  // seconds between the two latest presents, 0 before the second frame
double LastFrameSeconds() { return g_lastFrameDur; }

static void Pace() {
    double now = NowSeconds();
    double frame = g_lastPresent > 0 ? now - g_lastPresent : 0;
    if (frame > g_statMax) g_statMax = frame;
    g_statFrames++;
    if (g_statStart == 0) g_statStart = now;
    if (now - g_statStart >= 10.0) {
        Log("[state] %llu frames, %.1f fps average over %.1f s, longest frame %.1f ms, target %d fps%s",
            (unsigned long long)g_frames, g_statFrames / (now - g_statStart), now - g_statStart, g_statMax * 1000.0,
            g_fpsTarget, g_horPlus ? "" : "");
        if (g_horPlus)
            Log("[state] hor+: %llu triangles shifted, %llu full-width fades stretched, %llu HUD group moves",
                (unsigned long long)g_shiftedTris, (unsigned long long)g_fadeTris, (unsigned long long)g_hudGroups);
        g_statStart = now;
        g_statFrames = 0;
        g_statMax = 0;
    }
    double period = 1.0 / g_fpsTarget;
    // never catch up with a burst of short frames after a late one: that is uneven motion, not smoothness
    g_nextFrame += period;
    if (g_nextFrame < now) g_nextFrame = now;
    double wait = g_nextFrame - now;
    if (wait > 0.002 && g_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((wait - 0.0015) * 1e7);
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, 100);
    }
    while (NowSeconds() < g_nextFrame) YieldProcessor();
    double t = NowSeconds();
    if (g_lastPresent > 0) g_lastFrameDur = t - g_lastPresent;
    g_lastPresent = t;
}

// ---------------------------------------------------------------- frame dumps (test evidence, never the desktop)

static uint32_t g_nextDump;

static void DumpBackBuffer(IDirect3DDevice8* dev) {
    if (g_cfg.frameDumpDir.empty()) return;
    LayerCallScope layer;  // its image surface is no game texture upload (O-121)
    uint32_t now = ElapsedMs();
    // SPARTA_FRAME_DUMP_FROM_MS: no dumps before this time (dense dumps of one scene without gigabytes of menus)
    static const uint32_t from = [] {
        char b[32];
        DWORD n = GetEnvironmentVariableA("SPARTA_FRAME_DUMP_FROM_MS", b, sizeof b);
        return n && n < sizeof b ? uint32_t(strtoul(b, nullptr, 10)) : 0u;
    }();
    if (now < from || now < g_nextDump) return;
    g_nextDump = now + uint32_t(g_cfg.frameDumpIntervalMs);
    IDirect3DSurface8* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    D3DLOCKED_RECT lr;
    if (FAILED(bb->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        static bool warned;
        if (!warned) Log("framedump: back buffer is not lockable (format %d)", int(desc.Format));
        warned = true;
        bb->Release();
        return;
    }
    // half size above 1280 px keeps a run's dumps small; SPARTA_FRAME_DUMP_FULL=1 keeps every pixel (text, filtering)
    static const bool full = GetEnvironmentVariableA("SPARTA_FRAME_DUMP_FULL", nullptr, 0) != 0;
    int step = desc.Width > 1280 && !full ? 2 : 1;
    int w = int(desc.Width) / step, h = int(desc.Height) / step;
    int stride = (w * 3 + 3) & ~3;
    std::vector<uint8_t> px(size_t(stride) * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) + size_t(y * step) * lr.Pitch;
        uint8_t* dst = px.data() + size_t(h - 1 - y) * stride;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) {
                const uint8_t* p = src + x * step * 4;
                b = p[0]; g = p[1]; r = p[2];
            } else if (desc.Format == D3DFMT_R5G6B5) {
                uint16_t v = reinterpret_cast<const uint16_t*>(src)[x * step];
                r = uint8_t((v >> 11) << 3); g = uint8_t(((v >> 5) & 63) << 2); b = uint8_t((v & 31) << 3);
            } else {
                uint16_t v = reinterpret_cast<const uint16_t*>(src)[x * step];
                r = uint8_t(((v >> 10) & 31) << 3); g = uint8_t(((v >> 5) & 31) << 3); b = uint8_t((v & 31) << 3);
            }
            dst[x * 3] = b; dst[x * 3 + 1] = g; dst[x * 3 + 2] = r;
        }
    }
    bb->UnlockRect();
    bb->Release();
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\frame_%07u_ms.bmp", g_cfg.frameDumpDir.c_str(), now);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + DWORD(px.size());
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = DWORD(px.size());
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------- Direct3D 8

using Direct3DCreate8_t = IDirect3D8*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice8**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
using SetTextureStageState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
using EndStateBlock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, DWORD*);

static CreateDevice_t o_CreateDevice;
using Clear_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
static Clear_t o_Clear;
static Present_t o_Present;
static Reset_t o_Reset;
static SetTextureStageState_t o_SetTextureStageState;
static EndStateBlock_t o_EndStateBlock;
static uint64_t g_rehooks;
static IDirect3D8* g_d3d;
static UINT g_adapter;
static D3DDEVTYPE g_devType;

static void LogPP(const char* what, const D3DPRESENT_PARAMETERS* pp) {
    Log("%s: %ux%u fmt=%d count=%u ms=%d swap=%d wnd=%p windowed=%d depth=%d/%d flags=0x%lx refresh=%u interval=0x%x", what,
        pp->BackBufferWidth, pp->BackBufferHeight, int(pp->BackBufferFormat), pp->BackBufferCount,
        int(pp->MultiSampleType), int(pp->SwapEffect), pp->hDeviceWindow, pp->Windowed, pp->EnableAutoDepthStencil,
        int(pp->AutoDepthStencilFormat), pp->Flags, pp->FullScreen_RefreshRateInHz, pp->FullScreen_PresentationInterval);
}

// Rewrites the game's present parameters for the managed window. `msaa`/`depth24` let the caller retry without them.
static void AdjustPP(D3DPRESENT_PARAMETERS* pp, HWND focus, bool msaa, bool depth24) {
    D3DDISPLAYMODE dm{};
    g_d3d->GetAdapterDisplayMode(g_adapter, &dm);
    pp->Windowed = Fullscreen() ? FALSE : TRUE;
    pp->BackBufferWidth = UINT(g_presW);
    pp->BackBufferHeight = UINT(g_presH);
    pp->BackBufferFormat = dm.Format;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    // fullscreen: the chosen refresh rate, no vsync wait (the layer paces frames itself)
    pp->FullScreen_RefreshRateInHz = Fullscreen() ? UINT(g_cfg.refresh) : 0;
    pp->FullScreen_PresentationInterval = Fullscreen() ? D3DPRESENT_INTERVAL_IMMEDIATE : 0;
    pp->hDeviceWindow = g_gameWnd ? g_gameWnd : focus;
    pp->MultiSampleType = D3DMULTISAMPLE_NONE;
    if (pp->EnableAutoDepthStencil && pp->AutoDepthStencilFormat == D3DFMT_D16 && depth24) {
        for (D3DFORMAT f : {D3DFMT_D24X8, D3DFMT_D24S8}) {
            if (SUCCEEDED(g_d3d->CheckDeviceFormat(g_adapter, g_devType, dm.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, f)) &&
                SUCCEEDED(g_d3d->CheckDepthStencilMatch(g_adapter, g_devType, dm.Format, dm.Format, f))) {
                pp->AutoDepthStencilFormat = f;
                break;
            }
        }
    }
    if (!g_cfg.frameDumpDir.empty()) {
        pp->Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;  // dumps read the back buffer; MSAA cannot be locked
    } else if (g_scene) {
        // the game draws into a texture (no multisampled textures in D3D8); supersampling smooths every edge instead
    } else if (msaa && g_cfg.msaa >= 2) {
        auto t = static_cast<D3DMULTISAMPLE_TYPE>(g_cfg.msaa);
        if (SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(g_adapter, g_devType, pp->BackBufferFormat, TRUE, t)) &&
            (!pp->EnableAutoDepthStencil ||
             SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(g_adapter, g_devType, pp->AutoDepthStencilFormat, TRUE, t)))) {
            pp->MultiSampleType = t;
            pp->Flags &= ~DWORD(D3DPRESENTFLAG_LOCKABLE_BACKBUFFER);
        } else {
            Log("d3d: %dx MSAA not supported for this format, disabled", g_cfg.msaa);
        }
    }
}

static void WriteRenderVars() {
    if (!g_cfg.renderWidthVar || !g_cfg.renderHeightVar) return;
    auto wv = reinterpret_cast<volatile int32_t*>(g_cfg.renderWidthVar);
    auto hv = reinterpret_cast<volatile int32_t*>(g_cfg.renderHeightVar);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(const_cast<int32_t*>(wv), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        !VirtualQuery(const_cast<int32_t*>(hv), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) {
        Log("d3d: render size variables are not mapped; picture will stay at the game's size");
        return;
    }
    // only write when they hold what the game asked for: proves the addresses belong to this executable
    if (*wv != g_reqW || *hv != g_reqH) {
        Log("d3d: render size variables hold %dx%d, expected %dx%d; refusing to write (wrong executable?)", *wv, *hv,
            g_reqW, g_reqH);
        return;
    }
    *wv = g_renderW;
    *hv = g_renderH;
    g_renderVarsOk = true;
    Log("d3d: game render size %dx%d -> %dx%d", g_reqW, g_reqH, g_renderW, g_renderH);
}

// the game maps its 512x240 space with these on every frame: a live supersampling change only rewrites them
static void WriteRenderVarsLive() {
    if (!g_renderVarsOk) return;
    *reinterpret_cast<volatile int32_t*>(g_cfg.renderWidthVar) = g_renderW;
    *reinterpret_cast<volatile int32_t*>(g_cfg.renderHeightVar) = g_renderH;
}

// ---- hor+: move pre-transformed (XYZRHW) vertices into the centred 4:3 area when the game unlocks a vertex buffer ----

using CreateVertexBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer8**);
using VbLock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer8*, UINT, UINT, BYTE**, DWORD);
using VbUnlock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer8*);
static CreateVertexBuffer_t o_CreateVertexBuffer;
static VbLock_t o_VbLock;
static VbUnlock_t o_VbUnlock;

struct TrackedVb {
    IDirect3DVertexBuffer8* vb;
    UINT stride, length, texOffset;  // texOffset = byte offset of the first texture coordinate (0 = none)
    BYTE* lockPtr;
    UINT lockSize;
    std::vector<uint32_t> savedRhw;  // rhw of every locked vertex before the sentinel went in
};

// Games lock whole vertex buffers without D3DLOCK_DISCARD and rewrite only what they draw this frame; shifting the
// rest again would move stale vertices further each time. At lock every vertex's rhw gets a NaN sentinel (originals
// kept); at unlock only triangles the game overwrote are remapped and the others get their rhw back untouched.
static const uint32_t kRhwSentinel = 0x7FC0DEADu;
static std::vector<TrackedVb> g_vbs;


static UINT FvfStride(DWORD fvf, UINT* texOffset) {
    UINT s = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ: s = 12; break;
        case D3DFVF_XYZRHW: s = 16; break;
        case D3DFVF_XYZB1: s = 16; break;
        case D3DFVF_XYZB2: s = 20; break;
        case D3DFVF_XYZB3: s = 24; break;
        case D3DFVF_XYZB4: s = 28; break;
        case D3DFVF_XYZB5: s = 32; break;
    }
    if (fvf & D3DFVF_NORMAL) s += 12;
    if (fvf & D3DFVF_PSIZE) s += 4;
    if (fvf & D3DFVF_DIFFUSE) s += 4;
    if (fvf & D3DFVF_SPECULAR) s += 4;
    UINT tex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    *texOffset = tex ? s : 0;
    for (UINT i = 0; i < tex; i++) {
        UINT fmt = (fvf >> (16 + 2 * i)) & 3;  // D3DFVF_TEXTUREFORMAT2 = 0, 3 = 1, 4 = 2, 1 = 3
        s += fmt == 0 ? 8 : fmt == 1 ? 12 : fmt == 2 ? 16 : 4;
    }
    return s;
}

static TrackedVb* FindVb(IDirect3DVertexBuffer8* vb) {
    for (auto& t : g_vbs)
        if (t.vb == vb) return &t;
    return nullptr;
}

static HRESULT STDMETHODCALLTYPE H_VbLock(IDirect3DVertexBuffer8* vb, UINT off, UINT size, BYTE** data, DWORD flags) {
    HRESULT hr = o_VbLock(vb, off, size, data, flags);
    static int logged;
    if (logged < 8 && FindVb(vb)) {
        logged++;
        Log("d3d: lock vb %p off=%u size=%u flags=0x%lx -> %p", vb, off, size, flags, SUCCEEDED(hr) ? *data : nullptr);
    }
    if (SUCCEEDED(hr)) {
        if (TrackedVb* t = FindVb(vb)) {
            t->lockPtr = (flags & D3DLOCK_READONLY) || !g_horPlus ? nullptr : *data;
            t->lockSize = size ? size : (off < t->length ? t->length - off : 0);
            if (t->lockPtr) {
                UINT n = t->lockSize / t->stride;
                t->savedRhw.resize(n);
                for (UINT i = 0; i < n; i++) {
                    uint32_t* rhw = reinterpret_cast<uint32_t*>(t->lockPtr + size_t(i) * t->stride + 12);
                    t->savedRhw[i] = *rhw;
                    *rhw = kRhwSentinel;
                }
            }
        }
    }
    return hr;
}

// SPARTA_VB_DUMP_AT_MS=N: the triangles the game wrote at the first unlock after N ms go to <frame dump dir>\vb_<ms>.bin
// (stride, then raw vertices; untouched by hor+), for offline analysis of how a frame is built (2D layers, depths)
static void DebugDumpVb(const TrackedVb& t) {
    static const uint32_t at = [] {
        char b[32];
        DWORD n = GetEnvironmentVariableA("SPARTA_VB_DUMP_AT_MS", b, sizeof b);
        return n && n < sizeof b ? uint32_t(strtoul(b, nullptr, 10)) : 0u;
    }();
    static int dumps;
    if (!at || dumps >= 3 || ElapsedMs() < at || g_cfg.frameDumpDir.empty()) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\vb_%07u_%d.bin", g_cfg.frameDumpDir.c_str(), ElapsedMs(), dumps++);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[2] = {t.stride, t.lockSize / t.stride};
    fwrite(hdr, 4, 2, f);
    for (UINT i = 0; i < hdr[1]; i++) {
        const BYTE* v = t.lockPtr + size_t(i) * t.stride;
        uint32_t rhw = *reinterpret_cast<const uint32_t*>(v + 12);
        uint8_t written = rhw != kRhwSentinel;
        fwrite(&written, 1, 1, f);
        fwrite(v, 1, t.stride, f);
    }
    fclose(f);
}

// ---- ultrawide HUD (hor+): the game's 2D overlay is drawn last, as screen-aligned sprites on top of its 3D. In
// frames that have 3D, that trailing run of sprites (by draw order: the game's painter depth, nearest = last) is
// grouped by touching pieces (a portrait and its number move together) and each group in the top or bottom band of
// the screen moves out in proportion to the wider picture, so corner HUD reaches the screen's corners and a row of
// portraits spreads evenly. Dialogs, menus and pointers (mid-screen), wide pieces and 2D-only screens stay put.
// Settings: [Display] HudSpread, HudBand, HudMaxWidth, HudMinScene, HudMargin.
HudSettings& HudConfig() {
    static HudSettings h = [] {
        HudSettings r;
        r.on = IniInt("Display", "HudSpread", 0) != 0;
        r.band = float(IniInt("Display", "HudBand", 30)) / 100.0f;           // % of the height, top and bottom
        r.maxWidth = float(IniInt("Display", "HudMaxWidth", 50)) / 100.0f;   // % of the 4:3 width
        r.minScene = IniInt("Display", "HudMinScene", 100);                   // 3D triangles needed in the frame
        r.margin = float(IniInt("Display", "HudMargin", 24));                 // px at 1920 wide: "touching"
        return r;
    }();
    return h;
}

static void HudShifts(const TrackedVb& t, UINT tris, std::vector<float>& extra) {
    extra.assign(tris, 0.0f);
    const HudSettings& h = HudConfig();
    if (!h.on) return;
    struct Tri {
        UINT i;
        float z, x0, x1, y0, y1;
        bool sprite;
    };
    std::vector<Tri> list;
    list.reserve(tris);
    int scene = 0;
    for (UINT i = 0; i < tris; i++) {
        const BYTE* v = t.lockPtr + size_t(i) * 3 * t.stride;
        float xs[3], ys[3];
        bool written = true;
        for (int k = 0; k < 3; k++) {
            const float* f = reinterpret_cast<const float*>(v + k * t.stride);
            written = written && *reinterpret_cast<const uint32_t*>(f + 3) != kRhwSentinel;
            xs[k] = f[0];
            ys[k] = f[1];
        }
        if (!written) continue;
        auto distinct = [](const float* a) {
            int d = 1;
            if (std::fabs(a[1] - a[0]) > 0.05f) d++;
            if (std::fabs(a[2] - a[0]) > 0.05f && std::fabs(a[2] - a[1]) > 0.05f) d++;
            return d;
        };
        bool sprite = distinct(xs) <= 2 && distinct(ys) <= 2;
        scene += !sprite;
        list.push_back({i, reinterpret_cast<const float*>(v)[2], std::min({xs[0], xs[1], xs[2]}), std::max({xs[0], xs[1], xs[2]}),
                        std::min({ys[0], ys[1], ys[2]}), std::max({ys[0], ys[1], ys[2]}), sprite});
    }
    if (scene < h.minScene) return;  // 2D screens (title, loading, menus without 3D) keep their layout
    std::sort(list.begin(), list.end(), [](const Tri& a, const Tri& b) { return a.z < b.z; });
    size_t nHud = 0;
    while (nHud < list.size() && nHud < 1024 && list[nHud].sprite) nHud++;
    if (!nHud) return;
    std::vector<int> parent(nHud);
    for (size_t a = 0; a < nHud; a++) parent[a] = int(a);
    auto find = [&](int a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    };
    const float m = h.margin * float(g_renderW) / 1920.0f;
    for (size_t a = 0; a < nHud; a++)
        for (size_t b = a + 1; b < nHud; b++) {
            const Tri &p = list[a], &q = list[b];
            if (p.x0 - m <= q.x1 && q.x0 - m <= p.x1 && p.y0 - m <= q.y1 && q.y0 - m <= p.y1) parent[find(int(a))] = find(int(b));
        }
    struct Box {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    };
    std::vector<Box> box(nHud);
    for (size_t a = 0; a < nHud; a++) {
        Box& g = box[find(int(a))];
        g.x0 = std::min(g.x0, list[a].x0);
        g.x1 = std::max(g.x1, list[a].x1);
        g.y0 = std::min(g.y0, list[a].y0);
        g.y1 = std::max(g.y1, list[a].y1);
    }
    const float W = float(g_renderW), H = float(g_renderH), k = float(g_bbW) / W;
    std::vector<float> shift(nHud, 0.0f);
    for (size_t a = 0; a < nHud; a++) {
        if (find(int(a)) != int(a)) continue;
        const Box& g = box[a];
        bool banded = g.y1 < h.band * H || g.y0 > (1.0f - h.band) * H;
        if (!banded || g.x1 - g.x0 > h.maxWidth * W) continue;
        float sft = ((g.x0 + g.x1) * 0.5f - W * 0.5f) * (k - 1.0f);
        sft = std::max(sft, -g_xOffset - g.x0);       // never past the screen's left edge
        sft = std::min(sft, W + g_xOffset - g.x1);    // nor its right edge
        shift[a] = sft;
        g_hudGroups++;
    }
    for (size_t a = 0; a < nHud; a++) extra[list[a].i] = shift[find(int(a))];
}

static HRESULT STDMETHODCALLTYPE H_VbUnlock(IDirect3DVertexBuffer8* vb) {
    TrackedVb* t = FindVb(vb);
    if (t && t->lockPtr) DebugDumpVb(*t);
    if (t && t->lockPtr && g_horPlus) {
        const float edge = 0.75f, right = float(g_renderW), bottom = float(g_renderH);
        UINT n = t->lockSize / t->stride;
        UINT tris = n / 3;  // triangle lists: three vertices per triangle
        static std::vector<float> hud;
        HudShifts(*t, tris, hud);
        for (UINT i = 0; i < tris; i++) {
            BYTE* v = t->lockPtr + size_t(i) * 3 * t->stride;
            uint32_t* rhw[3] = {reinterpret_cast<uint32_t*>(v + 12), reinterpret_cast<uint32_t*>(v + t->stride + 12),
                                reinterpret_cast<uint32_t*>(v + 2 * t->stride + 12)};
            if (*rhw[0] == kRhwSentinel || *rhw[1] == kRhwSentinel || *rhw[2] == kRhwSentinel) {
                for (int k = 0; k < 3; k++)  // not (completely) written this time: leave it as it was
                    if (*rhw[k] == kRhwSentinel) *rhw[k] = t->savedRhw[i * 3 + k];
                continue;
            }
            float* x[3] = {reinterpret_cast<float*>(v), reinterpret_cast<float*>(v + t->stride),
                           reinterpret_cast<float*>(v + 2 * t->stride)};
            bool fullScreen = true;  // every corner on the 4:3 frame's edges: a full-screen rectangle
            for (int k = 0; k < 3; k++) {
                float px = *x[k], py = *reinterpret_cast<const float*>(v + k * t->stride + 4);
                bool onX = px <= edge || px >= right - edge, onY = py <= edge || py >= bottom - edge;
                fullScreen = fullScreen && onX && onY;
            }
            bool untextured = true;
            if (t->texOffset) {
                const float* uv0 = reinterpret_cast<const float*>(v + t->texOffset);
                for (int k = 1; k < 3 && untextured; k++) {
                    const float* uv = reinterpret_cast<const float*>(v + k * t->stride + t->texOffset);
                    untextured = uv[0] == uv0[0] && uv[1] == uv0[1];
                }
            }
            if (untextured && fullScreen) {
                // a full-width flat triangle (fade, flash, tint): stretch it over the whole screen
                for (float* p : x) *p = *p <= edge ? 0.0f : (*p >= right - edge ? float(g_bbW) : *p + g_xOffset);
                g_fadeTris++;
            } else {
                for (float* p : x) *p += g_xOffset + hud[i];
                g_shiftedTris++;
            }
        }
        for (UINT k = tris * 3; k < n; k++) {  // a trailing partial triangle is never drawn: restore it
            uint32_t* r = reinterpret_cast<uint32_t*>(t->lockPtr + size_t(k) * t->stride + 12);
            if (*r == kRhwSentinel) *r = t->savedRhw[k];
        }
        t->lockPtr = nullptr;
    }
    return o_VbUnlock(vb);
}

static HRESULT STDMETHODCALLTYPE H_CreateVertexBuffer(IDirect3DDevice8* dev, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                      IDirect3DVertexBuffer8** out) {
    HRESULT hr = o_CreateVertexBuffer(dev, len, usage, fvf, pool, out);
    if (SUCCEEDED(hr) && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        UINT texOff = 0;
        UINT stride = FvfStride(fvf, &texOff);
        g_vbs.push_back({*out, stride, len, texOff, nullptr, 0});
        void* p;
        if ((p = HookVtable(*out, 11, reinterpret_cast<void*>(H_VbLock)))) o_VbLock = reinterpret_cast<VbLock_t>(p);
        if ((p = HookVtable(*out, 12, reinterpret_cast<void*>(H_VbUnlock)))) o_VbUnlock = reinterpret_cast<VbUnlock_t>(p);
        Log("d3d: pre-transformed vertex buffer %p (%u bytes, fvf 0x%lx, stride %u)%s", *out, len, fvf, stride,
            g_horPlus ? ": hor+ remap" : "");
    }
    return hr;
}

// ---- offscreen scene: supersampling (averaged 2x2 at Present) and the ambient hor+ side fill ----
// The game never asks for the back buffer, the render target or the viewport (it clears depth and draws), so its
// frame can go into a render-target texture. At Present the layer draws that texture into the real back buffer and
// into a chain of half-size copies; before the next frame the smallest copy, magnified (heavily blurred), is painted
// into the hor+ side areas, where the game draws nothing at the edges of its levels. Game geometry covers it.

static IDirect3DSurface8 *g_sceneSurf, *g_sceneDepth, *g_backSurf;
static std::vector<IDirect3DTexture8*> g_chain;  // halving copies of the finished frame (ambient source)
static DWORD g_sceneState;                       // the game's states around the layer's passes
static D3DFORMAT g_bbFmt = D3DFMT_UNKNOWN, g_depthFmt = D3DFMT_UNKNOWN;
static bool g_wantDepth;

static void SceneRelease(IDirect3DDevice8* dev) {
    for (auto* t : g_chain) t->Release();
    g_chain.clear();
    if (g_sceneSurf) g_sceneSurf->Release();
    if (g_sceneTex) g_sceneTex->Release();
    if (g_sceneDepth) g_sceneDepth->Release();
    if (g_backSurf) g_backSurf->Release();
    g_sceneSurf = g_sceneDepth = g_backSurf = nullptr;
    g_sceneTex = nullptr;
    if (g_sceneState) dev->DeleteStateBlock(g_sceneState);
    g_sceneState = 0;
}

static bool SceneCreate(IDirect3DDevice8* dev) {
    LayerCallScope layer;  // the scene's textures are the layer's, not the game's (O-121)
    if (!g_scene) return false;
    bool ok = SUCCEEDED(dev->CreateTexture(UINT(g_bbW), UINT(g_bbH), 1, D3DUSAGE_RENDERTARGET, g_bbFmt, D3DPOOL_DEFAULT, &g_sceneTex)) &&
              SUCCEEDED(g_sceneTex->GetSurfaceLevel(0, &g_sceneSurf)) &&
              (!g_wantDepth || SUCCEEDED(dev->CreateDepthStencilSurface(UINT(g_bbW), UINT(g_bbH), g_depthFmt, D3DMULTISAMPLE_NONE, &g_sceneDepth))) &&
              SUCCEEDED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &g_backSurf)) &&
              SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &g_sceneState));
    if (ok && g_horPlus && g_cfg.sideFill == "ambient") {
        UINT w = UINT(g_bbW), h = UINT(g_bbH);
        while (ok && w > 256) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
            IDirect3DTexture8* t = nullptr;
            ok = SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, g_bbFmt, D3DPOOL_DEFAULT, &t));
            if (ok) g_chain.push_back(t);
        }
    }
    if (!ok) {
        Log("d3d: offscreen scene %dx%d could not be created; drawing straight into the back buffer", g_bbW, g_bbH);
        SceneRelease(dev);
        return false;
    }
    dev->SetRenderTarget(g_sceneSurf, g_sceneDepth);
    D3DRECT all{0, 0, g_bbW, g_bbH};
    o_Clear(dev, 1, &all, D3DCLEAR_TARGET | (g_sceneDepth ? D3DCLEAR_ZBUFFER : 0), D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    Log("d3d: offscreen scene %dx%d ready (%u side-fill level(s))", g_bbW, g_bbH, unsigned(g_chain.size()));
    return true;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

// Draws `tex` (its [u0,u1]x[v0,v1] part) over the target rectangle; pixel centres sit on integer coordinates in
// D3D8, so -0.5 maps texels to pixels: an exact 2:1 reduction with bilinear filtering averages 2x2 texels.
static void DrawQuad(IDirect3DDevice8* dev, IDirect3DTexture8* tex, float x0, float y0, float x1, float y1, float u0,
                     float v0, float u1, float v1) {
    QuadVertex q[4] = {{x0 - 0.5f, y0 - 0.5f, 0, 1, u0, v0}, {x1 - 0.5f, y0 - 0.5f, 0, 1, u1, v0},
                       {x0 - 0.5f, y1 - 0.5f, 0, 1, u0, v1}, {x1 - 0.5f, y1 - 0.5f, 0, 1, u1, v1}};
    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
}

static void QuadStates(IDirect3DDevice8* dev) {
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
}

// Before Present: the finished scene into the back buffer (2x2 average when supersampling) and the side-fill chain.
static void ScenePresentPass(IDirect3DDevice8* dev) {
    LayerCallScope layer;
    dev->CaptureStateBlock(g_sceneState);
    dev->BeginScene();
    QuadStates(dev);
    dev->SetRenderTarget(g_backSurf, nullptr);
    if (g_ss == 1) {  // 1:1 copy
        dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    }
    DrawQuad(dev, g_sceneTex, 0, 0, float(g_presW), float(g_presH), 0, 0, 1, 1);
    dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DTexture8* src = g_sceneTex;
    for (auto* t : g_chain) {
        IDirect3DSurface8* s = nullptr;
        if (FAILED(t->GetSurfaceLevel(0, &s))) break;
        D3DSURFACE_DESC d;
        s->GetDesc(&d);
        dev->SetRenderTarget(s, nullptr);
        DrawQuad(dev, src, 0, 0, float(d.Width), float(d.Height), 0, 0, 1, 1);
        s->Release();
        src = t;
    }
    dev->SetRenderTarget(g_backSurf, nullptr);  // the overlay and the frame dump read the finished picture
    dev->EndScene();
}

// After Present: the scene is the render target again; the hor+ sides start as the blurred last frame (or black).
static void SceneNextFrame(IDirect3DDevice8* dev) {
    LayerCallScope layer;
    dev->SetRenderTarget(g_sceneSurf, g_sceneDepth);
    if (g_horPlus) {
        if (!g_chain.empty()) {
            dev->BeginScene();
            QuadStates(dev);
            float l = g_xOffset, r = g_xOffset + float(g_renderW), w = float(g_bbW), h = float(g_bbH);
            DrawQuad(dev, g_chain.back(), 0, 0, l, h, 0, 0, l / w, 1);
            DrawQuad(dev, g_chain.back(), r, 0, w, h, r / w, 0, 1, 1);
            dev->EndScene();
        } else {
            D3DRECT sides[2] = {{0, 0, LONG(g_xOffset), g_bbH}, {LONG(g_xOffset) + g_renderW, 0, g_bbW, g_bbH}};
            o_Clear(dev, 2, sides, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        }
    }
    dev->ApplyStateBlock(g_sceneState);
}

// ---- live settings (F1 overlay) ----
static bool g_pendingDisplay;
static bool g_pendingAmbient;

static void ComputeFpsTarget() {
    g_fpsTarget = g_cfg.fpsLimit < 0 ? int(g_refreshHz ? g_refreshHz : 60) : (g_cfg.fpsLimit == 0 ? 1000 : g_cfg.fpsLimit);
    if (g_fpsTarget > 1000) g_fpsTarget = 1000;  // >= 1 ms per frame: the game measures frames in whole milliseconds
    if (g_fpsTarget < 10) g_fpsTarget = 10;
}

void SetFpsLimit(int fps) {
    g_cfg.fpsLimit = fps;
    ComputeFpsTarget();
}
int FpsTarget() { return g_fpsTarget; }
void SetAnisotropy(int level) { g_cfg.anisotropy = level; }  // the game sets its filters every frame
void SetRunInBackground(bool on) { g_cfg.runInBackground = on; }
void RequestSideFill(bool ambient) {
    g_pendingAmbient = ambient;
    g_pendingDisplay = true;
}
void* Direct3D8Object() { return g_d3d; }
unsigned Direct3DAdapter() { return g_adapter; }
DisplayInfo GetDisplayInfo() {
    return {g_outW, g_outH, g_presW, g_presH, g_renderW, g_renderH, g_ss, g_horPlus, g_sceneTex != nullptr, g_lastFrameDur, g_frames};
}

// Between two frames (after Present): a side-fill change rebuilds the scene (or returns to drawing straight into the
// back buffer); the game's render size stays. Supersampling is not live: the game lays out menus and fixed quads once
// at its render size, so a new size leaves them at the old scale until a restart (O-120). MSAA needs a new device.
static void ApplyPending(IDirect3DDevice8* dev) {
    if (!g_pendingDisplay) return;
    g_pendingDisplay = false;
    if (g_sceneTex) SceneRelease(dev);
    g_cfg.sideFill = g_pendingAmbient ? "ambient" : "black";
    ComputeSizes();
    if (!(g_scene && SceneCreate(dev))) {
        if (g_scene) {  // the scene could not be created: plain drawing at the window size
            g_cfg.supersample = 1;
            g_cfg.sideFill = "black";
            ComputeSizes();
        }
        IDirect3DSurface8* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            dev->SetRenderTarget(bb, g_autoDepth);
            bb->Release();
        }
    }
    WriteRenderVarsLive();
    Log("display: live change: side fill %s, offscreen scene %s, game render %dx%d", g_cfg.sideFill.c_str(),
        g_sceneTex ? "yes" : "no", g_renderW, g_renderH);
}

// SPARTA_TRACE_PRESENT_MS=N: every N ms, log the game's call chain into Present (which loop is drawing), reads only
static uint32_t g_tracePresentMs, g_nextTrace;

static void TracePresentCaller(void* frame) {
    uint32_t now = ElapsedMs();
    if (!g_tracePresentMs || now < g_nextTrace) return;
    g_nextTrace = now + g_tracePresentMs;
    DWORD ret[8] = {};
    DWORD ebp = reinterpret_cast<DWORD>(frame);
    for (int i = 0; i < 8 && ebp; i++) {
        DWORD f[2];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ebp), f, sizeof f, &got) || got != sizeof f) break;
        ret[i] = f[1];
        if (f[0] <= ebp) break;
        ebp = f[0];
    }
    Log("[present] chain %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", ret[0], ret[1], ret[2], ret[3], ret[4], ret[5],
        ret[6], ret[7]);
}

// The runtime writes its own state-setting entries back into the device's vtable (state block recording does): the
// layer's hooks (texture redirection, filtering) are re-applied right away.
static void KeepHooks(IDirect3DDevice8* dev, const char* when) {
    if (int n = RehookDevice(dev)) {
        if (g_rehooks++ < 3) Log("d3d: %d device hook(s) had been written back by the runtime (%s); re-applied", n, when);
    }
}

static HRESULT STDMETHODCALLTYPE H_EndStateBlock(IDirect3DDevice8* dev, DWORD* token) {
    HRESULT hr = o_EndStateBlock(dev, token);
    KeepHooks(dev, "after EndStateBlock");
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice8* dev, const RECT* src, const RECT* dst, HWND wnd,
                                           const RGNDATA* dirty) {
    if (!g_firstFrameMs) g_firstFrameMs = ElapsedMs() ? ElapsedMs() : 1;
    KeepHooks(dev, "at Present");
    TracePresentCaller(__builtin_frame_address(0));
    if (g_sceneTex) {
        ScenePresentPass(dev);
        OverlayRender(dev, g_presW, g_presH);  // on the finished picture, at window resolution
        DumpBackBuffer(dev);
        HRESULT hr = o_Present(dev, src, (Managed() && g_pillarbox) ? &g_dest : dst, wnd, dirty);
        SceneNextFrame(dev);
        ApplyPending(dev);
        AutosaveTick();
        if (++g_frames % 60 == 1) PaintBars();
        Pace();
        return hr;
    }
    OverlayRender(dev, g_presW, g_presH);
    DumpBackBuffer(dev);
    HRESULT hr = o_Present(dev, src, (Managed() && g_pillarbox) ? &g_dest : dst, wnd, dirty);
    if (g_horPlus && o_Clear) {
        // the game clears only its 4:3 area: start every frame with black sides, so what the 3D does not cover there
        // (the edge of a level) is black instead of the previous screen
        D3DRECT sides[2] = {{0, 0, LONG(g_xOffset), g_bbH}, {LONG(g_xOffset) + g_renderW, 0, g_bbW, g_bbH}};
        o_Clear(dev, 2, sides, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    }
    ApplyPending(dev);
    AutosaveTick();
    if (++g_frames % 60 == 1) PaintBars();
    Pace();
    return hr;
}

static void UseMipChains(IDirect3DDevice8* dev);

static HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice8* dev, D3DPRESENT_PARAMETERS* pp) {
    LogPP("d3d: Reset requested", pp);
    if (Managed()) AdjustPP(pp, pp->hDeviceWindow, true, g_cfg.depth24);
    bool scene = g_sceneTex != nullptr;
    if (scene) SceneRelease(dev);  // default-pool resources and surface references must be gone before Reset
    if (g_autoDepth) g_autoDepth->Release();
    g_autoDepth = nullptr;
    OverlayDeviceReset(true);
    HRESULT hr = o_Reset(dev, pp);
    Log("d3d: Reset -> 0x%08lx", hr);
    if (SUCCEEDED(hr)) {
        UseMipChains(dev);  // Reset restores the default states
        if (g_wantDepth) dev->GetDepthStencilSurface(&g_autoDepth);
        if (scene) SceneCreate(dev);
    }
    OverlayDeviceReset(false);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetTextureStageState(IDirect3DDevice8* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE t,
                                                        DWORD v) {
    if (g_layerCalls) return o_SetTextureStageState(dev, stage, t, v);
    if (g_cfg.anisotropy > 0 && (t == D3DTSS_MAGFILTER || t == D3DTSS_MINFILTER) && v != D3DTEXF_NONE) {
        if (g_cfg.anisotropy >= 2 && t == D3DTSS_MINFILTER) {
            o_SetTextureStageState(dev, stage, D3DTSS_MAXANISOTROPY, DWORD(g_cfg.anisotropy));
            v = D3DTEXF_ANISOTROPIC;
        } else {
            v = D3DTEXF_LINEAR;
        }
    }
    // replacement textures carry full mip chains, but D3D8 samples only level 0 unless a mip filter is set: larger
    // replacements (8x font sheets, distant ground) would alias. The game's own textures have one level each.
    if (t == D3DTSS_MIPFILTER && v == D3DTEXF_NONE && TexturePackActive()) v = D3DTEXF_LINEAR;
    return o_SetTextureStageState(dev, stage, t, v);
}

static void UseMipChains(IDirect3DDevice8* dev) {
    if (!TexturePackActive() || !o_SetTextureStageState) return;
    for (DWORD s = 0; s < 2; s++) o_SetTextureStageState(dev, s, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
}

static HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D8* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** out) {
    LogPP("d3d: CreateDevice requested", pp);
    Log("d3d: adapter=%u type=%d focus=%p flags=0x%lx", adapter, int(type), focus, flags);
    g_d3d = d3d;
    UINT count = d3d->GetAdapterCount();
    if (adapter >= count) {
        // the old launcher counted DirectDraw drivers (incl. virtual displays); Direct3D 8 may list fewer adapters
        Log("d3d: adapter %u does not exist (%u adapter(s)); using the primary adapter", adapter, count);
        adapter = 0;
    }
    g_adapter = adapter;
    g_devType = type;
    if (!Managed()) return o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (!g_gameWnd) {  // window was created before the hooks (or with another class): size it here
        g_gameWnd = focus;
        RECT rc;
        GetClientRect(focus, &rc);
        g_reqW = rc.right;
        g_reqH = rc.bottom;
        ComputeSizes();
    }
    WrapGameWindow(g_gameWnd);
    D3DPRESENT_PARAMETERS orig = *pp;
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        *pp = orig;
        AdjustPP(pp, focus, attempt == 0, attempt < 2 && g_cfg.depth24);
        LogPP(attempt ? "d3d: retry with" : "d3d: creating with", pp);
        hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
        if (SUCCEEDED(hr)) break;
        Log("d3d: CreateDevice failed 0x%08lx", hr);
        if (attempt == 2 && Fullscreen()) {  // the chosen display mode does not work: borderless at the monitor size
            Log("d3d: exclusive fullscreen %dx%d@%d failed; falling back to borderless", g_outW, g_outH, g_cfg.refresh);
            g_cfg.mode = "borderless";
            ComputeSizes();
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
            o_SetWindowPos(g_gameWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, g_winW, g_winH, SWP_NOZORDER);
            attempt = -1;
        }
    }
    if (FAILED(hr)) return hr;
    g_bbFmt = pp->BackBufferFormat;
    g_wantDepth = pp->EnableAutoDepthStencil != 0;
    g_depthFmt = pp->AutoDepthStencilFormat;
    o_Clear = reinterpret_cast<Clear_t>((*reinterpret_cast<void***>(*out))[36]);  // not hooked: called directly
    if (g_wantDepth) (*out)->GetDepthStencilSurface(&g_autoDepth);
    if (g_scene && !SceneCreate(*out) && g_ssWanted > 1) {
        g_cfg.supersample = 1;  // the game must not be told about a scene size that does not exist
        ComputeSizes();
    }
    WriteRenderVars();
    D3DDISPLAYMODE dm{};
    d3d->GetAdapterDisplayMode(adapter, &dm);
    g_refreshHz = dm.RefreshRate ? dm.RefreshRate : 60;
    ComputeFpsTarget();
    Log("d3d: device %p ready; desktop %ux%u@%uHz, frame pacing at %d fps", *out, dm.Width, dm.Height, dm.RefreshRate,
        g_fpsTarget);
    HookDevice(*out, 15, reinterpret_cast<void*>(H_Present), reinterpret_cast<void**>(&o_Present));
    HookDevice(*out, 14, reinterpret_cast<void*>(H_Reset), reinterpret_cast<void**>(&o_Reset));
    HookDevice(*out, 63, reinterpret_cast<void*>(H_SetTextureStageState), reinterpret_cast<void**>(&o_SetTextureStageState));
    HookDevice(*out, 53, reinterpret_cast<void*>(H_EndStateBlock), reinterpret_cast<void**>(&o_EndStateBlock));
    o_Clear = reinterpret_cast<Clear_t>((*reinterpret_cast<void***>(*out))[36]);  // not hooked: called directly
    InstallTextureHooks(*out);
    UseMipChains(*out);
    HookDevice(*out, 23, reinterpret_cast<void*>(H_CreateVertexBuffer), reinterpret_cast<void**>(&o_CreateVertexBuffer));
    return hr;
}

static HMODULE g_realD3d8;

static FARPROC RealProc(const char* name) {
    if (!g_realD3d8) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);  // SysWOW64 for a 32-bit process
        std::string path = std::string(sys) + "\\d3d8.dll";
        g_realD3d8 = LoadLibraryA(path.c_str());
        Log("d3d: system d3d8 %s %s", path.c_str(), g_realD3d8 ? "loaded" : "MISSING");
    }
    return g_realD3d8 ? GetProcAddress(g_realD3d8, name) : nullptr;
}

}  // namespace compat

using namespace compat;

extern "C" IDirect3D8* WINAPI Sparta_Direct3DCreate8(UINT sdk) {
    SetProcessDPIAware();  // a DPI-unaware window would be bitmap-stretched by Windows on scaled displays
    static bool muted;
    if (!muted) {
        muted = true;
        if (g_cfg.mute) MuteProcessAudio();
        else CheckProcessAudio();
    }
    {
        char buf[32];
        if (GetEnvironmentVariableA("SPARTA_TRACE_PRESENT_MS", buf, sizeof buf)) g_tracePresentMs = uint32_t(atoi(buf));
    }
    if (!g_timer) g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) {
        timeBeginPeriod(1);
        g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    auto real = reinterpret_cast<Direct3DCreate8_t>(RealProc("Direct3DCreate8"));
    if (!real) return nullptr;
    IDirect3D8* d3d = real(sdk);
    Log("d3d: Direct3DCreate8(%u) -> %p", sdk, d3d);
    if (d3d) {
        void* p = HookVtable(d3d, 15, reinterpret_cast<void*>(H_CreateDevice));
        if (p) o_CreateDevice = reinterpret_cast<CreateDevice_t>(p);
    }
    return d3d;
}

// rarely used d3d8 exports, forwarded so the proxy is a drop-in replacement
extern "C" HRESULT WINAPI Sparta_ValidatePixelShader(DWORD* a, DWORD* b, BOOL c, char** d) {
    auto f = reinterpret_cast<HRESULT(WINAPI*)(DWORD*, DWORD*, BOOL, char**)>(RealProc("ValidatePixelShader"));
    return f ? f(a, b, c, d) : E_FAIL;
}
extern "C" HRESULT WINAPI Sparta_ValidateVertexShader(DWORD* a, DWORD* b, DWORD* c, BOOL d, char** e) {
    auto f = reinterpret_cast<HRESULT(WINAPI*)(DWORD*, DWORD*, DWORD*, BOOL, char**)>(RealProc("ValidateVertexShader"));
    return f ? f(a, b, c, d, e) : E_FAIL;
}
extern "C" void WINAPI Sparta_DebugSetMute() {
    auto f = reinterpret_cast<void(WINAPI*)()>(RealProc("DebugSetMute"));
    if (f) f();
}
