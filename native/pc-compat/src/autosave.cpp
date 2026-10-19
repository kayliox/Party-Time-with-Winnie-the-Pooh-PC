// Autosave: console ports often save only from a menu, so quitting loses the progress since then. The layer watches
// the game's live progress block and, once it has changed and settled (or when the window is closed), does exactly
// what the game's own save action does (copy the block, call the game's save function), between two frames on the
// game's thread. Every address is data ([Autosave] in spartacompat.ini); the target slot is checked against the save
// file first, so another profile is never overwritten.
#include "compat.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace compat {

namespace {

bool g_configured, g_enabled;
uintptr_t g_watch, g_copySrc, g_copyDst, g_call, g_argVar;
size_t g_watchLen, g_copyLen;
unsigned g_argMax = 255;
std::string g_file;
uint32_t g_fileBase, g_fileStride, g_nameLen = 16, g_stableMs = 1500, g_minIntervalMs = 5000;
std::vector<uint8_t> g_last, g_saved;
uint32_t g_changedAt, g_lastSaveMs;
bool g_pending, g_forceNow, g_inSave;
AutosaveStatus g_status;

bool ReadMem(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a), out, n, &got) && got == n;
}

std::vector<uint32_t> ParseNumbers(const std::string& s, char sep) {
    std::vector<uint32_t> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(sep, i);
        std::string part = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!part.empty()) v.push_back(uint32_t(strtoul(part.c_str(), nullptr, 0)));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return v;
}

std::string Name(const uint8_t* p) {
    size_t n = 0;
    while (n < g_nameLen && p[n]) n++;
    return std::string(reinterpret_cast<const char*>(p), n);
}

void SetStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_status.message != buf) Log("autosave: %s", buf);
    g_status.message = buf;
}

// The save file's slot must be missing, empty or already this profile: returns false (and why) otherwise.
bool SlotIsOurs(unsigned slot, const std::vector<uint8_t>& cur, bool* unchanged) {
    *unchanged = false;
    std::string path = g_cfg.exeDir + g_file;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return true;  // no save file yet: the game creates it
    std::vector<uint8_t> data(GetFileSize(f, nullptr) > 0x100000 ? 0 : GetFileSize(f, nullptr));
    DWORD got = 0;
    bool ok = !data.empty() && ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size();
    CloseHandle(f);
    size_t off = size_t(g_fileBase) + size_t(slot) * g_fileStride;
    if (!ok || off + cur.size() > data.size()) return true;  // unreadable or short: the game rewrites it as it would
    const uint8_t* s = data.data() + off;
    if (memcmp(s, cur.data(), cur.size()) == 0) {
        *unchanged = true;
        return true;
    }
    if (s[0] && Name(s) != Name(cur.data())) {
        SetStatus("not saved: slot %u of %s holds another profile (\"%s\", playing \"%s\")", slot + 1, g_file.c_str(), Name(s).c_str(),
                  Name(cur.data()).c_str());
        return false;
    }
    return true;
}

bool Save(const char* why) {
    std::vector<uint8_t> cur(g_watchLen);
    if (!ReadMem(g_watch, cur.data(), cur.size())) return false;
    g_pending = false;
    if (!cur[0]) {
        SetStatus("not saved: no profile loaded");
        return false;
    }
    uint32_t arg = 0;
    if (!ReadMem(g_argVar, &arg, sizeof arg) || (arg & 0xFF) > g_argMax) {
        SetStatus("not saved: no valid save slot (0x%08x)", arg);
        return false;
    }
    unsigned slot = arg & 0xFF;
    bool unchanged = false;
    if (!SlotIsOurs(slot, cur, &unchanged)) return false;
    g_saved = cur;
    if (unchanged) {
        SetStatus("\"%s\" is saved (slot %u)", Name(cur.data()).c_str(), slot + 1);
        return true;
    }
    if (g_copyLen) memcpy(reinterpret_cast<void*>(g_copyDst), reinterpret_cast<const void*>(g_copySrc), g_copyLen);
    g_inSave = true;
    char ok = reinterpret_cast<char(__cdecl*)(uint32_t)>(g_call)(arg);  // the game's own save function
    g_inSave = false;
    g_lastSaveMs = ElapsedMs() ? ElapsedMs() : 1;
    g_status.lastSaveMs = g_lastSaveMs;
    g_status.slot = int(slot);
    g_status.saves++;
    g_status.message.clear();  // every write is logged, also when it repeats the previous one
    SetStatus("%s \"%s\" to slot %u (%s)", ok ? "saved" : "save FAILED for", Name(cur.data()).c_str(), slot + 1, why);
    return ok != 0;
}

}  // namespace

