// Player-facing installer for a pc-compat game: prepares a playable copy from the player's own game CD (or a disc image
// made from it), checks that it is the supported edition, adds the compatibility layer and its settings, and creates
// shortcuts. Nothing is written to the registry and no admin rights are needed: removing the game = deleting its folder.
// The game facts come from games/<ID>/profile.json (generated game_manifest.h); the layer and settings are embedded.
// Command line (tests, power users): Setup.exe /source <drive, folder or image> /target <folder> [/quiet] [/noshortcuts]
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game_manifest.h"
#include "iso9660.h"
#include "resource.h"

namespace {

// ---------------------------------------------------------------- small helpers

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty() || b == L".") return a;
    wchar_t last = a.back();
    return (last == L'\\' || last == L'/') ? a + b : a + L"\\" + b;
}

std::wstring Slashes(std::wstring s) {
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return s;
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    return buf;
}

std::wstring Mb(uint64_t bytes) { return Format(L"%llu MB", (unsigned long long)((bytes + (1 << 20) - 1) >> 20)); }

// "\\?\" paths reach names Win32 normally cannot open, such as an extracted ISO 9660 name ending in a dot ("NL.")
std::wstring LongPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    std::vector<wchar_t> full(32768);
    DWORD n = GetFullPathNameW(p.c_str(), 32768, full.data(), nullptr);
    if (!n || n >= 32768) return p;
    std::wstring f(full.data(), n);
    if (f.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + f.substr(2);
    return L"\\\\?\\" + f;
}

// a copied name as a CD driver shows it: no trailing dots in any component
std::wstring CleanRel(const std::wstring& rel) {
    std::wstring out, part;
    for (size_t i = 0; i <= rel.size(); i++) {
        if (i == rel.size() || rel[i] == L'\\') {
            while (!part.empty() && part.back() == L'.') part.pop_back();
            if (!out.empty()) out += L'\\';
            out += part;
            part.clear();
        } else {
            part += rel[i];
        }
    }
    return out;
}

