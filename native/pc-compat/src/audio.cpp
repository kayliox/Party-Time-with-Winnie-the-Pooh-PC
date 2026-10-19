// The game's audio session: unattended test runs mute it (SPARTA_MUTE=1) without touching the system volume. Windows
// keeps an app's mute in its per-app volume settings (by executable path), so a test mute must never outlive the test:
// it is undone at the timed exit, and a marker file next to the game lets the next normal start undo it after a crash
// or a kill (O-122). A normal start also reports a mute set in the Windows volume mixer.
#include "compat.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>

namespace compat {

static const GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIidIAudioSessionManager = {0xBFA971F1, 0x4D5E, 0x40BB, {0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4}};

enum class SessionOp { Mute, Unmute, Query };
struct SessionJob {
    SessionOp op;
    HRESULT hr;
    BOOL muted;
};

static bool g_testMuted;
static bool g_windowsMuted;  // muted in the Windows volume mixer at start (not by us)

static std::string MarkerPath() { return g_cfg.exeDir + "spartacompat.testmute"; }

static DWORD WINAPI SessionThread(LPVOID arg) {
    // COM may not be initialised inside DllMain or on the game's thread: use a short-lived thread of our own
    auto* job = static_cast<SessionJob*>(arg);
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager* mgr = nullptr;
    ISimpleAudioVolume* vol = nullptr;
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator, reinterpret_cast<void**>(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(kIidIAudioSessionManager, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr));
    if (SUCCEEDED(hr)) hr = mgr->GetSimpleAudioVolume(nullptr, FALSE, &vol);  // this process's default session
    if (SUCCEEDED(hr)) hr = vol->GetMute(&job->muted);
    if (SUCCEEDED(hr) && job->op != SessionOp::Query) {
        BOOL want = job->op == SessionOp::Mute;
        if (job->muted != want) hr = vol->SetMute(want, nullptr);
    }
    job->hr = hr;
    if (vol) vol->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return 0;
}

static SessionJob RunSessionJob(SessionOp op) {
    SessionJob job{op, E_FAIL, FALSE};
    HANDLE t = CreateThread(nullptr, 0, SessionThread, &job, 0, nullptr);
    if (t) {
        if (WaitForSingleObject(t, 5000) != WAIT_OBJECT_0) job.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        CloseHandle(t);
    }
    return job;
}

void MuteProcessAudio() {
    // the marker goes first: if the run dies after the mute, the next normal start of this install undoes it
    HANDLE f = CreateFileA(MarkerPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        static const char text[] = "A SpartaRecomp test run muted this game's audio session; the next normal start unmutes it.\r\n";
        DWORD n;
        WriteFile(f, text, sizeof text - 1, &n, nullptr);
        CloseHandle(f);
    }
    SessionJob job = RunSessionJob(SessionOp::Mute);
    g_testMuted = SUCCEEDED(job.hr);
    Log("audio: process audio session %s (0x%08lx)", g_testMuted ? "muted for this test run" : "NOT muted", job.hr);
}

void UnmuteTestAudio() {
    if (!g_testMuted) return;
    SessionJob job = RunSessionJob(SessionOp::Unmute);
    if (SUCCEEDED(job.hr)) {
        DeleteFileA(MarkerPath().c_str());
        g_testMuted = false;
    }
    Log("audio: test mute %s (0x%08lx)", SUCCEEDED(job.hr) ? "undone" : "NOT undone", job.hr);
}

void CheckProcessAudio() {
    bool marker = GetFileAttributesA(MarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    SessionJob job = RunSessionJob(marker ? SessionOp::Unmute : SessionOp::Query);
    if (FAILED(job.hr)) {
        Log("audio: could not read the game's audio session (0x%08lx)", job.hr);
        return;
    }
    if (marker) {
        DeleteFileA(MarkerPath().c_str());
        Log("audio: %s", job.muted ? "undid the mute a test run had left in Windows' per-app volume settings"
                                   : "test-run marker removed (the session was not muted)");
    } else if (job.muted) {
        g_windowsMuted = true;
        Log("audio: WARNING: Windows has this game muted (Volume mixer, per-app setting); the game is silent until it is unmuted there");
    } else {
        Log("audio: game audio session not muted");
    }
}

bool AudioMutedInWindows() { return g_windowsMuted; }

bool UnmuteProcessAudio() {  // the overlay's button: the same as unmuting the game in the Windows volume mixer
    SessionJob job = RunSessionJob(SessionOp::Unmute);
    if (SUCCEEDED(job.hr)) g_windowsMuted = false;
    Log("audio: unmute requested in the overlay: %s (0x%08lx)", SUCCEEDED(job.hr) ? "done" : "failed", job.hr);
    return SUCCEEDED(job.hr);
}

}  // namespace compat
