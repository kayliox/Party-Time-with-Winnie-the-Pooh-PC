// Portable registry: keys under configured prefixes live in an ini file next to the game instead of the real registry.
// The game then needs no installer, leaves nothing behind, and its launcher settings can be shipped as data.
#include "compat.h"

#include <cstdio>
#include <cstring>

namespace compat {

using RegCreateKeyExA_t = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
using RegOpenKeyExA_t = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using RegQueryValueExA_t = LONG(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegSetValueExA_t = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
using RegCloseKey_t = LONG(WINAPI*)(HKEY);

static RegCreateKeyExA_t o_RegCreateKeyExA;
static RegOpenKeyExA_t o_RegOpenKeyExA;
static RegQueryValueExA_t o_RegQueryValueExA;
static RegSetValueExA_t o_RegSetValueExA;
static RegCloseKey_t o_RegCloseKey;

static CRITICAL_SECTION g_regLock;
static std::vector<std::string> g_keys;  // index -> full key path ("" = free slot)

// fake handles are odd numbers in a range real (kernel, multiple-of-4) registry handles never use
static HKEY MakeHandle(size_t i) { return reinterpret_cast<HKEY>(uintptr_t(0x6B000001u + 2u * i)); }
static bool HandleIndex(HKEY h, size_t& i) {
    uintptr_t v = reinterpret_cast<uintptr_t>(h);
    if (v < 0x6B000001u || v >= 0x6B000001u + 2u * 4096 || !(v & 1)) return false;
    i = (v - 0x6B000001u) / 2;
    return i < g_keys.size() && !g_keys[i].empty();
}

static bool RootName(HKEY root, std::string& out) {
    size_t i;
    if (root == HKEY_CURRENT_USER) out = "HKCU";
    else if (root == HKEY_LOCAL_MACHINE) out = "HKLM";
    else if (root == HKEY_CLASSES_ROOT) out = "HKCR";
    else if (root == HKEY_USERS) out = "HKU";
    else if (HandleIndex(root, i)) out = g_keys[i];
    else return false;
    return true;
}

static bool IsVirtual(const std::string& path) {
    for (auto& p : g_cfg.regPrefixes)
        if (path.size() >= p.size() && _strnicmp(path.c_str(), p.c_str(), p.size()) == 0 &&
            (path.size() == p.size() || path[p.size()] == '\\'))
            return true;
    return false;
}

static bool VirtualPath(HKEY root, LPCSTR sub, std::string& out) {
    if (g_cfg.regPrefixes.empty() || !RootName(root, out)) return false;
    if (sub && *sub) {
        out += '\\';
        out += sub;
    }
    while (!out.empty() && out.back() == '\\') out.pop_back();
    return IsVirtual(out);
}

static HKEY OpenVirtual(const std::string& path) {
    EnterCriticalSection(&g_regLock);
    size_t i = 0;
    while (i < g_keys.size() && !g_keys[i].empty()) i++;
    if (i == g_keys.size()) g_keys.push_back(path);
    else g_keys[i] = path;
    LeaveCriticalSection(&g_regLock);
    return MakeHandle(i);
}

static LONG WINAPI H_RegCreateKeyExA(HKEY root, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
                                     LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp) {
    std::string path;
    if (VirtualPath(root, sub, path)) {
        if (_stricmp(path.c_str(), g_cfg.padRegKey.c_str()) == 0) AutofillPads();
        *out = OpenVirtual(path);
        if (disp) *disp = REG_OPENED_EXISTING_KEY;
        Log("vreg: open %s", path.c_str());
        return ERROR_SUCCESS;
    }
    return o_RegCreateKeyExA(root, sub, res, cls, opt, sam, sa, out, disp);
}

static LONG WINAPI H_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
    std::string path;
    if (VirtualPath(root, sub, path)) {
        if (_stricmp(path.c_str(), g_cfg.padRegKey.c_str()) == 0) AutofillPads();
        *out = OpenVirtual(path);
        Log("vreg: open %s", path.c_str());
        return ERROR_SUCCESS;
    }
    return o_RegOpenKeyExA(root, sub, opt, sam, out);
}

static LONG WINAPI H_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb) {
    size_t i;
    if (!HandleIndex(h, i)) return o_RegQueryValueExA(h, name, res, type, data, cb);
    char buf[2048];
    const char* vn = (name && *name) ? name : "@";
    GetPrivateProfileStringA(g_keys[i].c_str(), vn, "\x01", buf, sizeof buf, g_cfg.regStore.c_str());
    if (buf[0] == '\x01') {
        Log("vreg: %s\\%s not set", g_keys[i].c_str(), vn);
        return ERROR_FILE_NOT_FOUND;
    }
    BYTE tmp[1024];
    DWORD t, n;
    if (_strnicmp(buf, "dword:", 6) == 0) {
        DWORD v = strtoul(buf + 6, nullptr, 0);
        memcpy(tmp, &v, 4);
        t = REG_DWORD;
        n = 4;
    } else {
        const char* s = _strnicmp(buf, "sz:", 3) == 0 ? buf + 3 : buf;
        n = DWORD(strlen(s) + 1);
        if (n > sizeof tmp) n = sizeof tmp;
        memcpy(tmp, s, n);
        tmp[n - 1] = 0;
        t = REG_SZ;
    }
    if (type) *type = t;
    if (!cb) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    if (!data) {
        *cb = n;
        return ERROR_SUCCESS;
    }
    if (*cb < n) {
        *cb = n;
        return ERROR_MORE_DATA;
    }
    memcpy(data, tmp, n);
    *cb = n;
    return ERROR_SUCCESS;
}