bool MakeDirs(const std::wstring& dir) {
    if (dir.empty()) return true;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2 && !MakeDirs(dir.substr(0, cut))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

FILE* g_log;
void Log(const std::wstring& s) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetSystemTime(&t);
    fwprintf(g_log, L"%04u-%02u-%02uT%02u:%02u:%02uZ %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, s.c_str());
    fflush(g_log);
}

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void Add(const uint8_t* p, size_t n) {
        if (hash_) BCryptHashData(hash_, const_cast<PUCHAR>(p), ULONG(n), 0);
    }
    std::string Hex() {
        uint8_t d[32] = {};
        if (hash_) BCryptFinishHash(hash_, d, 32, 0);
        char out[65];
        for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
        return std::string(out, 64);
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// ---------------------------------------------------------------- where the game comes from: a disc drive/folder or an image

struct SrcFile {
    std::wstring rel;  // relative to the listed directory, '\' separators
    uint64_t size;
};

class Source {
public:
    virtual ~Source() = default;
    virtual bool Exists(const std::wstring& rel, bool* isDir) = 0;
    virtual bool ListTree(const std::wstring& rel, std::vector<SrcFile>* out) = 0;
    virtual bool Stream(const std::wstring& rel, const std::function<bool(const uint8_t*, size_t)>& sink) = 0;
};

class DirSource : public Source {
public:
    // only the root is normalised: GetFullPathName would also drop a trailing dot from the names below it
    explicit DirSource(const std::wstring& root) : root_(LongPath(root)) {}
    bool Exists(const std::wstring& rel, bool* isDir) override {
        DWORD a = GetFileAttributesW(JoinPath(root_, Slashes(rel)).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) return false;
        *isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
    }
    bool ListTree(const std::wstring& rel, std::vector<SrcFile>* out) override { return Walk(JoinPath(root_, Slashes(rel)), L"", out, 0); }
    bool Stream(const std::wstring& rel, const std::function<bool(const uint8_t*, size_t)>& sink) override {
        HANDLE f = CreateFileW(JoinPath(root_, Slashes(rel)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        std::vector<uint8_t> buf(1 << 20);
        bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr)) {
                ok = false;
                break;
            }
            if (got == 0) break;
            if (!sink(buf.data(), got)) {
                ok = false;
                break;
            }
        }
        CloseHandle(f);
        return ok;
    }

private:
    bool Walk(const std::wstring& dir, const std::wstring& prefix, std::vector<SrcFile>* out, int depth) {
        if (depth > 32) return false;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool ok = true;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring rel = prefix.empty() ? n : prefix + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = Walk(JoinPath(dir, n), rel, out, depth + 1) && ok;
            else out->push_back({rel, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return ok;
    }
    std::wstring root_;
};

class ImageSource : public Source {
public:
    bool Open(const std::wstring& path, std::wstring* err) { return iso_.Open(path, err); }
    bool Exists(const std::wstring& rel, bool* isDir) override {
        IsoEntry e;
        if (!iso_.Find(rel, &e)) return false;
        *isDir = e.dir;
        return true;
    }
    bool ListTree(const std::wstring& rel, std::vector<SrcFile>* out) override {
        IsoEntry e;
        return iso_.Find(rel, &e) && e.dir && Walk(e, L"", out, 0);
    }
    bool Stream(const std::wstring& rel, const std::function<bool(const uint8_t*, size_t)>& sink) override {
        IsoEntry e;
        return iso_.Find(rel, &e) && iso_.Read(e, sink);
    }

private:
    bool Walk(const IsoEntry& dir, const std::wstring& prefix, std::vector<SrcFile>* out, int depth) {
        std::vector<IsoEntry> items;
        if (depth > 32 || !iso_.List(dir, &items)) return false;
        for (const IsoEntry& e : items) {
            std::wstring rel = prefix.empty() ? e.name : prefix + L"\\" + e.name;
            if (e.dir) {
                if (!Walk(e, rel, out, depth + 1)) return false;
            } else {
                out->push_back({rel, e.size});
            }
        }
        return true;
    }
    IsoImage iso_;
};

std::unique_ptr<Source> OpenSource(const std::wstring& path, std::wstring* err) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        *err = L"Not found: " + path;
        return nullptr;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) return std::make_unique<DirSource>(path);
    auto img = std::make_unique<ImageSource>();
    if (!img->Open(path, err)) return nullptr;
    return img;
}

enum class Check { NotGame, Supported, OtherVersion };

struct CheckResult {
    Check kind = Check::NotGame;
    std::string exeHash;
    std::wstring message;
};

CheckResult CheckGame(Source& src) {
    CheckResult r;
    bool dir = false;
    std::wstring exeRel = std::wstring(kInstall[0].from) + L"/" + GAME_EXE;
    if (!src.Exists(DISC_MARKER, &dir) || !src.Exists(exeRel, &dir) || dir) {
        r.message = L"This is not the " GAME_TITLE L" disc (" DISC_MARKER L" or " + exeRel + L" is missing).";
        return r;
    }
    Sha256 h;
    if (!src.Stream(exeRel, [&](const uint8_t* p, size_t n) {
            h.Add(p, n);
            return true;
        })) {
        r.message = L"The disc could not be read (" + exeRel + L"). Clean the disc or make a new image.";
        return r;
    }
    r.exeHash = h.Hex();
    if (r.exeHash == GAME_EXE_SHA256) {
        r.kind = Check::Supported;
        r.message = L"Found " GAME_TITLE L" (" GAME_EDITION L"). This is the supported version.";
    } else {
        r.kind = Check::OtherVersion;
        r.message = L"Found " GAME_TITLE L", but a different version than the supported " GAME_EDITION
                    L". The fixes check the game before changing it, so some may switch themselves off.";
    }
    return r;
}

// ---------------------------------------------------------------- installing

struct Job {
    std::wstring source, target;
    bool shortcuts = true;
    bool quiet = false;
    HWND notify = nullptr;
};

enum { WM_APP_PROGRESS = WM_APP + 1, WM_APP_STATUS, WM_APP_DONE };

void Status(const Job& job, const std::wstring& s) {
    Log(s);
    if (job.notify) PostMessageW(job.notify, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(s)));
}

