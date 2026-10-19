// Entry point of the PC compatibility layer: logging, configuration, import/vtable hooking and verified byte patches.
#include "compat.h"

#include <objbase.h>
#include <dshow.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace compat {

Config g_cfg;

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;
static LARGE_INTEGER g_qpcFreq, g_qpcStart;

double NowSeconds() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart);
}

uint32_t ElapsedMs() { return uint32_t(NowSeconds() * 1000.0); }

void LogInit(const std::string& path) {
    InitializeCriticalSection(&g_logLock);
    // the previous session's log is kept as <name>.prev.log: a problem the player reports after restarting the game
    // (a lost save, a crash) is otherwise overwritten by the very launch that noticed it
    std::string prev = path;
    size_t dot = prev.rfind('.');
    prev = (dot == std::string::npos ? prev : prev.substr(0, dot)) + ".prev.log";
    MoveFileExA(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = fopen(path.c_str(), "w");
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "[%9.3f] ", NowSeconds());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

std::string IniString(const char* section, const char* key, const char* def) {
    char buf[1024];
    GetPrivateProfileStringA(section, key, def, buf, sizeof buf, g_cfg.iniPath.c_str());
    // strip trailing comments ("value ; comment") and blanks
    char* semi = strchr(buf, ';');
    if (semi) *semi = 0;
    std::string s = buf;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

int IniInt(const char* section, const char* key, int def) {
    std::string s = IniString(section, key, "");
    if (s.empty()) return def;
    return int(strtol(s.c_str(), nullptr, 0));
}

static std::string AbsPath(const std::string& p) {
    if (p.size() > 1 && (p[1] == ':' || (p[0] == '\\' && p[1] == '\\'))) return p;
    char full[MAX_PATH];
    std::string joined = g_cfg.exeDir + p;
    if (GetFullPathNameA(joined.c_str(), MAX_PATH, full, nullptr) == 0) return joined;
    return full;
}

static std::string Env(const char* name) {
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return (n > 0 && n < sizeof buf) ? std::string(buf, n) : std::string();
}

bool LoadConfig() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string e = exe;
    g_cfg.exeDir = e.substr(0, e.find_last_of("\\/") + 1);
    g_cfg.iniPath = g_cfg.exeDir + "spartacompat.ini";
    bool exists = GetFileAttributesA(g_cfg.iniPath.c_str()) != INVALID_FILE_ATTRIBUTES;

    g_cfg.vcdEnable = IniInt("VirtualCd", "Enable", 0) != 0;
    std::string root = IniString("VirtualCd", "Root", "cd");
    g_cfg.vcdRoot = AbsPath(root);
    while (!g_cfg.vcdRoot.empty() && (g_cfg.vcdRoot.back() == '\\' || g_cfg.vcdRoot.back() == '/')) g_cfg.vcdRoot.pop_back();
    std::string drive = IniString("VirtualCd", "Drive", "auto");
    if (drive.size() == 1 && isalpha((unsigned char)drive[0])) g_cfg.vcdDrive = char(toupper(drive[0]));

    for (int i = 1; i <= 8; i++) {
        char key[32];
        snprintf(key, sizeof key, "Virtualize%d", i);
        std::string p = IniString("Registry", i == 1 ? "Virtualize" : key, "");
        if (!p.empty()) g_cfg.regPrefixes.push_back(p);
    }
    g_cfg.regStore = AbsPath(IniString("Registry", "Store", "registry.ini"));

    g_cfg.mode = IniString("Display", "Mode", "borderless");
    g_cfg.width = IniInt("Display", "Width", 0);
    g_cfg.height = IniInt("Display", "Height", 0);
    g_cfg.refresh = IniInt("Display", "Refresh", 0);
    g_cfg.runInBackground = IniInt("Display", "RunInBackground", 0) != 0;
    g_cfg.overlay = IniInt("Overlay", "Enable", 1) != 0;
    g_cfg.aspect = IniString("Display", "Aspect", "4:3");
    g_cfg.fpsLimit = IniInt("Display", "FpsLimit", -1);
    g_cfg.msaa = IniInt("Display", "MSAA", 0);
    g_cfg.anisotropy = IniInt("Display", "Anisotropy", 0);
    g_cfg.depth24 = IniInt("Display", "Depth24", 1) != 0;
    g_cfg.supersample = IniInt("Display", "Supersample", 1) >= 2 ? 2 : 1;
    g_cfg.sideFill = IniString("Display", "SideFill", "black");
    g_cfg.hideCursor = IniInt("Display", "HideCursor", 1) != 0;

    g_cfg.renderWidthVar = uintptr_t(strtoul(IniString("Game", "RenderWidthVar", "0").c_str(), nullptr, 0));
    g_cfg.renderHeightVar = uintptr_t(strtoul(IniString("Game", "RenderHeightVar", "0").c_str(), nullptr, 0));

    g_cfg.autoPads = IniInt("Input", "AutoPads", 0) != 0;
    g_cfg.padRegKey = IniString("Input", "RegistryKey", "");
    g_cfg.padCountValue = IniString("Input", "PadCountValue", "");
    g_cfg.playerCountValue = IniString("Input", "PlayerCountValue", "");
    g_cfg.comboValueFormat = IniString("Input", "PlayerDeviceValueFormat", "");
    g_cfg.padGuidValueFormat = IniString("Input", "PadGuidValueFormat", "");
    g_cfg.keyboardPlayers = IniInt("Input", "KeyboardDevices", 2);
    g_cfg.maxPlayers = IniInt("Input", "MaxPlayers", 4);

    g_cfg.frameDumpDir = Env("SPARTA_FRAME_DUMP_DIR");
    std::string iv = Env("SPARTA_FRAME_DUMP_INTERVAL_MS");
    if (!iv.empty()) g_cfg.frameDumpIntervalMs = atoi(iv.c_str());
    g_cfg.windowHidden = Env("SPARTA_WINDOW_HIDDEN") == "1";
    g_cfg.mute = Env("SPARTA_MUTE") == "1";
    g_cfg.inputScript = Env("SPARTA_INPUT_SCRIPT");
    std::string ex = Env("SPARTA_EXIT_AFTER_MS");
    if (!ex.empty()) g_cfg.exitAfterMs = atoi(ex.c_str());
    std::string fl = Env("SPARTA_FPS_LIMIT");
    if (!fl.empty()) g_cfg.fpsLimit = atoi(fl.c_str());
    return exists;
}

void* HookImport(HMODULE module, const char* dll, const char* func, void* replacement) {
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;  // bound-only imports carry no names
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(ibn->Name), func) != 0) continue;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            void* orig = reinterpret_cast<void*>(addrs->u1.Function);
            addrs->u1.Function = reinterpret_cast<uintptr_t>(replacement);
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return nullptr;
}

