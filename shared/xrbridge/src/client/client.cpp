// XrBridge game-side client (see include/xrbridge/client.h).
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "xrbridge/client.h"
#include "xrbridge/names.h"
#include "../common/log.h"

namespace xrb {

static HANDLE g_map = nullptr, g_evtFrame = nullptr, g_evtHost = nullptr, g_hostProc = nullptr;
static IpcShared* g_shm = nullptr;
static uint32_t g_lastBeat = 0;
static DWORD g_lastBeatTick = 0;
static uint32_t g_sync = SYNC_SLOT_STATE;

bool ClientInit(const ClientSettings& s) {
    if (g_shm) return true;
    if (!ValidInstanceName(s.instanceName.c_str())) { LOGE("xrbridge: invalid instance name '%s'", s.instanceName.c_str()); return false; }
    const char* n = s.instanceName.c_str();
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(IpcShared), ShmName(n).c_str());
    if (!g_map) { LOGE("xrbridge: CreateFileMapping failed %lu", GetLastError()); return false; }
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_shm = (IpcShared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IpcShared));
    if (!g_shm) { LOGE("xrbridge: MapViewOfFile failed %lu", GetLastError()); CloseHandle(g_map); g_map = nullptr; return false; }
    if (existed && g_shm->magic == kIpcMagic && g_shm->hostPid) {
        // A host from a previous game run is still alive (quick restart / crash). Ask it to quit and wait for it,
        // so it can't overwrite the state of the session we are about to start.
        LOGW("xrbridge: previous host (pid %u) still running - stopping it", g_shm->hostPid);
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, g_shm->hostPid);
        g_shm->quitRequest = 1;
        if (old) { if (WaitForSingleObject(old, 5000) != WAIT_OBJECT_0) LOGW("xrbridge: previous host did not exit"); CloseHandle(old); }
    }
    g_evtFrame = CreateEventW(nullptr, FALSE, FALSE, FrameEventName(n).c_str());
    g_evtHost = CreateEventW(nullptr, FALSE, FALSE, HostEventName(n).c_str());
    if (!g_evtFrame || !g_evtHost) LOGW("xrbridge: CreateEvent failed %lu (pacing disabled)", GetLastError());

    ZeroMemory(g_shm, sizeof(IpcShared));
    g_shm->version = kIpcVersion;
    g_shm->gamePid = GetCurrentProcessId();
    strncpy_s(g_shm->appName, s.appName.c_str(), _TRUNCATE);
    g_shm->hudDistance = s.hudDistance;
    g_shm->hudWidth = s.hudWidth;
    g_shm->hudOffsetY = s.hudOffsetY;
    g_shm->hudFollowHead = s.hudFollowHead ? 1 : 0;
    g_shm->hudPremultiplied = s.hudPremultiplied ? 1 : 0;
    g_shm->menuDistance = s.menuDistance;
    g_shm->menuWidth = s.menuWidth;
    g_shm->hostLogLevel = (uint32_t)s.logLevel;
    g_shm->lastSlot = -1;
    MemoryBarrier();
    g_shm->magic = kIpcMagic;

    if (s.hostExePath.empty()) { LOGW("xrbridge: no host exe configured - running without VR output"); return true; }
    if (GetFileAttributesW(s.hostExePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOGW("xrbridge: %S not found - running without VR output", s.hostExePath.c_str());
        return true;
    }
    std::wstring cmd = L"\"" + s.hostExePath + L"\" --name " + std::wstring(s.instanceName.begin(), s.instanceName.end()) +
                       L" --pid " + std::to_wstring(GetCurrentProcessId());
    if (!s.hostLogPath.empty()) cmd += L" --log \"" + s.hostLogPath + L"\"";
    std::wstring dir = s.hostExePath.substr(0, s.hostExePath.find_last_of(L"\\/"));
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        g_hostProc = pi.hProcess;
        CloseHandle(pi.hThread);
        LOGI("xrbridge: launched host pid %lu", pi.dwProcessId);
    } else {
        LOGE("xrbridge: failed to launch host (%lu): %S", GetLastError(), cmd.c_str());
    }
    return true;
}

void ClientShutdown() {
    if (g_shm) g_shm->quitRequest = 1;
}

bool ClientHostConnected() {
    if (!g_shm || g_shm->hostState < HOST_READY || g_shm->hostState >= HOST_ERROR) return false;
    uint32_t b = g_shm->hostHeartbeat;
    DWORD now = GetTickCount();
    if (b != g_lastBeat) { g_lastBeat = b; g_lastBeatTick = now; }
    return now - g_lastBeatTick < 2000;
}