bool WriteResource(int id, const std::wstring& dest) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!data) return false;
    HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, size, &put, nullptr) && put == size;
    CloseHandle(f);
    return ok;
}

// Settings a player can change (F1 overlay or by hand): an update keeps their values; everything else (patches,
// addresses, new keys) comes from the new version.
const wchar_t* const kUserKeys[] = {
    L"Display|Mode", L"Display|Width", L"Display|Height", L"Display|Refresh", L"Display|Aspect", L"Display|FpsLimit",
    L"Display|MSAA", L"Display|Supersample", L"Display|SideFill", L"Display|HudSpread", L"Display|HudBand",
    L"Display|HudMaxWidth", L"Display|Anisotropy", L"Display|RunInBackground", L"Display|Renderer", L"Display|FlipModel",
    L"Display|Downsample", L"Overlay|Enable", L"Overlay|FpsCounter", L"Textures|Enable", L"Textures|SoftEdges",
    L"Textures|Dump", L"Autosave|Enable"};

bool InstallSettings(const Job& job, bool update) {
    std::wstring ini = JoinPath(job.target, L"spartacompat.ini");
    std::vector<std::pair<std::wstring, std::wstring>> keep;
    if (update && FileExists(ini)) {
        for (const wchar_t* uk : kUserKeys) {
            std::wstring s = uk, sec = s.substr(0, s.find(L'|')), key = s.substr(s.find(L'|') + 1);
            wchar_t v[256];
            DWORD n = GetPrivateProfileStringW(sec.c_str(), key.c_str(), L"\x01", v, 256, ini.c_str());
            if (n && v[0] != 1) keep.push_back({s, v});
        }
    }
    if (!WriteResource(IDR_SETTINGS, ini)) return false;
    for (auto& k : keep) {
        std::wstring sec = k.first.substr(0, k.first.find(L'|')), key = k.first.substr(k.first.find(L'|') + 1);
        WritePrivateProfileStringW(sec.c_str(), key.c_str(), k.second.c_str(), ini.c_str());
    }
    if (!keep.empty()) Status(job, Format(L"Kept %u of your settings.", unsigned(keep.size())));
    std::wstring reg = JoinPath(job.target, L"registry.ini");  // the game's own options (keys, players): never replaced
    if (!(update && FileExists(reg)) && !WriteResource(IDR_REGISTRY, reg)) return false;
    MakeDirs(JoinPath(job.target, L"licenses"));
    return WriteResource(IDR_PROXY, JoinPath(job.target, GAME_PROXY)) && WriteResource(IDR_LICENSE, JoinPath(job.target, L"licenses\\fix-LICENSE.txt")) &&
           WriteResource(IDR_NOTICES, JoinPath(job.target, L"licenses\\THIRD_PARTY_NOTICES.txt")) &&
           WriteResource(IDR_README, JoinPath(job.target, L"How to play (fix).txt"));
}

bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(dir.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(GAME_TITLE);
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf))) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    if (pf) pf->Release();
    link->Release();
    return ok;
}

void MakeShortcuts(const Job& job) {
    std::wstring exe = JoinPath(job.target, GAME_EXE);
    const KNOWNFOLDERID* folders[] = {&FOLDERID_Desktop, &FOLDERID_Programs};
    for (const KNOWNFOLDERID* id : folders) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &path))) {
            std::wstring lnk = JoinPath(path, SHORTCUT_NAME L".lnk");
            Status(job, MakeShortcut(lnk, exe, job.target) ? L"Shortcut: " + lnk : L"Could not create the shortcut " + lnk);
            CoTaskMemFree(path);
        }
    }
}