void AutosaveInit() {
    std::string watch = IniString("Autosave", "Watch", "");
    std::vector<uint32_t> w = ParseNumbers(watch, ':');
    if (w.size() != 2 || !w[1] || w[1] > 0x10000) return;  // not described for this game
    std::vector<uint32_t> c = ParseNumbers(IniString("Autosave", "Copy", ""), ':');
    std::vector<uint32_t> fs = ParseNumbers(IniString("Autosave", "FileSlots", ""), ':');
    g_watch = w[0];
    g_watchLen = w[1];
    if (c.size() == 3 && c[2] <= 0x10000) {
        g_copySrc = c[0];
        g_copyDst = c[1];
        g_copyLen = c[2];
    }
    g_call = strtoul(IniString("Autosave", "Call", "0").c_str(), nullptr, 0);
    g_argVar = strtoul(IniString("Autosave", "ArgVar", "0").c_str(), nullptr, 0);
    g_argMax = unsigned(IniInt("Autosave", "ArgMax", 255));
    g_file = IniString("Autosave", "File", "");
    if (fs.size() == 2) {
        g_fileBase = fs[0];
        g_fileStride = fs[1];
    }
    g_nameLen = uint32_t(IniInt("Autosave", "NameLen", 16));
    g_stableMs = uint32_t(IniInt("Autosave", "StableMs", 1500));
    g_minIntervalMs = uint32_t(IniInt("Autosave", "MinIntervalMs", 5000));
    // like a patch, the save function must still have its known first bytes (a different exe must not be called)
    std::vector<uint8_t> check;
    std::string hex = IniString("Autosave", "CallCheck", "");
    for (size_t i = 0; i + 1 < hex.size();) {
        if (hex[i] == ' ') {
            i++;
            continue;
        }
        check.push_back(uint8_t(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
        i += 2;
    }
    std::vector<uint8_t> code(check.size());
    if (!g_call || !g_argVar || g_file.empty() || !g_fileStride || check.empty() || !ReadMem(g_call, code.data(), code.size()) || code != check) {
        Log("autosave: refused: the save function at 0x%08lx is not the expected code (or [Autosave] is incomplete)", (unsigned long)g_call);
        return;
    }
    g_configured = true;
    g_enabled = IniInt("Autosave", "Enable", 1) != 0;
    g_status.configured = true;
    Log("autosave: %s (watching 0x%08lx+0x%x, save function 0x%08lx, slot from 0x%08lx, file %s)", g_enabled ? "on" : "off",
        (unsigned long)g_watch, unsigned(g_watchLen), (unsigned long)g_call, (unsigned long)g_argVar, g_file.c_str());
}

void AutosaveTick() {
    if (!g_configured || g_inSave) return;
    std::vector<uint8_t> cur(g_watchLen);
    if (!ReadMem(g_watch, cur.data(), cur.size())) return;
    uint32_t now = ElapsedMs();
    if (g_last.empty()) {  // the first look is the starting state, not a change
        g_last = cur;
        g_saved = cur;
    } else if (cur != g_last) {
        g_last = cur;
        g_changedAt = now;
        g_pending = cur != g_saved;
    }
    if (g_forceNow) {
        g_forceNow = false;
        Save("Save now");
    } else if (g_enabled && g_pending && now - g_changedAt >= g_stableMs && (!g_lastSaveMs || now - g_lastSaveMs >= g_minIntervalMs)) {
        Save("progress changed");
    }
}

void AutosaveFlush(const char* why) {
    if (!g_configured || !g_enabled || g_inSave) return;
    std::vector<uint8_t> cur(g_watchLen);
    if (ReadMem(g_watch, cur.data(), cur.size()) && !g_saved.empty() && cur != g_saved) Save(why);
}

void AutosaveRequestNow() { g_forceNow = true; }
void SetAutosaveEnabled(bool on) { g_enabled = on && g_configured; }
bool AutosaveEnabled() { return g_enabled; }
const AutosaveStatus& GetAutosaveStatus() { return g_status; }

}  // namespace compat
