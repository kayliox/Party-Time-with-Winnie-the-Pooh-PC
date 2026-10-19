// Shared declarations of the SpartaRecomp PC compatibility layer (a d3d8.dll proxy loaded by old Windows games).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace compat {

// ---- logging (spartacompat.log next to the game executable) ----
void LogInit(const std::string& path);
void Log(const char* fmt, ...);

// ---- configuration (spartacompat.ini next to the game executable) ----
struct Config {
    std::string exeDir;       // directory of the game executable, with trailing backslash
    std::string iniPath;      // spartacompat.ini

    // [VirtualCd] a folder that the game sees as a CD-ROM drive
    bool vcdEnable = false;
    std::string vcdRoot;      // absolute, no trailing backslash
    char vcdDrive = 0;        // drive letter (upper case), chosen at startup when "auto"

    // [Registry] keys under these prefixes are served from a portable ini file instead of the real registry
    std::vector<std::string> regPrefixes;  // e.g. "HKCU\SOFTWARE\Disney Interactive"
    std::string regStore;                  // absolute path of the store ini

    // [Display]
    std::string mode = "borderless";  // borderless | fullscreen (exclusive) | windowed | game (leave the window alone)
    int width = 0, height = 0;         // output resolution, 0 = the monitor's size (borderless scales it to the screen)
    int refresh = 0;                   // exclusive fullscreen refresh rate, 0 = the adapter's default
    std::string aspect = "4:3";        // 4:3 (pillarbox) | stretch
    int fpsLimit = -1;                 // -1 = monitor refresh rate, 0 = no limit (1000 fps guard), N = N fps
    int msaa = 0;                      // 0, 2, 4, 8
    int anisotropy = 0;                // 0 = leave the game's filters alone, 1 = bilinear, 2..16 = anisotropic
    bool depth24 = true;               // upgrade a 16-bit depth buffer to 24 bits when available
    int supersample = 1;               // 2 = the game renders at twice the size, averaged down 2x2 at Present (all edges)
    std::string sideFill = "black";    // hor+ side areas where the game draws nothing: black | ambient (blurred scene)
    bool hideCursor = true;
    bool runInBackground = false;      // keep running when another window is in front (the game pauses itself)
    bool overlay = true;               // [Overlay] Enable: F1 settings overlay

    // [Game] internal render size variables written at device creation (absolute virtual addresses)
    uintptr_t renderWidthVar = 0, renderHeightVar = 0;

    // [Input] fill the game's pad settings from the DirectInput devices present at startup
    bool autoPads = false;
    std::string padRegKey;          // registry key that receives the pad values
    std::string padCountValue;      // e.g. NbPad
    std::string playerCountValue;   // e.g. NbPlayer
    std::string comboValueFormat;   // e.g. ComboIndex%d (1-based player)
    std::string padGuidValueFormat; // e.g. CONTROL_GUID_%d%d (pad, part 1..5)
    int keyboardPlayers = 2;        // devices 0..keyboardPlayers-1 are keyboard layouts
    int maxPlayers = 4;

    // test hooks (environment): frame dumps, hidden window, scripted keyboard input
    std::string frameDumpDir;       // SPARTA_FRAME_DUMP_DIR
    int frameDumpIntervalMs = 2000; // SPARTA_FRAME_DUMP_INTERVAL_MS
    bool windowHidden = false;      // SPARTA_WINDOW_HIDDEN=1
    std::string inputScript;        // SPARTA_INPUT_SCRIPT: "ms:DIK[:holdms],..." (hex DIK codes)
    int exitAfterMs = 0;            // SPARTA_EXIT_AFTER_MS: terminate the process after this time
    bool mute = false;              // SPARTA_MUTE=1: mute the game's audio session
};

extern Config g_cfg;

bool LoadConfig();
std::string IniString(const char* section, const char* key, const char* def);
int IniInt(const char* section, const char* key, int def);

// ---- hooking helpers ----
// Replaces an import of the main executable (by DLL and function name). Returns the original pointer or nullptr.
void* HookImport(HMODULE module, const char* dll, const char* func, void* replacement);
// Replaces entry `index` of a COM object's vtable. Returns the original pointer.
void* HookVtable(void* object, int index, void* replacement);
// Hooks an IDirect3DDevice8 method and keeps it hooked: *original receives the pointer to chain to, and
// RehookDevice() re-applies every registered device hook the runtime has written back (returns how many).
void HookDevice(void* device, int index, void* replacement, void** original);
int RehookDevice(void* device);
// Applies the [Patches] section: "name = va : original-hex : new-hex". Returns false if any patch was refused.
bool ApplyPatches();
// Applies the [Hooks] section: "name = va : original-hex : builtin[:arg]" (a jump into a built-in function).
bool ApplyHooks();

