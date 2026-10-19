// In-game settings overlay: F1 (or Back+Start on a gamepad) opens Dear ImGui over the game's finished frame. Most
// settings apply at once (frame rate, filtering, supersampling, ultrawide fill and HUD, HD textures); window mode,
// resolution, refresh rate, aspect and MSAA need a restart (a button restarts the game). Every change is written to
// spartacompat.ini immediately, so it stays. While open, the game receives no keyboard or pad input.
#include "compat.h"

#include <d3d8.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx8.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace compat {

namespace {

struct RestartSettings {
    std::string mode, aspect;
    int width, height, refresh, msaa, supersample;
    bool operator!=(const RestartSettings& o) const {
        return mode != o.mode || aspect != o.aspect || width != o.width || height != o.height || refresh != o.refresh ||
               msaa != o.msaa || supersample != o.supersample;
    }
};

struct Mode {
    unsigned w, h;
    std::vector<unsigned> rates;
};

bool g_ready, g_failed, g_open, g_fpsCounter, g_comboDown;
HWND g_wnd;
int g_tab = -1;               // tab to select on the next frame (tests)
RestartSettings g_start, g_now;
std::vector<Mode> g_modes;
double g_fps;
bool g_f1Down;
bool g_popupWasOpen;  // Escape that closed a dropdown must not close the panel too

// SPARTA_OVERLAY_KEYS (tests): "ms:Key,..." with Up Down Left Right Space Enter Escape (fed to the panel), F1 (toggles
// it) and Tab0-Tab4 (selects a tab), so unattended runs change settings through the same UI the player uses. An "f"
// before the time counts from the first presented frame (the movies before it vary in length), as in the input script.
// SPARTA_OVERLAY_OPEN_MS opens the panel once at such a time, SPARTA_OVERLAY_TAB picks its first tab.
struct TestKey {
    uint32_t ms;
    bool fromFirstFrame;
    ImGuiKey key;  // ImGuiKey_None: F1 or a tab
    int tab;       // >= 0: select this tab
};
std::vector<TestKey> g_testKeys;
size_t g_testNext;
ImGuiKey g_testDown = ImGuiKey_None;
struct {
    uint32_t ms;
    bool fromFirstFrame, pending;
} g_forceOpen;
bool g_envRead;

uint32_t ParseTime(const char* s, bool* fromFirstFrame) {
    *fromFirstFrame = *s == 'f';
    return uint32_t(strtoul(s + (*fromFirstFrame ? 1 : 0), nullptr, 10));
}

bool Due(uint32_t ms, bool fromFirstFrame) {
    uint32_t first = FirstFrameMs();
    if (fromFirstFrame && !first) return false;
    return ElapsedMs() >= (fromFirstFrame ? first : 0) + ms;
}

using XInputGetState_t = DWORD(WINAPI*)(DWORD, void*);
XInputGetState_t g_xinputGetState;

void IniWrite(const char* section, const char* key, const std::string& value) {
    WritePrivateProfileStringA(section, key, value.c_str(), g_cfg.iniPath.c_str());
    Log("overlay: [%s] %s=%s", section, key, value.c_str());
}
void IniWriteInt(const char* section, const char* key, int value) { IniWrite(section, key, std::to_string(value)); }

void LoadModes() {
    auto* d3d = static_cast<IDirect3D8*>(Direct3D8Object());
    if (!d3d) return;
    D3DDISPLAYMODE desk{};
    d3d->GetAdapterDisplayMode(Direct3DAdapter(), &desk);
    UINT n = d3d->GetAdapterModeCount(Direct3DAdapter());
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m{};
        if (FAILED(d3d->EnumAdapterModes(Direct3DAdapter(), i, &m)) || m.Format != desk.Format || m.Width < 640) continue;
        auto it = std::find_if(g_modes.begin(), g_modes.end(), [&](const Mode& x) { return x.w == m.Width && x.h == m.Height; });
        if (it == g_modes.end()) {
            g_modes.push_back({m.Width, m.Height, {}});
            it = g_modes.end() - 1;
        }
        if (m.RefreshRate && std::find(it->rates.begin(), it->rates.end(), m.RefreshRate) == it->rates.end())
            it->rates.push_back(m.RefreshRate);
    }
    for (auto& m : g_modes) std::sort(m.rates.rbegin(), m.rates.rend());
    std::sort(g_modes.begin(), g_modes.end(), [](const Mode& a, const Mode& b) { return a.w * a.h > b.w * b.h; });
}