void* HookVtable(void* object, int index, void* replacement) {
    void** vtbl = *reinterpret_cast<void***>(object);
    if (vtbl[index] == replacement) return nullptr;  // already hooked (shared vtable)
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old);
    void* orig = vtbl[index];
    vtbl[index] = replacement;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return orig;
}

// The D3D8 runtime keeps each device's vtable inside the device object and writes the original state-setting entries
// (SetRenderState, SetTexture, ...) back into it, e.g. around state block recording: a hook installed once at
// CreateDevice silently stops working. Device hooks are therefore registered and re-applied whenever they were lost.
struct DeviceHook {
    int index;
    void* replacement;
    void** original;
};
static std::vector<DeviceHook> g_deviceHooks;

void HookDevice(void* device, int index, void* replacement, void** original) {
    if (void* p = HookVtable(device, index, replacement)) *original = p;
    for (auto& h : g_deviceHooks)
        if (h.index == index) {
            h.replacement = replacement;
            h.original = original;
            return;
        }
    g_deviceHooks.push_back({index, replacement, original});
}

int RehookDevice(void* device) {
    void** vtbl = *reinterpret_cast<void***>(device);
    int n = 0;
    for (auto& h : g_deviceHooks) {
        if (vtbl[h.index] == h.replacement) continue;
        *h.original = vtbl[h.index];  // chain to whatever the runtime installed there now
        HookVtable(device, h.index, h.replacement);
        n++;
    }
    return n;
}

static bool ParseHex(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    int nib = -1;
    for (char c : s) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ' ' || c == '\t') continue;
        else return false;
        if (nib < 0) nib = v;
        else { out.push_back(uint8_t(nib << 4 | v)); nib = -1; }
    }
    return nib < 0 && !out.empty();
}