bool ClientHostVisible() {
    return ClientHostConnected() && g_shm->hostState >= HOST_RUNNING;
}

const char* ClientHostError() {
    return (g_shm && g_shm->hostState == HOST_ERROR) ? g_shm->hostError : "";
}

bool ClientWaitRecommendedSize(unsigned* w, unsigned* h, unsigned timeoutMs) {
    if (!g_shm || !g_hostProc) return false;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeoutMs) {
        if (g_shm->hostState == HOST_ERROR) { LOGE("xrbridge: host error: %s", g_shm->hostError); return false; }
        if (g_shm->recWidth && g_shm->recHeight) {
            *w = g_shm->recWidth; *h = g_shm->recHeight;
            LOGI("xrbridge: host recommends %ux%u per eye (runtime '%s', system '%s')", *w, *h, g_shm->runtimeName, g_shm->systemName);
            return true;
        }
        if (WaitForSingleObject(g_hostProc, 50) == WAIT_OBJECT_0) { LOGE("xrbridge: host exited early - see its log"); return false; }
    }
    LOGW("xrbridge: timed out waiting for the host");
    return false;
}

uint64_t ClientAdapterLuid() { return g_shm ? g_shm->adapterLuid : 0; }

bool ClientGetLatestPoses(IpcPose pose[2], IpcFov fov[2], int64_t* time) {
    if (!ClientHostConnected()) return false;
    for (int tries = 0; tries < 64; ++tries) {
        uint32_t s0 = g_shm->poseSeq;
        if (s0 & 1) { if (tries > 8) SwitchToThread(); else YieldProcessor(); continue; }   // host mid-write (maybe preempted)
        MemoryBarrier();
        memcpy(pose, g_shm->eyePose, sizeof(IpcPose) * 2);
        memcpy(fov, g_shm->eyeFov, sizeof(IpcFov) * 2);
        *time = g_shm->predictedTime;
        MemoryBarrier();
        if (g_shm->poseSeq == s0) return fov[0].right != 0;
    }
    return false;
}

void ClientPublishTextures(const TextureSet& t) {
    if (!g_shm) return;
    g_sync = t.sync;
    g_shm->atlasWidth = t.atlasWidth;
    g_shm->atlasHeight = t.atlasHeight;
    g_shm->texFormat = t.dxgiFormat;
    g_shm->syncMode = t.sync;
    g_shm->eyeRect[0] = t.eyeRect[0];
    g_shm->eyeRect[1] = t.eyeRect[1];
    g_shm->hudRect = t.hudRect;
    // Withdraw the old frames: with lastSlot = -1 the host can't claim anything new; a slot it is copying right
    // now (READING) is waited for, so the textures are never reused under an in-flight host copy.
    g_shm->lastSlot = -1;
    MemoryBarrier();
    for (int i = 0; i < kSlots; ++i) {
        DWORD t0 = GetTickCount();
        for (;;) {
            LONG st = g_shm->slotState[i];
            if (st == SLOT_READING && GetTickCount() - t0 < 200 && ClientHostConnected()) { Sleep(0); continue; }
            if (InterlockedCompareExchange((volatile LONG*)&g_shm->slotState[i], SLOT_FREE, st) == st) break;
        }
        g_shm->sharedHandle[i] = t.handles[i];
    }
    MemoryBarrier();
    g_shm->gameState = g_shm->gameState + 1;   // generation: host reopens everything
    LOGI("xrbridge: published %d shared atlases %ux%u fmt %u (%s)", kSlots, t.atlasWidth, t.atlasHeight, t.dxgiFormat,
         t.sync == SYNC_KEYED_MUTEX ? "keyed mutex" : "slot state");
}

int ClientAcquireSlot() {
    if (!g_shm || g_sync != SYNC_SLOT_STATE) return -1;
    int last = g_shm->lastSlot;
    int start = (last + 1 + kSlots) % kSlots;
    // 1st choice: a free slot. 2nd: a submitted slot the host skipped (READY but no longer the newest).
    for (int pass = 0; pass < 2; ++pass) {
        LONG from = pass == 0 ? SLOT_FREE : SLOT_READY;
        for (int k = 0; k < kSlots; ++k) {
            int s = (start + k) % kSlots;
            if (pass == 1 && s == last) continue;
            if (InterlockedCompareExchange((volatile LONG*)&g_shm->slotState[s], SLOT_RENDERING, from) == from) return s;
        }
    }
    return -1;
}