void ApplyStyle(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 10.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 6.0f;
    s.WindowPadding = ImVec2(18, 16);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(12, 10);
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.96f, 0.68f, 0.18f, 1.0f);  // honey
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 0.97f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.11f, 0.98f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.10f, 0.07f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.14f, 0.11f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.46f, 0.32f, 0.08f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.62f, 0.44f, 0.12f, 1.0f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.80f, 0.35f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.30f, 0.22f, 0.08f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.40f, 0.12f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.70f, 0.50f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.16f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.24f, 0.18f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.36f, 0.27f, 0.10f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.52f, 0.38f, 0.12f, 1.0f);
    s.ScaleAllSizes(scale);
}

bool Init(IDirect3DDevice8* dev, int height) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    dev->GetCreationParameters(&cp);
    g_wnd = cp.hFocusWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written next to the game but spartacompat.ini
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    float size = std::max(16.0f, std::round(float(height) / 60.0f));  // 24 px at 1440 lines
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string font = std::string(win) + "\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font.c_str(), size);
    else io.Fonts->AddFontDefault();
    ImGui::GetStyle().FontSizeBase = size;
    ApplyStyle(size / 16.0f);
    if (!ImGui_ImplWin32_Init(g_wnd) || !ImGui_ImplDX8_Init(dev)) {
        Log("overlay: initialisation failed; F1 overlay disabled");
        g_failed = true;
        return false;
    }
    g_start = {g_cfg.mode, g_cfg.aspect, g_cfg.width, g_cfg.height, g_cfg.refresh, g_cfg.msaa, g_cfg.supersample >= 2 ? 2 : 1};
    g_now = g_start;
    LoadModes();
    g_ready = true;
    Log("overlay: ready (F1 or Back+Start; %u display mode(s), font %.0f px)", unsigned(g_modes.size()), size);
    return true;
}

bool PadComboPressed() {
    if (!g_xinputGetState) {
        static bool tried;
        if (tried) return false;
        tried = true;
        for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryA(dll)) {
                g_xinputGetState = reinterpret_cast<XInputGetState_t>(GetProcAddress(m, "XInputGetState"));
                if (g_xinputGetState) break;
            }
        if (!g_xinputGetState) return false;
    }
    struct {
        DWORD packet;
        WORD buttons;
        BYTE lt, rt;
        SHORT lx, ly, rx, ry;
    } st{};
    for (DWORD i = 0; i < 4; i++)
        if (g_xinputGetState(i, &st) == 0 && (st.buttons & 0x0030) == 0x0030) return true;  // START | BACK
    return false;
}