static LONG WINAPI H_RegSetValueExA(HKEY h, LPCSTR name, DWORD res, DWORD type, const BYTE* data, DWORD cb) {
    size_t i;
    if (!HandleIndex(h, i)) return o_RegSetValueExA(h, name, res, type, data, cb);
    char buf[1100];
    if ((type == REG_DWORD) && cb >= 4) {
        DWORD v;
        memcpy(&v, data, 4);
        snprintf(buf, sizeof buf, "dword:0x%08lx", v);
    } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        snprintf(buf, sizeof buf, "sz:%.*s", int(cb > 1024 ? 1024 : cb), reinterpret_cast<const char*>(data));
    } else {
        Log("vreg: value type %lu of %s not supported, ignored", type, name ? name : "@");
        return ERROR_SUCCESS;
    }
    WritePrivateProfileStringA(g_keys[i].c_str(), (name && *name) ? name : "@", buf, g_cfg.regStore.c_str());
    return ERROR_SUCCESS;
}

static LONG WINAPI H_RegCloseKey(HKEY h) {
    size_t i;
    if (!HandleIndex(h, i)) return o_RegCloseKey(h);
    EnterCriticalSection(&g_regLock);
    g_keys[i].clear();
    LeaveCriticalSection(&g_regLock);
    return ERROR_SUCCESS;
}

void InstallRegistryHooks(HMODULE exe) {
    InitializeCriticalSection(&g_regLock);
    if (g_cfg.regPrefixes.empty()) return;
    for (auto& p : g_cfg.regPrefixes) Log("vreg: %s -> %s", p.c_str(), g_cfg.regStore.c_str());
#define HOOK(name) o_##name = reinterpret_cast<name##_t>(HookImport(exe, "advapi32.dll", #name, reinterpret_cast<void*>(H_##name))); \
    if (!o_##name) o_##name = reinterpret_cast<name##_t>(GetProcAddress(GetModuleHandleA("advapi32.dll"), #name));
    HOOK(RegCreateKeyExA)
    HOOK(RegOpenKeyExA)
    HOOK(RegQueryValueExA)
    HOOK(RegSetValueExA)
    HOOK(RegCloseKey)
#undef HOOK
}

// Writes a DWORD into the portable store (used by the pad autofill before the game reads its settings).
void RegStoreDword(const std::string& key, const std::string& name, DWORD v) {
    char buf[32];
    snprintf(buf, sizeof buf, "dword:0x%08lx", v);
    WritePrivateProfileStringA(key.c_str(), name.c_str(), buf, g_cfg.regStore.c_str());
}

}  // namespace compat
