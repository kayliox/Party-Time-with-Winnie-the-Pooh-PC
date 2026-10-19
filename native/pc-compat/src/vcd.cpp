// Virtual CD: a folder with the disc contents appears to the game as a CD-ROM drive letter, so no ISO has to be mounted.
#include "compat.h"

#include <cstring>

namespace compat {

using GetLogicalDrives_t = DWORD(WINAPI*)();
using GetDriveTypeA_t = UINT(WINAPI*)(LPCSTR);
using GetFileAttributesA_t = DWORD(WINAPI*)(LPCSTR);
using SetCurrentDirectoryA_t = BOOL(WINAPI*)(LPCSTR);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using DeleteFileA_t = BOOL(WINAPI*)(LPCSTR);

static GetLogicalDrives_t o_GetLogicalDrives;
static GetDriveTypeA_t o_GetDriveTypeA;
static GetFileAttributesA_t o_GetFileAttributesA;
static SetCurrentDirectoryA_t o_SetCurrentDirectoryA;
static CreateFileA_t o_CreateFileA;
static CreateFileW_t o_CreateFileW;
static DeleteFileA_t o_DeleteFileA;

bool TranslatePathA(const char* in, std::string& out) {
    if (!g_cfg.vcdEnable || !in || !in[0] || in[1] != ':') return false;
    if (toupper((unsigned char)in[0]) != g_cfg.vcdDrive) return false;
    const char* rest = in + 2;
    while (*rest == '\\' || *rest == '/') rest++;
    out = g_cfg.vcdRoot;
    if (*rest) {
        out += '\\';
        out += rest;
    }
    return true;
}

static DWORD WINAPI H_GetLogicalDrives() {
    DWORD m = o_GetLogicalDrives();
    if (g_cfg.vcdEnable) m |= 1u << (g_cfg.vcdDrive - 'A');
    return m;
}

static UINT WINAPI H_GetDriveTypeA(LPCSTR root) {
    if (g_cfg.vcdEnable && root && root[0] && root[1] == ':' && toupper((unsigned char)root[0]) == g_cfg.vcdDrive)
        return DRIVE_CDROM;
    return o_GetDriveTypeA(root);
}

static DWORD WINAPI H_GetFileAttributesA(LPCSTR p) {
    std::string t;
    if (TranslatePathA(p, t)) {
        DWORD a = o_GetFileAttributesA(t.c_str());
        // a CD reports its files read-only
        return a == INVALID_FILE_ATTRIBUTES ? a : (a | FILE_ATTRIBUTE_READONLY);
    }
    return o_GetFileAttributesA(p);
}

static BOOL WINAPI H_SetCurrentDirectoryA(LPCSTR p) {
    std::string t;
    if (TranslatePathA(p, t)) {
        BOOL ok = o_SetCurrentDirectoryA(t.c_str());
        Log("vcd: SetCurrentDirectory(%s) -> %s : %s", p, t.c_str(), ok ? "ok" : "FAILED");
        return ok;
    }
    return o_SetCurrentDirectoryA(p);
}

static HANDLE WINAPI H_CreateFileA(LPCSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                                   HANDLE tmpl) {
    std::string t;
    const char* path = TranslatePathA(p, t) ? t.c_str() : p;
    HANDLE h = o_CreateFileA(path, acc, share, sa, disp, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE) Log("CreateFileA(%s) failed: %lu", p ? p : "(null)", GetLastError());
    return h;
}

static HANDLE WINAPI H_CreateFileW(LPCWSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                                   HANDLE tmpl) {
    if (g_cfg.vcdEnable && p && p[0] && p[1] == L':' && towupper(p[0]) == wchar_t(g_cfg.vcdDrive)) {
        char narrow[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, p, -1, narrow, sizeof narrow, nullptr, nullptr);
        std::string t;
        if (TranslatePathA(narrow, t)) return o_CreateFileA(t.c_str(), acc, share, sa, disp, flags, tmpl);
    }
    return o_CreateFileW(p, acc, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI H_DeleteFileA(LPCSTR p) {
    std::string t;
    if (TranslatePathA(p, t)) {
        SetLastError(ERROR_WRITE_PROTECT);
        return FALSE;
    }
    return o_DeleteFileA(p);
}

void InstallVcdHooks(HMODULE exe) {
    if (!g_cfg.vcdEnable) return;
    if (GetFileAttributesA(g_cfg.vcdRoot.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("vcd: root folder %s does not exist; virtual CD disabled", g_cfg.vcdRoot.c_str());
        g_cfg.vcdEnable = false;
        return;
    }
    if (!g_cfg.vcdDrive) {
        // the highest free letter keeps clear of real drives and of letters users map by hand
        DWORD used = GetLogicalDrives();
        for (char c = 'Z'; c >= 'D'; c--)
            if (!(used & (1u << (c - 'A')))) { g_cfg.vcdDrive = c; break; }
    }
    if (!g_cfg.vcdDrive) {
        Log("vcd: no free drive letter; virtual CD disabled");
        g_cfg.vcdEnable = false;
        return;
    }
    Log("vcd: %c: -> %s", g_cfg.vcdDrive, g_cfg.vcdRoot.c_str());
#define HOOK(dll, name) o_##name = reinterpret_cast<name##_t>(HookImport(exe, dll, #name, reinterpret_cast<void*>(H_##name))); \
    if (!o_##name) o_##name = reinterpret_cast<name##_t>(GetProcAddress(GetModuleHandleA(dll), #name));
    HOOK("kernel32.dll", GetLogicalDrives)
    HOOK("kernel32.dll", GetDriveTypeA)
    HOOK("kernel32.dll", GetFileAttributesA)
    HOOK("kernel32.dll", SetCurrentDirectoryA)
    HOOK("kernel32.dll", CreateFileA)
    HOOK("kernel32.dll", CreateFileW)
    HOOK("kernel32.dll", DeleteFileA)
#undef HOOK
}

}  // namespace compat