bool IsPreparedCopy(const std::wstring& target) {
    return FileExists(JoinPath(target, GAME_EXE)) && FileExists(JoinPath(target, L"spartacompat.ini"));
}

DWORD RunJob(Job& job) {
    MakeDirs(job.target);
    g_log = _wfopen(JoinPath(job.target, L"fix-install.log").c_str(), L"a, ccs=UTF-8");
    Log(L"installer for " GAME_TITLE L", target " + job.target);
    bool update = IsPreparedCopy(job.target);
    if (update) {
        Status(job, L"Updating the fix in your existing copy (your saves and settings stay).");
    } else {
        std::wstring err;
        std::unique_ptr<Source> src = OpenSource(job.source, &err);
        if (!src) {
            Status(job, L"Cannot read the game disc: " + err);
            return 2;
        }
        CheckResult c = CheckGame(*src);
        Status(job, c.message);
        if (c.kind == Check::NotGame) return 3;
        // plan: the installer's game files go to the folder, the disc's data folders to cd\ (the layer serves them as the CD)
        struct Item {
            std::wstring from, to;
            uint64_t size;
        };
        std::vector<Item> plan;
        uint64_t total = 0;
        for (const InstallMap& m : kInstall) {
            std::vector<SrcFile> files;
            if (!src->ListTree(m.from, &files)) {
                Status(job, std::wstring(L"The disc has no readable ") + m.from);
                return 4;
            }
            for (auto& f : files) plan.push_back({Slashes(std::wstring(m.from)) + L"\\" + f.rel, JoinPath(JoinPath(job.target, Slashes(m.to)), CleanRel(f.rel)), f.size});
        }
        for (const wchar_t* cd : kCd) {
            std::vector<SrcFile> files;
            if (!src->ListTree(cd, &files)) {
                Status(job, std::wstring(L"The disc has no readable ") + cd);
                return 4;
            }
            for (auto& f : files) plan.push_back({Slashes(cd) + L"\\" + f.rel, JoinPath(JoinPath(job.target, L"cd\\" + Slashes(cd)), CleanRel(f.rel)), f.size});
        }
        for (auto& i : plan) total += i.size;
        ULARGE_INTEGER freeBytes;
        if (GetDiskFreeSpaceExW(job.target.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < total + (64ull << 20)) {
            Status(job, L"Not enough free space: the game needs " + Mb(total) + L", the drive has " + Mb(freeBytes.QuadPart) + L".");
            return 5;
        }
        Status(job, Format(L"Copying %u files (%ls) ...", unsigned(plan.size()), Mb(total).c_str()));
        uint64_t done = 0;
        DWORD lastPost = 0;
        std::string exeHash;
        for (auto& i : plan) {
            size_t cut = i.to.find_last_of(L'\\');
            MakeDirs(i.to.substr(0, cut));
            HANDLE out = CreateFileW(i.to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE) {
                Status(job, L"Cannot write " + i.to + Format(L" (error %lu).", GetLastError()));
                return 6;
            }
            bool isExe = Lower(i.to) == Lower(JoinPath(job.target, GAME_EXE));
            Sha256 h;
            bool ok = src->Stream(i.from, [&](const uint8_t* p, size_t n) {
                DWORD put = 0;
                if (!WriteFile(out, p, DWORD(n), &put, nullptr) || put != n) return false;
                if (isExe) h.Add(p, n);
                done += n;
                DWORD now = GetTickCount();
                if (job.notify && now - lastPost > 100) {
                    lastPost = now;
                    PostMessageW(job.notify, WM_APP_PROGRESS, WPARAM(total ? done * 1000 / total : 0), 0);
                }
                return true;
            });
            CloseHandle(out);
            if (!ok) {
                Status(job, L"Copy failed: " + i.from + L". Clean the disc or make a new image, then try again.");
                return 7;
            }
            if (isExe) exeHash = h.Hex();
        }
        if (exeHash != c.exeHash) {
            Status(job, L"The copied game program differs from the disc's: the disc reads unreliably.");
            return 8;
        }
        Log(L"copied " + Mb(total));
    }
    if (job.notify) PostMessageW(job.notify, WM_APP_PROGRESS, 1000, 0);
    if (!InstallSettings(job, update)) {
        Status(job, L"Could not write the fix files into " + job.target + L".");
        return 9;
    }
    if (job.shortcuts) MakeShortcuts(job);
    Status(job, update ? L"Done: the fix is updated." : L"Done. Start the game with the shortcut or the Play button. Press F1 in the game for settings.");
    return 0;
}

DWORD WINAPI JobThread(LPVOID p) {
    auto* job = static_cast<Job*>(p);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DWORD code = RunJob(*job);
    if (g_log) {
        Log(Format(L"result %lu", code));
        fclose(g_log);
        g_log = nullptr;
    }
    CoUninitialize();
    if (job->notify) PostMessageW(job->notify, WM_APP_DONE, code, 0);
    return code;
}

// ---------------------------------------------------------------- window

enum : int {
    ID_DRIVE_RADIO = 101, ID_DRIVES, ID_IMAGE_RADIO, ID_IMAGE, ID_IMAGE_BROWSE, ID_TARGET, ID_TARGET_BROWSE, ID_DESKTOP,
    ID_INSTALL, ID_PLAY, ID_CLOSE
};

struct Ui {
    HWND wnd, driveRadio, drives, imageRadio, image, imageBrowse, check, target, targetBrowse, note, shortcuts, progress, status,
        install, play, close;
    HFONT font, bold, title;
    int dpi = 96;
    bool busy = false, done = false;
    Job job;
    CheckResult result;
    std::vector<std::wstring> driveRoots;
} g_ui;

int S(int v) { return MulDiv(v, g_ui.dpi, 96); }

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_ui.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return c;
}

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n) + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(size_t(n));
    return s;
}