// ---- built-in variables that patches may reference as {name} (expands to the 4-byte little-endian address) ----

// Carried tick counts of the per-frame delta: ticksN = this frame's share of (delta >> N), the remainder kept for the
// next frame. Old engines truncate delta >> N per frame, which reaches 0 (things stop) once the frame rate is high.
static volatile int32_t g_ticks13, g_ticks16, g_ticks17;
static uint32_t g_acc13, g_acc16, g_acc17;
static volatile int32_t* g_deltaVar;
static volatile uint64_t g_ticks16Total;
// hor+: how far the picture now reaches beyond each side of the game's original screen, in the game's 2D units
// (patches widen the game's own culling by it; 0 = original behaviour)
static volatile int32_t g_wsExt;
// ...and the full width the picture covers in those units ([Game] ScreenUnitsX + 2 * extension), for code that builds
// its view volume from the screen width (frustum planes). Holds ScreenUnitsX until the window size is known.
static volatile int32_t g_wsUnits;

void SetWideExtension(int units) {
    g_wsExt = units;
    g_wsUnits = IniInt("Game", "ScreenUnitsX", 0) + 2 * units;
    Log("hor+: culling extended by %d units per side (view width %d units)", units, int(g_wsUnits));
}

static std::string ExpandBuiltinAddresses(const std::string& hex) {
    static const struct { const char* name; const volatile void* addr; } vars[] = {
        {"ticks13", &g_ticks13}, {"ticks16", &g_ticks16}, {"ticks17", &g_ticks17}, {"ws_ext", &g_wsExt},
        {"ws_units", &g_wsUnits}};
    std::string out;
    for (size_t i = 0; i < hex.size(); i++) {
        if (hex[i] != '{') { out += hex[i]; continue; }
        size_t e = hex.find('}', i);
        if (e == std::string::npos) return "?";
        std::string name = hex.substr(i + 1, e - i - 1);
        const volatile void* addr = nullptr;
        for (auto& v : vars) if (name == v.name) addr = v.addr;
        if (!addr) return "?";  // unknown variable: the caller reports a malformed patch
        uint32_t a = uint32_t(reinterpret_cast<uintptr_t>(addr));
        char buf[9];
        snprintf(buf, sizeof buf, "%02x%02x%02x%02x", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
        out += buf;
        i = e;
    }
    return out;
}

extern "C" void __cdecl UpdateDeltaTicks(int32_t v) {
    *g_deltaVar = v;  // the store the hook replaced
    uint32_t d = v > 0 ? uint32_t(v) : 0;
    g_acc13 += d; g_ticks13 = int32_t(g_acc13 >> 13); g_acc13 &= (1u << 13) - 1;
    g_acc16 += d; g_ticks16 = int32_t(g_acc16 >> 16); g_acc16 &= (1u << 16) - 1;
    g_acc17 += d; g_ticks17 = int32_t(g_acc17 >> 17); g_acc17 &= (1u << 17) - 1;
    g_ticks16Total += uint64_t(g_ticks16);
}

uint64_t Ticks16Total() { return g_ticks16Total; }

// Replaces a 5-byte "mov [delta], eax": every register and flag preserved, eax handed to UpdateDeltaTicks.
extern "C" __attribute__((naked)) void Builtin_StoreDeltaTicks() {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "pushl %eax\n\t"
        "call _UpdateDeltaTicks\n\t"
        "addl $4, %esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "ret\n\t");
}

bool ApplyPatches() {
    g_wsUnits = IniInt("Game", "ScreenUnitsX", 0);  // the original width until hor+ knows the window
    char keys[8192];
    DWORD n = GetPrivateProfileStringA("Patches", nullptr, "", keys, sizeof keys, g_cfg.iniPath.c_str());
    bool ok = true;
    for (char* k = keys; k < keys + n && *k; k += strlen(k) + 1) {
        std::string v = IniString("Patches", k, "");
        // va : original : replacement
        size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
        std::vector<uint8_t> orig, repl;
        std::string replHex = c2 == std::string::npos ? std::string() : ExpandBuiltinAddresses(v.substr(c2 + 1));
        if (c1 == std::string::npos || c2 == std::string::npos || !ParseHex(v.substr(c1 + 1, c2 - c1 - 1), orig) ||
            !ParseHex(replHex, repl) || orig.size() != repl.size()) {
            Log("patch %s: malformed '%s' (want va:orighex:newhex of equal length)", k, v.c_str());
            ok = false;
            continue;
        }
        auto addr = reinterpret_cast<uint8_t*>(strtoul(v.substr(0, c1).c_str(), nullptr, 0));
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize < addr + orig.size()) {
            Log("patch %s: address %p is not mapped, refused", k, addr);
            ok = false;
            continue;
        }
        if (memcmp(addr, orig.data(), orig.size()) != 0) {
            Log("patch %s: bytes at %p do not match the expected original, refused (wrong executable version?)", k, addr);
            ok = false;
            continue;
        }
        DWORD old;
        VirtualProtect(addr, repl.size(), PAGE_EXECUTE_READWRITE, &old);
        memcpy(addr, repl.data(), repl.size());
        VirtualProtect(addr, repl.size(), old, &old);
        FlushInstructionCache(GetCurrentProcess(), addr, repl.size());
        Log("patch %s: applied %u byte(s) at %p", k, unsigned(repl.size()), addr);
    }
    return ok;
}

