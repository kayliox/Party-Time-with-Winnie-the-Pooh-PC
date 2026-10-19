// Input side of the layer: fills the game's pad settings from the DirectInput devices present at startup (what the
// old launcher did by hand), and injects scripted keyboard input for unattended test runs (SPARTA_INPUT_SCRIPT).
#define DIRECTINPUT_VERSION 0x0800
#include "compat.h"

#include <dinput.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace compat {

void RegStoreDword(const std::string& key, const std::string& name, DWORD v);

// ---------------------------------------------------------------- pad autofill

static BOOL CALLBACK EnumPad(LPCDIDEVICEINSTANCEA inst, LPVOID ctx) {
    auto* list = static_cast<std::vector<DIDEVICEINSTANCEA>*>(ctx);
    list->push_back(*inst);
    return list->size() < 8 ? DIENUM_CONTINUE : DIENUM_STOP;
}

static void StorePads(const std::vector<DIDEVICEINSTANCEA>& pads) {
    int n = int(pads.size());
    int players = g_cfg.keyboardPlayers + n;
    if (players > g_cfg.maxPlayers) players = g_cfg.maxPlayers;
    if (!g_cfg.padCountValue.empty()) RegStoreDword(g_cfg.padRegKey, g_cfg.padCountValue, DWORD(n));
    if (!g_cfg.playerCountValue.empty()) RegStoreDword(g_cfg.padRegKey, g_cfg.playerCountValue, DWORD(players));
    char name[96];
    if (!g_cfg.comboValueFormat.empty()) {
        // player p uses device p-1: keyboard layouts first, then pads (the launcher's own default order)
        for (int p = 1; p <= g_cfg.maxPlayers; p++) {
            snprintf(name, sizeof name, g_cfg.comboValueFormat.c_str(), p);
            RegStoreDword(g_cfg.padRegKey, name, p <= players ? DWORD(p - 1) : 0xFFFFFFFFu);
        }
    }
    for (int i = 0; i < n && !g_cfg.padGuidValueFormat.empty(); i++) {
        const GUID& g = pads[i].guidInstance;
        DWORD parts[5] = {g.Data1, DWORD(g.Data2), DWORD(g.Data3),
                          DWORD(g.Data4[0]) << 24 | DWORD(g.Data4[1]) << 16 | DWORD(g.Data4[2]) << 8 | g.Data4[3],
                          DWORD(g.Data4[4]) << 24 | DWORD(g.Data4[5]) << 16 | DWORD(g.Data4[6]) << 8 | g.Data4[7]};
        for (int k = 0; k < 5; k++) {
            snprintf(name, sizeof name, g_cfg.padGuidValueFormat.c_str(), i, k + 1);
            RegStoreDword(g_cfg.padRegKey, name, parts[k]);
        }
        Log("input: pad %d = %s", i, pads[i].tszInstanceName);
    }
    Log("input: %d pad(s), %d player device(s) configured", n, players);
}

// DirectInput's device enumeration can take a minute on some PCs (a slow HID device: 0.8 s, 5.5 s and 59.5 s on the
// first machine), so it runs on its own thread from startup and the game waits for it only briefly.
static HANDLE g_padsDone;

static DWORD WINAPI PadEnumThread(LPVOID) {
    double t0 = NowSeconds();
    auto create = reinterpret_cast<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>(
        GetProcAddress(LoadLibraryA("dinput8.dll"), "DirectInput8Create"));
    IDirectInput8A* di = nullptr;
    std::vector<DIDEVICEINSTANCEA> pads;
    if (create && SUCCEEDED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                   reinterpret_cast<void**>(&di), nullptr))) {
        di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumPad, &pads, DIEDFL_ATTACHEDONLY);
        di->Release();
    }
    Log("input: pad enumeration took %.1f s", NowSeconds() - t0);
    StorePads(pads);
    SetEvent(g_padsDone);
    return 0;
}

void StartPadEnumeration() {
    if (!g_cfg.autoPads || g_cfg.padRegKey.empty()) return;
    g_padsDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, PadEnumThread, nullptr, 0, nullptr));
}

void AutofillPads() {
    static bool waited;
    if (waited || !g_padsDone) return;
    waited = true;
    if (WaitForSingleObject(g_padsDone, 2000) != WAIT_OBJECT_0)
        Log("input: pad enumeration still running after 2 s; the game starts with the pad settings of the previous run");
}

// ---------------------------------------------------------------- scripted keyboard (tests)

struct KeyEvent {
    uint32_t atMs, holdMs;
    uint8_t dik;
    bool fromFirstFrame;  // 'f' prefix: time counts from the first presented frame (menus after movies/loading)
    uint32_t everyMs = 0; // 'm' prefix: pressed every everyMs until the first frame is presented (skips intro
                          // movies of any length: movies present no frames, so the timeline cannot be known)
};
static std::vector<KeyEvent> g_script;