std::wstring SelectedSource() {
    if (SendMessageW(g_ui.imageRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) return GetText(g_ui.image);
    LRESULT i = SendMessageW(g_ui.drives, CB_GETCURSEL, 0, 0);
    return (i >= 0 && size_t(i) < g_ui.driveRoots.size()) ? g_ui.driveRoots[size_t(i)] : L"";
}

void UpdateState() {
    std::wstring target = GetText(g_ui.target);
    bool update = !target.empty() && IsPreparedCopy(target);
    bool ready = update || g_ui.result.kind != Check::NotGame;
    EnableWindow(g_ui.install, ready && !g_ui.busy && !target.empty());
    SetWindowTextW(g_ui.install, update ? L"Update fix" : L"Install");
    if (update) SetWindowTextW(g_ui.note, L"This folder already has a prepared copy: Update replaces only the fix and keeps your saves and settings (no disc needed).");
    else SetWindowTextW(g_ui.note, L"About 600 MB. Nothing is written to the Windows registry; to remove the game later, delete this folder.");
}

void CheckSelection() {
    g_ui.result = CheckResult();
    std::wstring s = SelectedSource();
    if (s.empty()) {
        SetWindowTextW(g_ui.check, L"Insert the game CD (or mount your disc image) and choose its drive, or choose an image file.");
    } else {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::wstring err;
        std::unique_ptr<Source> src = OpenSource(s, &err);
        if (src) g_ui.result = CheckGame(*src);
        else g_ui.result.message = L"Cannot read it: " + err;
        SetCursor(old);
        std::wstring mark = g_ui.result.kind == Check::Supported ? L"✔ " : g_ui.result.kind == Check::OtherVersion ? L"⚠ " : L"✖ ";
        SetWindowTextW(g_ui.check, (mark + g_ui.result.message).c_str());
    }
    UpdateState();
}

void FillDrives() {
    SendMessageW(g_ui.drives, CB_RESETCONTENT, 0, 0);
    g_ui.driveRoots.clear();
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW(511, buf);
    int pick = -1;
    for (wchar_t* d = buf; d < buf + n && *d; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_CDROM) continue;
        wchar_t label[MAX_PATH + 1] = L"";
        bool disc = GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) != 0;
        std::wstring text = std::wstring(d, 2) + (disc ? (label[0] ? L"   " + std::wstring(label) : L"   (disc)") : L"   (no disc)");
        bool dir = false;
        if (disc && pick < 0 && DirSource(d).Exists(DISC_MARKER, &dir)) pick = int(g_ui.driveRoots.size());
        g_ui.driveRoots.push_back(d);
        SendMessageW(g_ui.drives, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (g_ui.driveRoots.empty()) SendMessageW(g_ui.drives, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(no CD/DVD drive found)"));
    SendMessageW(g_ui.drives, CB_SETCURSEL, pick >= 0 ? pick : 0, 0);
    if (pick >= 0) {
        SendMessageW(g_ui.driveRadio, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_ui.imageRadio, BM_SETCHECK, BST_UNCHECKED, 0);
    }
}

bool Browse(bool folder, std::wstring* out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC spec[] = {{L"Disc images (*.iso, *.bin)", L"*.iso;*.bin"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, spec);
        dlg->SetTitle(L"Choose the disc image you made from your game CD");
    } else {
        dlg->SetTitle(L"Choose where to put the game");
    }
    bool ok = false;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(g_ui.wnd)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            *out = p;
            CoTaskMemFree(p);
            ok = true;
        }
        item->Release();
    }
    dlg->Release();
    return ok;
}