// ---- built-in replacement functions for [Hooks] (cdecl, no arguments, result in eax) ----

static double g_frameDeltaScale = 0;
static double g_frameDeltaQuantum = 0;  // 0 = exact; else whole multiples of it, remainder carried to the next frame
static double g_deltaCarry;
static uint64_t g_deltaFrame = ~0ull;
static uint32_t g_deltaValue;

// Precise duration of the previous frame in the game's own fixed-point unit (scale = units per second). Replaces
// delta code that works from whole milliseconds (5 or 6 ms at 180 Hz = +-10 % jitter in movement).
// With a quantum (e.g. 0x10000 = one 1/120 s logic tick) every frame gets a whole number of ticks: engines that count
// "delta >> 16" ticks per frame then never see 0 at high frame rates (transitions, timers and cutscenes would stop),
// and at the matching frame rate every frame is exactly one tick (even motion). The value is computed once per
// presented frame, so several callers in one frame agree; the carry is capped so a long frame (loading, a dialog)
// is not paid back with a burst of extra ticks afterwards.
static volatile uint64_t g_deltaCalls, g_deltaReturned;  // statistics for the heartbeat
static uint32_t FrameDeltaValue();

extern "C" uint32_t __cdecl Builtin_FrameDelta() {
    uint32_t r = FrameDeltaValue();
    g_deltaCalls++;
    g_deltaReturned += r;
    return r;
}

static uint32_t FrameDeltaValue() {
    double s = LastFrameSeconds();
    if (s <= 0) s = 0.016;  // before the second frame: the game's own initial ring value (16 ms)
    if (g_frameDeltaQuantum <= 0) return uint32_t(s * g_frameDeltaScale + 0.5);
    uint64_t f = FramesPresented();
    if (f != g_deltaFrame) {
        g_deltaFrame = f;
        g_deltaCarry += s * g_frameDeltaScale;
        // nearest whole tick with a signed carry: frame-time jitter around one tick never yields 0- or 2-tick frames
        double ticks = std::floor(g_deltaCarry / g_frameDeltaQuantum + 0.5);
        if (ticks < 0) ticks = 0;
        if (ticks > 6) ticks = 6;  // at most 6 ticks per frame (the game clamps its delta to 0x60000 itself)
        g_deltaCarry -= ticks * g_frameDeltaQuantum;
        if (g_deltaCarry > g_frameDeltaQuantum) g_deltaCarry = g_frameDeltaQuantum;  // no catch-up bursts
        g_deltaValue = uint32_t(ticks * g_frameDeltaQuantum);
    }
    return g_deltaValue;
}

// The game's own printf-style debug output (an empty stub in release builds: "push ebp; mov ebp,esp; pop ebp; ret")
// into spartacompat.log, prefixed "game:". Repeats are counted instead of logged and the total is capped, so a message
// printed every frame cannot flood the log. Diagnostics only: the arguments are formatted as the game's format says.
static CRITICAL_SECTION g_debugPrintLock;
static int g_debugPrintBudget = 3000;