// ---- subsystems ----
void InstallVcdHooks(HMODULE exe);
void InstallRegistryHooks(HMODULE exe);
void InstallWindowHooks(HMODULE exe);
void InstallInputHooks(HMODULE exe);
void AutofillPads();
void StartPadEnumeration();
void LogWatches();
void MuteProcessAudio();        // test runs (SPARTA_MUTE=1), with a marker file until undone
void UnmuteTestAudio();         // before a test run's timed exit
void CheckProcessAudio();       // normal start: undo a test mute left behind, report a Windows mixer mute
bool AudioMutedInWindows();
bool UnmuteProcessAudio();

// Autosave ([Autosave] in spartacompat.ini): the game's own save action, run when its progress block changes
struct AutosaveStatus {
    bool configured = false;
    uint32_t lastSaveMs = 0;  // ElapsedMs() of the last write, 0 = none yet
    int slot = -1;
    unsigned saves = 0;
    std::string message;
};
void AutosaveInit();
void AutosaveTick();                 // between frames, on the game's thread
void AutosaveFlush(const char* why);  // the window is closing: save pending progress now
void AutosaveRequestNow();
void SetAutosaveEnabled(bool on);
bool AutosaveEnabled();
const AutosaveStatus& GetAutosaveStatus();
// texture census/dump (SPARTA_TEX_CENSUS=1 or SPARTA_TEX_DUMP_DIR): device = IDirect3DDevice8*
void InstallTextureHooks(void* device);
void LogTextureStats();
bool TexturePackActive();  // replacements (with mip chains) may be bound

// Paths seen by the game -> real paths (virtual CD). Returns true when translated.
bool TranslatePathA(const char* in, std::string& out);

// timing shared by the pacer and the test hooks
double NowSeconds();
uint32_t ElapsedMs();  // since the layer was loaded
uint64_t FramesPresented();
// A rectangle the game gives in its own render coordinates (its DirectShow video window) -> window client pixels.
void MovieRect(long* l, long* t, long* w, long* h);
int RenderScale();  // the game renders at this multiple of the window size (supersampling), else 1
uint32_t FirstFrameMs();  // ElapsedMs() of the first presented frame, 0 before it
double LastFrameSeconds();
uint64_t Ticks16Total();
void SetWideExtension(int units);

// ---- live settings (the F1 overlay changes these while the game runs; it also writes them to the ini) ----
struct HudSettings {
    bool on;
    float band, maxWidth, margin;
    int minScene;
};
HudSettings& HudConfig();
void SetFpsLimit(int fps);          // -1 monitor refresh, 0 unlimited, N
int FpsTarget();
void SetAnisotropy(int level);      // 0 game's own, 1 bilinear, 2..16
void RequestSideFill(bool ambient);   // applied between two frames
void SetRunInBackground(bool on);
struct DisplayInfo {
    int outW, outH, presW, presH, renderW, renderH, ss;
    bool horPlus, scene;
    double frameSeconds;
    uint64_t frames;
};
DisplayInfo GetDisplayInfo();
void* Direct3D8Object();  // IDirect3D8* (display mode lists)
unsigned Direct3DAdapter();
void SetTexturePackEnabled(bool on);
bool TexturePackEnabled();
void SetSoftEdges(bool on);
bool SoftEdgesEnabled();
void SetTextureDump(bool on);
bool TextureDumpEnabled();
struct TextureInfo {
    unsigned created, loaded, packSize;
    uint64_t unique, missing, dumped;
};
TextureInfo GetTextureInfo();

// ---- in-game overlay (overlay.cpp) ----
void OverlayRender(void* device, int width, int height);  // at Present, with the back buffer as render target
bool OverlayWantsInput();  // open: the game gets no keyboard or pad input
// The layer's own device work (offscreen scene, overlay) passes the game-facing hooks untouched: its textures must
// never be tracked as game textures (a released one's stale entry once let the upload hook over-read, O-121).
extern bool g_layerCalls;
struct LayerCallScope {
    bool prev;
    LayerCallScope() : prev(g_layerCalls) { g_layerCalls = true; }
    ~LayerCallScope() { g_layerCalls = prev; }
};
bool OverlayWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);  // true = handled
void OverlayDeviceReset(bool before);

}  // namespace compat