void StartInstall() {
    std::wstring target = GetText(g_ui.target);
    if (!IsPreparedCopy(target) && g_ui.result.kind == Check::OtherVersion &&
        MessageBoxW(g_ui.wnd, L"Your copy is a different version than the one this fix was made for. It may work only in part. Install anyway?",
                    L"Different version", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    g_ui.job.source = SelectedSource();
    g_ui.job.target = target;
    g_ui.job.shortcuts = SendMessageW(g_ui.shortcuts, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_ui.job.notify = g_ui.wnd;
    g_ui.busy = true;
    for (HWND h : {g_ui.driveRadio, g_ui.drives, g_ui.imageRadio, g_ui.image, g_ui.imageBrowse, g_ui.target, g_ui.targetBrowse, g_ui.shortcuts, g_ui.install})
        EnableWindow(h, FALSE);
    SendMessageW(g_ui.progress, PBM_SETPOS, 0, 0);
    CloseHandle(CreateThread(nullptr, 0, JobThread, &g_ui.job, 0, nullptr));
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_DRIVES:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        SendMessageW(g_ui.driveRadio, BM_SETCHECK, BST_CHECKED, 0);
                        SendMessageW(g_ui.imageRadio, BM_SETCHECK, BST_UNCHECKED, 0);
                        CheckSelection();
                    } else if (HIWORD(wp) == CBN_DROPDOWN) {
                        FillDrives();
                    }
                    break;
                case ID_DRIVE_RADIO:
                case ID_IMAGE_RADIO:
                    if (HIWORD(wp) == BN_CLICKED) CheckSelection();
                    break;
                case ID_IMAGE_BROWSE: {
                    std::wstring p;
                    if (Browse(false, &p)) {
                        SetWindowTextW(g_ui.image, p.c_str());
                        SendMessageW(g_ui.imageRadio, BM_SETCHECK, BST_CHECKED, 0);
                        SendMessageW(g_ui.driveRadio, BM_SETCHECK, BST_UNCHECKED, 0);
                        CheckSelection();
                    }
                    break;
                }
                case ID_IMAGE:
                    if (HIWORD(wp) == EN_KILLFOCUS && SendMessageW(g_ui.imageRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) CheckSelection();
                    break;
                case ID_TARGET_BROWSE: {
                    std::wstring p;
                    if (Browse(true, &p)) {
                        if (!IsPreparedCopy(p)) p = JoinPath(p, DEFAULT_FOLDER);
                        SetWindowTextW(g_ui.target, p.c_str());
                        UpdateState();
                    }
                    break;
                }
                case ID_TARGET:
                    if (HIWORD(wp) == EN_CHANGE) UpdateState();
                    break;
                case ID_INSTALL:
                    StartInstall();
                    break;
                case ID_PLAY:
                    ShellExecuteW(w, L"open", JoinPath(g_ui.job.target, GAME_EXE).c_str(), nullptr, g_ui.job.target.c_str(), SW_SHOWNORMAL);
                    PostMessageW(w, WM_CLOSE, 0, 0);
                    break;
                case ID_CLOSE:
                    PostMessageW(w, WM_CLOSE, 0, 0);
                    break;
            }
            return 0;
        case WM_APP_PROGRESS:
            SendMessageW(g_ui.progress, PBM_SETPOS, wp, 0);
            return 0;
        case WM_APP_STATUS: {
            std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));
            SetWindowTextW(g_ui.status, s->c_str());
            return 0;
        }
        case WM_APP_DONE:
            g_ui.busy = false;
            if (wp == 0) {
                g_ui.done = true;
                ShowWindow(g_ui.install, SW_HIDE);  // Play takes its place
                ShowWindow(g_ui.play, SW_SHOW);
                SetWindowTextW(g_ui.close, L"Close");
                SetFocus(g_ui.play);
            } else {
                for (HWND h : {g_ui.driveRadio, g_ui.drives, g_ui.imageRadio, g_ui.image, g_ui.imageBrowse, g_ui.target, g_ui.targetBrowse, g_ui.shortcuts})
                    EnableWindow(h, TRUE);
                UpdateState();
                MessageBoxW(w, (GetText(g_ui.status) + L"\n\nDetails: fix-install.log in the target folder.").c_str(), L"Not finished", MB_OK | MB_ICONERROR);
            }
            return 0;
        case WM_CLOSE:
            if (g_ui.busy && MessageBoxW(w, L"The game is still being copied. Stop and close?", L"Close", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            DestroyWindow(w);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void BuildUi() {
    const int W = 600;
    int y = 14;
    Make(L"STATIC", GAME_TITLE, SS_LEFT, 20, y, W - 40, 26, 0, g_ui.title);
    y += 30;
    Make(L"STATIC", L"Unofficial fix: prepares a copy of the game from your own CD that plays well on Windows 10 and 11 (widescreen, smooth "
                    L"frame rate, sharper picture, autosave, settings with F1). You need your own original CD of " GAME_TITLE L" for Windows ("
                    GAME_EDITION L"), or a disc image you made from it.",
         SS_LEFT, 20, y, W - 40, 64, 0, g_ui.font);
    y += 74;
    Make(L"BUTTON", L"1. Your game disc", BS_GROUPBOX, 14, y, W - 28, 150, 0, g_ui.bold);
    g_ui.driveRadio = Make(L"BUTTON", L"Game CD in drive:", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 30, y + 26, 150, 24, ID_DRIVE_RADIO, g_ui.font);
    g_ui.drives = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 190, y + 25, 260, 200, ID_DRIVES, g_ui.font);
    g_ui.imageRadio = Make(L"BUTTON", L"Disc image file:", BS_AUTORADIOBUTTON | WS_TABSTOP, 30, y + 58, 150, 24, ID_IMAGE_RADIO, g_ui.font);
    g_ui.image = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 190, y + 58, 290, 24, ID_IMAGE, g_ui.font);
    g_ui.imageBrowse = Make(L"BUTTON", L"Browse...", WS_TABSTOP, 488, y + 57, 84, 26, ID_IMAGE_BROWSE, g_ui.font);
    g_ui.check = Make(L"STATIC", L"", SS_LEFT, 30, y + 92, W - 60, 50, 0, g_ui.font);
    y += 162;
    Make(L"BUTTON", L"2. Where to put the game", BS_GROUPBOX, 14, y, W - 28, 100, 0, g_ui.bold);
    g_ui.target = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30, y + 26, 450, 24, ID_TARGET, g_ui.font);
    g_ui.targetBrowse = Make(L"BUTTON", L"Browse...", WS_TABSTOP, 488, y + 25, 84, 26, ID_TARGET_BROWSE, g_ui.font);
    g_ui.note = Make(L"STATIC", L"", SS_LEFT, 30, y + 56, W - 60, 40, 0, g_ui.font);
    y += 110;
    g_ui.shortcuts = Make(L"BUTTON", L"Shortcuts on the desktop and in the Start menu", BS_AUTOCHECKBOX | WS_TABSTOP, 20, y, 400, 24, ID_DESKTOP, g_ui.font);
    SendMessageW(g_ui.shortcuts, BM_SETCHECK, BST_CHECKED, 0);
    y += 32;
    g_ui.progress = Make(PROGRESS_CLASSW, L"", 0, 20, y, W - 40, 18, 0, g_ui.font);
    SendMessageW(g_ui.progress, PBM_SETRANGE32, 0, 1000);
    y += 24;
    g_ui.status = Make(L"STATIC", L"", SS_LEFT, 20, y, W - 40, 40, 0, g_ui.font);
    y += 48;
    g_ui.install = Make(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 300, y, 130, 32, ID_INSTALL, g_ui.bold);
    g_ui.play = Make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 300, y, 130, 32, ID_PLAY, g_ui.bold);
    ShowWindow(g_ui.play, SW_HIDE);
    g_ui.close = Make(L"BUTTON", L"Cancel", WS_TABSTOP, W - 156, y, 130, 32, ID_CLOSE, g_ui.font);
    SendMessageW(g_ui.driveRadio, BM_SETCHECK, BST_CHECKED, 0);
    PWSTR profile = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
        SetWindowTextW(g_ui.target, JoinPath(JoinPath(profile, L"Games"), DEFAULT_FOLDER).c_str());
        CoTaskMemFree(profile);
    }
    FillDrives();
    if (!g_ui.job.target.empty()) SetWindowTextW(g_ui.target, g_ui.job.target.c_str());
    if (!g_ui.job.shortcuts) SendMessageW(g_ui.shortcuts, BM_SETCHECK, BST_UNCHECKED, 0);  // /noshortcuts
    if (!g_ui.job.source.empty()) {  // "Setup.exe /source <image or folder>" opens with that disc chosen
        SetWindowTextW(g_ui.image, g_ui.job.source.c_str());
        SendMessageW(g_ui.imageRadio, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_ui.driveRadio, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    CheckSelection();
}

int RunUi(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_ui.dpi = int(GetDpiForSystem());
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, UINT(g_ui.dpi));
    g_ui.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    g_ui.bold = CreateFontIndirectW(&lf);
    lf.lfHeight = MulDiv(lf.lfHeight, 3, 2);
    g_ui.title = CreateFontIndirectW(&lf);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"SpartaRecompFixSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    RECT r{0, 0, S(600), S(620)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, UINT(g_ui.dpi));
    g_ui.wnd = CreateWindowExW(0, wc.lpszClassName, GAME_TITLE L": fix setup", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                               r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    BuildUi();
    ShowWindow(g_ui.wnd, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_ui.wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Job job;
    bool cli = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = Lower(argv[i]);
        if ((a == L"/source" || a == L"-source") && i + 1 < argc) job.source = argv[++i], cli = true;
        else if ((a == L"/target" || a == L"-target") && i + 1 < argc) job.target = argv[++i], cli = true;
        else if (a == L"/quiet" || a == L"-quiet") job.quiet = true;
        else if (a == L"/noshortcuts" || a == L"-noshortcuts") job.shortcuts = false;
    }
    LocalFree(argv);
    if (cli && job.quiet) {  // unattended: exit code 0 = installed (or updated), details in <target>\fix-install.log
        if (job.target.empty()) return 1;
        return int(JobThread(&job));
    }
    g_ui.job.source = job.source;  // prefilled in the window
    g_ui.job.target = job.target;
    g_ui.job.shortcuts = job.shortcuts;
    return RunUi(inst);
}