extern "C" void __cdecl Builtin_DebugPrint(const char* fmt, ...) {
    if (!fmt || g_debugPrintBudget <= 0) return;
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    static char last[600];
    static unsigned repeats;
    EnterCriticalSection(&g_debugPrintLock);
    if (strcmp(buf, last) == 0) {
        repeats++;
    } else {
        if (repeats) Log("game: (repeated %u more times)", repeats);
        repeats = 0;
        memcpy(last, buf, n + 1);
        if (--g_debugPrintBudget == 0) Log("game: debug output budget used up; further messages are dropped");
        else Log("game: %s", buf);
    }
    LeaveCriticalSection(&g_debugPrintLock);
}

bool ApplyHooks() {
    char keys[4096];
    DWORD n = GetPrivateProfileStringA("Hooks", nullptr, "", keys, sizeof keys, g_cfg.iniPath.c_str());
    bool ok = true;
    for (char* k = keys; k < keys + n && *k; k += strlen(k) + 1) {
        std::string v = IniString("Hooks", k, "");
        size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
        std::vector<uint8_t> orig;
        if (c1 == std::string::npos || c2 == std::string::npos || !ParseHex(v.substr(c1 + 1, c2 - c1 - 1), orig) || orig.size() < 5) {
            Log("hook %s: malformed '%s' (want va:orighex(>=5 bytes):builtin[:arg])", k, v.c_str());
            ok = false;
            continue;
        }
        std::string rest = v.substr(c2 + 1), builtin = rest, arg;
        size_t c3 = rest.find(':');
        if (c3 != std::string::npos) {
            builtin = rest.substr(0, c3);
            arg = rest.substr(c3 + 1);
        }
        void* target = nullptr;
        uint8_t opcode = 0xE9;  // jmp: the built-in replaces the whole function
        if (builtin == "frame_delta") {
            // arg = scale (units per second)[:quantum]
            size_t c4 = arg.find(':');
            g_frameDeltaScale = double(strtoul(arg.substr(0, c4).c_str(), nullptr, 0));
            if (c4 != std::string::npos) g_frameDeltaQuantum = double(strtoul(arg.substr(c4 + 1).c_str(), nullptr, 0));
            if (g_frameDeltaScale > 0) target = reinterpret_cast<void*>(Builtin_FrameDelta);
        } else if (builtin == "store_delta_ticks") {
            // call: the built-in replaces one 5-byte store instruction and returns to the next one
            g_deltaVar = reinterpret_cast<volatile int32_t*>(strtoul(arg.c_str(), nullptr, 0));
            if (g_deltaVar && orig.size() == 5) {
                target = reinterpret_cast<void*>(Builtin_StoreDeltaTicks);
                opcode = 0xE8;
            }
        } else if (builtin == "debug_print") {
            InitializeCriticalSection(&g_debugPrintLock);
            target = reinterpret_cast<void*>(Builtin_DebugPrint);
        }
        if (!target) {
            Log("hook %s: unknown built-in or bad argument '%s'", k, rest.c_str());
            ok = false;
            continue;
        }
        auto addr = reinterpret_cast<uint8_t*>(strtoul(v.substr(0, c1).c_str(), nullptr, 0));
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || memcmp(addr, orig.data(), orig.size()) != 0) {
            Log("hook %s: bytes at %p do not match the expected original, refused (wrong executable version?)", k, addr);
            ok = false;
            continue;
        }
        uint8_t jmp[5] = {opcode};
        int32_t rel = int32_t(reinterpret_cast<uint8_t*>(target) - (addr + 5));
        memcpy(jmp + 1, &rel, 4);
        DWORD old;
        VirtualProtect(addr, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy(addr, jmp, 5);
        VirtualProtect(addr, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), addr, 5);
        Log("hook %s: %p -> built-in %s %s", k, addr, builtin.c_str(), arg.c_str());
    }
    return ok;
}

// "module+0xoffset" for a code address, so a crash log says where it happened without a debugger
static void DescribeAddress(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &m) &&
        m && GetModuleFileNameA(m, path, MAX_PATH)) {
        static HMODULE self;
        if (!self)
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&DescribeAddress), &self);
        const char* name = strrchr(path, '\\');
        // the layer is a d3d8.dll too: name it apart from the system's Direct3D 8 runtime
        snprintf(out, n, "%s+0x%lx", m == self ? "pc-compat" : (name ? name + 1 : path),
                 static_cast<unsigned long>(a - reinterpret_cast<uintptr_t>(m)));
    } else {
        snprintf(out, n, "%08lx", static_cast<unsigned long>(a));
    }
}