void ClientCancelSlot(int slot) {
    if (!g_shm || slot < 0 || slot >= kSlots) return;
    InterlockedCompareExchange((volatile LONG*)&g_shm->slotState[slot], SLOT_FREE, SLOT_RENDERING);
}

void ClientSubmitSlot(int slot, const IpcPose pose[2], const IpcFov fov[2], int64_t time, uint32_t flags, const IpcMotion* motion) {
    if (!g_shm || slot < 0 || slot >= kSlots) return;
    memcpy(g_shm->slotPose[slot], pose, sizeof(IpcPose) * 2);
    memcpy(g_shm->slotFov[slot], fov, sizeof(IpcFov) * 2);
    g_shm->slotTime[slot] = time;
    g_shm->slotFlags[slot] = flags;
    if (motion) g_shm->slotMotion[slot] = *motion;
    else { g_shm->slotMotion[slot] = IpcMotion{}; g_shm->slotFlags[slot] = flags & ~SLOT_REPROJECT; }
    MemoryBarrier();
    if (g_sync == SYNC_SLOT_STATE &&
        InterlockedCompareExchange((volatile LONG*)&g_shm->slotState[slot], SLOT_READY, SLOT_RENDERING) != SLOT_RENDERING) {
        LOGW("xrbridge: submit of slot %d that the game does not own - dropped", slot);
        return;
    }
    MemoryBarrier();
    g_shm->lastSlot = slot;
    g_shm->submitSeq = g_shm->submitSeq + 1;
    if (g_evtFrame) SetEvent(g_evtFrame);
}

int ClientLastSubmittedSlot() { return g_shm ? g_shm->lastSlot : -1; }

void ClientRequestDump(uint32_t id) {
    if (g_shm) { g_shm->dumpRequest = id; LOGI("xrbridge: dump %u requested", id); }
}

void ClientRequestRecenter() {
    if (g_shm) { g_shm->recenterRequest = g_shm->recenterRequest + 1; LOGI("xrbridge: recenter requested"); }
}

bool ClientWaitHostFrame(unsigned ms) {
    if (!g_evtHost || !ClientHostVisible()) return false;
    return WaitForSingleObject(g_evtHost, ms) == WAIT_OBJECT_0;
}

uint32_t ClientHostFrameCount() { return g_shm ? g_shm->hostHeartbeat : 0; }
int64_t ClientDisplayPeriodNs() { return g_shm ? g_shm->displayPeriodNs : 0; }

void ClientSetHud(float distance, float width, float offsetY, bool followHead) {
    if (!g_shm) return;
    g_shm->hudDistance = distance; g_shm->hudWidth = width; g_shm->hudOffsetY = offsetY; g_shm->hudFollowHead = followHead;
}

void ClientSetMenu(float distance, float width) {
    if (!g_shm) return;
    g_shm->menuDistance = distance; g_shm->menuWidth = width;
}

// ------------------------------------------------------------------ math helpers
void PoseToViewRH(const IpcPose& p, float o[16]) {
    // rotation matrix of the eye (columns = eye axes in world), then view = inverse(rotation) * translate(-pos)
    float x = p.qx, y = p.qy, z = p.qz, w = p.qw;
    float r[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    // row-vector convention: v' = v * M. Upper 3x3 of M = R (since R^T transposed back), translation row = -pos * R
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i * 4 + j] = r[i][j];
    o[3] = o[7] = o[11] = 0;
    for (int j = 0; j < 3; ++j) o[12 + j] = -(p.px * r[0][j] + p.py * r[1][j] + p.pz * r[2][j]);
    o[15] = 1;
}

void FovToProjRH(const IpcFov& f, float n, float fa, float o[16]) {
    float l = tanf(f.left), r = tanf(f.right), u = tanf(f.up), d = tanf(f.down);
    float w = r - l, h = u - d;
    memset(o, 0, sizeof(float) * 16);
    // row-vector, right-handed, depth 0..1 (D3D)
    o[0] = 2 / w;
    o[5] = 2 / h;
    o[8] = (r + l) / w;
    o[9] = (u + d) / h;
    o[10] = fa / (n - fa);
    o[11] = -1;
    o[14] = n * fa / (n - fa);
}

} // namespace xrb