// F1 or Back+Start toggles; test runs never read the physical keyboard but can open it at a set time
void PollToggle() {
    if (!g_envRead) {
        g_envRead = true;
        char b[32];
        DWORD n = GetEnvironmentVariableA("SPARTA_OVERLAY_OPEN_MS", b, sizeof b);
        if (n && n < sizeof b) {
            g_forceOpen.ms = ParseTime(b, &g_forceOpen.fromFirstFrame);
            g_forceOpen.pending = true;
        }
        n = GetEnvironmentVariableA("SPARTA_OVERLAY_TAB", b, sizeof b);
        if (n && n < sizeof b) g_tab = atoi(b);
        char keys[2048];
        n = GetEnvironmentVariableA("SPARTA_OVERLAY_KEYS", keys, sizeof keys);
        if (n && n < sizeof keys) {
            static const struct {
                const char* name;
                ImGuiKey key;
            } names[] = {{"Up", ImGuiKey_UpArrow}, {"Down", ImGuiKey_DownArrow}, {"Left", ImGuiKey_LeftArrow},
                         {"Right", ImGuiKey_RightArrow}, {"Space", ImGuiKey_Space}, {"Enter", ImGuiKey_Enter},
                         {"Escape", ImGuiKey_Escape}, {"F1", ImGuiKey_None}};
            for (char* tok = strtok(keys, ","); tok; tok = strtok(nullptr, ",")) {
                char* colon = strchr(tok, ':');
                if (!colon) continue;
                bool ff;
                uint32_t ms = ParseTime(tok, &ff);
                const char* name = colon + 1;
                size_t before = g_testKeys.size();
                if (strncmp(name, "Tab", 3) == 0 && name[3] >= '0' && name[3] <= '4' && !name[4])
                    g_testKeys.push_back({ms, ff, ImGuiKey_None, name[3] - '0'});
                for (const auto& k : names)
                    if (strcmp(name, k.name) == 0) g_testKeys.push_back({ms, ff, k.key, -1});
                if (g_testKeys.size() == before) Log("overlay: unknown scripted key '%s' ignored", name);
            }
            Log("overlay: %u scripted overlay key(s)", unsigned(g_testKeys.size()));
        }
        g_fpsCounter = IniInt("Overlay", "FpsCounter", 0) != 0;
    }
    if (g_forceOpen.pending && Due(g_forceOpen.ms, g_forceOpen.fromFirstFrame)) {
        g_open = true;
        g_forceOpen.pending = false;
    }
    // F1 and tab entries act here (the panel may be closed); panel keys are fed in OverlayRender, in script order
    if (g_testNext < g_testKeys.size() && g_testKeys[g_testNext].key == ImGuiKey_None &&
        Due(g_testKeys[g_testNext].ms, g_testKeys[g_testNext].fromFirstFrame)) {
        const TestKey& k = g_testKeys[g_testNext++];
        if (k.tab >= 0) g_tab = k.tab;
        else g_open = !g_open;
    }
    if (g_cfg.windowHidden || !g_cfg.inputScript.empty()) return;
    bool fg = g_wnd ? GetForegroundWindow() == g_wnd : true;
    bool f1 = fg && (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (f1 && !g_f1Down) g_open = !g_open;
    g_f1Down = f1;
    bool combo = fg && PadComboPressed();
    if (combo && !g_comboDown) g_open = !g_open;
    g_comboDown = combo;
}

void RestartGame() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cmd = std::string("cmd.exe /c ping -n 3 127.0.0.1 >nul & start \"\" \"") + exe + "\"";
    STARTUPINFOA si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_cfg.exeDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("overlay: restarting the game to apply display settings");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    Log("overlay: restart failed (%lu)", GetLastError());
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// one entry of a dropdown; the current value gets the keyboard/pad focus when the list opens (else Up from the first
// entry wraps to the last one)
bool Choice(const char* label, bool selected) {
    bool picked = ImGui::Selectable(label, selected);
    if (selected) ImGui::SetItemDefaultFocus();
    return picked;
}

bool TabFor(const char* name, int index) {
    ImGuiTabItemFlags f = g_tab == index ? ImGuiTabItemFlags_SetSelected : 0;
    return ImGui::BeginTabItem(name, nullptr, f);
}

void DisplayTab() {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int monW = mi.rcMonitor.right - mi.rcMonitor.left, monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    ImGui::SeparatorText("Window (applies after a restart)");
    static const char* modes[] = {"Borderless fullscreen", "Exclusive fullscreen", "Windowed"};
    static const char* modeKeys[] = {"borderless", "fullscreen", "windowed"};
    int m = g_now.mode == "fullscreen" ? 1 : g_now.mode == "windowed" ? 2 : 0;
    if (ImGui::Combo("Window mode", &m, modes, 3)) {
        g_now.mode = modeKeys[m];
        IniWrite("Display", "Mode", g_now.mode);
    }
    char cur[64];
    if (g_now.width > 0) snprintf(cur, sizeof cur, "%d x %d", g_now.width, g_now.height);
    else snprintf(cur, sizeof cur, "Native (%d x %d)", monW, monH);
    if (ImGui::BeginCombo("Resolution", cur)) {
        char lbl[64];
        snprintf(lbl, sizeof lbl, "Native (%d x %d)", monW, monH);
        int pick = -2;  // -1 = native, else an index into g_modes
        if (Choice(lbl, g_now.width == 0)) pick = -1;
        for (size_t i = 0; i < g_modes.size(); i++) {
            snprintf(lbl, sizeof lbl, "%u x %u", g_modes[i].w, g_modes[i].h);
            if (Choice(lbl, g_now.width == int(g_modes[i].w) && g_now.height == int(g_modes[i].h))) pick = int(i);
        }
        ImGui::EndCombo();
        if (pick != -2) {
            g_now.width = pick < 0 ? 0 : int(g_modes[size_t(pick)].w);
            g_now.height = pick < 0 ? 0 : int(g_modes[size_t(pick)].h);
            IniWriteInt("Display", "Width", g_now.width);
            IniWriteInt("Display", "Height", g_now.height);
        }
    }
    HelpMarker("Borderless always covers the screen: a lower resolution is scaled up to it (faster, softer). "
               "Exclusive fullscreen switches the display to this resolution.");
    if (g_now.mode == "fullscreen") {
        unsigned w = g_now.width > 0 ? unsigned(g_now.width) : unsigned(monW), h = g_now.height > 0 ? unsigned(g_now.height) : unsigned(monH);
        const Mode* md = nullptr;
        for (const Mode& x : g_modes)
            if (x.w == w && x.h == h) md = &x;
        char rl[32];
        if (g_now.refresh > 0) snprintf(rl, sizeof rl, "%d Hz", g_now.refresh);
        else snprintf(rl, sizeof rl, "Default");
        if (ImGui::BeginCombo("Refresh rate", rl)) {
            int pick = -1;
            if (Choice("Default", g_now.refresh == 0)) pick = 0;
            if (md)
                for (unsigned r : md->rates) {
                    snprintf(rl, sizeof rl, "%u Hz", r);
                    if (Choice(rl, g_now.refresh == int(r))) pick = int(r);
                }
            ImGui::EndCombo();
            if (pick >= 0) {
                g_now.refresh = pick;
                IniWriteInt("Display", "Refresh", pick);
            }
        }
    }
    static const char* aspects[] = {"Ultrawide (hor+)", "4:3 with black bars", "Stretch"};
    static const char* aspectKeys[] = {"hor+", "4:3", "stretch"};
    int a = g_now.aspect == "4:3" ? 1 : g_now.aspect == "stretch" ? 2 : 0;
    if (ImGui::Combo("Aspect", &a, aspects, 3)) {
        g_now.aspect = aspectKeys[a];
        IniWrite("Display", "Aspect", g_now.aspect);
    }

    ImGui::SeparatorText("Frame rate");
    static const int fps[] = {30, 60, 120, 144, 165, 180, 240, -1, 0};
    int curFps = g_cfg.fpsLimit;
    char fl[64];
    auto fpsLabel = [&](int v, char* out, size_t n) {
        if (v == -1) snprintf(out, n, "Monitor refresh");
        else if (v == 0) snprintf(out, n, "Unlimited");
        else if (v == 120) snprintf(out, n, "120 fps (the game's own speed)");
        else snprintf(out, n, "%d fps", v);
    };
    fpsLabel(curFps, fl, sizeof fl);
    if (ImGui::BeginCombo("Frame rate limit", fl)) {
        for (int v : fps) {
            char l[64];
            fpsLabel(v, l, sizeof l);
            if (Choice(l, curFps == v)) {
                SetFpsLimit(v);
                IniWriteInt("Display", "FpsLimit", v);
            }
        }
        ImGui::EndCombo();
    }
    HelpMarker("The game's logic runs 120 steps per second: above 120 fps some frames repeat a step, 60 and 120 are exact.");
    bool bg = g_cfg.runInBackground;
    if (ImGui::Checkbox("Keep running in the background", &bg)) {
        SetRunInBackground(bg);
        IniWriteInt("Display", "RunInBackground", bg ? 1 : 0);
    }
    if (ImGui::Checkbox("Show FPS counter", &g_fpsCounter)) IniWriteInt("Overlay", "FpsCounter", g_fpsCounter ? 1 : 0);
}

void GraphicsTab() {
    DisplayInfo di = GetDisplayInfo();
    ImGui::SeparatorText("Smoothing (applies after a restart)");
    for (int f : {1, 2}) {
        if (f == 2) ImGui::SameLine();
        if (ImGui::RadioButton(f == 1 ? "Supersampling off" : "Supersampling 2x", g_now.supersample == f) && g_now.supersample != f) {
            g_now.supersample = f;
            IniWriteInt("Display", "Supersample", f);
        }
    }
    HelpMarker("2x renders the game at twice the resolution and averages it down: smooth edges everywhere, including "
               "trees and sprites. Needs more GPU power.");
    static const char* msaa[] = {"Off", "2x", "4x", "8x"};
    int mi = g_now.msaa >= 8 ? 3 : g_now.msaa >= 4 ? 2 : g_now.msaa >= 2 ? 1 : 0;
    if (ImGui::Combo("MSAA", &mi, msaa, 4)) {
        static const int v[] = {0, 2, 4, 8};
        g_now.msaa = v[mi];
        IniWriteInt("Display", "MSAA", g_now.msaa);
    }
    HelpMarker("Used only when supersampling and the ultrawide gap fill are both off.");
    ImGui::SeparatorText("Filtering");
    static const char* aniso[] = {"Off (the game's own)", "Bilinear", "2x", "4x", "8x", "16x"};
    static const int anisoVal[] = {0, 1, 2, 4, 8, 16};
    int ai = 0;
    for (int i = 0; i < 6; i++)
        if (g_cfg.anisotropy == anisoVal[i]) ai = i;
    if (ImGui::Combo("Anisotropic filtering", &ai, aniso, 6)) {
        SetAnisotropy(anisoVal[ai]);
        IniWriteInt("Display", "Anisotropy", anisoVal[ai]);
    }
    HelpMarker("Keeps ground and walls sharp at a slant. Applies at once.");
    ImGui::SeparatorText("Ultrawide");
    if (!di.horPlus) ImGui::TextDisabled("Aspect is not ultrawide (hor+): these do nothing now.");
    bool fill = g_cfg.sideFill == "ambient";
    if (ImGui::Checkbox("Fill the gaps at the screen edges", &fill)) {
        RequestSideFill(fill);
        IniWrite("Display", "SideFill", fill ? "ambient" : "black");
    }
    HelpMarker("Where a level has nothing beyond the original 4:3 view, show a soft blur of the scene instead of black.");
    HudSettings& h = HudConfig();
    if (ImGui::Checkbox("Ultrawide HUD", &h.on)) IniWriteInt("Display", "HudSpread", h.on ? 1 : 0);
    HelpMarker("Moves player panels and corner pictures out to the screen edges. Dialogs and menus stay centred.");
    if (!h.on) ImGui::BeginDisabled();
    int band = int(h.band * 100.0f + 0.5f), width = int(h.maxWidth * 100.0f + 0.5f);
    // sliders apply while dragging and are saved once, when let go
    if (ImGui::SliderInt("HUD band (of height)", &band, 10, 50, "%d%%")) h.band = float(band) / 100.0f;
    if (ImGui::IsItemDeactivatedAfterEdit()) IniWriteInt("Display", "HudBand", band);
    if (ImGui::SliderInt("Widest HUD piece moved", &width, 20, 80, "%d%%")) h.maxWidth = float(width) / 100.0f;
    if (ImGui::IsItemDeactivatedAfterEdit()) IniWriteInt("Display", "HudMaxWidth", width);
    if (!h.on) ImGui::EndDisabled();
}

void TexturesTab() {
    TextureInfo ti = GetTextureInfo();
    bool pack = TexturePackEnabled();
    if (ti.packSize == 0) ImGui::BeginDisabled();
    if (ImGui::Checkbox("HD textures", &pack)) {
        SetTexturePackEnabled(pack);
        IniWriteInt("Textures", "Enable", pack ? 1 : 0);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u in the pack, %u in use", ti.packSize, ti.loaded);
    bool soft = SoftEdgesEnabled();
    if (ImGui::Checkbox("Smooth text edges", &soft)) {
        SetSoftEdges(soft);
        IniWriteInt("Textures", "SoftEdges", soft ? 1 : 0);
    }
    bool dump = TextureDumpEnabled();
    if (ImGui::Checkbox("Collect textures the pack does not have yet", &dump)) {
        SetTextureDump(dump);
        IniWriteInt("Textures", "Dump", dump ? 1 : 0);
    }
    HelpMarker("Saves them to textures\\dump while you play; they can be upscaled into the pack later.");
    if (ti.packSize == 0) ImGui::EndDisabled();
    ImGui::Text("Seen this session: %llu distinct, %llu without a replacement, %llu collected",
                (unsigned long long)ti.unique, (unsigned long long)ti.missing, (unsigned long long)ti.dumped);
}

void GameTab() {
    const AutosaveStatus& st = GetAutosaveStatus();
    ImGui::SeparatorText("Saving");
    if (!st.configured) {
        ImGui::TextDisabled("Autosave is not available for this game (no [Autosave] description).");
    } else {
        bool on = AutosaveEnabled();
        if (ImGui::Checkbox("Autosave progress", &on)) {
            SetAutosaveEnabled(on);
            IniWriteInt("Autosave", "Enable", on ? 1 : 0);
        }
        HelpMarker("The game itself saves only from its pause menu. Autosave saves your profile a moment after your "
                   "progress changes and when the game is closed (also with Alt+F4). It never writes over another profile.");
        if (ImGui::Button("Save now")) AutosaveRequestNow();
        ImGui::SameLine();
        if (st.lastSaveMs) {
            unsigned ago = (ElapsedMs() - st.lastSaveMs) / 1000;
            ImGui::Text("Last save: %u:%02u ago, slot %d", ago / 60, ago % 60, st.slot + 1);
        } else {
            ImGui::TextDisabled("No save written this session");
        }
        if (!st.message.empty()) ImGui::TextWrapped("%s", st.message.c_str());
    }
    ImGui::SeparatorText("Sound");
    if (AudioMutedInWindows()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Windows has this game muted (volume mixer).");
        ImGui::SameLine();
        if (ImGui::Button("Unmute")) UnmuteProcessAudio();
    } else {
        ImGui::Text("Not muted. The game's volume is in its own options and in the Windows volume mixer.");
    }
}

void InfoTab() {
    DisplayInfo di = GetDisplayInfo();
    ImGui::Text("Frame rate: %.0f fps (limit %d)", g_fps, FpsTarget());
    ImGui::Text("Output: %d x %d, back buffer %d x %d", di.outW, di.outH, di.presW, di.presH);
    ImGui::Text("Game renders at %d x %d%s", di.renderW, di.renderH, di.ss > 1 ? " (supersampled 2x)" : "");
    ImGui::Text("Ultrawide (hor+): %s, offscreen scene: %s", di.horPlus ? "yes" : "no", di.scene ? "yes" : "no");
    ImGui::Text("Frames shown: %llu", (unsigned long long)di.frames);
    ImGui::Separator();
    ImGui::TextDisabled("Settings file: %s", g_cfg.iniPath.c_str());
    ImGui::TextDisabled("SpartaRecomp PC compatibility layer, built " __DATE__);
}

void Panel(int width, int height) {
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    float w = std::min(float(width) * 0.95f, ImGui::GetFontSize() * 36.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, float(height) * 0.95f));  // fixed width, height fits
    if (ImGui::Begin("SpartaRecomp settings   (F1 to close)", &g_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 19.0f);
        if (ImGui::BeginTabBar("tabs")) {
            if (TabFor("Display", 0)) { DisplayTab(); ImGui::EndTabItem(); }
            if (TabFor("Graphics", 1)) { GraphicsTab(); ImGui::EndTabItem(); }
            if (TabFor("Textures", 2)) { TexturesTab(); ImGui::EndTabItem(); }
            if (TabFor("Game", 3)) { GameTab(); ImGui::EndTabItem(); }
            if (TabFor("Info", 4)) { InfoTab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        g_tab = -1;
        ImGui::PopItemWidth();
        if (g_now != g_start) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "Some changes apply after a restart.");
            if (ImGui::Button("Restart now")) RestartGame();
            ImGui::SameLine();
            ImGui::TextDisabled("(save your game first)");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Changes are saved to spartacompat.ini as you make them.");
        if (ImGui::Button("Close")) g_open = false;
    }
    ImGui::End();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !g_popupWasOpen) g_open = false;
    g_popupWasOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void FpsCounter() {
    ImGui::SetNextWindowPos(ImVec2(12, 12));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%.0f fps", g_fps);
    ImGui::End();
}

}  // namespace

bool OverlayWantsInput() { return g_open; }

bool OverlayWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (!g_ready || !g_open) return false;
    ImGui_ImplWin32_WndProcHandler(w, msg, wp, lp);
    switch (msg) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            *result = 0;
            return true;  // the game must not react while the overlay has the input (Alt+F4 still reaches it)
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);  // the overlay draws its own cursor
                *result = TRUE;
                return true;
            }
            return false;
        default:
            return false;
    }
}