static bool IsImageCode(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (a < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE)
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool g_layerCalls;

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    static volatile LONG count = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW) {
        if (InterlockedIncrement(&count) <= 20) {
            auto* c = ep->ContextRecord;
            Log("exception 0x%08lx at %p (first chance) eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx info=%p",
                code, ep->ExceptionRecord->ExceptionAddress, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp,
                ep->ExceptionRecord->NumberParameters > 1 ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]) : nullptr);
            // the faulting module and the code addresses on the stack (likely callers; a scan, so some can be stale)
            char where[160], item[160];
            DescribeAddress(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress), where, sizeof where);
            std::string callers;
            auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
            uintptr_t sp = c->Esp, top = reinterpret_cast<uintptr_t>(tib->StackBase);
            int found = 0;
            for (uintptr_t a = sp; a >= reinterpret_cast<uintptr_t>(tib->StackLimit) && a + 4 <= top && a < sp + 2048 && found < 12; a += 4) {
                uintptr_t v = *reinterpret_cast<const uintptr_t*>(a);
                if (!IsImageCode(v)) continue;
                DescribeAddress(v, item, sizeof item);
                callers += ' ';
                callers += item;
                found++;
            }
            Log("  in %s; code addresses on the stack:%s", where, callers.empty() ? " none" : callers.c_str());
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Diagnostics that never change game state: where the main thread is when no frame arrives, and COM objects created.
static DWORD g_mainThreadId;

void LogWatches() {
    char keys[4096];
    DWORD n = GetPrivateProfileStringA("Watch", nullptr, "", keys, sizeof keys, g_cfg.iniPath.c_str());
    std::string line;
    for (char* k = keys; k < keys + n && *k; k += strlen(k) + 1) {
        std::string v = IniString("Watch", k, "");
        size_t c = v.find(':');
        auto addr = reinterpret_cast<const void*>(strtoul(v.substr(0, c).c_str(), nullptr, 0));
        std::string type = c == std::string::npos ? "u32" : v.substr(c + 1);
        uint8_t buf[4] = {};
        SIZE_T got = 0;
        char item[128];
        if (!ReadProcessMemory(GetCurrentProcess(), addr, buf, 4, &got) || got != 4) {
            snprintf(item, sizeof item, " %s=?", k);
        } else {
            uint32_t u;
            memcpy(&u, buf, 4);
            if (type == "u8") snprintf(item, sizeof item, " %s=%u", k, unsigned(buf[0]));
            else if (type == "u16") snprintf(item, sizeof item, " %s=%u", k, unsigned(u & 0xffff));
            else if (type == "i32") snprintf(item, sizeof item, " %s=%d", k, int32_t(u));
            else if (type == "f32") { float f; memcpy(&f, buf, 4); snprintf(item, sizeof item, " %s=%g", k, double(f)); }
            else snprintf(item, sizeof item, " %s=%u", k, u);
        }
        line += item;
    }
    if (!line.empty()) Log("[watch]%s", line.c_str());
}

static DWORD WINAPI Heartbeat(LPVOID) {
    HANDLE main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainThreadId);
    uint64_t lastFrames = 0;
    for (;;) {
        Sleep(10000);
        LogWatches();
        LogTextureStats();
        uint64_t f = FramesPresented();
        if (f != lastFrames) {
            static uint64_t lastTicks;
            uint64_t ticks = Ticks16Total();
            static uint64_t lastCalls, lastReturned;
            uint64_t calls = g_deltaCalls, returned = g_deltaReturned;
            Log("[state] %llu frames in the last 10 s (%.1f fps), %llu presented, %llu delta ticks (1/120 s); frame_delta: %llu calls, %.1f ticks returned",
                (unsigned long long)(f - lastFrames), (f - lastFrames) / 10.0, (unsigned long long)f,
                (unsigned long long)(ticks - lastTicks), (unsigned long long)(calls - lastCalls),
                double(returned - lastReturned) / 65536.0);
            lastTicks = ticks;
            lastCalls = calls;
            lastReturned = returned;
            lastFrames = f;
            continue;
        }
        if (!main || SuspendThread(main) == DWORD(-1)) continue;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        BOOL ok = GetThreadContext(main, &c);
        // walk a few EBP frames for probable return addresses (reads only)
        DWORD ret[6] = {};
        DWORD ebp = c.Ebp;
        for (int i = 0; ok && i < 6 && ebp; i++) {
            DWORD frame[2];
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ebp), frame, sizeof frame, &got) || got != sizeof frame) break;
            ret[i] = frame[1];
            if (frame[0] <= ebp) break;
            ebp = frame[0];
        }
        ResumeThread(main);
        if (ok)
            Log("[state] no new frame for 10 s (%llu presented); main thread eip=%08lx esp=%08lx returns %08lx %08lx %08lx %08lx %08lx %08lx",
                (unsigned long long)f, c.Eip, c.Esp, ret[0], ret[1], ret[2], ret[3], ret[4], ret[5]);
    }
    return 0;
}