static void ParseScript() {
    // "ms:DIK[:hold]" presses once; "from-to/every:DIK[:hold]" repeats (robust when loading times vary);
    // "mEVERY:DIK[:hold]" repeats only until the first presented frame (intro movies)
    const char* s = g_cfg.inputScript.c_str();
    while (*s) {
        unsigned at = 0, to = 0, every = 0, dik = 0, hold = 150;
        int used = 0;
        if (*s == 'm') {
            if (sscanf(s + 1, "%u:%x:%u%n", &every, &dik, &hold, &used) == 3 ||
                (hold = 150, sscanf(s + 1, "%u:%x%n", &every, &dik, &used) == 2)) {
                KeyEvent e{0, hold, uint8_t(dik), false};
                e.everyMs = every ? every : 1000;
                g_script.push_back(e);
                s += 1 + used;
                while (*s == ',' || *s == ' ') s++;
                continue;
            }
            Log("input: cannot parse script near '%s'", s);
            break;
        }
        bool ff = *s == 'f';
        if (ff) s++;
        if (sscanf(s, "%u-%u/%u:%x:%u%n", &at, &to, &every, &dik, &hold, &used) == 5 ||
            (hold = 150, sscanf(s, "%u-%u/%u:%x%n", &at, &to, &every, &dik, &used) == 4)) {
            for (unsigned t = at; every && t <= to; t += every) g_script.push_back({t, hold, uint8_t(dik), ff});
            s += used;
        } else if (hold = 150, sscanf(s, "%u:%x:%u%n", &at, &dik, &hold, &used) == 3 ||
                   (hold = 150, sscanf(s, "%u:%x%n", &at, &dik, &used) == 2)) {
            g_script.push_back({at, hold, uint8_t(dik), ff});
            s += used;
        } else {
            Log("input: cannot parse script near '%s'", s);
            break;
        }
        while (*s == ',' || *s == ' ') s++;
    }
    Log("input: %u scripted key press(es)", unsigned(g_script.size()));
}

static void ApplyScript(uint8_t* keys) {
    uint32_t now = ElapsedMs(), first = FirstFrameMs();
    for (auto& e : g_script) {
        if (e.everyMs) {
            if (!first && now % e.everyMs < e.holdMs) keys[e.dik] = 0x80;
            continue;
        }
        if (e.fromFirstFrame && !first) continue;
        uint32_t t = now - (e.fromFirstFrame ? first : 0);
        if (now >= (e.fromFirstFrame ? first : 0) && t >= e.atMs && t < e.atMs + e.holdMs) keys[e.dik] = 0x80;
    }
}

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
using SetCooperativeLevel_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, HWND, DWORD);
using Acquire_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*);

static DirectInput8Create_t o_DirectInput8Create;
static CreateDevice_t o_DiCreateDevice;
static GetDeviceState_t o_GetDeviceState;
static SetCooperativeLevel_t o_SetCooperativeLevel;
static Acquire_t o_Acquire;
static IDirectInputDevice8A* g_keyboard;

// Pads while the overlay is open: the state read when it opened, with every button released and hats centred, so
// the game neither moves nor reacts while the player uses the overlay (axes keep their resting position).
static std::vector<std::pair<IDirectInputDevice8A*, std::vector<BYTE>>> g_frozenPads;
// After the overlay closes, the key or button that closed it (Escape, F1, Back+Start) is still held: the game sees
// nothing from that device until everything on it is released, or it would act on it (Escape opens its own menu).
static std::vector<IDirectInputDevice8A*> g_heldDevices;

static bool IsPad(DWORD cb) { return cb == sizeof(DIJOYSTATE) || cb == sizeof(DIJOYSTATE2); }

static void ReleaseButtons(BYTE* b, DWORD cb) {
    auto* js = reinterpret_cast<DIJOYSTATE*>(b);
    for (DWORD& pov : js->rgdwPOV) pov = 0xFFFFFFFF;
    memset(js->rgbButtons, 0, cb == sizeof(DIJOYSTATE2) ? 128 : sizeof js->rgbButtons);
}

static bool AnyPressed(const BYTE* b, DWORD cb, bool keyboard) {
    if (keyboard) {
        for (DWORD i = 0; i < cb; i++)
            if (b[i] & 0x80) return true;
        return false;
    }
    if (!IsPad(cb)) return false;
    auto* js = reinterpret_cast<const DIJOYSTATE*>(b);
    for (DWORD i = 0; i < (cb == sizeof(DIJOYSTATE2) ? 128u : 32u); i++)
        if (js->rgbButtons[i] & 0x80) return true;
    for (DWORD pov : js->rgdwPOV)
        if (LOWORD(pov) != 0xFFFF) return true;
    return false;
}