void OverlayDeviceReset(bool before) {
    if (g_ready && before) ImGui_ImplDX8_InvalidateDeviceObjects();  // state block; textures are managed
}

void OverlayRender(void* device, int width, int height) {
    if (!g_cfg.overlay || g_failed) return;
    auto* dev = static_cast<IDirect3DDevice8*>(device);
    PollToggle();
    double frame = GetDisplayInfo().frameSeconds;
    if (frame > 0) g_fps = g_fps > 0 ? g_fps * 0.9 + (1.0 / frame) * 0.1 : 1.0 / frame;
    if (!g_open && !g_fpsCounter) return;
    LayerCallScope layer;
    if (!g_ready && !Init(dev, height)) return;
    ImGui_ImplDX8_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));  // the back buffer, which may differ from the window
    io.DisplayFramebufferScale = ImVec2(1, 1);
    // the back buffer can be smaller than the window (borderless below native): mouse positions are scaled to it
    POINT pt;
    RECT rc;
    if (g_cfg.windowHidden || (g_wnd && GetForegroundWindow() != g_wnd))
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    else if (g_wnd && GetCursorPos(&pt) && ScreenToClient(g_wnd, &pt) && GetClientRect(g_wnd, &rc) && rc.right > 0 && rc.bottom > 0)
        io.AddMousePosEvent(float(pt.x) * width / rc.right, float(pt.y) * height / rc.bottom);
    if (g_testDown != ImGuiKey_None) {
        io.AddKeyEvent(g_testDown, false);
        g_testDown = ImGuiKey_None;
    } else if (g_testNext < g_testKeys.size() && g_testKeys[g_testNext].key != ImGuiKey_None &&  // F1/tabs: PollToggle
               Due(g_testKeys[g_testNext].ms, g_testKeys[g_testNext].fromFirstFrame)) {
        g_testDown = g_testKeys[g_testNext++].key;
        io.AddKeyEvent(g_testDown, true);
    }
    io.MouseDrawCursor = g_open && !g_cfg.windowHidden;
    ImGui::NewFrame();
    if (g_open) Panel(width, height);
    if (g_fpsCounter) FpsCounter();
    ImGui::Render();
    dev->BeginScene();
    ImGui_ImplDX8_RenderDrawData(ImGui::GetDrawData());
    dev->EndScene();
}

}  // namespace compat