using CoCreateInstance_t = HRESULT(WINAPI*)(REFCLSID, IUnknown*, DWORD, REFIID, LPVOID*);
static CoCreateInstance_t o_CoCreateInstance;

// DirectShow movies: games place the video window from their own (4:3) render size at the window's left edge; the
// layer moves it by the same offset as the game picture (hor+ side width or pillarbox), so movies play centred.
static const GUID kClsidFilterGraph = {0xe436ebb3, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID kIidVideoWindow = {0x56a868b4, 0x0ad4, 0x11ce, {0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
using GraphQI_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
using SetWindowPosition_t = HRESULT(STDMETHODCALLTYPE*)(void*, long, long, long, long);
static GraphQI_t o_GraphQI;
static SetWindowPosition_t o_SetWindowPosition;

// Which renderer plays a movie and, when the game hides the video window at its end, how many frames it drew and
// dropped: evidence for "choppy movies" without watching the screen (reads only).
static const GUID kIidFilterGraph = {0x56a8689f, 0x0ad4, 0x11ce, {0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID kIidQualProp = {0x1bd0ecb0, 0xf8e2, 0x11ce, {0xaa, 0xc6, 0x00, 0x20, 0xaf, 0x0b, 0x99, 0xa3}};
static IQualProp* g_movieQual;
static uint32_t g_movieStartMs;

static void MovieRendererStart(void* videoWindow) {
    if (g_movieQual) {
        g_movieQual->Release();
        g_movieQual = nullptr;
    }
    IFilterGraph* graph = nullptr;
    if (FAILED(static_cast<IUnknown*>(videoWindow)->QueryInterface(kIidFilterGraph, reinterpret_cast<void**>(&graph)))) return;
    IEnumFilters* en = nullptr;
    std::string names;
    if (SUCCEEDED(graph->EnumFilters(&en))) {
        IBaseFilter* f = nullptr;
        while (en->Next(1, &f, nullptr) == S_OK) {
            FILTER_INFO fi{};
            char name[128] = "?";
            if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
                WideCharToMultiByte(CP_UTF8, 0, fi.achName, -1, name, sizeof name, nullptr, nullptr);
                if (fi.pGraph) fi.pGraph->Release();
            }
            IQualProp* q = nullptr;
            bool renderer = SUCCEEDED(f->QueryInterface(kIidQualProp, reinterpret_cast<void**>(&q)));
            names += names.empty() ? "" : ", ";
            names += name;
            if (renderer) names += " (video renderer)";
            if (renderer && !g_movieQual) g_movieQual = q;
            else if (q) q->Release();
            f->Release();
        }
        en->Release();
    }
    graph->Release();
    Log("movie: filters: %s", names.c_str());
    g_movieStartMs = ElapsedMs();
}

static void MovieRendererEnd() {
    if (!g_movieQual) return;
    int drawn = 0, dropped = 0, avg = 0, jitter = 0;
    g_movieQual->get_FramesDrawn(&drawn);
    g_movieQual->get_FramesDroppedInRenderer(&dropped);
    g_movieQual->get_AvgFrameRate(&avg);
    g_movieQual->get_Jitter(&jitter);
    Log("movie: ended after %.1f s: renderer drew %d frame(s), dropped %d, average %.2f fps, jitter %d ms",
        (ElapsedMs() - g_movieStartMs) / 1000.0, drawn, dropped, avg / 100.0, jitter);
    g_movieQual->Release();
    g_movieQual = nullptr;
}

using PutVisible_t = HRESULT(STDMETHODCALLTYPE*)(void*, long);
static PutVisible_t o_PutVisible;

static HRESULT STDMETHODCALLTYPE H_PutVisible(void* self, long visible) {
    if (!visible) MovieRendererEnd();  // the game hides the video window when a movie ends or is skipped
    return o_PutVisible(self, visible);
}

static HRESULT STDMETHODCALLTYPE H_SetWindowPosition(void* self, long l, long t, long w, long h) {
    // the game places its video window in its own render coordinates (doubled by supersampling, offset by hor+,
    // scaled when the back buffer is stretched to the screen): back to window client pixels
    long nl = l, nt = t, nw = w, nh = h;
    MovieRect(&nl, &nt, &nw, &nh);
    Log("movie: video window %ld,%ld %ldx%ld -> %ld,%ld %ldx%ld", l, t, w, h, nl, nt, nw, nh);
    MovieRendererStart(self);
    return o_SetWindowPosition(self, nl, nt, nw, nh);
}

static HRESULT STDMETHODCALLTYPE H_GraphQI(IUnknown* self, REFIID iid, void** out) {
    HRESULT hr = o_GraphQI(self, iid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(iid, kIidVideoWindow)) {
        void* p = HookVtable(*out, 39, reinterpret_cast<void*>(H_SetWindowPosition));  // IVideoWindow::SetWindowPosition
        if (p) o_SetWindowPosition = reinterpret_cast<SetWindowPosition_t>(p);
        if ((p = HookVtable(*out, 19, reinterpret_cast<void*>(H_PutVisible)))) o_PutVisible = reinterpret_cast<PutVisible_t>(p);
    }
    return hr;
}

static HRESULT WINAPI H_CoCreateInstance(REFCLSID clsid, IUnknown* outer, DWORD ctx, REFIID iid, LPVOID* out) {
    HRESULT hr = o_CoCreateInstance(clsid, outer, ctx, iid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(clsid, kClsidFilterGraph)) {
        void* p = HookVtable(*out, 0, reinterpret_cast<void*>(H_GraphQI));  // QueryInterface of the graph
        if (p) o_GraphQI = reinterpret_cast<GraphQI_t>(p);
    }
    Log("com: CoCreateInstance {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} -> 0x%08lx", clsid.Data1, clsid.Data2,
        clsid.Data3, clsid.Data4[0], clsid.Data4[1], clsid.Data4[2], clsid.Data4[3], clsid.Data4[4], clsid.Data4[5],
        clsid.Data4[6], clsid.Data4[7], hr);
    return hr;
}

static DWORD WINAPI ExitTimer(LPVOID) {
    Sleep(DWORD(g_cfg.exitAfterMs));
    Log("SPARTA_EXIT_AFTER_MS reached (%d ms): terminating the game", g_cfg.exitAfterMs);
    UnmuteTestAudio();  // Windows would otherwise remember the test mute for this exe (O-122)
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void Startup() {
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    bool haveIni = LoadConfig();
    LogInit(g_cfg.exeDir + "spartacompat.log");
    Log("SpartaRecomp PC compatibility layer loaded into %s", g_cfg.exeDir.c_str());
    if (!haveIni) Log("no spartacompat.ini found: running with defaults (no virtual CD, no patches)");
    AddVectoredExceptionHandler(0, CrashLogger);
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!ApplyPatches()) Log("WARNING: some patches were refused; see above");
    if (!ApplyHooks()) Log("WARNING: some hooks were refused; see above");
    AutosaveInit();
    InstallVcdHooks(exe);
    InstallRegistryHooks(exe);
    InstallWindowHooks(exe);
    InstallInputHooks(exe);
    StartPadEnumeration();
    o_CoCreateInstance = reinterpret_cast<CoCreateInstance_t>(
        HookImport(exe, "ole32.dll", "CoCreateInstance", reinterpret_cast<void*>(H_CoCreateInstance)));
    g_mainThreadId = GetCurrentThreadId();
    CloseHandle(CreateThread(nullptr, 0, Heartbeat, nullptr, 0, nullptr));
    if (g_cfg.exitAfterMs > 0) CloseHandle(CreateThread(nullptr, 0, ExitTimer, nullptr, 0, nullptr));
}

}  // namespace compat

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        compat::Startup();
    }
    return TRUE;
}