static HRESULT STDMETHODCALLTYPE H_GetDeviceState(IDirectInputDevice8A* dev, DWORD cb, LPVOID data) {
    bool overlay = OverlayWantsInput();
    bool keyboard = dev == g_keyboard;
    if (keyboard && !g_script.empty()) {
        // scripted runs ignore the real keyboard: the user's typing elsewhere must not reach the game
        memset(data, 0, cb);
        if (!overlay && cb >= 256) ApplyScript(static_cast<uint8_t*>(data));
        return DI_OK;
    }
    auto* b = static_cast<BYTE*>(data);
    if (overlay) {
        if (std::find(g_heldDevices.begin(), g_heldDevices.end(), dev) == g_heldDevices.end()) g_heldDevices.push_back(dev);
        if (keyboard || !IsPad(cb)) {  // the overlay keeps every key to itself
            memset(data, 0, cb);
            return DI_OK;
        }
        for (auto& f : g_frozenPads)
            if (f.first == dev && f.second.size() == cb) {
                memcpy(data, f.second.data(), cb);
                return DI_OK;
            }
        HRESULT hr = o_GetDeviceState(dev, cb, data);
        if (FAILED(hr)) return hr;
        ReleaseButtons(b, cb);
        g_frozenPads.push_back({dev, std::vector<BYTE>(b, b + cb)});
        return DI_OK;
    }
    g_frozenPads.clear();
    HRESULT hr = o_GetDeviceState(dev, cb, data);
    auto held = std::find(g_heldDevices.begin(), g_heldDevices.end(), dev);
    if (FAILED(hr) || held == g_heldDevices.end()) return hr;
    if (!AnyPressed(b, cb, keyboard)) g_heldDevices.erase(held);
    else if (keyboard || !IsPad(cb)) memset(data, 0, cb);
    else ReleaseButtons(b, cb);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetCooperativeLevel(IDirectInputDevice8A* dev, HWND wnd, DWORD flags) {
    if (dev == g_keyboard && !g_script.empty()) flags = DISCL_BACKGROUND | DISCL_NONEXCLUSIVE;
    return o_SetCooperativeLevel(dev, wnd, flags);
}

static HRESULT STDMETHODCALLTYPE H_Acquire(IDirectInputDevice8A* dev) {
    HRESULT hr = o_Acquire(dev);
    return (dev == g_keyboard && !g_script.empty()) ? DI_OK : hr;
}

static HRESULT STDMETHODCALLTYPE H_DiCreateDevice(IDirectInput8A* di, REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) {
    HRESULT hr = o_DiCreateDevice(di, guid, out, outer);
    if (SUCCEEDED(hr)) {
        // every device (keyboard and pads share the vtable): the overlay must be able to hold the game's input
        void* p;
        if ((p = HookVtable(*out, 9, reinterpret_cast<void*>(H_GetDeviceState)))) o_GetDeviceState = reinterpret_cast<GetDeviceState_t>(p);
    }
    if (SUCCEEDED(hr) && IsEqualGUID(guid, GUID_SysKeyboard)) {
        g_keyboard = *out;
        Log("input: keyboard device %p", *out);
        if (!g_script.empty()) {
            void* p;
            if ((p = HookVtable(*out, 13, reinterpret_cast<void*>(H_SetCooperativeLevel))))
                o_SetCooperativeLevel = reinterpret_cast<SetCooperativeLevel_t>(p);
            if ((p = HookVtable(*out, 7, reinterpret_cast<void*>(H_Acquire)))) o_Acquire = reinterpret_cast<Acquire_t>(p);
        }
    } else if (SUCCEEDED(hr)) {
        Log("input: device {%08lx-...} created", guid.Data1);
    }
    return hr;
}

static HRESULT WINAPI H_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
    HRESULT hr = o_DirectInput8Create(inst, ver, iid, out, outer);
    if (SUCCEEDED(hr) && IsEqualIID(iid, IID_IDirectInput8A)) {
        void* p = HookVtable(*out, 3, reinterpret_cast<void*>(H_DiCreateDevice));
        if (p) o_DiCreateDevice = reinterpret_cast<CreateDevice_t>(p);
    }
    return hr;
}

void InstallInputHooks(HMODULE exe) {
    if (!g_cfg.inputScript.empty()) ParseScript();
    o_DirectInput8Create = reinterpret_cast<DirectInput8Create_t>(
        HookImport(exe, "dinput8.dll", "DirectInput8Create", reinterpret_cast<void*>(H_DirectInput8Create)));
    if (!o_DirectInput8Create) Log("input: game does not import DirectInput8Create; input hooks inactive");
}

}  // namespace compat
